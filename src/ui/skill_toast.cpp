#include "skill_toast.h"
#include "gear_widget.h"
#include "ui_host.h"
#include "ui_i18n.h"
#include "ui_motion.h"
#include "ui_surface.h"
#include "ui_theme.h"
#include <windows.h>
#include <algorithm>
#include <cmath>

namespace SkillToast {
namespace {
constexpr wchar_t kClass[] = L"PBZSkillToast_v1";
// DIPs. The notice sits kMargin from the client's right edge and kBottom above its bottom edge,
// higher while the gear pill (48 DIP) is showing in the same corner.
constexpr double kMargin = 28, kBottom = 96, kAboveGear = 28 + 48 + 14, kMaxText = 420, kShadow = 16;
constexpr ULONGLONG kLife = 2600;

SRWLOCK lock_ = SRWLOCK_INIT;
std::wstring pending_title_, pending_detail_; bool pending_ = false;
HWND window_ = nullptr;
UiSurface::Surface surface_;
std::wstring title_, detail_;
ULONGLONG shown_ = 0;
bool active_ = false, leaving_ = false, reduced_ = false;
UiMotion::Tween fade_, slide_;
double scale_ = 1;
int width_ = 0, height_ = 0, margin_ = 0, last_alpha_ = -1;
POINT last_{-1, -1};

int S(double value) { return static_cast<int>(std::lround(value * scale_)); }
UINT Dpi() { return static_cast<UINT>(std::lround(96 * scale_)); }

bool Create() {
    if (window_) return true;
    WNDCLASSW cls{}; cls.lpfnWndProc = DefWindowProcW; cls.hInstance = GetModuleHandleW(nullptr); cls.lpszClassName = kClass;
    if (!RegisterClassW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    // Click-through: the notice never takes input from the game.
    window_ = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT, kClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, cls.hInstance, nullptr);
    if (!window_) return false;
    if (!UiHost::Attach(window_)) { DestroyWindow(window_); window_ = nullptr; return false; }
    return true;
}
void Render() {
    const std::wstring title = UiI18n::Translate(title_), detail = UiI18n::Translate(detail_);
    const HFONT title_font = UiTheme::GetFontFor(UiTheme::Font::Heading, Dpi(), title.c_str()), detail_font = UiTheme::GetFont(UiTheme::Font::Small, Dpi());
    HDC screen = GetDC(nullptr); HDC measure = CreateCompatibleDC(screen); ReleaseDC(nullptr, screen);
    RECT title_box{0, 0, S(kMaxText), 0}, detail_box{0, 0, S(kMaxText), 0};
    HGDIOBJ old = SelectObject(measure, title_font); DrawTextW(measure, title.c_str(), -1, &title_box, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(measure, detail_font); DrawTextW(measure, detail.c_str(), -1, &detail_box, DT_CALCRECT | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(measure, old); DeleteDC(measure);
    const int text_width = (std::min)(S(kMaxText), static_cast<int>((std::max)(title_box.right, detail_box.right)));
    margin_ = S(kShadow);
    width_ = S(18) + S(4) + S(14) + text_width + S(22);
    height_ = S(14) + title_box.bottom + S(2) + detail_box.bottom + S(14);
    if (!surface_.Ensure(width_ + 2 * margin_, height_ + 2 * margin_)) return;
    surface_.Clear(); HDC dc = surface_.dc; SetBkMode(dc, TRANSPARENT);
    const RECT body{margin_, margin_, margin_ + width_, margin_ + height_};
    SetDCBrushColor(dc, UiTheme::Mix(UiTheme::Panel(), UiTheme::Text(), 5)); FillRect(dc, &body, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    // Accent bar on the left, like the game's skill prompts.
    const RECT bar{body.left + S(18), body.top + S(14), body.left + S(18) + S(4), body.bottom - S(14)};
    SetDCBrushColor(dc, UiTheme::Accent()); FillRect(dc, &bar, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    const int left = bar.right + S(14);
    RECT title_rect{left, body.top + S(14), body.right - S(22), body.top + S(14) + title_box.bottom};
    RECT detail_rect{left, title_rect.bottom + S(2), body.right - S(22), body.bottom - S(14)};
    old = SelectObject(dc, title_font); SetTextColor(dc, UiTheme::Text());
    DrawTextW(dc, title.c_str(), -1, &title_rect, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, detail_font); SetTextColor(dc, UiTheme::MutedText());
    DrawTextW(dc, detail.c_str(), -1, &detail_rect, DT_SINGLELINE | DT_LEFT | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, old);
    surface_.Shape(body, S(12), UiTheme::Mix(UiTheme::Border(), UiTheme::Accent(), 35), margin_, .5);
    last_ = {-1, -1}; last_alpha_ = -1;
}
}

void Show(const std::wstring& title, const std::wstring& detail) {
    AcquireSRWLockExclusive(&lock_); pending_title_ = title; pending_detail_ = detail; pending_ = true; ReleaseSRWLockExclusive(&lock_);
}
void Tick() {
    const ULONGLONG now = GetTickCount64();
    bool fresh = false;
    AcquireSRWLockExclusive(&lock_);
    if (pending_) { title_ = pending_title_; detail_ = pending_detail_; pending_ = false; fresh = true; }
    ReleaseSRWLockExclusive(&lock_);
    if (fresh) {
        if (!UiHost::InGame() || !Create()) return;
        const HWND game = UiHost::Game(); const UINT dpi = game ? GetDpiForWindow(game) : 96;
        scale_ = std::clamp(static_cast<double>(dpi ? dpi : 96) / 96.0, .75, 2.5); reduced_ = UiMotion::ReducedMotion();
        const bool replace = active_ && !leaving_;
        Render(); shown_ = now; active_ = true; leaving_ = false;
        fade_.ease = UiMotion::EaseOutCubic; slide_.ease = UiMotion::EaseOutQuint;
        if (!replace) { fade_.Jump(0); slide_.Jump(S(14)); }
        fade_.Set(1, 200, now, reduced_); slide_.Set(0, 320, now, reduced_);
    }
    if (!active_ || !window_) return;
    if (!UiHost::InGame()) { ShowWindow(window_, SW_HIDE); active_ = false; return; }
    if (!leaving_ && now >= shown_ + kLife) { leaving_ = true; fade_.ease = UiMotion::EaseInCubic; fade_.Set(0, 240, now, reduced_); slide_.Set(S(8), 240, now, reduced_); }
    if (leaving_ && !fade_.Running(now)) { ShowWindow(window_, SW_HIDE); active_ = false; return; }
    const SIZE client = UiHost::ClientSize(); const POINT origin = UiHost::ClientOrigin();
    const int bottom = S(GearWidget::Active() ? kAboveGear : kBottom);
    const POINT at{origin.x + client.cx - S(kMargin) - width_ - margin_, origin.y + client.cy - bottom - height_ - margin_ + static_cast<int>(std::lround(slide_.At(now)))};
    const int alpha = static_cast<int>(std::clamp(std::lround(fade_.At(now) * 255), 0L, 255L));
    if (at.x != last_.x || at.y != last_.y || alpha != last_alpha_) { surface_.Present(window_, at, static_cast<BYTE>(alpha)); last_ = at; last_alpha_ = alpha; }
    if (!IsWindowVisible(window_)) { ShowWindow(window_, SW_SHOWNA); SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); }
}
bool Active() { return active_; }
void Shutdown() {
    if (window_) { UiHost::Detach(window_); DestroyWindow(window_); }
    window_ = nullptr; surface_.Reset(); active_ = false;
}
}
