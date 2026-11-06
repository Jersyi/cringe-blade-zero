#include "donate_page.h"
#include "debug_hub.h"
#include "donate_card.generated.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include <objbase.h>
#include <objidl.h>
#include <gdiplus.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <memory>
#include <string>
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")

namespace DonatePage {
namespace {
constexpr wchar_t kClass[] = L"PBZDonatePage_v1", kCardClass[] = L"PBZDonateCard_v1";
constexpr wchar_t kUrl[] = L"https://afdian.com/a/Jersyi1";
constexpr int kOpen = 401, kCopy = 402;
HWND window_ = nullptr, title_ = nullptr, free_ = nullptr, thanks_ = nullptr, support_ = nullptr, scan_ = nullptr;
HWND link_ = nullptr, open_ = nullptr, copy_ = nullptr, note_ = nullptr, card_ = nullptr;
std::unique_ptr<Gdiplus::Bitmap> image_;
bool image_tried_ = false, card_hot_ = false;
UiI18n::Language language_ = UiI18n::Language::SimplifiedChinese;
// Set by the browser thread: 1 opened, 2 failed (the page then copies the link instead).
volatile LONG open_result_ = 0;

std::wstring Tr(const wchar_t* text) { return UiI18n::Translate(text ? text : L""); }
HWND Control(const wchar_t* cls, const wchar_t* text, DWORD style, int id = 0, UiTheme::Font font = UiTheme::Font::Body) {
    const HWND control = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, window_,
        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (control) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(UiTheme::GetFont(font, window_)), FALSE);
        UiI18n::Bind(control, text);
    }
    return control;
}

// The card is a JPEG (the author's own image), decoded once into a premultiplied bitmap.
Gdiplus::Bitmap* Image() {
    if (image_tried_) return image_.get();
    image_tried_ = true;
    IStream* stream = SHCreateMemStream(DonateCardData::kJpeg, static_cast<UINT>(sizeof(DonateCardData::kJpeg)));
    if (!stream) return nullptr;
    std::unique_ptr<Gdiplus::Bitmap> decoded(Gdiplus::Bitmap::FromStream(stream));
    stream->Release();
    if (!decoded || decoded->GetLastStatus() != Gdiplus::Ok) return nullptr;
    const INT width = static_cast<INT>(decoded->GetWidth()), height = static_cast<INT>(decoded->GetHeight());
    auto copy = std::make_unique<Gdiplus::Bitmap>(width, height, PixelFormat32bppPARGB);
    if (copy->GetLastStatus() != Gdiplus::Ok) return nullptr;
    { Gdiplus::Graphics graphics(copy.get()); graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy); graphics.DrawImage(decoded.get(), 0, 0, width, height); }
    image_ = std::move(copy);
    return image_.get();
}
Gdiplus::Color Argb(COLORREF color, BYTE alpha = 255) { return Gdiplus::Color(alpha, GetRValue(color), GetGValue(color), GetBValue(color)); }
void RoundedPath(Gdiplus::GraphicsPath& path, float x, float y, float w, float h, float radius) {
    const float d = (std::min)(radius * 2, (std::min)(w, h));
    path.AddArc(x, y, d, d, 180, 90); path.AddArc(x + w - d, y, d, d, 270, 90);
    path.AddArc(x + w - d, y + h - d, d, d, 0, 90); path.AddArc(x, y + h - d, d, d, 90, 90); path.CloseFigure();
}
void PaintCard(HWND window) {
    PAINTSTRUCT paint{}; const HDC target = BeginPaint(window, &paint);
    RECT client{}; GetClientRect(window, &client);
    const int width = (std::max)(1L, client.right), height = (std::max)(1L, client.bottom);
    const HDC dc = CreateCompatibleDC(target);
    const HBITMAP bitmap = dc ? CreateCompatibleBitmap(target, width, height) : nullptr;
    const HGDIOBJ old = bitmap ? SelectObject(dc, bitmap) : nullptr;
    const HDC surface = bitmap ? dc : target;
    FillRect(surface, &client, UiTheme::Background());
    {
        Gdiplus::Graphics graphics(surface);
        graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
        graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
        const auto m = UiLayout::For(window);
        const float radius = static_cast<float>(m.S(14)), inset = 1.5f;
        // Fit the card inside the window, keeping its aspect.
        float w = static_cast<float>(width) - 2 * inset, h = w * DonateCardData::kHeight / DonateCardData::kWidth;
        if (h > static_cast<float>(height) - 2 * inset) { h = static_cast<float>(height) - 2 * inset; w = h * DonateCardData::kWidth / DonateCardData::kHeight; }
        const float x = (static_cast<float>(width) - w) / 2, y = (static_cast<float>(height) - h) / 2;
        Gdiplus::GraphicsPath path; RoundedPath(path, x, y, w, h, radius);
        if (Gdiplus::Bitmap* image = Image()) {
            graphics.SetClip(&path);
            graphics.DrawImage(image, Gdiplus::RectF(x, y, w, h));
            graphics.ResetClip();
        } else {
            Gdiplus::SolidBrush brush(Argb(UiTheme::Panel())); graphics.FillPath(&brush, &path);
        }
        Gdiplus::Pen pen(Argb(card_hot_ ? UiTheme::Accent() : UiTheme::Border(), card_hot_ ? 230 : 160), card_hot_ ? 2.f : 1.f);
        graphics.DrawPath(&pen, &path);
    }
    if (bitmap) { BitBlt(target, 0, 0, width, height, dc, 0, 0, SRCCOPY); SelectObject(dc, old); DeleteObject(bitmap); }
    if (dc) DeleteDC(dc);
    EndPaint(window, &paint);
}

// ---- actions ---------------------------------------------------------------------------
bool CopyLink() {
    if (!OpenClipboard(window_)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = sizeof(kUrl);
        if (HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            if (void* data = GlobalLock(memory)) {
                memcpy(data, kUrl, bytes); GlobalUnlock(memory);
                ok = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
            }
            if (!ok) GlobalFree(memory);
        }
    }
    CloseClipboard();
    return ok;
}
// ShellExecute can take seconds (browser start, shell extensions): never on the UI worker.
DWORD WINAPI OpenThread(void*) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const HINSTANCE result = ShellExecuteW(nullptr, L"open", kUrl, nullptr, nullptr, SW_SHOWNORMAL);
    InterlockedExchange(&open_result_, reinterpret_cast<INT_PTR>(result) > 32 ? 1 : 2);
    if (SUCCEEDED(com)) CoUninitialize();
    return 0;
}
void OpenLink() {
    InterlockedExchange(&open_result_, 0);
    if (const HANDLE thread = CreateThread(nullptr, 0, &OpenThread, nullptr, 0, nullptr)) {
        CloseHandle(thread);
        DebugHub::Notify(Tr(L"正在浏览器中打开爱发电页面…").c_str());
    } else {
        InterlockedExchange(&open_result_, 2);
    }
}

// ---- layout ----------------------------------------------------------------------------
void Show(HWND control, bool show) { if (control && (IsWindowVisible(control) != FALSE) != show) ShowWindow(control, show ? SW_SHOWNA : SW_HIDE); }
void Layout() {
    if (!window_ || !title_) return;
    const auto m = UiLayout::For(window_);
    const int x = m.Margin(), gap = m.Gap();
    // Sized from the viewport, not this page: the page starts at the Attach minimum (460 wide, which would
    // stack the columns and flash a scroll rail) and grows to its own minimum, which would feed back here.
    RECT view{}; GetClientRect(GetParent(window_), &view);
    const int w = (std::max)(m.S(300), static_cast<int>(view.right) - 2 * x);
    // Card at its natural reading size (QR scannable from a phone), smaller only in a short hub.
    const int card_h = std::clamp(static_cast<int>(view.bottom) - 2 * x, m.S(360), m.S(520));
    const int card_w = card_h * DonateCardData::kWidth / DonateCardData::kHeight;
    const int text_min = m.S(320), side = m.S(28);
    const bool columns = w >= card_w + side + text_min;
    const int tx = columns ? x + card_w + side : x, tw = columns ? w - card_w - side : w;
    int y = x;
    UiLayout::Place(title_, tx, y, tw, m.Section()); y += m.Section() + gap / 2;
    const int free_h = UiLayout::TextHeight(free_, tw - m.S(28)) + m.S(20);
    UiLayout::Place(free_, tx, y, tw, free_h); y += free_h + gap;
    const int thanks_h = UiLayout::TextHeight(thanks_, tw);
    UiLayout::Place(thanks_, tx, y, tw, thanks_h); y += thanks_h + gap + gap / 2;
    UiLayout::Place(support_, tx, y, tw, m.Section()); y += m.Section() + gap / 2;
    const int scan_h = UiLayout::TextHeight(scan_, tw);
    UiLayout::Place(scan_, tx, y, tw, scan_h); y += scan_h + gap / 2;
    const int link_h = UiLayout::TextHeight(link_, tw);
    UiLayout::Place(link_, tx, y, tw, link_h); y += link_h + gap;
    const int open_w = (std::max)(m.S(150), UiLayout::TextWidth(open_) + m.S(36));
    const int copy_w = (std::max)(m.S(110), UiLayout::TextWidth(copy_) + m.S(36));
    if (open_w + gap + copy_w <= tw) {
        UiLayout::Place(open_, tx, y, open_w, m.Button()); UiLayout::Place(copy_, tx + open_w + gap, y, copy_w, m.Button());
        y += m.Button() + gap;
    } else {
        UiLayout::Place(open_, tx, y, (std::min)(tw, open_w), m.Button()); y += m.Button() + gap;
        UiLayout::Place(copy_, tx, y, (std::min)(tw, copy_w), m.Button()); y += m.Button() + gap;
    }
    const int note_h = UiLayout::TextHeight(note_, tw);
    UiLayout::Place(note_, tx, y, tw, note_h); y += note_h;
    int bottom = y;
    if (columns) {
        UiLayout::Place(card_, x, x, card_w, card_h);
        bottom = (std::max)(bottom, x + card_h);
    } else {
        y += gap * 2;
        UiLayout::Place(card_, x + (w - card_w) / 2, y, card_w, card_h);
        bottom = y + card_h;
    }
    Show(card_, true);
    DebugHub::SetMinimum(window_, m.S(300) + 2 * x, bottom + x);
    // Themed statics draw without erasing: moved ones would leave their old text behind.
    RedrawWindow(window_, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
}

LRESULT CALLBACK CardProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintCard(window); return 0;
    case WM_SIZE: InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_MOUSEMOVE:
        if (!card_hot_) { card_hot_ = true; InvalidateRect(window, nullptr, FALSE); TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0}; TrackMouseEvent(&track); }
        return 0;
    case WM_MOUSELEAVE: card_hot_ = false; InvalidateRect(window, nullptr, FALSE); return 0;
    case WM_SETCURSOR: if (LOWORD(lparam) == HTCLIENT) { SetCursor(LoadCursorW(nullptr, IDC_HAND)); return TRUE; } break;
    case WM_LBUTTONUP: OpenLink(); return 0;
    default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_COMMAND:
        if (LOWORD(wparam) == IDCANCEL) { DebugHub::Hide(); return 0; }
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kOpen) { OpenLink(); return 0; }
        if (HIWORD(wparam) == BN_CLICKED && LOWORD(wparam) == kCopy) {
            DebugHub::Notify(Tr(CopyLink() ? L"已复制爱发电链接" : L"没能写入剪贴板，请手动输入链接").c_str()); return 0;
        }
        return 0;
    case WM_SIZE: Layout(); return 0;
    case WM_DPICHANGED_AFTERPARENT:
        UiLayout::UseFont(window, UiTheme::GetFont(UiTheme::Font::Body, window));
        if (link_) SendMessageW(link_, WM_SETFONT, reinterpret_cast<WPARAM>(UiTheme::GetFont(UiTheme::Font::Strong, window)), FALSE);
        Layout(); return 0;
    case WM_CLOSE: DebugHub::Hide(); return 0;
    case WM_DESTROY: window_ = nullptr; return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
    }
}
bool RegisterClasses() {
    WNDCLASSEXW cls{}; cls.cbSize = sizeof(cls); cls.hInstance = GetModuleHandleW(nullptr); cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    struct { const wchar_t* name; WNDPROC proc; HBRUSH brush; } classes[] = {
        {kClass, &Proc, reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1)}, {kCardClass, &CardProc, nullptr}};
    for (const auto& c : classes) {
        cls.lpszClassName = c.name; cls.lpfnWndProc = c.proc; cls.hbrBackground = c.brush;
        if (!RegisterClassExW(&cls) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    }
    return true;
}
}

HWND CreatePage(HWND parent) {
    if (!parent) return nullptr;
    if (!window_) {
        if (!RegisterClasses()) return nullptr;
        window_ = CreateWindowExW(0, kClass, L"Donate", WS_CHILD | WS_CLIPCHILDREN, 0, 0, 800, 650, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!window_) return nullptr;
        title_ = Control(L"STATIC", L"这个 MOD 完全免费", SS_LEFT);
        free_ = Control(L"STATIC", L"豪之刃是免费 MOD，不打赏也可以使用全部功能，没有任何功能需要付费或打赏才能解锁。", SS_LEFT);
        thanks_ = Control(L"STATIC", L"如果你喜欢它、想支持作者继续更新，可以在爱发电为我发电。打赏完全自愿，非常感谢！", SS_LEFT);
        support_ = Control(L"STATIC", L"爱发电（自愿）", SS_LEFT);
        scan_ = Control(L"STATIC", L"用手机扫描二维码，或点下面的按钮在浏览器中打开：", SS_LEFT);
        link_ = Control(L"STATIC", kUrl, SS_LEFT, 0, UiTheme::Font::Strong);
        open_ = Control(L"BUTTON", L"打开爱发电页面", BS_DEFPUSHBUTTON | WS_TABSTOP, kOpen);
        copy_ = Control(L"BUTTON", L"复制链接", BS_PUSHBUTTON | WS_TABSTOP, kCopy);
        note_ = Control(L"STATIC", L"游戏全屏时，浏览器可能在游戏后面打开，按 Alt+Tab 切换过去。点二维码卡片也能打开。", SS_LEFT);
        card_ = CreateWindowExW(0, kCardClass, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, window_, nullptr, GetModuleHandleW(nullptr), nullptr);
        UiTheme::SetRole(title_, UiTheme::Role::Section);
        UiTheme::SetRole(free_, UiTheme::Role::Card);
        UiTheme::SetRole(thanks_, UiTheme::Role::Hint);
        UiTheme::SetRole(support_, UiTheme::Role::Section);
        UiTheme::SetRole(scan_, UiTheme::Role::Hint);
        UiTheme::SetRole(note_, UiTheme::Role::Hint);
        language_ = UiI18n::GetLanguage();
        DebugHub::Attach(window_, parent, 460, 600);
    }
    Layout(); return window_;
}
void Tick() {
    // The browser thread's result: a failed open copies the link so it can be pasted by hand.
    if (const LONG result = InterlockedExchange(&open_result_, 0); result == 2 && window_) {
        DebugHub::Notify(Tr(CopyLink() ? L"没能打开浏览器，链接已复制，请粘贴到浏览器" : L"没能打开浏览器，请手动输入链接").c_str());
    }
    if (!window_ || !IsWindowVisible(window_)) return;
    if (UiI18n::GetLanguage() != language_) { language_ = UiI18n::GetLanguage(); Layout(); }
}
void Shutdown() {
    if (window_) DestroyWindow(window_);
    window_ = title_ = free_ = thanks_ = support_ = scan_ = link_ = open_ = copy_ = note_ = card_ = nullptr;
    image_.reset(); image_tried_ = false;
}
}
