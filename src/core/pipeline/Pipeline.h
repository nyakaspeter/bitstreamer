// Complete capture -> encode -> bitstream-output pipeline with supervision:
// device negotiation, two real-time threads, failure detection and automatic
// recovery (hot-plug, format change, sleep/resume, audio service restart).
#pragma once

#include <atomic>
#include <mutex>
#include <thread>

#include "../dsp/Upmixer.h"
#include "../engine/BitstreamEngine.h"
#include "../wasapi/Capture.h"
#include "../wasapi/Output.h"
#include "../wasapi/SteamSpeakers.h"

namespace bs {

struct PipelineConfig {
    std::wstring outputDeviceId;   // required unless nullSink
    CodecId codec = CodecId::Ac3;
    uint32_t bitrate = 0;          // 0 = codec default
    uint32_t marginUs = 4000;      // slack kept before the output needs the next frame
    bool autoReconnect = true;
    // Make the Steam Streaming Speakers device the Windows default playback
    // device when encoding starts, and switch back when it stops.
    bool switchDefaultDevice = true;

    // Used by the automated tests to run without audio hardware.
    CaptureBackend captureBackend = CaptureBackend::Loopback;
    int64_t capturePeriodHns = 0;  // synthetic source only
    int64_t outputPeriodHns = 0;   // 0 = smallest the device accepts
    bool nullSink = false;
};

// Standby: armed, but the Steam Streaming Speakers device is not the Windows
// default playback device, so nothing is captured and the output is released.
enum class PipelineState { Stopped, Starting, Running, Standby, Recovering, Error };

// What is wrong while the pipeline is recovering or has failed.
enum class PipelineProblem { None, OutputBusy, OutputUnavailable, InputMissing, InputUnavailable, Encoder };

struct PipelineStatus {
    PipelineState state = PipelineState::Stopped;
    std::string message;           // technical detail of the last error
    PipelineProblem problem = PipelineProblem::None;
    bool sessionValid = false;     // the fields below describe a live session
    CaptureInfo capture;
    OutputInfo output;
    EncoderCapabilities encoder;
    EngineStats stats;
    uint32_t restarts = 0;
};

// Latency of a running session, from timestamps on the audio itself.
struct LatencyBreakdown {
    bool valid = false;
    double frameDurationMs = 0;        // codec frame length (known)
    double submitMs = 0;               // first sample captured -> frame handed to the output device (measured)
    double outputDelayMs = 0;          // buffering inside the output device
    bool outputDelayMeasured = false;  // from the driver's clock; otherwise one device period (estimate)
    double algorithmicDelayMs = 0;     // codec look-ahead
    double encodeMs = 0;               // encoder execution time per frame (measured)
    double totalMs = 0;                // submit + output delay + look-ahead; the receiver is not included
};
LatencyBreakdown computeLatency(const PipelineStatus& st);

class Pipeline {
public:
    Pipeline();
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;

    void start(const PipelineConfig& config);  // asynchronous
    void stop();                               // blocks until threads exited
    // Waits until the first session is running (true) or failed (false).
    bool waitUntilRunning(int timeoutMs, std::string& error);
    PipelineStatus status();
    void setUpmix(UpmixMode mode) { upmix_.store(static_cast<int>(mode)); }  // takes effect immediately

private:
    struct Session;
    void controlLoop();
    bool openSession(Session& s, std::string& error);
    void runSession(Session& s);
    void setState(PipelineState st, const std::string& msg);

    PipelineConfig cfg_;
    std::thread controlThread_;
    UniqueHandle stopEvent_;     // manual reset: user requested stop
    UniqueHandle changeEvent_;   // auto reset: device topology changed
    std::atomic<bool> stopRequested_{false};

    std::mutex mutex_;           // guards session_, state_, message_
    Session* session_ = nullptr;
    PipelineState state_ = PipelineState::Stopped;
    std::string message_;
    uint32_t restarts_ = 0;
    std::wstring previousDefaultId_;  // default playback device to restore when encoding stops
    std::atomic<int> upmix_{0};
};

}  // namespace bs
