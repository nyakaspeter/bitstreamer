#include "DriftResampler.h"

#include <cmath>
#include <cstring>

namespace bs {

namespace {
double besselI0(double x) {
    double sum = 1.0, term = 1.0;
    for (int k = 1; k < 32; ++k) {
        term *= (x / (2.0 * k)) * (x / (2.0 * k));
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}
}  // namespace

void DriftResampler::configure(uint16_t channels) {
    channels_ = channels;
    const double pi = 3.14159265358979323846;
    const double beta = 9.0;
    const double i0b = besselI0(beta);
    const int half = kTaps / 2;
    table_.assign(static_cast<size_t>(kPhases + 1) * kTaps, 0.0f);
    for (int p = 0; p <= kPhases; ++p) {
        const double frac = static_cast<double>(p) / kPhases;
        double sum = 0.0;
        double coeff[kTaps];
        for (int j = 0; j < kTaps; ++j) {
            // Tap j samples hist[base + j]; the output sits at base + half - 1 + frac.
            const double x = static_cast<double>(j - (half - 1)) - frac;
            // Exact zeros at integer offsets make the zero-phase kernel a pure
            // delay, so an uncorrected stream passes through bit-exactly.
            const bool integer = std::fabs(x - std::round(x)) < 1e-9;
            const double sinc = std::fabs(x) < 1e-9 ? 1.0 : integer ? 0.0 : std::sin(pi * x) / (pi * x);
            const double r = x / half;
            const double w = std::fabs(r) >= 1.0 ? 0.0 : besselI0(beta * std::sqrt(1.0 - r * r)) / i0b;
            coeff[j] = sinc * w;
            sum += coeff[j];
        }
        for (int j = 0; j < kTaps; ++j)
            table_[static_cast<size_t>(p) * kTaps + j] = static_cast<float>(coeff[j] / sum);  // unity DC gain
    }
    hist_.assign(channels_, std::vector<float>(kTaps + kMaxPush + 8, 0.0f));
    reset();
}

void DriftResampler::reset() {
    for (auto& h : hist_) std::fill(h.begin(), h.end(), 0.0f);
    // Start with half a kernel of silence so the first output is available as
    // soon as the first half kernel of real input has arrived.
    fill_ = static_cast<size_t>(kTaps / 2 - 1);
    pos_ = 0.0;
}

void DriftResampler::push(const float* interleaved, size_t frames) {
    if (frames > kMaxPush) frames = kMaxPush;
    for (uint16_t c = 0; c < channels_; ++c) {
        float* dst = hist_[c].data() + fill_;
        const float* src = interleaved + c;
        for (size_t i = 0; i < frames; ++i) dst[i] = src[i * channels_];
    }
    fill_ += frames;
}

size_t DriftResampler::pull(float* const* dest, size_t destOffset, size_t maxFrames) {
    size_t produced = 0;
    double pos = pos_;
    const double step = step_;
    while (produced < maxFrames) {
        const size_t base = static_cast<size_t>(pos);
        if (base + kTaps > fill_) break;
        const double fracPhase = (pos - static_cast<double>(base)) * kPhases;
        const int p = static_cast<int>(fracPhase);
        const float a = static_cast<float>(fracPhase - p);
        const float* c0 = &table_[static_cast<size_t>(p) * kTaps];
        const float* c1 = c0 + kTaps;
        for (uint16_t c = 0; c < channels_; ++c) {
            const float* h = hist_[c].data() + base;
            float acc0 = 0.0f, acc1 = 0.0f;
            for (int j = 0; j < kTaps; ++j) {
                acc0 += h[j] * c0[j];
                acc1 += h[j] * c1[j];
            }
            dest[c][destOffset + produced] = acc0 + (acc1 - acc0) * a;
        }
        ++produced;
        pos += step;
    }
    // Discard fully consumed history.
    const size_t shift = static_cast<size_t>(pos);
    if (shift > 0) {
        const size_t drop = shift > fill_ ? fill_ : shift;
        for (uint16_t c = 0; c < channels_; ++c)
            std::memmove(hist_[c].data(), hist_[c].data() + drop, (fill_ - drop) * sizeof(float));
        fill_ -= drop;
        pos -= static_cast<double>(drop);
    }
    pos_ = pos;
    return produced;
}

}  // namespace bs
