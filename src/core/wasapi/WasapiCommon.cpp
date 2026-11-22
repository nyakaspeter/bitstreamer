#include "WasapiCommon.h"

#include <avrt.h>
#include <propsys.h>

#include <algorithm>
#include <cstdio>
#include <cwctype>

namespace bs {

namespace {

// Property keys and GUIDs are defined locally to avoid INITGUID ordering issues.
constexpr PROPERTYKEY kPkeyDeviceFriendlyName{{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
constexpr PROPERTYKEY kPkeyInterfaceFriendlyName{{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};

constexpr GUID kSubtypePcm{0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
constexpr GUID kSubtypeFloat{0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

#pragma pack(push, 1)
struct WaveFormatIec61937 {
    WAVEFORMATEXTENSIBLE ext;
    DWORD encodedSamplesPerSec;
    DWORD encodedChannelCount;
    DWORD averageBytesPerSec;
};
#pragma pack(pop)

bool guidEq(const GUID& a, const GUID& b) { return std::memcmp(&a, &b, sizeof(GUID)) == 0; }

GUID toGuid(const Guid128& g) {
    GUID out;
    out.Data1 = g.d1; out.Data2 = g.d2; out.Data3 = g.d3;
    std::memcpy(out.Data4, g.d4, 8);
    return out;
}

std::string propString(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::string out;
    if (SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) out = narrow(v.pwszVal);
    PropVariantClear(&v);
    return out;
}

ComPtr<IMMDeviceEnumerator> createEnumerator() {
    ComPtr<IMMDeviceEnumerator> e;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&e));
    return e;
}

}  // namespace

std::string hrToString(HRESULT hr) {
    struct Known { HRESULT hr; const char* name; };
    static const Known known[] = {
        {AUDCLNT_E_UNSUPPORTED_FORMAT, "AUDCLNT_E_UNSUPPORTED_FORMAT"},
        {AUDCLNT_E_DEVICE_IN_USE, "AUDCLNT_E_DEVICE_IN_USE (another application holds the device exclusively)"},
        {AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED, "AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED (enable exclusive mode in the device's Advanced properties)"},
        {AUDCLNT_E_DEVICE_INVALIDATED, "AUDCLNT_E_DEVICE_INVALIDATED (device removed or reconfigured)"},
        {AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED, "AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED"},
        {AUDCLNT_E_BUFFER_SIZE_ERROR, "AUDCLNT_E_BUFFER_SIZE_ERROR"},
        {AUDCLNT_E_INVALID_DEVICE_PERIOD, "AUDCLNT_E_INVALID_DEVICE_PERIOD"},
        {AUDCLNT_E_ENDPOINT_CREATE_FAILED, "AUDCLNT_E_ENDPOINT_CREATE_FAILED"},
        {AUDCLNT_E_SERVICE_NOT_RUNNING, "AUDCLNT_E_SERVICE_NOT_RUNNING (Windows Audio service stopped)"},
        {AUDCLNT_E_ENGINE_PERIODICITY_LOCKED, "AUDCLNT_E_ENGINE_PERIODICITY_LOCKED"},
        {AUDCLNT_E_ENGINE_FORMAT_LOCKED, "AUDCLNT_E_ENGINE_FORMAT_LOCKED"},
        {AUDCLNT_E_RESOURCES_INVALIDATED, "AUDCLNT_E_RESOURCES_INVALIDATED"},
        {AUDCLNT_E_CPUUSAGE_EXCEEDED, "AUDCLNT_E_CPUUSAGE_EXCEEDED"},
        {E_ACCESSDENIED, "E_ACCESSDENIED"},
        {E_INVALIDARG, "E_INVALIDARG"},
        {E_NOTFOUND, "E_NOTFOUND (device not found)"},
        {E_NOINTERFACE, "E_NOINTERFACE"},
        {E_OUTOFMEMORY, "E_OUTOFMEMORY"},
    };
    for (const auto& k : known)
        if (k.hr == hr) return k.name;
    char buf[32];
    std::snprintf(buf, sizeof buf, "HRESULT 0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

MmcssScope::MmcssScope(bool enable) {
    if (!enable) return;
    DWORD index = 0;
    handle_ = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    if (handle_) AvSetMmThreadPriority(handle_, AVRT_PRIORITY_CRITICAL);
}

MmcssScope::~MmcssScope() {
    if (handle_) AvRevertMmThreadCharacteristics(handle_);
}

AudioFormat parseWaveFormat(const WAVEFORMATEX* w) {
    AudioFormat f;
    if (!w) return f;
    f.sampleRate = w->nSamplesPerSec;
    f.channels = w->nChannels;
    f.validBits = w->wBitsPerSample;
    WORD tag = w->wFormatTag;
    if (tag == WAVE_FORMAT_EXTENSIBLE && w->cbSize >= 22) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(w);
        f.channelMask = ext->dwChannelMask;
        if (ext->Samples.wValidBitsPerSample) f.validBits = ext->Samples.wValidBitsPerSample;
        if (guidEq(ext->SubFormat, kSubtypeFloat)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (guidEq(ext->SubFormat, kSubtypePcm)) tag = WAVE_FORMAT_PCM;
        else tag = 0;
    } else {
        f.channelMask = w->nChannels == 1 ? spk::FC : w->nChannels == 2 ? spk::Stereo : 0;
    }
    if (tag == WAVE_FORMAT_IEEE_FLOAT && w->wBitsPerSample == 32) f.sampleFormat = SampleFormat::Float32;
    else if (tag == WAVE_FORMAT_PCM && w->wBitsPerSample == 16) f.sampleFormat = SampleFormat::Int16;
    else if (tag == WAVE_FORMAT_PCM && w->wBitsPerSample == 24) f.sampleFormat = SampleFormat::Int24Packed;
    else if (tag == WAVE_FORMAT_PCM && w->wBitsPerSample == 32) f.sampleFormat = SampleFormat::Int32;
    return f;
}

WaveFormatBuf makePcmFormat(const AudioFormat& fmt) {
    WaveFormatBuf buf;
    auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(buf.bytes);
    ext->Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ext->Format.nChannels = fmt.channels;
    ext->Format.nSamplesPerSec = fmt.sampleRate;
    ext->Format.wBitsPerSample = static_cast<WORD>(fmt.bytesPerSample() * 8);
    ext->Format.nBlockAlign = static_cast<WORD>(fmt.bytesPerFrame());
    ext->Format.nAvgBytesPerSec = fmt.sampleRate * fmt.bytesPerFrame();
    ext->Format.cbSize = 22;
    ext->Samples.wValidBitsPerSample = fmt.validBits ? fmt.validBits : ext->Format.wBitsPerSample;
    ext->dwChannelMask = fmt.channelMask;
    ext->SubFormat = fmt.sampleFormat == SampleFormat::Float32 ? kSubtypeFloat : kSubtypePcm;
    return buf;
}

std::vector<DeviceInfo> enumerateDevices(DeviceFlow flow) {
    std::vector<DeviceInfo> out;
    auto enumerator = createEnumerator();
    if (!enumerator) return out;
    const EDataFlow df = flow == DeviceFlow::Capture ? eCapture : eRender;

    std::wstring defaultId;
    {
        ComPtr<IMMDevice> def;
        if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(df, eConsole, &def)) && def) {
            LPWSTR id = nullptr;
            if (SUCCEEDED(def->GetId(&id)) && id) { defaultId = id; CoTaskMemFree(id); }
        }
    }

    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(enumerator->EnumAudioEndpoints(df, DEVICE_STATE_ACTIVE, &coll)) || !coll) return out;
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev)) || !dev) continue;
        DeviceInfo info;
        info.flow = flow;
        LPWSTR id = nullptr;
        if (FAILED(dev->GetId(&id)) || !id) continue;
        info.id = id;
        CoTaskMemFree(id);
        info.isDefault = info.id == defaultId;

        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props)) && props) {
            info.name = propString(props.Get(), kPkeyDeviceFriendlyName);
            info.interfaceName = propString(props.Get(), kPkeyInterfaceFriendlyName);
        }

        ComPtr<IAudioClient> client;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client)) && client) {
            WAVEFORMATEX* mix = nullptr;
            if (SUCCEEDED(client->GetMixFormat(&mix)) && mix) {
                info.mixFormat = parseWaveFormat(mix);
                info.mixFormatValid = true;
                CoTaskMemFree(mix);
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

ComPtr<IMMDevice> openDevice(const std::wstring& id, std::string& error) {
    ComPtr<IMMDevice> dev;
    auto enumerator = createEnumerator();
    if (!enumerator) { error = "cannot create the audio device enumerator"; return dev; }
    const HRESULT hr = enumerator->GetDevice(id.c_str(), &dev);
    if (FAILED(hr) || !dev) { error = "cannot open audio endpoint: " + hrToString(hr); return nullptr; }
    DWORD state = 0;
    if (SUCCEEDED(dev->GetState(&state)) && state != DEVICE_STATE_ACTIVE) {
        error = "audio endpoint is not active (unplugged or disabled)";
        return nullptr;
    }
    return dev;
}

std::vector<BitstreamFormat> bitstreamFormatCandidates(const TransportDescriptor& t) {
    std::vector<BitstreamFormat> out;
    const WORD channels = t.transportChannels;
    const DWORD rate = t.transportSampleRate;
    const WORD blockAlign = static_cast<WORD>(channels * 2);
    const GUID sub = toGuid(t.waveSubFormat);
    const DWORD encodedMask = t.encodedChannels == 6 ? spk::Surround51Back : spk::Stereo;

    auto fillBase = [&](WAVEFORMATEX& f, WORD tag, WORD cb) {
        f.wFormatTag = tag;
        f.nChannels = channels;
        f.nSamplesPerSec = rate;
        f.wBitsPerSample = 16;
        f.nBlockAlign = blockAlign;
        f.nAvgBytesPerSec = rate * blockAlign;
        f.cbSize = cb;
    };
    auto addIec = [&](DWORD mask, const char* label) {
        BitstreamFormat b;
        b.label = label;
        auto* w = reinterpret_cast<WaveFormatIec61937*>(b.format.bytes);
        fillBase(w->ext.Format, WAVE_FORMAT_EXTENSIBLE, sizeof(WaveFormatIec61937) - sizeof(WAVEFORMATEX));
        w->ext.Samples.wValidBitsPerSample = 16;
        w->ext.dwChannelMask = mask;
        w->ext.SubFormat = sub;
        w->encodedSamplesPerSec = t.encodedSampleRate;
        w->encodedChannelCount = t.encodedChannels;
        w->averageBytesPerSec = 0;
        out.push_back(b);
    };
    auto addExt = [&](DWORD mask, const char* label) {
        BitstreamFormat b;
        b.label = label;
        auto* w = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(b.format.bytes);
        fillBase(w->Format, WAVE_FORMAT_EXTENSIBLE, 22);
        w->Samples.wValidBitsPerSample = 16;
        w->dwChannelMask = mask;
        w->SubFormat = sub;
        out.push_back(b);
    };

    addIec(encodedMask, "WAVEFORMATEXTENSIBLE_IEC61937");
    if (encodedMask != spk::Stereo) addIec(spk::Stereo, "WAVEFORMATEXTENSIBLE_IEC61937 (stereo mask)");
    addExt(spk::Stereo, "WAVEFORMATEXTENSIBLE + IEC 61937 subtype");
    if (encodedMask != spk::Stereo) addExt(encodedMask, "WAVEFORMATEXTENSIBLE + IEC 61937 subtype (encoded mask)");
    if (t.legacyFormatTag) {
        BitstreamFormat b;
        b.label = "legacy WAVEFORMATEX S/PDIF format tag";
        fillBase(*b.format.wfx(), t.legacyFormatTag, 0);
        out.push_back(b);
    }
    return out;
}

BitstreamSupport probeBitstreamSupport(const std::wstring& deviceId, const TransportDescriptor& t) {
    BitstreamSupport s;
    auto dev = openDevice(deviceId, s.error);
    if (!dev) return s;
    ComPtr<IAudioClient> client;
    const HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr) || !client) { s.error = "Activate: " + hrToString(hr); return s; }
    for (const auto& c : bitstreamFormatCandidates(t)) {
        if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, c.format.wfx(), nullptr) != S_OK) continue;
        s.negotiated = true;
        s.formatLabel = c.label;
        break;
    }
    return s;
}

// ------------------------------------------------------------ notifications

struct DeviceNotifier::Impl final : IMMNotificationClient {
    std::function<void()> onChange;
    ComPtr<IMMDeviceEnumerator> enumerator;
    LONG refs = 1;

    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG r = InterlockedDecrement(&refs);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { notify(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }
    void notify() { if (onChange) onChange(); }
};

DeviceNotifier::DeviceNotifier(std::function<void()> onChange) {
    impl_ = new Impl();
    impl_->onChange = std::move(onChange);
    impl_->enumerator = createEnumerator();
    if (impl_->enumerator) impl_->enumerator->RegisterEndpointNotificationCallback(impl_);
}

DeviceNotifier::~DeviceNotifier() {
    if (impl_->enumerator) impl_->enumerator->UnregisterEndpointNotificationCallback(impl_);
    impl_->onChange = nullptr;
    impl_->enumerator.Reset();
    impl_->Release();
}

}  // namespace bs
