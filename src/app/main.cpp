#include "pch.h"

#include <commctrl.h>
#include <shellapi.h>

#include "MainWindow.h"

#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

using namespace winrt;
using namespace winrt::Microsoft::UI::Xaml;

namespace {

bool hasArgument(const wchar_t* wanted) {
    int count = 0;
    wchar_t** args = CommandLineToArgvW(GetCommandLineW(), &count);
    bool found = false;
    for (int i = 1; args && i < count; ++i)
        if (wcscmp(args[i], wanted) == 0) found = true;
    if (args) LocalFree(args);
    return found;
}

// A message box with one action button and Quit. Returns true for the action.
bool ask(const std::wstring& text, const wchar_t* action) {
    constexpr int kAction = 100, kQuit = 101;
    const TASKDIALOG_BUTTON buttons[] = {{kAction, action}, {kQuit, L"Quit"}};
    TASKDIALOGCONFIG config{};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_SIZE_TO_CONTENT;
    config.pszWindowTitle = L"bitstreamer";
    config.pszMainIcon = MAKEINTRESOURCEW(1);
    config.hInstance = GetModuleHandleW(nullptr);
    config.pszContent = text.c_str();
    config.pButtons = buttons;
    config.cButtons = 2;
    config.nDefaultButton = kAction;
    int pressed = kQuit;
    if (FAILED(TaskDialogIndirect(&config, &pressed, nullptr, nullptr))) return false;
    return pressed == kAction;
}

// The input device is Valve's "Steam Streaming Speakers". If it is missing it
// is installed from the driver package that ships with Steam (after asking;
// Windows shows its administrator prompt). Returns false if the user quits.
bool ensureSteamSpeakers() {
    for (;;) {
        if (!bs::findSteamSpeakers().id.empty()) return true;

        if (bs::steamSpeakersDriverInf().empty()) {
            if (!ask(L"bitstreamer needs Steam, which provides the virtual sound device it captures from.\n\n"
                     L"Install Steam, then choose Retry.", L"Retry"))
                return false;
            continue;
        }
        if (!ask(L"bitstreamer needs the \"Steam Streaming Speakers\" virtual sound device, which is not installed yet.", L"Install"))
            return false;

        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::string error;
        if (bs::installSteamSpeakersElevated(exe, L"--install-steam-speakers", error) == bs::SteamInstallResult::Installed) return true;
        if (!ask(L"The device could not be installed: " + std::wstring(to_hstring(error)) + L".", L"Retry")) return false;
    }
}

bool g_startMinimized = false;

// The app has no compiled XAML; its layout is parsed at run time. The
// metadata provider is what lets the parser find the WinUI control types.
struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    void OnLaunched(LaunchActivatedEventArgs const&) {
        Resources().MergedDictionaries().Append(Controls::XamlControlsResources());
        // Lives until the process ends: quitting stops the encoder explicitly.
        new MainWindow(g_startMinimized);
    }
    Markup::IXamlType GetXamlType(winrt::Windows::UI::Xaml::Interop::TypeName const& type) { return provider_.GetXamlType(type); }
    Markup::IXamlType GetXamlType(hstring const& name) { return provider_.GetXamlType(name); }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return provider_.GetXmlnsDefinitions(); }

private:
    XamlTypeInfo::XamlControlsXamlMetaDataProvider provider_;
};

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    // Elevated helper mode: install the driver and report the Win32 result.
    if (hasArgument(L"--install-steam-speakers")) return static_cast<int>(bs::installSteamSpeakersDriver());

    // Only one instance may run: a second launch brings the first to the front.
    const HANDLE instanceMutex = CreateMutexW(nullptr, TRUE, L"Local\\bitstreamer.single-instance");
    if (instanceMutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        PostMessageW(HWND_BROADCAST, MainWindow::showMessageId(), 0, 0);
        return 0;
    }

    init_apartment(apartment_type::single_threaded);
    g_startMinimized = hasArgument(L"--minimized");
    if (!ensureSteamSpeakers()) return 0;

    Application::Start([](auto&&) { make<App>(); });
    if (instanceMutex) CloseHandle(instanceMutex);
    return 0;
}
