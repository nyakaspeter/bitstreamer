// The application window: level meters, output / format / upmix selection,
// Start/Stop, and a settings pane that opens beside the main column.
#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include "Settings.h"
#include "Tray.h"

class MainWindow {
public:
    explicit MainWindow(bool startMinimized);
    ~MainWindow();
    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    // Window message a second launch broadcasts to bring this one forward.
    static unsigned showMessageId();

private:
    struct Output {
        std::wstring id;
        winrt::hstring name;
        bool ac3 = false;
        bool dts = false;
    };
    struct MeterBar {
        winrt::Microsoft::UI::Xaml::FrameworkElement track{nullptr};
        winrt::Microsoft::UI::Xaml::FrameworkElement fill{nullptr};
        winrt::Microsoft::UI::Xaml::FrameworkElement hot{nullptr};
        double target = 0, shown = 0;  // 0..1 of the track height
    };

    void buildWindow();
    void buildMeters();
    void buildBackdrop();
    void connectHandlers();
    void showWindow();
    void quit();
    void sizeWindowInitially();
    void growWindow(int deltaWidth);
    bool maximized() const;

    void refreshOutputs();
    void refreshFormats();
    void onOutputChanged();
    void onFormatChanged();
    void startEncoding();
    void stopEncoding();
    void toggleEncoding();
    void updateStartButton();

    void tick();
    void updateMeters(const bs::PipelineStatus& st);
    void animateMeters();
    void setMessage(const wchar_t* text);
    void updateDetails(const bs::PipelineStatus& st);

    void openSettings(bool open);
    void populateSettings();
    void onFormatSettingChanged(bs::CodecId id);

    template <typename T>
    T find(const wchar_t* name) const { return root_.FindName(name).as<T>(); }

    static LRESULT CALLBACK subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR ref);

    AppSettings settings_;
    bs::Pipeline pipeline_;
    std::unique_ptr<bs::DeviceNotifier> notifier_;
    std::unique_ptr<Tray> tray_;

    winrt::Microsoft::UI::Xaml::Window window_{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement root_{nullptr};
    HWND hwnd_ = nullptr;
    winrt::Microsoft::UI::Composition::SystemBackdrops::DesktopAcrylicController backdrop_{nullptr};
    winrt::Microsoft::UI::Composition::SystemBackdrops::SystemBackdropConfiguration backdropConfig_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer timer_{nullptr};
    winrt::Microsoft::UI::Dispatching::DispatcherQueueTimer deviceRefreshTimer_{nullptr};

    winrt::Microsoft::UI::Xaml::Controls::ComboBox output_{nullptr}, format_{nullptr}, upmix_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Button start_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock startLabel_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::FontIcon startIcon_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Primitives::ToggleButton settingsToggle_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::TextBlock message_{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement bars_{nullptr};
    winrt::Microsoft::UI::Xaml::FrameworkElement settingsPane_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::Grid details_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ComboBox ac3Bitrate_{nullptr}, dtsBitrate_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::NumberBox ac3Margin_{nullptr}, dtsMargin_{nullptr};
    winrt::Microsoft::UI::Xaml::Controls::ToggleSwitch startWithWindows_{nullptr}, startOnLaunch_{nullptr},
        switchToSteam_{nullptr}, keepInTray_{nullptr};
    winrt::Microsoft::UI::Xaml::Media::Animation::Storyboard settingsIn_{nullptr};

    std::vector<Output> outputs_;
    std::vector<bs::CodecId> formats_;
    std::array<MeterBar, 6> meter_;
    std::vector<std::pair<std::wstring, std::wstring>> detailRows_;
    std::wstring messageText_;

    bool encoding_ = false;
    bool populating_ = false;
    bool quitting_ = false;
    bool shown_ = false;
    bool settingsOpen_ = false;
    int detailsCountdown_ = 0;
    std::chrono::steady_clock::time_point lastFrame_ = std::chrono::steady_clock::now();
};
