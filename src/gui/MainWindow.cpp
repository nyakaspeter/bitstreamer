#include "MainWindow.h"

#include <QAction>
#include <QApplication>
#include <QCloseEvent>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include "LevelMeter.h"
#include "SettingsDialog.h"

using namespace bs;

namespace {

enum OutputRole { RoleId = Qt::UserRole, RoleAc3, RoleDts };

TransportDescriptor transportFor(CodecId id) {
    auto enc = createEncoder(id);
    auto pk = createPacketizer(id);
    std::string err;
    TransportDescriptor t;
    if (enc->initialize({48000, 6, spk::Surround51Side, SampleFormat::Float32, 32}, {}, err) &&
        pk->initialize(enc->capabilities(), err))
        t = pk->descriptor();
    return t;
}

QColor mutedText(const QWidget* w, float alpha = 0.6f) {
    QColor c = w->palette().color(QPalette::WindowText);
    c.setAlphaF(alpha);
    return c;
}

QLabel* fieldCaption(const QString& text, QWidget* parent) {
    auto* l = new QLabel(text, parent);
    QPalette p = l->palette();
    p.setColor(QPalette::WindowText, mutedText(parent));
    l->setPalette(p);
    return l;
}

}  // namespace

unsigned MainWindow::showMessageId() {
    static const unsigned id = RegisterWindowMessageW(L"bitstreamer.show-window");
    return id;
}

MainWindow::MainWindow(bool startMinimized, QWidget* parent) : QWidget(parent) {
    settings_.load();
    winId();  // create the native window now so a second launch can signal it even while hidden
    buildUi();
    buildTray();
    refreshOutputs();

    // Device hot-plug: the notifier calls back on a COM thread; debounce on the GUI thread.
    deviceRefreshTimer_.setSingleShot(true);
    deviceRefreshTimer_.setInterval(700);
    connect(&deviceRefreshTimer_, &QTimer::timeout, this, [this] { if (!encoding_) refreshOutputs(); });
    notifier_ = std::make_unique<DeviceNotifier>([this] {
        QMetaObject::invokeMethod(this, [this] { deviceRefreshTimer_.start(); }, Qt::QueuedConnection);
    });

    connect(&timer_, &QTimer::timeout, this, &MainWindow::tick);
    timer_.start(50);

    const bool autoStart = settings_.startEncodingOnLaunch;
    if (autoStart && !settings_.outputDeviceId.isEmpty()) startEncoding();
    if (startMinimized && tray_ && tray_->isVisible()) hide(); else show();
}

MainWindow::~MainWindow() {
    notifier_.reset();
    pipeline_.stop();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("bitstreamer"));
    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(28, 24, 28, 24);
    column->setSpacing(0);

    // Level meters on a soft card.
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("card"));
    auto* cardLayout = new QVBoxLayout(card);
    cardLayout->setContentsMargins(20, 22, 20, 12);
    meter_ = new LevelMeter;
    cardLayout->addWidget(meter_);
    column->addWidget(card, 1);
    column->addSpacing(22);

    column->addWidget(fieldCaption(tr("Output"), this));
    column->addSpacing(6);
    output_ = new QComboBox;
    output_->setMinimumHeight(36);
    output_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    column->addWidget(output_);
    column->addSpacing(16);

    column->addWidget(fieldCaption(tr("Format"), this));
    column->addSpacing(6);
    format_ = new QComboBox;
    format_->setMinimumHeight(36);
    column->addWidget(format_);
    column->addSpacing(16);

    column->addWidget(fieldCaption(tr("Upmix"), this));
    column->addSpacing(6);
    upmix_ = new QComboBox;
    upmix_->setMinimumHeight(36);
    upmix_->addItem(tr("Off"), static_cast<int>(UpmixMode::Off));
    upmix_->addItem(tr("Quad speakers"),static_cast<int>(UpmixMode::Quad));
    upmix_->addItem(tr("Virtual surround"), static_cast<int>(UpmixMode::VirtualSurround));
    upmix_->setItemData(0, tr("Stereo plays on the front left and right speakers only."), Qt::ToolTipRole);
    upmix_->setItemData(1, tr("Stereo is also played on the surround speakers."), Qt::ToolTipRole);
    upmix_->setItemData(2, tr("Stereo is spread over all speakers: speech towards the centre, ambience to the surrounds, bass to the subwoofer."), Qt::ToolTipRole);
    upmix_->setToolTip(tr("What to do with stereo sound. Real 5.1 sound is never changed."));
    upmix_->setCurrentIndex(std::max(0, upmix_->findData(settings_.upmix)));
    pipeline_.setUpmix(static_cast<UpmixMode>(settings_.upmix));
    column->addWidget(upmix_);
    column->addSpacing(26);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(10);
    startButton_ = new QPushButton(tr("Start"));
    startButton_->setObjectName(QStringLiteral("primary"));
    startButton_->setMinimumHeight(40);
    startButton_->setCursor(Qt::PointingHandCursor);
    settingsButton_ = new QPushButton(tr("Settings"));
    settingsButton_->setMinimumHeight(40);
    settingsButton_->setMinimumWidth(104);
    buttons->addWidget(startButton_, 1);
    buttons->addWidget(settingsButton_);
    column->addLayout(buttons);

    // Only two elements are styled by hand; everything else is the native
    // Windows 11 style, which follows the system light / dark theme.
    const QColor accent = palette().color(QPalette::Accent);
    const QColor text = palette().color(QPalette::WindowText);
    const bool dark = palette().color(QPalette::Window).lightness() < 128;
    setStyleSheet(QStringLiteral(
        "QFrame#card { background: rgba(%1,%2,%3,%4); border-radius: 10px; }"
        "QPushButton#primary { background: %5; color: %6; border: none; border-radius: 6px; font-weight: 600; }"
        "QPushButton#primary:hover { background: %7; }"
        "QPushButton#primary:pressed { background: %8; }"
        "QPushButton#primary:disabled { background: rgba(%1,%2,%3,40); color: rgba(%1,%2,%3,110); }")
            .arg(text.red()).arg(text.green()).arg(text.blue()).arg(dark ? 13 : 10)
            .arg(accent.name(), dark ? QStringLiteral("#101010") : QStringLiteral("#ffffff"),
                 accent.lighter(110).name(), accent.darker(112).name()));

    connect(startButton_, &QPushButton::clicked, this, &MainWindow::toggleEncoding);
    connect(settingsButton_, &QPushButton::clicked, this, &MainWindow::openSettings);
    connect(output_, &QComboBox::currentIndexChanged, this, [this] { if (!populating_) onOutputChanged(); });
    connect(format_, &QComboBox::currentIndexChanged, this, [this] { if (!populating_) onFormatChanged(); });
    // The upmix changes on the fly; encoding is not restarted.
    connect(upmix_, &QComboBox::currentIndexChanged, this, [this] {
        if (populating_ || upmix_->currentIndex() < 0) return;  // blanked while no output is available
        settings_.upmix = upmix_->currentData().toInt();
        pipeline_.setUpmix(static_cast<UpmixMode>(settings_.upmix));
        settings_.save();
    });

    setMinimumWidth(380);
    resize(400, 590);
}

void MainWindow::buildTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;
    tray_ = new QSystemTrayIcon(QApplication::windowIcon(), this);
    auto* menu = new QMenu(this);
    auto* showAction = menu->addAction(tr("Show"));
    trayToggle_ = menu->addAction(tr("Start"));
    menu->addSeparator();
    auto* quitAction = menu->addAction(tr("Quit"));
    connect(showAction, &QAction::triggered, this, &MainWindow::showWindow);
    connect(trayToggle_, &QAction::triggered, this, &MainWindow::toggleEncoding);
    connect(quitAction, &QAction::triggered, this, &MainWindow::quitApplication);
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::Trigger || r == QSystemTrayIcon::DoubleClick) showWindow();
    });
    tray_->setContextMenu(menu);
    tray_->setToolTip(QStringLiteral("bitstreamer"));
    tray_->show();
}

void MainWindow::showWindow() {
    showNormal();
    raise();
    activateWindow();
}

// Outputs: only devices whose driver accepted a Dolby Digital or DTS format.
void MainWindow::refreshOutputs() {
    populating_ = true;
    const TransportDescriptor ac3 = transportFor(CodecId::Ac3);
    const CodecInfo* dtsInfo = findCodec(CodecId::Dts);
    const bool haveDts = dtsInfo && dtsInfo->available;
    const TransportDescriptor dts = haveDts ? transportFor(CodecId::Dts) : TransportDescriptor{};
    const std::wstring inputId = findSteamSpeakers().id;

    output_->clear();
    for (const DeviceInfo& d : enumerateDevices(DeviceFlow::Render)) {
        if (d.id == inputId) continue;
        const bool okAc3 = probeBitstreamSupport(d.id, ac3).negotiated;
        const bool okDts = haveDts && probeBitstreamSupport(d.id, dts).negotiated;
        if (!okAc3 && !okDts) continue;
        output_->addItem(QString::fromStdString(d.name), QString::fromStdWString(d.id));
        const int i = output_->count() - 1;
        output_->setItemData(i, okAc3, RoleAc3);
        output_->setItemData(i, okDts, RoleDts);
    }
    int idx = output_->findData(settings_.outputDeviceId, RoleId);
    if (idx < 0 && output_->count() > 0) idx = 0;
    output_->setCurrentIndex(idx);
    // Without a usable output there is nothing to choose: all three selects
    // are disabled and left blank (the meter box says why).
    const bool hasOutput = output_->count() > 0;
    output_->setEnabled(hasOutput);
    upmix_->setCurrentIndex(hasOutput ? std::max(0, upmix_->findData(settings_.upmix)) : -1);
    upmix_->setEnabled(hasOutput);
    populating_ = false;

    settings_.outputDeviceId = idx >= 0 ? output_->itemData(idx, RoleId).toString() : QString();
    refreshFormats();
}

// Formats: the codecs the selected output accepts.
void MainWindow::refreshFormats() {
    populating_ = true;
    format_->clear();
    const int i = output_->currentIndex();
    if (i >= 0) {
        if (output_->itemData(i, RoleAc3).toBool()) format_->addItem(tr("Dolby Digital"), QStringLiteral("ac3"));
        if (output_->itemData(i, RoleDts).toBool()) format_->addItem(tr("DTS"), QStringLiteral("dts"));
    }
    int idx = format_->findData(QString::fromLatin1(codecShortName(settings_.codec)));
    if (idx < 0 && format_->count() > 0) idx = 0;
    format_->setCurrentIndex(idx);
    format_->setEnabled(format_->count() > 0);
    populating_ = false;

    if (idx >= 0) parseCodec(format_->itemData(idx).toString().toStdString(), settings_.codec);
    settings_.loadTuning();
    startButton_->setEnabled(encoding_ || idx >= 0);
}

void MainWindow::onOutputChanged() {
    settings_.outputDeviceId = output_->currentIndex() >= 0 ? output_->currentData(RoleId).toString() : QString();
    refreshFormats();
    settings_.save();
    if (encoding_) startEncoding();
}

void MainWindow::onFormatChanged() {
    if (format_->currentIndex() >= 0) parseCodec(format_->currentData().toString().toStdString(), settings_.codec);
    settings_.loadTuning();
    settings_.save();
    if (encoding_) startEncoding();
}

void MainWindow::startEncoding() {
    if (settings_.outputDeviceId.isEmpty()) return;
    pipeline_.start(settings_.toConfig());
    encoding_ = true;
    startButton_->setText(tr("Stop"));
    if (trayToggle_) trayToggle_->setText(tr("Stop"));
}

void MainWindow::stopEncoding() {
    pipeline_.stop();
    encoding_ = false;
    startButton_->setText(tr("Start"));
    if (trayToggle_) trayToggle_->setText(tr("Start"));
    meter_->clear();
    refreshOutputs();
}

void MainWindow::toggleEncoding() {
    if (encoding_) stopEncoding(); else startEncoding();
}

void MainWindow::tick() {
    const PipelineStatus st = pipeline_.status();

    // A short message in the meter box whenever nothing is being encoded.
    // The technical detail of a problem is in Settings.
    QString status;
    if (!encoding_) {
        status = startButton_->isEnabled() ? tr("Press Start to begin encoding.") : tr("No Dolby Digital or DTS capable device is connected.");
    } else {
        if (st.state == PipelineState::Standby) {
            status = tr("Waiting for Steam Streaming Speakers to be selected as the playback device in Windows.");
        } else {
            switch (st.problem) {
                case PipelineProblem::OutputBusy: status = tr("The selected output is in use by another program."); break;
                case PipelineProblem::OutputUnavailable: status = tr("The selected output is not available. Check that it is connected and switched on."); break;
                case PipelineProblem::InputMissing: status = tr("The Steam Streaming Speakers device was not found. Restart bitstreamer to install it."); break;
                case PipelineProblem::InputUnavailable: status = tr("The Steam Streaming Speakers device is not responding."); break;
                case PipelineProblem::Encoder: status = tr("The encoder could not be started."); break;
                case PipelineProblem::None: break;
            }
        }
    }
    meter_->setMessage(status);

    if (st.sessionValid) meter_->setLevels(st.stats.peaks);
    else if (encoding_) meter_->clear();

    if (tray_) {
        tray_->setToolTip(!encoding_ ? QStringLiteral("bitstreamer")
                          : st.state == PipelineState::Standby ? tr("bitstreamer: waiting") : tr("bitstreamer: encoding"));
    }
    if (settingsDialog_) settingsDialog_->setDetails(detailsText(st));
}

// Detailed read-out for the Settings dialog, as a two-column table.
QString MainWindow::detailsText(const PipelineStatus& st) const {
    if (!st.sessionValid) return encoding_ && !st.message.empty() ? QString::fromStdString(st.message).toHtmlEscaped() : tr("Not encoding.");
    const LatencyBreakdown b = computeLatency(st);
    const EngineStats& s = st.stats;
    QString rows;
    const auto row = [&rows](const QString& name, const QString& value, bool sub = false) {
        rows += QStringLiteral("<tr><td style='padding:2px 28px 2px %1px; opacity:0.6'><span style='color:gray'>%2</span></td>"
                               "<td style='padding:2px 0'>%3</td></tr>")
                    .arg(sub ? 16 : 0).arg(name.toHtmlEscaped(), value.toHtmlEscaped());
    };
    const auto ms = [](double v, int d = 1) { return QString::number(v, 'f', d) + QStringLiteral(" ms"); };
    row(tr("Input"), tr("Steam Streaming Speakers, 5.1"));
    row(tr("Output"), QString::fromStdString(st.output.deviceName));
    row(tr("Format"), tr("%1, %2 kbit/s").arg(st.encoder.codec == CodecId::Dts ? tr("DTS") : tr("Dolby Digital"))
                          .arg(st.encoder.bitrate / 1000.0, 0, 'f', st.encoder.bitrate % 1000 ? 1 : 0));
    if (b.valid) {
        row(tr("Latency"), ms(b.totalMs));
        row(tr("Codec frame"), ms(b.frameDurationMs), true);
        row(tr("Capture and queueing"), ms(b.submitMs - b.frameDurationMs), true);
        row(tr("Output device buffer"), b.outputDelayMeasured ? ms(b.outputDelayMs) : tr("%1 (estimate)").arg(ms(b.outputDelayMs)), true);
        row(tr("Codec look-ahead"), st.encoder.algorithmicDelayEstimated ? tr("%1 (estimate)").arg(ms(b.algorithmicDelayMs))
                                                                         : ms(b.algorithmicDelayMs), true);
        row(tr("Encoding time"), tr("%1 per frame").arg(ms(b.encodeMs, 2)));
    } else {
        row(tr("Latency"), tr("measuring"));
    }
    row(tr("Dropouts"), QString::number(s.underruns + s.overrunDrops + st.restarts));
    return QStringLiteral("<table cellspacing='0' cellpadding='0'>") + rows + QStringLiteral("</table>");
}

void MainWindow::openSettings() {
    if (settingsDialog_) { settingsDialog_->raise(); settingsDialog_->activateWindow(); return; }
    settingsDialog_ = new SettingsDialog(settings_, this);
    settingsDialog_->setAttribute(Qt::WA_DeleteOnClose);
    connect(settingsDialog_, &QDialog::accepted, this, &MainWindow::applySettings);
    settingsDialog_->show();
}

void MainWindow::applySettings() {
    if (!settingsDialog_) return;
    const AppSettings edited = settingsDialog_->result();
    const bool autostartChanged = edited.startWithWindows != settings_.startWithWindows;
    const bool pipelineChanged = !(edited.format(settings_.codec) == settings_.format(settings_.codec)) ||
                                 edited.switchToSteamSpeakers != settings_.switchToSteamSpeakers;
    const QString device = settings_.outputDeviceId;
    const CodecId codec = settings_.codec;
    settings_ = edited;
    settings_.outputDeviceId = device;
    settings_.codec = codec;
    settings_.save();
    settings_.saveTuning();
    if (autostartChanged) AppSettings::setStartWithWindows(settings_.startWithWindows);
    if (encoding_ && pipelineChanged) startEncoding();
}

void MainWindow::quitApplication() {
    quitting_ = true;
    close();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    if (!quitting_ && settings_.minimizeToTray && tray_ && tray_->isVisible()) {
        hide();
        event->ignore();
        return;
    }
    pipeline_.stop();
    event->accept();
    QApplication::quit();
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
    const MSG* msg = static_cast<const MSG*>(message);
    if (msg && msg->message == showMessageId()) {
        showWindow();
        if (result) *result = 0;
        return true;
    }
    return QWidget::nativeEvent(eventType, message, result);
}
