#include "BitstreamEngine.h"

#include <algorithm>
#include <cmath>

namespace bs {

namespace {
constexpr double kMaxAdjust = 0.005;     // +-0.5 % resampling range
constexpr double kMaxIntegral = 0.001;   // +-1000 ppm sustained drift
constexpr double kCorrectionSeconds = 3; // proportional time constant
}  // namespace

BitstreamEngine::BitstreamEngine() = default;
BitstreamEngine::~BitstreamEngine() = default;

bool BitstreamEngine::initialize(const EngineConfig& config, std::string& error) {
    cfg_ = config;
    encoder_ = createEncoder(cfg_.codec);
    packetizer_ = createPacketizer(cfg_.codec);
    if (!encoder_ || !packetizer_) { error = "codec not available"; return false; }

    AudioFormat in;
    in.sampleRate = cfg_.sampleRate;
    in.channels = 6;
    in.channelMask = spk::Surround51Side;
    in.sampleFormat = SampleFormat::Float32;
    in.validBits = 32;
    EncoderSettings es;
    es.bitrate = cfg_.bitrate;
    if (!encoder_->initialize(in, es, error)) return false;
    caps_ = encoder_->capabilities();
    if (!packetizer_->initialize(caps_, error)) return false;
    transport_ = packetizer_->descriptor();

    burstFrames_ = transport_.burstFrames;
    bytesPerFrame_ = static_cast<uint32_t>(transport_.transportChannels) * 2;
    dtNs_ = 1e9 / static_cast<double>(caps_.sampleRate);
    for (auto& s : slots_) s.bytes.assign(transport_.burstBytes(), 0);
    if (!buildSilenceBurst(error)) return false;
    resampler_.configure(caps_.channels);
    resetStreams();
    return true;
}

void BitstreamEngine::resetStreams() {
    head_.store(0); tail_.store(0);
    headLocal_ = tailLocal_ = 0;
    curRemainPub_.store(0);
    minHeadroom_.store(INT32_MAX);
    starveSeq_.store(1);
    primedSeq_ = 0;
    cur_ = nullptr; burstOff_ = 0; curRemain_ = 0; curIsSilence_ = false;
    resumeSlackFrames_.store(cfg_.marginFrames + cfg_.capturePeriodFrames + cfg_.outputPeriodFrames);
    starving_ = true; streamStarted_ = false;
    accumFill_ = 0;
    margin_ = cfg_.marginFrames;
    seenUnderruns_ = underrunEvents_.load();
    windowFrames_ = 0; skipWindows_ = 1;
    integ_ = adjust_ = 0;
    lastMinHeadroomValid_ = false;
    resampler_.reset();
    resampler_.setStep(1.0);
    if (encoder_) { encoder_->reset(); planes_ = encoder_->inputPlanes(); }
    requestStatsReset();
}

// ------------------------------------------------------------- producer

void BitstreamEngine::pushCapture(const float* in, uint32_t frames, int64_t tFirstNs, int64_t tNowNs,
                                  bool discontinuity) {
    const uint32_t epoch = statsEpoch_.load(std::memory_order_relaxed);
    if (epoch != producerEpoch_) {
        producerEpoch_ = epoch;
        hEncode_.reset();
        framesEncoded_ = overrunDrops_ = discontinuities_ = encodeErrors_ = primes_ = 0;
    }
    const uint16_t ch = caps_.channels;
    const uint32_t frameSamples = caps_.frameSamples;

    if (discontinuity) {
        ++discontinuities_;
        accumFill_ = 0;
        resampler_.reset();
    }
    if (frames == 0) return;

    // Peak meters with exponential decay (~300 ms).
    const float decay = std::exp(-static_cast<float>(frames) / (0.3f * static_cast<float>(caps_.sampleRate)));
    for (uint16_t c = 0; c < ch; ++c) {
        float pk = 0.0f;
        const float* p = in + c;
        for (uint32_t i = 0; i < frames; ++i) {
            const float a = std::fabs(p[static_cast<size_t>(i) * ch]);
            if (a > pk) pk = a;
        }
        peaks_[c] = std::max(pk, peaks_[c] * decay);
    }

    uint32_t off = 0;
    while (off < frames) {
        const uint32_t n = std::min<uint32_t>(frames - off, static_cast<uint32_t>(DriftResampler::kMaxPush));
        resampler_.push(in + static_cast<size_t>(off) * ch, n);
        off += n;
        const int64_t tPushedEndNs = tFirstNs + static_cast<int64_t>(off * dtNs_);
        for (;;) {
            const double pending = resampler_.pendingInputFrames();
            const size_t got = resampler_.pull(planes_, accumFill_, frameSamples - accumFill_);
            if (got == 0) break;
            if (accumFill_ == 0) accumT0_ = tPushedEndNs - static_cast<int64_t>((pending + 1.0) * dtNs_);
            accumFill_ += static_cast<uint32_t>(got);
            if (accumFill_ == frameSamples) {
                encodeFrame(tNowNs);
                accumFill_ = 0;
            }
        }
    }

    if (publishCountdown_ == 0) {
        publishProducer();
        publishCountdown_ = 1;
    } else {
        --publishCountdown_;
    }
}

void BitstreamEngine::encodeFrame(int64_t tNowNs) {
    const int64_t e0 = nowNs();
    AudioBufferView view{planes_, caps_.channels, caps_.frameSamples};
    const EncodeResult res = encoder_->encode(view);
    planes_ = encoder_->inputPlanes();
    if (res.status != EncodeResult::Status::Ok) {
        if (res.status == EncodeResult::Status::Error) ++encodeErrors_;
        return;
    }

    // Admission control: never queue stale audio. If more than one full burst
    // (plus the margin) is still waiting, this frame would only add delay.
    const uint32_t head = head_.load(std::memory_order_acquire);
    const uint32_t queued = tailLocal_ - head;
    const int64_t level = curRemainPub_.load(std::memory_order_relaxed) + static_cast<int64_t>(queued) * burstFrames_;
    // Frames complete on the capture-packet grid, so up to two capture periods
    // of encoded audio can legitimately be waiting on top of that (with DTS a
    // 10 ms packet regularly completes a frame slightly early).
    // One further burst is allowed because output resumes on a burst boundary
    // after running dry, which can leave up to one burst of excess for the
    // controller to trim.
    const int64_t maxLevel = 2 * static_cast<int64_t>(burstFrames_) + margin_ + cfg_.outputPeriodFrames + 2 * static_cast<int64_t>(cfg_.capturePeriodFrames);
    if (queued >= kSlots - 2 || level > maxLevel) {
        ++overrunDrops_;
        controlTick();
        return;
    }

    BurstSlot& slot = slots_[tailLocal_ % kSlots];
    if (!packetizer_->packetize(res.data, slot.bytes)) {
        ++encodeErrors_;
        return;
    }
    const int64_t e1 = nowNs();
    slot.t0 = accumT0_;
    slot.t1 = tNowNs;
    slot.t2 = tNowNs + (e1 - e0);

    // After the output side ran dry it resumes at a burst boundary (see
    // render()); the controller restarts its observation window.
    const uint32_t seq = starveSeq_.load(std::memory_order_acquire);
    if (seq != primedSeq_) {
        primedSeq_ = seq;
        ++primes_;
        windowFrames_ = 0;
        skipWindows_ = 1;
        minHeadroom_.store(INT32_MAX, std::memory_order_relaxed);
    }
    resumeSlackFrames_.store(margin_ + cfg_.capturePeriodFrames + cfg_.outputPeriodFrames, std::memory_order_relaxed);
    ++tailLocal_;
    tail_.store(tailLocal_, std::memory_order_release);

    hEncode_.add(static_cast<double>(e1 - e0) / 1000.0);
    ++framesEncoded_;
    controlTick();
}

void BitstreamEngine::controlTick() {
    const uint64_t u = underrunEvents_.load(std::memory_order_relaxed);
    if (u != seenUnderruns_) {
        seenUnderruns_ = u;
        if (cfg_.adaptiveMargin) margin_ = std::min(cfg_.maxMarginFrames, margin_ + caps_.sampleRate / 1000);
    }

    ++windowFrames_;
    if (static_cast<uint64_t>(windowFrames_) * caps_.frameSamples < caps_.sampleRate / 2) return;
    const uint32_t windowFramesUsed = windowFrames_;
    windowFrames_ = 0;
    const int32_t h = minHeadroom_.exchange(INT32_MAX, std::memory_order_relaxed);
    if (h == INT32_MAX) return;
    if (skipWindows_ > 0) { --skipWindows_; return; }
    lastMinHeadroom_ = h;
    lastMinHeadroomValid_ = true;

    // PI controller. Positive error = more audio queued than needed, so the
    // resampler must consume input faster than it produces output.
    // Gain scheduling: large errors (after a realignment) are removed quickly,
    // small ones slowly so measurement noise does not modulate the pitch.
    const double err = static_cast<double>(h) - static_cast<double>(margin_);
    const double fastKp = 1.0 / (static_cast<double>(caps_.sampleRate) * kCorrectionSeconds);
    const bool large = std::fabs(err) > static_cast<double>(caps_.sampleRate) * 0.002;
    // Too little headroom risks an audible underrun, so that side always
    // reacts fast; excess queueing only costs a little latency for a while.
    const bool urgent = err < 0;
    const double kp = (large || urgent) ? fastKp : fastKp * 0.25;
    // The queue is an integrator with gain g (frames per unit ratio per
    // control window); ki = kp^2 * g / 4 keeps the loop critically damped.
    // The integral term (sustained clock drift) is charged slowly while
    // excess is being trimmed so that the start-up transient cannot wind it up.
    const double g = static_cast<double>(windowFramesUsed) * caps_.frameSamples;
    const double kiFast = fastKp * fastKp * g / 4.0;
    const double ki = urgent ? kiFast : kiFast / 16.0;
    const double unclamped = kp * err + integ_;
    if (std::fabs(unclamped) < kMaxAdjust) integ_ = std::clamp(integ_ + ki * err, -kMaxIntegral, kMaxIntegral);
    adjust_ = std::clamp(kp * err + integ_, -kMaxAdjust, kMaxAdjust);    resampler_.setStep(1.0 + adjust_);
}

void BitstreamEngine::publishProducer() {
    ProducerSnap s{};
    s.encode = hEncode_.summary();
    s.framesEncoded = framesEncoded_;
    s.overrunDrops = overrunDrops_;
    s.discontinuities = discontinuities_;
    s.encodeErrors = encodeErrors_;
    s.primes = primes_;
    s.adjustPpm = adjust_ * 1e6;
    s.integPpm = integ_ * 1e6;
    s.minHeadroom = lastMinHeadroom_;
    s.minHeadroomValid = lastMinHeadroomValid_;
    s.marginFrames = margin_;
    std::memcpy(s.peaks, peaks_, sizeof s.peaks);
    producerSnap_.write(s);
}

// ------------------------------------------------------------- consumer

void BitstreamEngine::render(uint8_t* dst, uint32_t frames, int64_t tNowNs, int64_t deviceQueued) {
    const uint32_t epoch = statsEpoch_.load(std::memory_order_relaxed);
    if (epoch != consumerEpoch_) {
        consumerEpoch_ = epoch;
        hAccum_.reset(); hQueue_.reset(); hOutDelay_.reset(); hSubmit_.reset(); hTotal_.reset(); hLevel_.reset();
        burstsPlayed_ = underruns_ = padFrames_ = 0;
    }

    uint32_t tail = tail_.load(std::memory_order_acquire);
    if (streamStarted_) {
        // Occupancy: complete bursts waiting behind the one being transmitted.
        hLevel_.add(static_cast<double>(tail - headLocal_) * burstFrames_ * dtNs_ / 1000.0);
    }

    uint32_t w = 0;
    while (w < frames) {
        if (curRemain_ == 0) {
            // A burst boundary: decide what the next burst period carries.
            tail = tail_.load(std::memory_order_acquire);
            bool takeReal = headLocal_ != tail;
            if (takeReal && starving_) {
                // (Re)alignment after running dry: only resume when the waiting
                // burst has enough slack to cover the arrival jitter of the
                // ones that follow; otherwise play one more silent burst.
                const BurstSlot& next = slots_[headLocal_ % kSlots];
                const double slack = static_cast<double>(tNowNs - next.t2) / dtNs_ + w;
                if (slack < static_cast<double>(resumeSlackFrames_.load(std::memory_order_relaxed))) takeReal = false;
            }
            if (!takeReal) {
                // Nothing ready (or not ready early enough). The stream keeps
                // its burst cadence by sending an encoded frame of silence, so
                // the receiver's decoder stays locked and a late frame costs
                // one frame of silence instead of a re-synchronisation.
                cur_ = &silence_;
                curIsSilence_ = true;
                if (streamStarted_) padFrames_ += burstFrames_;
                if (!starving_) {
                    starving_ = true;
                    if (streamStarted_) {
                        ++underruns_;
                        underrunEvents_.fetch_add(1, std::memory_order_relaxed);
                    }
                    starveSeq_.fetch_add(1, std::memory_order_release);
                }
            } else {
                cur_ = &slots_[headLocal_ % kSlots];
                curIsSilence_ = false;
                ++headLocal_;
                head_.store(headLocal_, std::memory_order_release);
                // Timing slack of this burst: how long it was ready before the
                // device needed its first byte. This is the quantity the latency
                // controller minimizes; it would be negative in an underrun.
                if (!starving_) {
                    const int32_t slack = static_cast<int32_t>(static_cast<double>(tNowNs - cur_->t2) / dtNs_);
                    int32_t prev = minHeadroom_.load(std::memory_order_relaxed);
                    while (slack < prev && !minHeadroom_.compare_exchange_weak(prev, slack, std::memory_order_relaxed)) {}
                }
                starving_ = false;
                streamStarted_ = true;
            }
            burstOff_ = 0;
            curRemain_ = static_cast<int32_t>(burstFrames_);
        }
        const uint32_t n = std::min(frames - w, burstFrames_ - burstOff_);
        if (burstOff_ == 0 && !curIsSilence_) recordBurstStart(*cur_, tNowNs, w, deviceQueued);
        std::memcpy(dst + static_cast<size_t>(w) * bytesPerFrame_,
                    cur_->bytes.data() + static_cast<size_t>(burstOff_) * bytesPerFrame_,
                    static_cast<size_t>(n) * bytesPerFrame_);
        burstOff_ += n;
        curRemain_ -= static_cast<int32_t>(n);
        w += n;
    }
    curRemainPub_.store(curRemain_, std::memory_order_relaxed);

    if (tNowNs - lastConsumerPublishNs_ > 200'000'000 || tNowNs < lastConsumerPublishNs_) {
        lastConsumerPublishNs_ = tNowNs;
        publishConsumer();
    }
}

void BitstreamEngine::recordBurstStart(const BurstSlot& slot, int64_t tNowNs, uint32_t offsetFrames,
                                       int64_t deviceQueued) {
    ++burstsPlayed_;
    const double submitUs = static_cast<double>(tNowNs - slot.t0) / 1000.0;
    hAccum_.add(static_cast<double>(slot.t1 - slot.t0) / 1000.0);
    hQueue_.add(static_cast<double>(tNowNs - slot.t2) / 1000.0);
    hSubmit_.add(submitUs);
    if (deviceQueued >= 0) {
        const double outUs = static_cast<double>(deviceQueued + offsetFrames) * dtNs_ / 1000.0;
        hOutDelay_.add(outUs);
        hTotal_.add(submitUs + outUs);
    }
}

void BitstreamEngine::publishConsumer() {
    ConsumerSnap s{};
    s.accumulation = hAccum_.summary();
    s.queue = hQueue_.summary();
    s.outputDelay = hOutDelay_.summary();
    s.submit = hSubmit_.summary();
    s.total = hTotal_.summary();
    s.level = hLevel_.summary();
    s.burstsPlayed = burstsPlayed_;
    s.underruns = underruns_;
    s.padFrames = padFrames_;
    consumerSnap_.write(s);
}

EngineStats BitstreamEngine::stats() const {
    const ProducerSnap p = producerSnap_.read();
    const ConsumerSnap c = consumerSnap_.read();
    EngineStats s;
    s.encodeUs = p.encode;
    s.accumulationUs = c.accumulation;
    s.queueUs = c.queue;
    s.outputDelayUs = c.outputDelay;
    s.submitLatencyUs = c.submit;
    s.totalUs = c.total;
    s.queueLevelUs = c.level;
    s.outputDelayKnown = c.outputDelay.count > 0;
    s.framesEncoded = p.framesEncoded;
    s.overrunDrops = p.overrunDrops;
    s.discontinuities = p.discontinuities;
    s.encodeErrors = p.encodeErrors;
    s.primes = p.primes;
    s.burstsPlayed = c.burstsPlayed;
    s.underruns = c.underruns;
    s.padFrames = c.padFrames;
    s.resampleAdjustPpm = p.adjustPpm;
    s.driftEstimatePpm = p.integPpm;
    s.minHeadroomFrames = p.minHeadroom;
    s.minHeadroomValid = p.minHeadroomValid;
    s.marginFrames = p.marginFrames;
    for (size_t i = 0; i < 8; ++i) s.peaks[i] = p.peaks[i];
    return s;
}

// Encodes one frame of digital silence with a separate encoder instance (so
// the live encoder's state is untouched) and packetizes it. It is sent while
// no audio is ready, which keeps the burst cadence the receiver locked on to.
bool BitstreamEngine::buildSilenceBurst(std::string& error) {
    auto enc = createEncoder(cfg_.codec);
    AudioFormat in{cfg_.sampleRate, 6, spk::Surround51Side, SampleFormat::Float32, 32};
    EncoderSettings es;
    es.bitrate = cfg_.bitrate;
    if (!enc || !enc->initialize(in, es, error)) return false;
    silence_.bytes.assign(transport_.burstBytes(), 0);
    bool ok = false;
    // The first frames carry the encoder's start-up transient; use a later one.
    for (int i = 0; i < 4; ++i) {
        float* const* planes = enc->inputPlanes();
        for (uint16_t c = 0; c < caps_.channels; ++c) std::fill_n(planes[c], caps_.frameSamples, 0.0f);
        const EncodeResult r = enc->encode({planes, caps_.channels, caps_.frameSamples});
        if (r.status == EncodeResult::Status::Ok) ok = packetizer_->packetize(r.data, silence_.bytes);
    }
    if (!ok) error = "could not prepare the silence frame";
    return ok;
}

}  // namespace bs
