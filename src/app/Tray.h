// Notification-area icon with a small menu (Show, Start/Stop, Quit).
#pragma once

#include <functional>
#include <string>

#include <shellapi.h>

class Tray {
public:
    struct Actions {
        std::function<void()> show;
        std::function<void()> toggle;
        std::function<void()> quit;
    };

    explicit Tray(Actions actions);
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    bool visible() const { return added_; }
    void setTooltip(const std::wstring& text);
    void setEncoding(bool encoding) { encoding_ = encoding; }

private:
    static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    void add();
    void showMenu();

    Actions actions_;
    HWND hwnd_ = nullptr;
    NOTIFYICONDATAW data_{};
    bool added_ = false;
    bool encoding_ = false;
    std::wstring tooltip_;
    UINT taskbarCreated_ = 0;
};
