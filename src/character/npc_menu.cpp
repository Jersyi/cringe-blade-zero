#include "npc_menu.h"
#include "debug_hub.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "npc_retarget.h"
#include "character_swap.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwctype>
#include <string>
#include <vector>

namespace NpcMenu {
namespace {
constexpr wchar_t kClass[] = L"PBZ_NpcRetarget_Window_v1";
constexpr int kSearch = 4601, kList = 4602, kInspect = 4603;
constexpr int kApply = 4610, kRestore = 4611, kClose = 4612;
constexpr int kExperimental = 4613;
HWND window_ = nullptr, return_window_ = nullptr;
HWND title_ = nullptr, introduction_ = nullptr, search_label_ = nullptr, search_ = nullptr, list_ = nullptr;
HWND detail_ = nullptr, gates_ = nullptr, active_ = nullptr, status_ = nullptr;
HWND inspect_ = nullptr, apply_ = nullptr, restore_ = nullptr, close_ = nullptr;
HWND experimental_ = nullptr;
HFONT font_ = nullptr;
Logger logger_ = nullptr;
DWORD worker_thread_ = 0;
int dpi_ = 96;
bool shown_ = false, updating_ = false;
volatile LONG show_requested_ = 0;
ULONGLONG next_update_ = 0, notice_until_ = 0;
std::wstring notice_;
std::vector<NpcRetarget::Entry> entries_;
NpcRetarget::Snapshot snapshot_;

int S(int value) { return MulDiv(value, dpi_, 96); }
void Log(const char* text) { if (logger_) logger_(text); }
std::wstring Text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    if (length < 1 || length > 16384) return {};
    std::vector<wchar_t> value(static_cast<size_t>(length) + 1);
    GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    return value.data();
}
std::wstring Lower(std::wstring value) {
    for (wchar_t& c : value) c = static_cast<wchar_t>(std::towlower(c));
    return value;
}
void SetText(HWND control, const std::wstring& value) {
    if (control) UiI18n::SetText(control,value.c_str());
}
void Notice(const wchar_t* text) { notice_ = text; notice_until_ = GetTickCount64() + 6000; }
int32_t Selection() {
    const LRESULT row = SendMessageW(list_, LB_GETCURSEL, 0, 0);
    return row == LB_ERR ? -1 : static_cast<int32_t>(SendMessageW(list_, LB_GETITEMDATA, static_cast<WPARAM>(row), 0));
}
const NpcRetarget::Entry* Selected() {
    const int32_t index = Selection();
    const auto found = std::find_if(entries_.begin(), entries_.end(), [&](const NpcRetarget::Entry& entry) { return entry.index == index; });
    return found == entries_.end() ? nullptr : &*found;
}
std::wstring BoneEvidence(const std::string& id) {
    if (id == "MuXiaoKui")
        return L"骨架差异：Hun 1134 骨 → 沐小葵 1090 骨；已核定共享骨 1004。\r\n"
            L"NPC 另有 86 根专有骨；保留主角完整动画骨架，武器按当前姿势与 NPC 手部比例适配。";
    if (id == "MoTian")
        return L"骨架差异：Hun 1134 骨 → 魔渊 1004 骨；已核定共享骨 1004。\r\n"
            L"保留主角完整动画骨架与武器子骨骼运动。包含原 NPC 的头部静态附件。";
    return L"此项尚未附骨架差异核定资料。";
}
void SelectionChanged() {
    const NpcRetarget::Entry* entry = Selected();
    if (entry) {
        const std::wstring id(entry->id.begin(), entry->id.end());
        SetText(detail_, entry->name + L" · 原 NPC 模型 / 实验适配\r\n标识：" + id + L"\r\n\r\n" +
            BoneEvidence(entry->id) + L"\r\n\r\n" + entry->detail);
    } else SetText(detail_, L"选择沐小葵或魔渊查看来源、骨架差异与适配范围。\r\n这两项属于原 NPC 模型，不属于主角的同名套装。");
    const bool can_request = snapshot_.ready && !snapshot_.busy && entry;
    EnableWindow(inspect_, can_request); EnableWindow(apply_, can_request && snapshot_.experimental && snapshot_.appearance_gate && !snapshot_.cleanup_pending && !snapshot_.disabled);
    EnableWindow(restore_, snapshot_.active || snapshot_.busy || snapshot_.cleanup_pending);
}
bool CatalogChanged(const std::vector<NpcRetarget::Entry>& entries) {
    if (entries.size() != entries_.size()) return true;
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].index != entries_[i].index || entries[i].id != entries_[i].id ||
            entries[i].name != entries_[i].name || entries[i].detail != entries_[i].detail) return true;
    return false;
}
void Filter() {
    const int32_t selected = Selection();
    const std::wstring search = Lower(Text(search_));
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0); SendMessageW(list_, LB_RESETCONTENT, 0, 0);
    int selected_row = -1;
    for (const NpcRetarget::Entry& entry : entries_) {
        const std::wstring id(entry.id.begin(), entry.id.end());
        if (!search.empty() && Lower(entry.name + L" " + UiI18n::Translate(entry.name) + L" " + id).find(search) == std::wstring::npos) continue;
        const std::wstring title = entry.name + L"   ·   原 NPC / 实验适配";
        const LRESULT row = UiI18n::AddString(list_,LB_ADDSTRING,0,title.c_str());
        if (row == LB_ERR || row == LB_ERRSPACE) continue;
        SendMessageW(list_, LB_SETITEMDATA, static_cast<WPARAM>(row), entry.index);
        if (entry.index == selected) selected_row = static_cast<int>(row);
    }
    const LRESULT count = SendMessageW(list_, LB_GETCOUNT, 0, 0);
    SendMessageW(list_, LB_SETCURSEL, static_cast<WPARAM>(selected_row >= 0 ? selected_row : count > 0 ? 0 : -1), 0);
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0); InvalidateRect(list_, nullptr, TRUE);
    SelectionChanged();
}
void Update() {
    if (!window_) return;
    updating_ = true;
    snapshot_ = NpcRetarget::GetSnapshot();
    SendMessageW(experimental_, BM_SETCHECK, snapshot_.experimental ? BST_CHECKED : BST_UNCHECKED, 0);
    EnableWindow(experimental_, !snapshot_.disabled);
    if (CatalogChanged(snapshot_.entries)) { entries_ = snapshot_.entries; Filter(); }
    const CharacterSwap::Snapshot outfit = CharacterSwap::GetSnapshot();
    std::wstring state;
    if (snapshot_.active) {
        state = L"当前 NPC：" + snapshot_.selected + L"    部件 " + std::to_wstring(snapshot_.parts) +
            L"    武器挂点 " + std::to_wstring(snapshot_.sockets) + L"    已适配共享骨 " + std::to_wstring(snapshot_.retargeted_bones);
    } else state = L"当前 NPC：未应用";
    if (snapshot_.cleanup_pending) state += L"\r\n上一会话仍有恢复项目；已保留记录，完成恢复前不能应用另一模型。";
    else if (snapshot_.disabled) state += L"\r\nNPC 模块已达到独立故障上限；可提交恢复，新的应用已关闭。";
    else if (!snapshot_.appearance_gate) state += L"\r\n主角外观互斥适配器尚未接入；当前只允许检查模板。";
    if (outfit.active) state += L"\r\n主角同骨架套装：" + outfit.selected + L"。切换到 NPC 前必须通过原生会话释放校验。";
    else if (snapshot_.busy) state += L"\r\n游戏线程正在等待空闲或处理请求；恢复按钮可取消待应用操作。";
    else if (!snapshot_.ready) state += L"\r\n请进入可操作场景；可用后再检查模板或应用模型。";
    SetText(active_, state);
    std::wstring status = snapshot_.entries.empty() ? L"NPC 适配模块尚未初始化；菜单仅等待后端快照。" : snapshot_.status;
    if (GetTickCount64() < notice_until_ && !notice_.empty()) status = notice_ + L"\r\n" + status;
    SetText(status_, status); SelectionChanged(); updating_ = false;
}
void Submit(NpcRetarget::Action action) {
    const int32_t entry = action == NpcRetarget::Action::Restore ? -1 : Selection();
    if (NpcRetarget::Request(action, entry)) {
        if (action == NpcRetarget::Action::Restore) Notice(L"恢复请求已排队；只由游戏线程恢复原模型、材质和临时适配。");
        else if (action == NpcRetarget::Action::Inspect) Notice(L"模板检查请求已排队；检查结果会写入调试日志。");
        else Notice(L"实验适配请求已排队；将等待主角空闲后更换。");
    } else Notice(L"后端未接受此请求，请检查状态和所选模型。");
    Update();
}
bool SameProcess(HWND window) {
    DWORD process = 0; GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}
void Hide() {
    if (!window_) return;
    if (DebugHub::IsEmbedded(window_)) { DebugHub::Hide(); return; }
    const HWND foreground = GetForegroundWindow();
    const bool owns_foreground = foreground == window_ || IsChild(window_, foreground);
    shown_ = false; ShowWindow(window_, SW_HIDE);
    if (owns_foreground && return_window_ && IsWindow(return_window_) && SameProcess(return_window_))
        SetForegroundWindow(return_window_);
    return_window_ = nullptr;
}
HWND Control(const wchar_t* type, const wchar_t* title, DWORD style, int id, DWORD extended = 0) {
    HWND control = CreateWindowExW(extended, type, title, WS_CHILD | WS_VISIBLE | style, 0, 0, 1, 1,
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    if (control && font_) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), FALSE);
    if (control&&(_wcsicmp(type,L"EDIT")||(style&ES_READONLY))) UiI18n::Bind(control,title);
    return control;
}
BOOL CALLBACK SetChildFont(HWND control, LPARAM font) { SendMessageW(control, WM_SETFONT, static_cast<WPARAM>(font), TRUE); return TRUE; }
void Font() {
    font_ = UiTheme::GetFont(UiTheme::Font::Body, static_cast<UINT>(dpi_));
    if (window_ && font_) EnumChildWindows(window_, SetChildFont, reinterpret_cast<LPARAM>(font_));
}
void Layout() {
    if (!window_) return;
    RECT rectangle{}; GetClientRect(window_, &rectangle);
    // The hub already provides a physical-pixel viewport. Keep its footer in
    // view and let only the list and long descriptions scroll internally.
    const int width = rectangle.right, height = rectangle.bottom;
    const UiLayout::Metrics m{dpi_};
    const int margin = m.Margin(), gap = m.Gap();
    const int content = (std::max)(1, width - 2 * margin), line = m.Line();
    const int edit_height = m.Row(), button_height = m.Button();
    const int left = (std::min)(std::clamp(content * 3 / 10, 210, 300), content / 2);
    const int right = margin + left + gap, right_width = (std::max)(1, content - left - gap);
    const int title_height = m.Section(), introduction_top = margin + title_height + gap / 2;
    const int introduction_height = (std::min)(2 * line + 6, (std::max)(line + 6, height / 10));
    const int search_top = introduction_top + introduction_height + gap;
    const int body_top = search_top + edit_height + gap;
    const int status_height = 2 * line + S(14), status_top = height - margin - status_height;
    const int buttons_top = status_top - gap - button_height;
    const int active_height = 2 * line + S(14), active_top = buttons_top - gap - active_height;
    const int body_height = (std::max)(1, active_top - gap - body_top);
    const int detail_height = (std::max)(1, (body_height - gap) * 3 / 5);
    const int gates_top = body_top + detail_height + gap;
    MoveWindow(title_, margin, margin, content, title_height, TRUE);
    MoveWindow(introduction_, margin, introduction_top, content, introduction_height, TRUE);
    const int label_width = (std::min)(UiLayout::TextWidth(search_label_) + gap, left / 2);
    MoveWindow(search_label_, margin, search_top + (edit_height - line) / 2, label_width, line, TRUE);
    MoveWindow(search_, margin + label_width + gap, search_top, (std::max)(1, left - label_width - gap), edit_height, TRUE);
    MoveWindow(experimental_, right, search_top, (std::min)(right_width, UiLayout::CheckWidth(experimental_, m)), edit_height, TRUE);
    MoveWindow(list_, margin, body_top, left, body_height, TRUE);
    SendMessageW(list_, LB_SETHORIZONTALEXTENT, S(310), 0);
    MoveWindow(detail_, right, body_top, right_width, detail_height, TRUE);
    MoveWindow(gates_, right, gates_top, right_width, (std::max)(1, active_top - gap - gates_top), TRUE);
    MoveWindow(active_, margin, active_top, content, active_height, TRUE);
    const int button_width = (std::max)(1, (content - 3 * gap) / 4);
    const HWND buttons[] = {inspect_, apply_, restore_, close_};
    for (int i = 0; i < 4; ++i)
        MoveWindow(buttons[i], margin + i * (button_width + gap), buttons_top, button_width, button_height, TRUE);
    MoveWindow(status_, margin, status_top, content, status_height, TRUE);
}
LRESULT CALLBACK Procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { Hide(); return 0; }
    if (message == WM_SIZE) { Layout(); return 0; }
    if (message == WM_GETMINMAXINFO) {
        auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
        RECT minimum{0, 0, 720, 600};
        AdjustWindowRectExForDpi(&minimum, static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE)), FALSE,
            static_cast<DWORD>(GetWindowLongPtrW(window, GWL_EXSTYLE)), static_cast<UINT>(dpi_));
        limits->ptMinTrackSize = {minimum.right - minimum.left, minimum.bottom - minimum.top}; return 0;
    }
    if (message == WM_DPICHANGED) {
        dpi_ = HIWORD(wparam); if (dpi_ <= 0) dpi_ = 96;
        if (!DebugHub::IsEmbedded(window)) {
            const auto* rectangle = reinterpret_cast<const RECT*>(lparam);
            SetWindowPos(window, nullptr, rectangle->left, rectangle->top, rectangle->right - rectangle->left,
                rectangle->bottom - rectangle->top, SWP_NOACTIVATE | SWP_NOZORDER);
        }
        Font(); Layout(); return 0;
    }
    if (message == WM_DPICHANGED_AFTERPARENT) {
        dpi_ = static_cast<int>(GetDpiForWindow(window)); if (dpi_ <= 0) dpi_ = 96;
        Font(); Layout(); return 0;
    }
    if (message == WM_COMMAND && !updating_) {
        const int id = LOWORD(wparam), notification = HIWORD(wparam);
        if (id == kSearch && notification == EN_CHANGE) Filter();
        else if (id == kList && notification == LBN_SELCHANGE) SelectionChanged();
        else if (id == kInspect) Submit(NpcRetarget::Action::Inspect);
        else if (id == kApply || id == IDOK) { if (IsWindowEnabled(apply_)) Submit(NpcRetarget::Action::Apply); }
        else if (id == kRestore) Submit(NpcRetarget::Action::Restore);
        else if (id == kExperimental) {
            const bool enabled = SendMessageW(experimental_, BM_GETCHECK, 0, 0) == BST_CHECKED;
            if (!NpcRetarget::SetExperimental(enabled)) Notice(L"实验开关未接受请求；故障停用时仍可恢复原模型。");
            Update();
        }
        else if (id == kClose || id == IDCANCEL) Hide();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
bool Create() {
    if (window_) return true;
    WNDCLASSEXW type{}; type.cbSize = sizeof(type); type.lpfnWndProc = Procedure;
    type.hInstance = GetModuleHandleW(nullptr); type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1); type.lpszClassName = kClass;
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) { Log("NPC_MENU window class registration failed"); return false; }
    window_ = CreateWindowExW(WS_EX_CONTROLPARENT, kClass, L"PBZ · NPC 原模型实验适配", WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, S(1040), S(820), nullptr, nullptr, type.hInstance, nullptr);
    if (!window_) { Log("NPC_MENU window creation failed"); return false; }
    UiI18n::Bind(window_,L"PBZ · NPC 原模型实验适配");
    dpi_ = static_cast<int>(GetDpiForWindow(window_)); if (dpi_ <= 0) dpi_ = 96; Font();
    title_ = Control(L"STATIC", L"NPC 原模型 · 实验重定向", SS_LEFT, 0);
    UiTheme::SetRole(title_, UiTheme::Role::Section);
    introduction_ = Control(L"EDIT", L"保留主角操作与能力，使用沐小葵 / 魔渊的原 NPC 外观。\r\n更换会等待空闲；可随时提交恢复请求。这里的模型仍需完成实机动作与部件验收。", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0);
    search_label_ = Control(L"STATIC", L"搜索", SS_LEFT, 0);
    search_ = Control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, kSearch, WS_EX_CLIENTEDGE);
    SendMessageW(search_, EM_SETLIMITTEXT, 128, 0);
    list_ = Control(L"LISTBOX", L"", LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP, kList, WS_EX_CLIENTEDGE);
    experimental_ = Control(L"BUTTON", L"启用实验模型切换", BS_AUTOCHECKBOX | WS_TABSTOP, kExperimental);
    detail_ = Control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
    gates_ = Control(L"EDIT", L"待验收：完整身体 / 头部 / 发丝 / 衣料；走跑、攻击、受击与武器位置；恢复与跨图清理。\r\n"
        L"主角保留原生动画、技能与武器骨骼，NPC 外观独立适配。\r\n"
        L"武器按当前动画与手部比例修正，保留技能中的独立运动。\r\n"
        L"与主角同骨架套装互斥，必须先完整恢复上一种覆盖，再装配下一种。", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0);
    active_ = Control(L"EDIT", L"当前 NPC：未应用", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 0);
    inspect_ = Control(L"BUTTON", L"检查模板", BS_PUSHBUTTON | WS_TABSTOP, kInspect);
    apply_ = Control(L"BUTTON", L"应用模型", BS_DEFPUSHBUTTON | WS_TABSTOP, kApply);
    restore_ = Control(L"BUTTON", L"恢复原模型", BS_PUSHBUTTON | WS_TABSTOP, kRestore);
    close_ = Control(L"BUTTON", L"关闭", BS_PUSHBUTTON | WS_TABSTOP, kClose);
    status_ = Control(L"EDIT", L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = (std::min)(S(1040), static_cast<int>(work.right - work.left - S(24)));
    const int height = (std::min)(S(820), static_cast<int>(work.bottom - work.top - S(24)));
    SetWindowPos(window_, nullptr, work.left + S(12), work.top + S(12), width, height, SWP_NOACTIVATE | SWP_NOZORDER);
    Layout(); Update(); Log("NPC_MENU hidden worker window created; no engine calls"); return true;
}
void ShowOnWorker() {
    if (DebugHub::Enabled()) { DebugHub::ShowPage(DebugHub::Page::Npc); return; }
    if (!Create()) return;
    HWND foreground = GetForegroundWindow();
    if (foreground != window_ && !IsChild(window_, foreground) && SameProcess(foreground)) return_window_ = foreground;
    shown_ = true; Update(); ShowWindow(window_, SW_SHOWNORMAL); SetForegroundWindow(window_); SetFocus(list_);
}
}
void Initialize(Logger logger) {
    logger_ = logger;
    if (worker_thread_ && worker_thread_ != GetCurrentThreadId()) { Log("NPC_MENU Initialize refused on a different thread"); return; }
    worker_thread_ = GetCurrentThreadId(); Create();
}
HWND CreatePage(HWND parent) {
    if (!parent||worker_thread_!=GetCurrentThreadId()||!Create()) return nullptr;
    DebugHub::Attach(window_,parent,720,600); Layout(); Update(); return window_;
}
void Show() {
    if (DebugHub::Enabled()) { DebugHub::ShowPage(DebugHub::Page::Npc); return; }
    InterlockedExchange(&show_requested_, 1);
}
void Tick() {
    if (!worker_thread_ || worker_thread_ != GetCurrentThreadId()) return;
    if (InterlockedExchange(&show_requested_, 0)) ShowOnWorker();
    if (!window_) return;
    MSG message{};
    while (!DebugHub::IsEmbedded(window_)&&PeekMessageW(&message, window_, 0, 0, PM_REMOVE))
        if (!IsDialogMessageW(window_, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    const bool refresh=DebugHub::IsEmbedded(window_)?IsWindowVisible(window_)!=FALSE:shown_;
    if (refresh && GetTickCount64() >= next_update_) { next_update_ = GetTickCount64() + 250; Update(); }
}
void Shutdown() {
    if (!worker_thread_ || worker_thread_ != GetCurrentThreadId()) return;
    InterlockedExchange(&show_requested_, 0); shown_ = false;
    if (window_) DestroyWindow(window_);
    window_ = nullptr; return_window_ = nullptr;
    font_ = nullptr; UnregisterClassW(kClass, GetModuleHandleW(nullptr)); worker_thread_ = 0;
}
}
