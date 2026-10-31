#include "hair_controls.h"
#include "debug_hub.h"
#include "ui_i18n.h"
#include "ui_layout.h"
#include "ui_theme.h"
#include "groom_force.h"
#include "pad_triggers.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <cwchar>

namespace {
constexpr uintptr_t kHairWrapper = 0x0df47258;
constexpr uintptr_t kAppliedHairMode = 0x0df46cf0;
constexpr uintptr_t kViewModeCache = 0x0df48f00;
constexpr uintptr_t kShaderPrint = 0x0de9d004;
constexpr uint64_t PairOf(int32_t value) { return static_cast<uint32_t>(value) | (static_cast<uint64_t>(static_cast<uint32_t>(value)) << 32); }
constexpr uint64_t kBoundsMode = PairOf(32);
constexpr int kHairButton = 101, kBoundsButton = 102, kRestoreButton = 103, kForceButton = 104, kOutfitButton = 105, kTriggersButton = 107, kBossButton = 108;
constexpr int kKeys[4]{VK_F7, VK_F8, VK_F9, VK_F6};
const wchar_t* ModeName(int32_t value) { return value == 0 ? L"Mesh" : value == 1 ? L"Groom" : value == 2 ? L"Both" : L"Unavailable"; }
}

bool HairMemory::Read(uintptr_t address, void* data, size_t bytes) {
    if (address < 0x10000 || address > UINTPTR_MAX - bytes) return false;
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), data, bytes, &n) && n == bytes;
}
bool HairMemory::Writable(uintptr_t address, size_t bytes) {
    if (address < 0x10000 || address > UINTPTR_MAX - bytes) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info)) || info.State != MEM_COMMIT ||
        (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD p = info.Protect & 0xff;
    return (p == PAGE_READWRITE || p == PAGE_WRITECOPY) &&
        address + bytes <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}
bool HairMemory::Exchange64(uintptr_t address, uint64_t expected, uint64_t desired) {
    if ((address & 7) || !Writable(address, 8)) return false;
    // A console object could be destroyed during engine shutdown after VirtualQuery.
    // Fail closed rather than let an asynchronous worker fault the process.
    __try {
        return static_cast<uint64_t>(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(address),
            static_cast<LONG64>(desired), static_cast<LONG64>(expected))) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool HairMemory::Exchange32(uintptr_t address, int32_t expected, int32_t desired) {
    if ((address & 3) || !Writable(address, 4)) return false;
    __try {
        return InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(address), desired, expected) == expected;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool HairMemory::Pair(uintptr_t address, int32_t maximum, uint64_t& value) {
    std::array<int32_t, 2> pair{};
    if ((address & 7) || !Read(address, pair.data(), sizeof(pair)) || !Writable(address, sizeof(pair)) ||
        pair[0] < 0 || pair[0] > maximum || pair[1] < 0 || pair[1] > maximum) return false;
    std::memcpy(&value, pair.data(), sizeof(value)); return true;
}

HairControls::HairControls(uintptr_t image_base, bool create_panel, void (*open_boss_menu)()) : base_(image_base), open_boss_menu_(open_boss_menu) {
    for (int i = 0; i < 4; ++i) key_down_[i] = (GetAsyncKeyState(kKeys[i]) & 0x8000) != 0;
    if (create_panel) CreatePanel();
}
HairControls::~HairControls() { Restore(); if (panel_) DestroyWindow(panel_); }
bool HairControls::Foreground() const {
    DWORD process = 0; GetWindowThreadProcessId(GetForegroundWindow(), &process);
    return process == GetCurrentProcessId();
}
DWORD HairControls::Status() const {
    DWORD status = bounds_on_ ? 1u : 0u;
    if (hair_saved_) status |= 2;
    return status;
}
bool HairControls::ToggleHair() {
    uintptr_t pointer = 0; uint64_t current = 0;
    if (!HairMemory::Read(base_ + kHairWrapper + 16, &pointer, sizeof(pointer)) || !HairMemory::Pair(pointer, 2, current)) {
        wcscpy_s(notice_, L"Hair CVar unavailable; no change."); return false;
    }
    if (hair_saved_ && pointer != hair_pointer_) {
        wcscpy_s(notice_, L"Hair CVar identity changed; restore/restart before retrying."); return false;
    }
    const int32_t old_mode = static_cast<int32_t>(current & 0xffffffffu);
    const int32_t next_mode = old_mode == 1 ? 0 : 1;
    const uint64_t next = PairOf(next_mode);
    if (!HairMemory::Exchange64(pointer, current, next)) {
        wcscpy_s(notice_, L"Hair value changed concurrently; press again."); return false;
    }
    if (!hair_saved_) { hair_saved_ = true; hair_pointer_ = pointer; hair_original_ = current; }
    hair_written_ = next;
    swprintf_s(notice_, L"Requested %s; waiting for the game's own refresh.", ModeName(next_mode));
    UpdatePanel(); return true;
}
bool HairControls::ToggleBounds() {
    if (bounds_on_) { const bool ok = RestoreBounds(); UpdatePanel(); return ok; }
    uintptr_t pointer = 0; uint64_t view = 0; int32_t shader = -1;
    if (!HairMemory::Read(base_ + kViewModeCache, &pointer, sizeof(pointer)) || !HairMemory::Pair(pointer, 40, view) ||
        !HairMemory::Read(base_ + kShaderPrint, &shader, sizeof(shader)) || shader < 0 || shader > 1 ||
        !HairMemory::Writable(base_ + kShaderPrint, sizeof(shader))) {
        wcscpy_s(notice_, L"GPU debug CVar not initialized; no change."); return false;
    }
    if (!HairMemory::Exchange32(base_ + kShaderPrint, shader, 1)) {
        wcscpy_s(notice_, L"ShaderPrint changed concurrently; no change."); return false;
    }
    if (!HairMemory::Exchange64(pointer, view, kBoundsMode)) {
        HairMemory::Exchange32(base_ + kShaderPrint, 1, shader);
        wcscpy_s(notice_, L"View mode changed concurrently; no change."); return false;
    }
    bounds_pointer_ = pointer; bounds_original_ = view; shader_original_ = shader; bounds_on_ = true;
    wcscpy_s(notice_, L"GPU cluster/group AABB requested (strand geometry only).");
    UpdatePanel(); return true;
}
bool HairControls::ToggleForce() {
    if (!GroomForce::Ready()) { wcscpy_s(notice_, L"Force Groom not ready (engine hook/layout pending)."); UpdatePanel(); return false; }
    if (!GroomForce::Active()) {
        // Forced Grooms carry bIsMainHair, so they only show while MainHairMode allows Groom.
        uintptr_t pointer = 0; uint64_t current = 0;
        if (HairMemory::Read(base_ + kHairWrapper + 16, &pointer, sizeof(pointer)) && HairMemory::Pair(pointer, 2, current) &&
            static_cast<int32_t>(current & 0xffffffffu) == 0) ToggleHair();
        wcscpy_s(notice_, L"Force Groom requested: loading Groom assets on the game thread.");
    } else {
        wcscpy_s(notice_, L"Force Groom off requested: removing added Grooms.");
    }
    GroomForce::Request(GroomForce::kToggle);
    UpdatePanel(); return true;
}
bool HairControls::PlayCredits() {
    if (!GroomForce::Ready()) { wcscpy_s(notice_, L"Credits not ready (engine hook/layout pending)."); UpdatePanel(); return false; }
    GroomForce::RequestCredits();
    wcscpy_s(notice_, L"Credits requested: pushing WB_PersonnelList (no save, closes before settlement).");
    UpdatePanel(); return true;
}
bool HairControls::ToggleOutfit() {
    if (!GroomForce::Ready()) { wcscpy_s(notice_, L"Outfit unlock not ready (engine hook/layout pending)."); UpdatePanel(); return false; }
    const bool on = !GroomForce::OutfitUnlockActive();
    GroomForce::RequestOutfitUnlock(on);
    wcscpy_s(notice_, on ? L"Outfit unlock requested: enabling the game's outfit logic on the player." : L"Outfit unlock off requested.");
    UpdatePanel(); return true;
}
bool HairControls::ToggleTriggers() {
    const bool off = !PadTriggers::ForcedOff();
    PadTriggers::SetForcedOff(off);
    wcscpy_s(notice_, off ? L"Adaptive triggers forced off (saved in PBZDebugMenu.ini)." : L"Adaptive triggers left to the game (saved).");
    UpdatePanel(); return true;
}
bool HairControls::RestoreBounds() {
    if (!bounds_on_) return true;
    uintptr_t pointer = 0; uint64_t current = 0; int32_t shader = -1;
    const bool identity = HairMemory::Read(base_ + kViewModeCache, &pointer, sizeof(pointer)) && pointer == bounds_pointer_;
    bool view_ok = false, shader_ok = false;
    if (identity && HairMemory::Pair(pointer, 40, current))
        view_ok = current != kBoundsMode || HairMemory::Exchange64(pointer, kBoundsMode, bounds_original_);
    if (HairMemory::Read(base_ + kShaderPrint, &shader, sizeof(shader)))
        shader_ok = shader != 1 || HairMemory::Exchange32(base_ + kShaderPrint, 1, shader_original_);
    bounds_on_ = false;
    wcscpy_s(notice_, view_ok && shader_ok ? L"GPU AABB off; original debug values restored." : L"Restore skipped changed/unavailable data.");
    return view_ok && shader_ok;
}
bool HairControls::RestoreHair() {
    if (!hair_saved_) return true;
    uintptr_t pointer = 0; uint64_t current = 0;
    const bool ok = HairMemory::Read(base_ + kHairWrapper + 16, &pointer, sizeof(pointer)) && pointer == hair_pointer_ &&
        HairMemory::Pair(pointer, 2, current) && (current != hair_written_ || HairMemory::Exchange64(pointer, current, hair_original_));
    hair_saved_ = false; return ok;
}
void HairControls::Restore() {
    if (GroomForce::Active()) GroomForce::Request(GroomForce::kOff);
    const bool bounds = RestoreBounds(); const bool hair = RestoreHair();
    wcscpy_s(notice_, bounds && hair ? L"Original hair/debug values restored where still owned." : L"Restore skipped changed/unavailable data.");
    UpdatePanel();
}
void HairControls::ShowPanel(bool show) {
    shown_ = show;
    if (DebugHub::Enabled()) { if (show) DebugHub::ShowPage(DebugHub::Page::Groom); else DebugHub::Hide(); return; }
    if (panel_) ShowWindow(panel_, show && Foreground() ? SW_SHOWNOACTIVATE : SW_HIDE);
}
void HairControls::Tick() {
    MSG message{};
    if (!DebugHub::Enabled()) while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
    const bool foreground = Foreground();
    for (int i = 0; i < 4; ++i) {
        const bool down = (GetAsyncKeyState(kKeys[i]) & 0x8000) != 0;
        const bool press = !DebugHub::Enabled() && foreground && down && !key_down_[i];
        key_down_[i] = down;
        if (!press) continue;
        if (i == 0) { if (GetAsyncKeyState(VK_SHIFT) & 0x8000) continue; if (DebugHub::Enabled()) DebugHub::ShowPage(DebugHub::Page::Groom); else ShowPanel(!shown_); }
        else if (i == 1) ToggleHair();
        else if (i == 2) { if (GetAsyncKeyState(VK_SHIFT) & 0x8000) ToggleTriggers(); else ToggleBounds(); }
        else if (GetAsyncKeyState(VK_SHIFT) & 0x8000) ToggleOutfit();
        else ToggleForce();
    }
    if (GetTickCount64() >= next_refresh_) { UpdatePanel(); if (!DebugHub::Enabled()) ShowPanel(shown_); next_refresh_ = GetTickCount64() + 250; }
}
HWND HairControls::CreatePage(HWND parent) {
    if (!panel_) CreatePanel();
    if (!panel_) return nullptr;
    DebugHub::Attach(panel_,parent,490,425); UpdatePanel(); return panel_;
}
void HairControls::CreatePanel() {
    const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetModuleHandleW(nullptr));
    WNDCLASSEXW wc{sizeof(wc)}; wc.lpfnWndProc = WindowProc; wc.hInstance = instance;
    wc.lpszClassName = L"PBZHairControls_v3"; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW); wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return;
    panel_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"PBZ Hair Debug  |  F7 hide/show", WS_CAPTION | WS_SYSMENU,
        24, 60, 490, 462, nullptr, nullptr, instance, this);
    if (!panel_) return;
    const HFONT font = UiTheme::GetFont(UiTheme::Font::Body, panel_);
    auto child = [&](const wchar_t* type, const wchar_t* text, DWORD style, int id) {
        if (!_wcsicmp(type,L"BUTTON")) style|=WS_TABSTOP;
        HWND handle = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10,
            panel_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
        SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); UiI18n::Bind(handle,text); return handle;
    };
    status_text_ = child(L"STATIC", L"Waiting for hair CVars...", SS_LEFT, 0);
    mode_section_ = child(L"STATIC", L"毛发表示", SS_LEFT, 0);
    const bool hub = DebugHub::Enabled();
    hair_button_ = child(L"BUTTON", hub ? L"蒙皮 / Groom" : L"F8  Mesh / Groom", BS_PUSHBUTTON, kHairButton);
    bounds_button_ = child(L"BUTTON", hub ? L"GPU AABB：关" : L"F9  GPU AABB", BS_PUSHBUTTON, kBoundsButton);
    restore_button_ = child(L"BUTTON", L"Restore original", BS_PUSHBUTTON, kRestoreButton);
    force_section_ = child(L"STATIC", L"强制 Groom 与外观", SS_LEFT, 0);
    force_button_ = child(L"BUTTON", hub ? L"强制 Groom：关" : L"F6  Force Groom", BS_PUSHBUTTON, kForceButton);
    force_text_ = child(L"STATIC", L"", SS_LEFT, 0);
    outfit_button_ = child(L"BUTTON", hub ? L"套装：关" : L"Shift+F6  Outfits", BS_PUSHBUTTON, kOutfitButton);
    outfit_text_ = child(L"STATIC", L"", SS_LEFT, 0);
    triggers_button_ = child(L"BUTTON", hub ? L"扳机：游戏" : L"Shift+F9  Triggers", BS_PUSHBUTTON, kTriggersButton);
    triggers_text_ = child(L"STATIC", L"", SS_LEFT, 0);
    boss_button_ = child(L"BUTTON", hub ? L"Boss 连战 / 角色替换" : L"F10  Boss Rush / 角色替换", BS_PUSHBUTTON, kBossButton);
    EnableWindow(boss_button_, open_boss_menu_ != nullptr);
    note_text_ = child(L"STATIC", L"Only strand GPU AABBs; cards/mesh have no strand cluster boxes.", SS_LEFT, 0);
    UiTheme::SetRole(status_text_, UiTheme::Role::Card); UiTheme::SetRole(note_text_, UiTheme::Role::Hint);
    UiTheme::SetRole(mode_section_, UiTheme::Role::Section); UiTheme::SetRole(force_section_, UiTheme::Role::Section);
    for (HWND text : {force_text_, outfit_text_, triggers_text_}) UiTheme::SetRole(text, UiTheme::Role::Hint);
    Layout();
    UpdatePanel(); if (!DebugHub::Enabled()) ShowPanel(true); else { shown_=false; ShowWindow(panel_,SW_HIDE); }
}
void HairControls::Layout() {
    if (!panel_ || !note_text_) return;
    const UiLayout::Metrics m = UiLayout::For(panel_);
    RECT client{}; GetClientRect(panel_, &client);
    const int margin = m.Margin(), gap = m.Gap(), x = margin, width = (std::max)(m.S(520), static_cast<int>(client.right) - 2 * margin);
    int y = margin;
    const int status = (std::max)(UiLayout::TextHeight(status_text_, width - m.S(24), 4) + m.S(18), 4 * m.Line() + m.S(18));
    UiLayout::Place(status_text_, x, y, width, status); y += status + gap * 2;
    UiLayout::Place(mode_section_, x, y, width, m.Section()); y += m.Section() + gap / 2;
    y = UiLayout::ButtonRow({hair_button_, bounds_button_, restore_button_}, x, y, width, m, m.S(190)) + gap * 2;
    UiLayout::Place(force_section_, x, y, width, m.Section()); y += m.Section() + gap / 2;
    // Each toggle sits beside its live description, in a two-column grid.
    const HWND rows[][2] = {{force_button_, force_text_}, {outfit_button_, outfit_text_}, {triggers_button_, triggers_text_}};
    int column = m.S(200); for (const auto& row : rows) column = (std::max)(column, UiLayout::ButtonWidth(row[0], m));
    column = (std::min)(column, width / 2);
    const int text_width = width - column - gap * 2;
    for (const auto& row : rows) {
        const int text = (std::max)(m.Button(), UiLayout::TextHeight(row[1], text_width, 2));
        UiLayout::Place(row[0], x, y, column, m.Button());
        UiLayout::Place(row[1], x + column + gap * 2, y + (std::min)(m.S(7), (text - m.Line()) / 2), text_width, text);
        y += text + gap;
    }
    y += gap;
    y = UiLayout::ButtonRow({boss_button_}, x, y, width, m, m.S(240)) + gap;
    const int note = UiLayout::TextHeight(note_text_, width);
    UiLayout::Place(note_text_, x, y, width, note); y += note + margin;
    DebugHub::SetMinimum(panel_, m.S(520) + 2 * margin, y);
}
void HairControls::UpdatePanel() {
    if (!panel_ || !status_text_) return;
    uintptr_t pointer = 0; uint64_t pair = 0; int32_t applied = -1;
    const bool hair_ok = HairMemory::Read(base_ + kHairWrapper + 16, &pointer, sizeof(pointer)) && HairMemory::Pair(pointer, 2, pair);
    HairMemory::Read(base_ + kAppliedHairMode, &applied, sizeof(applied));
    if (applied < 0 || applied > 2) applied = -1;
    wchar_t text[640]{};
    const auto format=UiI18n::Translate(L"Hair CVar: %s     Game cache: %s\r\nGPU AABB request: %s  (ViewMode 32 + ShaderPrint)\r\n%s\r\nEmpty/hidden/card-only Grooms may show no boxes.");
    const auto mode=UiI18n::Translate(hair_ok?ModeName(static_cast<int32_t>(pair & 0xffffffffu)):L"Unavailable"),cache=UiI18n::Translate(ModeName(applied));
    const auto bounds=UiI18n::Translate(bounds_on_?L"ON":L"OFF"),notice=UiI18n::Translate(notice_);
    swprintf_s(text,format.c_str(),mode.c_str(),cache.c_str(),bounds.c_str(),notice.c_str());
    UiI18n::SetText(status_text_, text);
    const bool hub = DebugHub::Enabled();
    UiI18n::SetText(bounds_button_, bounds_on_ ? (hub ? L"GPU AABB：开" : L"F9  GPU AABB: ON") : (hub ? L"GPU AABB：关" : L"F9  GPU AABB: OFF"));
    EnableWindow(hair_button_, hair_ok);
    wchar_t force[320]{}; GroomForce::Status(force, 320);
    UiI18n::SetText(force_text_, force);
    UiI18n::SetText(force_button_, GroomForce::Active() ? (hub ? L"强制 Groom：开" : L"F6  Force Groom: ON") : (hub ? L"强制 Groom：关" : L"F6  Force Groom: OFF"));
    EnableWindow(force_button_, GroomForce::Ready());
    wchar_t outfit[200]{}; GroomForce::OutfitStatus(outfit, 200);
    UiI18n::SetText(outfit_text_, outfit);
    UiI18n::SetText(outfit_button_, GroomForce::OutfitUnlockActive() ? (hub ? L"套装：开" : L"Shift+F6  Outfits: ON") : (hub ? L"套装：关" : L"Shift+F6  Outfits: OFF"));
    EnableWindow(outfit_button_, GroomForce::Ready());
    wchar_t triggers[200]{}; PadTriggers::Status(triggers, 200);
    UiI18n::SetText(triggers_text_, triggers);
    UiI18n::SetText(triggers_button_, PadTriggers::ForcedOff() ? (hub ? L"扳机：关" : L"Shift+F9  Triggers: OFF") : (hub ? L"扳机：游戏" : L"Shift+F9  Triggers: game"));
}
LRESULT CALLBACK HairControls::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    HairControls* self = reinterpret_cast<HairControls*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        self = static_cast<HairControls*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    }
    if (message == WM_MOUSEACTIVATE) return DebugHub::IsEmbedded(window)?MA_ACTIVATE:MA_NOACTIVATE;
    if (self && message == WM_CLOSE) { if (DebugHub::IsEmbedded(window)) DebugHub::Hide(); else { self->RestoreBounds(); self->ShowPanel(false); } return 0; }
    if (self && message == WM_DESTROY) self->panel_=nullptr;
    if (self && message == WM_SIZE) { self->Layout(); return 0; }
    if (self && message == WM_DPICHANGED_AFTERPARENT) { UiLayout::UseFont(window, UiTheme::GetFont(UiTheme::Font::Body, window)); self->Layout(); return 0; }
    if (self && message == WM_COMMAND && self->Foreground()) {
        const int command = LOWORD(wparam);
        if (command == kHairButton) self->ToggleHair();
        else if (command == kBoundsButton) self->ToggleBounds();
        else if (command == kRestoreButton) self->Restore();
        else if (command == kForceButton) self->ToggleForce();
        else if (command == kOutfitButton) self->ToggleOutfit();
        else if (command == kTriggersButton) self->ToggleTriggers();
        else if (command == kBossButton && self->open_boss_menu_) self->open_boss_menu_();
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
