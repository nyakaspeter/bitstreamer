// The input device: Valve's "Steam Streaming Speakers" virtual playback
// endpoint. It is a silent, Microsoft-signed virtual sound device whose driver
// package ships with Steam. bitstreamer does not redistribute it; it installs
// the copy Steam placed on the user's disk, sets the endpoint to 5.1 and makes
// it the default playback device while encoding, so games send discrete 5.1
// PCM to it, which is then captured through WASAPI loopback.
#pragma once

#include "WasapiCommon.h"

namespace bs {

// The active Steam Streaming Speakers playback endpoint; empty id if absent.
DeviceInfo findSteamSpeakers();

// Full path of Steam's driver INF, or empty if Steam's drivers are not on disk.
std::wstring steamSpeakersDriverInf();

// Installs the driver with DiInstallDriverW. Must run elevated. Returns 0 on
// success, otherwise a Win32 error code (ERROR_ACCESS_DENIED when not elevated).
unsigned long installSteamSpeakersDriver();

enum class SteamInstallResult { Installed, DriverFilesMissing, Cancelled, Failed };

// Relaunches `installerExe installerArgs` elevated (UAC prompt), waits for it
// and then for the endpoint to appear. The elevated process is expected to
// call installSteamSpeakersDriver() and return its result as exit code.
// The default playback device is preserved across the installation.
SteamInstallResult installSteamSpeakersElevated(const std::wstring& installerExe, const std::wstring& installerArgs,
                                                std::string& error);

// Sets the endpoint's shared format to 48 kHz 5.1. No elevation required.
bool configureSteamSpeakers51(const std::wstring& deviceId, std::string& error);

// Default playback device handling (console, multimedia and communications roles).
std::wstring defaultPlaybackDeviceId();
bool setDefaultPlaybackDevice(const std::wstring& deviceId, std::string& error);

}  // namespace bs
