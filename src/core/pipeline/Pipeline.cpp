#include "Pipeline.h"

#include <chrono>

namespace bs {

LatencyBreakdown computeLatency(const PipelineStatus& st) {
    LatencyBreakdown b;
    if (!st.sessionValid) return b;
    const EngineStats& s = st.stats;
    const double rate = st.encoder.sampleRate ? static_cast<double>(st.encoder.sampleRate) : 48000.0;
    b.frameDurationMs = st.encoder.frameSamples / rate * 1000.0;
    b.submitMs = s.submitLatencyUs.mean / 1000.0;
    b.outputDelayMeasured = s.outputDelayKnown;
    b.outputDelayMs = s.outputDelayKnown ? s.outputDelayUs.mean / 1000.0 : st.output.bufferFrames / rate * 1000.0;
    b.algorithmicDelayMs = st.encoder.algorithmicDelaySamples / rate * 1000.0;
    b.encodeMs = s.encodeUs.mean / 1000.0;
    b.totalMs = b.submitMs + b.outputDelayMs + b.algorithmicDelayMs;
    b.valid = s.submitLatencyUs.count > 0;
    return b;
}

namespace {
// Maps the technical error text onto the few situations a user can act on.
PipelineProblem classifyProblem(const std::string& text) {
    const auto has = [&text](const char* s) { return text.find(s) != std::string::npos; };
    if (has("DEVICE_IN_USE")) return PipelineProblem::OutputBusy;
    if (has("not installed")) return PipelineProblem::InputMissing;
    if (has("encoder")) return PipelineProblem::Encoder;
    if (has("input") || has("Steam Streaming Speakers")) return PipelineProblem::InputUnavailable;
    return PipelineProblem::OutputUnavailable;
}
}  // namespace

struct Pipeline::Session final : ICaptureSink {
    BitstreamEngine engine;
    std::unique_ptr<ICaptureSource> capture;
    std::unique_ptr<IBitstreamSink> sink;
    HANDLE stopEvent = nullptr;
    UniqueHandle endEvent;  // manual reset: a thread failed
    std::atomic<bool> failed{false};
    std::string failureText;  // written once by the failing thread, read after join
    bool standby = false;     // ended because the default playback device moved away
    Upmixer upmixer;
    const std::atomic<int>* upmixMode = nullptr;

    void onCapture(float* p, uint32_t frames, int64_t tFirstNs, int64_t tNowNs, bool disc) override {
        // The upmix happens before the encoder, so the level meters (taken
        // from the encoder's input) show what is actually sent out.
        upmixer.setMode(static_cast<UpmixMode>(upmixMode->load(std::memory_order_relaxed)));
        upmixer.process(p, frames);
        engine.pushCapture(p, frames, tFirstNs, tNowNs, disc);
    }

    void fail(const std::string& text) {
        if (!failed.exchange(true)) failureText = text;
        SetEvent(endEvent.get());
    }

    // One real-time thread per device: capture (which also encodes) and output.
    void audioThread(bool isOutput) {
        ComInit com;
        MmcssScope mmcss(true);
        HANDLE handles[3] = {stopEvent, endEvent.get(), isOutput ? sink->event() : capture->event()};
        for (;;) {
            const DWORD r = WaitForMultipleObjects(3, handles, FALSE, 2000);
            if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1) break;
            if (r == WAIT_TIMEOUT) {
                fail(std::string(isOutput ? "output" : "input") + " device stopped responding");
                break;
            }
            HRESULT hr = S_OK;
            if (isOutput) {
                if (!sink->service(engine, hr)) { fail("output device failure: " + hrToString(hr)); break; }
            } else {
                if (!capture->read(*this, hr)) { fail("input device failure: " + hrToString(hr)); break; }
            }
        }
    }
};

Pipeline::Pipeline() {
    stopEvent_.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    changeEvent_.reset(CreateEventW(nullptr, FALSE, FALSE, nullptr));
}

Pipeline::~Pipeline() { stop(); }

void Pipeline::start(const PipelineConfig& config) {
    stop();
    cfg_ = config;
    stopRequested_.store(false);
    ResetEvent(stopEvent_.get());
    {
        std::lock_guard lock(mutex_);
        state_ = PipelineState::Starting;
        message_.clear();
        restarts_ = 0;
    }
    controlThread_ = std::thread([this] { controlLoop(); });
}

void Pipeline::stop() {
    if (!controlThread_.joinable()) return;
    stopRequested_.store(true);
    SetEvent(stopEvent_.get());
    controlThread_.join();
    std::lock_guard lock(mutex_);
    if (state_ != PipelineState::Error) state_ = PipelineState::Stopped;
}

bool Pipeline::waitUntilRunning(int timeoutMs, std::string& error) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        {
            std::lock_guard lock(mutex_);
            if (state_ == PipelineState::Running) return true;
            if (state_ != PipelineState::Starting) {
                error = message_.empty() ? "pipeline is not running" : message_;
                return false;
            }
        }
        if (std::chrono::steady_clock::now() > deadline) { error = "timed out waiting for the pipeline to start"; return false; }
        Sleep(10);
    }
}

void Pipeline::setState(PipelineState st, const std::string& msg) {
    std::lock_guard lock(mutex_);
    state_ = st;
    message_ = msg;
}

PipelineStatus Pipeline::status() {
    PipelineStatus st;
    std::lock_guard lock(mutex_);
    st.state = state_;
    st.message = message_;
    if (state_ == PipelineState::Recovering || state_ == PipelineState::Error) st.problem = classifyProblem(message_);
    st.restarts = restarts_;
    if (session_) {
        st.sessionValid = true;
        st.capture = session_->capture->info();
        st.output = session_->sink->info();
        st.encoder = session_->engine.encoderCaps();
        st.stats = session_->engine.stats();
    }
    return st;
}

bool Pipeline::openSession(Session& s, std::string& error) {
    const CodecInfo* codec = findCodec(cfg_.codec);
    if (!codec || !codec->available) { error = "the selected format is not available in this build"; return false; }

    // ---- input: the Steam Streaming Speakers device, captured through loopback ----
    CaptureParams cp;
    cp.backend = cfg_.captureBackend;
    cp.periodHns = cfg_.capturePeriodHns;
    if (cfg_.captureBackend == CaptureBackend::Loopback) {
        const DeviceInfo steam = findSteamSpeakers();
        if (steam.id.empty()) { error = "the Steam Streaming Speakers device is not installed"; return false; }
        cp.deviceId = steam.id;
        // Every time encoding starts: make sure the device is 5.1 at 48 kHz.
        const bool is51 = steam.mixFormatValid && steam.mixFormat.channels == 6 && steam.mixFormat.sampleRate == 48000;
        if (!is51) {
            if (!configureSteamSpeakers51(cp.deviceId, error)) return false;
            Sleep(400);  // the endpoint restarts with its new format
        }
    }
    s.capture = createCaptureSource(cp.backend);
    if (!s.capture->open(cp, error)) { error = "input: " + error; return false; }

    // ---- encoder ----
    EngineConfig ec;
    ec.codec = cfg_.codec;
    ec.bitrate = cfg_.bitrate;
    ec.sampleRate = s.capture->info().format.sampleRate;
    ec.marginFrames = static_cast<uint32_t>(static_cast<uint64_t>(cfg_.marginUs) * ec.sampleRate / 1'000'000);
    if (!s.engine.initialize(ec, error)) { error = "encoder: " + error; return false; }

    // ---- output ----
    if (!cfg_.nullSink && cfg_.outputDeviceId.empty()) { error = "no output device selected"; return false; }
    s.sink = createBitstreamSink(cfg_.nullSink);
    OutputParams op;
    op.deviceId = cfg_.outputDeviceId;
    op.transport = s.engine.transport();
    op.periodHns = cfg_.outputPeriodHns;
    op.nullSink = cfg_.nullSink;
    if (!s.sink->open(op, error)) { error = "output: " + error; return false; }

    s.engine.setPeriodHints(s.capture->info().periodFrames, s.sink->info().bufferFrames);
    s.engine.resetStreams();
    s.upmixMode = &upmix_;
    s.stopEvent = stopEvent_.get();
    s.endEvent.reset(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    return true;
}

void Pipeline::runSession(Session& s) {
    std::string error;
    if (!s.sink->start(error) || !s.capture->start(error)) {
        s.fail(error);
    } else {
        std::thread out([&s] { s.audioThread(true); });
        std::thread cap([&s] { s.audioThread(false); });
        // While the audio threads run, watch the Windows default playback
        // device. The output is held in exclusive mode, so the moment the user
        // selects another device the session ends and the output is released;
        // otherwise ordinary playback to that output would fail.
        const bool follow = cfg_.captureBackend == CaptureBackend::Loopback;
        const std::wstring inputId = s.capture->info().deviceId;
        HANDLE handles[3] = {stopEvent_.get(), s.endEvent.get(), changeEvent_.get()};
        for (;;) {
            const DWORD r = WaitForMultipleObjects(3, handles, FALSE, 500);
            if (r == WAIT_OBJECT_0 || r == WAIT_OBJECT_0 + 1) break;
            if (follow && defaultPlaybackDeviceId() != inputId) {
                s.standby = true;
                SetEvent(s.endEvent.get());
                break;
            }
        }
        out.join();
        cap.join();
    }
    s.capture->stop();
    s.sink->stop();
}

void Pipeline::controlLoop() {
    ComInit com;
    DeviceNotifier notifier([this] { SetEvent(changeEvent_.get()); });
    const bool follow = cfg_.captureBackend == CaptureBackend::Loopback;

    // Optionally make the Steam device the default playback device for as
    // long as encoding is switched on.
    bool switchedDefault = false;
    if (follow && cfg_.switchDefaultDevice) {
        const DeviceInfo steam = findSteamSpeakers();
        const std::wstring current = defaultPlaybackDeviceId();
        std::string ignored;
        if (!steam.id.empty() && current != steam.id && setDefaultPlaybackDevice(steam.id, ignored)) {
            previousDefaultId_ = current;
            switchedDefault = true;
        }
    }

    while (!stopRequested_.load()) {
        // Encode only while the Steam device is the Windows default playback
        // device. Otherwise stand by with every device released, so the user
        // can play to any other output (including ours) as if bitstreamer
        // were not running.
        if (follow) {
            const DeviceInfo steam = findSteamSpeakers();
            if (!steam.id.empty() && defaultPlaybackDeviceId() != steam.id) {
                setState(PipelineState::Standby, "");
                HANDLE handles[2] = {stopEvent_.get(), changeEvent_.get()};
                WaitForMultipleObjects(2, handles, FALSE, 1000);
                continue;
            }
        }

        auto session = std::make_unique<Session>();
        std::string error;
        bool standby = false;
        if (openSession(*session, error)) {
            {
                std::lock_guard lock(mutex_);
                session_ = session.get();
                state_ = PipelineState::Running;
                message_.clear();
            }
            runSession(*session);
            {
                std::lock_guard lock(mutex_);
                session_ = nullptr;
            }
            if (stopRequested_.load()) break;
            standby = session->standby && !session->failed.load();
            error = session->failureText.empty() ? "session ended unexpectedly" : session->failureText;
        }
        session.reset();
        if (standby) continue;  // not a failure: the loop goes to standby above
        if (!cfg_.autoReconnect) {
            setState(PipelineState::Error, error);
            break;
        }
        {
            std::lock_guard lock(mutex_);
            state_ = PipelineState::Recovering;
            message_ = error;
            ++restarts_;
        }
        // Retry promptly when the device topology changes, otherwise poll.
        HANDLE handles[2] = {stopEvent_.get(), changeEvent_.get()};
        if (WaitForMultipleObjects(2, handles, FALSE, 2000) == WAIT_OBJECT_0 + 1) Sleep(300);  // let the endpoint settle
    }

    // Encoding was switched off: give the default playback device back, but
    // only if we changed it and the user has not picked another one since.
    if (switchedDefault) {
        std::string ignored;
        const DeviceInfo steam = findSteamSpeakers();
        if (steam.id.empty() || defaultPlaybackDeviceId() == steam.id) setDefaultPlaybackDevice(previousDefaultId_, ignored);
        previousDefaultId_.clear();
    }
    std::lock_guard lock(mutex_);
    if (state_ != PipelineState::Error) { state_ = PipelineState::Stopped; message_.clear(); }
}

}  // namespace bs
