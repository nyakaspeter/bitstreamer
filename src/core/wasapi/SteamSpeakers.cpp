#include "SteamSpeakers.h"

#include <shellapi.h>

namespace bs {

namespace {

// IPolicyConfig: the interface the Windows Sound control panel itself uses to
// change an endpoint's format and the default device. It is not part of the
// public SDK; the layout below is the long-stable Windows 7+ version that
// other open-source audio tools (for example Sunshine) rely on as well.
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
constexpr GUID kClsidPolicyConfigClient{0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

ComPtr<IPolicyConfig> policyConfig(std::string& error) {
    ComPtr<IPolicyConfig> policy;
    const HRESULT hr = CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig),
                                        reinterpret_cast<void**>(policy.GetAddressOf()));
    if (FAILED(hr) || !policy) error = "the Windows audio policy interface is unavailable: " + hrToString(hr);
    return policy;
}

}  // namespace

DeviceInfo findSteamSpeakers() {
    for (const DeviceInfo& d : enumerateDevices(DeviceFlow::Render)) {
        if (d.interfaceName == "Steam Streaming Speakers" || d.name.find("Steam Streaming Speakers") != std::string::npos)
            return d;
    }
    return {};
}

std::wstring steamSpeakersDriverInf() {
    wchar_t path[MAX_PATH] = {};
    const DWORD n = ExpandEnvironmentStringsW(
        L"%CommonProgramFiles(x86)%\\Steam\\drivers\\Windows10\\x64\\SteamStreamingSpeakers.inf", path, MAX_PATH);
    if (n == 0 || n > MAX_PATH) return {};
    return GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES ? std::wstring() : std::wstring(path);
}

unsigned long installSteamSpeakersDriver() {
    const std::wstring inf = steamSpeakersDriverInf();
    if (inf.empty()) return ERROR_FILE_NOT_FOUND;
    const HMODULE newdev = LoadLibraryExW(L"newdev.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!newdev) return GetLastError();
    using DiInstallDriverFn = BOOL(WINAPI*)(HWND, LPCWSTR, DWORD, PBOOL);
    const auto install = reinterpret_cast<DiInstallDriverFn>(GetProcAddress(newdev, "DiInstallDriverW"));
    unsigned long result = ERROR_PROC_NOT_FOUND;
    if (install) result = install(nullptr, inf.c_str(), 0, nullptr) ? 0 : GetLastError();
    FreeLibrary(newdev);
    return result;
}

SteamInstallResult installSteamSpeakersElevated(const std::wstring& installerExe, const std::wstring& installerArgs,
                                                std::string& error) {
    if (steamSpeakersDriverInf().empty()) {
        error = "Steam's audio drivers were not found on this computer";
        return SteamInstallResult::DriverFilesMissing;
    }
    // Windows usually makes a newly installed playback device the default.
    const std::wstring previousDefault = defaultPlaybackDeviceId();

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof sei;
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = installerExe.c_str();
    sei.lpParameters = installerArgs.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei) || !sei.hProcess) {
        const DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) { error = "the administrator prompt was declined"; return SteamInstallResult::Cancelled; }
        error = "could not start the installer (error " + std::to_string(e) + ")";
        return SteamInstallResult::Failed;
    }
    WaitForSingleObject(sei.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    if (code != 0) {
        error = "driver installation failed (error " + std::to_string(code) + ")";
        return SteamInstallResult::Failed;
    }
    // The endpoint appears a few seconds after the driver is installed.
    for (int i = 0; i < 40; ++i) {
        if (!findSteamSpeakers().id.empty()) {
            std::string ignored;
            if (!previousDefault.empty()) setDefaultPlaybackDevice(previousDefault, ignored);
            return SteamInstallResult::Installed;
        }
        Sleep(500);
    }
    error = "the driver was installed but the Steam Streaming Speakers device did not appear";
    return SteamInstallResult::Failed;
}

bool configureSteamSpeakers51(const std::wstring& deviceId, std::string& error) {
    auto policy = policyConfig(error);
    if (!policy) return false;
    HRESULT last = E_FAIL;
    // Highest bit depth first; the driver accepts at least one of these.
    for (const AudioFormat& fmt : {AudioFormat{48000, 6, spk::Surround51Back, SampleFormat::Int32, 24},
                                   AudioFormat{48000, 6, spk::Surround51Back, SampleFormat::Int24Packed, 24},
                                   AudioFormat{48000, 6, spk::Surround51Back, SampleFormat::Int16, 16},
                                   AudioFormat{48000, 6, spk::Surround51Side, SampleFormat::Int16, 16}}) {
        WaveFormatBuf wf = makePcmFormat(fmt);
        last = policy->SetDeviceFormat(deviceId.c_str(), wf.wfx(), nullptr);
        if (SUCCEEDED(last)) return true;
    }
    error = "could not set Steam Streaming Speakers to 5.1 / 48000 Hz: " + hrToString(last);
    return false;
}

std::wstring defaultPlaybackDeviceId() {
    // Asked for often (the pipeline follows the default device), so this does
    // not enumerate endpoints.
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) || !enumerator)
        return {};
    ComPtr<IMMDevice> device;
    if (FAILED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)) || !device) return {};
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return {};
    std::wstring result = id;
    CoTaskMemFree(id);
    return result;
}

bool setDefaultPlaybackDevice(const std::wstring& deviceId, std::string& error) {
    auto policy = policyConfig(error);
    if (!policy) return false;
    HRESULT hr = S_OK;
    for (ERole role : {eConsole, eMultimedia, eCommunications}) {
        const HRESULT r = policy->SetDefaultEndpoint(deviceId.c_str(), role);
        if (FAILED(r)) hr = r;
    }
    if (FAILED(hr)) { error = "could not change the default playback device: " + hrToString(hr); return false; }
    return true;
}

}  // namespace bs
