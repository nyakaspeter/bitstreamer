#include "Output.h"

#include <algorithm>

#include "../engine/BitstreamEngine.h"

namespace bs {

namespace {

class WasapiBitstreamOutput final : public IBitstreamSink {
public:
    ~WasapiBitstreamOutput() override { stop(); }

    bool open(const OutputParams& params, std::string& error) override {
        device_ = openDevice(params.deviceId, error);
        if (!device_) return false;
        for (const auto& d : enumerateDevices(DeviceFlow::Render))
            if (d.id == params.deviceId) info_.deviceName = d.name;
        rate_ = params.transport.transportSampleRate;
        event_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));

        if (!activate(error)) return false;
        REFERENCE_TIME defP = 0, minP = 0;
        client_->GetDevicePeriod(&defP, &minP);
        info_.defaultPeriodHns = defP;
        info_.minPeriodHns = minP;

        // Format negotiation: the device must accept one of the compressed
        // (IEC 61937) representations of this bitstream.
        const auto candidates = bitstreamFormatCandidates(params.transport);
        const BitstreamFormat* chosen = nullptr;
        for (const auto& c : candidates) {
            if (client_->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, c.format.wfx(), nullptr) == S_OK) {
                chosen = &c;
                break;
            }
        }
        if (!chosen) {
            error = "the output device did not accept any " + params.transport.name +
                    " format in exclusive mode. The device or the connected sink does not advertise support "
                    "for this bitstream.";
            return false;
        }
        info_.formatLabel = chosen->label;

        std::vector<REFERENCE_TIME> periods;
        if (params.periodHns > 0) periods.push_back(std::max<REFERENCE_TIME>(params.periodHns, minP));
        periods.push_back(minP);
        periods.push_back(defP);
        HRESULT lastHr = E_FAIL;
        bool ok = false;
        for (REFERENCE_TIME period : periods) {
            if (period <= 0) continue;
            HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                             period, chosen->format.wfx(), nullptr);
            if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED) {
                UINT32 aligned = 0;
                client_->GetBufferSize(&aligned);
                period = static_cast<REFERENCE_TIME>(10'000'000.0 * aligned / rate_ + 0.5);
                if (!activate(error)) return false;
                hr = client_->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, period,
                                         period, chosen->format.wfx(), nullptr);
            }
            if (SUCCEEDED(hr)) { info_.periodHns = period; ok = true; break; }
            lastHr = hr;
            if (!activate(error)) return false;
            if (hr == AUDCLNT_E_DEVICE_IN_USE || hr == AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED) break;
        }
        if (!ok) { error = "Initialize(exclusive output): " + hrToString(lastHr); return false; }

        HRESULT hr = client_->GetBufferSize(&info_.bufferFrames);
        if (FAILED(hr)) { error = "GetBufferSize: " + hrToString(hr); return false; }
        hr = client_->SetEventHandle(event_.get());
        if (FAILED(hr)) { error = "SetEventHandle: " + hrToString(hr); return false; }
        hr = client_->GetService(IID_PPV_ARGS(&render_));
        if (FAILED(hr)) { error = "GetService(IAudioRenderClient): " + hrToString(hr); return false; }
        REFERENCE_TIME lat = 0;
        if (SUCCEEDED(client_->GetStreamLatency(&lat))) info_.streamLatencyHns = lat;
        if (SUCCEEDED(client_->GetService(IID_PPV_ARGS(&clock_))) && clock_) {
            UINT64 freq = 0;
            if (SUCCEEDED(clock_->GetFrequency(&freq)) && freq > 0) {
                clockFreq_ = static_cast<double>(freq);
                info_.clockAvailable = true;
            }
        }
        bytesPerFrame_ = chosen->format.wfx()->nBlockAlign;
        return true;
    }

    const OutputInfo& info() const override { return info_; }
    HANDLE event() const override { return event_.get(); }

    bool start(std::string& error) override {
        // Event-driven exclusive streams require the first buffer up front.
        BYTE* p = nullptr;
        HRESULT hr = render_->GetBuffer(info_.bufferFrames, &p);
        if (FAILED(hr)) { error = "GetBuffer(prefill): " + hrToString(hr); return false; }
        std::memset(p, 0, static_cast<size_t>(info_.bufferFrames) * bytesPerFrame_);
        render_->ReleaseBuffer(info_.bufferFrames, 0);
        written_ = info_.bufferFrames;
        hr = client_->Start();
        if (FAILED(hr)) { error = "output Start: " + hrToString(hr); return false; }
        started_ = true;
        return true;
    }

    void stop() override {
        if (started_ && client_) client_->Stop();
        started_ = false;
    }

    bool service(BitstreamEngine& engine, HRESULT& failure) override {
        BYTE* p = nullptr;
        HRESULT hr = render_->GetBuffer(info_.bufferFrames, &p);
        if (FAILED(hr)) { failure = hr; return false; }
        const int64_t now = nowNs();
        int64_t queued = -1;
        if (info_.clockAvailable) {
            UINT64 pos = 0, qpc = 0;
            if (SUCCEEDED(clock_->GetPosition(&pos, &qpc)) && qpc != 0) {
                const double playedAtQpc = static_cast<double>(pos) / clockFreq_ * rate_;
                const double sinceQpc = static_cast<double>(now - static_cast<int64_t>(qpc) * 100) * rate_ / 1e9;
                const double q = static_cast<double>(written_) - (playedAtQpc + sinceQpc);
                // Only trust plausible values (the clock of some drivers idles at 0).
                if (pos != 0 && q > -static_cast<double>(rate_) && q < static_cast<double>(rate_))
                    queued = static_cast<int64_t>(q < 0 ? 0 : q);
            }
        }
        engine.render(p, info_.bufferFrames, now, queued);
        hr = render_->ReleaseBuffer(info_.bufferFrames, 0);
        if (FAILED(hr)) { failure = hr; return false; }
        written_ += info_.bufferFrames;
        return true;
    }

private:
    bool activate(std::string& error) {
        client_.Reset();
        const HRESULT hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(hr) || !client_) { error = "Activate(IAudioClient): " + hrToString(hr); return false; }
        return true;
    }

    OutputInfo info_;
    ComPtr<IMMDevice> device_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioRenderClient> render_;
    ComPtr<IAudioClock> clock_;
    UniqueHandle event_;
    double clockFreq_ = 0;
    uint32_t rate_ = 48000;
    uint32_t bytesPerFrame_ = 4;
    uint64_t written_ = 0;
    bool started_ = false;
};

// Hardware-free sink paced by a high-resolution timer. Burst data is rendered
// into a scratch buffer and discarded; device-side buffering is reported as
// unknown.
class NullBitstreamSink final : public IBitstreamSink {
public:
    bool open(const OutputParams& params, std::string& error) override {
        rate_ = params.transport.transportSampleRate;
        info_.deviceName = "null sink";
        info_.formatLabel = "none (timer-paced null sink)";
        info_.nullSink = true;
        info_.periodHns = params.periodHns > 0 ? params.periodHns : 30'000;
        info_.minPeriodHns = info_.defaultPeriodHns = info_.periodHns;
        info_.bufferFrames = static_cast<uint32_t>(info_.periodHns * rate_ / 10'000'000LL);
        buffer_.assign(static_cast<size_t>(info_.bufferFrames) * params.transport.transportChannels * 2, 0);
        timer_.reset(CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /*HIGH_RESOLUTION*/, TIMER_ALL_ACCESS));
        if (!timer_) timer_.reset(CreateWaitableTimerW(nullptr, FALSE, nullptr));
        if (!timer_) { error = "cannot create timer"; return false; }
        return true;
    }
    const OutputInfo& info() const override { return info_; }
    HANDLE event() const override { return timer_.get(); }
    bool start(std::string&) override {
        startNs_ = nowNs();
        periods_ = 0;
        arm();
        return true;
    }
    void stop() override { if (timer_) CancelWaitableTimer(timer_.get()); }
    bool service(BitstreamEngine& engine, HRESULT&) override {
        const int64_t now = nowNs();
        const uint64_t due = static_cast<uint64_t>((now - startNs_) * rate_ / 1'000'000'000LL) / info_.bufferFrames;
        while (periods_ < due) {
            engine.render(buffer_.data(), info_.bufferFrames, now, -1);
            ++periods_;
        }
        arm();
        return true;
    }

private:
    void arm() {
        const int64_t nextNs = startNs_ + static_cast<int64_t>((periods_ + 1) * info_.bufferFrames * 1'000'000'000ULL / rate_);
        LARGE_INTEGER due;
        due.QuadPart = -std::max<int64_t>(1, (nextNs - nowNs()) / 100);
        SetWaitableTimer(timer_.get(), &due, 0, nullptr, nullptr, FALSE);
    }
    OutputInfo info_;
    UniqueHandle timer_;
    std::vector<uint8_t> buffer_;
    uint32_t rate_ = 48000;
    int64_t startNs_ = 0;
    uint64_t periods_ = 0;
};

}  // namespace

std::unique_ptr<IBitstreamSink> createBitstreamSink(bool nullSink) {
    if (nullSink) return std::make_unique<NullBitstreamSink>();
    return std::make_unique<WasapiBitstreamOutput>();
}

}  // namespace bs
