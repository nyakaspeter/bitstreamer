#include "AppSettings.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QSettings>

#include <algorithm>
#include <cmath>

using namespace bs;

namespace {
QString tuningGroup(const QString& deviceId, CodecId codec) {
    const QByteArray hash = QCryptographicHash::hash(deviceId.toUtf8(), QCryptographicHash::Sha1).toHex().left(16);
    return QStringLiteral("formats/%1_%2").arg(QString::fromLatin1(hash), QString::fromLatin1(codecShortName(codec)));
}
}  // namespace

FormatSettings AppSettings::defaults(CodecId id) {
    FormatSettings f;
    if (const CodecInfo* c = findCodec(id)) f.bitrate = c->defaultBitrate;
    // Slack the encoder keeps before the output needs the next frame. 4 ms is
    // deliberately generous: late frames were seen at 1 ms and once at 2 ms.
    f.marginMs = 4.0;
    return f;
}

void AppSettings::load() {
    QSettings s;
    outputDeviceId = s.value("outputDeviceId").toString();
    CodecId c;
    if (parseCodec(s.value("codec", "ac3").toString().toStdString(), c)) codec = c;
    upmix = s.value("upmix", 0).toInt();
    startEncodingOnLaunch = s.value("startEncodingOnLaunch", false).toBool();
    switchToSteamSpeakers = s.value("switchToSteamSpeakers", true).toBool();
    minimizeToTray = s.value("minimizeToTray", true).toBool();
    startWithWindows = s.value("startWithWindows", false).toBool();
    loadTuning();
}

void AppSettings::save() const {
    QSettings s;
    s.setValue("outputDeviceId", outputDeviceId);
    s.setValue("codec", QString::fromLatin1(codecShortName(codec)));
    s.setValue("bitrateAc3", ac3.bitrate);
    s.setValue("bitrateDts", dts.bitrate);
    s.setValue("upmix", upmix);
    s.setValue("startEncodingOnLaunch", startEncodingOnLaunch);
    s.setValue("switchToSteamSpeakers", switchToSteamSpeakers);
    s.setValue("minimizeToTray", minimizeToTray);
    s.setValue("startWithWindows", startWithWindows);
}

void AppSettings::loadTuning() {
    QSettings s;
    for (CodecId id : {CodecId::Ac3, CodecId::Dts}) {
        const FormatSettings def = defaults(id);
        FormatSettings& f = format(id);
        f.bitrate = s.value(id == CodecId::Dts ? "bitrateDts" : "bitrateAc3", def.bitrate).toUInt();
        s.beginGroup(tuningGroup(outputDeviceId, id));
        // Whole milliseconds, never below the 2 ms that plays without dropouts
        // (older versions stored smaller and fractional values).
        f.marginMs = std::max(2.0, std::round(s.value("marginMs", def.marginMs).toDouble()));
        s.endGroup();
    }
}

void AppSettings::saveTuning() const {
    QSettings s;
    for (CodecId id : {CodecId::Ac3, CodecId::Dts}) {
        const FormatSettings& f = format(id);
        s.beginGroup(tuningGroup(outputDeviceId, id));
        s.setValue("marginMs", f.marginMs);
        s.endGroup();
    }
}

PipelineConfig AppSettings::toConfig(CodecId id) const {
    const FormatSettings& f = format(id);
    PipelineConfig c;
    c.outputDeviceId = outputDeviceId.toStdWString();
    c.codec = id;
    c.bitrate = f.bitrate;
    c.marginUs = static_cast<uint32_t>(f.marginMs * 1000.0);
    c.autoReconnect = true;
    c.switchDefaultDevice = switchToSteamSpeakers;
    return c;
}

void AppSettings::setStartWithWindows(bool enabled) {
    QSettings run("HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run", QSettings::NativeFormat);
    if (enabled) {
        const QString exe = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
        run.setValue("bitstreamer", QStringLiteral("\"%1\" --minimized").arg(exe));
    } else {
        run.remove("bitstreamer");
    }
}
