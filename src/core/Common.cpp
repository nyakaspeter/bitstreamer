#include "Common.h"

#include <cstdio>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bs {

int64_t nowNs() {
    static const int64_t freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (c.QuadPart / freq) * 1'000'000'000LL + (c.QuadPart % freq) * 1'000'000'000LL / freq;
}

std::string describeChannelMask(uint32_t mask, uint16_t channels) {
    if (mask == spk::Stereo) return "stereo";
    if (mask == spk::Surround51Back) return "5.1 (back)";
    if (mask == spk::Surround51Side) return "5.1 (side)";
    if (mask == spk::Surround71) return "7.1";
    if (mask == spk::FC && channels == 1) return "mono";
    char buf[48];
    std::snprintf(buf, sizeof buf, "%u ch, mask 0x%X", channels, mask);
    return buf;
}

std::string AudioFormat::describe() const {
    const char* fmt = "?";
    switch (sampleFormat) {
        case SampleFormat::Int16: fmt = "16-bit int"; break;
        case SampleFormat::Int24Packed: fmt = "24-bit int"; break;
        case SampleFormat::Int32: fmt = validBits == 24 ? "24-in-32-bit int" : "32-bit int"; break;
        case SampleFormat::Float32: fmt = "32-bit float"; break;
        default: break;
    }
    char buf[128];
    std::snprintf(buf, sizeof buf, "%u Hz, %u ch [%s], %s", sampleRate, channels,
                  describeChannelMask(channelMask, channels).c_str(), fmt);
    return buf;
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

}  // namespace bs
