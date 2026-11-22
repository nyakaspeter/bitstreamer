// PCM capture sources. A source delivers canonical interleaved float 5.1
// (FL FR FC LFE Ls Rs) together with capture-clock timestamps.
#pragma once

#include "WasapiCommon.h"

namespace bs {

enum class CaptureBackend {
    Loopback,   // WASAPI loopback of a playback endpoint
    Synthetic,  // timer-paced silence generator (automated tests, no hardware)
};

struct CaptureParams {
    std::wstring deviceId;
    CaptureBackend backend = CaptureBackend::Loopback;
    int64_t periodHns = 0;  // synthetic source only
};

struct CaptureInfo {
    CaptureBackend backend = CaptureBackend::Loopback;
    std::string deviceName;
    std::wstring deviceId;
    AudioFormat format;         // format delivered by the device
    uint32_t periodFrames = 0;  // delivery granularity
    uint32_t bufferFrames = 0;
};

class ICaptureSink {
public:
    virtual ~ICaptureSink() = default;
    // The buffer is scratch memory owned by the source; the sink may modify it.
    virtual void onCapture(float* interleaved, uint32_t frames, int64_t tFirstNs, int64_t tNowNs,
                           bool discontinuity) = 0;
};

class ICaptureSource {
public:
    virtual ~ICaptureSource() = default;
    virtual bool open(const CaptureParams& params, std::string& error) = 0;
    virtual const CaptureInfo& info() const = 0;
    virtual HANDLE event() const = 0;  // signalled when data is available
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    // Drains all available packets into the sink. Real-time safe.
    virtual bool read(ICaptureSink& sink, HRESULT& failure) = 0;
};

std::unique_ptr<ICaptureSource> createCaptureSource(CaptureBackend backend);

// Accepts the two 5.1 layouts Windows offers (side or back surrounds) and
// fills the identity mapping; anything else is refused with an explanation.
bool buildChannelMap(const AudioFormat& deviceFormat, int map[6], std::string& error);

// Converts `frames` frames of device PCM into canonical interleaved float.
void convertToCanonical(const uint8_t* src, const AudioFormat& fmt, const int map[6], uint32_t frames, float* dst);

}  // namespace bs
