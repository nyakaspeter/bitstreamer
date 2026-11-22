// Windows Core Audio helpers: device enumeration, format descriptions,
// bitstream capability probing, MMCSS and device-change notifications.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wrl/client.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "../Common.h"
#include "../codec/Codec.h"

namespace bs {

using Microsoft::WRL::ComPtr;

std::string hrToString(HRESULT hr);

struct ComInit {
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
    HRESULT hr;
};

// Registers the calling thread with the Multimedia Class Scheduler Service
// ("Pro Audio", critical priority) for its lifetime.
class MmcssScope {
public:
    explicit MmcssScope(bool enable);
    ~MmcssScope();
    bool active() const { return handle_ != nullptr; }
private:
    HANDLE handle_ = nullptr;
};

struct HandleCloser { void operator()(HANDLE h) const { if (h) CloseHandle(h); } };
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

// Storage for any WAVEFORMATEX variant used by this application.
struct WaveFormatBuf {
    alignas(8) uint8_t bytes[64] = {};
    WAVEFORMATEX* wfx() { return reinterpret_cast<WAVEFORMATEX*>(bytes); }
    const WAVEFORMATEX* wfx() const { return reinterpret_cast<const WAVEFORMATEX*>(bytes); }
};

AudioFormat parseWaveFormat(const WAVEFORMATEX* wfx);
WaveFormatBuf makePcmFormat(const AudioFormat& fmt);  // WAVEFORMATEXTENSIBLE

enum class DeviceFlow { Capture, Render };

struct DeviceInfo {
    std::wstring id;
    std::string name;           // endpoint friendly name
    std::string interfaceName;  // adapter / driver name
    DeviceFlow flow = DeviceFlow::Render;
    bool isDefault = false;
    bool mixFormatValid = false;
    AudioFormat mixFormat;      // shared-mode engine format
};

std::vector<DeviceInfo> enumerateDevices(DeviceFlow flow);
ComPtr<IMMDevice> openDevice(const std::wstring& id, std::string& error);

// Ordered list of wave formats that may carry the given transport on an
// exclusive-mode stream, most specific first.
struct BitstreamFormat {
    std::string label;
    WaveFormatBuf format;
};
std::vector<BitstreamFormat> bitstreamFormatCandidates(const TransportDescriptor& t);

struct BitstreamSupport {
    bool negotiated = false;   // driver accepted a compressed (IEC 61937) format
    std::string formatLabel;   // label of the first accepted compressed format
    std::string error;         // set when the device could not be queried
};
// Non-intrusive: uses IAudioClient::IsFormatSupported only, never opens a stream.
BitstreamSupport probeBitstreamSupport(const std::wstring& deviceId, const TransportDescriptor& t);

// Invokes the callback (on a COM worker thread) whenever endpoints are added,
// removed, change state or the default device changes.
class DeviceNotifier {
public:
    explicit DeviceNotifier(std::function<void()> onChange);
    ~DeviceNotifier();
    DeviceNotifier(const DeviceNotifier&) = delete;
    DeviceNotifier& operator=(const DeviceNotifier&) = delete;
private:
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace bs
