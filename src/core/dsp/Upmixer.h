// Stereo-to-surround upmixing, applied to the captured 5.1 PCM in place.
//
//   Off              stereo plays on the front left and right only.
//   Quad             front left / right are mirrored to the surrounds.
//   VirtualSurround  the stereo signal is split into frequency bands and
//                    each band is steered on its own. In every band the
//                    dominant sound is found and placed where it sits in
//                    the stereo image: between left, centre and right when
//                    the two channels carry it in phase (speech moves to
//                    the centre), towards the surrounds when they carry it
//                    out of phase. What the two channels do not share
//                    (ambience, reverb) stays on the fronts and is also
//                    played, delayed, on the surround of the same side. The
//                    low end feeds the subwoofer.
//
// Upmixing only acts on stereo material: as soon as the source itself uses
// the centre, LFE or surround channels (a real 5.1 mix), it is passed through
// untouched, and the upmix fades back in once those channels have been silent
// for a moment.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs {

enum class UpmixMode : int { Off = 0, Quad = 1, VirtualSurround = 2 };

class Upmixer {
public:
    static constexpr int kMaxBands = 8;

    explicit Upmixer(uint32_t sampleRate = 48000);

    // Thread-safe; takes effect on the next processed block.
    void setMode(UpmixMode mode) { mode_.store(static_cast<int>(mode), std::memory_order_relaxed); }
    UpmixMode mode() const { return static_cast<UpmixMode>(mode_.load(std::memory_order_relaxed)); }

    // Audio thread. `interleaved` is canonical 5.1: FL FR FC LFE Ls Rs.
    void process(float* interleaved, uint32_t frames);

    void reset();

private:
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        double z1 = 0, z2 = 0;
        double run(double x) {
            const double y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    // Splits one channel into bands with 4th-order Linkwitz-Riley crossovers
    // arranged as a tree. The low and high output of such a crossover add up
    // to an all-pass, and each branch is passed through the all-passes of the
    // crossovers it does not contain, so all bands share one phase response
    // and sum back to the input without colouring it.
    struct BandSplitter {
        struct Node {
            Biquad lp[2], hp[2];
            std::array<Biquad, kMaxBands> apLow, apHigh;
            int apLowCount = 0, apHighCount = 0;
            int lowChild = -1, highChild = -1;  // node index, or -1 when the output is a band
            int lowBand = 0, highBand = 0;
        };
        std::array<Node, kMaxBands> nodes;
        int nodeCount = 0;

        void design(const double* crossoverHz, int crossovers, uint32_t rate);
        void run(double x, double* out) { runNode(0, x, out); }

    private:
        int build(const double* hz, int lo, int hi, uint32_t rate);
        void runNode(int index, double x, double* out);
    };
    struct BandState {
        double pLL = 0, pRR = 0, pLR = 0;  // smoothed covariance of the band's L and R
        // Output mix of this band: front L, front R, centre, surround L,
        // surround R, each as a blend of the band's L and R. `target` is
        // recomputed from the covariance, `gain` follows it smoothly.
        float target[5][2] = {};
        float gain[5][2] = {};
    };

    void updateSteering(int band);

    std::atomic<int> mode_{0};
    uint32_t rate_;

    // Detection of real multichannel content.
    uint32_t multichannelHold_ = 0;  // samples until the source counts as stereo again
    float mix_ = 0.0f;               // 0 = pass through, 1 = fully upmixed
    int activeMode_ = 0;             // mode the current mix_ belongs to

    // Virtual surround state.
    BandSplitter splitL_, splitR_;
    std::array<BandState, kMaxBands> band_;
    uint32_t steerCountdown_ = 0;
    std::vector<float> delayL_, delayR_;  // surround delay lines
    size_t delayPos_ = 0;
    float lfeLp1_ = 0.0f, lfeLp2_ = 0.0f; // two cascaded one-pole low-passes for the LFE
};

}  // namespace bs
