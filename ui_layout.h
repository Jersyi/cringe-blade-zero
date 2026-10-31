#pragma once
#include <windows.h>
#include <algorithm>
#include <initializer_list>
#include <string>
#include <vector>

// UI worker only. Shared spacing tokens and placement helpers for hub pages.
// Pages keep their own controls and messages; only geometry goes through here.
namespace UiLayout {
struct Metrics {
    int dpi = 96;
    int S(int value) const { return MulDiv(value, dpi, 96); }
    int Margin() const { return S(18); }
    int Gap() const { return S(10); }
    int Row() const { return S(34); }
    int Button() const { return S(36); }
    int Check() const { return S(28); }
    int Section() const { return S(28); }
    int Line() const { return S(21); }
};
inline Metrics For(HWND window) {
    const UINT dpi = window ? GetDpiForWindow(window) : 96;
    return Metrics{dpi ? static_cast<int>(dpi) : 96};
}
inline void Place(HWND control, int x, int y, int width, int height) {
    if (control) SetWindowPos(control, nullptr, x, y, (std::max)(1, width), (std::max)(1, height), SWP_NOZORDER | SWP_NOACTIVATE);
}
inline std::wstring Caption(HWND control) {
    const int length = control ? GetWindowTextLengthW(control) : 0;
    if (length <= 0) return {};
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1); text.resize(static_cast<size_t>(length)); return text;
}
// Measures text in the control's own font: width of the caption on one line,
// or the wrapped height for a given width.
inline SIZE Measure(HWND control, int width = 0, const wchar_t* text = nullptr) {
    SIZE result{0, 0}; if (!control) return result;
    HDC dc = GetDC(control); if (!dc) return result;
    const HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(control, WM_GETFONT, 0, 0)));
    const std::wstring caption = text ? std::wstring(text) : Caption(control);
    RECT area{0, 0, width > 0 ? width : 100000, 0};
    UINT flags = DT_CALCRECT | DT_NOPREFIX | DT_EXPANDTABS | (width > 0 ? DT_WORDBREAK | DT_EDITCONTROL : DT_SINGLELINE);
    DrawTextW(dc, caption.empty() ? L" " : caption.c_str(), -1, &area, flags);
    TEXTMETRICW metrics{}; GetTextMetricsW(dc, &metrics);
    SelectObject(dc, old); ReleaseDC(control, dc);
    result.cx = area.right; result.cy = (std::max)(static_cast<LONG>(metrics.tmHeight), area.bottom);
    return result;
}
inline int TextWidth(HWND control, const wchar_t* text = nullptr) { return Measure(control, 0, text).cx; }
inline int TextHeight(HWND control, int width, int minimum_lines = 1) {
    const SIZE size = Measure(control, width);
    const SIZE line = Measure(control, 0, L"Ag");
    return (std::max)(static_cast<int>(size.cy), static_cast<int>(line.cy) * minimum_lines);
}
// Combo boxes size their selection field from the item height, not the window.
inline void PlaceCombo(HWND combo, int x, int y, int width, int height, int dropdown) {
    if (!combo) return;
    const int item = (std::max)(1, height - 6);
    if (SendMessageW(combo, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0) != item) SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), item);
    Place(combo, x, y, width, height + dropdown);
}
// Check boxes are sized to their caption so clicks on empty space do nothing.
inline int CheckWidth(HWND check, const Metrics& m) { return TextWidth(check) + m.S(18 + 9 + 8); }
inline int ButtonWidth(HWND button, const Metrics& m) { return TextWidth(button) + m.S(36); }
// Places buttons in rows of equal width. A row wraps when a button would get
// narrower than its caption. Returns the bottom of the last row.
inline int Buttons(std::initializer_list<HWND> list, int x, int y, int width, const Metrics& m, int columns = 0) {
    std::vector<HWND> buttons; for (HWND button : list) if (button) buttons.push_back(button);
    if (buttons.empty()) return y;
    int widest = 0; for (HWND button : buttons) widest = (std::max)(widest, ButtonWidth(button, m));
    const int count = static_cast<int>(buttons.size()), gap = m.Gap();
    int per_row = columns > 0 ? (std::min)(columns, count) : count;
    while (per_row > 1 && (width - gap * (per_row - 1)) / per_row < widest) --per_row;
    const int rows = (count + per_row - 1) / per_row;
    for (int row = 0; row < rows; ++row) {
        const int first = row * per_row, last = (std::min)(count, first + per_row), cells = per_row;
        const int cell = (width - gap * (cells - 1)) / cells;
        for (int i = first; i < last; ++i) Place(buttons[static_cast<size_t>(i)], x + (i - first) * (cell + gap), y, cell, m.Button());
        y += m.Button() + (row + 1 < rows ? gap : 0);
    }
    return y;
}
// Left-to-right flow of fixed-width items that wraps at the right edge.
struct Flow {
    int left, right, x, y, line, gap, row_gap;
    Flow(int left_edge, int top, int width, int line_height, int spacing, int row_spacing)
        : left(left_edge), right(left_edge + width), x(left_edge), y(top), line(line_height), gap(spacing), row_gap(row_spacing) {}
    // Returns false when the item started a new row.
    bool Add(HWND control, int width, int height = 0, int offset = 0) {
        bool same_row = true;
        if (x > left && x + width > right) { x = left; y += line + row_gap; same_row = false; }
        width = (std::min)(width, right - x);
        Place(control, x, y + offset + (height > 0 ? (line - height) / 2 : 0), width, height > 0 ? height : line);
        x += width + gap; return same_row;
    }
    bool AddCombo(HWND combo, int width, int dropdown) {
        bool same_row = true;
        if (x > left && x + width > right) { x = left; y += line + row_gap; same_row = false; }
        width = (std::min)(width, right - x);
        PlaceCombo(combo, x, y, width, line, dropdown);
        x += width + gap; return same_row;
    }
    // Moves the cursor right by extra spacing between groups on one row.
    void Space(int extra) { if (x > left) x += extra; }
    void Break() { if (x > left) { x = left; y += line + row_gap; } }
    int Bottom() const { return x > left ? y + line : y - row_gap; }
};
inline BOOL CALLBACK FontProc(HWND child, LPARAM font) { SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE); return TRUE; }
// Sets one font on every descendant (pages share the theme's DPI font).
inline void UseFont(HWND root, HFONT font) { if (root && font) EnumChildWindows(root, FontProc, reinterpret_cast<LPARAM>(font)); }
// Buttons at their caption width (at least minimum), left to right, wrapping.
inline int ButtonRow(std::initializer_list<HWND> list, int x, int y, int width, const Metrics& m, int minimum) {
    Flow flow(x, y, width, m.Button(), m.Gap(), m.Gap());
    for (HWND button : list) if (button) flow.Add(button, (std::max)(minimum, ButtonWidth(button, m)));
    return flow.Bottom();
}
}
