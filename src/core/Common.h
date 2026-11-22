// Shared basic types: audio formats, monotonic time, statistics, logging.
#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace bs {

// ---------------------------------------------------------------- formats

enum class SampleFormat { Unknown, Int16, Int24Packed, Int32, Float32 };

// Speaker bits are identical to the Windows SPEAKER_* / FFmpeg AV_CH_* bits.
namespace spk {
inline constexpr uint32_t FL = 0x1, FR = 0x2, FC = 0x4, LFE = 0x8;
inline constexpr uint32_t BL = 0x10, BR = 0x20, SL = 0x200, SR = 0x400;
inline constexpr uint32_t Stereo = FL | FR;
inline constexpr uint32_t Surround51Back = FL | FR | FC | LFE | BL | BR;   // 0x3F
inline constexpr uint32_t Surround51Side = FL | FR | FC | LFE | SL | SR;   // 0x60F
inline constexpr uint32_t Surround71 = Surround51Back | SL | SR;           // 0x63F
}  // namespace spk

struct AudioFormat {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint32_t channelMask = 0;
    SampleFormat sampleFormat = SampleFormat::Unknown;
    uint16_t validBits = 0;

    uint32_t bytesPerSample() const {
        switch (sampleFormat) {
            case SampleFormat::Int16: return 2;
            case SampleFormat::Int24Packed: return 3;
            case SampleFormat::Int32:
            case SampleFormat::Float32: return 4;
            default: return 0;
        }
    }
    uint32_t bytesPerFrame() const { return bytesPerSample() * channels; }
    std::string describe() const;
};

std::string describeChannelMask(uint32_t mask, uint16_t channels);

// The canonical internal PCM representation is float32 with the channel order
// of the encoder's layout (for 5.1: FL FR FC LFE SL SR), which matches the
// Windows channel order. Encoders receive it planar.
struct AudioBufferView {
    const float* const* planes = nullptr;
    uint16_t channels = 0;
    uint32_t frames = 0;
};

// ------------------------------------------------------------------- time

// Monotonic nanoseconds derived from QueryPerformanceCounter. WASAPI QPC
// positions (100 ns units) are directly comparable after multiplying by 100.
int64_t nowNs();

// ------------------------------------------------------------- statistics

struct StatSummary {  // all values in microseconds unless noted by the owner
    uint64_t count = 0;
    double mean = 0, min = 0, max = 0, p50 = 0, p99 = 0, last = 0;
};

// Fixed-size histogram: no allocation, O(1) insert, usable on audio threads.
class Histogram {
public:
    static constexpr int kBuckets = 4096;
    explicit Histogram(double bucketWidthUs = 25.0) : width_(bucketWidthUs) { reset(); }
    void reset() {
        counts_.fill(0);
        count_ = 0; sum_ = 0; min_ = 0; max_ = 0; last_ = 0;
    }
    void add(double us) {
        if (count_ == 0) { min_ = max_ = us; }
        if (us < min_) min_ = us;
        if (us > max_) max_ = us;
        sum_ += us; last_ = us; ++count_;
        int b = us <= 0 ? 0 : static_cast<int>(us / width_);
        if (b >= kBuckets) b = kBuckets - 1;
        ++counts_[static_cast<size_t>(b)];
    }
    StatSummary summary() const {
        StatSummary s;
        s.count = count_;
        if (!count_) return s;
        s.mean = sum_ / static_cast<double>(count_);
        s.min = min_; s.max = max_; s.last = last_;
        s.p50 = percentile(0.50);
        s.p99 = percentile(0.99);
        return s;
    }
private:
    double percentile(double q) const {
        const uint64_t target = static_cast<uint64_t>(q * static_cast<double>(count_ - 1)) + 1;
        uint64_t acc = 0;
        for (int i = 0; i < kBuckets; ++i) {
            acc += counts_[static_cast<size_t>(i)];
            if (acc >= target) {
                double v = (i + 0.5) * width_;
                return v < min_ ? min_ : (v > max_ ? max_ : v);
            }
        }
        return max_;
    }
    double width_;
    std::array<uint32_t, kBuckets> counts_{};
    uint64_t count_ = 0;
    double sum_ = 0, min_ = 0, max_ = 0, last_ = 0;
};

// Single-writer / multi-reader snapshot publication without locks. T must be
// trivially copyable. The writer never blocks; readers retry on a torn read.
template <typename T>
class SeqLock {
public:
    void write(const T& v) {
        const uint32_t s = seq_.load(std::memory_order_relaxed);
        seq_.store(s + 1, std::memory_order_release);
        std::memcpy(&value_, &v, sizeof(T));
        seq_.store(s + 2, std::memory_order_release);
    }
    T read() const {
        T out;
        for (;;) {
            const uint32_t a = seq_.load(std::memory_order_acquire);
            if (a & 1u) continue;
            std::memcpy(&out, &value_, sizeof(T));
            if (seq_.load(std::memory_order_acquire) == a) return out;
        }
    }
private:
    std::atomic<uint32_t> seq_{0};
    T value_{};
};

std::string narrow(std::wstring_view w);

}  // namespace bs
