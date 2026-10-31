#include "ui_host.h"
#include <cwchar>

namespace UiHost {
namespace {
HWND game_ = nullptr;
POINT origin_{};
int client_w_ = 0, client_h_ = 0;
bool ready_ = false, foreground_ = false;
ULONGLONG next_scan_ = 0;

bool IsGame(HWND window) {
    DWORD process = 0;
    wchar_t type[96]{};
    return window && IsWindow(window) && GetWindowThreadProcessId(window, &process) && process == GetCurrentProcessId() &&
        GetClassNameW(window, type, 96) && !_wcsicmp(type, L"UnrealWindow");
}
BOOL CALLBACK Find(HWND window, LPARAM result) {
    if (!IsGame(window) || !IsWindowVisible(window) || IsIconic(window)) return TRUE;
    *reinterpret_cast<HWND*>(result) = window;
    return FALSE;
}
void Refresh() {
    if (!IsGame(game_)) {
        game_ = nullptr;
        if (GetTickCount64() >= next_scan_) {
            next_scan_ = GetTickCount64() + 400;
            EnumWindows(Find, reinterpret_cast<LPARAM>(&game_));
        }
    }
    ready_ = false;
    foreground_ = false;
    client_w_ = client_h_ = 0;
    origin_ = {};
    if (!game_) return;
    RECT window{}, client{};
    if (!GetWindowRect(game_, &window) || !GetClientRect(game_, &client)) return;
    // PBZ's game window is a borderless popup, so the client origin is the
    // window origin. A framed window uses the leftover non-client as a guess
    // and never sends a message to the game thread.
    const int extra_w = (window.right - window.left) - (client.right - client.left);
    const int extra_h = (window.bottom - window.top) - (client.bottom - client.top);
    const int side = extra_w > 0 ? extra_w / 2 : 0;
    origin_.x = window.left + side;
    origin_.y = window.top + (extra_h > side ? extra_h - side : 0);
    client_w_ = client.right - client.left;
    client_h_ = client.bottom - client.top;
    ready_ = IsWindowVisible(game_) && !IsIconic(game_) && client_w_ > 64 && client_h_ > 64;
    DWORD process = 0;
    const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &process);
    foreground_ = process == GetCurrentProcessId();
}
}
void Tick() { Refresh(); }
HWND Game() { return IsGame(game_) ? game_ : nullptr; }
bool InGame() { return ready_ && foreground_; }
POINT ClientOrigin() { return origin_; }
SIZE ClientSize() { return {client_w_, client_h_}; }
RECT ClientScreen() { return {origin_.x, origin_.y, origin_.x + client_w_, origin_.y + client_h_}; }
bool FocusInside(HWND root) {
    if (!root) return false;
    HWND window = GetFocus();
    for (int i = 0; window && i < 16; ++i) {
        if (window == root) return true;
        const HWND parent = GetParent(window);
        window = parent ? parent : GetWindow(window, GW_OWNER);
    }
    return false;
}
bool Attach(HWND window) {
    Refresh();
    if (!game_ || !window || !IsWindow(window)) return false;
    if (GetParent(window) == game_ && (GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)) return true;
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_POPUP | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_OVERLAPPED);
    style |= WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    LONG_PTR extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    extended &= ~static_cast<LONG_PTR>(WS_EX_APPWINDOW | WS_EX_TOPMOST | WS_EX_TOOLWINDOW);
    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetWindowLongPtrW(window, GWL_EXSTYLE, extended);
    SetLastError(ERROR_SUCCESS);
    SetParent(window, game_);
    if (GetParent(window) != game_) return false;
    SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_NOREDRAW);
    return true;
}
void Detach(HWND window) {
    if (!window || !IsWindow(window) || !(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)) return;
    ShowWindow(window, SW_HIDE);
    LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    style &= ~static_cast<LONG_PTR>(WS_CHILD);
    style |= WS_POPUP;
    SetWindowLongPtrW(window, GWL_STYLE, style);
    SetParent(window, nullptr);
}
}
