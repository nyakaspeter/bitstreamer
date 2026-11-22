// Settings: per-format options for Dolby Digital and DTS side by side,
// application options and a detailed
// read-out of the running session.
#pragma once

#include <QDialog>

#include "AppSettings.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;

class SettingsDialog : public QDialog {
    Q_OBJECT
public:
    explicit SettingsDialog(const AppSettings& settings, QWidget* parent = nullptr);
    AppSettings result() const;                 // settings as edited
    void setDetails(const QString& html);       // refreshed live by the main window


private:
    struct FormatColumn {
        QComboBox* bitrate = nullptr;
        QDoubleSpinBox* margin = nullptr;
    };
    void addColumn(QGridLayout* grid, int column, bs::CodecId id, const QString& title, FormatColumn& out);
    void readColumn(const FormatColumn& col, FormatSettings& f) const;

    AppSettings settings_;
    FormatColumn ac3_;
    FormatColumn dts_;
    QCheckBox* startOnLaunch_;
    QCheckBox* switchToSteam_;
    QCheckBox* tray_;
    QCheckBox* startWithWindows_;
    QLabel* details_;
};
