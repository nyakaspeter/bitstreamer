#include "pch.h"
#include "Settings.h"

#include <algorithm>
#include <cwchar>

using namespace bs;

namespace {

const wchar_t* const kRoot = L"Software\\bitstreamer\\bitstreamer";
const wchar_t* const kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

// A registry key that reads and writes simple values. Earlier versions wrote
// numbers and switches as text, so reading accepts either form.
class Key {
public:
    Key(const std::wstring& path, bool write) {
        if (write) RegCreateKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &key_, nullptr);
        else RegOpenKeyExW(HKEY_CURRENT_USER, path.c_str(), 0, KEY_READ, &key_);
    }
    ~Key() { if (key_) RegCloseKey(key_); }
    Key(const Key&) = delete;
    Key& operator=(const Key&) = delete;

    std::wstring text(const wchar_t* name, const std::wstring& fallback) const {
        if (!key_) return fallback;
        DWORD type = 0, size = 0;
        if (RegQueryValueExW(key_, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS) return fallback;
        if (type == REG_DWORD) {
            DWORD v = 0;
            size = sizeof(v);
            if (RegQueryValueExW(key_, name, nullptr, nullptr, reinterpret_cast<BYTE*>(&v), &size) != ERROR_SUCCESS) return fallback;
            return std::to_wstring(v);
        }
        if (type != REG_SZ) return fallback;
        std::wstring s(size / sizeof(wchar_t) + 1, L'\0');
        size = static_cast<DWORD>(s.size() * sizeof(wchar_t));
        if (RegQueryValueExW(key_, name, nullptr, nullptr, reinterpret_cast<BYTE*>(s.data()), &size) != ERROR_SUCCESS) return fallback;
        s.resize(std::wcslen(s.c_str()));
        return s;
    }
    double number(const wchar_t* name, double fallback) const {
        const std::wstring s = text(name, L"");
        if (s.empty()) return fallback;
        wchar_t* end = nullptr;
        const double v = std::wcstod(s.c_str(), &end);
        return end == s.c_str() ? fallback : v;
    }
    bool flag(const wchar_t* name, bool fallback) const {
        const std::wstring s = text(name, L"");
        if (s.empty()) return fallback;
        return s == L"true" || s == L"1";
    }
    void setText(const wchar_t* name, const std::wstring& value) const {
        if (key_) RegSetValueExW(key_, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                                 static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    }
    void setNumber(const wchar_t* name, uint32_t value) const {
        const DWORD v = value;
        if (key_) RegSetValueExW(key_, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(v));
    }
    void setFlag(const wchar_t* name, bool value) const { setText(name, value ? L"true" : L"false"); }
    void remove(const wchar_t* name) const { if (key_) RegDeleteValueW(key_, name); }

private:
    HKEY key_ = nullptr;
};

std::wstring widen(const char* s) { return std::wstring(s, s + std::strlen(s)); }

// Sub-key holding what is stored per output device and format.
std::wstring tuningPath(const std::wstring& deviceId, CodecId codec) {
    uint64_t hash = 1469598103934665603ull;  // FNV-1a over the device id
    for (wchar_t c : deviceId) {
        hash ^= static_cast<uint64_t>(c);
        hash *= 1099511628211ull;
    }
    wchar_t hex[17];
    swprintf_s(hex, L"%016llx", static_cast<unsigned long long>(hash));
    return std::wstring(kRoot) + L"\\devices\\" + hex + L"_" + widen(codecShortName(codec));
}

}  // namespace

FormatSettings AppSettings::defaults(CodecId id) {
    FormatSettings f;
    if (const CodecInfo* c = findCodec(id)) f.bitrate = c->defaultBitrate;
    // Slack the encoder keeps before the output needs the next frame. 4 ms is
    // deliberately generous: late frames were seen at 1 ms and once at 2 ms.
    f.marginMs = 4;
    return f;
}

void AppSettings::load() {
    const Key k(kRoot, false);
    outputDeviceId = k.text(L"outputDeviceId", L"");
    const std::wstring name = k.text(L"codec", L"ac3");
    CodecId c;
    if (parseCodec(name == L"dts" ? "dts" : "ac3", c)) codec = c;
    upmix = std::clamp(static_cast<int>(k.number(L"upmix", 0)), 0, 2);
    startEncodingOnLaunch = k.flag(L"startEncodingOnLaunch", false);
    switchToSteamSpeakers = k.flag(L"switchToSteamSpeakers", true);
    minimizeToTray = k.flag(L"minimizeToTray", true);
    startWithWindows = k.flag(L"startWithWindows", false);
    loadTuning();
}

void AppSettings::save() const {
    const Key k(kRoot, true);
    k.setText(L"outputDeviceId", outputDeviceId);
    k.setText(L"codec", widen(codecShortName(codec)));
    k.setNumber(L"bitrateAc3", ac3.bitrate);
    k.setNumber(L"bitrateDts", dts.bitrate);
    k.setNumber(L"upmix", static_cast<uint32_t>(upmix));
    k.setFlag(L"startEncodingOnLaunch", startEncodingOnLaunch);
    k.setFlag(L"switchToSteamSpeakers", switchToSteamSpeakers);
    k.setFlag(L"minimizeToTray", minimizeToTray);
    k.setFlag(L"startWithWindows", startWithWindows);
}

void AppSettings::loadTuning() {
    const Key root(kRoot, false);
    for (CodecId id : {CodecId::Ac3, CodecId::Dts}) {
        const FormatSettings def = defaults(id);
        FormatSettings& f = format(id);
        f.bitrate = static_cast<uint32_t>(root.number(id == CodecId::Dts ? L"bitrateDts" : L"bitrateAc3", def.bitrate));
        // Only a bitrate the encoder offers is kept.
        if (const CodecInfo* info = findCodec(id))
            if (std::find(info->bitrates.begin(), info->bitrates.end(), f.bitrate) == info->bitrates.end()) f.bitrate = def.bitrate;
        const Key k(tuningPath(outputDeviceId, id), false);
        f.marginMs = std::clamp(static_cast<int>(k.number(L"marginMs", def.marginMs)), 2, 30);
    }
}

void AppSettings::saveTuning() const {
    for (CodecId id : {CodecId::Ac3, CodecId::Dts}) {
        const Key k(tuningPath(outputDeviceId, id), true);
        k.setNumber(L"marginMs", static_cast<uint32_t>(format(id).marginMs));
    }
}

PipelineConfig AppSettings::toConfig() const {
    const FormatSettings& f = format(codec);
    PipelineConfig c;
    c.outputDeviceId = outputDeviceId;
    c.codec = codec;
    c.bitrate = f.bitrate;
    c.marginUs = static_cast<uint32_t>(f.marginMs) * 1000u;
    c.autoReconnect = true;
    c.switchDefaultDevice = switchToSteamSpeakers;
    return c;
}

void AppSettings::setStartWithWindows(bool enabled) {
    const Key run(kRunKey, true);
    if (enabled) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        run.setText(L"bitstreamer", L"\"" + std::wstring(exe) + L"\" --minimized");
    } else {
        run.remove(L"bitstreamer");
    }
}
