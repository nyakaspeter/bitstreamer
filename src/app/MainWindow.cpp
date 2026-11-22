#include "pch.h"
#include "MainWindow.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include <commctrl.h>
#include <DispatcherQueue.h>

#include "Layout.h"

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;
using namespace winrt::Microsoft::UI::Xaml::Controls;
using namespace winrt::Microsoft::UI::Xaml::Media;
using namespace winrt::Microsoft::UI::Composition::SystemBackdrops;
using namespace bs;

namespace {

constexpr int kTickMs = 16;

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

std::wstring number(double value, int decimals) {
    wchar_t buf[48];
    swprintf_s(buf, L"%.*f", decimals, value);
    return buf;
}
std::wstring ms(double value, int decimals = 1) { return number(value, decimals) + L" ms"; }
std::wstring kbit(uint32_t bitrate) { return number(bitrate / 1000.0, bitrate % 1000 ? 1 : 0) + L" kbit/s"; }

SystemBackdropTheme backdropTheme(ElementTheme theme) {
    return theme == ElementTheme::Dark ? SystemBackdropTheme::Dark
         : theme == ElementTheme::Light ? SystemBackdropTheme::Light : SystemBackdropTheme::Default;
}

// Shows the safety margin as "4 ms" and reads it back with or without the unit.
struct MillisecondsFormatter : implements<MillisecondsFormatter, winrt::Windows::Globalization::NumberFormatting::INumberFormatter2,
                                          winrt::Windows::Globalization::NumberFormatting::INumberParser> {
    hstring FormatInt(int64_t value) const { return hstring(std::to_wstring(value) + L" ms"); }
    hstring FormatUInt(uint64_t value) const { return hstring(std::to_wstring(value) + L" ms"); }
    hstring FormatDouble(double value) const { return hstring(number(value, 0) + L" ms"); }
    winrt::Windows::Foundation::IReference<int64_t> ParseInt(hstring const& text) const {
        const auto v = ParseDouble(text);
        return v ? winrt::Windows::Foundation::IReference<int64_t>(std::llround(v.Value())) : nullptr;
    }
    winrt::Windows::Foundation::IReference<uint64_t> ParseUInt(hstring const& text) const {
        const auto v = ParseDouble(text);
        return v && v.Value() >= 0 ? winrt::Windows::Foundation::IReference<uint64_t>(static_cast<uint64_t>(std::llround(v.Value()))) : nullptr;
    }
    winrt::Windows::Foundation::IReference<double> ParseDouble(hstring const& text) const {
        wchar_t* end = nullptr;
        const double v = std::wcstod(text.c_str(), &end);
        if (end == text.c_str()) return nullptr;  // no number: the box keeps its value
        return v;
    }
};

// The acrylic controller needs a system dispatcher queue on the thread.
void ensureSystemDispatcherQueue() {
    static winrt::Windows::System::DispatcherQueueController controller{nullptr};
    if (controller || winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return;
    const DispatcherQueueOptions options{sizeof(DispatcherQueueOptions), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE};
    check_hresult(CreateDispatcherQueueController(options, reinterpret_cast<PDISPATCHERQUEUECONTROLLER*>(put_abi(controller))));
}

}  // namespace

unsigned MainWindow::showMessageId() {
    static const unsigned id = RegisterWindowMessageW(L"bitstreamer.show-window");
    return id;
}

MainWindow::MainWindow(bool startMinimized) {
    settings_.load();
    buildWindow();
    buildMeters();
    buildBackdrop();
    tray_ = std::make_unique<Tray>(Tray::Actions{[this] { showWindow(); }, [this] { toggleEncoding(); }, [this] { quit(); }});
    pipeline_.setUpmix(static_cast<UpmixMode>(settings_.upmix));
    connectHandlers();
    refreshOutputs();

    // Device hot-plug: the notifier calls back on a COM thread; debounce on the UI thread.
    const auto queue = winrt::Microsoft::UI::Dispatching::DispatcherQueue::GetForCurrentThread();
    deviceRefreshTimer_ = queue.CreateTimer();
    deviceRefreshTimer_.Interval(std::chrono::milliseconds(700));
    deviceRefreshTimer_.IsRepeating(false);
    deviceRefreshTimer_.Tick([this](auto&&, auto&&) { if (!encoding_) refreshOutputs(); });
    notifier_ = std::make_unique<DeviceNotifier>([this, queue] {
        queue.TryEnqueue([this] {
            deviceRefreshTimer_.Stop();
            deviceRefreshTimer_.Start();
        });
    });

    timer_ = queue.CreateTimer();
    timer_.Interval(std::chrono::milliseconds(kTickMs));
    timer_.Tick([this](auto&&, auto&&) { tick(); });
    timer_.Start();
    CompositionTarget::Rendering([this](auto&&, auto&&) { animateMeters(); });

    if (settings_.startEncodingOnLaunch && !settings_.outputDeviceId.empty()) startEncoding();
    if (!(startMinimized && tray_->visible())) showWindow();
}

MainWindow::~MainWindow() {
    notifier_.reset();
    pipeline_.stop();
}

// ------------------------------------------------------------------ window

void MainWindow::buildWindow() {
    window_ = Window();
    window_.Title(L"bitstreamer");
    root_ = Markup::XamlReader::Load(layout::kWindow).as<FrameworkElement>();
    window_.Content(root_);
    window_.ExtendsContentIntoTitleBar(true);
    window_.SetTitleBar(find<UIElement>(L"titleBar"));
    check_hresult(window_.as<::IWindowNative>()->get_WindowHandle(&hwnd_));

    const HINSTANCE instance = GetModuleHandleW(nullptr);
    for (const auto [which, metric] : {std::pair{ICON_BIG, SM_CXICON}, std::pair{ICON_SMALL, SM_CXSMICON}}) {
        const int size = GetSystemMetrics(metric);
        SendMessageW(hwnd_, WM_SETICON, static_cast<WPARAM>(which),
                     reinterpret_cast<LPARAM>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, LR_DEFAULTCOLOR)));
    }
    SetWindowSubclass(hwnd_, &MainWindow::subclassProc, 1, reinterpret_cast<DWORD_PTR>(this));

    output_ = find<ComboBox>(L"output");
    format_ = find<ComboBox>(L"format");
    upmix_ = find<ComboBox>(L"upmix");
    start_ = find<Button>(L"start");
    // Looked up once: after the button's style is swapped they can no longer be found by name.
    startLabel_ = find<TextBlock>(L"startLabel");
    startIcon_ = find<FontIcon>(L"startIcon");
    settingsToggle_ = find<Primitives::ToggleButton>(L"settingsToggle");
    message_ = find<TextBlock>(L"message");
    bars_ = find<FrameworkElement>(L"bars");
    settingsPane_ = find<FrameworkElement>(L"settingsPane");
    details_ = find<Grid>(L"details");
    ac3Bitrate_ = find<ComboBox>(L"ac3Bitrate");
    dtsBitrate_ = find<ComboBox>(L"dtsBitrate");
    ac3Margin_ = find<NumberBox>(L"ac3Margin");
    dtsMargin_ = find<NumberBox>(L"dtsMargin");
    const auto msFormat = make<MillisecondsFormatter>();
    ac3Margin_.NumberFormatter(msFormat);
    dtsMargin_.NumberFormatter(msFormat);
    startWithWindows_ = find<ToggleSwitch>(L"startWithWindows");
    startOnLaunch_ = find<ToggleSwitch>(L"startOnLaunch");
    switchToSteam_ = find<ToggleSwitch>(L"switchToSteam");
    keepInTray_ = find<ToggleSwitch>(L"keepInTray");

    // The settings pane fades and slides in when it opens.
    using namespace Animation;
    settingsIn_ = Storyboard();
    DoubleAnimation fade;
    fade.From(0.0);
    fade.To(1.0);
    fade.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(220)));
    Storyboard::SetTarget(fade, find<DependencyObject>(L"settingsContent"));
    Storyboard::SetTargetProperty(fade, L"Opacity");
    DoubleAnimation slide;
    slide.From(-20.0);
    slide.To(0.0);
    slide.Duration(DurationHelper::FromTimeSpan(std::chrono::milliseconds(260)));
    CubicEase ease;
    ease.EasingMode(EasingMode::EaseOut);
    slide.EasingFunction(ease);
    Storyboard::SetTarget(slide, find<DependencyObject>(L"settingsShift"));
    Storyboard::SetTargetProperty(slide, L"X");
    settingsIn_.Children().Append(fade);
    settingsIn_.Children().Append(slide);

    struct UpmixChoice { const wchar_t* name; const wchar_t* hint; };
    for (const UpmixChoice& choice : {UpmixChoice{L"Off", L"Stereo plays on the front left and right speakers only."},
                                      UpmixChoice{L"Quad speakers", L"Stereo is also played on the surround speakers."},
                                      UpmixChoice{L"Virtual surround", L"Stereo is spread over all speakers: speech towards the centre, "
                                                                       L"ambience to the surrounds, bass to the subwoofer."}}) {
        ComboBoxItem item;
        item.Content(box_value(choice.name));
        ToolTipService::SetToolTip(item, box_value(choice.hint));
        upmix_.Items().Append(item);
    }

    sizeWindowInitially();
}

void MainWindow::buildMeters() {
    static const wchar_t* const names[6] = {L"L", L"R", L"C", L"LFE", L"LS", L"RS"};
    const auto panel = bars_.as<Grid>();
    for (int c = 0; c < 6; ++c) {
        const auto bar = Markup::XamlReader::Load(layout::kMeterBar).as<FrameworkElement>();
        Grid::SetColumn(bar, c);
        panel.Children().Append(bar);
        MeterBar& m = meter_[static_cast<size_t>(c)];
        m.track = bar.FindName(L"track").as<FrameworkElement>();
        m.fill = bar.FindName(L"fill").as<FrameworkElement>();
        m.hot = bar.FindName(L"hot").as<FrameworkElement>();
        bar.FindName(L"label").as<TextBlock>().Text(names[c]);
    }
}

// A see-through acrylic background that stays see-through while the window
// is not focused (the stock backdrop turns solid then, and this window
// spends most of its life in the background).
void MainWindow::buildBackdrop() {
    try {
        if (!DesktopAcrylicController::IsSupported()) return;
        ensureSystemDispatcherQueue();
        backdropConfig_ = SystemBackdropConfiguration();
        backdropConfig_.IsInputActive(true);
        backdropConfig_.Theme(backdropTheme(root_.ActualTheme()));
        backdrop_ = DesktopAcrylicController();
        backdrop_.AddSystemBackdropTarget(window_.as<winrt::Microsoft::UI::Composition::ICompositionSupportsSystemBackdrop>());
        backdrop_.SetSystemBackdropConfiguration(backdropConfig_);
        root_.ActualThemeChanged([this](FrameworkElement const& sender, auto&&) {
            backdropConfig_.Theme(backdropTheme(sender.ActualTheme()));
        });
    } catch (...) {
        backdrop_ = nullptr;
        window_.SystemBackdrop(DesktopAcrylicBackdrop());
    }
}

void MainWindow::connectHandlers() {
    start_.Click([this](auto&&, auto&&) { toggleEncoding(); });
    settingsToggle_.Click([this](auto&&, auto&&) {
        const auto checked = settingsToggle_.IsChecked();
        openSettings(checked && checked.Value());
    });
    output_.SelectionChanged([this](auto&&, auto&&) { if (!populating_) onOutputChanged(); });
    format_.SelectionChanged([this](auto&&, auto&&) { if (!populating_) onFormatChanged(); });
    // The upmix changes on the fly; encoding is not restarted.
    upmix_.SelectionChanged([this](auto&&, auto&&) {
        if (populating_ || upmix_.SelectedIndex() < 0) return;  // blanked while no output is available
        settings_.upmix = upmix_.SelectedIndex();
        pipeline_.setUpmix(static_cast<UpmixMode>(settings_.upmix));
        settings_.save();
    });

    // Settings take effect as they are changed.
    const auto bitrateChanged = [this](CodecId id, ComboBox const& box) {
        const CodecInfo* info = findCodec(id);
        const int i = box.SelectedIndex();
        if (populating_ || !info || i < 0 || i >= static_cast<int>(info->bitrates.size())) return;
        settings_.format(id).bitrate = info->bitrates[static_cast<size_t>(i)];
        onFormatSettingChanged(id);
    };
    ac3Bitrate_.SelectionChanged([=, this](auto&&, auto&&) { bitrateChanged(CodecId::Ac3, ac3Bitrate_); });
    dtsBitrate_.SelectionChanged([=, this](auto&&, auto&&) { bitrateChanged(CodecId::Dts, dtsBitrate_); });
    const auto marginChanged = [this](CodecId id, NumberBox const& box) {
        if (populating_) return;
        const double entered = box.Value();
        int& margin = settings_.format(id).marginMs;
        if (!std::isnan(entered)) margin = std::clamp(static_cast<int>(std::lround(entered)), 2, 30);
        if (std::isnan(entered) || entered != margin) {  // emptied, fractional or out of range: show what is used
            populating_ = true;
            box.Value(margin);
            populating_ = false;
        }
        onFormatSettingChanged(id);
    };
    ac3Margin_.ValueChanged([=, this](auto&&, auto&&) { marginChanged(CodecId::Ac3, ac3Margin_); });
    dtsMargin_.ValueChanged([=, this](auto&&, auto&&) { marginChanged(CodecId::Dts, dtsMargin_); });

    startWithWindows_.Toggled([this](auto&&, auto&&) {
        if (populating_) return;
        settings_.startWithWindows = startWithWindows_.IsOn();
        settings_.save();
        AppSettings::setStartWithWindows(settings_.startWithWindows);
    });
    startOnLaunch_.Toggled([this](auto&&, auto&&) {
        if (populating_) return;
        settings_.startEncodingOnLaunch = startOnLaunch_.IsOn();
        settings_.save();
    });
    switchToSteam_.Toggled([this](auto&&, auto&&) {
        if (populating_) return;
        settings_.switchToSteamSpeakers = switchToSteam_.IsOn();
        settings_.save();
        if (encoding_) startEncoding();
    });
    keepInTray_.Toggled([this](auto&&, auto&&) {
        if (populating_) return;
        settings_.minimizeToTray = keepInTray_.IsOn();
        settings_.save();
    });

    // Closing the window keeps the app running in the notification area.
    window_.AppWindow().Closing([this](auto&&, winrt::Microsoft::UI::Windowing::AppWindowClosingEventArgs const& args) {
        args.Cancel(true);
        if (!quitting_ && settings_.minimizeToTray && tray_ && tray_->visible()) window_.AppWindow().Hide();
        else quit();
    });
}

void MainWindow::showWindow() {
    if (!shown_) {
        window_.Activate();
        shown_ = true;
    } else {
        window_.AppWindow().Show();
    }
    if (IsIconic(hwnd_)) ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
}

void MainWindow::quit() {
    if (quitting_) return;
    quitting_ = true;
    if (timer_) timer_.Stop();
    notifier_.reset();
    pipeline_.stop();
    tray_.reset();
    Application::Current().Exit();
}

bool MainWindow::maximized() const {
    return IsZoomed(hwnd_) != FALSE;
}

// The size the window opens with: the main column alone.
void MainWindow::sizeWindowInitially() {
    const double scale = GetDpiForWindow(hwnd_) / 96.0;
    window_.AppWindow().ResizeClient({static_cast<int32_t>(std::lround(layout::kMainWidth * scale)),
                                      static_cast<int32_t>(std::lround(layout::kHeight * scale))});
}

// Widens or narrows the window (by device-independent pixels) when the
// settings pane opens or closes, keeping whatever size the user gave it,
// and keeps it on the screen. A maximized window is left alone: the pane
// simply takes its space from the main column.
void MainWindow::growWindow(int deltaWidth) {
    if (maximized()) return;
    const double scale = GetDpiForWindow(hwnd_) / 96.0;
    const auto appWindow = window_.AppWindow();
    const auto outer = appWindow.Size();  // the whole window: resizing by client size made it creep taller
    const int32_t minWidth = static_cast<int32_t>(std::lround((layout::kMinMainWidth + (settingsOpen_ ? layout::kSettingsWidth : 0)) * scale));
    const int32_t width = std::max(minWidth, outer.Width + static_cast<int32_t>(std::lround(deltaWidth * scale)));
    if (width == outer.Width) return;
    appWindow.Resize({width, outer.Height});

    const auto area = winrt::Microsoft::UI::Windowing::DisplayArea::GetFromWindowId(appWindow.Id(), winrt::Microsoft::UI::Windowing::DisplayAreaFallback::Nearest);
    const auto work = area.WorkArea();
    const auto pos = appWindow.Position();
    const auto size = appWindow.Size();
    const int32_t x = std::max(work.X, std::min(pos.X, work.X + work.Width - size.Width));
    const int32_t y = std::max(work.Y, std::min(pos.Y, work.Y + work.Height - size.Height));
    if (x != pos.X || y != pos.Y) appWindow.Move({x, y});
}

LRESULT CALLBACK MainWindow::subclassProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR ref) {
    MainWindow* self = reinterpret_cast<MainWindow*>(ref);
    if (msg == showMessageId()) {
        self->showWindow();
        return 0;
    }
    if (msg == WM_GETMINMAXINFO) {
        // The window cannot be made smaller than its content needs.
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        const double scale = GetDpiForWindow(hwnd) / 96.0;
        MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lp);
        const int width = layout::kMinMainWidth + (self->settingsOpen_ ? layout::kSettingsWidth : 0);
        info->ptMinTrackSize.x = static_cast<LONG>(std::lround(width * scale));
        info->ptMinTrackSize.y = static_cast<LONG>(std::lround(layout::kMinHeight * scale));
        return r;
    }
    if (msg == WM_SIZE && wp == SIZE_RESTORED) {
        // Coming back from maximized with the settings pane open: make sure it still fits.
        const LRESULT r = DefSubclassProc(hwnd, msg, wp, lp);
        self->growWindow(0);
        return r;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ---------------------------------------------------------------- devices

// Outputs: only devices whose driver accepted a Dolby Digital or DTS format.
void MainWindow::refreshOutputs() {
    populating_ = true;
    const TransportDescriptor ac3 = transportFor(CodecId::Ac3);
    const CodecInfo* dtsInfo = findCodec(CodecId::Dts);
    const bool haveDts = dtsInfo && dtsInfo->available;
    const TransportDescriptor dts = haveDts ? transportFor(CodecId::Dts) : TransportDescriptor{};
    const std::wstring inputId = findSteamSpeakers().id;

    outputs_.clear();
    output_.Items().Clear();
    int selected = -1;
    for (const DeviceInfo& d : enumerateDevices(DeviceFlow::Render)) {
        if (d.id == inputId) continue;
        const bool okAc3 = probeBitstreamSupport(d.id, ac3).negotiated;
        const bool okDts = haveDts && probeBitstreamSupport(d.id, dts).negotiated;
        if (!okAc3 && !okDts) continue;
        if (d.id == settings_.outputDeviceId) selected = static_cast<int>(outputs_.size());
        outputs_.push_back({d.id, to_hstring(d.name), okAc3, okDts});
        output_.Items().Append(box_value(outputs_.back().name));
    }
    if (selected < 0 && !outputs_.empty()) selected = 0;
    output_.SelectedIndex(selected);
    // Without a usable output there is nothing to choose: all three selects
    // are disabled and left blank (the meter box says why).
    const bool hasOutput = !outputs_.empty();
    output_.IsEnabled(hasOutput);
    upmix_.SelectedIndex(hasOutput ? settings_.upmix : -1);
    upmix_.IsEnabled(hasOutput);
    populating_ = false;

    settings_.outputDeviceId = selected >= 0 ? outputs_[static_cast<size_t>(selected)].id : std::wstring();
    refreshFormats();
}

// Formats: the codecs the selected output accepts.
void MainWindow::refreshFormats() {
    populating_ = true;
    formats_.clear();
    format_.Items().Clear();
    const int i = output_.SelectedIndex();
    if (i >= 0 && i < static_cast<int>(outputs_.size())) {
        const Output& o = outputs_[static_cast<size_t>(i)];
        if (o.ac3) { formats_.push_back(CodecId::Ac3); format_.Items().Append(box_value(L"Dolby Digital")); }
        if (o.dts) { formats_.push_back(CodecId::Dts); format_.Items().Append(box_value(L"DTS")); }
    }
    int selected = -1;
    for (size_t f = 0; f < formats_.size(); ++f)
        if (formats_[f] == settings_.codec) selected = static_cast<int>(f);
    if (selected < 0 && !formats_.empty()) selected = 0;
    format_.SelectedIndex(selected);
    format_.IsEnabled(!formats_.empty());
    populating_ = false;

    if (selected >= 0) settings_.codec = formats_[static_cast<size_t>(selected)];
    settings_.loadTuning();
    updateStartButton();
    if (settingsOpen_) populateSettings();
}

void MainWindow::onOutputChanged() {
    const int i = output_.SelectedIndex();
    settings_.outputDeviceId = i >= 0 && i < static_cast<int>(outputs_.size()) ? outputs_[static_cast<size_t>(i)].id : std::wstring();
    refreshFormats();
    settings_.save();
    if (encoding_) startEncoding();
}

void MainWindow::onFormatChanged() {
    const int i = format_.SelectedIndex();
    if (i >= 0 && i < static_cast<int>(formats_.size())) settings_.codec = formats_[static_cast<size_t>(i)];
    settings_.loadTuning();
    settings_.save();
    if (settingsOpen_) populateSettings();
    if (encoding_) startEncoding();
}

// --------------------------------------------------------------- encoding

void MainWindow::startEncoding() {
    if (settings_.outputDeviceId.empty()) return;
    pipeline_.start(settings_.toConfig());
    encoding_ = true;
    updateStartButton();
}

void MainWindow::stopEncoding() {
    pipeline_.stop();
    encoding_ = false;
    updateStartButton();
    refreshOutputs();
}

void MainWindow::toggleEncoding() {
    if (encoding_) stopEncoding(); else startEncoding();
}

void MainWindow::updateStartButton() {
    startLabel_.Text(encoding_ ? L"Stop" : L"Start");
    Automation::AutomationProperties::SetName(start_, encoding_ ? L"Stop" : L"Start");  // for screen readers
    startIcon_.Glyph(encoding_ ? L"" : L"");  // stop and play symbols
    start_.IsEnabled(encoding_ || format_.SelectedIndex() >= 0);
    // Start is the highlighted action; Stop is an ordinary button.
    const auto style = Application::Current().Resources().TryLookup(box_value(encoding_ ? L"DefaultButtonStyle" : L"AccentButtonStyle"));
    if (style) start_.Style(style.as<Style>());
    if (tray_) tray_->setEncoding(encoding_);
}

// ------------------------------------------------------------------- tick

void MainWindow::tick() {
    const PipelineStatus st = pipeline_.status();

    // A short message in the meter box whenever nothing is being encoded.
    // The technical detail of a problem is in the settings pane.
    const wchar_t* status = L"";
    if (!encoding_) {
        status = start_.IsEnabled() ? L"Press Start to begin encoding." : L"No Dolby Digital or DTS capable device is connected.";
    } else if (st.state == PipelineState::Standby) {
        status = L"Waiting for Steam Streaming Speakers to be selected as the playback device in Windows.";
    } else {
        switch (st.problem) {
            case PipelineProblem::OutputBusy: status = L"The selected output is in use by another program."; break;
            case PipelineProblem::OutputUnavailable: status = L"The selected output is not available. Check that it is connected and switched on."; break;
            case PipelineProblem::InputMissing: status = L"The Steam Streaming Speakers device was not found. Restart bitstreamer to install it."; break;
            case PipelineProblem::InputUnavailable: status = L"The Steam Streaming Speakers device is not responding."; break;
            case PipelineProblem::Encoder: status = L"The encoder could not be started."; break;
            case PipelineProblem::None: break;
        }
    }
    setMessage(status);
    updateMeters(st);

    if (tray_) {
        tray_->setTooltip(!encoding_ ? L"bitstreamer"
                          : st.state == PipelineState::Standby ? L"bitstreamer: waiting" : L"bitstreamer: encoding");
    }
    if (settingsOpen_ && --detailsCountdown_ <= 0) {
        detailsCountdown_ = 250 / kTickMs;
        updateDetails(st);
    }
}

void MainWindow::setMessage(const wchar_t* text) {
    if (messageText_ == text) return;
    messageText_ = text;
    message_.Text(text);
    bars_.Opacity(messageText_.empty() ? 1.0 : 0.4);  // the bars step back behind a message
}

void MainWindow::updateMeters(const PipelineStatus& st) {
    for (size_t c = 0; c < 6; ++c) {
        MeterBar& m = meter_[c];
        m.target = 0.0;
        if (st.sessionValid) {
            const float lin = st.stats.peaks[c];
            const double db = lin > 1e-6f ? 20.0 * std::log10(static_cast<double>(lin)) : -120.0;
            m.target = std::clamp((db + 60.0) / 60.0, 0.0, 1.0);
        }
    }
}

// Moves the bars towards their levels. Runs once per displayed frame, in
// step with the screen, and eases by elapsed time: quick on the way up, slow
// on the way down, so the bars glide instead of stepping.
void MainWindow::animateMeters() {
    const auto now = std::chrono::steady_clock::now();
    const double dt = std::clamp(std::chrono::duration<double>(now - lastFrame_).count(), 0.0, 0.1);
    lastFrame_ = now;
    for (MeterBar& m : meter_) {
        const double seconds = m.target > m.shown ? 0.035 : 0.25;
        double next = m.shown + (m.target - m.shown) * (1.0 - std::exp(-dt / seconds));
        if (next < 0.001 && m.target == 0.0) next = 0.0;
        if (next == m.shown) continue;
        m.shown = next;
        const double height = m.track.ActualHeight();
        if (height <= 0.0) continue;
        const double fill = m.shown * height;
        m.fill.Height(fill);
        m.hot.Height(fill);
        // Close to full scale the bar warms towards amber and red.
        m.hot.Opacity(std::clamp((m.shown - 0.88) / 0.1, 0.0, 1.0));
    }
}

// Read-out in the settings pane: what is running and where the delay comes from.
void MainWindow::updateDetails(const PipelineStatus& st) {
    std::vector<std::pair<std::wstring, std::wstring>> rows;  // a name starting with a space is a sub-item
    if (!st.sessionValid) {
        rows.push_back({L"Status", !encoding_ ? L"Not encoding"
                                   : st.state == PipelineState::Standby ? L"Waiting for Steam Streaming Speakers"
                                   : !st.message.empty() ? std::wstring(to_hstring(st.message)) : L"Starting"});
    } else {
        const LatencyBreakdown b = computeLatency(st);
        const EngineStats& s = st.stats;
        rows.push_back({L"Status", L"Encoding"});
        rows.push_back({L"Format", std::wstring(st.encoder.codec == CodecId::Dts ? L"DTS" : L"Dolby Digital") + L", " + kbit(st.encoder.bitrate)});
        if (b.valid) {
            rows.push_back({L"Latency", ms(b.totalMs)});
            rows.push_back({L" Codec frame", ms(b.frameDurationMs)});
            rows.push_back({L" Capture and queueing", ms(b.submitMs - b.frameDurationMs)});
            rows.push_back({L" Output device buffer", ms(b.outputDelayMs) + (b.outputDelayMeasured ? L"" : L" (estimate)")});
            rows.push_back({L" Codec look-ahead", ms(b.algorithmicDelayMs) + (st.encoder.algorithmicDelayEstimated ? L" (estimate)" : L"")});
            rows.push_back({L"Encoding time", ms(b.encodeMs, 2) + L" per frame"});
        } else {
            rows.push_back({L"Latency", L"measuring"});
        }
        // Every kind of interruption the app can see; the kinds that occurred are listed.
        const std::pair<const wchar_t*, uint64_t> kinds[] = {
            {L" Frame ready too late", s.underruns},        // a silent frame was sent in its place
            {L" Output device stalled", s.outputStalls},    // the output thread was held up; the receiver lost the stream
            {L" Gap in the input", s.discontinuities},      // Windows delivered the captured audio with a hole
            {L" Old audio discarded", s.overrunDrops},      // dropped to keep the delay from growing
            {L" Restarted", st.restarts},                   // a device failed and was reopened
        };
        uint64_t total = 0;
        for (const auto& kind : kinds) total += kind.second;
        rows.push_back({L"Dropouts", std::to_wstring(total)});
        for (const auto& kind : kinds)
            if (kind.second > 0) rows.push_back({kind.first, std::to_wstring(kind.second)});
    }
    if (rows == detailRows_) return;

    const auto children = details_.Children();
    const bool sameShape = rows.size() == detailRows_.size() &&
                           std::equal(rows.begin(), rows.end(), detailRows_.begin(), [](const auto& a, const auto& b) { return a.first == b.first; });
    if (sameShape) {
        for (size_t r = 0; r < rows.size(); ++r)
            if (rows[r].second != detailRows_[r].second) children.GetAt(static_cast<uint32_t>(r * 2 + 1)).as<TextBlock>().Text(rows[r].second);
    } else {
        children.Clear();
        details_.RowDefinitions().Clear();
        const auto secondary = Application::Current().Resources().TryLookup(box_value(L"TextFillColorSecondaryBrush"));
        for (size_t r = 0; r < rows.size(); ++r) {
            RowDefinition def;
            def.Height(GridLengthHelper::Auto());
            details_.RowDefinitions().Append(def);
            const bool sub = !rows[r].first.empty() && rows[r].first[0] == L' ';
            TextBlock name;
            name.Text(sub ? rows[r].first.substr(1) : rows[r].first);
            name.Margin({sub ? 14.0 : 0.0, 0, 0, 0});
            if (secondary) name.Foreground(secondary.as<Brush>());
            TextBlock value;
            value.Text(rows[r].second);
            value.TextWrapping(TextWrapping::Wrap);
            value.IsTextSelectionEnabled(true);
            Grid::SetRow(name, static_cast<int32_t>(r));
            Grid::SetRow(value, static_cast<int32_t>(r));
            Grid::SetColumn(value, 1);
            children.Append(name);
            children.Append(value);
        }
    }
    detailRows_ = std::move(rows);
}

// --------------------------------------------------------------- settings

void MainWindow::openSettings(bool open) {
    settingsOpen_ = open;
    if (open) {
        populateSettings();
        detailsCountdown_ = 0;
        settingsPane_.Visibility(Visibility::Visible);
        growWindow(layout::kSettingsWidth);
        settingsIn_.Begin();
    } else {
        settingsPane_.Visibility(Visibility::Collapsed);
        growWindow(-layout::kSettingsWidth);
    }
}

void MainWindow::populateSettings() {
    populating_ = true;
    const auto fill = [this](CodecId id, ComboBox const& bitrate, NumberBox const& margin) {
        const CodecInfo* info = findCodec(id);
        const bool available = info && info->available;
        bitrate.Items().Clear();
        bitrate.IsEnabled(available);
        margin.IsEnabled(available);
        if (!available) return;  // format not in this build
        const FormatSettings& f = settings_.format(id);
        int selected = 0;
        for (size_t i = 0; i < info->bitrates.size(); ++i) {
            bitrate.Items().Append(box_value(kbit(info->bitrates[i])));
            if (info->bitrates[i] == f.bitrate) selected = static_cast<int>(i);
        }
        bitrate.SelectedIndex(selected);
        margin.Value(f.marginMs);
    };
    fill(CodecId::Ac3, ac3Bitrate_, ac3Margin_);
    fill(CodecId::Dts, dtsBitrate_, dtsMargin_);
    startWithWindows_.IsOn(settings_.startWithWindows);
    startOnLaunch_.IsOn(settings_.startEncodingOnLaunch);
    switchToSteam_.IsOn(settings_.switchToSteamSpeakers);
    keepInTray_.IsOn(settings_.minimizeToTray);
    populating_ = false;
}

void MainWindow::onFormatSettingChanged(CodecId id) {
    settings_.save();
    settings_.saveTuning();
    if (encoding_ && id == settings_.codec) startEncoding();
}
