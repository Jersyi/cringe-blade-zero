#include "boss_menu.h"
#include "boss_rush.h"
#include "character_swap.h"
#include "boss_ai_menu.h"
#include "npc_menu.h"
#include "air_skills_menu.h"
#include "motian_grab_menu.h"
#include "debug_hub.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include <windows.h>
#include <algorithm>
#include <cwchar>
#include <cwctype>
#include <iterator>
#include <string>
#include <vector>

namespace BossMenu {
namespace {
constexpr int kSearch = 2001, kMap = 2002, kList = 2003, kOnlyAvailable = 2004;
constexpr int kTeleport = 2010, kStart = 2011, kNext = 2012, kRetry = 2013, kStop = 2014, kCharacter = 2015;
constexpr int kDirectFight = 2016;
constexpr int kCycleOverride = 2020, kCycle = 2021, kDifficultyOverride = 2022, kDifficulty = 2023;
constexpr int kAutoNext = 2024, kHeal = 2025, kApply = 2026;
constexpr int kAdvancedAI = 2027;
constexpr int kAirSkills = 2028;
constexpr int kMotianGrab = 2029;
HWND window_ = nullptr, return_window_ = nullptr, search_ = nullptr, maps_ = nullptr, list_ = nullptr;
HWND detail_ = nullptr, status_ = nullptr, only_available_ = nullptr, teleport_ = nullptr, start_ = nullptr;
HWND next_ = nullptr, retry_ = nullptr, stop_ = nullptr, cycle_override_ = nullptr, cycle_ = nullptr;
HWND direct_fight_ = nullptr;
HWND intro_ = nullptr, search_label_ = nullptr, map_label_ = nullptr, options_section_ = nullptr, links_section_ = nullptr;
HWND character_button_ = nullptr, ai_button_ = nullptr, air_button_ = nullptr, grab_button_ = nullptr;
HWND difficulty_override_ = nullptr, difficulty_ = nullptr, auto_next_ = nullptr, heal_ = nullptr, apply_ = nullptr, strength_ = nullptr;
HFONT font_ = nullptr;
bool shown_ = false, key_down_ = false, editing_ = false, initialized_options_ = false;
ULONGLONG next_update_ = 0, message_until_ = 0;
std::wstring message_;
std::vector<BossRush::Entry> entries_;
std::vector<BossRush::Difficulty> difficulties_;
std::vector<std::wstring> map_names_;
BossRush::Snapshot snapshot_;
HWND character_window_ = nullptr, character_list_ = nullptr, character_detail_ = nullptr, character_status_ = nullptr;
HWND character_advanced_ = nullptr, character_apply_ = nullptr, character_restore_ = nullptr;
HWND character_intro_ = nullptr, character_npc_ = nullptr, character_note_ = nullptr;
bool character_shown_ = false, character_catalog_built_ = false;
CharacterSwap::Snapshot character_snapshot_;
constexpr int kCharacterList = 3001, kCharacterAdvanced = 3002, kCharacterApply = 3003, kCharacterRestore = 3004;
constexpr int kNpcModels = 3005;
void ShowCharacterWindow();

bool Foreground() {
    DWORD process = 0; GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}
bool Checked(HWND control) { return SendMessageW(control, BM_GETCHECK, 0, 0) == BST_CHECKED; }
void Check(HWND control, bool checked) { SendMessageW(control, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0); }
std::wstring Text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length <= 0 || length > 4096) return {};
    std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
    GetWindowTextW(control, buffer.data(), static_cast<int>(buffer.size()));
    return buffer.data();
}
std::wstring Lower(std::wstring text) {
    for (wchar_t& value : text) value = static_cast<wchar_t>(std::towlower(value));
    return text;
}
void Notice(const wchar_t* text) { message_ = text; message_until_ = GetTickCount64() + 6000; }
int32_t Selection() {
    const LRESULT row = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    return row == LB_ERR ? -1 : static_cast<int32_t>(SendMessageW(list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0));
}
const BossRush::Entry* SelectedEntry() {
    const int32_t selected = Selection();
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const BossRush::Entry& entry) { return entry.index == selected; });
    return found == entries_.end() ? nullptr : &*found;
}
void UpdateSelection() {
    const BossRush::Entry* selected = SelectedEntry();
    std::wstring text;
    if (selected) {
        text = selected->name + L"  |  " + selected->map + L"\r\n" + selected->detail;
        if (!selected->available) text += L"\r\n这个遭遇的传送入口尚未确认，已禁用传送与连战入口。";
    } else text = L"选择一个 Boss 查看地图与入口说明。";
    if (Text(detail_) != text) UiI18n::SetText(detail_, text.c_str());
    const bool can_start = snapshot_.ready && !snapshot_.busy && selected && selected->available;
    EnableWindow(teleport_, can_start); EnableWindow(start_, can_start); EnableWindow(direct_fight_, can_start);
}
void Filter() {
    const int32_t selected = Selection();
    const std::wstring search = Lower(Text(search_));
    const LRESULT map_row = SendMessageW(maps_, CB_GETCURSEL, 0, 0);
    const std::wstring map = map_row > 0 && static_cast<size_t>(map_row) <= map_names_.size() ? map_names_[static_cast<size_t>(map_row) - 1] : L"";
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0); SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    int selected_row = -1, first_available = -1;
    for (const BossRush::Entry& entry : entries_) {
        if (Checked(only_available_) && !entry.available) continue;
        if (!map.empty() && map != entry.map) continue;
        const std::wstring id(entry.id.begin(), entry.id.end());
        if (!search.empty() && Lower(entry.name + L" " + entry.map + L" " + id).find(search) == std::wstring::npos) continue;
        const std::wstring row_text = (entry.available ? L"[可传送]  " : L"[入口待确认]  ") + entry.name + L"    |    " + entry.map;
        const LRESULT row = UiI18n::AddString(list_, LB_ADDSTRING, 0, row_text.c_str());
        if (row == LB_ERR || row == LB_ERRSPACE) continue;
        SendMessageW(list_, LB_SETITEMDATA, static_cast<WPARAM>(row), entry.index);
        if (selected == entry.index) selected_row = static_cast<int>(row);
        if (first_available < 0 && entry.available) first_available = static_cast<int>(row);
    }
    if (selected_row < 0) selected_row = first_available >= 0 ? first_available : 0;
    SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(selected_row), 0);
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0); InvalidateRect(list_, nullptr, TRUE);
    UpdateSelection();
}
bool CatalogChanged(const std::vector<BossRush::Entry>& entries) {
    if (entries.size() != entries_.size()) return true;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].index != entries_[i].index || entries[i].name != entries_[i].name || entries[i].map != entries_[i].map ||
            entries[i].detail != entries_[i].detail || entries[i].available != entries_[i].available) return true;
    return false;
}
void UpdateCatalog(const std::vector<BossRush::Entry>& entries) {
    const std::wstring prior_map = Text(maps_);
    entries_ = entries; map_names_.clear();
    for (const auto& entry : entries_) if (!entry.map.empty() && std::find(map_names_.begin(), map_names_.end(), entry.map) == map_names_.end()) map_names_.push_back(entry.map);
    std::sort(map_names_.begin(), map_names_.end());
    SendMessageW(maps_, CB_RESETCONTENT, 0, 0);
    UiI18n::AddString(maps_, CB_ADDSTRING, 0, L"全部地图");
    int selected = 0;
    for (size_t i = 0; i < map_names_.size(); ++i) {
        UiI18n::AddString(maps_, CB_ADDSTRING, 0, map_names_[i].c_str());
        if (map_names_[i] == prior_map) selected = static_cast<int>(i + 1);
    }
    SendMessageW(maps_, CB_SETCURSEL, selected, 0);
    Filter();
}
bool DifficultyChanged(const std::vector<BossRush::Difficulty>& values) {
    if (values.size() != difficulties_.size()) return true;
    for (size_t i = 0; i < values.size(); ++i) if (values[i].value != difficulties_[i].value || values[i].name != difficulties_[i].name) return true;
    return false;
}
void UpdateDifficulties(const std::vector<BossRush::Difficulty>& values, int32_t wanted) {
    difficulties_ = values; SendMessageW(difficulty_, CB_RESETCONTENT, 0, 0);
    int selected = -1;
    for (const auto& value : difficulties_) {
        const LRESULT row = UiI18n::AddString(difficulty_, CB_ADDSTRING, 0, value.name.c_str());
        if (row == CB_ERR || row == CB_ERRSPACE) continue;
        SendMessageW(difficulty_, CB_SETITEMDATA, static_cast<WPARAM>(row), value.value);
        if (value.value == wanted) selected = static_cast<int>(row);
    }
    SendMessageW(difficulty_, CB_SETCURSEL, static_cast<WPARAM>(selected >= 0 ? selected : 0), 0);
}
bool ReadOptions(BossRush::Options& options) {
    options = snapshot_.options;
    options.override_rank = false;
    options.override_cycle = snapshot_.cycle_supported && Checked(cycle_override_);
    options.override_difficulty = snapshot_.difficulty_supported && Checked(difficulty_override_);
    options.auto_next = Checked(auto_next_); options.heal_on_start = Checked(heal_);
    if (options.override_cycle) {
        const std::wstring text = Text(cycle_); wchar_t* end = nullptr;
        const long cycle = wcstol(text.c_str(), &end, 10);
        if (text.empty() || !end || *end || cycle < 1 || cycle > snapshot_.cycle_max) {
            wchar_t notice[120]{}; swprintf_s(notice, L"周目请输入 1 至 %d 的整数。", snapshot_.cycle_max); Notice(notice); return false;
        }
        options.cycle_number = static_cast<int32_t>(cycle);
    }
    const LRESULT difficulty = SendMessageW(difficulty_, CB_GETCURSEL, 0, 0);
    if (options.override_difficulty && difficulty == CB_ERR) { Notice(L"请选择游戏支持的难度。 "); return false; }
    if (difficulty != CB_ERR) options.difficulty = static_cast<int32_t>(SendMessageW(difficulty_, CB_GETITEMDATA, static_cast<WPARAM>(difficulty), 0));
    return true;
}
void Submit(BossRush::Action action) {
    BossRush::Options options;
    if (action == BossRush::Action::Stop) options = snapshot_.options;
    else if (!ReadOptions(options)) return;
    if (BossRush::Request(action, Selection(), options)) Notice(L"已提交请求，实际进度见下方状态。 ");
    else Notice(L"请求没有接受，请检查所选入口与参数。 ");
}
void Update() {
    if (!window_) return;
    editing_ = true;
    snapshot_ = BossRush::GetSnapshot();
    if (CatalogChanged(snapshot_.entries)) UpdateCatalog(snapshot_.entries);
    if (DifficultyChanged(snapshot_.difficulties)) UpdateDifficulties(snapshot_.difficulties, snapshot_.options.difficulty);
    if (!initialized_options_) {
        Check(cycle_override_, snapshot_.options.override_cycle); Check(difficulty_override_, snapshot_.options.override_difficulty);
        Check(auto_next_, snapshot_.options.auto_next); Check(heal_, snapshot_.options.heal_on_start);
        UiI18n::SetText(cycle_, std::to_wstring(snapshot_.options.cycle_number).c_str());
        initialized_options_ = true;
    }
    EnableWindow(cycle_override_, snapshot_.cycle_supported); EnableWindow(cycle_, snapshot_.cycle_supported && Checked(cycle_override_));
    EnableWindow(difficulty_override_, snapshot_.difficulty_supported); EnableWindow(difficulty_, snapshot_.difficulty_supported && Checked(difficulty_override_));
    EnableWindow(apply_, snapshot_.ready && !snapshot_.busy);
    EnableWindow(next_, snapshot_.ready && !snapshot_.busy && snapshot_.current >= 0);
    EnableWindow(retry_, snapshot_.ready && !snapshot_.busy && snapshot_.current >= 0);
    EnableWindow(stop_, snapshot_.running || snapshot_.busy || snapshot_.current >= 0);
    std::wstring status = snapshot_.status;
    if (snapshot_.running) status += L"  |  连战进度 " + std::to_wstring(snapshot_.completed) + L" / " + std::to_wstring(snapshot_.total);
    if (!message_.empty() && GetTickCount64() < message_until_) status = message_ + L"\r\n" + status;
    UiI18n::SetText(status_, status.c_str());
    UiI18n::SetText(strength_, snapshot_.rank_description.c_str());
    UpdateSelection(); editing_ = false;
}
void Hide() {
    if (DebugHub::IsEmbedded(window_)) { DebugHub::Hide(); return; }
    shown_ = false;
    if (character_window_) ShowWindow(character_window_, SW_HIDE);
    if (window_) ShowWindow(window_, SW_HIDE);
    if (return_window_ && IsWindow(return_window_)) SetForegroundWindow(return_window_);
}
// Everything below the Boss list: details, status, actions, options, links.
int LayoutLower(const UiLayout::Metrics& m, int x, int y, int width) {
    const int gap = m.Gap();
    const int detail = 3 * m.Line() + m.S(14), status = 2 * m.Line() + m.S(20);
    UiLayout::Place(detail_, x, y, width, detail); y += detail + gap;
    UiLayout::Place(status_, x, y, width, status); y += status + gap;
    y = UiLayout::Buttons({teleport_, start_, next_, retry_, direct_fight_, stop_}, x, y, width, m) + gap * 2;
    UiLayout::Place(options_section_, x, y, width, m.Section()); y += m.Section() + gap / 2;
    UiLayout::Flow options(x, y, width, m.Row(), gap, gap);
    options.Add(cycle_override_, UiLayout::CheckWidth(cycle_override_, m)); options.Add(cycle_, m.S(88)); options.Space(gap * 2);
    options.Add(difficulty_override_, UiLayout::CheckWidth(difficulty_override_, m));
    options.AddCombo(difficulty_, std::clamp(width / 3, m.S(220), m.S(320)), m.S(240));
    options.Break();
    options.Add(auto_next_, UiLayout::CheckWidth(auto_next_, m)); options.Space(gap * 2); options.Add(heal_, UiLayout::CheckWidth(heal_, m));
    const int apply = (std::max)(m.S(150), UiLayout::ButtonWidth(apply_, m));
    if (options.x + apply <= options.right) UiLayout::Place(apply_, options.right - apply, options.y + (m.Row() - m.Button()) / 2, apply, m.Button());
    else { options.Break(); options.Add(apply_, apply, m.Button()); }
    y = options.Bottom() + gap / 2;
    const int strength = UiLayout::TextHeight(strength_, width);
    UiLayout::Place(strength_, x, y, width, strength); y += strength + gap * 2;
    UiLayout::Place(links_section_, x, y, width, m.Section()); y += m.Section() + gap / 2;
    return UiLayout::Buttons({character_button_, ai_button_, air_button_, grab_button_}, x, y, width, m);
}
void Layout() {
    if (!window_ || !grab_button_) return;
    const UiLayout::Metrics m = UiLayout::For(window_);
    RECT client{}; GetClientRect(window_, &client);
    const int margin = m.Margin(), gap = m.Gap(), x = margin, width = (std::max)(m.S(680), static_cast<int>(client.right) - 2 * margin);
    int y = margin;
    const int intro = UiLayout::TextHeight(intro_, width);
    UiLayout::Place(intro_, x, y, width, intro); y += intro + gap;
    UiLayout::Flow filter(x, y, width, m.Row(), gap, gap);
    const int search_label = UiLayout::TextWidth(search_label_), map_label = UiLayout::TextWidth(map_label_);
    const int check = UiLayout::CheckWidth(only_available_, m), map_width = std::clamp(width / 3, m.S(220), m.S(380));
    filter.Add(search_label_, search_label, m.Line());
    filter.Add(search_, (std::max)(m.S(180), width - search_label - map_label - map_width - check - gap * 7));
    filter.Space(gap); filter.Add(map_label_, map_label, m.Line());
    filter.AddCombo(maps_, map_width, m.S(320)); filter.Space(gap); filter.Add(only_available_, check);
    y = filter.Bottom() + gap;
    // Measure the lower block first so the list takes whatever height is left.
    const int lower = LayoutLower(m, x, 0, width), minimum_list = m.S(150);
    const int list = (std::max)(minimum_list, static_cast<int>(client.bottom) - margin - lower - gap - y);
    UiLayout::Place(list_, x, y, width, list);
    SendMessageW(list_, LB_SETHORIZONTALEXTENT, m.S(1400), 0);
    LayoutLower(m, x, y + list + gap, width);
    DebugHub::SetMinimum(window_, m.S(680) + 2 * margin, y + minimum_list + gap + lower + margin);
}
void UseDpiFont() {
    font_ = UiTheme::GetFont(UiTheme::Font::Body, window_);
    UiLayout::UseFont(window_, font_); Layout();
}
LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { Hide(); return 0; }
    if (message == WM_SIZE) { Layout(); return 0; }
    if (message == WM_DPICHANGED_AFTERPARENT) { UseDpiFont(); return 0; }
    if (message == WM_COMMAND && !editing_) {
        const int id = LOWORD(wparam), event = HIWORD(wparam);
        if ((id == kSearch && event == EN_CHANGE) || (id == kMap && event == CBN_SELCHANGE) || id == kOnlyAvailable) Filter();
        else if (id == kList && event == LBN_SELCHANGE) UpdateSelection();
        else if (id == kTeleport) Submit(BossRush::Action::Teleport);
        else if (id == kStart) Submit(BossRush::Action::StartRush);
        else if (id == kDirectFight) Submit(BossRush::Action::DirectFight);
        else if (id == kNext) Submit(BossRush::Action::Next);
        else if (id == kRetry) Submit(BossRush::Action::Retry);
        else if (id == kStop) Submit(BossRush::Action::Stop);
        else if (id == kApply) Submit(BossRush::Action::ApplyOptions);
        else if (id == kCycleOverride || id == kDifficultyOverride) Update();
        else if (id == kCharacter) ShowCharacterWindow();
        else if (id == kAdvancedAI) BossAIMenu::Show();
        else if (id == kAirSkills) AirSkillsMenu::ToggleWindow();
        else if (id == kMotianGrab) MotianGrabMenu::ToggleWindow();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool Create() {
    if (window_) return true;
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance = instance; wc.lpfnWndProc = WindowProc;
    wc.lpszClassName = L"PBZBossRushMenu_v1"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"PBZ Boss Rush  |  F10 显示 / 隐藏",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT, 964, 772, nullptr, nullptr, instance, nullptr);
    if (!window_) return false;
    font_ = UiTheme::GetFont(UiTheme::Font::Body, window_);
    // EDIT clears WS_BORDER and paints its own square edge; a client edge keeps the themed frame.
    auto child = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id) {
        const bool edit = !_wcsicmp(type, L"EDIT") && (style & WS_BORDER);
        HWND result = CreateWindowExW(edit ? WS_EX_CLIENTEDGE : 0, type, text, WS_CHILD | WS_VISIBLE | (edit ? style & ~WS_BORDER : style), 0, 0, 10, 10,
            window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font_ ? font_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE); UiI18n::Bind(result,text); return result;
    };
    intro_ = child(L"STATIC", L"无生成器时自动直接创建；“直接触战”使用 Boss 默认 AI。\r\n操作菜单或切出时暂停战斗；切回继续。返回原存档请用“停止并返回”。", SS_LEFT, 0);
    search_label_ = child(L"STATIC", L"搜索", SS_LEFT, 0);
    search_ = child(L"EDIT", L"", WS_TABSTOP | WS_BORDER | ES_AUTOHSCROLL, kSearch);
    map_label_ = child(L"STATIC", L"地图", SS_LEFT, 0);
    maps_ = child(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kMap);
    only_available_ = child(L"BUTTON", L"只显示可传送", WS_TABSTOP | BS_AUTOCHECKBOX, kOnlyAvailable);
    list_ = child(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | WS_HSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, kList);
    detail_ = child(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0);
    status_ = child(L"STATIC", L"等待游戏线程初始化…", SS_LEFT, 0);
    teleport_ = child(L"BUTTON", L"传送所选 Boss", WS_TABSTOP | BS_PUSHBUTTON, kTeleport);
    start_ = child(L"BUTTON", L"从此处开始连战", WS_TABSTOP | BS_PUSHBUTTON, kStart);
    next_ = child(L"BUTTON", L"下一位", WS_TABSTOP | BS_PUSHBUTTON, kNext);
    retry_ = child(L"BUTTON", L"重试当前 Boss", WS_TABSTOP | BS_PUSHBUTTON, kRetry);
    direct_fight_ = child(L"BUTTON", L"直接触战", WS_TABSTOP | BS_PUSHBUTTON, kDirectFight);
    stop_ = child(L"BUTTON", L"停止并返回", WS_TABSTOP | BS_PUSHBUTTON, kStop);
    options_section_ = child(L"STATIC", L"难度 / 强度设置（默认保留当前游戏值）", SS_LEFT, 0);
    cycle_override_ = child(L"BUTTON", L"覆盖周目", WS_TABSTOP | BS_AUTOCHECKBOX, kCycleOverride);
    cycle_ = child(L"EDIT", L"1", WS_TABSTOP | WS_BORDER | ES_NUMBER | ES_AUTOHSCROLL, kCycle);
    SendMessageW(cycle_, EM_SETLIMITTEXT, 5, 0);
    difficulty_override_ = child(L"BUTTON", L"覆盖难度", WS_TABSTOP | BS_AUTOCHECKBOX, kDifficultyOverride);
    difficulty_ = child(L"COMBOBOX", L"", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, kDifficulty);
    auto_next_ = child(L"BUTTON", L"确认击败后自动下一位", WS_TABSTOP | BS_AUTOCHECKBOX, kAutoNext);
    heal_ = child(L"BUTTON", L"开战时恢复状态", WS_TABSTOP | BS_AUTOCHECKBOX, kHeal);
    apply_ = child(L"BUTTON", L"应用设置", WS_TABSTOP | BS_PUSHBUTTON, kApply);
    strength_ = child(L"STATIC", L"", SS_LEFT, 0);
    links_section_ = child(L"STATIC", L"相关页面", SS_LEFT, 0);
    character_button_ = child(L"BUTTON", L"角色替换…", WS_TABSTOP | BS_PUSHBUTTON, kCharacter);
    ai_button_ = child(L"BUTTON", L"攻击欲望 / AI 参数…", WS_TABSTOP | BS_PUSHBUTTON, kAdvancedAI);
    air_button_ = child(L"BUTTON", L"空中技能 / 跳跃取消 / JC…", WS_TABSTOP | BS_PUSHBUTTON, kAirSkills);
    grab_button_ = child(L"BUTTON", L"投技（三角＋圈）…", WS_TABSTOP | BS_PUSHBUTTON, kMotianGrab);
    UiTheme::SetRole(intro_, UiTheme::Role::Hint); UiTheme::SetRole(strength_, UiTheme::Role::Hint);
    UiTheme::SetRole(status_, UiTheme::Role::Card);
    UiTheme::SetRole(options_section_, UiTheme::Role::Section); UiTheme::SetRole(links_section_, UiTheme::Role::Section);
    UiTheme::SetRole(teleport_, UiTheme::Role::Primary); UiTheme::SetRole(apply_, UiTheme::Role::Primary);
    Layout();
    Update(); return true;
}
int32_t CharacterSelection() {
    const LRESULT row = SendMessageW(character_list_, LB_GETCURSEL, 0, 0);
    return row == LB_ERR ? -1 : static_cast<int32_t>(SendMessageW(character_list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0));
}
void UpdateCharacterSelection() {
    const int32_t selected = CharacterSelection();
    const auto found = std::find_if(character_snapshot_.entries.begin(), character_snapshot_.entries.end(),
        [&](const CharacterSwap::Entry& entry) { return entry.index == selected; });
    const bool valid = found != character_snapshot_.entries.end();
    const std::wstring detail = valid ? found->detail : L"选择一个完整模型装配预设查看来源。 ";
    if (Text(character_detail_) != detail) UiI18n::SetText(character_detail_, detail.c_str());
    EnableWindow(character_apply_, valid && character_snapshot_.ready && !character_snapshot_.busy);
}
void FilterCharacters() {
    const int32_t selected = CharacterSelection();
    SendMessageW(character_list_, LB_RESETCONTENT, 0, 0); int selected_row = 0;
    for (const auto& entry : character_snapshot_.entries) {
        if (entry.advanced && !Checked(character_advanced_)) continue;
        const std::wstring text = entry.name + (entry.advanced ? L"  [内部变体]" : L"");
        const LRESULT row = UiI18n::AddString(character_list_, LB_ADDSTRING, 0, text.c_str());
        if (row == LB_ERR || row == LB_ERRSPACE) continue;
        SendMessageW(character_list_, LB_SETITEMDATA, static_cast<WPARAM>(row), entry.index);
        if (entry.index == selected) selected_row = static_cast<int>(row);
    }
    SendMessageW(character_list_, LB_SETCURSEL, selected_row, 0); UpdateCharacterSelection();
}
void UpdateCharacters() {
    if (!character_window_) return;
    character_snapshot_ = CharacterSwap::GetSnapshot();
    if (!character_catalog_built_) { FilterCharacters(); character_catalog_built_ = !character_snapshot_.entries.empty(); }
    std::wstring text = character_snapshot_.status;
    if (!character_snapshot_.original.empty()) text += L"\r\n原模型：" + character_snapshot_.original;
    if (character_snapshot_.active && !character_snapshot_.selected.empty()) text += L"  |  当前临时模型：" + character_snapshot_.selected;
    UiI18n::SetText(character_status_, text.c_str());
    EnableWindow(character_restore_, character_snapshot_.active && !character_snapshot_.busy);
    UpdateCharacterSelection();
}
void LayoutCharacters() {
    if (!character_window_ || !character_note_) return;
    const UiLayout::Metrics m = UiLayout::For(character_window_);
    RECT client{}; GetClientRect(character_window_, &client);
    const int margin = m.Margin(), gap = m.Gap(), x = margin, width = (std::max)(m.S(600), static_cast<int>(client.right) - 2 * margin);
    int y = margin;
    const int intro = UiLayout::TextHeight(character_intro_, width);
    UiLayout::Place(character_intro_, x, y, width, intro); y += intro + gap;
    UiLayout::Place(character_advanced_, x, y, UiLayout::CheckWidth(character_advanced_, m), m.Check()); y += m.Check() + gap;
    const int status = 3 * m.Line() + m.S(20), note = UiLayout::TextHeight(character_note_, width);
    const int lower = status + gap + m.Button() + gap + note, minimum_body = m.S(220);
    const int body = (std::max)(minimum_body, static_cast<int>(client.bottom) - margin - lower - gap - y);
    const int list = std::clamp(width * 9 / 25, m.S(240), m.S(420));
    UiLayout::Place(character_list_, x, y, list, body);
    UiLayout::Place(character_detail_, x + list + gap, y, width - list - gap, body); y += body + gap;
    UiLayout::Place(character_status_, x, y, width, status); y += status + gap;
    y = UiLayout::ButtonRow({character_apply_, character_restore_, character_npc_}, x, y, width, m, m.S(170)) + gap;
    UiLayout::Place(character_note_, x, y, width, note);
    DebugHub::SetMinimum(character_window_, m.S(600) + 2 * margin, margin + intro + gap + m.Check() + gap + minimum_body + gap + lower + margin);
}
LRESULT CALLBACK CharacterWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_SIZE) { LayoutCharacters(); return 0; }
    if (message == WM_DPICHANGED_AFTERPARENT) { UiLayout::UseFont(window, UiTheme::GetFont(UiTheme::Font::Body, window)); LayoutCharacters(); return 0; }
    if (message == WM_CLOSE) {
        if (DebugHub::IsEmbedded(window)) { DebugHub::Hide(); return 0; }
        character_shown_ = false; ShowWindow(window, SW_HIDE); if (window_) SetForegroundWindow(window_); return 0;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam), event = HIWORD(wparam);
        if (id == kCharacterList && event == LBN_SELCHANGE) UpdateCharacterSelection();
        else if (id == kCharacterAdvanced) FilterCharacters();
        else if (id == kCharacterApply) CharacterSwap::Request(CharacterSwap::Action::Replace, CharacterSelection());
        else if (id == kCharacterRestore) CharacterSwap::Request(CharacterSwap::Action::Restore);
        else if (id == kNpcModels) NpcMenu::Show();
        UpdateCharacters(); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool CreateCharacters() {
    if (!Create()) return false;
    if (!character_window_) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{sizeof(wc)}; wc.hInstance = instance; wc.lpfnWndProc = CharacterWindowProc;
        wc.lpszClassName = L"PBZCharacterSwap_v1"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
        character_window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"PBZ 角色替换  |  主角兼容完整装配",
            WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT, 840, 590, window_, nullptr, instance, nullptr);
        if (!character_window_) return false;
        const HFONT font = UiTheme::GetFont(UiTheme::Font::Body, character_window_);
        auto child = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id) {
            const bool edit = !_wcsicmp(type, L"EDIT") && (style & WS_BORDER);
            HWND result = CreateWindowExW(edit ? WS_EX_CLIENTEDGE : 0, type, text, WS_CHILD | WS_VISIBLE | (edit ? style & ~WS_BORDER : style), 0, 0, 10, 10,
                character_window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
            SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); UiI18n::Bind(result,text); return result;
        };
        character_intro_ = child(L"STATIC", L"主角套装使用原生完整装配并保留动画。沐小葵、魔渊的原模型请进入实验重定向菜单。", SS_LEFT, 0);
        character_advanced_ = child(L"BUTTON", L"显示内部变体", WS_TABSTOP | BS_AUTOCHECKBOX, kCharacterAdvanced);
        character_list_ = child(L"LISTBOX", L"", WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT, kCharacterList);
        character_detail_ = child(L"EDIT", L"", WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0);
        character_status_ = child(L"STATIC", L"等待主角外观组件…", SS_LEFT, 0);
        character_apply_ = child(L"BUTTON", L"临时替换完整模型", WS_TABSTOP | BS_PUSHBUTTON, kCharacterApply);
        character_restore_ = child(L"BUTTON", L"恢复原模型", WS_TABSTOP | BS_PUSHBUTTON, kCharacterRestore);
        character_npc_ = child(L"BUTTON", L"NPC 原模型重定向…", WS_TABSTOP | BS_PUSHBUTTON, kNpcModels);
        character_note_ = child(L"STATIC", L"不提交外观保存。关闭窗口会保留临时模型；结束测试请点击“恢复原模型”。", SS_LEFT, 0);
        UiTheme::SetRole(character_intro_, UiTheme::Role::Hint); UiTheme::SetRole(character_note_, UiTheme::Role::Hint);
        UiTheme::SetRole(character_status_, UiTheme::Role::Card); UiTheme::SetRole(character_apply_, UiTheme::Role::Primary);
        LayoutCharacters();
    }
    return true;
}
void ShowCharacterWindow() {
    if (DebugHub::Enabled()) { DebugHub::ShowPage(DebugHub::Page::Character); return; }
    if (!CreateCharacters()) return;
    character_shown_ = true; UpdateCharacters();
    ShowWindow(character_window_, SW_SHOW); SetForegroundWindow(character_window_); SetFocus(character_list_);
}
}

void Initialize() { key_down_ = (GetAsyncKeyState(VK_F10) & 0x8000) != 0; }
void Show() {
    if (DebugHub::Enabled()) { DebugHub::ShowPage(DebugHub::Page::BossRush); return; }
    if (!window_ && !Create()) return;
    const bool foreground = Foreground();
    if (foreground && GetForegroundWindow() != window_ && GetForegroundWindow() != character_window_) return_window_ = GetForegroundWindow();
    shown_ = true; Update();
    if (foreground) { ShowWindow(window_, SW_SHOW); SetForegroundWindow(window_); SetFocus(search_); }
}
void Toggle() { if (DebugHub::Enabled()) DebugHub::Toggle(); else if (shown_) Hide(); else Show(); }
void ShowCharacters() {
    if (DebugHub::Enabled()) { DebugHub::ShowPage(DebugHub::Page::Character); return; }
    Show(); if (window_ && Foreground()) ShowCharacterWindow();
}
HWND CreatePage(HWND parent) {
    if (!Create()) return nullptr;
    DebugHub::Attach(window_,parent,948,700); Update(); return window_;
}
HWND CreateCharacterPage(HWND parent) {
    if (!CreateCharacters()) return nullptr;
    DebugHub::Attach(character_window_,parent,824,558); UpdateCharacters(); return character_window_;
}
void Tick() {
    if (!DebugHub::Enabled()) {
      MSG message{};
      while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (character_window_ && (message.hwnd == character_window_ || IsChild(character_window_, message.hwnd))) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { SendMessageW(character_window_, WM_CLOSE, 0, 0); continue; }
            if (IsDialogMessageW(character_window_, &message)) continue;
        } else if (window_ && (message.hwnd == window_ || IsChild(window_, message.hwnd))) {
            if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { Hide(); continue; }
            if (IsDialogMessageW(window_, &message)) continue;
        } else if (message.hwnd) {
            // The shared worker owns all native plugin menus. Route keyboard
            // navigation before this central pump dispatches their messages.
            HWND root = GetAncestor(message.hwnd, GA_ROOT);
            wchar_t type[128]{};
            if (root && GetClassNameW(root, type, 128) && !_wcsnicmp(type, L"PBZ", 3)) {
                if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { SendMessageW(root, WM_CLOSE, 0, 0); continue; }
                if (IsDialogMessageW(root, &message)) continue;
            }
        }
        TranslateMessage(&message); DispatchMessageW(&message);
      }
    }
    const bool foreground = Foreground(), down = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
    if (!DebugHub::Enabled() && foreground && down && !key_down_) Toggle();
    key_down_ = down;
    if ((window_||character_window_) && GetTickCount64() >= next_update_) {
        if (DebugHub::Enabled()) {
            if (DebugHub::IsEmbedded(window_)&&IsWindowVisible(window_)) Update();
            if (DebugHub::IsEmbedded(character_window_)&&IsWindowVisible(character_window_)) UpdateCharacters();
        } else {
            Update(); UpdateCharacters();
            ShowWindow(window_, shown_ && foreground ? SW_SHOWNOACTIVATE : SW_HIDE);
            if (character_window_) ShowWindow(character_window_, shown_ && character_shown_ && foreground ? SW_SHOWNOACTIVATE : SW_HIDE);
        }
        next_update_ = GetTickCount64() + 250;
    }
}
void Shutdown() {
    shown_ = false;
    if (character_window_) { DestroyWindow(character_window_); character_window_ = nullptr; }
    if (window_) { DestroyWindow(window_); window_ = nullptr; }
    font_ = nullptr; // Owned by UiTheme.
}
}
