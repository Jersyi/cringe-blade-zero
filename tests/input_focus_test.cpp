// Offline reproduction of the game-input path. A fake UnrealWindow runs on its
// own thread (the "game thread"), the real hub/hotkey/cursor/banner code runs on
// a worker thread exactly like proxy.cpp. Keys are injected with SendInput only
// while a window of this process is foreground, then counted where they land.
// Exit code = number of failed checks.
#include "debug_hub.h"
#include "entry_focus.h"
#include "ui_cursor.h"
#include "ui_glass.h"
#include "ui_host.h"
#include "ui_hotkeys.h"
#include "ui_i18n.h"
#include "ui_load_banner.h"
#include "ui_theme.h"
#include "motian_grab.h"
#include "motian_grab_menu.h"
#include "settings_store.h"
#include "grab_extras.h"
#include "gear_widget.h"
#include "air_skills.h"
#include "hub_pause.h"
#include "weapon_presets.h"
#include "weapon_presets_menu.h"
#include "donate_page.h"
#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// Grab backend stub for the page layout check (no engine in this harness).
namespace MotianGrab {
Snapshot GetSnapshot() {
    Snapshot s; s.ready = true; s.status = L"[harness] 模拟状态";
    Variant v; v.id = L"Mock_01"; v.name = L"模拟投技"; v.index = 0; v.boss_id = L"MoTian"; v.boss_name = L"魔渊"; v.supported = true; v.detail = L"harness";
    s.variants.push_back(v);
    BossInfo b; b.id = L"MoTian"; b.name = L"魔渊"; b.status = L"1 招"; b.move_count = 1; b.supported = true; s.bosses.push_back(b);
    return s;
}
bool RequestOptions(const Options&) { return true; }
bool RequestTrigger(int32_t) { return true; }
void RequestCancel() {}
}
// Weapon presets backend stub: real game weapon IDs, so the page draws the game's icons and names.
namespace WeaponPresets {
PresetView Preset(std::vector<std::wstring> main, std::vector<std::wstring> shaqi, int32_t main_index, int32_t shaqi_index) {
    PresetView view; view.known = true; view.main_weapons = std::move(main); view.shaqi_weapons = std::move(shaqi);
    view.main_index = main_index; view.shaqi_index = shaqi_index; return view;
}
Snapshot GetSnapshot() {
    Snapshot s; s.enabled = s.ready = s.can_save = s.touchpad_ready = true; s.current_set = 0; s.swaps = 3;
    s.current = Preset({L"BaiShanShuangJian", L"Double"}, {L"MiaoShouFeiDao", L"DuYanGong"}, 0, 1);
    s.sets[0] = s.current;
    s.sets[1] = Preset({L"JadeDualChakram", L"SnakeDual"}, {L"YouLongTou", L"CuiXinPiPa"}, 1, 0);
    s.status = L"[harness] 模拟状态"; s.read_status = L"[harness] 已读取"; s.touchpad_status = L"[harness] 触控板";
    return s;
}
bool RequestEnable(bool) { return true; }
bool RequestSwap() { return true; }
bool RequestReadCurrent() { return true; }
bool RequestSaveToSet(int32_t) { return true; }
bool RequestSwitchToSet(int32_t) { return true; }
}
namespace AirSkills {
Options harness_air;
Snapshot GetSnapshot() { Snapshot s; s.options = harness_air; return s; }
bool RequestOptions(const Options& o) { harness_air = o; return true; }
}
namespace {
HWND game_ = nullptr;
DWORD game_tid_ = 0;
std::atomic<int> game_keys_{0}, game_setfocus_{0}, game_killfocus_{0}, game_clicks_{0};
bool real_pads_ = false;  // --pads: the machine's own XInput / HID pads instead of the fake one
// UE marks Slate inactive on WM_ACTIVATEAPP(FALSE); CommonUI then ignores the mouse in game menus.
std::atomic<int> game_app_off_{0}, game_app_on_{0}, game_inactive_{0}, game_active_{0}, game_off_by_worker_{0};
std::atomic<DWORD> game_app_off_thread_{0}, worker_tid_{0};
std::atomic<bool> stop_{false};
std::atomic<unsigned> fake_pad_{0};
std::atomic<int> pad_fires_{0};
std::atomic<long long> sent_at_{0};
std::atomic<long long> worst_latency_us_{0};
LARGE_INTEGER frequency_{};
FILE* log_ = nullptr;
int failures_ = 0;

void Log(const char* format, ...) {
    char line[512]{};
    va_list args; va_start(args, format); vsnprintf(line, sizeof(line), format, args); va_end(args);
    std::printf("%s\n", line); std::fflush(stdout);
    if (log_) { std::fprintf(log_, "%s\n", line); std::fflush(log_); }
}
void LogLine(const char* text) { Log("  [module] %s", text); }
long long Now() { LARGE_INTEGER value{}; QueryPerformanceCounter(&value); return value.QuadPart; }
void Check(bool ok, const char* what) { Log("%s %s", ok ? "PASS" : "FAIL", what); if (!ok) ++failures_; }

LRESULT CALLBACK GameProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_KEYDOWN:
        if (wparam == VK_F24) {
            ++game_keys_;
            const long long sent = sent_at_.load();
            if (sent) {
                const long long us = (Now() - sent) * 1000000 / frequency_.QuadPart;
                long long worst = worst_latency_us_.load();
                while (us > worst && !worst_latency_us_.compare_exchange_weak(worst, us)) {}
            }
        }
        break;
    case WM_SETFOCUS: ++game_setfocus_; break;
    case WM_KILLFOCUS: ++game_killfocus_; break;
    case WM_LBUTTONDOWN: ++game_clicks_; break;
    case WM_ACTIVATEAPP: if (!wparam) { ++game_app_off_; game_app_off_thread_.store(static_cast<DWORD>(lparam)); if (static_cast<DWORD>(lparam) == worker_tid_.load()) ++game_off_by_worker_; } else ++game_app_on_; break;
    case WM_ACTIVATE: if (LOWORD(wparam) == WA_INACTIVE) ++game_inactive_; else ++game_active_; break;
    case WM_PAINT: {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(window, &paint);
        FillRect(dc, &paint.rcPaint, static_cast<HBRUSH>(GetStockObject(DKGRAY_BRUSH)));
        EndPaint(window, &paint); return 0;
    }
    default: break;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
DWORD WINAPI GameMain(void* ready) {
    WNDCLASSW cls{}; cls.lpfnWndProc = GameProc; cls.hInstance = GetModuleHandleW(nullptr);
    cls.lpszClassName = L"UnrealWindow"; cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    RegisterClassW(&cls);
    game_ = CreateWindowExW(0, L"UnrealWindow", L"PBZ input focus test (fake game)", WS_POPUP | WS_VISIBLE,
        80, 60, 1600, 1000, nullptr, nullptr, cls.hInstance, nullptr);
    game_tid_ = GetCurrentThreadId();
    SetEvent(static_cast<HANDLE>(ready));
    // The engine pumps its queue once per frame and runs the post-tick hook.
    while (!stop_.load()) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        UiCursor::AfterTick();
        MsgWaitForMultipleObjectsEx(0, nullptr, 16, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }
    DestroyWindow(game_);
    return 0;
}
const wchar_t* const kTitles[] = {L"Boss 连战", L"Boss AI", L"角色替换", L"NPC 重定向", L"招式派生", L"空中技能 / JC", L"投技", L"毛发 / Groom", L"武器预设", L"快捷键"};
// Every page but the optional supplement one, which the DLL registers only when its container is present.
static_assert(std::size(kTitles) == static_cast<size_t>(DebugHub::Page::Supplement), "one title per always-present page");
HWND MockPage(HWND parent) {
    HWND page = CreateWindowExW(0, L"STATIC", L"mock page", WS_POPUP, 0, 0, 640, 420, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    DebugHub::Attach(page, parent, 640, 420);
    return page;
}
std::wstring IniPath() {
    wchar_t ini[MAX_PATH]{}; GetTempPathW(MAX_PATH, ini); wcscat_s(ini, L"PBZInputFocusTest.ini");
    return ini;
}
// One fake Xbox pad in XInput slot 0: the test machine has none. Buttons come from fake_pad_.
DWORD WINAPI FakeXInput(DWORD user, void* state) {
    if (user != 0) return ERROR_DEVICE_NOT_CONNECTED;
    static DWORD packet = 0;
    auto* bytes = static_cast<unsigned char*>(state);
    std::memset(bytes, 0, 16);
    ++packet; std::memcpy(bytes, &packet, 4);
    const WORD buttons = static_cast<WORD>(fake_pad_.load());
    std::memcpy(bytes + 4, &buttons, 2);
    return ERROR_SUCCESS;
}
// Which worker window takes activation (the game thread then sees WM_ACTIVATEAPP FALSE).
LRESULT CALLBACK WorkerCbt(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HCBT_ACTIVATE) {
        wchar_t name[96]{}; GetClassNameW(reinterpret_cast<HWND>(wparam), name, 96);
        Log("  [cbt] worker activates %p (%ls) child=%d", reinterpret_cast<void*>(wparam), name,
            (GetWindowLongPtrW(reinterpret_cast<HWND>(wparam), GWL_STYLE) & WS_CHILD) ? 1 : 0);
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
DWORD WINAPI WorkerMain(void* ready) {
    worker_tid_.store(GetCurrentThreadId());
    SetWindowsHookExW(WH_CBT, &WorkerCbt, nullptr, GetCurrentThreadId());
    const std::wstring path = IniPath(); const wchar_t* ini = path.c_str();
    if (!real_pads_) UiHotkeys::SetXInputForTest(&FakeXInput);
    UiHotkeys::SetLogger(&LogLine);
    UiHost::GuardActivation(&LogLine); UiCursor::SetLogger(&LogLine);
    UiI18n::Initialize(ini); UiTheme::Initialize(); DebugHub::Initialize(); UiHotkeys::Initialize(ini);
    UiHotkeys::Bind(UiHotkeys::Action::GearUp, [] { ++pad_fires_; });
    UiHotkeys::Bind(UiHotkeys::Action::ToggleHub, [] { DebugHub::Toggle(); });  // as the mod binds it
    UiLoadBanner::Initialize(&LogLine); EntryFocus::Initialize();
    SettingsStore::Initialize(ini); HubPause::Initialize(&LogLine); GrabExtras::Initialize(&LogLine); MotianGrabMenu::Initialize(&LogLine); GearWidget::Initialize(&LogLine);
    for (int page = 0; page < static_cast<int>(DebugHub::Page::Supplement); ++page)
        if (page != static_cast<int>(DebugHub::Page::Hotkeys)) DebugHub::Register(static_cast<DebugHub::Page>(page), kTitles[page], &MockPage);
    DebugHub::Register(DebugHub::Page::Hotkeys, L"快捷键", &UiHotkeys::CreatePage);
    DebugHub::Register(DebugHub::Page::MotianGrab, L"投技", &MotianGrabMenu::CreatePage);
    DebugHub::Register(DebugHub::Page::WeaponPresets, L"武器预设", &WeaponPresetsMenu::CreatePage);
    // Supplement stays unregistered, so the Donate row follows the hotkey row directly.
    DebugHub::Register(DebugHub::Page::Donate, L"Donate", &DonatePage::CreatePage);
    UiLoadBanner::ShowSuccess();
    SetEvent(static_cast<HANDLE>(ready));
    while (!stop_.load()) {
        UiHotkeys::Tick(); DebugHub::Tick(); UiLoadBanner::Tick(); EntryFocus::Poll(); MotianGrabMenu::Tick(); WeaponPresetsMenu::Tick(); DonatePage::Tick(); GearWidget::Tick();
        Sleep(DebugHub::Visible() || DebugHub::Animating() || UiLoadBanner::Active() ? 16 : 25);
    }
    WeaponPresetsMenu::Shutdown(); DonatePage::Shutdown(); DebugHub::Shutdown(); UiLoadBanner::Shutdown(); UiGlass::Shutdown(); UiI18n::Shutdown(); UiTheme::Shutdown();
    return 0;
}
void Foreground(HWND window);
bool OursForeground() {
    DWORD process = 0; const HWND foreground = GetForegroundWindow();
    if (foreground) GetWindowThreadProcessId(foreground, &process);
    return process == GetCurrentProcessId();
}
void Foreground(HWND window) {
    const HWND current = GetForegroundWindow();
    const DWORD other = current ? GetWindowThreadProcessId(current, nullptr) : 0, self = GetCurrentThreadId();
    const BOOL linked = other && other != self ? AttachThreadInput(self, other, TRUE) : FALSE;
    SetForegroundWindow(window); BringWindowToTop(window);
    if (linked) AttachThreadInput(self, other, FALSE);
}
std::string Describe(HWND window) {
    if (!window) return "null";
    wchar_t name[96]{}; GetClassNameW(window, name, 96);
    char text[160]{}; std::snprintf(text, sizeof(text), "%p(%ls,tid=%lu,visible=%d)", static_cast<void*>(window), name,
        GetWindowThreadProcessId(window, nullptr), IsWindowVisible(window) ? 1 : 0);
    return text;
}
void Report(const char* phase) {
    GUITHREADINFO info{sizeof(info)};
    GetGUIThreadInfo(game_tid_, &info);
    Log("[%s] foreground=%s game-thread focus=%s active=%s game set/kill focus=%d/%d app off/on=%d/%d window off/on=%d/%d (off by tid %lu)", phase,
        Describe(GetForegroundWindow()).c_str(), Describe(info.hwndFocus).c_str(), Describe(info.hwndActive).c_str(),
        game_setfocus_.load(), game_killfocus_.load(), game_app_off_.load(), game_app_on_.load(), game_inactive_.load(), game_active_.load(), game_app_off_thread_.load());
}
// Presses F24 (unbound everywhere) count times; returns how many the fake game saw.
int Keys(int count) {
    if (!OursForeground()) { Foreground(game_); Sleep(150); }
    if (!OursForeground()) { Log("  skipped input: another process owns the foreground"); return -1; }
    const int before = game_keys_.load();
    for (int i = 0; i < count; ++i) {
        INPUT key[2]{};
        key[0].type = key[1].type = INPUT_KEYBOARD; key[0].ki.wVk = key[1].ki.wVk = VK_F24; key[1].ki.dwFlags = KEYEVENTF_KEYUP;
        sent_at_.store(Now()); SendInput(2, key, sizeof(INPUT)); Sleep(40);
    }
    Sleep(400);
    return game_keys_.load() - before;
}
void Press(WORD vk) {
    if (!OursForeground()) { Log("  skipped key: another process owns the foreground"); return; }
    INPUT key[2]{};
    key[0].type = key[1].type = INPUT_KEYBOARD; key[0].ki.wVk = key[1].ki.wVk = vk; key[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, key, sizeof(INPUT));
}
bool ClickAt(POINT at) {
    if (!OursForeground()) { Foreground(game_); Sleep(150); }
    if (!OursForeground()) { Log("  skipped click: another process owns the foreground"); return false; }
    POINT old{}; GetCursorPos(&old);
    SetCursorPos(at.x, at.y); Sleep(60);
    INPUT click[2]{};
    click[0].type = click[1].type = INPUT_MOUSE; click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &click[0], sizeof(INPUT)); Sleep(90); SendInput(1, &click[1], sizeof(INPUT)); Sleep(60);
    SetCursorPos(old.x, old.y);
    return true;
}
// Press at from, move in steps to to, release (real input, so capture is exercised).
bool DragAt(POINT from, POINT to) {
    if (!OursForeground()) { Foreground(game_); Sleep(150); }
    if (!OursForeground()) { Log("  skipped drag: another process owns the foreground"); return false; }
    POINT old{}; GetCursorPos(&old);
    SetCursorPos(from.x, from.y); Sleep(80);
    INPUT button{}; button.type = INPUT_MOUSE; button.mi.dwFlags = MOUSEEVENTF_LEFTDOWN; SendInput(1, &button, sizeof(INPUT)); Sleep(120);
    for (int i = 1; i <= 12; ++i) { SetCursorPos(from.x + (to.x - from.x) * i / 12, from.y + (to.y - from.y) * i / 12); Sleep(35); }
    Sleep(150);
    button.mi.dwFlags = MOUSEEVENTF_LEFTUP; SendInput(1, &button, sizeof(INPUT)); Sleep(120);
    SetCursorPos(old.x, old.y);
    return true;
}
struct Find { const wchar_t* cls; LONG style_mask; LONG style; HWND found; };
BOOL CALLBACK FindChild(HWND child, LPARAM data) {
    auto* f = reinterpret_cast<Find*>(data);
    wchar_t name[64]{}; GetClassNameW(child, name, 64);
    if (_wcsicmp(name, f->cls) || !IsWindowVisible(child)) return TRUE;
    if (f->style_mask && (GetWindowLongW(child, GWL_STYLE) & f->style_mask) != f->style) return TRUE;
    RECT r{}; GetWindowRect(child, &r); RECT g{}; GetWindowRect(game_, &g);
    if (r.top < g.top || r.bottom > g.bottom || r.right - r.left < 8) return TRUE;
    f->found = child; return FALSE;
}
HWND Visible(const wchar_t* cls, LONG mask = 0, LONG style = 0) { Find f{cls, mask, style, nullptr}; EnumChildWindows(game_, FindChild, reinterpret_cast<LPARAM>(&f)); return f.found; }
BOOL CALLBACK FindGrabPage(HWND child, LPARAM data) {
    wchar_t name[64]{}; GetClassNameW(child, name, 64);
    if (_wcsicmp(name, L"PBZMotianGrabWindow")) return TRUE;
    *reinterpret_cast<HWND*>(data) = child; return FALSE;
}
POINT Center(HWND w) { RECT r{}; GetWindowRect(w, &r); return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }
POINT Left(HWND w) { RECT r{}; GetWindowRect(w, &r); return {r.left + 10, (r.top + r.bottom) / 2}; }
struct BindFind { LONG_PTR index; HWND found; };
BOOL CALLBACK FindBind(HWND child, LPARAM data) {
    auto* f = reinterpret_cast<BindFind*>(data);
    wchar_t name[64]{}; GetClassNameW(child, name, 64);
    if (_wcsicmp(name, L"PBZHotkeyBind_v1") || GetWindowLongPtrW(child, GWLP_USERDATA) != f->index) return TRUE;
    f->found = child; return FALSE;
}
// A bind box of one set: its user data is action * 4 + device (keyboard, Xbox, PlayStation).
HWND BindBox(UiHotkeys::Action action, UiHotkeys::Device device) {
    BindFind f{static_cast<LONG_PTR>(action) * 4 + static_cast<LONG_PTR>(device), nullptr}; EnumChildWindows(game_, FindBind, reinterpret_cast<LPARAM>(&f)); return f.found;
}
std::wstring Saved(const wchar_t* key) {
    wchar_t value[64]{}; GetPrivateProfileStringW(L"Hotkeys", key, L"", value, 64, IniPath().c_str());
    return value;
}
std::wstring Shown(UiHotkeys::Action action) { return UiHotkeys::Plain(UiHotkeys::Get(action)); }
// A key held long enough for the worker's poll to see it (SendInput taps are over within a poll).
void Hold(WORD vk, DWORD ms) {
    INPUT key[1]{}; key[0].type = INPUT_KEYBOARD; key[0].ki.wVk = vk;
    SendInput(1, key, sizeof(INPUT)); Sleep(ms);
    key[0].ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1, key, sizeof(INPUT)); Sleep(150);
}
// DualSense USB input report: Triangle + Square, d-pad neutral (hat 8).
void FeedDualSense(DWORD ms) {
    unsigned char report[64]{}; report[0] = 0x01; report[8] = 0x08 | 0x10 | 0x80;
    for (DWORD at = 0; at < ms; at += 10) { UiHotkeys::NoteSonyReport(1, report, sizeof(report)); Sleep(10); }
}
// Clicks a bind box, holds the fake Xbox buttons, releases; true when the box was armed.
bool BindXbox(HWND box, unsigned buttons, DWORD sony_ms = 0) {
    PostMessageW(box, WM_LBUTTONDOWN, MK_LBUTTON, 0); PostMessageW(box, WM_LBUTTONUP, 0, 0); Sleep(300);
    const bool armed = UiHotkeys::Capturing();
    fake_pad_.store(buttons); Sleep(30);
    if (sony_ms) FeedDualSense(sony_ms); else Sleep(270);
    fake_pad_.store(0); Sleep(350);
    return armed;
}
HWND Focused() { GUITHREADINFO info{sizeof(info)}; GetGUIThreadInfo(game_tid_, &info); return info.hwndFocus; }
bool ClassOf(HWND window, const wchar_t* cls) { wchar_t name[64]{}; return window && GetClassNameW(window, name, 64) > 0 && !_wcsicmp(name, cls); }
// A fake pad press held long enough for the worker's poll (shorter than the hub's repeat delay), then the release.
void Tap(unsigned buttons, DWORD hold = 90) { fake_pad_.store(buttons); Sleep(hold); fake_pad_.store(0); Sleep(120); }
int Click() {
    if (!OursForeground()) { Log("  skipped click: another process owns the foreground"); return -1; }
    RECT rect{}; GetWindowRect(game_, &rect);
    const int before = game_clicks_.load();
    POINT old{}; GetCursorPos(&old);
    SetCursorPos(rect.left + 40, rect.bottom - 40);
    INPUT click[2]{};
    click[0].type = click[1].type = INPUT_MOUSE; click[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN; click[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, click, sizeof(INPUT)); Sleep(300);
    SetCursorPos(old.x, old.y);
    return game_clicks_.load() - before;
}
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR arguments, int) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    QueryPerformanceFrequency(&frequency_);
    wchar_t path[MAX_PATH]{}; GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring log_path = path; log_path = log_path.substr(0, log_path.find_last_of(L"\\/") + 1) + L"input_focus_test.log";
    if (arguments && *arguments) log_path = arguments;
    _wfopen_s(&log_, log_path.c_str(), L"w");
    HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    // --pads=<seconds>: the real pads (XInput and HID, no fake) over the fake game; the PAD / HOTKEY lines the
    // mod would log, for that long (press buttons meanwhile), then exit. A check of what this machine's pads read as.
    if (const wchar_t* pads = arguments ? wcsstr(arguments, L"--pads=") : nullptr) {
        real_pads_ = true;
        HANDLE g = CreateThread(nullptr, 0, GameMain, ready, 0, nullptr); WaitForSingleObject(ready, 5000);
        Foreground(game_); Sleep(200);
        HANDLE w = CreateThread(nullptr, 0, WorkerMain, ready, 0, nullptr); WaitForSingleObject(ready, 5000);
        Sleep(static_cast<DWORD>((std::max)(1, _wtoi(pads + 7))) * 1000);
        Log("pads: in hand=%d prompts=%d", static_cast<int>(UiHotkeys::ActivePad()), static_cast<int>(UiHotkeys::InputDevice()));
        stop_.store(true); PostThreadMessageW(game_tid_, WM_NULL, 0, 0);
        WaitForSingleObject(w, 5000); WaitForSingleObject(g, 5000);
        return 0;
    }
    // --shot=<file.bmp> [--page=N] [--lang=en]: open the hub over the fake game, save its pixels, exit.
    const wchar_t* shot = arguments ? wcsstr(arguments, L"--shot=") : nullptr;
    if (shot) {
        std::wstring target = shot + 7; target = target.substr(0, target.find(L' '));
        const wchar_t* page = wcsstr(arguments, L"--page=");
        HANDLE g = CreateThread(nullptr, 0, GameMain, ready, 0, nullptr); WaitForSingleObject(ready, 5000);
        Foreground(game_); Sleep(200);
        HANDLE w = CreateThread(nullptr, 0, WorkerMain, ready, 0, nullptr); WaitForSingleObject(ready, 5000);
        if (wcsstr(arguments, L"--lang=en")) UiI18n::SetLanguage(UiI18n::Language::English);
        if (wcsstr(arguments, L"--banner")) { const wchar_t* at = wcsstr(arguments, L"--at="); Sleep(at ? static_cast<DWORD>(_wtoi(at + 5)) : 1300); }
        else { Sleep(4300); }
        if (!wcsstr(arguments, L"--banner")) DebugHub::ShowPage(static_cast<DebugHub::Page>(page ? _wtoi(page + 7) : static_cast<int>(DebugHub::Page::Hotkeys)));
        // Gear first: the expanded panel collapses 1.6 s after the cursor leaves it.
        const wchar_t* gear = wcsstr(arguments, L"--gear=");
        if (gear) { Sleep(1500); GearWidget::Nudge(_wtoi(gear + 7)); Sleep(450); }
        else if (!wcsstr(arguments, L"--banner") && !wcsstr(arguments, L"--expand")) Sleep(1500);
        if (wcsstr(arguments, L"--expand")) {
            // Click the compact gear pill (bottom-right, 196x48 DIP) to open the full panel.
            Sleep(1500);
            HWND pill = FindWindowExW(game_, nullptr, L"PBZGearWidget_v1", nullptr);
            RECT box{}; GetClientRect(pill, &box);
            const LPARAM at = MAKELPARAM(box.right - box.right * 110 / 360, box.bottom - box.bottom * 24 / 326);
            PostMessageW(pill, WM_LBUTTONDOWN, MK_LBUTTON, at); PostMessageW(pill, WM_LBUTTONUP, 0, at);
            Log("expand: widget=%p client=%ldx%ld click=%d,%d foreground_ours=%d", static_cast<void*>(pill), box.right, box.bottom, GET_X_LPARAM(at), GET_Y_LPARAM(at), OursForeground() ? 1 : 0);
            Sleep(400);
        }
        if (const wchar_t* wheel = wcsstr(arguments, L"--wheel=")) {
            RECT area{}; GetWindowRect(game_, &area);
            POINT at{area.left + (area.right - area.left) * 2 / 3, area.top + (area.bottom - area.top) / 2};
            HWND under = WindowFromPoint(at);
            for (int i = 0; i < _wtoi(wheel + 8); ++i) { PostMessageW(under, WM_MOUSEWHEEL, MAKEWPARAM(0, -WHEEL_DELTA), MAKELPARAM(at.x, at.y)); Sleep(60); }
            Sleep(900);
        }
        RECT r{}; GetWindowRect(game_, &r); const int width = r.right - r.left, height = r.bottom - r.top;
        HDC screen = GetDC(nullptr); HDC dc = CreateCompatibleDC(screen);
        BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = width; info.bmiHeader.biHeight = -height;
        info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32; void* bits = nullptr;
        HBITMAP bitmap = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0); HGDIOBJ old = SelectObject(dc, bitmap);
        // Renders the window and its children even while other windows cover it.
        if (!PrintWindow(game_, dc, 2 /*PW_RENDERFULLCONTENT*/)) BitBlt(dc, 0, 0, width, height, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
        GdiFlush();
        BITMAPFILEHEADER file{}; file.bfType = 0x4d42; file.bfOffBits = sizeof(file) + sizeof(BITMAPINFOHEADER); file.bfSize = file.bfOffBits + width * height * 4;
        FILE* out = nullptr; _wfopen_s(&out, target.c_str(), L"wb");
        if (out) { fwrite(&file, sizeof(file), 1, out); fwrite(&info.bmiHeader, sizeof(BITMAPINFOHEADER), 1, out); fwrite(bits, 4, static_cast<size_t>(width) * height, out); fclose(out); }
        SelectObject(dc, old); DeleteObject(bitmap); DeleteDC(dc); ReleaseDC(nullptr, screen);
        stop_.store(true); PostThreadMessageW(game_tid_, WM_NULL, 0, 0);
        WaitForSingleObject(w, 5000); WaitForSingleObject(g, 5000);
        return 0;
    }
    // Hotkeys start from an ini of the one-binding layout (no Layout=2): HairMode typed the way a player
    // edits the ini by hand (no device prefix, spaces around '+'), GrabRelease rebound to a pad chord.
    // The pad checks write their own chords.
    WritePrivateProfileStringW(L"Hotkeys", nullptr, nullptr, IniPath().c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"HairMode", L"Shift + F11", IniPath().c_str());
    WritePrivateProfileStringW(L"Hotkeys", L"GrabRelease", L"ps:Triangle+Cross", IniPath().c_str());
    HANDLE game = CreateThread(nullptr, 0, GameMain, ready, 0, nullptr);
    WaitForSingleObject(ready, 5000);
    Foreground(game_); Sleep(300);
    HANDLE worker = CreateThread(nullptr, 0, WorkerMain, ready, 0, nullptr);
    WaitForSingleObject(ready, 5000);
    {
        wchar_t hub[32]{}; GetPrivateProfileStringW(L"Hotkeys", L"ToggleHub", L"", hub, 32, IniPath().c_str());
        Log("  [ini] ToggleHub=%ls HairMode=%ls", hub, Shown(UiHotkeys::Action::HairMode).c_str());
        Check(!wcscmp(hub, L"kb:F10"), "the panel key is written to the ini with its default F10");
        const UiHotkeys::Chord hair = UiHotkeys::Get(UiHotkeys::Action::HairMode);
        Check(hair.device == UiHotkeys::Device::Keyboard && hair.key == VK_F11 && hair.modifiers == 2, "a hand-typed ini chord without prefix reads as a keyboard chord");
        Log("  [ini] Layout=%ls GrabRelease=%ls / %ls / %ls", Saved(L"Layout").c_str(), Saved(L"GrabRelease").c_str(), Saved(L"GrabRelease.Xbox").c_str(), Saved(L"GrabRelease.PS").c_str());
        Check(Saved(L"Layout") == L"3" && Saved(L"GrabRelease") == L"kb:G" && Saved(L"GrabRelease.Xbox") == L"xb:Y+A" && Saved(L"GrabRelease.PS") == L"ps:Triangle+Cross",
            "a one-binding pad chord migrates into the Xbox and PlayStation sets, the keyboard gets its default");
        Check(Saved(L"NpcSkill") == L"kb:X" && Saved(L"NpcSkill.Xbox") == L"xb:L3+R3" && Saved(L"WeaponSwap.Xbox") == L"xb:LB+L3" && Saved(L"WeaponSwap.PS") == L"ps:SwipeLeft+SwipeRight",
            "every device's defaults are written to the ini");
    }
    Sleep(600);
    Report("banner showing");
    Check(Keys(5) == 5, "keys reach the game while the load banner is up");
    for (int i = 0; i < 100 && UiLoadBanner::Active(); ++i) Sleep(50);
    Sleep(300);
    Report("banner done");
    Check(Keys(5) == 5, "keys reach the game after the banner");

    Foreground(game_); Sleep(150);
    DebugHub::Toggle(); Sleep(900);
    Report("hub open");
    Check(DebugHub::Visible(), "hub opens on toggle");
    const int while_open = Keys(3);
    Log("  keys seen by the game while the hub is open: %d (0 expected: the hub owns input)", while_open);
    Check(while_open == 0, "game does not react to keys while the hub is open");

    DebugHub::Toggle(); Sleep(900);
    Report("hub closed by toggle");
    Check(!DebugHub::Visible(), "hub closes on toggle");
    Check(Keys(5) == 5, "keys reach the game after closing the hub");
    Check(Click() == 1, "clicks reach the game after closing the hub");

    DebugHub::Toggle(); Sleep(900);
    Press(VK_ESCAPE); Sleep(900);
    Report("hub closed by Esc");
    Check(!DebugHub::Visible(), "Esc closes the hub");
    Check(Keys(5) == 5, "keys reach the game after Esc");

    DebugHub::ShowPage(DebugHub::Page::Hotkeys); Sleep(900);
    // No supplement container here, so its page is not registered: asking for it must leave the hub where it is.
    DebugHub::ShowPage(DebugHub::Page::Supplement); Sleep(400);
    Check(Visible(L"PBZHotkeyBind_v1") != nullptr, "the absent supplement page is never shown (the hotkey page stays)");
    // The page after the missing one still opens (rows skip unregistered pages).
    DebugHub::ShowPage(DebugHub::Page::Donate); Sleep(600);
    Check(Visible(L"PBZDonatePage_v1") != nullptr, "the Donate page opens after the absent supplement page");
    DebugHub::ShowPage(DebugHub::Page::Hotkeys); Sleep(600);
    DebugHub::Toggle(); Sleep(900);
    Report("hotkey page closed");
    Check(Keys(5) == 5, "keys reach the game after visiting the hotkey page");
    // Mouse capture inside the open hub: the cursor gate must not steal it.
    DebugHub::ShowPage(DebugHub::Page::MotianGrab); Sleep(1000);
    if (!DebugHub::Visible()) { DebugHub::Toggle(); Sleep(900); }
    if (HWND box = Visible(L"Button", BS_TYPEMASK, BS_AUTOCHECKBOX)) {
        const LRESULT before = SendMessageW(box, BM_GETCHECK, 0, 0);
        ClickAt(Left(box)); Sleep(300);
        const LRESULT after = SendMessageW(box, BM_GETCHECK, 0, 0);
        Check(after != before, "checkbox toggles with a click while the hub is open");
        if (after != before) { ClickAt(Left(box)); Sleep(300); }
    } else Check(false, "checkbox found on the grab page");
    if (HWND combo = Visible(L"ComboBox")) {
        ClickAt(Center(combo)); Sleep(500);
        const HWND popup = FindWindowExW(game_, nullptr, L"PBZDropdownList_v1", nullptr);
        const HWND top = FindWindowW(L"PBZDropdownList_v1", nullptr);
        const bool open = (popup && IsWindowVisible(popup)) || (top && IsWindowVisible(top));
        Check(open, "drop-down list stays open after a click");
        Press(VK_ESCAPE); Sleep(400);
    } else Check(false, "combo box found on the grab page");
    // Header "pause game" switch: a click flips the persisted setting.
    if (HWND pause = FindWindowExW(FindWindowExW(game_, nullptr, L"PBZDebugHubWindow_v1", nullptr), nullptr, L"PBZDebugHubPause_v1", nullptr)) {
        const bool before = HubPause::Enabled();
        ClickAt(Center(pause)); Sleep(300);
        Check(HubPause::Enabled() != before, "pause switch toggles with a click");
        if (HubPause::Enabled() != before) { ClickAt(Center(pause)); Sleep(300); }
    } else Check(false, "pause switch found in the hub header");
    // Gear: press on the compact pill and drag sideways (one gear per 24 DIP), then drag the rail thumb.
    if (HWND gear = FindWindowExW(game_, nullptr, L"PBZGearWidget_v1", nullptr)) {
        RECT box{}; GetWindowRect(gear, &box);
        const double scale = (box.right - box.left) / 360.0;
        const int start = GearWidget::Level(), direction = start <= 5 ? 1 : -1;
        const POINT pill{box.right - static_cast<LONG>(110 * scale), box.bottom - static_cast<LONG>(24 * scale)};
        DragAt(pill, {pill.x + static_cast<LONG>(direction * (3 * 24 + 6) * scale), pill.y});
        Log("  gear pill drag: %d -> %d", start, GearWidget::Level());
        Check(GearWidget::Level() == start + 3 * direction, "gear pill press-and-drag moves three gears");
        Sleep(500);
        auto rail = [&](int value) { return POINT{box.left + static_cast<LONG>((30 + 30 * value) * scale), box.top + static_cast<LONG>((236 + 12) * scale)}; };
        const int from = GearWidget::Level();
        DragAt(rail(from), rail(2));
        Log("  gear rail drag: %d -> %d", from, GearWidget::Level());
        Check(GearWidget::Level() == 2, "gear rail thumb drags to gear 2");
    } else Check(false, "gear widget found while the hub is open");
    // Grab page vs the gear: a damage mode set outside the page (gear 4+ picks a multiplier)
    // must survive the page's own Apply, which writes every control back.
    {
        Sleep(800);  // the gear drag above applies its level after the release; let it land before the outside change
        GrabExtras::Options outside = GrabExtras::GetOptions();
        outside.damage = GrabExtras::Damage::Multiplier; outside.multiplier = 4.5f; GrabExtras::SetOptions(outside);
        Sleep(700);
        HWND page = nullptr; EnumChildWindows(game_, FindGrabPage, reinterpret_cast<LPARAM>(&page));
        if (page) {
            SendMessageW(page, WM_COMMAND, MAKEWPARAM(104, BN_CLICKED), 0);
            const GrabExtras::Options after = GrabExtras::GetOptions();
            Log("  grab page Apply after an outside change: damage mode=%d x%.2f", static_cast<int>(after.damage), static_cast<double>(after.multiplier));
            Check(after.damage == GrabExtras::Damage::Multiplier && after.multiplier > 4.49f && after.multiplier < 4.51f,
                "the grab page's Apply keeps a damage multiplier set outside the page");
            // Preset buttons (ids 120..124 = x4.5 / x10 / x15 / x20 / x30) set the multiplier at once, up to x30.
            SendMessageW(page, WM_COMMAND, MAKEWPARAM(124, BN_CLICKED), 0);
            const GrabExtras::Options preset = GrabExtras::GetOptions();
            Log("  grab page preset x30: damage mode=%d x%.2f", static_cast<int>(preset.damage), static_cast<double>(preset.multiplier));
            Check(preset.damage == GrabExtras::Damage::Multiplier && preset.multiplier > 29.99f && preset.multiplier < 30.01f, "the x30 preset sets the multiplier mode at x30");
            SendMessageW(page, WM_COMMAND, MAKEWPARAM(104, BN_CLICKED), 0);
            Check(GrabExtras::GetOptions().multiplier > 29.99f, "Apply keeps the x30 preset (the cap is x30)");
        } else Check(false, "grab page window found");
    }
    // Pads: a fake XInput slot drives the real capture path (no Xbox pad on this machine).
    DebugHub::ShowPage(DebugHub::Page::Hotkeys); Sleep(900);
    if (!DebugHub::Visible()) { DebugHub::Toggle(); Sleep(900); }
    if (!OursForeground()) { Foreground(game_); Sleep(150); }
    using UiHotkeys::Device;
    const HWND up = BindBox(UiHotkeys::Action::GearUp, Device::Xbox), down = BindBox(UiHotkeys::Action::GearDown, Device::PlayStation);
    const HWND glyphs = up ? FindWindowExW(GetParent(up), nullptr, L"PBZHotkeyGlyphs_v1", nullptr) : nullptr;
    if (!OursForeground()) Log("  skipped pad checks: another process owns the foreground");
    else if (up && down && glyphs && BindBox(UiHotkeys::Action::GearUp, Device::Keyboard)) {
        Check(BindXbox(up, 0x0100 | 0x0200), "bind box waits for a chord after a click");
        Log("  GearUp saved as %ls / %ls, shown as %ls", Saved(L"GearUp").c_str(), Saved(L"GearUp.Xbox").c_str(), Shown(UiHotkeys::Action::GearUp).c_str());
        Check(Saved(L"GearUp.Xbox") == L"xb:LB+RB" && Saved(L"GearUp") == L"kb:plus", "Xbox LB+RB records into the Xbox set; the keyboard binding stays");
        Check(UiHotkeys::InputDevice() == Device::Xbox && Shown(UiHotkeys::Action::GearUp) == L"LB + RB", "after an Xbox press the prompts show the Xbox set");
        const int fired = pad_fires_.load();
        // While the hub is up its pad navigation owns the pad (LB / RB change the page): GearUp stays quiet.
        fake_pad_.store(0x0100 | 0x0200); Sleep(250); fake_pad_.store(0); Sleep(250);
        Check(pad_fires_.load() == fired, "pad chords other than the panel's do not fire while the hub is up");
        DebugHub::Toggle(); Sleep(900);
        if (!OursForeground()) { Foreground(game_); Sleep(150); }
        fake_pad_.store(0x0100 | 0x0200); Sleep(250); fake_pad_.store(0); Sleep(250);
        Check(pad_fires_.load() == fired + 1, "Xbox LB+RB fires its action once");
        fake_pad_.store(0x0100); Sleep(250); fake_pad_.store(0); Sleep(250);
        Check(pad_fires_.load() == fired + 1, "LB alone does not fire LB+RB");
        DebugHub::ShowPage(DebugHub::Page::Hotkeys); Sleep(900);
        RECT g{}; GetClientRect(glyphs, &g);
        PostMessageW(glyphs, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(g.right - 6, g.bottom / 2)); Sleep(300);
        Log("  pad type PlayStation: PadGlyphs=%ls, active=%d", Saved(L"PadGlyphs").c_str(), static_cast<int>(UiHotkeys::ActivePad()));
        Check(Saved(L"PadGlyphs") == L"ps" && UiHotkeys::ActivePad() == Device::PlayStation, "the pad type switch picks the PlayStation set");
        DebugHub::Toggle(); Sleep(900);
        fake_pad_.store(0x0100 | 0x0200); Sleep(250); fake_pad_.store(0); Sleep(250);
        Check(pad_fires_.load() == fired + 1, "with the PlayStation set in use the Xbox chord does not fire");
        DebugHub::ShowPage(DebugHub::Page::Hotkeys); Sleep(900);
        PostMessageW(glyphs, WM_LBUTTONDOWN, MK_LBUTTON, MAKELPARAM(6, g.bottom / 2)); Sleep(300);
        Check(Saved(L"PadGlyphs") == L"auto" && UiHotkeys::ActivePad() == Device::Xbox, "back to auto: the Xbox pad in hand picks the Xbox set");
        // Steam Input / DS4Windows: a DualSense press also shows up as XInput, a moment earlier.
        BindXbox(down, 0x8000 | 0x4000, 300);
        Sleep(100);
        Log("  mirrored DualSense: GearDown.PS saved as %ls, hint %ls", Saved(L"GearDown.PS").c_str(), UiHotkeys::Hint(UiHotkeys::Action::GearDown).c_str());
        Check(Saved(L"GearDown.PS") == L"ps:Triangle+Square", "a DualSense mirrored into XInput records into the PlayStation set");
        Check(UiHotkeys::ActivePad() == Device::PlayStation && UiHotkeys::Hint(UiHotkeys::Action::GearDown) == L"三角 + 方块",
            "a DualSense press switches to the PlayStation set; prompts name its buttons");
    } else Check(false, "hotkey page has the per-device gear bind boxes and the pad type switch");
    // Pad navigation with the fake XInput pad: View + Menu opens and closes the hub, RB / LB change the page, the
    // D-pad moves the focus, A confirms, B steps back, and no other pad chord fires meanwhile.
    if (DebugHub::Visible()) { DebugHub::Toggle(); Sleep(900); }
    if (!OursForeground()) { Foreground(game_); Sleep(150); }
    if (!OursForeground()) Log("  skipped pad navigation checks: another process owns the foreground");
    else {
        Check(Saved(L"Layout") == L"3" && Saved(L"ToggleHub.Xbox") == L"xb:View+Menu" && Saved(L"ToggleHub.PS") == L"ps:Create+Options",
            "the panel's pad chords are written with their defaults (Layout=3)");
        const int fired = pad_fires_.load();
        fake_pad_.store(0x0020); Sleep(60); fake_pad_.store(0x0030); Sleep(200); fake_pad_.store(0); Sleep(800);
        Report("hub opened by View + Menu");
        Check(DebugHub::Visible(), "View then Menu opens the hub");
        Check(ClassOf(Focused(), L"PBZDebugHubNav_v1"), "the sidebar has the focus when the hub opens");
        Log("  pad in use: %d (1 = Xbox)", static_cast<int>(UiHotkeys::ActivePad()));
        DebugHub::ShowPage(DebugHub::Page::Hotkeys); Sleep(500);
        Tap(0x0200); Sleep(400);
        Check(Visible(L"PBZDonatePage_v1") != nullptr, "RB goes to the next page (Donate after the hotkeys)");
        Tap(0x0100); Sleep(400);
        Check(Visible(L"PBZHotkeyBind_v1") != nullptr, "LB goes back a page");
        Tap(0x0100 | 0x0200); Sleep(300);
        Check(UiHotkeys::ActivePad() == Device::Xbox && pad_fires_.load() == fired, "LB + RB (GearUp on the Xbox set) does not fire while the hub is up");
        DebugHub::ShowPage(DebugHub::Page::MotianGrab); Sleep(700);
        HWND grab = nullptr; EnumChildWindows(game_, FindGrabPage, reinterpret_cast<LPARAM>(&grab));
        Tap(0x0008); Sleep(200);
        const HWND first = Focused();
        Log("  Right from the sidebar: focus %s", Describe(first).c_str());
        Check(grab && first && IsChild(grab, first), "D-pad right from the sidebar moves the focus into the page");
        const HWND box = Visible(L"Button", BS_TYPEMASK, BS_AUTOCHECKBOX);
        int steps = 0;
        while (box && Focused() != box && steps < 40) { Tap(0x0002, 60); ++steps; }
        Log("  D-pad down to the first checkbox: %d steps, focus %s", steps, Describe(Focused()).c_str());
        Check(box && Focused() == box, "D-pad down walks the focus to the checkbox");
        if (box && Focused() == box) {
            const LRESULT before = SendMessageW(box, BM_GETCHECK, 0, 0);
            Tap(0x1000); Sleep(200);
            const LRESULT after = SendMessageW(box, BM_GETCHECK, 0, 0);
            Check(after != before, "A toggles the focused checkbox");
            if (after != before) { Tap(0x1000); Sleep(200); }
            Tap(0x0002, 60); Sleep(100);
            const HWND next = Focused();
            Tap(0x0001, 60); Sleep(100);
            Check(next != box && grab && IsChild(grab, next) && Focused() == box, "D-pad down then up: the next control and back");
        }
        steps = 0;
        while (!ClassOf(Focused(), L"ComboBox") && steps < 40) { Tap(0x0002, 60); ++steps; }
        Log("  D-pad down to a drop-down list: %d steps, focus %s", steps, Describe(Focused()).c_str());
        if (const HWND combo = ClassOf(Focused(), L"ComboBox") ? Focused() : nullptr) {
            Tap(0x1000); Sleep(300);
            const HWND popup = FindWindowExW(game_, nullptr, L"PBZDropdownList_v1", nullptr);
            const HWND top = FindWindowW(L"PBZDropdownList_v1", nullptr);
            Check((popup && IsWindowVisible(popup)) || (top && IsWindowVisible(top)), "A opens the focused drop-down list");
            Tap(0x2000); Sleep(300);
            Check(!(popup && IsWindowVisible(popup)) && !(top && IsWindowVisible(top)) && DebugHub::Visible() && Focused() == combo,
                "B closes the drop-down list; the hub and the focus stay");
        } else Check(false, "D-pad down reaches a drop-down list on the grab page");
        Tap(0x2000); Sleep(200);
        Check(ClassOf(Focused(), L"PBZDebugHubNav_v1") && DebugHub::Visible(), "B returns the focus to the sidebar");
        Tap(0x2000); Sleep(900);
        Check(!DebugHub::Visible(), "B in the sidebar hides the hub");
        Check(Keys(3) == 3, "keys reach the game after the hub closed by B");
        fake_pad_.store(0x0020 | 0x0010); Sleep(250); fake_pad_.store(0); Sleep(900);
        Check(DebugHub::Visible(), "View + Menu pressed together opens the hub");
        DebugHub::ShowPage(DebugHub::Page::Hotkeys); Sleep(600);
        Tap(0x0008); Sleep(200);
        steps = 0;
        while (!ClassOf(Focused(), L"PBZHotkeyBind_v1") && steps < 10) { Tap(0x0002, 60); ++steps; }
        Log("  hotkey page: %d steps to a bind box, focus %s", steps, Describe(Focused()).c_str());
        Check(ClassOf(Focused(), L"PBZHotkeyBind_v1") && !UiHotkeys::Capturing(), "a bind box reached with the D-pad is not armed");
        Tap(0x1000); Sleep(200);
        Check(UiHotkeys::Capturing(), "A arms the focused bind box");
        Hold(VK_ESCAPE, 120); Sleep(300);
        Check(!UiHotkeys::Capturing(), "Esc cancels the capture");
        if (!DebugHub::Visible()) { DebugHub::Toggle(); Sleep(900); }
        fake_pad_.store(0x0020 | 0x0010); Sleep(250); fake_pad_.store(0); Sleep(900);
        Check(!DebugHub::Visible(), "View + Menu closes the hub");
        Check(pad_fires_.load() == fired, "no pad chord fired during pad navigation");
        Check(Keys(3) == 3, "keys reach the game after the hub closed by View + Menu");
    }
    if (DebugHub::Visible()) { DebugHub::Toggle(); Sleep(900); }
    Check(Keys(3) == 3, "keys reach the game after the mouse checks");
    if (OursForeground()) {
        Hold(VK_F23, 120); Sleep(100);
        Log("  after a key: device=%d grab hint=%ls", static_cast<int>(UiHotkeys::InputDevice()), UiHotkeys::Hint(UiHotkeys::Action::GrabRelease).c_str());
        Check(UiHotkeys::InputDevice() == Device::Keyboard && UiHotkeys::Hint(UiHotkeys::Action::GrabRelease) == L"G", "after a key press the prompts show the keyboard set");
    }
    Report("end");
    Check(game_off_by_worker_.load() == 0, "the hub's thread never deactivates the game window (UE would drop menu mouse input)");
    Log("worst F24 delivery latency: %.1f ms", worst_latency_us_.load() / 1000.0);
    Check(worst_latency_us_.load() < 120000, "key delivery stays under 120 ms");

    stop_.store(true);
    PostThreadMessageW(game_tid_, WM_NULL, 0, 0);
    WaitForSingleObject(worker, 5000); WaitForSingleObject(game, 5000);
    WritePrivateProfileStringW(L"Hotkeys", nullptr, nullptr, IniPath().c_str());
    Log("failures=%d", failures_);
    if (log_) std::fclose(log_);
    return failures_;
}
