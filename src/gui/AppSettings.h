// Persistent application settings (per-user registry via QSettings).
#pragma once

#include <QString>

#include "core/pipeline/Pipeline.h"

// Everything that is configured separately for each format.
struct FormatSettings {
    uint32_t bitrate = 0;
    double marginMs = 4.0;

    bool operator==(const FormatSettings&) const = default;
};

struct AppSettings {
    QString outputDeviceId;
    bs::CodecId codec = bs::CodecId::Ac3;  // format selected in the main window

    // Per-format settings: defaults or values entered in Settings.
    // Bitrate is global; the other values are stored per
    // output device.
    FormatSettings ac3;
    FormatSettings dts;

    int upmix = 0;                  // bs::UpmixMode: what to do with stereo sound

    bool startEncodingOnLaunch = false;
    // Make the Steam device the Windows default playback device while
    // encoding is switched on, and switch back afterwards. When off, the app
    // never touches the default device and simply encodes whenever the user
    // has selected the Steam device themselves.
    bool switchToSteamSpeakers = true;
    bool minimizeToTray = true;
    bool startWithWindows = false;

    FormatSettings& format(bs::CodecId id) { return id == bs::CodecId::Dts ? dts : ac3; }
    const FormatSettings& format(bs::CodecId id) const { return id == bs::CodecId::Dts ? dts : ac3; }
    static FormatSettings defaults(bs::CodecId id);

    void load();
    void save() const;
    bs::PipelineConfig toConfig(bs::CodecId id) const;
    bs::PipelineConfig toConfig() const { return toConfig(codec); }

    // Tuning of both formats for the current output device.
    void loadTuning();   // falls back to the defaults when nothing is stored
    void saveTuning() const;

    static void setStartWithWindows(bool enabled);
};
