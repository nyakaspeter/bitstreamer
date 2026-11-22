#include "Capture.h"

#include <algorithm>
#include <cmath>

namespace bs {

bool buildChannelMap(const AudioFormat& f, int map[6], std::string& error) {
    // The input device is always configured for 5.1. Windows offers two 5.1
    // layouts (side or back surrounds); both are six discrete channels in the
    // same order, so no mixing is ever involved.
    const bool is51 = f.channels == 6 && (f.channelMask == spk::Surround51Back || f.channelMask == spk::Surround51Side ||
                                          f.channelMask == 0);
    if (!is51) {
        error = "the input device is not set to 5.1 (it reports " + describeChannelMask(f.channelMask, f.channels) + ")";
        return false;
    }
    for (int i = 0; i < 6; ++i) map[i] = i;  // FL FR FC LFE Ls Rs
    return true;
}

void convertToCanonical(const uint8_t* src, const AudioFormat& fmt, const int map[6], uint32_t frames, float* dst) {
    const uint32_t stride = fmt.bytesPerFrame();
    const uint32_t bps = fmt.bytesPerSample();
    for (int c = 0; c < 6; ++c) {
        const uint8_t* p = src + static_cast<size_t>(map[c]) * bps;
        float* d = dst + c;
        switch (fmt.sampleFormat) {
            case SampleFormat::Float32:
                for (uint32_t i = 0; i < frames; ++i, p += stride, d += 6) std::memcpy(d, p, 4);
                break;
            case SampleFormat::Int16:
                for (uint32_t i = 0; i < frames; ++i, p += stride, d += 6) {
                    int16_t v;
                    std::memcpy(&v, p, 2);
                    *d = static_cast<float>(v) * (1.0f / 32768.0f);
                }
                break;
            case SampleFormat::Int24Packed:
                for (uint32_t i = 0; i < frames; ++i, p += stride, d += 6) {
                    const int32_t v = static_cast<int32_t>((static_cast<uint32_t>(p[0]) << 8) |
                                                           (static_cast<uint32_t>(p[1]) << 16) |
                                                           (static_cast<uint32_t>(p[2]) << 24)) >> 8;
                    *d = static_cast<float>(v) * (1.0f / 8388608.0f);
                }
                break;
            case SampleFormat::Int32:
                for (uint32_t i = 0; i < frames; ++i, p += stride, d += 6) {
                    int32_t v;
                    std::memcpy(&v, p, 4);
                    *d = static_cast<float>(static_cast<double>(v) * (1.0 / 2147483648.0));
                }
                break;
            default:
                for (uint32_t i = 0; i < frames; ++i, d += 6) *d = 0.0f;
                break;
        }
    }
}

namespace {

constexpr uint32_t kScratchFrames = 4096;

// WASAPI loopback capture of a playback endpoint: taps the audio engine's
// mix for that endpoint, in the endpoint's own format, without touching the
// device driver's buffers.
class LoopbackCapture final : public ICaptureSource {
public:
    ~LoopbackCapture() override { stop(); }

    bool open(const CaptureParams& params, std::string& error) override {
        info_.backend = CaptureBackend::Loopback;
        info_.deviceId = params.deviceId;
        device_ = openDevice(params.deviceId, error);
        if (!device_) return false;
        for (const auto& d : enumerateDevices(DeviceFlow::Render))
            if (d.id == params.deviceId) info_.deviceName = d.name;
        event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
        scratch_.assign(static_cast<size_t>(kScratchFrames) * 6, 0.0f);

        HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(hr) || !client_) { error = "Activate(IAudioClient): " + hrToString(hr); return false; }
        REFERENCE_TIME defP = 0, minP = 0;
        client_->GetDevicePeriod(&defP, &minP);

        WAVEFORMATEX* mixRaw = nullptr;
        hr = client_->GetMixFormat(&mixRaw);
        if (FAILED(hr) || !mixRaw) { error = "GetMixFormat: " + hrToString(hr); return false; }
        std::unique_ptr<WAVEFORMATEX, void (*)(WAVEFORMATEX*)> mix(mixRaw, [](WAVEFORMATEX* p) { CoTaskMemFree(p); });
        AudioFormat fmt = parseWaveFormat(mix.get());
        if (fmt.sampleFormat == SampleFormat::Unknown) { error = "unsupported device sample format"; return false; }
        if (fmt.sampleRate != 48000) { error = "the input device is not running at 48000 Hz"; return false; }
        if (!buildChannelMap(fmt, map_, error)) return false;
        openKeepAlive(mix.get());
        hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_LOOPBACK, 0, 0,
                                 mix.get(), nullptr);
        if (FAILED(hr)) { error = "Initialize(loopback capture): " + hrToString(hr); return false; }
        info_.format = fmt;
        info_.periodFrames = static_cast<uint32_t>(defP * fmt.sampleRate / 10'000'000LL);

        hr = client_->GetBufferSize(&info_.bufferFrames);
        if (FAILED(hr)) { error = "GetBufferSize: " + hrToString(hr); return false; }
        hr = client_->SetEventHandle(event_.get());
        if (FAILED(hr)) { error = "SetEventHandle: " + hrToString(hr); return false; }
        hr = client_->GetService(IID_PPV_ARGS(&capture_));
        if (FAILED(hr)) { error = "GetService(IAudioCaptureClient): " + hrToString(hr); return false; }
        dtNs_ = 1e9 / static_cast<double>(info_.format.sampleRate);
        return true;
    }

    const CaptureInfo& info() const override { return info_; }
    HANDLE event() const override { return event_.get(); }

    bool start(std::string& error) override {
        if (keepClient_) {
            feedKeepAlive();
            keepClient_->Start();
        }
        const HRESULT hr = client_->Start();
        if (FAILED(hr)) { error = "capture Start: " + hrToString(hr); return false; }
        started_ = true;
        firstPacketNs_ = 0;
        return true;
    }

    void stop() override {
        if (started_ && client_) client_->Stop();
        if (started_ && keepClient_) keepClient_->Stop();
        started_ = false;
    }

    bool read(ICaptureSink& sink, HRESULT& failure) override {
        if (keepClient_) feedKeepAlive();
        for (;;) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 devPos = 0, qpcPos = 0;
            const HRESULT hr = capture_->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) return true;
            if (FAILED(hr)) { failure = hr; return false; }
            const int64_t now = nowNs();
            int64_t tFirst = static_cast<int64_t>(qpcPos) * 100;
            const int64_t fallback = now - static_cast<int64_t>(frames * dtNs_);
            // Causality: the last frame of a packet cannot be newer than "now".
            // A timestamp that violates this (or is missing) is replaced by the
            // conservative bound, which can only make reported latency larger.
            if ((flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) || qpcPos == 0 || tFirst > fallback ||
                now - tFirst > 1'000'000'000LL) {
                tFirst = fallback;
            }
            // While the stream is starting up, packets are flagged as not
            // continuous (always the first one, sometimes another right after
            // it). That is not a gap in the sound, so the flag only counts
            // once the stream has been running for half a second.
            if (firstPacketNs_ == 0) firstPacketNs_ = now;
            bool disc = (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && now - firstPacketNs_ > 500'000'000LL;
            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;

            uint32_t done = 0;
            while (done < frames) {
                const uint32_t n = std::min(frames - done, kScratchFrames);
                if (silent) {
                    std::fill_n(scratch_.data(), static_cast<size_t>(n) * 6, 0.0f);
                } else {
                    convertToCanonical(data + static_cast<size_t>(done) * info_.format.bytesPerFrame(), info_.format,
                                       map_, n, scratch_.data());
                }
                sink.onCapture(scratch_.data(), n, tFirst + static_cast<int64_t>(done * dtNs_), now, disc);
                disc = false;
                done += n;
            }
            capture_->ReleaseBuffer(frames);
        }
    }

private:
    // Loopback capture only delivers packets while something is playing to the
    // endpoint. A silent shared-mode stream of our own keeps the engine (and
    // therefore the packet flow and the encoder) running during silence.
    void openKeepAlive(const WAVEFORMATEX* mix) {
        ComPtr<IAudioClient> c;
        if (FAILED(device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &c)) || !c) return;
        if (FAILED(c->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000, 0, mix, nullptr))) return;
        if (FAILED(c->GetBufferSize(&keepFrames_)) || FAILED(c->GetService(IID_PPV_ARGS(&keepRender_)))) return;
        keepClient_ = c;
    }
    void feedKeepAlive() {
        UINT32 padding = 0;
        if (FAILED(keepClient_->GetCurrentPadding(&padding))) return;
        const UINT32 n = keepFrames_ - padding;
        if (n < keepFrames_ / 4) return;
        BYTE* p = nullptr;
        if (SUCCEEDED(keepRender_->GetBuffer(n, &p))) keepRender_->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
    }

    CaptureInfo info_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    ComPtr<IAudioClient> keepClient_;
    ComPtr<IAudioRenderClient> keepRender_;
    UINT32 keepFrames_ = 0;
    UniqueHandle event_;
    std::vector<float> scratch_;
    int map_[6] = {0, 1, 2, 3, 4, 5};
    double dtNs_ = 0;
    bool started_ = false;
    int64_t firstPacketNs_ = 0;  // when the first packet after Start() arrived
};

// Timer-paced silence source. It exercises the complete scheduling path
// without any audio hardware and gives reproducible benchmark input.
class SyntheticCapture final : public ICaptureSource {
public:
    bool open(const CaptureParams& params, std::string& error) override {
        info_.backend = CaptureBackend::Synthetic;
        info_.deviceName = "synthetic 5.1 source";
        info_.format = {48000, 6, spk::Surround51Side, SampleFormat::Float32, 32};
        periodHns_ = params.periodHns > 0 ? params.periodHns : 100'000;
        info_.periodFrames = static_cast<uint32_t>(periodHns_ * 48000 / 10'000'000LL);
        info_.bufferFrames = info_.periodFrames;
        timer_.reset(CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS));
        if (!timer_) timer_.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
        if (!timer_) { error = "cannot create timer"; return false; }
        scratch_.assign(static_cast<size_t>(kScratchFrames) * 6, 0.0f);
        return true;
    }
    const CaptureInfo& info() const override { return info_; }
    HANDLE event() const override { return timer_.get(); }
    bool start(std::string&) override {
        startNs_ = nowNs();
        delivered_ = 0;
        arm();
        return true;
    }
    void stop() override { if (timer_) CancelWaitableTimer(timer_.get()); }
    bool read(ICaptureSink& sink, HRESULT&) override {
        const int64_t now = nowNs();
        const uint64_t due = static_cast<uint64_t>((now - startNs_) * 48000 / 1'000'000'000LL);
        // Deliver whole periods only, like a real device.
        while (delivered_ + info_.periodFrames <= due) {
            const uint32_t n = std::min(info_.periodFrames, kScratchFrames);
            const int64_t tFirst = startNs_ + static_cast<int64_t>(delivered_ * 1'000'000'000ULL / 48000);
            sink.onCapture(scratch_.data(), n, tFirst, now, false);
            delivered_ += n;
        }
        arm();
        return true;
    }

private:
    void arm() {
        const int64_t nextNs = startNs_ + static_cast<int64_t>((delivered_ + info_.periodFrames) * 1'000'000'000ULL / 48000);
        LARGE_INTEGER due;
        due.QuadPart = -std::max<int64_t>(1, (nextNs - nowNs()) / 100);
        SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE);
    }
    CaptureInfo info_;
    UniqueHandle timer_;
    std::vector<float> scratch_;
    int64_t periodHns_ = 100'000;
    int64_t startNs_ = 0;
    uint64_t delivered_ = 0;
};

}  // namespace

std::unique_ptr<ICaptureSource> createCaptureSource(CaptureBackend backend) {
    if (backend == CaptureBackend::Synthetic) return std::make_unique<SyntheticCapture>();
    return std::make_unique<LoopbackCapture>();
}

}  // namespace bs
