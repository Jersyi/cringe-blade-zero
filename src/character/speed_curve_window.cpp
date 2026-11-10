#include "ui_paint_buffer.h"
#include "speed_curve_window.h"

#include <windowsx.h>
#include <objidl.h>
#include <gdiplus.h>
#include <algorithm>
#include <cmath>
#include <cwchar>

#include "ui_host.h"
#include "ui_i18n.h"
#include "ui_theme.h"

namespace SpeedCurveWindow {
namespace {
using NpcCombo::CurvePoint;
constexpr wchar_t kClass[] = L"PBZSpeedCurve_v1";
constexpr int kWidth = 640, kHeight = 430;  // 96-dpi units
constexpr double kLow = 0.5, kHigh = 10.0;  // the graph's speed range (×)
enum class Preset : int { Flat, FastSlow, SlowFast, Burst, SlowEnd, Toggle, Count };

HWND window_ = nullptr, page_ = nullptr;
int node_ = -1;
const char* last_close_ = "";
Host host_;
std::vector<CurvePoint> points_;
bool on_ = true;
int speed_ = 100;
std::wstring title_;
int hot_ = -1, drag_ = -1, hot_button_ = -1;
bool close_hot_ = false, inside_ = false;
POINT mouse_{-1, -1};
ULONGLONG next_read_ = 0;

std::wstring Tr(const wchar_t* text) { return UiI18n::Translate(text ? text : L""); }
int Dpi() { const UINT dpi = window_ ? GetDpiForWindow(window_) : 0; return dpi ? static_cast<int>(dpi) : 96; }
int S(int v) { return MulDiv(v, Dpi(), 96); }
Gdiplus::Color Argb(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

RECT Client() { RECT r{}; if (window_) GetClientRect(window_, &r); return r; }
RECT TitleRect() { const RECT c = Client(); return {0, 0, c.right, S(38)}; }
RECT CloseRect() { const RECT c = Client(); return {c.right - S(40), S(4), c.right - S(6), S(34)}; }
RECT GraphRect() { const RECT c = Client(); return {S(60), S(56), c.right - S(22), c.bottom - S(108)}; }
RECT InfoRect() { const RECT c = Client(); return {S(16), c.bottom - S(78), c.right - S(16), c.bottom - S(54)}; }
RECT ButtonRect(int i) {
    const RECT c = Client();
    const int count = static_cast<int>(Preset::Count), gap = S(8), left = S(16), right = c.right - S(16);
    const int w = (right - left - gap * (count - 1)) / count;
    const int x = left + i * (w + gap);
    return {x, c.bottom - S(46), x + w, c.bottom - S(14)};
}
double LogOf(double v) { return std::log((std::max)(v, 1e-6)); }
int XOf(double t) { const RECT g = GraphRect(); return g.left + static_cast<int>(std::lround(t / 100.0 * (g.right - g.left))); }
int YOf(double speed_percent) {
    const RECT g = GraphRect();
    const double f = (LogOf(speed_percent / 100.0) - LogOf(kLow)) / (LogOf(kHigh) - LogOf(kLow));
    return g.bottom - static_cast<int>(std::lround(std::clamp(f, 0.0, 1.0) * (g.bottom - g.top)));
}
double TOf(int x) { const RECT g = GraphRect(); return std::clamp((x - g.left) * 100.0 / (std::max)(1L, g.right - g.left), 0.0, 100.0); }
double SpeedOf(int y) {
    const RECT g = GraphRect();
    const double f = std::clamp((g.bottom - y) / static_cast<double>((std::max)(1L, g.bottom - g.top)), 0.0, 1.0);
    return std::exp(LogOf(kLow) + f * (LogOf(kHigh) - LogOf(kLow))) * 100.0;
}
int Snap(double percent, bool fine) {
    const int step = fine ? 1 : 5;
    return NpcCombo::ClampSpeed(static_cast<int>(std::lround(percent / step)) * step);
}
bool In(const RECT& r, POINT p) { return PtInRect(&r, p) != FALSE; }
int PointUnder(POINT p) {
    int best = -1; long best_d = S(10) * S(10) + 1;
    for (size_t i = 0; i < points_.size(); ++i) {
        const long dx = p.x - XOf(points_[i].t), dy = p.y - YOf(points_[i].speed), d = dx * dx + dy * dy;
        if (d < best_d) { best_d = d; best = static_cast<int>(i); }
    }
    return best;
}
int ButtonUnder(POINT p) {
    for (int i = 0; i < static_cast<int>(Preset::Count); ++i) if (In(ButtonRect(i), p)) return i;
    return -1;
}
std::wstring Times(double percent) { wchar_t t[24]{}; _snwprintf_s(t, _countof(t), _TRUNCATE, L"×%.2f", percent / 100.0); return t; }
std::wstring PresetText(Preset p) {
    switch (p) {
    case Preset::Flat: return Tr(L"平直");
    case Preset::FastSlow: return Tr(L"先快后慢");
    case Preset::SlowFast: return Tr(L"先慢后快");
    case Preset::Burst: return Tr(L"中段爆发");
    case Preset::SlowEnd: return Tr(L"收招慢放");
    case Preset::Toggle: return Tr(on_ ? L"关闭曲线" : L"开启曲线");
    default: return L"";
    }
}
std::vector<CurvePoint> PresetPoints(Preset p) {
    switch (p) {
    case Preset::Flat: return {{0, speed_}, {100, speed_}};
    case Preset::FastSlow: return {{0, 250}, {60, 150}, {100, 60}};
    case Preset::SlowFast: return {{0, 60}, {40, 150}, {100, 250}};
    case Preset::Burst: return {{0, 100}, {35, 100}, {50, 300}, {65, 100}, {100, 100}};
    case Preset::SlowEnd: return {{0, 150}, {70, 150}, {85, 40}, {100, 100}};
    default: return points_;
    }
}
void Invalidate() { if (window_) InvalidateRect(window_, nullptr, FALSE); }
void Write(bool commit) { if (host_.write) host_.write(points_, on_, commit); Invalidate(); }
bool Read() {
    if (!host_.read) return false;
    std::vector<CurvePoint> points; bool on = false; int speed = 100; std::wstring title;
    if (!host_.read(points, on, speed, title)) return false;
    if (points.size() < 2) points = NpcCombo::DefaultCurve(speed);
    if (points.size() != points_.size() || on != on_ || speed != speed_ || title != title_ ||
        !std::equal(points.begin(), points.end(), points_.begin(), [](const CurvePoint& a, const CurvePoint& b) { return a.t == b.t && a.speed == b.speed; })) {
        points_ = NpcCombo::CleanCurve(points); on_ = on; speed_ = speed; title_ = title;
        if (hot_ >= static_cast<int>(points_.size())) hot_ = -1;
        Invalidate();
    }
    return true;
}

void Label(HDC dc, const std::wstring& text, RECT r, UiTheme::Font font, COLORREF color, UINT flags) {
    const HGDIOBJ old = SelectObject(dc, UiTheme::GetFont(font, window_));
    SetTextColor(dc, color); SetBkMode(dc, TRANSPARENT);
    DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &r, flags | DT_NOPREFIX);
    SelectObject(dc, old);
}
void RoundRect(Gdiplus::Graphics& g, const RECT& r, float radius, COLORREF fill, BYTE fill_a, COLORREF edge, BYTE edge_a, float width = 1.f) {
    Gdiplus::GraphicsPath path;
    const float x = static_cast<float>(r.left), y = static_cast<float>(r.top), w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top);
    const float d = (std::min)(radius * 2, (std::min)(w, h));
    if (d <= 0) path.AddRectangle(Gdiplus::RectF(x, y, w, h));
    else {
        path.AddArc(x, y, d, d, 180, 90); path.AddArc(x + w - d, y, d, d, 270, 90);
        path.AddArc(x + w - d, y + h - d, d, d, 0, 90); path.AddArc(x, y + h - d, d, d, 90, 90); path.CloseFigure();
    }
    if (fill_a) { Gdiplus::SolidBrush b(Argb(fill, fill_a)); g.FillPath(&b, &path); }
    if (edge_a) { Gdiplus::Pen p(Argb(edge, edge_a), width); g.DrawPath(&p, &path); }
}

void Paint() {
    PAINTSTRUCT ps{}; const HDC target = BeginPaint(window_, &ps);
    const RECT client = Client();
    const int w = (std::max)(1L, client.right), h = (std::max)(1L, client.bottom);
    {
    UiPaintBuffer::Frame buffer(window_,target,w,h,&ps.rcPaint);
    const HDC surface=buffer.dc();
    const COLORREF panel = UiTheme::Panel(), base = UiTheme::Base(), text = UiTheme::Text(), muted = UiTheme::MutedText(), accent = UiTheme::Accent(),
                   border = UiTheme::Border();
    const COLORREF hue = UiTheme::HighContrast() ? UiTheme::AccentBlue() : UiTheme::Mix(RGB(74, 178, 140), text, 12);
    {
        Gdiplus::Graphics g(surface); g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        { Gdiplus::SolidBrush b(Argb(UiTheme::Mix(panel, base, 30))); g.FillRectangle(&b, 0, 0, w, h); }
        // Title bar.
        const RECT title = TitleRect();
        { Gdiplus::SolidBrush b(Argb(UiTheme::Mix(panel, hue, 40))); g.FillRectangle(&b, 0, 0, w, title.bottom); }
        RECT words{S(16), 0, CloseRect().left - S(8), title.bottom};
        Label(surface, Tr(L"加速曲线") + (title_.empty() ? L"" : L" · " + title_), words, UiTheme::Font::Strong, text, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        if (close_hot_) RoundRect(g, CloseRect(), static_cast<float>(S(6)), UiTheme::Danger(), 200, 0, 0);
        Label(surface, L"×", CloseRect(), UiTheme::Font::Strong, close_hot_ ? RGB(255, 255, 255) : text, DT_SINGLELINE | DT_VCENTER | DT_CENTER);
        // Graph.
        const RECT graph = GraphRect();
        RoundRect(g, graph, static_cast<float>(S(6)), base, 255, UiTheme::Mix(border, panel, 20), 255);
        Gdiplus::Pen grid(Argb(UiTheme::Mix(base, text, 12)), 1.f), unit(Argb(UiTheme::Mix(base, text, 35)), 1.5f);
        for (const double v : {0.5, 1.0, 2.0, 3.0, 5.0, 10.0}) {
            const int y = YOf(v * 100);
            g.DrawLine(v == 1.0 ? &unit : &grid, graph.left, y, graph.right, y);
            wchar_t t[16]{}; _snwprintf_s(t, _countof(t), _TRUNCATE, v < 1 ? L"×%.1f" : L"×%.0f", v);
            RECT r{0, y - S(10), graph.left - S(6), y + S(10)};
            Label(surface, t, r, UiTheme::Font::Small, v == 1.0 ? text : muted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT);
        }
        for (int t = 0; t <= 100; t += 25) {
            const int x = XOf(t);
            if (t > 0 && t < 100) g.DrawLine(&grid, x, graph.top, x, graph.bottom);
            wchar_t s[8]{}; _snwprintf_s(s, _countof(s), _TRUNCATE, L"%d%%", t);
            RECT r{x - S(30), graph.bottom + S(4), x + S(30), graph.bottom + S(22)};
            Label(surface, s, r, UiTheme::Font::Small, muted, DT_SINGLELINE | DT_TOP | DT_CENTER);
        }
        RECT axis{graph.left, graph.bottom + S(4), graph.right, graph.bottom + S(22)};
        RECT axis_right = axis; axis_right.left = XOf(75) + S(32); axis_right.right = XOf(100) - S(30);
        Label(surface, Tr(L"招式进度 →"), axis_right, UiTheme::Font::Small, muted, DT_SINGLELINE | DT_TOP | DT_RIGHT | DT_END_ELLIPSIS);
        // The curve (dimmed while 曲线调节 is off) and the area to ×1.
        const COLORREF line = on_ ? hue : UiTheme::Mix(base, text, 40);
        std::vector<Gdiplus::PointF> samples;
        for (int i = 0; i <= 160; ++i) {
            const double t = i / 160.0;
            samples.push_back({static_cast<float>(XOf(t * 100)), static_cast<float>(YOf(NpcCombo::CurveSpeed(points_, t)))});
        }
        {
            std::vector<Gdiplus::PointF> area = samples;
            const float one = static_cast<float>(YOf(100));
            area.push_back({samples.back().X, one}); area.push_back({samples.front().X, one});
            Gdiplus::SolidBrush fill(Argb(line, on_ ? 46 : 24));
            g.FillPolygon(&fill, area.data(), static_cast<INT>(area.size()));
            Gdiplus::Pen pen(Argb(line), static_cast<float>(S(2)) + .5f); pen.SetLineJoin(Gdiplus::LineJoinRound);
            g.DrawLines(&pen, samples.data(), static_cast<INT>(samples.size()));
        }
        // The mouse's place on the curve.
        if (inside_ && drag_ < 0 && hot_ < 0) {
            const double t = TOf(mouse_.x);
            const float x = static_cast<float>(XOf(t)), y = static_cast<float>(YOf(NpcCombo::CurveSpeed(points_, t / 100.0)));
            Gdiplus::Pen pen(Argb(UiTheme::Mix(base, text, 30)), 1.f); pen.SetDashStyle(Gdiplus::DashStyleDash);
            g.DrawLine(&pen, x, static_cast<float>(graph.top), x, static_cast<float>(graph.bottom));
            Gdiplus::SolidBrush dot(Argb(line)); g.FillEllipse(&dot, x - S(3), y - S(3), static_cast<float>(S(6)), static_cast<float>(S(6)));
        }
        // Points.
        for (size_t i = 0; i < points_.size(); ++i) {
            const bool hot = static_cast<int>(i) == hot_ || static_cast<int>(i) == drag_, end = i == 0 || i + 1 == points_.size();
            const float r = static_cast<float>(S(hot ? 8 : 6));
            const float x = static_cast<float>(XOf(points_[i].t)), y = static_cast<float>(YOf(points_[i].speed));
            Gdiplus::SolidBrush fill(Argb(hot ? accent : base)); Gdiplus::Pen edge(Argb(on_ ? (hot ? accent : line) : line), static_cast<float>(S(2)));
            if (end) { g.FillRectangle(&fill, x - r, y - r, r * 2, r * 2); g.DrawRectangle(&edge, x - r, y - r, r * 2, r * 2); }
            else { g.FillEllipse(&fill, x - r, y - r, r * 2, r * 2); g.DrawEllipse(&edge, x - r, y - r, r * 2, r * 2); }
        }
        // Info line: the point / the mouse, else the curve's mean pace; the hint on the right.
        const RECT info = InfoRect();
        std::wstring left;
        const int shown = drag_ >= 0 ? drag_ : hot_;
        wchar_t t[160]{};
        if (shown >= 0 && static_cast<size_t>(shown) < points_.size()) {
            _snwprintf_s(t, _countof(t), _TRUNCATE, Tr(L"第 %d 点：进度 %d%% · %s").c_str(), shown + 1, points_[static_cast<size_t>(shown)].t, Times(points_[static_cast<size_t>(shown)].speed).c_str());
            left = t;
        } else if (inside_) {
            const double at = TOf(mouse_.x);
            _snwprintf_s(t, _countof(t), _TRUNCATE, Tr(L"进度 %.0f%% · 曲线 %s").c_str(), at, Times(NpcCombo::CurveSpeed(points_, at / 100.0)).c_str());
            left = t;
        } else {
            const double mean = NpcCombo::CurveMeanSpeed(points_);
            _snwprintf_s(t, _countof(t), _TRUNCATE, Tr(L"平均 %s · 招式用时为原来的 %.0f%%").c_str(), Times(mean).c_str(), 10000.0 / (std::max)(1.0, mean));
            left = t;
        }
        if (!on_) left += Tr(L"（曲线未开启：节点用固定倍数）");
        RECT info_left = info; info_left.right = info.left + (info.right - info.left) * 46 / 100;
        Label(surface, left, info_left, UiTheme::Font::Small, on_ ? text : muted, DT_SINGLELINE | DT_VCENTER | DT_LEFT | DT_END_ELLIPSIS);
        RECT info_right = info; info_right.left = info_left.right + S(8);
        Label(surface, Tr(L"拖动点 · 双击加点 · 右键删点 · 滚轮微调（Shift 更细）"), info_right, UiTheme::Font::Small, muted, DT_SINGLELINE | DT_VCENTER | DT_RIGHT | DT_END_ELLIPSIS);
        // Buttons.
        for (int i = 0; i < static_cast<int>(Preset::Count); ++i) {
            const RECT r = ButtonRect(i); const bool hot = hot_button_ == i, toggle = i == static_cast<int>(Preset::Toggle);
            const COLORREF tint = toggle ? (on_ ? UiTheme::Danger() : hue) : accent;
            RoundRect(g, r, static_cast<float>(S(6)), UiTheme::Mix(base, tint, hot ? 34 : 10), 255, UiTheme::Mix(border, tint, 35), 255);
            Label(surface, PresetText(static_cast<Preset>(i)), r, UiTheme::Font::Small, text, DT_SINGLELINE | DT_VCENTER | DT_CENTER | DT_END_ELLIPSIS);
        }
        Gdiplus::Pen frame(Argb(UiTheme::Mix(border, hue, 30)), 1.f);
        g.DrawRectangle(&frame, 0, 0, w - 1, h - 1);
    }
    }
    EndPaint(window_, &ps);
}

void Hover(POINT p) {
    mouse_ = p;
    const RECT graph = GraphRect();
    RECT near_graph = graph; InflateRect(&near_graph, S(10), S(10));
    const bool inside = In(near_graph, p);
    const int hot = inside ? PointUnder(p) : -1, button = ButtonUnder(p);
    const bool close = In(CloseRect(), p);
    if (inside != inside_ || hot != hot_ || button != hot_button_ || close != close_hot_ || inside) {
        inside_ = inside; hot_ = hot; hot_button_ = button; close_hot_ = close; Invalidate();
    }
    TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window_, 0}; TrackMouseEvent(&track);
}
void DragTo(POINT p, bool fine) {
    if (drag_ < 0 || static_cast<size_t>(drag_) >= points_.size()) return;
    const size_t i = static_cast<size_t>(drag_);
    CurvePoint& point = points_[i];
    if (i > 0 && i + 1 < points_.size()) {
        const int lo = points_[i - 1].t + NpcCombo::kCurveGap, hi = points_[i + 1].t - NpcCombo::kCurveGap;
        point.t = std::clamp(static_cast<int>(std::lround(TOf(p.x))), lo, (std::max)(lo, hi));
    }
    point.speed = Snap(SpeedOf(p.y), fine);
    Write(false);
}
void AddAt(POINT p, bool fine) {
    if (points_.size() >= NpcCombo::kCurvePointsMax) return;
    const int t = static_cast<int>(std::lround(TOf(p.x)));
    for (const CurvePoint& q : points_) if (std::abs(q.t - t) < NpcCombo::kCurveGap) return;
    points_.push_back({t, Snap(SpeedOf(p.y), fine)});
    points_ = NpcCombo::CleanCurve(points_);
    for (size_t i = 0; i < points_.size(); ++i) if (points_[i].t == t) hot_ = static_cast<int>(i);
    Write(true);
}
void RemoveAt(int i) {
    if (i <= 0 || static_cast<size_t>(i) + 1 >= points_.size()) return;  // the ends stay
    points_.erase(points_.begin() + i); hot_ = -1;
    Write(true);
}
void Press(int button) {
    if (button < 0 || button >= static_cast<int>(Preset::Count)) return;
    if (button == static_cast<int>(Preset::Toggle)) on_ = !on_;
    else { points_ = NpcCombo::CleanCurve(PresetPoints(static_cast<Preset>(button))); on_ = true; }
    Write(true);
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_NCHITTEST: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(window, &p);
        if (In(TitleRect(), p) && !In(CloseRect(), p)) return HTCAPTION;
        return HTCLIENT;
    }
    case WM_NCLBUTTONDBLCLK: if (wparam == HTCAPTION) return 0; break;
    case WM_MOUSEACTIVATE: SetWindowPos(window, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); return MA_NOACTIVATE;
    case WM_SETCURSOR:
        if (LOWORD(lparam) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, hot_ >= 0 || drag_ >= 0 ? IDC_HAND : IDC_ARROW)); return TRUE; }
        break;
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: Paint(); return 0;
    case WM_MOUSEMOVE: {
        const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
        if (drag_ >= 0) { mouse_ = p; DragTo(p, (wparam & MK_SHIFT) != 0); return 0; }
        Hover(p); return 0;
    }
    case WM_MOUSELEAVE: if (drag_ < 0) { inside_ = false; hot_ = -1; hot_button_ = -1; close_hot_ = false; Invalidate(); } return 0;
    case WM_LBUTTONDOWN: {
        const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; Hover(p);
        if (close_hot_) { Close("its close button"); return 0; }
        if (hot_button_ >= 0) { Press(hot_button_); return 0; }
        if (hot_ >= 0) { drag_ = hot_; SetCapture(window); Invalidate(); }
        return 0;
    }
    case WM_LBUTTONUP:
        if (drag_ >= 0) { drag_ = -1; if (GetCapture() == window) ReleaseCapture(); Write(true); Hover({GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}); }
        return 0;
    case WM_CAPTURECHANGED: if (drag_ >= 0 && reinterpret_cast<HWND>(lparam) != window) { drag_ = -1; Write(true); } return 0;
    case WM_LBUTTONDBLCLK: {
        const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; Hover(p);
        if (hot_ < 0 && hot_button_ < 0 && In(GraphRect(), p)) AddAt(p, (wparam & MK_SHIFT) != 0);
        else if (hot_button_ >= 0) Press(hot_button_);
        return 0;
    }
    case WM_RBUTTONUP: { const POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; Hover(p); if (hot_ >= 0) RemoveAt(hot_); return 0; }
    case WM_MOUSEWHEEL: {
        POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)}; ScreenToClient(window, &p); Hover(p);
        if (hot_ >= 0 && static_cast<size_t>(hot_) < points_.size()) {
            const int notches = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA, step = (GET_KEYSTATE_WPARAM(wparam) & MK_SHIFT) ? 1 : 5;
            points_[static_cast<size_t>(hot_)].speed = NpcCombo::ClampSpeed(points_[static_cast<size_t>(hot_)].speed + notches * step);
            Write(true);
        }
        return 0;
    }
    case WM_DPICHANGED_AFTERPARENT: Invalidate(); return 0;
    case WM_CLOSE: Close("WM_CLOSE"); return 0;
    case WM_DESTROY: if (window_ == window) window_ = nullptr; return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// The hub window the page is in: its top window below the game window (the hub is a child of the game window, and
// layered) or below the desktop (offline preview). The curve window is a child of it, so it is drawn into the hub's
// own surface: a child of the game window of its own was drawn over by the game's swap chain (the game showed
// through a blank rectangle, 11-08).
HWND HubOf(HWND page) {
    const HWND desktop = GetDesktopWindow(), game = UiHost::Game();
    HWND w = page;
    for (int i = 0; w && i < 32; ++i) {
        const HWND parent = GetAncestor(w, GA_PARENT);
        if (!parent || parent == desktop || parent == game) return w;
        w = parent;
    }
    return page ? GetAncestor(page, GA_ROOT) : nullptr;
}
// Where the window opens: centred over the page, in the hub's client, kept inside it.
void PlaceOver(HWND page) {
    RECT pr{}; GetWindowRect(page, &pr);
    const int w = S(kWidth), h = S(kHeight);
    POINT at{(pr.left + pr.right - w) / 2, (pr.top + pr.bottom - h) / 2};
    if (const HWND parent = GetParent(window_)) {
        ScreenToClient(parent, &at);
        RECT pc{}; GetClientRect(parent, &pc);
        at.x = std::clamp(at.x, 0L, (std::max)(0L, pc.right - w)); at.y = std::clamp(at.y, 0L, (std::max)(0L, pc.bottom - h));
    }
    SetWindowPos(window_, HWND_TOP, at.x, at.y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
}
}  // namespace

void Open(HWND page, int node, Host host) {
    host_ = std::move(host); node_ = node; page_ = page; hot_ = drag_ = hot_button_ = -1; points_.clear(); title_.clear();
    if (!window_) {
        WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.style = CS_DBLCLKS; cls.hInstance = GetModuleHandleW(nullptr); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        cls.lpszClassName = kClass; cls.lpfnWndProc = &Proc;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
        const HWND hub = HubOf(page);
        if (!hub) return;
        window_ = CreateWindowExW(0, kClass, L"加速曲线", WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, 0, 0, 10, 10, hub, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) return;
        if (!Read()) { Close("its node could not be read"); return; }
        PlaceOver(page);
    } else {
        if (!Read()) { Close("its node could not be read"); return; }
        SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    }
    next_read_ = GetTickCount64() + 150;
    Invalidate();
}
int EditedNode() { return window_ ? node_ : -1; }
const char* LastClose() { return last_close_; }
void Close(const char* why) {
    if (window_) last_close_ = why;
    if (drag_ >= 0) { drag_ = -1; if (window_ && GetCapture() == window_) ReleaseCapture(); if (host_.write) host_.write(points_, on_, true); }
    const HWND window = window_; window_ = nullptr; node_ = -1; host_ = {};
    if (window) DestroyWindow(window);
    if (page_ && IsWindow(page_)) InvalidateRect(page_, nullptr, FALSE);
}
void Tick(HWND page) {
    if (!window_) return;
    if (!page || !IsWindowVisible(page)) { Close("the page was hidden"); return; }
    const ULONGLONG now = GetTickCount64();
    if (drag_ < 0 && now >= next_read_) { next_read_ = now + 150; if (!Read()) { Close("its node is gone"); return; } }
    if (GetWindow(window_, GW_HWNDPREV)) SetWindowPos(window_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}
HWND Window() { return window_; }
POINT PointAt(size_t index) { return index < points_.size() ? POINT{XOf(points_[index].t), YOf(points_[index].speed)} : POINT{-1, -1}; }
RECT Graph() { return GraphRect(); }
}  // namespace SpeedCurveWindow
