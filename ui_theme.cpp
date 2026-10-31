#include "ui_theme.h"
#include "ui_assets.h"
#include "ui_host.h"
#include "ui_motion.h"
#include "ui_surface.h"
#include <commctrl.h>
#include <windowsx.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cwchar>
#include <cmath>
#include <map>
#include <new>
#include <string>
#include <utility>
#include <vector>
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdiplus.lib")

namespace UiTheme {
namespace {
constexpr UINT_PTR kSubclass = 0x50425a54484d01;
constexpr UINT_PTR kAnimationTimer = 0x50425a54;
constexpr ULONGLONG kDuration = 180;
constexpr UINT_PTR kHotTimer = 0x50425a55, kPopupTimer = 0x50425a56;
constexpr wchar_t kRoleProperty[] = L"PBZ.UiTheme.Role";
constexpr wchar_t kSpacerProperty[] = L"PBZ.UiTheme.RowSpacer";
constexpr wchar_t kSkinClass[] = L"PBZScrollSkin_v1";
constexpr wchar_t kDropdownClass[] = L"PBZDropdownList_v1";
enum class Kind { Container, Other, Button, Edit, ListBox, ComboBox, ListView, Header, Static, Tab, ToolTip };
struct Visual { double hover = 0, press = 0, focus = 0, check = 0; };
// Custom scroll bar laid over a control's native one; the control keeps its
// own scroll state and the overlay only drives it with WM_V/HSCROLL.
struct Skin {
    HWND window = nullptr, control = nullptr; int bar = SB_VERT; Skin** slot = nullptr;
    bool hot = false, drag = false, usable = false, timer = false; int grab = 0;
    UiMotion::Tween grow; SCROLLINFO info{};
};
struct Record {
    HWND window; Kind kind; bool apply_pending = false, hover = false, timer = false;
    Visual from, to; ULONGLONG start = 0;
    int hot_item = -1, hot_previous = -1; UiMotion::Tween hot_fade; bool hot_timer = false;
    bool scrollable = false; Skin* skins[2]{};
};
// The single open custom drop-down list of a CBS_DROPDOWNLIST combo box.
struct Dropdown {
    HWND popup = nullptr, combo = nullptr; bool open = false, closing = false, above = false;
    std::vector<std::wstring> items; int hot = -1, hot_previous = -1, selected = -1, visible = 0;
    int item = 0, pad = 0, margin = 0, width = 0, height = 0; RECT anchor{};
    double scroll = 0; int scroll_target = 0;
    UiMotion::Tween fade, slide, hot_fade;
    UiSurface::Surface surface;
};
Dropdown dropdown_;
HFONT FontFor(Font role, HWND window, const std::wstring& text);
struct Palette {
    // The game's menus: black ink, warm off-white text, pale-gold arrows and
    // parchment brush strokes (T_SettingsSelectedBar averages #dcd9d2).
    COLORREF base = RGB(0x09, 0x09, 0x0a), background = RGB(0x11, 0x11, 0x12), panel = RGB(0x1d, 0x1c, 0x1a);
    COLORREF border = RGB(0x3c, 0x39, 0x34), text = RGB(0xec, 0xe7, 0xdd);
    COLORREF muted = RGB(0x9c, 0x96, 0x8b), gold = RGB(0xd8, 0xc2, 0x92);
    COLORREF blue = RGB(0xb4, 0xbe, 0xc8), selection = RGB(0x33, 0x2f, 0x29), danger = RGB(0xc4, 0x48, 0x3a);
};
Palette colors_;
HBRUSH background_ = nullptr, panel_ = nullptr, base_ = nullptr;
DWORD thread_ = 0;
UINT apply_message_ = 0;
bool high_contrast_ = false;
bool reduced_motion_ = false;
ULONG_PTR graphics_token_ = 0;
uint64_t starts_ = 0, completes_ = 0, cancels_ = 0;
std::vector<Record*> records_;
std::map<std::pair<int, UINT>, HFONT> fonts_;
std::wstring icon_face_, body_face_;
bool classes_registered_ = false;

LRESULT CALLBACK Procedure(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
void ApplyOne(HWND window, bool root);
bool Bordered(HWND window);
void CloseDropdown(bool commit);
double HotAlpha(const Record& record, int item);
void Fill(HDC dc, const RECT& rectangle, COLORREF color);
int Scale(HWND window, int value) {
    const UINT dpi = GetDpiForWindow(window);
    return MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96);
}
Record* State(HWND window) {
    DWORD_PTR data = 0;
    return GetWindowSubclass(window, Procedure, kSubclass, &data) ? reinterpret_cast<Record*>(data) : nullptr;
}
Visual Sample(const Record& state, ULONGLONG now) {
    if (!state.timer) return state.to;
    const double t = UiMotion::EaseOutCubic(static_cast<double>(now - state.start) / static_cast<double>(kDuration));
    return {state.from.hover + (state.to.hover - state.from.hover) * t,
        state.from.press + (state.to.press - state.from.press) * t,
        state.from.focus + (state.to.focus - state.from.focus) * t,
        state.from.check + (state.to.check - state.from.check) * t};
}
Visual Current(HWND window) { const Record* state = State(window); return state ? Sample(*state, GetTickCount64()) : Visual{}; }
void Stop(Record& state, bool completed) {
    if (state.timer) { KillTimer(state.window, kAnimationTimer); state.timer = false; if (completed) ++completes_; else ++cancels_; }
    state.from = state.to;
}
bool MotionControl(Kind kind) {
    return kind == Kind::Button || kind == Kind::ComboBox || kind == Kind::Edit || kind == Kind::ListBox || kind == Kind::ListView;
}
void Target(Record& state) {
    if (!MotionControl(state.kind)) return;
    const bool enabled = IsWindowEnabled(state.window) != FALSE;
    const bool focus = GetFocus() == state.window || IsChild(state.window, GetFocus());
    const bool pressed = state.kind == Kind::Button && (SendMessageW(state.window, BM_GETSTATE, 0, 0) & BST_PUSHED);
    const bool checked = state.kind == Kind::Button && SendMessageW(state.window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    const Visual target{enabled && state.hover ? 1.0 : 0.0, enabled && pressed ? 1.0 : 0.0, enabled && focus ? 1.0 : 0.0, checked ? 1.0 : 0.0};
    if (target.hover == state.to.hover && target.press == state.to.press && target.focus == state.to.focus && target.check == state.to.check) return;
    const ULONGLONG now = GetTickCount64();
    const Visual from = Sample(state, now);
    if (state.timer) { ++cancels_; state.timer = false; KillTimer(state.window, kAnimationTimer); }
    state.from = from; state.to = target; state.start = now;
    if (!reduced_motion_ && !high_contrast_ && IsWindowVisible(state.window) && SetTimer(state.window, kAnimationTimer, 16, nullptr)) {
        state.timer = true; ++starts_;
    } else state.from = state.to;
    RedrawWindow(state.window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME);
}
int Percent(double value) { return static_cast<int>(std::round(std::clamp(value, 0.0, 1.0) * 100.0)); }
Gdiplus::Color Color(COLORREF value, BYTE alpha = 255) { return {alpha, GetRValue(value), GetGValue(value), GetBValue(value)}; }
void RoundedPath(Gdiplus::GraphicsPath& path, const Gdiplus::RectF& rect, float radius) {
    const float r = (std::min)(radius, (std::min)(rect.Width, rect.Height) / 2.0f), d = r * 2;
    if (d <= 0.01f) { path.AddRectangle(rect); return; }
    path.AddArc(rect.X, rect.Y, d, d, 180, 90); path.AddArc(rect.GetRight() - d, rect.Y, d, d, 270, 90);
    path.AddArc(rect.GetRight() - d, rect.GetBottom() - d, d, d, 0, 90); path.AddArc(rect.X, rect.GetBottom() - d, d, d, 90, 90); path.CloseFigure();
}
void Rounded(HDC dc, const RECT& rect, int radius, COLORREF face, COLORREF edge, double halo = 0, bool fill = true) {
    if (rect.right <= rect.left || rect.bottom <= rect.top) return;
    if (!graphics_token_) {
        HPEN pen = CreatePen(PS_SOLID, 1, edge); HBRUSH brush = CreateSolidBrush(face);
        HGDIOBJ old_pen = SelectObject(dc, pen), old_brush = SelectObject(dc, fill ? static_cast<HGDIOBJ>(brush) : GetStockObject(HOLLOW_BRUSH));
        RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius * 2, radius * 2);
        SelectObject(dc, old_pen); SelectObject(dc, old_brush); DeleteObject(pen); DeleteObject(brush); return;
    }
    Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    Gdiplus::GraphicsPath path;
    RoundedPath(path, {static_cast<float>(rect.left) + .5f, static_cast<float>(rect.top) + .5f,
        static_cast<float>(rect.right - rect.left) - 1, static_cast<float>(rect.bottom - rect.top) - 1}, static_cast<float>(radius));
    if (fill) { Gdiplus::SolidBrush brush(Color(face)); graphics.FillPath(&brush, &path); }
    if (halo > .001) {
        Gdiplus::Pen glow(Color(edge, static_cast<BYTE>(std::clamp(halo, 0.0, 1.0) * 40.0)), 3.0f); graphics.DrawPath(&glow, &path);
    }
    if (edge != face) { Gdiplus::Pen pen(Color(edge), 1.0f); graphics.DrawPath(&pen, &path); }
}
// Rounded fill without an outline, used for selection pills and accent bars.
void Pill(HDC dc, const RECT& rect, int radius, COLORREF face) { Rounded(dc, rect, radius, face, face); }
Role RoleOf(HWND window) { return static_cast<Role>(reinterpret_cast<INT_PTR>(GetPropW(window, kRoleProperty))); }
bool Primary(HWND window) {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    return RoleOf(window) == Role::Primary || (style & BS_TYPEMASK) == BS_DEFPUSHBUTTON;
}
// Paints the non-client band of a bordered control: padding added by
// WM_NCCALCSIZE, the rounded edge and a paint-only corner mask. Scroll bars
// keep their native (dark-themed) drawing.
void PaintControlFrame(HWND window) {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE), extended = GetWindowLongPtrW(window, GWL_EXSTYLE);
    if (!(style & WS_BORDER) && !(extended & WS_EX_CLIENTEDGE)) return;
    HDC dc = GetWindowDC(window); if (!dc) return;
    RECT screen{}, rectangle{}, client{}; GetWindowRect(window, &screen); GetClientRect(window, &client);
    MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2);
    rectangle = screen; OffsetRect(&client, -screen.left, -screen.top); OffsetRect(&rectangle, -screen.left, -screen.top);
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const COLORREF face = enabled ? colors_.panel : Mix(colors_.background, colors_.panel, 45);
    const int saved = SaveDC(dc);
    ExcludeClipRect(dc, client.left, client.top, client.right, client.bottom);
    for (const LONG object : {OBJID_VSCROLL, OBJID_HSCROLL}) {
        SCROLLBARINFO bar{sizeof(bar)};
        if (GetScrollBarInfo(window, object, &bar) && !(bar.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN))) {
            RECT area = bar.rcScrollBar; OffsetRect(&area, -screen.left, -screen.top);
            ExcludeClipRect(dc, area.left, area.top, area.right, area.bottom);
        }
    }
    Fill(dc, rectangle, face);
    RestoreDC(dc, saved);
    const Visual visual = Current(window); const int radius = (std::max)(4, Scale(window, 6));
    COLORREF edge = enabled ? Mix(colors_.border, colors_.blue, Percent(visual.hover * .7)) : Mix(colors_.border, colors_.background, 40);
    edge = Mix(edge, colors_.gold, Percent(visual.focus));
    if (graphics_token_) {
        Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias); Gdiplus::GraphicsPath path;
        const Gdiplus::RectF bounds(0, 0, static_cast<float>(rectangle.right), static_cast<float>(rectangle.bottom));
        RoundedPath(path, bounds, static_cast<float>(radius)); Gdiplus::Region corners(bounds); corners.Exclude(&path);
        Gdiplus::SolidBrush brush(Color(colors_.background)); graphics.FillRegion(&brush, &corners);
    }
    Rounded(dc, rectangle, radius, face, edge, (std::max)(visual.focus, visual.hover * .5), false); ReleaseDC(window, dc);
}
Kind Classify(HWND window, bool root) {
    wchar_t name[128]{}; GetClassNameW(window, name, static_cast<int>(std::size(name)));
    if (!_wcsicmp(name, L"Button")) return Kind::Button;
    if (!_wcsicmp(name, L"Edit")) return Kind::Edit;
    if (!_wcsicmp(name, L"ListBox")) return Kind::ListBox;
    if (!_wcsicmp(name, L"ComboBox")) return Kind::ComboBox;
    if (!_wcsicmp(name, WC_LISTVIEWW)) return Kind::ListView;
    if (!_wcsicmp(name, WC_HEADERW)) return Kind::Header;
    if (!_wcsicmp(name, L"Static")) return Kind::Static;
    if (!_wcsicmp(name, WC_TABCONTROLW)) return Kind::Tab;
    if (!_wcsicmp(name, TOOLTIPS_CLASSW)) return Kind::ToolTip;
    return root || !wcsncmp(name, L"PBZ", 3) ? Kind::Container : Kind::Other;
}
std::wstring Caption(HWND window) {
    const int size = GetWindowTextLengthW(window);
    if (size <= 0 || size > 32768) return {};
    std::wstring text(static_cast<size_t>(size) + 1, L'\0');
    GetWindowTextW(window, text.data(), size + 1); text.resize(static_cast<size_t>(size)); return text;
}
void Fill(HDC dc, const RECT& rectangle, COLORREF color) {
    SetDCBrushColor(dc, color); FillRect(dc, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
void Frame(HDC dc, const RECT& rectangle, COLORREF color) {
    SetDCBrushColor(dc, color); FrameRect(dc, &rectangle, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}
class FontScope {
public:
    FontScope(HDC dc, HWND window, HFONT forced = nullptr) : dc_(dc) {
        HFONT font = forced ? forced : reinterpret_cast<HFONT>(SendMessageW(window, WM_GETFONT, 0, 0));
        previous_ = SelectObject(dc_, font ? font : GetStockObject(DEFAULT_GUI_FONT));
    }
    ~FontScope() { SelectObject(dc_, previous_); }
private:
    HDC dc_; HGDIOBJ previous_;
};
// Paint into an off-screen bitmap and copy once, so custom-drawn controls never
// show a half-painted frame while they animate.
class Buffer {
public:
    Buffer(HDC target, const RECT& area) : target_(target), area_(area) {
        dc_ = CreateCompatibleDC(target);
        bitmap_ = dc_ ? CreateCompatibleBitmap(target, (std::max)(1L, area.right - area.left), (std::max)(1L, area.bottom - area.top)) : nullptr;
        if (bitmap_) { old_ = SelectObject(dc_, bitmap_); SetViewportOrgEx(dc_, -area.left, -area.top, nullptr); }
    }
    ~Buffer() {
        if (bitmap_) {
            SetViewportOrgEx(dc_, 0, 0, nullptr);
            BitBlt(target_, area_.left, area_.top, area_.right - area_.left, area_.bottom - area_.top, dc_, 0, 0, SRCCOPY);
            SelectObject(dc_, old_); DeleteObject(bitmap_);
        }
        if (dc_) DeleteDC(dc_);
    }
    HDC dc() const { return bitmap_ ? dc_ : target_; }
private:
    HDC target_, dc_ = nullptr; RECT area_; HBITMAP bitmap_ = nullptr; HGDIOBJ old_ = nullptr;
};
UINT CaptionFlags(HWND window) {
    // Pages define no mnemonics, so '&' in captions such as "Stop & Return" stays literal.
    UINT flags = DT_VCENTER | DT_NOPREFIX;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    flags |= (style & BS_MULTILINE) ? DT_WORDBREAK : DT_SINGLELINE | DT_END_ELLIPSIS;
    if (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_RTLREADING) flags |= DT_RTLREADING;
    return flags;
}
void DrawCheck(HDC dc, RECT box, bool radio, LRESULT state, bool enabled, const Visual& visual) {
    const double on = std::clamp(visual.check, 0.0, 1.0);
    const COLORREF accent = enabled ? colors_.gold : Mix(colors_.muted, colors_.background, 35);
    COLORREF face = Mix(colors_.panel, colors_.blue, Percent(visual.press * .18 + visual.hover * .10));
    face = Mix(face, accent, Percent(on));
    COLORREF edge = enabled ? Mix(colors_.border, colors_.blue, Percent(visual.hover)) : Mix(colors_.border, colors_.background, 30);
    edge = Mix(edge, accent, Percent((std::max)(on, visual.focus)));
    const int size = box.right - box.left;
    if (radio) {
        Rounded(dc, box, size / 2, colors_.panel, edge, (std::max)(visual.focus, visual.hover * .5));
        if (state == BST_CHECKED) {
            const int inset = static_cast<int>(std::lround(size / 2.0 - (size * .28) * (.4 + .6 * on)));
            RECT dot = box; InflateRect(&dot, -inset, -inset); Pill(dc, dot, size, accent);
        }
        return;
    }
    Rounded(dc, box, (std::max)(3, size / 4), face, edge, (std::max)(visual.focus, visual.hover * .5));
    if (state == BST_CHECKED && graphics_token_) {
        // The tick draws itself in as the fill fades to the accent colour.
        Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Color(colors_.base), (std::max)(1.6f, size / 8.0f)); pen.SetLineJoin(Gdiplus::LineJoinRound);
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound);
        const float x = static_cast<float>(box.left), y = static_cast<float>(box.top), s = static_cast<float>(size);
        const Gdiplus::PointF a(x + s * .24f, y + s * .52f), b(x + s * .43f, y + s * .70f), c(x + s * .76f, y + s * .32f);
        const float progress = static_cast<float>(UiMotion::EaseOutCubic(on));
        if (progress < .45f) {
            const float t = progress / .45f; graphics.DrawLine(&pen, a, Gdiplus::PointF(a.X + (b.X - a.X) * t, a.Y + (b.Y - a.Y) * t));
        } else {
            const float t = (progress - .45f) / .55f; graphics.DrawLine(&pen, a, b);
            graphics.DrawLine(&pen, b, Gdiplus::PointF(b.X + (c.X - b.X) * t, b.Y + (c.Y - b.Y) * t));
        }
    } else if (state == BST_CHECKED) {
        HPEN check_pen = CreatePen(PS_SOLID, 2, colors_.base); HGDIOBJ old = SelectObject(dc, check_pen);
        MoveToEx(dc, box.left + size / 5, box.top + size / 2, nullptr);
        LineTo(dc, box.left + size * 2 / 5, box.top + size * 3 / 4); LineTo(dc, box.left + size * 4 / 5, box.top + size / 4);
        SelectObject(dc, old); DeleteObject(check_pen);
    } else if (state == BST_INDETERMINATE) {
        const int inset = (std::max)(3, size / 4);
        InflateRect(&box, -inset, -inset); Pill(dc, box, 2, accent);
    }
}
bool ButtonSupported(HWND window) {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE), type = style & BS_TYPEMASK;
    if (high_contrast_ || (style & (BS_BITMAP | BS_ICON)) || type == BS_OWNERDRAW || type == BS_GROUPBOX ||
        type == BS_SPLITBUTTON || type == BS_DEFSPLITBUTTON || type == BS_COMMANDLINK || type == BS_DEFCOMMANDLINK) return false;
    return type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON || type == BS_CHECKBOX || type == BS_AUTOCHECKBOX ||
        type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON || type == BS_3STATE || type == BS_AUTO3STATE;
}
bool DrawButton(NMCUSTOMDRAW* draw) {
    HWND window = draw->hdr.hwndFrom;
    if (!ButtonSupported(window)) return false;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE), type = style & BS_TYPEMASK;
    if (draw->dwDrawStage != CDDS_PREPAINT) return false;
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const LRESULT button_state = SendMessageW(window, BM_GETSTATE, 0, 0);
    const bool pressed = (draw->uItemState & CDIS_SELECTED) || (button_state & BST_PUSHED);
    const bool focused = GetFocus() == window || (draw->uItemState & CDIS_FOCUS);
    const bool hot = (draw->uItemState & CDIS_HOT) || (button_state & BST_HOT);
    const bool check = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE;
    const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    const bool pushlike = (style & BS_PUSHLIKE) != 0;
    Visual visual = Current(window);
    const bool is_checked = SendMessageW(window, BM_GETCHECK, 0, 0) == BST_CHECKED;
    if (!State(window)) visual = {hot ? 1.0 : 0.0, pressed ? 1.0 : 0.0, focused ? 1.0 : 0.0, is_checked ? 1.0 : 0.0};
    Buffer buffer(draw->hdc, draw->rc); HDC dc = buffer.dc();
    RECT rectangle = draw->rc, text_rect = rectangle;
    FontScope font(dc, window); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? colors_.text : Mix(colors_.muted, colors_.background, 30));
    UINT flags = CaptionFlags(window);
    if ((check || radio) && !pushlike) {
        Fill(dc, rectangle, colors_.background);
        const int box_size = (std::max)(14, Scale(window, 18)), pad = (std::max)(2, Scale(window, 2));
        RECT box{rectangle.left + pad, rectangle.top + (rectangle.bottom - rectangle.top - box_size) / 2, 0, 0};
        box.right = box.left + box_size; box.bottom = box.top + box_size;
        const int spacing = Scale(window, 9);
        if (style & BS_LEFTTEXT) {
            box.left = rectangle.right - pad - box_size; box.right = rectangle.right - pad;
            text_rect.right = box.left - spacing;
        } else text_rect.left = box.right + spacing;
        DrawCheck(dc, box, radio, SendMessageW(window, BM_GETCHECK, 0, 0), enabled, visual);
        if (enabled) SetTextColor(dc, Mix(colors_.text, colors_.muted, Percent((1.0 - visual.hover) * .18)));
        flags |= DT_LEFT;
    } else {
        const bool checked = pushlike && SendMessageW(window, BM_GETCHECK, 0, 0) != BST_UNCHECKED;
        const bool primary = Primary(window) && enabled;
        Fill(dc, rectangle, colors_.background);
        COLORREF face = enabled ? Mix(colors_.panel, colors_.background, 10) : Mix(colors_.background, colors_.panel, 40);
        COLORREF border = enabled ? colors_.border : Mix(colors_.border, colors_.background, 45);
        if (primary) {
            SetTextColor(dc, RGB(0x14, 0x12, 0x10));
        } else if (enabled) {
            face = Mix(face, colors_.blue, Percent(visual.hover * .10 + (checked ? .16 : visual.press * .14)));
            border = Mix(border, colors_.blue, Percent(visual.hover * .75));
        }
        border = primary ? border : Mix(border, colors_.gold, Percent(visual.focus));
        RECT body = rectangle;
        // Pressing sinks the face by one pixel; the hit rectangle never changes.
        if (visual.press > .5 && enabled) OffsetRect(&body, 0, 1), OffsetRect(&text_rect, 0, 1);
        if (primary) {
            // The game's selected-row brush stroke; hover brightens, press dims.
            Gdiplus::Graphics graphics(dc);
            UiAssets::DrawBar(graphics, UiAssets::Id::SelectedBar, Gdiplus::RectF(static_cast<float>(body.left), static_cast<float>(body.top),
                static_cast<float>(body.right - body.left), static_cast<float>(body.bottom - body.top)),
                static_cast<float>(.86 + .14 * visual.hover - .18 * visual.press));
            if (visual.focus > .01) Rounded(dc, body, (std::max)(5, Scale(window, 6)), colors_.background, Mix(colors_.background, colors_.gold, Percent(visual.focus)), 0, false);
        } else Rounded(dc, body, (std::max)(5, Scale(window, 8)), face, border, (std::max)(visual.focus, visual.hover * .55));
        InflateRect(&text_rect, -Scale(window, 8), -1); flags |= DT_CENTER;
    }
    const std::wstring text = Caption(window); DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &text_rect, flags);
    if (focused && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS) && ((check || radio) && !pushlike)) {
        RECT focus = text_rect; DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &focus, flags | DT_CALCRECT);
        focus.top = rectangle.top + 2; focus.bottom = rectangle.bottom - 2; InflateRect(&focus, 3, 0);
        Rounded(dc, focus, Scale(window, 4), colors_.background, Mix(colors_.gold, colors_.background, 45), 0, false);
    }
    return true;
}
bool DrawHeader(NMCUSTOMDRAW* draw) {
    if (high_contrast_ || draw->dwDrawStage != CDDS_ITEMPREPAINT) return false;
    wchar_t text[2048]{}; HDITEMW item{};
    item.mask = HDI_TEXT | HDI_FORMAT; item.pszText = text; item.cchTextMax = static_cast<int>(std::size(text));
    if (!Header_GetItem(draw->hdr.hwndFrom, static_cast<int>(draw->dwItemSpec), &item) ||
        (item.fmt & (HDF_BITMAP | HDF_IMAGE | HDF_CHECKBOX))) return false;
    HDC dc = draw->hdc; RECT rectangle = draw->rc, text_rect = rectangle;
    Fill(dc, rectangle, draw->uItemState & CDIS_HOT ? Mix(colors_.panel, colors_.blue, 10) : colors_.panel);
    RECT line{rectangle.left, rectangle.bottom - 1, rectangle.right, rectangle.bottom}; Fill(dc, line, colors_.border);
    RECT divider{rectangle.right - 1, rectangle.top + Scale(draw->hdr.hwndFrom, 7), rectangle.right, rectangle.bottom - Scale(draw->hdr.hwndFrom, 7)};
    Fill(dc, divider, colors_.border);
    FontScope font(dc, draw->hdr.hwndFrom); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, colors_.muted);
    InflateRect(&text_rect, -Scale(draw->hdr.hwndFrom, 10), 0);
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
    flags |= (item.fmt & HDF_CENTER) ? DT_CENTER : ((item.fmt & HDF_RIGHT) ? DT_RIGHT : DT_LEFT);
    if (item.fmt & (HDF_SORTUP | HDF_SORTDOWN)) {
        const int edge = Scale(draw->hdr.hwndFrom, 5), center = rectangle.top + (rectangle.bottom - rectangle.top) / 2;
        text_rect.right -= edge * 3;
        const int x = rectangle.right - edge * 2;
        POINT points[3]{{x - edge, center}, {x + edge, center}, {x, center + ((item.fmt & HDF_SORTUP) ? -edge : edge)}};
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH)), old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, colors_.gold); SetDCPenColor(dc, colors_.gold); Polygon(dc, points, 3);
        SelectObject(dc, old_brush); SelectObject(dc, old_pen);
    }
    DrawTextW(dc, text, -1, &text_rect, flags); return true;
}
// Every report cell is painted here: zebra rows, hover fade, selection with an
// accent bar and a hairline separator. The list view keeps its own behaviour.
bool DrawCell(NMLVCUSTOMDRAW* draw) {
    HWND window = draw->nmcd.hdr.hwndFrom;
    if ((GetWindowLongPtrW(window, GWL_STYLE) & LVS_TYPEMASK) != LVS_REPORT ||
        (ListView_GetExtendedListViewStyle(window) & LVS_EX_CHECKBOXES) || ListView_GetImageList(window, LVSIL_STATE) ||
        (ListView_GetImageList(window, LVSIL_SMALL) && !GetPropW(window, kSpacerProperty))) return false;
    const int row = static_cast<int>(draw->nmcd.dwItemSpec), column = draw->iSubItem;
    RECT rectangle{}, header_rectangle{}; HWND header = ListView_GetHeader(window);
    if (!ListView_GetItemRect(window, row, &rectangle, LVIR_BOUNDS) || !Header_GetItemRect(header, column, &header_rectangle)) return false;
    MapWindowPoints(header, window, reinterpret_cast<POINT*>(&header_rectangle), 2);
    rectangle.left = header_rectangle.left; rectangle.right = header_rectangle.right;
    HDC dc = draw->nmcd.hdc;
    const bool selected = (ListView_GetItemState(window, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    const Record* record = State(window);
    const double hover = record ? HotAlpha(*record, row) : 0.0;
    COLORREF face = (row & 1) ? Mix(colors_.panel, colors_.background, 45) : colors_.panel;
    face = selected ? colors_.selection : Mix(face, colors_.blue, static_cast<int>(std::lround(10 * hover)));
    Fill(dc, rectangle, face);
    RECT line{rectangle.left, rectangle.bottom - 1, rectangle.right, rectangle.bottom}; Fill(dc, line, Mix(face, colors_.border, 45));
    if (selected && column == 0) { RECT bar{rectangle.left, rectangle.top + Scale(window, 5), rectangle.left + Scale(window, 3), rectangle.bottom - Scale(window, 5)}; Pill(dc, bar, 2, colors_.gold); }
    wchar_t text[2048]{}; ListView_GetItemText(window, row, column, text, static_cast<int>(std::size(text)));
    LVCOLUMNW info{}; info.mask = LVCF_FMT; ListView_GetColumn(window, column, &info);
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX;
    flags |= (info.fmt & LVCFMT_CENTER) ? DT_CENTER : ((info.fmt & LVCFMT_RIGHT) ? DT_RIGHT : DT_LEFT);
    FontScope font(dc, window); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, !IsWindowEnabled(window) ? colors_.muted : selected || hover > .5 ? colors_.text : Mix(colors_.text, colors_.muted, 18));
    InflateRect(&rectangle, -Scale(window, 10), 0); DrawTextW(dc, text, -1, &rectangle, flags); return true;
}
LRESULT CustomDraw(NMHDR* header, bool* handled) {
    *handled = false;
    if (!header || header->code != NM_CUSTOMDRAW || high_contrast_) return 0;
    auto* draw = reinterpret_cast<NMCUSTOMDRAW*>(header);
    const Kind kind = Classify(header->hwndFrom, false);
    if (kind == Kind::Button && DrawButton(draw)) { *handled = true; return CDRF_SKIPDEFAULT; }
    if (kind == Kind::Header) {
        if (draw->dwDrawStage == CDDS_PREPAINT) { *handled = true; return CDRF_NOTIFYITEMDRAW; }
        if (DrawHeader(draw)) { *handled = true; return CDRF_SKIPDEFAULT; }
    }
    if (kind == Kind::ListView) {
        auto* list = reinterpret_cast<NMLVCUSTOMDRAW*>(draw);
        if (draw->dwDrawStage == CDDS_PREPAINT) { *handled = true; return CDRF_NOTIFYITEMDRAW; }
        if (draw->dwDrawStage == CDDS_ITEMPREPAINT || draw->dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM)) {
            const bool selected = ListView_GetItemState(header->hwndFrom, static_cast<int>(draw->dwItemSpec), LVIS_SELECTED) != 0;
            list->clrText = colors_.text;
            list->clrTextBk = selected ? colors_.selection :
                ((draw->dwItemSpec & 1) ? Mix(colors_.panel, colors_.background, 55) : colors_.panel);
            *handled = true;
            if (draw->dwDrawStage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM) && DrawCell(list)) return CDRF_SKIPDEFAULT;
            return draw->dwDrawStage == CDDS_ITEMPREPAINT ? CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT : CDRF_NEWFONT;
        }
        if (draw->dwDrawStage == CDDS_ITEMPOSTPAINT && GetFocus() == header->hwndFrom &&
            (ListView_GetItemState(header->hwndFrom, static_cast<int>(draw->dwItemSpec), LVIS_FOCUSED) & LVIS_FOCUSED)) {
            RECT rectangle{};
            if (ListView_GetItemRect(header->hwndFrom, static_cast<int>(draw->dwItemSpec), &rectangle, LVIR_BOUNDS)) Frame(draw->hdc, rectangle, Mix(colors_.gold, colors_.panel, 35));
        }
    }
    return 0;
}
void GroupBox(HWND window, HDC dc) {
    // Group boxes overlap their sibling inputs. Fill only the parts no sibling
    // covers, so the interior is painted without overpainting those controls.
    RECT rectangle{}; GetClientRect(window, &rectangle);
    const int saved = SaveDC(dc);
    if (HWND parent = GetParent(window)) {
        for (HWND sibling = GetWindow(parent, GW_CHILD); sibling; sibling = GetWindow(sibling, GW_HWNDNEXT)) {
            if (sibling == window || !IsWindowVisible(sibling)) continue;
            RECT area{}; GetWindowRect(sibling, &area); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&area), 2);
            ExcludeClipRect(dc, area.left, area.top, area.right, area.bottom);
        }
    }
    Fill(dc, rectangle, colors_.background);
    RestoreDC(dc, saved);
    FontScope font(dc, window); TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    RECT frame = rectangle; frame.top += metrics.tmHeight / 2; frame.bottom -= 1;
    Rounded(dc, frame, (std::max)(5, Scale(window, 10)), colors_.background, colors_.border, 0, false);
    const std::wstring text = Caption(window); SIZE size{};
    GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &size);
    RECT title{Scale(window, 10), 0, (std::min)(rectangle.right - 2, Scale(window, 18) + size.cx), metrics.tmHeight + 2};
    Fill(dc, title, colors_.background); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, IsWindowEnabled(window) ? colors_.muted : Mix(colors_.muted, colors_.background, 25));
    title.left += Scale(window, 4);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &title, DT_SINGLELINE | DT_LEFT | DT_VCENTER | DT_END_ELLIPSIS);
}
// Section heading: accent bar, semibold caption and a hairline to the right edge.
void SectionStatic(HWND window, HDC target) {
    RECT rectangle{}; GetClientRect(window, &rectangle);
    Buffer buffer(target, rectangle); HDC dc = buffer.dc();
    Fill(dc, rectangle, colors_.background);
    const std::wstring text = Caption(window);
    FontScope font(dc, window, FontFor(Font::Heading, window, text)); SetBkMode(dc, TRANSPARENT);
    const int middle = (rectangle.top + rectangle.bottom) / 2, mark = Scale(window, 12);
    // The game's pale-gold menu arrow, then the caption and a fading ink line.
    RECT arrow{rectangle.left, middle - mark / 2, rectangle.left + mark, middle - mark / 2 + mark};
    {
        Gdiplus::Graphics graphics(dc);
        UiAssets::Draw(graphics, UiAssets::Id::GoldArrow, Gdiplus::RectF(static_cast<float>(arrow.left), static_cast<float>(arrow.top),
            static_cast<float>(mark), static_cast<float>(mark)), IsWindowEnabled(window) ? 1.f : .45f);
    }
    RECT label{arrow.right + Scale(window, 8), rectangle.top, rectangle.right, rectangle.bottom};
    SetTextColor(dc, IsWindowEnabled(window) ? colors_.text : colors_.muted);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &label, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT measured = label; DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &measured, DT_SINGLELINE | DT_CALCRECT | DT_NOPREFIX);
    const int start = measured.right + Scale(window, 14);
    if (start < rectangle.right) {
        const int line = Scale(window, 10);
        Gdiplus::Graphics graphics(dc);
        UiAssets::Draw(graphics, UiAssets::Id::Line, Gdiplus::RectF(static_cast<float>(start), static_cast<float>(middle - line / 2),
            static_cast<float>(rectangle.right - start), static_cast<float>(line)), .55f);
    }
}
// Card statics show live status text inside a rounded, slightly raised panel.
void CardStatic(HWND window, HDC target) {
    RECT rectangle{}; GetClientRect(window, &rectangle);
    Buffer buffer(target, rectangle); HDC dc = buffer.dc();
    Fill(dc, rectangle, colors_.background);
    Rounded(dc, rectangle, Scale(window, 8), Mix(colors_.background, colors_.panel, 70), Mix(colors_.border, colors_.background, 30));
    RECT accent{rectangle.left + 1, rectangle.top + Scale(window, 10), rectangle.left + 1 + Scale(window, 2), rectangle.bottom - Scale(window, 10)};
    if (accent.bottom > accent.top) Pill(dc, accent, 1, Mix(colors_.gold, colors_.background, 30));
    FontScope font(dc, window); SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, IsWindowEnabled(window) ? colors_.text : colors_.muted);
    RECT text = rectangle; InflateRect(&text, -Scale(window, 12), -Scale(window, 8));
    const std::wstring caption = Caption(window);
    DrawTextW(dc, caption.c_str(), static_cast<int>(caption.size()), &text, DT_WORDBREAK | DT_EXPANDTABS | DT_NOPREFIX | DT_EDITCONTROL);
}
// Hovered rows fade in, the previously hovered row fades out.
double HotAlpha(const Record& record, int item) {
    const double t = record.hot_fade.At(GetTickCount64());
    return item == record.hot_item ? t : item == record.hot_previous ? 1.0 - t : 0.0;
}
void InvalidateItem(Record& record, int item) {
    if (item < 0) return;
    RECT area{};
    if (record.kind == Kind::ListBox) {
        if (SendMessageW(record.window, LB_GETITEMRECT, static_cast<WPARAM>(item), reinterpret_cast<LPARAM>(&area)) == LB_ERR) return;
    } else if (!ListView_GetItemRect(record.window, item, &area, LVIR_BOUNDS)) return;
    InvalidateRect(record.window, &area, FALSE);
}
void SetHot(Record& record, int item) {
    if (item == record.hot_item) return;
    InvalidateItem(record, record.hot_previous);
    record.hot_previous = record.hot_item; record.hot_item = item;
    record.hot_fade.from = 0; record.hot_fade.to = 1; record.hot_fade.start = GetTickCount64();
    record.hot_fade.duration = reduced_motion_ ? 0 : 150;
    InvalidateItem(record, record.hot_previous); InvalidateItem(record, record.hot_item);
    if (record.hot_fade.duration && !record.hot_timer) record.hot_timer = SetTimer(record.window, kHotTimer, 16, nullptr) != 0;
}
COLORREF ControlFace(HWND control) {
    const Kind kind = Classify(control, false);
    return Bordered(control) || kind == Kind::ListBox || kind == Kind::ListView ? colors_.panel : colors_.background;
}
struct ThumbGeometry { int start = 0, length = 0, track = 0, inset = 0, minimum = 0, maximum = 0; bool active = false; };
ThumbGeometry SkinThumb(const Skin& skin) {
    ThumbGeometry g; RECT client{}; GetClientRect(skin.window, &client);
    g.inset = Scale(skin.window, 3);
    g.track = (std::max)(0, static_cast<int>(skin.bar == SB_VERT ? client.bottom : client.right) - 2 * g.inset);
    const SCROLLINFO& info = skin.info; const int page = static_cast<int>(info.nPage), range = info.nMax - info.nMin + 1;
    g.minimum = info.nMin; g.maximum = info.nMax - (std::max)(page - 1, 0);
    g.active = skin.usable && range > 0 && g.maximum > g.minimum && g.track > 0;
    if (!g.active) return g;
    g.length = std::clamp(page > 0 ? MulDiv(g.track, page, range) : g.track / 8, (std::min)(g.track, Scale(skin.window, 28)), g.track);
    g.start = MulDiv(g.track - g.length, std::clamp(info.nPos, g.minimum, g.maximum) - g.minimum, g.maximum - g.minimum);
    return g;
}
void PaintSkin(Skin& skin, HDC target) {
    RECT client{}; GetClientRect(skin.window, &client);
    Buffer buffer(target, client); HDC dc = buffer.dc();
    const COLORREF face = ControlFace(skin.control);
    Fill(dc, client, face);
    const ThumbGeometry g = SkinThumb(skin); if (!g.active) return;
    const double grow = skin.grow.At(GetTickCount64());
    const bool vertical = skin.bar == SB_VERT;
    const int across = vertical ? client.right : client.bottom;
    const int thickness = static_cast<int>(std::lround(Scale(skin.window, 4) + (Scale(skin.window, 8) - Scale(skin.window, 4)) * grow));
    const int offset = (across - thickness) / 2;
    if (grow > .01) {
        const RECT track = vertical ? RECT{offset, g.inset, offset + thickness, g.inset + g.track} : RECT{g.inset, offset, g.inset + g.track, offset + thickness};
        Pill(dc, track, thickness / 2, Mix(face, colors_.border, static_cast<int>(std::lround(55 * grow))));
    }
    const int start = g.inset + g.start;
    const RECT thumb = vertical ? RECT{offset, start, offset + thickness, start + g.length} : RECT{start, offset, start + g.length, offset + thickness};
    const COLORREF color = Mix(Mix(colors_.border, colors_.muted, static_cast<int>(std::lround(30 + 50 * grow))), colors_.gold, skin.drag ? 55 : 0);
    Pill(dc, thumb, thickness / 2, color);
}
void ScrollControlTo(Skin& skin, int target) {
    const UINT message = skin.bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL;
    int position = GetScrollPos(skin.control, skin.bar);
    if (position != target) {
        SendMessageW(skin.control, message, MAKEWPARAM(SB_THUMBPOSITION, static_cast<WORD>(std::clamp(target, 0, 0xffff))), 0);
        position = GetScrollPos(skin.control, skin.bar);
    }
    // Controls that read the native track position ignore SB_THUMBPOSITION; step
    // them by lines instead, stopping when the position no longer moves.
    for (int guard = 0; position != target && guard < 4096; ++guard) {
        SendMessageW(skin.control, message, position < target ? SB_LINEDOWN : SB_LINEUP, 0);
        const int next = GetScrollPos(skin.control, skin.bar);
        if (next == position || (position < target) != (next < target)) { position = next; break; }
        position = next;
    }
}
void SyncSkins(Record& record);
LRESULT CALLBACK SkinProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    Skin* skin = reinterpret_cast<Skin*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (!skin) return DefWindowProcW(window, message, wparam, lparam);
    const auto refresh = [&]() { if (Record* record = State(skin->control)) SyncSkins(*record); InvalidateRect(window, nullptr, FALSE); };
    const auto animate = [&](double target, DWORD duration) {
        skin->grow.Set(target, duration, GetTickCount64(), reduced_motion_);
        if (!skin->timer) skin->timer = SetTimer(window, kAnimationTimer, 16, nullptr) != 0;
        InvalidateRect(window, nullptr, FALSE);
    };
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: { PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); PaintSkin(*skin, dc); EndPaint(window, &paint); return 0; }
    case WM_MOUSEMOVE: {
        if (!skin->hot) { skin->hot = true; animate(1, 160); TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track); }
        if (skin->drag) {
            const ThumbGeometry g = SkinThumb(*skin);
            const int at = (skin->bar == SB_VERT ? GET_Y_LPARAM(lparam) : GET_X_LPARAM(lparam)) - g.inset - skin->grab;
            const int free = (std::max)(1, g.track - g.length);
            ScrollControlTo(*skin, g.minimum + MulDiv(std::clamp(at, 0, free), g.maximum - g.minimum, free));
            refresh();
        }
        return 0;
    }
    case WM_MOUSELEAVE: skin->hot = false; if (!skin->drag) animate(0, 260); return 0;
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK: {
        const ThumbGeometry g = SkinThumb(*skin); if (!g.active) return 0;
        const int at = (skin->bar == SB_VERT ? GET_Y_LPARAM(lparam) : GET_X_LPARAM(lparam)) - g.inset;
        if (at >= g.start && at < g.start + g.length) { skin->drag = true; skin->grab = at - g.start; SetCapture(window); }
        else SendMessageW(skin->control, skin->bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, at < g.start ? SB_PAGEUP : SB_PAGEDOWN, 0);
        refresh(); return 0;
    }
    case WM_LBUTTONUP: case WM_CAPTURECHANGED:
        if (skin->drag) {
            skin->drag = false; if (GetCapture() == window) ReleaseCapture();
            SendMessageW(skin->control, skin->bar == SB_VERT ? WM_VSCROLL : WM_HSCROLL, SB_ENDSCROLL, 0);
            if (!skin->hot) animate(0, 260); refresh();
        }
        return 0;
    case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL: SendMessageW(skin->control, message, wparam, lparam); refresh(); return 0;
    case WM_TIMER:
        if (wparam == kAnimationTimer) {
            InvalidateRect(window, nullptr, FALSE);
            if (!skin->grow.Running(GetTickCount64())) { KillTimer(window, kAnimationTimer); skin->timer = false; }
        }
        return 0;
    case WM_NCDESTROY:
        if (skin->slot) *skin->slot = nullptr;
        SetWindowLongPtrW(window, GWLP_USERDATA, 0); delete skin; return DefWindowProcW(window, message, wparam, lparam);
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}
void SyncSkins(Record& record) {
    if (!record.scrollable || high_contrast_) return;
    const HWND parent = GetParent(record.window); if (!parent) return;
    for (int axis = 0; axis < 2; ++axis) {
        SCROLLBARINFO bar{sizeof(bar)};
        const bool shown = IsWindowVisible(record.window) && GetScrollBarInfo(record.window, axis == 0 ? OBJID_VSCROLL : OBJID_HSCROLL, &bar) &&
            !(bar.rgstate[0] & (STATE_SYSTEM_INVISIBLE | STATE_SYSTEM_OFFSCREEN)) && bar.rcScrollBar.right > bar.rcScrollBar.left && bar.rcScrollBar.bottom > bar.rcScrollBar.top;
        Skin*& skin = record.skins[axis];
        if (!shown) { if (skin && IsWindowVisible(skin->window)) ShowWindow(skin->window, SW_HIDE); continue; }
        if (!skin) {
            auto* created = new (std::nothrow) Skin; if (!created) continue;
            created->control = record.window; created->bar = axis == 0 ? SB_VERT : SB_HORZ;
            created->window = CreateWindowExW(WS_EX_NOPARENTNOTIFY, kSkinClass, L"", WS_CHILD | WS_CLIPSIBLINGS, 0, 0, 1, 1, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
            if (!created->window) { delete created; continue; }
            created->slot = &skin; skin = created; SetWindowLongPtrW(created->window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        }
        RECT area = bar.rcScrollBar, current{}; MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&area), 2);
        GetWindowRect(skin->window, &current); MapWindowPoints(nullptr, parent, reinterpret_cast<POINT*>(&current), 2);
        const bool moved = !EqualRect(&area, &current) || !IsWindowVisible(skin->window);
        if (moved) SetWindowPos(skin->window, HWND_TOP, area.left, area.top, area.right - area.left, area.bottom - area.top, SWP_NOACTIVATE | SWP_SHOWWINDOW);
        SCROLLINFO info{sizeof(info), SIF_ALL}; GetScrollInfo(record.window, skin->bar, &info);
        const bool usable = !(bar.rgstate[0] & STATE_SYSTEM_UNAVAILABLE) && IsWindowEnabled(record.window);
        if (moved || usable != skin->usable || info.nMin != skin->info.nMin || info.nMax != skin->info.nMax || info.nPage != skin->info.nPage || info.nPos != skin->info.nPos) {
            skin->info = info; skin->usable = usable; InvalidateRect(skin->window, nullptr, FALSE);
        }
    }
}
void DestroySkins(Record& record) {
    for (Skin*& skin : record.skins) if (skin) { Skin* owned = skin; skin = nullptr; owned->slot = nullptr; DestroyWindow(owned->window); }
}
bool SkinTrigger(UINT message) {
    return message == WM_PAINT || message == WM_NCPAINT || message == WM_WINDOWPOSCHANGED || message == WM_SHOWWINDOW ||
        message == WM_VSCROLL || message == WM_HSCROLL || message == WM_MOUSEWHEEL || message == WM_MOUSEHWHEEL || message == WM_KEYDOWN ||
        message == WM_SETTEXT || message == WM_ENABLE || message == WM_SETREDRAW || (message >= EM_GETSEL && message <= EM_GETIMESTATUS) ||
        (message >= LB_ADDSTRING && message <= LB_MSGMAX) || (message >= LVM_FIRST && message < LVM_FIRST + 0x100);
}

// ---- custom drop-down list for CBS_DROPDOWNLIST combo boxes -------------------
int DropdownMax() { return (std::max)(0, static_cast<int>(dropdown_.items.size()) * dropdown_.item - dropdown_.visible * dropdown_.item); }
RECT DropdownBody() {
    return {dropdown_.margin, dropdown_.margin, dropdown_.margin + dropdown_.width, dropdown_.margin + dropdown_.height};
}
void RenderDropdown() {
    Dropdown& d = dropdown_;
    if (!d.surface.Ensure(d.width + 2 * d.margin, d.height + 2 * d.margin)) return;
    d.surface.Clear(); HDC dc = d.surface.dc;
    const RECT body = DropdownBody();
    const COLORREF face = Mix(colors_.panel, colors_.text, 3);
    Fill(dc, body, face);
    const int saved = SaveDC(dc);
    IntersectClipRect(dc, body.left, body.top + d.pad / 2, body.right, body.bottom - d.pad / 2);
    FontScope font(dc, d.combo); SetBkMode(dc, TRANSPARENT);
    const int scroll = static_cast<int>(std::lround(d.scroll)), radius = Scale(d.combo, 7), inset = Scale(d.combo, 5);
    const double fade = d.hot_fade.At(GetTickCount64());
    for (int i = 0; i < static_cast<int>(d.items.size()); ++i) {
        RECT row{body.left + inset, body.top + d.pad + i * d.item - scroll, body.right - inset, body.top + d.pad + (i + 1) * d.item - scroll};
        if (row.bottom < body.top || row.top > body.bottom) continue;
        const double hover = i == d.hot ? fade : i == d.hot_previous ? 1.0 - fade : 0.0;
        if (i == d.selected) {
            Pill(dc, row, radius, Mix(colors_.selection, colors_.blue, static_cast<int>(std::lround(10 * hover))));
            RECT bar{row.left, row.top + Scale(d.combo, 8), row.left + Scale(d.combo, 3), row.bottom - Scale(d.combo, 8)}; Pill(dc, bar, 2, colors_.gold);
        } else if (hover > .01) Pill(dc, row, radius, Mix(face, colors_.blue, static_cast<int>(std::lround(14 * hover))));
        SetTextColor(dc, i == d.selected ? colors_.text : Mix(colors_.muted, colors_.text, static_cast<int>(std::lround(40 + 60 * hover))));
        RECT text = row; text.left += Scale(d.combo, 14); text.right -= Scale(d.combo, 10);
        DrawTextW(dc, d.items[static_cast<size_t>(i)].c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    RestoreDC(dc, saved);
    const int maximum = DropdownMax();
    if (maximum > 0) {
        const int track = d.height - 2 * d.pad, length = (std::max)(Scale(d.combo, 24), MulDiv(track, d.visible, static_cast<int>(d.items.size())));
        const int top = body.top + d.pad + MulDiv(track - length, scroll, maximum), x = body.right - Scale(d.combo, 6);
        RECT thumb{x - Scale(d.combo, 3), top, x, top + length}; Pill(dc, thumb, 2, Mix(colors_.border, colors_.muted, 40));
    }
    d.surface.Shape(body, Scale(d.combo, 10), Mix(colors_.border, colors_.muted, 15), d.margin, .42);
}
void PresentDropdown() {
    Dropdown& d = dropdown_; const ULONGLONG now = GetTickCount64();
    const POINT origin{d.anchor.left - d.margin, (d.above ? d.anchor.top - Scale(d.combo, 4) - d.height : d.anchor.bottom + Scale(d.combo, 4)) - d.margin +
        static_cast<int>(std::lround(d.slide.At(now)))};
    d.surface.Present(d.popup, origin, static_cast<BYTE>(std::clamp(std::lround(d.fade.At(now) * 255), 0L, 255L)));
}
void EnsureVisible(int index) {
    Dropdown& d = dropdown_; if (index < 0) return;
    const int top = index * d.item, bottom = top + d.item, view = d.visible * d.item;
    if (top < d.scroll_target) d.scroll_target = top; else if (bottom > d.scroll_target + view) d.scroll_target = bottom - view;
    d.scroll_target = std::clamp(d.scroll_target, 0, DropdownMax());
}
void DropdownHot(int index) {
    Dropdown& d = dropdown_;
    if (index == d.hot) return;
    d.hot_previous = d.hot; d.hot = index;
    d.hot_fade.from = 0; d.hot_fade.to = 1; d.hot_fade.start = GetTickCount64(); d.hot_fade.duration = reduced_motion_ ? 0 : 120;
}
void OpenDropdown(HWND combo) {
    Dropdown& d = dropdown_;
    if (d.open) CloseDropdown(false);
    const LRESULT count = SendMessageW(combo, CB_GETCOUNT, 0, 0); if (count <= 0) return;
    d.items.clear();
    for (LRESULT i = 0; i < count; ++i) {
        const LRESULT length = SendMessageW(combo, CB_GETLBTEXTLEN, static_cast<WPARAM>(i), 0);
        std::wstring text(static_cast<size_t>((std::max)(static_cast<LRESULT>(0), length)) + 1, L'\0');
        if (length > 0) SendMessageW(combo, CB_GETLBTEXT, static_cast<WPARAM>(i), reinterpret_cast<LPARAM>(text.data()));
        text.resize(static_cast<size_t>((std::max)(static_cast<LRESULT>(0), length))); d.items.push_back(std::move(text));
    }
    d.combo = combo; d.item = Scale(combo, 34); d.pad = Scale(combo, 6); d.margin = Scale(combo, 16);
    d.visible = (std::min)(static_cast<int>(d.items.size()), 9);
    GetWindowRect(combo, &d.anchor);
    COMBOBOXINFO info{sizeof(info)}; if (GetComboBoxInfo(combo, &info)) { MapWindowPoints(combo, nullptr, reinterpret_cast<POINT*>(&info.rcItem), 2); }
    d.width = d.anchor.right - d.anchor.left; d.height = d.visible * d.item + 2 * d.pad;
    MONITORINFO monitor{sizeof(monitor)}; GetMonitorInfoW(MonitorFromWindow(combo, MONITOR_DEFAULTTONEAREST), &monitor);
    d.above = d.anchor.bottom + Scale(combo, 4) + d.height > monitor.rcWork.bottom && d.anchor.top - d.height > monitor.rcWork.top;
    d.selected = static_cast<int>(SendMessageW(combo, CB_GETCURSEL, 0, 0)); d.hot = d.hot_previous = -1; DropdownHot(d.selected); d.hot_fade.Jump(1);
    d.scroll_target = 0; EnsureVisible(d.selected); d.scroll = d.scroll_target;
    if (!d.popup) d.popup = CreateWindowExW(WS_EX_LAYERED | WS_EX_NOACTIVATE, kDropdownClass, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!d.popup) return;
    UiHost::Attach(d.popup);
    SetWindowPos(d.popup, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const ULONGLONG now = GetTickCount64();
    d.fade.from = 0; d.fade.to = 1; d.fade.start = now; d.fade.duration = reduced_motion_ ? 0 : 150; d.fade.ease = UiMotion::EaseOutCubic;
    d.slide.from = static_cast<double>(Scale(combo, d.above ? 8 : -8)); d.slide.to = 0; d.slide.start = now; d.slide.duration = reduced_motion_ ? 0 : 240;
    d.slide.ease = UiMotion::EaseOutQuint;
    d.open = true; d.closing = false;
    RenderDropdown(); PresentDropdown(); ShowWindow(d.popup, SW_SHOWNOACTIVATE);
    SetCapture(d.popup); SetTimer(d.popup, kPopupTimer, 16, nullptr);
    InvalidateRect(combo, nullptr, FALSE);
}
void CloseDropdown(bool commit) {
    Dropdown& d = dropdown_; if (!d.open || d.closing) return;
    d.closing = true;
    const ULONGLONG now = GetTickCount64();
    d.fade.Set(0, 110, now, reduced_motion_); d.fade.ease = UiMotion::EaseInCubic;
    d.slide.Set(static_cast<double>(Scale(d.combo, d.above ? 4 : -4)), 110, now, reduced_motion_);
    if (GetCapture() == d.popup) ReleaseCapture();
    const HWND combo = d.combo;
    if (commit && d.hot >= 0 && d.hot != d.selected && IsWindow(combo)) {
        SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(d.hot), 0);
        SendMessageW(GetParent(combo), WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(combo), CBN_SELCHANGE), reinterpret_cast<LPARAM>(combo));
    }
    if (IsWindow(combo)) InvalidateRect(combo, nullptr, FALSE);
}
int DropdownHit(LPARAM lparam, bool* inside) {
    const Dropdown& d = dropdown_; const POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
    const RECT body = DropdownBody(); *inside = PtInRect(&body, point) != FALSE;
    if (!*inside) return -1;
    const int index = (point.y - body.top - d.pad + static_cast<int>(std::lround(d.scroll))) / (std::max)(1, d.item);
    return index >= 0 && index < static_cast<int>(d.items.size()) && point.y >= body.top + d.pad && point.y < body.bottom - d.pad ? index : -1;
}
void DropdownKey(WPARAM key) {
    Dropdown& d = dropdown_; const int last = static_cast<int>(d.items.size()) - 1;
    int index = d.hot < 0 ? d.selected : d.hot;
    if (key == VK_UP) --index; else if (key == VK_DOWN) ++index;
    else if (key == VK_PRIOR) index -= d.visible; else if (key == VK_NEXT) index += d.visible;
    else if (key == VK_HOME) index = 0; else if (key == VK_END) index = last;
    index = std::clamp(index, 0, (std::max)(0, last)); DropdownHot(index); EnsureVisible(index);
}
LRESULT CALLBACK DropdownProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    Dropdown& d = dropdown_;
    switch (message) {
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        if (!d.open || d.closing) return 0;
        bool inside = false; const int index = DropdownHit(lparam, &inside);
        if (inside && index >= 0) DropdownHot(index);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        bool inside = false; DropdownHit(lparam, &inside);
        if (!inside) CloseDropdown(false);
        return 0;
    }
    case WM_LBUTTONUP: {
        bool inside = false; const int index = DropdownHit(lparam, &inside);
        if (inside && index >= 0) { DropdownHot(index); CloseDropdown(true); }
        return 0;
    }
    case WM_MOUSEWHEEL:
        d.scroll_target = std::clamp(d.scroll_target - GET_WHEEL_DELTA_WPARAM(wparam) * d.item * 3 / WHEEL_DELTA, 0, DropdownMax()); return 0;
    case WM_CAPTURECHANGED: if (d.open && !d.closing && reinterpret_cast<HWND>(lparam) != window) CloseDropdown(false); return 0;
    case WM_TIMER: {
        if (wparam != kPopupTimer) return 0;
        const ULONGLONG now = GetTickCount64();
        RECT anchor{};
        if (!d.closing && (!IsWindow(d.combo) || !IsWindowVisible(d.combo) || (GetWindowRect(d.combo, &anchor), !EqualRect(&anchor, &d.anchor)))) CloseDropdown(false);
        const double previous = d.scroll;
        d.scroll += (d.scroll_target - d.scroll) * (reduced_motion_ ? 1.0 : .35);
        if (std::fabs(d.scroll_target - d.scroll) < .5) d.scroll = d.scroll_target;
        if (d.hot_fade.Running(now) || previous != d.scroll || d.hot_fade.start + d.hot_fade.duration + 32 > now) RenderDropdown();
        PresentDropdown();
        if (d.closing && !d.fade.Running(now)) {
            KillTimer(window, kPopupTimer); ShowWindow(window, SW_HIDE); d.open = d.closing = false; d.combo = nullptr;
        }
        return 0;
    }
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}
// Combo messages handled in place of the native drop-down. Returns true when consumed.
bool ComboMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam, LRESULT* result) {
    Dropdown& d = dropdown_; const bool mine = d.open && !d.closing && d.combo == window;
    *result = 0;
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
        if (!IsWindowEnabled(window)) return false;
        SetFocus(window); if (mine) CloseDropdown(false); else OpenDropdown(window); return true;
    case WM_SYSKEYDOWN:
        if ((wparam == VK_DOWN || wparam == VK_UP) && (lparam & (1 << 29))) { if (mine) CloseDropdown(true); else OpenDropdown(window); return true; }
        return false;
    case WM_KEYDOWN:
        if (!mine) { if (wparam == VK_F4) { OpenDropdown(window); return true; } return false; }
        if (wparam == VK_RETURN || wparam == VK_SPACE || wparam == VK_F4) { CloseDropdown(true); return true; }
        if (wparam == VK_ESCAPE) { CloseDropdown(false); return true; }
        if (wparam == VK_TAB) { CloseDropdown(false); return false; }
        DropdownKey(wparam); return true;
    case WM_CHAR: return mine;
    case WM_GETDLGCODE: if (mine) { *result = DLGC_WANTALLKEYS; return true; } return false;
    case WM_MOUSEWHEEL: if (mine) { SendMessageW(d.popup, message, wparam, lparam); return true; } return false;
    case WM_KILLFOCUS: if (mine) CloseDropdown(false); return false;
    case CB_SHOWDROPDOWN: if (wparam && !mine) OpenDropdown(window); else if (!wparam && mine) CloseDropdown(false); *result = TRUE; return true;
    case CB_GETDROPPEDSTATE: *result = mine; return true;
    default: return false;
    }
}
void RegisterClasses() {
    if (classes_registered_) return;
    WNDCLASSEXW type{sizeof(type)}; type.hInstance = GetModuleHandleW(nullptr); type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = kSkinClass; type.lpfnWndProc = SkinProc; RegisterClassExW(&type);
    type.lpszClassName = kDropdownClass; type.lpfnWndProc = DropdownProc; RegisterClassExW(&type);
    classes_registered_ = true;
}

// Non-owner-drawn list boxes keep their native selection, scrolling and
// keyboard logic; only the paint pass is replaced.
void PaintList(HWND window, Record& record, HDC target) {
    RECT client{}; GetClientRect(window, &client);
    Buffer buffer(target, client); HDC dc = buffer.dc();
    const bool enabled = IsWindowEnabled(window) != FALSE;
    const COLORREF face = enabled ? colors_.panel : Mix(colors_.background, colors_.panel, 45);
    Fill(dc, client, face);
    FontScope font(dc, window); SetBkMode(dc, TRANSPARENT);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    const bool multiple = (style & (LBS_MULTIPLESEL | LBS_EXTENDEDSEL)) != 0;
    const LRESULT count = SendMessageW(window, LB_GETCOUNT, 0, 0), selection = SendMessageW(window, LB_GETCURSEL, 0, 0);
    const LRESULT caret = SendMessageW(window, LB_GETCARETINDEX, 0, 0);
    const bool focused = GetFocus() == window;
    const int pad = Scale(window, 8), radius = Scale(window, 5);
    std::wstring text;
    for (LRESULT index = SendMessageW(window, LB_GETTOPINDEX, 0, 0); index >= 0 && index < count; ++index) {
        RECT item{}; if (SendMessageW(window, LB_GETITEMRECT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(&item)) == LB_ERR) break;
        if (item.top >= client.bottom) break;
        const bool selected = multiple ? SendMessageW(window, LB_GETSEL, static_cast<WPARAM>(index), 0) > 0 : index == selection;
        RECT shape{(std::max)(item.left, client.left) + 3, item.top + 1, client.right - 3, item.bottom - 1};
        if (selected) {
            Pill(dc, shape, radius, focused ? colors_.selection : Mix(colors_.selection, face, 35));
            RECT bar{shape.left, shape.top + 3, shape.left + Scale(window, 3), shape.bottom - 3}; Pill(dc, bar, 2, colors_.gold);
        } else if (const double hover = enabled ? HotAlpha(record, static_cast<int>(index)) : 0.0; hover > .01)
            Pill(dc, shape, radius, Mix(face, colors_.blue, static_cast<int>(std::lround(12 * hover))));
        if (focused && index == caret && !selected && !(SendMessageW(window, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS))
            Rounded(dc, shape, radius, face, Mix(colors_.gold, face, 50), 0, false);
        const LRESULT length = SendMessageW(window, LB_GETTEXTLEN, static_cast<WPARAM>(index), 0);
        if (length <= 0 || length > 32768) continue;
        text.assign(static_cast<size_t>(length) + 1, L'\0');
        SendMessageW(window, LB_GETTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(text.data())); text.resize(static_cast<size_t>(length));
        SetTextColor(dc, !enabled ? colors_.muted : selected ? colors_.text : Mix(colors_.text, colors_.muted, 20));
        RECT label = item; label.left += pad + Scale(window, 4); label.right = (std::max)(label.right, client.right) - pad;
        DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &label, DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_EXPANDTABS);
    }
}
bool ComboSupported(HWND window) {
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    return !high_contrast_ && (style & 3) == CBS_DROPDOWNLIST && !(style & (CBS_OWNERDRAWFIXED | CBS_OWNERDRAWVARIABLE));
}
void DrawCombo(HWND window, HDC target, bool hover) {
    RECT rectangle{}; GetClientRect(window, &rectangle);
    COMBOBOXINFO info{sizeof(info)}; if (!GetComboBoxInfo(window, &info)) return;
    Buffer buffer(target, rectangle); HDC dc = buffer.dc();
    const bool enabled = IsWindowEnabled(window) != FALSE, focused = GetFocus() == window || IsChild(window, GetFocus());
    const bool dropped = SendMessageW(window, CB_GETDROPPEDSTATE, 0, 0) != FALSE;
    Visual visual = Current(window); if (!State(window)) visual = {hover ? 1.0 : 0.0, 0, focused ? 1.0 : 0.0, 0};
    COLORREF edge_color = enabled ? Mix(colors_.border, colors_.blue, Percent(visual.hover * .75)) : Mix(colors_.border, colors_.background, 40);
    edge_color = Mix(edge_color, colors_.gold, Percent((std::max)(visual.focus, dropped ? 1.0 : 0.0)));
    const COLORREF face = enabled ? Mix(colors_.panel, colors_.blue, Percent(visual.hover * .06)) : Mix(colors_.background, colors_.panel, 45);
    Fill(dc, rectangle, colors_.background);
    Rounded(dc, rectangle, (std::max)(5, Scale(window, 8)), face, edge_color, (std::max)(visual.focus, visual.hover * .55));
    RECT button = info.rcButton;
    const int edge = (std::max)(3, Scale(window, 4)), x = (button.left + button.right) / 2, y = (button.top + button.bottom) / 2;
    if (graphics_token_) {
        // Chevron rotates to point up while the list is open.
        Gdiplus::Graphics graphics(dc); graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        Gdiplus::Pen pen(Color(enabled ? (dropped ? colors_.gold : colors_.muted) : colors_.border), (std::max)(1.5f, Scale(window, 2) * .8f));
        pen.SetStartCap(Gdiplus::LineCapRound); pen.SetEndCap(Gdiplus::LineCapRound); pen.SetLineJoin(Gdiplus::LineJoinRound);
        const float direction = dropped ? -1.0f : 1.0f, half = static_cast<float>(edge) * .5f;
        const Gdiplus::PointF points[3]{{x - static_cast<float>(edge), y - half * direction}, {static_cast<float>(x), y + half * direction}, {x + static_cast<float>(edge), y - half * direction}};
        graphics.DrawLines(&pen, points, 3);
    } else {
        POINT points[3]{{x - edge, y - edge / 2}, {x + edge, y - edge / 2}, {x, y + edge / 2 + 1}};
        HGDIOBJ old_brush = SelectObject(dc, GetStockObject(DC_BRUSH)), old_pen = SelectObject(dc, GetStockObject(DC_PEN));
        SetDCBrushColor(dc, enabled ? colors_.muted : colors_.border); SetDCPenColor(dc, enabled ? colors_.muted : colors_.border);
        Polygon(dc, points, 3); SelectObject(dc, old_brush); SelectObject(dc, old_pen);
    }
    const LRESULT index = SendMessageW(window, CB_GETCURSEL, 0, 0);
    const LRESULT length = index == CB_ERR ? CB_ERR : SendMessageW(window, CB_GETLBTEXTLEN, static_cast<WPARAM>(index), 0);
    if (length >= 0 && length <= 32768) {
        std::wstring text(static_cast<size_t>(length) + 1, L'\0');
        SendMessageW(window, CB_GETLBTEXT, static_cast<WPARAM>(index), reinterpret_cast<LPARAM>(text.data()));
        RECT item = info.rcItem; item.left = rectangle.left + Scale(window, 10); item.right = button.left - Scale(window, 2);
        FontScope font(dc, window); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, enabled ? colors_.text : colors_.muted);
        DrawTextW(dc, text.c_str(), static_cast<int>(length), &item, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
}
BOOL CALLBACK ApplyChild(HWND child, LPARAM) { ApplyOne(child, false); return TRUE; }
bool Bordered(HWND window) {
    return (GetWindowLongPtrW(window, GWL_STYLE) & WS_BORDER) || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_CLIENTEDGE);
}
void RefreshColors(HWND window, Kind kind) {
    // DarkMode_Explorer only recolours the native scroll bars; frames and
    // items keep the custom painting below.
    if (kind == Kind::Edit || kind == Kind::ListBox || kind == Kind::ListView)
        SetWindowTheme(window, high_contrast_ ? nullptr : L"DarkMode_Explorer", nullptr);
    if (kind == Kind::ComboBox || kind == Kind::Header)
        SetWindowTheme(window, high_contrast_ ? nullptr : L"", high_contrast_ ? nullptr : L"");
    if (kind == Kind::ListView && !high_contrast_) {
        ListView_SetExtendedListViewStyleEx(window, LVS_EX_GRIDLINES, 0);
        const int row = Scale(window, 32);
        HIMAGELIST spacer = reinterpret_cast<HIMAGELIST>(GetPropW(window, kSpacerProperty));
        int width = 0, height = 0; if (spacer) ImageList_GetIconSize(spacer, &width, &height);
        if (!spacer || height != row) {
            // A 1-pixel-wide image list only sets the row height; the list view owns and frees it.
            if ((spacer = ImageList_Create(1, row, ILC_COLOR32, 1, 1)) != nullptr) { ListView_SetImageList(window, spacer, LVSIL_SMALL); SetPropW(window, kSpacerProperty, spacer); }
        }
    }
    if (kind == Kind::ListView) {
        const COLORREF face = high_contrast_ ? colors_.background : colors_.panel;
        ListView_SetBkColor(window, face); ListView_SetTextBkColor(window, face);
        ListView_SetTextColor(window, colors_.text);
        HWND tip = ListView_GetToolTips(window); if (tip) ApplyOne(tip, false);
    } else if (kind == Kind::ToolTip) {
        SetWindowTheme(window, high_contrast_ ? nullptr : L"", high_contrast_ ? nullptr : L"");
        SendMessageW(window, TTM_SETTIPBKCOLOR, colors_.panel, 0); SendMessageW(window, TTM_SETTIPTEXTCOLOR, colors_.text, 0);
    } else if (kind == Kind::ComboBox) {
        COMBOBOXINFO info{sizeof(info)};
        if (GetComboBoxInfo(window, &info)) {
            if (info.hwndItem && info.hwndItem != window) ApplyOne(info.hwndItem, false);
            if (info.hwndList && info.hwndList != window) SetWindowTheme(info.hwndList, high_contrast_ ? nullptr : L"DarkMode_Explorer", nullptr);
        }
    }
    if (kind == Kind::Edit && Bordered(window))
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    if (!(GetWindowLongPtrW(window, GWL_STYLE) & WS_CHILD)) {
        const BOOL dark = !high_contrast_;
        DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    }
}
void ApplyOne(HWND window, bool root) {
    if (!window || !IsWindow(window) || GetWindowThreadProcessId(window, nullptr) != thread_) return;
    wchar_t name[64]{}; GetClassNameW(window, name, 64);
    if (!_wcsicmp(name, kSkinClass) || !_wcsicmp(name, kDropdownClass)) return;
    DWORD_PTR data = 0;
    if (!GetWindowSubclass(window, Procedure, kSubclass, &data)) {
        auto* record = new (std::nothrow) Record{window, Classify(window, root)};
        if (!record) return;
        if (!SetWindowSubclass(window, Procedure, kSubclass, reinterpret_cast<DWORD_PTR>(record))) { delete record; return; }
        records_.push_back(record); data = reinterpret_cast<DWORD_PTR>(record);
    }
    auto* record = reinterpret_cast<Record*>(data);
    if (root && record->kind == Kind::Other) record->kind = Kind::Container;
    RefreshColors(window, record->kind);
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if (record->kind == Kind::Static && !(style & SS_NOPREFIX) && (style & SS_TYPEMASK) <= SS_RIGHT)
        SetWindowLongPtrW(window, GWL_STYLE, style | SS_NOPREFIX);
    if (!high_contrast_ && GetParent(window) && (record->kind == Kind::ListView ||
        ((record->kind == Kind::Edit || record->kind == Kind::ListBox) && (style & (WS_VSCROLL | WS_HSCROLL))))) {
        record->scrollable = true;
        if (!(style & WS_CLIPSIBLINGS)) SetWindowLongPtrW(window, GWL_STYLE, style | WS_CLIPSIBLINGS);
        SyncSkins(*record);
    }
    Target(*record);
}
// Bordered edits get inner padding; single-line ones are also centred vertically.
void EditPadding(HWND window, RECT& client) {
    if (high_contrast_ || !Bordered(window)) return;
    const bool multiline = (GetWindowLongPtrW(window, GWL_STYLE) & ES_MULTILINE) != 0;
    const int horizontal = Scale(window, multiline ? 8 : 9);
    int vertical = Scale(window, multiline ? 5 : 0);
    if (!multiline) {
        HDC dc = GetDC(window); TEXTMETRICW metrics{};
        if (dc) { FontScope font(dc, window); GetTextMetricsW(dc, &metrics); ReleaseDC(window, dc); }
        const int available = client.bottom - client.top - static_cast<int>(metrics.tmHeight);
        vertical = (std::max)(0, available / 2);
    }
    if (client.right - client.left > horizontal * 2 + 8) { client.left += horizontal; client.right -= horizontal; }
    if (client.bottom - client.top > vertical * 2 + 8) { client.top += vertical; client.bottom -= vertical; }
}
bool ListMessage(UINT message) {
    return message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_KEYDOWN ||
        message == WM_CHAR || message == LB_SETCURSEL || message == LB_SETSEL || message == LB_SETCARETINDEX ||
        message == LB_SELITEMRANGE || message == LB_SELITEMRANGEEX || message == WM_CAPTURECHANGED;
}
LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam, Record* record);
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR data) {
    auto* record = reinterpret_cast<Record*>(data);
    if (message == WM_NCDESTROY || !record->scrollable || !SkinTrigger(message)) return Handle(window, message, wparam, lparam, record);
    const LRESULT result = Handle(window, message, wparam, lparam, record);
    SyncSkins(*record); return result;
}
LRESULT Handle(HWND window, UINT message, WPARAM wparam, LPARAM lparam, Record* record) {
    if (message == WM_NCDESTROY) {
        Stop(*record, false); RemovePropW(window, kRoleProperty); RemovePropW(window, kSpacerProperty);
        if (record->hot_timer) KillTimer(window, kHotTimer);
        DestroySkins(*record);
        if (dropdown_.combo == window) { CloseDropdown(false); dropdown_.combo = nullptr; }
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        RemoveWindowSubclass(window, Procedure, kSubclass);
        records_.erase(std::remove(records_.begin(), records_.end(), record), records_.end()); delete record; return result;
    }
    if (message == apply_message_ && apply_message_) {
        record->apply_pending = false; Apply(window); return 0;
    }
    if (message == WM_PARENTNOTIFY && LOWORD(wparam) == WM_CREATE && !record->apply_pending && apply_message_) {
        record->apply_pending = PostMessageW(window, apply_message_, 0, 0) != FALSE;
    }
    if (message == WM_MOUSEMOVE && MotionControl(record->kind) && !record->hover) {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track);
        record->hover = true; Target(*record);
    }
    if (message == WM_MOUSEMOVE && record->kind == Kind::ListBox && !high_contrast_) {
        const LRESULT hit = SendMessageW(window, LB_ITEMFROMPOINT, 0, lparam);
        SetHot(*record, HIWORD(hit) ? -1 : static_cast<int>(LOWORD(hit)));
    }
    if (message == WM_MOUSEMOVE && record->kind == Kind::ListView && !high_contrast_) {
        LVHITTESTINFO hit{}; hit.pt = {GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        SetHot(*record, ListView_HitTest(window, &hit));
    }
    if (message == WM_MOUSELEAVE && record->hover) {
        record->hover = false;
        if (record->kind == Kind::ListBox || record->kind == Kind::ListView) SetHot(*record, -1);
        Target(*record);
    }
    if (message == WM_TIMER && wparam == kHotTimer) {
        InvalidateItem(*record, record->hot_item); InvalidateItem(*record, record->hot_previous);
        if (!record->hot_fade.Running(GetTickCount64())) { KillTimer(window, kHotTimer); record->hot_timer = false; }
        return 0;
    }
    if (record->kind == Kind::ComboBox && ComboSupported(window)) {
        LRESULT result = 0; if (ComboMessage(window, message, wparam, lparam, &result)) return result;
    }
    if (message == WM_TIMER && wparam == kAnimationTimer) {
        if (!record->timer) return 0;
        if (!IsWindowVisible(window)) {
            record->hover = false; Stop(*record, false); record->from = record->to = {};
        } else if (reduced_motion_) Stop(*record, false);
        else if (GetTickCount64() - record->start >= kDuration) Stop(*record, true);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME); return 0;
    }
    if (message == WM_SHOWWINDOW && !wparam) { record->hover = false; Stop(*record, false); record->from = record->to = {}; }
    if (message == WM_SETTINGCHANGE) {
        reduced_motion_ = UiMotion::ReducedMotion();
        if (reduced_motion_) for (Record* state : records_) Stop(*state, false);
    }
    if (message == WM_NOTIFY) {
        bool handled = false; const LRESULT result = CustomDraw(reinterpret_cast<NMHDR*>(lparam), &handled);
        if (handled) return result;
    }
    if (message == WM_CTLCOLORSTATIC || message == WM_CTLCOLOREDIT || message == WM_CTLCOLORLISTBOX || message == WM_CTLCOLORBTN) {
        const HWND control = reinterpret_cast<HWND>(lparam); HDC dc = reinterpret_cast<HDC>(wparam);
        const Kind kind = Classify(control, false);
        const bool panel = kind == Kind::Edit || kind == Kind::ListBox || kind == Kind::ComboBox;
        COLORREF text = IsWindowEnabled(control) ? colors_.text : (high_contrast_ ? colors_.muted : Mix(colors_.muted, colors_.background, 22));
        if (kind == Kind::Static && RoleOf(control) == Role::Hint && !high_contrast_) text = colors_.muted;
        const bool read_only = kind == Kind::Edit && (GetWindowLongPtrW(control, GWL_STYLE) & ES_READONLY) && !Bordered(control);
        SetTextColor(dc, read_only && !high_contrast_ ? Mix(colors_.text, colors_.muted, 30) : text);
        const bool filled = panel && !read_only;
        SetBkColor(dc, filled ? colors_.panel : colors_.background); SetBkMode(dc, filled ? OPAQUE : TRANSPARENT);
        if (read_only) SetBkMode(dc, OPAQUE);
        return reinterpret_cast<LRESULT>(filled ? panel_ : background_);
    }
    if (message == WM_ERASEBKGND && record->kind == Kind::Container) {
        RECT rectangle{}; GetClientRect(window, &rectangle); FillRect(reinterpret_cast<HDC>(wparam), &rectangle, background_); return 1;
    }
    if (message == WM_ERASEBKGND && record->kind == Kind::Button && !high_contrast_ &&
        (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX) return 1;
    if (record->kind == Kind::Static && !high_contrast_ && (RoleOf(window) == Role::Section || RoleOf(window) == Role::Card)) {
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{}; HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
            if (RoleOf(window) == Role::Section) SectionStatic(window, dc); else CardStatic(window, dc);
            if (message == WM_PAINT) EndPaint(window, &paint);
            return 0;
        }
        if (message == WM_SETTEXT || message == WM_ENABLE) {
            const LRESULT result = DefSubclassProc(window, message, wparam, lparam); InvalidateRect(window, nullptr, FALSE); return result;
        }
    }
    if (record->kind == Kind::ListBox && !high_contrast_ &&
        !(GetWindowLongPtrW(window, GWL_STYLE) & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE))) {
        if (message == WM_ERASEBKGND) return 1;
        if (message == WM_PAINT || message == WM_PRINTCLIENT) {
            PAINTSTRUCT paint{}; HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
            PaintList(window, *record, dc); if (message == WM_PAINT) EndPaint(window, &paint); return 0;
        }
        if (ListMessage(message) || message == WM_SETFOCUS || message == WM_KILLFOCUS || (message == WM_MOUSEMOVE && (wparam & MK_LBUTTON))) {
            // The native list paints selection changes straight to its DC;
            // repaint in the same message so only the themed frame is shown.
            const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
            if (message == WM_SETFOCUS || message == WM_KILLFOCUS) Target(*record);
            RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | (message == WM_SETFOCUS || message == WM_KILLFOCUS ? RDW_FRAME : 0));
            return result;
        }
    }
    // Standard BUTTON custom draw is not available under every host activation
    // context. Painting the existing native HWND also covers comctl32 v5 hosts;
    // its original procedure still owns input, check state and accessibility.
    if ((message == WM_PAINT || message == WM_PRINTCLIENT) && record->kind == Kind::Button && ButtonSupported(window)) {
        PAINTSTRUCT paint{}; HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
        NMCUSTOMDRAW draw{}; draw.hdr.hwndFrom = window; draw.hdc = dc; draw.dwDrawStage = CDDS_PREPAINT;
        if (record->hover) draw.uItemState |= CDIS_HOT;
        GetClientRect(window, &draw.rc); DrawButton(&draw);
        if (message == WM_PAINT) EndPaint(window, &paint);
        return 0;
    }
    if (message == WM_ERASEBKGND && record->kind == Kind::Button && ButtonSupported(window)) return 1;
    if ((message == WM_PAINT || message == WM_PRINTCLIENT) && record->kind == Kind::ComboBox && ComboSupported(window)) {
        PAINTSTRUCT paint{}; HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wparam);
        DrawCombo(window, dc, record->hover); if (message == WM_PAINT) EndPaint(window, &paint); return 0;
    }
    if (message == WM_PAINT && record->kind == Kind::Button && !high_contrast_ &&
        (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint); GroupBox(window, dc); EndPaint(window, &paint); return 0;
    }
    if (message == WM_PRINTCLIENT && record->kind == Kind::Button && !high_contrast_ &&
        (GetWindowLongPtrW(window, GWL_STYLE) & BS_TYPEMASK) == BS_GROUPBOX) {
        GroupBox(window, reinterpret_cast<HDC>(wparam)); return 0;
    }
    if (message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == WM_UPDATEUISTATE ||
        message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_LBUTTONDBLCLK || message == WM_CAPTURECHANGED ||
        message == WM_KEYDOWN || message == WM_KEYUP || message == BM_SETSTATE || message == BM_SETCHECK || message == CB_SHOWDROPDOWN) {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        if (message == WM_ENABLE && !wparam) record->hover = false;
        Target(*record); if (Record* parent = State(GetParent(window))) Target(*parent);
        RedrawWindow(window, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME); return result;
    }
    if (message == WM_COMMAND && (HIWORD(wparam) == CBN_DROPDOWN || HIWORD(wparam) == CBN_CLOSEUP)) {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        InvalidateRect(reinterpret_cast<HWND>(lparam), nullptr, FALSE); return result;
    }
    if (message == WM_SETFONT && record->kind == Kind::Edit && Bordered(window)) {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED); return result;
    }
    if (message == WM_NCCALCSIZE && wparam && record->kind == Kind::Edit) {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        EditPadding(window, reinterpret_cast<NCCALCSIZE_PARAMS*>(lparam)->rgrc[0]); return result;
    }
    if (message == WM_NCPAINT && !high_contrast_ &&
        (record->kind == Kind::Edit || record->kind == Kind::ListBox || record->kind == Kind::ComboBox || record->kind == Kind::ListView)) {
        const LRESULT result = DefSubclassProc(window, message, wparam, lparam);
        PaintControlFrame(window);
        return result;
    }
    return DefSubclassProc(window, message, wparam, lparam);
}
HFONT MakeFont(Font role, UINT dpi) {
    const auto px = [&](int value) { return -MulDiv(value, static_cast<int>(dpi ? dpi : 96), 96); };
    if (role == Font::Icon) {
        if (icon_face_.empty()) {
            icon_face_ = L"Segoe MDL2 Assets";
            HDC screen = GetDC(nullptr);
            LOGFONTW query{}; query.lfCharSet = DEFAULT_CHARSET; wcscpy_s(query.lfFaceName, L"Segoe Fluent Icons");
            if (screen && EnumFontFamiliesExW(screen, &query, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM) -> int { return 0; }, 0, 0) == 0)
                icon_face_ = L"Segoe Fluent Icons";
            if (screen) ReleaseDC(nullptr, screen);
        }
        return CreateFontW(px(16), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            ANTIALIASED_QUALITY, DEFAULT_PITCH, icon_face_.c_str());
    }
    if (body_face_.empty()) {
        // The game sets Chinese body text in STZhongsong (STZHONGS.ufont); use the
        // installed copy when present. Titles use the embedded game title face.
        body_face_ = L"Microsoft YaHei UI";
        HDC screen = GetDC(nullptr);
        LOGFONTW query{}; query.lfCharSet = DEFAULT_CHARSET; wcscpy_s(query.lfFaceName, L"STZhongsong");
        if (screen && EnumFontFamiliesExW(screen, &query, [](const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM) -> int { return 0; }, 0, 0) == 0)
            body_face_ = L"STZhongsong";
        if (screen) ReleaseDC(nullptr, screen);
    }
    const bool display = (role == Font::Title || role == Font::Heading) && *UiAssets::TitleFace();
    const int size = role == Font::Title ? 25 : role == Font::Heading ? 17 : role == Font::Small ? 13 : 15;
    const int weight = display ? FW_NORMAL : role == Font::Title || role == Font::Strong || role == Font::Heading ? FW_BOLD : FW_NORMAL;
    return CreateFontW(px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, display ? UiAssets::TitleFace() : body_face_.c_str());
}
}
COLORREF Mix(COLORREF a, COLORREF b, int percent) {
    percent = std::clamp(percent, 0, 100);
    return RGB((GetRValue(a) * (100 - percent) + GetRValue(b) * percent) / 100,
        (GetGValue(a) * (100 - percent) + GetGValue(b) * percent) / 100,
        (GetBValue(a) * (100 - percent) + GetBValue(b) * percent) / 100);
}
void Initialize() {
    if (thread_) return;
    thread_ = GetCurrentThreadId(); apply_message_ = RegisterWindowMessageW(L"PBZ.DebugDLL.UiTheme.ApplyChildren.v1");
    HIGHCONTRASTW contrast{sizeof(contrast)};
    high_contrast_ = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) && (contrast.dwFlags & HCF_HIGHCONTRASTON);
    reduced_motion_ = UiMotion::ReducedMotion();
    Gdiplus::GdiplusStartupInput input;
    if (Gdiplus::GdiplusStartup(&graphics_token_, &input, nullptr) != Gdiplus::Ok) graphics_token_ = 0;
    colors_ = Palette{};
    if (high_contrast_) {
        colors_.base = colors_.background = colors_.panel = GetSysColor(COLOR_WINDOW);
        colors_.text = GetSysColor(COLOR_WINDOWTEXT); colors_.muted = GetSysColor(COLOR_GRAYTEXT);
        colors_.border = GetSysColor(COLOR_WINDOWFRAME); colors_.gold = colors_.blue = GetSysColor(COLOR_HIGHLIGHT);
        colors_.selection = colors_.danger = GetSysColor(COLOR_HIGHLIGHT);
    }
    background_ = CreateSolidBrush(colors_.background); panel_ = CreateSolidBrush(colors_.panel); base_ = CreateSolidBrush(colors_.base);
}
bool DismissPopup() {
    if (!dropdown_.open || dropdown_.closing) return false;
    CloseDropdown(false); return true;
}
bool PopupActive() { return dropdown_.open; }
void Apply(HWND root) {
    Initialize(); if (thread_ != GetCurrentThreadId() || !root) return;
    RegisterClasses();
    ApplyOne(root, true); EnumChildWindows(root, ApplyChild, 0);
    RedrawWindow(root, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
}
HFONT GetFont(Font role, UINT dpi) {
    Initialize(); if (!dpi) dpi = 96;
    const auto key = std::make_pair(static_cast<int>(role), dpi);
    const auto found = fonts_.find(key); if (found != fonts_.end()) return found->second;
    HFONT font = MakeFont(role, dpi); if (font) fonts_.emplace(key, font);
    return font ? font : static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
}
HFONT GetFont(Font role, HWND window) { const UINT dpi = window ? GetDpiForWindow(window) : 96; return GetFont(role, dpi ? dpi : 96); }
HFONT GetFontFor(Font role, UINT dpi, const wchar_t* text, int length) {
    HFONT font = GetFont(role, dpi);
    if ((role != Font::Title && role != Font::Heading) || !text) return font;
    static HDC probe = CreateCompatibleDC(nullptr);
    if (!probe) return font;
    const HGDIOBJ old = SelectObject(probe, font);
    const bool covered = UiAssets::Covers(probe, text, length);
    SelectObject(probe, old);
    return covered ? font : GetFont(Font::Strong, dpi);
}
namespace {
HFONT FontFor(Font role, HWND window, const std::wstring& text) {
    const UINT dpi = window ? GetDpiForWindow(window) : 96;
    return GetFontFor(role, dpi ? dpi : 96, text.c_str(), static_cast<int>(text.size()));
}
}
void SetRole(HWND control, Role role) {
    if (!control) return;
    if (role == Role::None) RemovePropW(control, kRoleProperty);
    else SetPropW(control, kRoleProperty, reinterpret_cast<HANDLE>(static_cast<INT_PTR>(role)));
    InvalidateRect(control, nullptr, TRUE);
}
Role GetRole(HWND control) { return control ? RoleOf(control) : Role::None; }
HBRUSH Background() { Initialize(); return background_; }
HBRUSH PanelBrush() { Initialize(); return panel_; }
HBRUSH BaseBrush() { Initialize(); return base_; }
COLORREF Text() { return colors_.text; }
COLORREF MutedText() { return colors_.muted; }
COLORREF Accent() { return colors_.gold; }
COLORREF AccentBlue() { return colors_.blue; }
COLORREF Panel() { return colors_.panel; }
COLORREF Border() { return colors_.border; }
COLORREF Selection() { return colors_.selection; }
COLORREF Base() { return colors_.base; }
COLORREF Surface() { return colors_.background; }
COLORREF Danger() { return colors_.danger; }
bool HighContrast() { return high_contrast_; }
AnimationStats GetAnimationStats() {
    AnimationStats result; result.started = starts_; result.completed = completes_; result.cancelled = cancels_;
    result.reduced_motion = reduced_motion_;
    for (const Record* state : records_) if (state->timer) { ++result.active_controls; ++result.active_timers; }
    return result;
}
void Shutdown() {
    if (thread_ != GetCurrentThreadId()) return;
    const auto records = records_; records_.clear();
    for (Record* record : records) {
        Stop(*record, false); if (record->hot_timer) KillTimer(record->window, kHotTimer);
        DestroySkins(*record); RemoveWindowSubclass(record->window, Procedure, kSubclass); delete record;
    }
    if (dropdown_.popup) DestroyWindow(dropdown_.popup);
    dropdown_.popup = dropdown_.combo = nullptr; dropdown_.open = dropdown_.closing = false; dropdown_.items.clear(); dropdown_.surface.Reset();
    if (classes_registered_) { UnregisterClassW(kSkinClass, GetModuleHandleW(nullptr)); UnregisterClassW(kDropdownClass, GetModuleHandleW(nullptr)); classes_registered_ = false; }
    if (background_) DeleteObject(background_); if (panel_) DeleteObject(panel_); if (base_) DeleteObject(base_);
    background_ = panel_ = base_ = nullptr; thread_ = 0; apply_message_ = 0;
    for (auto& font : fonts_) DeleteObject(font.second);
    fonts_.clear(); icon_face_.clear();
    UiAssets::Shutdown();
    if (graphics_token_) Gdiplus::GdiplusShutdown(graphics_token_);
    graphics_token_ = 0;
}
}
