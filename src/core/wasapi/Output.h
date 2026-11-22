// Compressed-bitstream output sinks. The WASAPI sink opens the endpoint in
// exclusive, event-driven mode so the burst data bypasses the Windows mixer.
#pragma once

#include "WasapiCommon.h"

namespace bs {

class BitstreamEngine;

struct OutputParams {
    std::wstring deviceId;
    TransportDescriptor transport;
    int64_t periodHns = 0;         // 0 = minimum the device accepts
    bool nullSink = false;         // timer-paced sink without hardware
};

struct OutputInfo {
    std::string deviceName;
    std::string formatLabel;       // negotiated wave format
    bool nullSink = false;
    uint32_t bufferFrames = 0;
    int64_t periodHns = 0;
    int64_t minPeriodHns = 0;
    int64_t defaultPeriodHns = 0;
    int64_t streamLatencyHns = 0;  // IAudioClient::GetStreamLatency (driver reported)
    bool clockAvailable = false;   // IAudioClock usable for measuring device-side buffering
    std::string notes;
};

class IBitstreamSink {
public:
    virtual ~IBitstreamSink() = default;
    virtual bool open(const OutputParams& params, std::string& error) = 0;
    virtual const OutputInfo& info() const = 0;
    virtual HANDLE event() const = 0;  // signalled when the device wants data
    virtual bool start(std::string& error) = 0;
    virtual void stop() = 0;
    // Pulls one device buffer from the engine. Real-time safe.
    virtual bool service(BitstreamEngine& engine, HRESULT& failure) = 0;
};

std::unique_ptr<IBitstreamSink> createBitstreamSink(bool nullSink);

}  // namespace bs
