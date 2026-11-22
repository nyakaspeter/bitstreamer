#include "Upmixer.h"

#include <algorithm>
#include <cmath>

#include <pmmintrin.h>
#include <xmmintrin.h>

namespace bs {

namespace {
constexpr float kSilence = 1e-4f;            // -80 dBFS: below this a channel counts as unused
constexpr double kHoldSeconds = 1.5;         // source must be stereo-only this long before upmixing
constexpr double kFadeInSeconds = 0.15;
constexpr double kFadeOutSeconds = 0.01;     // real 5.1 content takes over almost at once
constexpr double kSurroundDelaySeconds = 0.012;
constexpr float kLfeLowpassHz = 110.0f;
constexpr float kLfeGain = 0.35f;            // decoders play the LFE channel 10 dB hot
constexpr double kGainSmoothSeconds = 0.02;
constexpr uint32_t kSteerInterval = 16;      // samples between steering updates
constexpr double kDominanceKnee = 0.1;       // below this a band counts as having no dominant sound

// Six bands, a little over an octave each, with the voice range (500 Hz to
// 2.8 kHz) split in two so speech and music sharing it can part ways. The
// decoders this follows (DTS Neo:6, Dolby's later upmixers, Faller's method)
// steer in many more, narrower bands, but they do so on blocks of audio and
// pay for it in delay.
constexpr int kBands = 6;
constexpr double kCrossoverHz[kBands - 1] = {200.0, 500.0, 1200.0, 2800.0, 6500.0};
// Share of a band's dominant sound that is re-placed across left, centre and
// right. The rest stays where the stereo mix had it, which keeps the front
// stage from collapsing into the centre speaker (the music modes of Pro
// Logic II and Neo:6 hold back part of the centre the same way). The bass
// band is not steered, as in Pro Logic: it has no useful direction and stays
// on the front speakers.
constexpr double kFrontSteer[kBands] = {0.0, 0.6, 0.75, 0.75, 0.75, 0.6};
// Share of an out-of-phase dominant sound that is moved to the surrounds.
constexpr double kRearSteer[kBands] = {0.0, 0.7, 0.7, 0.7, 0.7, 0.5};
// Level at which a band's ambience is added to the surrounds: 6 dB down, as
// in a passive matrix decoder, and lower still at the top, which stands in
// for the high-frequency roll-off those decoders give the surrounds.
constexpr double kAmbience[kBands] = {0.0, 0.5, 0.5, 0.5, 0.5, 0.35};
// How long each band looks back to judge what the two channels share:
// around the 20 ms those decoders use, longer where the band is low and
// narrow and needs the time for a steady reading.
constexpr double kAnalysisSeconds[kBands] = {0.0, 0.05, 0.035, 0.025, 0.02, 0.015};
static_assert(kBands <= Upmixer::kMaxBands);

constexpr double kPi = 3.14159265358979323846;
constexpr double kButterworthQ = 0.70710678118654752;

float onePoleCoeff(float hz, uint32_t rate) {
    return 1.0f - std::exp(-6.2831853f * hz / static_cast<float>(rate));
}

enum class FilterKind { LowPass, HighPass, AllPass };

template <typename B>
void designBiquad(B& q, FilterKind kind, double hz, uint32_t rate) {
    const double w = 2.0 * kPi * hz / rate;
    const double cs = std::cos(w), alpha = std::sin(w) / (2.0 * kButterworthQ);
    const double a0 = 1.0 + alpha;
    double b0 = 0, b1 = 0, b2 = 0;
    switch (kind) {
    case FilterKind::LowPass:  b0 = (1 - cs) / 2; b1 = 1 - cs;    b2 = (1 - cs) / 2; break;
    case FilterKind::HighPass: b0 = (1 + cs) / 2; b1 = -(1 + cs); b2 = (1 + cs) / 2; break;
    case FilterKind::AllPass:  b0 = 1 - alpha;    b1 = -2 * cs;   b2 = 1 + alpha;    break;
    }
    q.b0 = b0 / a0; q.b1 = b1 / a0; q.b2 = b2 / a0;
    q.a1 = -2 * cs / a0; q.a2 = (1 - alpha) / a0;
    q.z1 = q.z2 = 0;
}
}  // namespace

void Upmixer::BandSplitter::design(const double* crossoverHz, int crossovers, uint32_t rate) {
    nodeCount = 0;
    build(crossoverHz, 0, crossovers, rate);
}

// Builds the node for crossovers [lo, hi). Crossover i separates band i from band i + 1.
int Upmixer::BandSplitter::build(const double* hz, int lo, int hi, uint32_t rate) {
    const int index = nodeCount++;
    const int mid = (lo + hi) / 2;
    Node& n = nodes[static_cast<size_t>(index)];
    n = Node{};
    for (auto& q : n.lp) designBiquad(q, FilterKind::LowPass, hz[mid], rate);
    for (auto& q : n.hp) designBiquad(q, FilterKind::HighPass, hz[mid], rate);
    // Each side makes up for the crossovers that only the other side has.
    for (int i = mid + 1; i < hi; ++i) designBiquad(n.apLow[static_cast<size_t>(n.apLowCount++)], FilterKind::AllPass, hz[i], rate);
    for (int i = lo; i < mid; ++i) designBiquad(n.apHigh[static_cast<size_t>(n.apHighCount++)], FilterKind::AllPass, hz[i], rate);
    n.lowBand = mid;
    n.highBand = mid + 1;
    const int low = mid > lo ? build(hz, lo, mid, rate) : -1;
    const int high = hi > mid + 1 ? build(hz, mid + 1, hi, rate) : -1;
    nodes[static_cast<size_t>(index)].lowChild = low;
    nodes[static_cast<size_t>(index)].highChild = high;
    return index;
}

void Upmixer::BandSplitter::runNode(int index, double x, double* out) {
    Node& n = nodes[static_cast<size_t>(index)];
    double lo = n.lp[1].run(n.lp[0].run(x));
    double hi = n.hp[1].run(n.hp[0].run(x));
    for (int i = 0; i < n.apLowCount; ++i) lo = n.apLow[static_cast<size_t>(i)].run(lo);
    for (int i = 0; i < n.apHighCount; ++i) hi = n.apHigh[static_cast<size_t>(i)].run(hi);
    if (n.lowChild >= 0) runNode(n.lowChild, lo, out); else out[n.lowBand] = lo;
    if (n.highChild >= 0) runNode(n.highChild, hi, out); else out[n.highBand] = hi;
}

Upmixer::Upmixer(uint32_t sampleRate) : rate_(sampleRate) {
    const size_t delay = static_cast<size_t>(kSurroundDelaySeconds * sampleRate) + 1;
    delayL_.assign(delay, 0.0f);
    delayR_.assign(delay, 0.0f);
    reset();
}

void Upmixer::reset() {
    multichannelHold_ = static_cast<uint32_t>(kHoldSeconds * rate_);
    mix_ = 0.0f;
    activeMode_ = 0;
    splitL_.design(kCrossoverHz, kBands - 1, rate_);
    splitR_.design(kCrossoverHz, kBands - 1, rate_);
    for (BandState& b : band_) {
        b = BandState{};
        b.target[0][0] = b.gain[0][0] = 1.0f;  // front left = the band's left
        b.target[1][1] = b.gain[1][1] = 1.0f;  // front right = the band's right
    }
    steerCountdown_ = 0;
    std::fill(delayL_.begin(), delayL_.end(), 0.0f);
    std::fill(delayR_.begin(), delayR_.end(), 0.0f);
    delayPos_ = 0;
    lfeLp1_ = lfeLp2_ = 0.0f;
}

// Works out where a band's sound belongs from the covariance of its two
// channels. The dominant sound is the principal component: its direction is
// an angle that runs once around the listener,
//
//     0° left   45° centre   90° right   120° right surround
//     150° left surround   180° left again (with the sign flipped)
//
// the front half being sound the channels carry in phase, the rear half sound
// they carry out of phase (the convention of matrix-encoded surround). Its
// share of the band's power says how much of the band it is. That part is
// taken out of left and right and played at constant power on the two
// speakers next to its direction. What is left over is ambience: it stays on
// the fronts and is added to the surround of its own side.
void Upmixer::updateSteering(int band) {
    BandState& s = band_[static_cast<size_t>(band)];
    double dominance = 0.0, theta = 0.0;
    const double total = s.pLL + s.pRR;
    if (total > 1e-10) {
        const double dx = s.pLL - s.pRR, dy = 2.0 * s.pLR;
        dominance = std::clamp((std::sqrt(dx * dx + dy * dy) / total - kDominanceKnee) / (1.0 - kDominanceKnee), 0.0, 1.0);
        theta = 0.5 * std::atan2(dy, dx);
        if (theta < 0.0) theta += kPi;
    }
    const double a = std::cos(theta), b = std::sin(theta);  // dominant sound = a * left + b * right

    double gL = 0, gR = 0, gC = 0, gLs = 0, gRs = 0, steer;
    if (theta <= kPi / 2) {
        steer = kFrontSteer[band];
        if (theta <= kPi / 4) { gL = std::cos(2 * theta); gC = std::sin(2 * theta); }
        else { const double u = 2 * (theta - kPi / 4); gC = std::cos(u); gR = std::sin(u); }
    } else {
        steer = kRearSteer[band];
        if (theta <= 2 * kPi / 3) { const double v = 3 * (theta - kPi / 2); gR = std::cos(v); gRs = std::sin(v); }
        else if (theta <= 5 * kPi / 6) { const double v = 3 * (theta - 2 * kPi / 3); gRs = std::cos(v); gLs = -std::sin(v); }
        else { const double v = 3 * (theta - 5 * kPi / 6); gLs = -std::cos(v); gL = -std::sin(v); }
    }
    const double moved = steer * dominance;
    const double amb = kAmbience[band];
    auto set = [&s](int out, double fromLeft, double fromRight) {
        s.target[out][0] = static_cast<float>(fromLeft);
        s.target[out][1] = static_cast<float>(fromRight);
    };
    set(0, 1.0 + moved * (gL - a) * a, moved * (gL - a) * b);
    set(1, moved * (gR - b) * a, 1.0 + moved * (gR - b) * b);
    set(2, moved * gC * a, moved * gC * b);
    set(3, moved * gLs * a + amb * (1.0 - dominance * a * a), moved * gLs * b - amb * dominance * a * b);
    set(4, moved * gRs * a - amb * dominance * a * b, moved * gRs * b + amb * (1.0 - dominance * b * b));
}

void Upmixer::process(float* x, uint32_t frames) {
    const int wanted = mode_.load(std::memory_order_relaxed);
    if (wanted == 0 && mix_ <= 0.0f) {
        // Nothing to do; keep the content detector primed for when it is switched on.
        activeMode_ = 0;
        return;
    }
    // Filter states decay towards zero in silence; without this the CPU would
    // crawl through denormal numbers.
    const unsigned int savedCsr = _mm_getcsr();
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);

    const uint32_t hold = static_cast<uint32_t>(kHoldSeconds * rate_);
    const float fadeIn = 1.0f / static_cast<float>(kFadeInSeconds * rate_);
    const float fadeOut = 1.0f / static_cast<float>(kFadeOutSeconds * rate_);
    const float gainSmooth = 1.0f / static_cast<float>(kGainSmoothSeconds * rate_);
    const float aLfe = onePoleCoeff(kLfeLowpassHz, rate_);
    double powerSmooth[kBands] = {};
    bool steered[kBands] = {};
    for (int b = 0; b < kBands; ++b) {
        steered[b] = kFrontSteer[b] > 0.0 || kRearSteer[b] > 0.0 || kAmbience[b] > 0.0;
        if (steered[b]) powerSmooth[b] = 1.0 / (kAnalysisSeconds[b] * rate_);
    }

    for (uint32_t i = 0; i < frames; ++i) {
        float* f = x + static_cast<size_t>(i) * 6;
        const float l = f[0], r = f[1];

        // Real multichannel content present? Then it must not be altered.
        const bool multichannel = std::fabs(f[2]) > kSilence || std::fabs(f[3]) > kSilence ||
                                  std::fabs(f[4]) > kSilence || std::fabs(f[5]) > kSilence;
        if (multichannel) multichannelHold_ = hold;
        else if (multichannelHold_ > 0) --multichannelHold_;

        // A mode change fades the old mode out before the new one fades in.
        const bool on = wanted != 0 && multichannelHold_ == 0 && (activeMode_ == wanted || mix_ <= 0.0f);
        if (on) {
            activeMode_ = wanted;
            mix_ = std::min(1.0f, mix_ + fadeIn);
        } else {
            mix_ = std::max(0.0f, mix_ - fadeOut);
        }

        // The band split and analysis run continuously so their state is
        // settled when the upmix fades in.
        double bl[kMaxBands], br[kMaxBands];
        splitL_.run(l, bl);
        splitR_.run(r, br);
        const bool steerNow = steerCountdown_ == 0;
        steerCountdown_ = steerNow ? kSteerInterval - 1 : steerCountdown_ - 1;

        double out[5] = {};  // front L, front R, centre, surround L, surround R
        for (int b = 0; b < kBands; ++b) {
            if (!steered[b]) {
                out[0] += bl[b];
                out[1] += br[b];
                continue;
            }
            BandState& s = band_[static_cast<size_t>(b)];
            s.pLL += (bl[b] * bl[b] - s.pLL) * powerSmooth[b];
            s.pRR += (br[b] * br[b] - s.pRR) * powerSmooth[b];
            s.pLR += (bl[b] * br[b] - s.pLR) * powerSmooth[b];
            if (steerNow) updateSteering(b);
            for (int o = 0; o < 5; ++o) {
                s.gain[o][0] += (s.target[o][0] - s.gain[o][0]) * gainSmooth;
                s.gain[o][1] += (s.target[o][1] - s.gain[o][1]) * gainSmooth;
                out[o] += s.gain[o][0] * bl[b] + s.gain[o][1] * br[b];
            }
        }

        // The surrounds are delayed so they read as ambience behind the
        // listener rather than as a second front stage.
        const float delayedL = delayL_[delayPos_], delayedR = delayR_[delayPos_];
        delayL_[delayPos_] = static_cast<float>(out[3]);
        delayR_[delayPos_] = static_cast<float>(out[4]);
        if (++delayPos_ >= delayL_.size()) delayPos_ = 0;
        const float mid = 0.5f * (l + r);
        lfeLp1_ += (mid - lfeLp1_) * aLfe;
        lfeLp2_ += (lfeLp1_ - lfeLp2_) * aLfe;

        if (mix_ <= 0.0f) continue;

        float ul = l, ur = r, uc = 0.0f, ulfe = 0.0f, uls = 0.0f, urs = 0.0f;
        if (activeMode_ == static_cast<int>(UpmixMode::Quad)) {
            uls = l;
            urs = r;
        } else {
            ul = static_cast<float>(out[0]);
            ur = static_cast<float>(out[1]);
            uc = static_cast<float>(out[2]);
            uls = delayedL;
            urs = delayedR;
            ulfe = lfeLp2_ * kLfeGain;
        }
        const float m = mix_;
        f[0] = l + (ul - l) * m;
        f[1] = r + (ur - r) * m;
        f[2] += uc * m;
        f[3] += ulfe * m;
        f[4] += uls * m;
        f[5] += urs * m;
    }
    _mm_setcsr(savedCsr);
}

}  // namespace bs
