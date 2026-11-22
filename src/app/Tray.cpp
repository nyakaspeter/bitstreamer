#include "pch.h"
#include "Tray.h"

namespace {
constexpr UINT kTrayMessage = WM_APP + 1;
constexpr UINT kCmdShow = 1, kCmdToggle = 2, kCmdQuit = 3;
const wchar_t* const kClassName = L"bitstreamer.tray";
}  // namespace

Tray::Tray(Actions actions) : actions_(std::move(actions)) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = &Tray::wndProc;
    wc.hInstance = instance;
    wc.lpszClassName = kClassName;
    RegisterClassW(&wc);
    hwnd_ = CreateWindowExW(0, kClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
    // Explorer announces a restart with this message; the icon must be added again.
    taskbarCreated_ = RegisterWindowMessageW(L"TaskbarCreated");

    data_.cbSize = sizeof(data_);
    data_.hWnd = hwnd_;
    data_.uID = 1;
    data_.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data_.uCallbackMessage = kTrayMessage;
    data_.hIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                                GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR));
    wcscpy_s(data_.szTip, L"bitstreamer");
    tooltip_ = L"bitstreamer";
    add();
}

Tray::~Tray() {
    if (added_) Shell_NotifyIconW(NIM_DELETE, &data_);
    if (data_.hIcon) DestroyIcon(data_.hIcon);
    if (hwnd_) DestroyWindow(hwnd_);
}

void Tray::add() {
    added_ = Shell_NotifyIconW(NIM_ADD, &data_) != FALSE;
}

void Tray::setTooltip(const std::wstring& text) {
    if (text == tooltip_) return;
    tooltip_ = text;
    wcsncpy_s(data_.szTip, text.c_str(), _TRUNCATE);
    if (added_) Shell_NotifyIconW(NIM_MODIFY, &data_);
}

void Tray::showMenu() {
    const HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, kCmdShow, L"Show");
    AppendMenuW(menu, MF_STRING, kCmdToggle, encoding_ ? L"Stop" : L"Start");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kCmdQuit, L"Quit");
    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(hwnd_);  // so the menu closes when the user clicks elsewhere
    const UINT cmd = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, pt.x, pt.y, 0, hwnd_, nullptr));
    DestroyMenu(menu);
    if (cmd == kCmdShow && actions_.show) actions_.show();
    else if (cmd == kCmdToggle && actions_.toggle) actions_.toggle();
    else if (cmd == kCmdQuit && actions_.quit) actions_.quit();
}

LRESULT CALLBACK Tray::wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
    Tray* self = reinterpret_cast<Tray*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        if (msg == kTrayMessage) {
            const UINT event = LOWORD(lp);
            if (event == WM_LBUTTONUP || event == WM_LBUTTONDBLCLK) { if (self->actions_.show) self->actions_.show(); }
            else if (event == WM_RBUTTONUP || event == WM_CONTEXTMENU) self->showMenu();
            return 0;
        }
        if (msg == self->taskbarCreated_ && self->taskbarCreated_ != 0) {
            self->add();
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
