#include "boss_ai_menu.h"
#include "boss_ai.h"
#include "debug_hub.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include <windows.h>
#include <commctrl.h>
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>
#pragma comment(lib, "comctl32.lib")

namespace BossAIMenu {
namespace {
constexpr wchar_t kClass[] = L"PBZ.DebugDLL.Boss.AdvancedAI";
constexpr int kList = 4201, kSearch = 4202, kFilter = 4203, kValue = 4204;
constexpr int kApply = 4210, kRefresh = 4211, kRestore = 4212, kClose = 4213;
HWND window_ = nullptr, return_window_ = nullptr, target_ = nullptr, list_ = nullptr, search_ = nullptr, filter_ = nullptr;
HWND detail_ = nullptr, value_ = nullptr, range_ = nullptr, apply_ = nullptr, refresh_ = nullptr, restore_ = nullptr, close_ = nullptr;
HWND search_label_ = nullptr, value_label_ = nullptr, status_ = nullptr;
HFONT font_ = nullptr;
BossAI::Snapshot snapshot_;
std::vector<BossAI::Parameter> displayed_;
bool shown_ = false, updating_ = false;
int dpi_ = 96;
ULONGLONG next_update_ = 0, notice_until_ = 0;
std::wstring notice_;

int S(int value) { return MulDiv(value, dpi_, 96); }
std::wstring Text(HWND window) {
    const int count = window ? GetWindowTextLengthW(window) : 0;
    if (count <= 0 || count > 8192) return {};
    std::vector<wchar_t> value(static_cast<size_t>(count) + 1);
    GetWindowTextW(window, value.data(), static_cast<int>(value.size()));
    return value.data();
}
std::wstring Number(float value) {
    wchar_t text[80]{};
    swprintf_s(text, L"%.7g", static_cast<double>(value));
    return text;
}
std::wstring Lower(std::wstring text) {
    for (wchar_t& value : text) value = static_cast<wchar_t>(std::towlower(value));
    return text;
}
const wchar_t* KindName(BossAI::Kind kind) {
    switch (kind) {
    case BossAI::Kind::Probability: return L"概率";
    case BossAI::Kind::Weight: return L"相对权重";
    case BossAI::Kind::BehaviorCooldown: return L"行为冷却";
    case BossAI::Kind::RemainingCooldown: return L"剩余冷却";
    }
    return L"";
}
void Notice(const wchar_t* text) { notice_ = text; notice_until_ = GetTickCount64() + 7000; }
int32_t SelectedIndex() {
    const int row = ListView_GetNextItem(list_, -1, LVNI_SELECTED);
    if (row < 0) return -1;
    LVITEMW item{};
    item.mask = LVIF_PARAM; item.iItem = row;
    return ListView_GetItem(list_, &item) ? static_cast<int32_t>(item.lParam) : -1;
}
const BossAI::Parameter* Selected() {
    const int32_t index = SelectedIndex();
    const auto row = std::find_if(snapshot_.parameters.begin(), snapshot_.parameters.end(),
        [index](const BossAI::Parameter& item) { return item.index == index; });
    return row == snapshot_.parameters.end() ? nullptr : &*row;
}
void UpdateSelection(bool replace_value) {
    const BossAI::Parameter* parameter = Selected();
    const bool available = parameter && snapshot_.ready;
    EnableWindow(apply_, available);
    EnableWindow(value_, available);
    if (!parameter) {
        UiI18n::SetText(detail_, snapshot_.ready ? L"选择一个真实参数，查看类型、单位和实例来源。" :
            L"当前没有绑定可用 Boss AI。进入 Boss 战并等待原生生成器产生战斗角色后，这里才会显示该实例实际提供的参数。");
        UiI18n::SetText(range_, L"没有可编辑的实例参数");
        if (replace_value) UiI18n::SetText(value_, L"");
        return;
    }
    const std::wstring type = KindName(parameter->kind);
    std::wstring text = L"真实 Key：" + parameter->key + L"\r\n类型：float32 / " + type +
        L"    单位：" + parameter->unit + L"    当前值（上次读取）：" + Number(parameter->current) +
        L"    基线：" + Number(parameter->baseline) + L"\r\n实例来源：" + parameter->route;
    if (parameter->changed) text += L"\r\n本项已修改；恢复全部会还原所跟踪的实例值。";
    UiI18n::SetText(detail_, text.c_str());
    const std::wstring range = L"可输入 " + Number(parameter->minimum) + L" 至 " + Number(parameter->maximum) +
        (parameter->kind == BossAI::Kind::Probability ? L"，概率使用 0–1。" : L"（界面约束，不代表原生硬上限）。");
    UiI18n::SetText(range_, range.c_str());
    if (replace_value) UiI18n::SetText(value_, Number(parameter->current).c_str());
}
bool SameRows(const std::vector<BossAI::Parameter>& values) {
    if (values.size() != displayed_.size()) return false;
    for (size_t i = 0; i < values.size(); ++i) {
        const auto& a = values[i]; const auto& b = displayed_[i];
        if (a.index != b.index || a.kind != b.kind || a.key != b.key || a.label != b.label || a.unit != b.unit ||
            a.route != b.route || a.current != b.current || a.baseline != b.baseline || a.changed != b.changed) return false;
    }
    return true;
}
void Cell(int row, int column, const std::wstring& value) {
    if (column==0||column==4) UiI18n::SetCell(list_,row,column,value.c_str());
    else ListView_SetItemText(list_,row,column,const_cast<wchar_t*>(value.c_str()));
}
void Rebuild(int32_t wanted, bool target_changed) {
    const std::wstring search = Lower(Text(search_));
    const LRESULT filter = SendMessageW(filter_, CB_GETCURSEL, 0, 0);
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    int selected_row = -1, visible = 0;
    for (const auto& parameter : snapshot_.parameters) {
        if (filter > 0 && static_cast<int>(parameter.kind) + 1 != filter) continue;
        if (!search.empty() && Lower(parameter.label + L" " + parameter.key + L" " + parameter.route).find(search) == std::wstring::npos) continue;
        std::wstring label = (parameter.changed ? L"* " : L"") + parameter.label;
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = visible; item.pszText = label.data(); item.lParam = parameter.index;
        const int row = ListView_InsertItem(list_, &item);
        if (row < 0) continue;
        Cell(row,0,label);
        Cell(row, 1, parameter.key);
        Cell(row, 2, Number(parameter.current));
        Cell(row, 3, Number(parameter.baseline));
        Cell(row, 4, parameter.unit);
        Cell(row, 5, parameter.route);
        if (parameter.index == wanted) selected_row = row;
        ++visible;
    }
    if (selected_row < 0 && visible > 0) selected_row = 0;
    if (selected_row >= 0) {
        ListView_SetItemState(list_, selected_row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
        ListView_EnsureVisible(list_, selected_row, FALSE);
    }
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
    UpdateSelection(target_changed || GetFocus() != value_);
}
void Update() {
    if (!window_) return;
    updating_ = true;
    const int32_t selected = SelectedIndex();
    BossAI::Snapshot next = BossAI::GetSnapshot();
    const bool changed_target = snapshot_.generation != next.generation;
    const bool changed_rows = changed_target || !SameRows(next.parameters);
    snapshot_ = std::move(next);
    if (changed_rows) {
        displayed_ = snapshot_.parameters;
        Rebuild(changed_target ? -1 : selected, changed_target);
        if (changed_target && (shown_||(DebugHub::IsEmbedded(window_)&&IsWindowVisible(window_)))) Notice(L"绑定的 Boss 实例已改变；旧输入已清除。 ");
    } else UpdateSelection(false);
    const std::wstring target = snapshot_.target.empty() ? L"当前目标：未绑定 Boss AI" :
        L"当前目标：" + snapshot_.target + L"    |    实际可用参数 " + std::to_wstring(snapshot_.parameters.size()) + L" 项";
    UiI18n::SetText(target_, target.c_str());
    EnableWindow(refresh_, !snapshot_.target.empty());
    EnableWindow(restore_, std::any_of(snapshot_.parameters.begin(), snapshot_.parameters.end(), [](const BossAI::Parameter& p) { return p.changed; }));
    std::wstring status = snapshot_.status;
    if (notice_until_ > GetTickCount64()) status = notice_ + L"\r\n" + status;
    else status += L"\r\n点击刷新读取当前值；数值读回通过后，仍需在战斗中观察实际攻击频率。";
    UiI18n::SetText(status_, status.c_str());
    updating_ = false;
}
void Apply() {
    const BossAI::Parameter* parameter = Selected();
    if (!parameter || !snapshot_.ready) { Notice(L"当前没有可编辑的 Boss 实例参数。 "); return; }
    const std::wstring text = Text(value_);
    wchar_t* end = nullptr;
    errno = 0;
    const float value = wcstof(text.c_str(), &end);
    while (end && *end && std::iswspace(*end)) ++end;
    if (text.empty() || !end || end == text.c_str() || *end || errno == ERANGE || !std::isfinite(value) ||
        value < parameter->minimum || value > parameter->maximum) {
        Notice(L"请输入当前参数范围内的有限数值；概率使用 0–1。 "); return;
    }
    if (BossAI::RequestSet(snapshot_.generation, parameter->index, value))
        Notice(L"已提交到游戏线程；下方状态与刷新后的当前值显示实际结果。 ");
    else Notice(L"请求队列已满，请稍后重试。 ");
}
void Hide() {
    if (DebugHub::IsEmbedded(window_)) { DebugHub::Hide(); return; }
    shown_ = false;
    if (window_) ShowWindow(window_, SW_HIDE);
    if (return_window_ && IsWindow(return_window_)) SetForegroundWindow(return_window_);
}
HWND Control(const wchar_t* class_name, const wchar_t* text, DWORD style, int id, DWORD extended = 0) {
    HWND child = CreateWindowExW(extended, class_name, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (child && font_) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
    UiI18n::Bind(child,text);
    return child;
}
BOOL CALLBACK SetChildFont(HWND child, LPARAM font) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE); return TRUE;
}
void Font() {
    font_ = UiTheme::GetFont(UiTheme::Font::Body, static_cast<UINT>(dpi_));
    if (window_ && font_) EnumChildWindows(window_, SetChildFont, reinterpret_cast<LPARAM>(font_));
}
void Layout() {
    if (!window_) return;
    RECT rect{};
    GetClientRect(window_, &rect);
    // The hub supplies physical client pixels; only fonts and rows scale with DPI.
    const int width = rect.right, height = rect.bottom;
    const UiLayout::Metrics m{dpi_};
    const int margin = m.Margin(), gap = m.Gap();
    const int content = (std::max)(1, width - margin * 2), line = m.Line();
    const int edit_height = m.Row(), button_height = m.Button();
    const int target_height = 2 * line + 6, search_top = margin + target_height + gap;
    const int search_label_width = (std::min)(UiLayout::TextWidth(search_label_) + gap, content / 6);
    const int filter_width = (std::min)(std::clamp(content / 3, 200, 320), content / 2);
    const int search_width = (std::max)(1, content - search_label_width - filter_width - 2 * gap);
    MoveWindow(target_, margin, margin, content, target_height, TRUE);
    MoveWindow(search_label_, margin, search_top + (edit_height - line) / 2, search_label_width, line, TRUE);
    MoveWindow(search_, margin + search_label_width + gap, search_top, search_width, edit_height, TRUE);
    UiLayout::PlaceCombo(filter_, width - margin - filter_width, search_top, filter_width, edit_height, S(220));

    const int status_height = 2 * line + S(14), status_top = height - margin - status_height;
    const int buttons_top = status_top - gap - button_height;
    const int value_height = 2 * line + 4, value_top = buttons_top - gap - value_height;
    const int detail_height = 3 * line + S(14), detail_top = value_top - gap - detail_height;
    const int list_top = search_top + edit_height + gap;
    MoveWindow(list_, margin, list_top, content, (std::max)(1, detail_top - gap - list_top), TRUE);
    MoveWindow(detail_, margin, detail_top, content, detail_height, TRUE);
    const int value_label_width = (std::min)(UiLayout::TextWidth(value_label_) + gap, content / 5);
    const int value_width = (std::min)(std::clamp(content / 5, 110, 190), content / 3);
    const int range_left = margin + value_label_width + gap + value_width + gap;
    MoveWindow(value_label_, margin, value_top + (edit_height - line) / 2, value_label_width, line, TRUE);
    MoveWindow(value_, margin + value_label_width + gap, value_top, value_width, edit_height, TRUE);
    MoveWindow(range_, range_left, value_top, (std::max)(1, width - margin - range_left), value_height, TRUE);
    const int button_width = (std::max)(1, (content - 3 * gap) / 4);
    const HWND buttons[] = {apply_, refresh_, restore_, close_};
    for (int i = 0; i < 4; ++i)
        MoveWindow(buttons[i], margin + i * (button_width + gap), buttons_top, button_width, button_height, TRUE);
    MoveWindow(status_, margin, status_top, content, status_height, TRUE);

    // Long provenance scrolls inside the table rather than widening the page.
    const int fractions[] = {25, 24, 14, 12, 9};
    for (int i = 0; i < 5; ++i) ListView_SetColumnWidth(list_, i, content * fractions[i] / 100);
    ListView_SetColumnWidth(list_, 5, (std::max)(260, S(240)));
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { Hide(); return 0; }
    if (message == WM_SIZE) { Layout(); return 0; }
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT minimum{0, 0, 720, 600};
        AdjustWindowRectExForDpi(&minimum, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), static_cast<UINT>(dpi_));
        limits->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top};
        return 0;
    }
    if (message == WM_DPICHANGED) {
        dpi_ = HIWORD(wparam); if (dpi_ <= 0) dpi_ = 96;
        if (!DebugHub::IsEmbedded(window)) {
            const auto* suggested = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                suggested->bottom - suggested->top, SWP_NOACTIVATE | SWP_NOZORDER);
        }
        Font(); Layout(); return 0;
    }
    if (message == WM_DPICHANGED_AFTERPARENT) {
        dpi_ = static_cast<int>(GetDpiForWindow(window)); if (dpi_ <= 0) dpi_ = 96;
        Font(); Layout(); return 0;
    }
    if (message == WM_COMMAND && !updating_) {
        const int id = LOWORD(wparam), notification = HIWORD(wparam);
        if (id == kApply || id == IDOK) Apply();
        else if (id == kRefresh) {
            if (BossAI::RequestRefresh(snapshot_.generation)) Notice(L"已请求重新读取原生实例参数。 ");
        } else if (id == kRestore) {
            if (BossAI::RequestRestore(snapshot_.generation)) Notice(L"已请求恢复此实例跟踪过的 AI 修改。 ");
        } else if (id == kClose || id == IDCANCEL) Hide();
        else if ((id == kSearch && notification == EN_CHANGE) || (id == kFilter && notification == CBN_SELCHANGE)) {
            updating_ = true; Rebuild(SelectedIndex(), false); updating_ = false;
        }
        return 0;
    }
    if (message == WM_NOTIFY && !updating_) {
        const auto* header = reinterpret_cast<const NMHDR*>(lparam);
        if (header->idFrom == kList && header->code == LVN_ITEMCHANGED) {
            const auto* changed = reinterpret_cast<const NMLISTVIEW*>(lparam);
            if ((changed->uNewState & LVIS_SELECTED) && !(changed->uOldState & LVIS_SELECTED)) UpdateSelection(true);
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool Create() {
    if (window_) return true;
    INITCOMMONCONTROLSEX common{sizeof(common), ICC_LISTVIEW_CLASSES};
    if (!InitCommonControlsEx(&common)) return false;
    WNDCLASSEXW window_class{};
    window_class.cbSize = sizeof(window_class); window_class.lpfnWndProc = Procedure;
    window_class.hInstance = GetModuleHandleW(nullptr); window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1); window_class.lpszClassName = kClass;
    if (!RegisterClassExW(&window_class) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"PBZ · Boss 高级 AI 参数（原生实例）",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, S(1180), S(790), nullptr, nullptr, window_class.hInstance, nullptr);
    if (!window_) return false;
    dpi_ = static_cast<int>(GetDpiForWindow(window_));
    if (dpi_ <= 0) dpi_ = 96;
    Font();
    target_ = Control(L"EDIT", L"当前目标：未绑定 Boss AI", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0);
    search_label_ = Control(L"STATIC", L"搜索", SS_LEFT, 0);
    search_ = Control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kSearch, WS_EX_CLIENTEDGE);
    UiI18n::Cue(search_,L"筛选真实 Key、行为名或实例来源");
    filter_ = Control(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, kFilter);
    for (const wchar_t* name : {L"全部实际参数", L"进攻 / 反应概率", L"反应 / 闪避权重", L"行为冷却定义", L"当前剩余冷却"})
        UiI18n::AddString(filter_, CB_ADDSTRING, 0, name);
    SendMessageW(filter_, CB_SETCURSEL, 0, 0);
    list_ = Control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, kList, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES | LVS_EX_LABELTIP);
    const wchar_t* labels[] = {L"参数 / 行为", L"真实 Key", L"当前值", L"基线", L"单位", L"实例来源"};
    constexpr int widths[] = {300, 260, 148, 122, 110, 430};
    for (int i = 0; i < static_cast<int>(std::size(labels)); ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        column.pszText = const_cast<wchar_t*>(labels[i]); column.cx = S(widths[i]); column.iSubItem = i;
        ListView_InsertColumn(list_, i, &column);
    }
    detail_ = Control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | WS_VSCROLL | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
    value_label_ = Control(L"STATIC", L"编辑值", SS_LEFT, 0);
    value_ = Control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kValue, WS_EX_CLIENTEDGE);
    SendMessageW(value_, EM_SETLIMITTEXT, 64, 0);
    range_ = Control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0);
    apply_ = Control(L"BUTTON", L"应用参数", BS_DEFPUSHBUTTON | WS_TABSTOP, kApply);
    refresh_ = Control(L"BUTTON", L"刷新", BS_PUSHBUTTON | WS_TABSTOP, kRefresh);
    restore_ = Control(L"BUTTON", L"恢复修改", BS_PUSHBUTTON | WS_TABSTOP, kRestore);
    close_ = Control(L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP, kClose);
    status_ = Control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0, WS_EX_CLIENTEDGE);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = (std::min)(S(1180), static_cast<int>(work.right - work.left - S(24)));
    const int height = (std::min)(S(790), static_cast<int>(work.bottom - work.top - S(24)));
    SetWindowPos(window_, nullptr, work.left + S(12), work.top + S(12), width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    Layout(); Update();
    return true;
}
}
void Initialize() { if (!DebugHub::Enabled()) Create(); }
void Show() {
    if (DebugHub::Enabled()) { DebugHub::ShowPage(DebugHub::Page::BossAI); return; }
    if (!Create()) return;
    HWND foreground = GetForegroundWindow();
    if (foreground != window_ && !IsChild(window_, foreground)) return_window_ = foreground;
    shown_ = true;
    Update(); ShowWindow(window_, SW_SHOWNORMAL); SetForegroundWindow(window_);
}
HWND CreatePage(HWND parent) {
    if (!Create()) return nullptr;
    DebugHub::Attach(window_,parent,720,600); Layout(); Update(); return window_;
}
void Tick() {
    if (!window_) return;
    if (!DebugHub::Enabled()) {
        MSG message{};
        while (PeekMessageW(&message, window_, 0, 0, PM_REMOVE)) {
            if (!IsDialogMessageW(window_, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
    }
    const bool visible=DebugHub::IsEmbedded(window_)?IsWindowVisible(window_)!=FALSE:shown_;
    if (visible && GetTickCount64() >= next_update_) { next_update_ = GetTickCount64() + 250; Update(); }
}
void Shutdown() {
    shown_ = false;
    if (window_) DestroyWindow(window_);
    window_ = nullptr;
    font_ = nullptr; // Owned by UiTheme.
    UnregisterClassW(kClass, GetModuleHandleW(nullptr));
}
}
