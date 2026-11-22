// The device-independent heart of the pipeline.
//
//   pushCapture()  (capture thread)  PCM -> drift resampler -> encoder input
//                                    frame -> encode -> IEC 61937 burst -> queue
//   render()       (output thread)   queue -> device buffer
//
// The two entry points may run on the same thread or on two different
// threads; the only shared state is a bounded lock-free single-producer /
// single-consumer burst queue plus a few atomics. No allocation, locking,
// logging or system call happens in either path (the encoder library's own
// small allocations excepted).
//
// Latency policy: the queue is kept as close to empty as scheduling jitter
// allows. For every burst the output side measures its timing slack (how long
// it was ready before the device needed it). A controller trims the PCM
// resampling ratio so that the minimum slack converges to a small safety
// margin. This simultaneously corrects clock drift between the
// capture and the output device and removes any excess queueing.
#pragma once

#include <array>
#include <atomic>
#include <memory>

#include "../codec/Codec.h"
#include "../dsp/DriftResampler.h"

namespace bs {


struct EngineConfig {
    CodecId codec = CodecId::Ac3;
    uint32_t bitrate = 0;               // 0 = codec default
    uint32_t sampleRate = 48000;
    uint32_t marginFrames = 96;         // target minimum output headroom (2 ms)
    uint32_t maxMarginFrames = 1440;    // adaptive margin ceiling (30 ms)
    bool adaptiveMargin = true;         // widen the margin after an underrun
    uint32_t capturePeriodFrames = 480; // capture delivery granularity (hint)
    uint32_t outputPeriodFrames = 480;  // output pull granularity (hint)
};

struct EngineStats {
    // --- measured per encoded frame, microseconds ---
    StatSummary accumulationUs;   // first sample captured -> frame complete (includes frame duration)
    StatSummary encodeUs;         // encoder + packetizer execution time
    StatSummary queueUs;          // burst ready -> first byte handed to the device
    StatSummary outputDelayUs;    // device-side buffering at hand-off (empty if unknown)
    StatSummary submitLatencyUs;  // first sample captured -> first byte handed to the device
    StatSummary totalUs;          // submitLatency + outputDelay
    StatSummary queueLevelUs;     // complete bursts waiting behind the one in transmission, sampled per device period
    bool outputDelayKnown = false;

    // --- counters since the last statistics reset ---
    uint64_t framesEncoded = 0;
    uint64_t burstsPlayed = 0;
    uint64_t underruns = 0;       // output needed data but no burst was ready
    uint64_t padFrames = 0;       // zero frames inserted because of underruns
    uint64_t outputStalls = 0;    // the output thread was called so late that the device ran dry
    uint64_t overrunDrops = 0;    // bursts discarded to avoid stale audio
    uint64_t discontinuities = 0; // capture glitches reported by the device
    uint64_t encodeErrors = 0;
    uint64_t primes = 0;          // queue (re)alignments

    // --- controller / clock state ---
    double resampleAdjustPpm = 0;     // current resampling correction
    double driftEstimatePpm = 0;      // integral term: sustained clock offset
    int32_t minHeadroomFrames = 0;    // minimum burst timing slack in the last control window
    bool minHeadroomValid = false;
    uint32_t marginFrames = 0;
    std::array<float, 8> peaks{};     // per-channel peak level (linear), canonical order
};

class BitstreamEngine {
public:
    BitstreamEngine();
    ~BitstreamEngine();

    bool initialize(const EngineConfig& config, std::string& error);

    // Device delivery granularities, used to size the (re)alignment padding.
    // Call before streaming starts.
    void setPeriodHints(uint32_t captureFrames, uint32_t outputFrames) {
        cfg_.capturePeriodFrames = captureFrames;
        cfg_.outputPeriodFrames = outputFrames;
    }

    const EncoderCapabilities& encoderCaps() const { return caps_; }
    const TransportDescriptor& transport() const { return transport_; }
    const EngineConfig& config() const { return cfg_; }
    // Constant latency contributed by the drift resampler, in frames.
    double resamplerDelayFrames() const {
        return DriftResampler::latencyFrames();
    }

    // Producer side. `interleaved` is canonical float PCM (caps.channels).
    // tFirstNs: capture-clock time of the first frame; tNowNs: time of the read.
    void pushCapture(const float* interleaved, uint32_t frames, int64_t tFirstNs, int64_t tNowNs,
                     bool discontinuity);

    // Consumer side. Fills `frames` transport frames (burst data or zeros).
    // deviceQueuedFrames: frames already queued in the device ahead of this
    // buffer, or -1 if unknown.
    void render(uint8_t* dst, uint32_t frames, int64_t tNowNs, int64_t deviceQueuedFrames);

    EngineStats stats() const;
    void requestStatsReset() { statsEpoch_.fetch_add(1, std::memory_order_relaxed); }

    // Drops all queued audio and resynchronizes. Only call while neither
    // pushCapture() nor render() is executing.
    void resetStreams();

private:
    static constexpr uint32_t kSlots = 8;

    struct BurstSlot {
        std::vector<uint8_t> bytes;
        int64_t t0 = 0, t1 = 0, t2 = 0;
    };
    struct ProducerSnap {
        StatSummary encode;
        uint64_t framesEncoded, overrunDrops, discontinuities, encodeErrors, primes;
        double adjustPpm, integPpm;
        int32_t minHeadroom; bool minHeadroomValid;
        uint32_t marginFrames;
        float peaks[8];
    };
    struct ConsumerSnap {
        StatSummary accumulation, queue, outputDelay, submit, total, level;
        uint64_t burstsPlayed, underruns, padFrames, outputStalls;
    };

    void encodeFrame(int64_t tNowNs);
    void controlTick();
    bool buildSilenceBurst(std::string& error);
    void publishProducer();
    void publishConsumer();
    void recordBurstStart(const BurstSlot& slot, int64_t tNowNs, uint32_t offsetFrames, int64_t deviceQueued);

    EngineConfig cfg_;
    EncoderCapabilities caps_;
    TransportDescriptor transport_;
    std::unique_ptr<ISurroundEncoder> encoder_;
    std::unique_ptr<IBitstreamPacketizer> packetizer_;
    double dtNs_ = 0;
    uint32_t burstFrames_ = 0;
    uint32_t bytesPerFrame_ = 4;

    // ---- shared ----
    std::array<BurstSlot, kSlots> slots_;
    BurstSlot silence_;  // an encoded frame of digital silence, sent while no audio is ready
    std::atomic<uint32_t> head_{0};
    std::atomic<uint32_t> tail_{0};
    std::atomic<int32_t> curRemainPub_{0};
    std::atomic<int32_t> minHeadroom_{INT32_MAX};
    std::atomic<uint32_t> starveSeq_{1};
    std::atomic<uint32_t> resumeSlackFrames_{0};  // slack a burst needs before output resumes after running dry
    std::atomic<uint64_t> underrunEvents_{0};
    std::atomic<uint32_t> statsEpoch_{0};
    SeqLock<ProducerSnap> producerSnap_;
    SeqLock<ConsumerSnap> consumerSnap_;

    // ---- producer-owned ----
    DriftResampler resampler_;
    float* const* planes_ = nullptr;
    uint32_t accumFill_ = 0;
    int64_t accumT0_ = 0;
    uint32_t tailLocal_ = 0;
    uint32_t primedSeq_ = 0;
    uint32_t margin_ = 0;
    uint64_t seenUnderruns_ = 0;
    uint32_t windowFrames_ = 0;
    uint32_t skipWindows_ = 0;
    double integ_ = 0, adjust_ = 0;
    int32_t lastMinHeadroom_ = 0;
    bool lastMinHeadroomValid_ = false;
    uint32_t producerEpoch_ = 0;
    uint32_t publishCountdown_ = 0;
    Histogram hEncode_;
    uint64_t framesEncoded_ = 0, overrunDrops_ = 0, discontinuities_ = 0, encodeErrors_ = 0, primes_ = 0;
    float peaks_[8] = {};

    // ---- consumer-owned ----
    BurstSlot* cur_ = nullptr;
    uint32_t headLocal_ = 0;
    uint32_t burstOff_ = 0;
    bool curIsSilence_ = false;
    int32_t curRemain_ = 0;
    bool starving_ = true, streamStarted_ = false;
    uint32_t consumerEpoch_ = 0;
    int64_t lastConsumerPublishNs_ = 0;
    Histogram hAccum_, hQueue_, hOutDelay_, hSubmit_, hTotal_, hLevel_;
    uint64_t burstsPlayed_ = 0, underruns_ = 0, padFrames_ = 0, outputStalls_ = 0;
    int64_t lastRenderNs_ = 0;   // consumer thread: time of the previous render() call
};

}  // namespace bs
