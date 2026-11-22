#pragma once

#include <QPointer>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

#include <atomic>
#include <memory>
#include <thread>

#include "AppSettings.h"
#include "core/pipeline/Pipeline.h"

class LevelMeter;
class SettingsDialog;
class QComboBox;
class QLabel;
class QPushButton;

class MainWindow : public QWidget {
    Q_OBJECT
public:
    explicit MainWindow(bool startMinimized, QWidget* parent = nullptr);
    ~MainWindow() override;

    // Registered window message that asks a running instance to show itself.
    static unsigned showMessageId();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void buildUi();
    void buildTray();
    void refreshOutputs();
    void refreshFormats();
    void onOutputChanged();
    void onFormatChanged();
    void tick();
    void startEncoding();
    void stopEncoding();
    void toggleEncoding();
    void openSettings();
    void applySettings();
    void showWindow();
    void quitApplication();
    QString detailsText(const bs::PipelineStatus& st) const;

    AppSettings settings_;
    bs::Pipeline pipeline_;
    bool encoding_ = false;
    bool quitting_ = false;
    bool populating_ = false;
    QTimer timer_;
    QTimer deviceRefreshTimer_;
    std::unique_ptr<bs::DeviceNotifier> notifier_;
    QPointer<SettingsDialog> settingsDialog_;
    QSystemTrayIcon* tray_ = nullptr;
    QAction* trayToggle_ = nullptr;


    LevelMeter* meter_ = nullptr;
    QComboBox* output_ = nullptr;
    QComboBox* format_ = nullptr;
    QComboBox* upmix_ = nullptr;
    QPushButton* startButton_ = nullptr;
    QPushButton* settingsButton_ = nullptr;
};
