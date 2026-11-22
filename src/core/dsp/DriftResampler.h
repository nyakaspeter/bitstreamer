// Variable-ratio polyphase windowed-sinc resampler used for clock-domain
// correction between the capture clock and the output device clock.
//
// The encoded bitstream cannot be stretched, so drift has to be absorbed in
// the PCM domain before encoding. The ratio stays within a fraction of a
// percent of 1.0; at exactly 1.0 with zero phase the filter is a pure delay
// of latencyFrames() samples and passes samples unmodified.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bs {

class DriftResampler {
public:
    static constexpr int kTaps = 32;
    static constexpr int kPhases = 512;
    static constexpr size_t kMaxPush = 1024;  // frames per push() call

    void configure(uint16_t channels);
    void reset();

    // Input frames consumed per output frame (1.0 = no correction).
    void setStep(double step) { step_ = step; }
    double step() const { return step_; }

    // Appends up to kMaxPush interleaved frames. All previously pushed input
    // must have been drained with pull() first.
    void push(const float* interleaved, size_t frames);

    // Produces up to maxFrames output frames into planar destination buffers
    // starting at destOffset. Returns the number of frames produced.
    size_t pull(float* const* destPlanes, size_t destOffset, size_t maxFrames);

    // Frames between the next output sample and the newest pushed input.
    double pendingInputFrames() const { return static_cast<double>(fill_) - pos_ - kTaps / 2; }
    static constexpr double latencyFrames() { return kTaps / 2; }

private:
    uint16_t channels_ = 0;
    std::vector<float> table_;               // (kPhases + 1) * kTaps coefficients
    std::vector<std::vector<float>> hist_;   // per-channel history
    size_t fill_ = 0;
    double pos_ = 0.0;
    double step_ = 1.0;
};

}  // namespace bs
