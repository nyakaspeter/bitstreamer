#include "SettingsDialog.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLabel>
#include <QLocale>
#include <QPushButton>
#include <QVBoxLayout>

using namespace bs;

namespace {
QLabel* sectionTitle(const QString& text) {
    auto* l = new QLabel(text);
    QFont f = l->font();
    f.setBold(true);
    l->setFont(f);
    l->setContentsMargins(0, 10, 0, 2);
    return l;
}
QLabel* muted(const QString& text) {
    auto* l = new QLabel(text);
    l->setWordWrap(true);
    QPalette p = l->palette();
    QColor c = p.color(QPalette::WindowText);
    c.setAlphaF(0.6f);
    p.setColor(QPalette::WindowText, c);
    l->setPalette(p);
    return l;
}
}  // namespace

SettingsDialog::SettingsDialog(const AppSettings& settings, QWidget* parent) : QDialog(parent), settings_(settings) {
    setWindowTitle(tr("Settings"));
    setMinimumWidth(520);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 16, 24, 20);
    layout->setSpacing(8);

    // ---- Formats: every per-format setting, for both formats ----
    layout->addWidget(sectionTitle(tr("Formats")));
    auto* grid = new QGridLayout;
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(8);
    grid->setColumnStretch(1, 1);
    grid->setColumnStretch(2, 1);
    grid->addWidget(new QLabel(tr("Bitrate")), 1, 0);
    grid->addWidget(new QLabel(tr("Safety margin")), 2, 0);
    addColumn(grid, 1, CodecId::Ac3, tr("Dolby Digital"), ac3_);
    addColumn(grid, 2, CodecId::Dts, tr("DTS"), dts_);
    layout->addLayout(grid);


    // ---- Application ----
    layout->addWidget(sectionTitle(tr("Application")));
    startOnLaunch_ = new QCheckBox(tr("Start encoding when bitstreamer starts"));
    startOnLaunch_->setChecked(settings.startEncodingOnLaunch);
    tray_ = new QCheckBox(tr("Keep running in the notification area when the window is closed"));
    tray_->setChecked(settings.minimizeToTray);
    startWithWindows_ = new QCheckBox(tr("Start with Windows"));
    startWithWindows_->setChecked(settings.startWithWindows);
    layout->addWidget(startWithWindows_);
    layout->addWidget(startOnLaunch_);
    switchToSteam_ = new QCheckBox(tr("Switch to Steam Streaming Speakers when encoding starts"));
    switchToSteam_->setToolTip(tr("On: starting makes the Steam device the Windows playback device and stopping switches back.\n"
                                  "Off: bitstreamer never changes the playback device; it encodes whenever you select the Steam device yourself."));
    switchToSteam_->setChecked(settings.switchToSteamSpeakers);
    layout->addWidget(switchToSteam_);
    layout->addWidget(tray_);

    // ---- Details ----
    layout->addWidget(sectionTitle(tr("Details")));
    details_ = new QLabel(tr("Not encoding."));
    details_->setTextFormat(Qt::RichText);
    details_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    details_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    details_->setMinimumHeight(215);
    layout->addWidget(details_, 1);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addSpacing(6);
    layout->addWidget(buttons);

}

void SettingsDialog::addColumn(QGridLayout* grid, int column, CodecId id, const QString& title, FormatColumn& out) {
    const CodecInfo* codec = findCodec(id);
    if (!codec || !codec->available) return;
    const FormatSettings& f = settings_.format(id);

    auto* header = muted(title);
    grid->addWidget(header, 0, column);

    out.bitrate = new QComboBox;
    for (uint32_t br : codec->bitrates)
        out.bitrate->addItem(tr("%1 kbit/s").arg(QLocale().toString(br / 1000.0, 'f', br % 1000 ? 1 : 0)), br);
    out.bitrate->setCurrentIndex(std::max(0, out.bitrate->findData(f.bitrate)));
    grid->addWidget(out.bitrate, 1, column);

    out.margin = new QDoubleSpinBox;
    out.margin->setRange(2, 30);
    out.margin->setDecimals(0);
    out.margin->setSingleStep(1);
    out.margin->setSuffix(tr(" ms"));
    out.margin->setValue(f.marginMs);
    grid->addWidget(out.margin, 2, column);
}

void SettingsDialog::readColumn(const FormatColumn& col, FormatSettings& f) const {
    if (!col.bitrate) return;  // format not available in this build
    if (col.bitrate->currentIndex() >= 0) f.bitrate = col.bitrate->currentData().toUInt();
    f.marginMs = col.margin->value();
}

AppSettings SettingsDialog::result() const {
    AppSettings s = settings_;
    readColumn(ac3_, s.ac3);
    readColumn(dts_, s.dts);
    s.startEncodingOnLaunch = startOnLaunch_->isChecked();
    s.switchToSteamSpeakers = switchToSteam_->isChecked();
    s.minimizeToTray = tray_->isChecked();
    s.startWithWindows = startWithWindows_->isChecked();
    return s;
}

void SettingsDialog::setDetails(const QString& html) {
    if (details_->text() != html) details_->setText(html);
}
