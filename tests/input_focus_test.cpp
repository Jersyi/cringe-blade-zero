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
#include <windows.h>
#include <windowsx.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
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
std::atomic<bool> stop_{false};
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
const wchar_t* const kTitles[] = {L"Boss 连战", L"Boss AI", L"角色替换", L"NPC 重定向", L"空中技能 / JC", L"投技", L"毛发 / Groom", L"武器预设", L"快捷键"};
HWND MockPage(HWND parent) {
    HWND page = CreateWindowExW(0, L"STATIC", L"mock page", WS_POPUP, 0, 0, 640, 420, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    DebugHub::Attach(page, parent, 640, 420);
    return page;
}
DWORD WINAPI WorkerMain(void* ready) {
    wchar_t ini[MAX_PATH]{}; GetTempPathW(MAX_PATH, ini); wcscat_s(ini, L"PBZInputFocusTest.ini");
    UiI18n::Initialize(ini); UiTheme::Initialize(); DebugHub::Initialize(); UiHotkeys::Initialize(ini);
    UiLoadBanner::Initialize(&LogLine); EntryFocus::Initialize();
    SettingsStore::Initialize(ini); HubPause::Initialize(&LogLine); GrabExtras::Initialize(&LogLine); MotianGrabMenu::Initialize(&LogLine); GearWidget::Initialize(&LogLine);
    for (int page = 0; page < static_cast<int>(DebugHub::Page::Count); ++page)
        if (page != static_cast<int>(DebugHub::Page::Hotkeys)) DebugHub::Register(static_cast<DebugHub::Page>(page), kTitles[page], &MockPage);
    DebugHub::Register(DebugHub::Page::Hotkeys, L"快捷键", &UiHotkeys::CreatePage);
    DebugHub::Register(DebugHub::Page::MotianGrab, L"投技", &MotianGrabMenu::CreatePage);
    DebugHub::Register(DebugHub::Page::WeaponPresets, L"武器预设", &WeaponPresetsMenu::CreatePage);
    UiLoadBanner::ShowSuccess();
    SetEvent(static_cast<HANDLE>(ready));
    while (!stop_.load()) {
        UiHotkeys::Tick(); DebugHub::Tick(); UiLoadBanner::Tick(); EntryFocus::Poll(); MotianGrabMenu::Tick(); WeaponPresetsMenu::Tick(); GearWidget::Tick();
        Sleep(DebugHub::Visible() || DebugHub::Animating() || UiLoadBanner::Active() ? 16 : 25);
    }
    WeaponPresetsMenu::Shutdown(); DebugHub::Shutdown(); UiLoadBanner::Shutdown(); UiGlass::Shutdown(); UiI18n::Shutdown(); UiTheme::Shutdown();
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
    Log("[%s] foreground=%s game-thread focus=%s active=%s game set/kill focus=%d/%d", phase,
        Describe(GetForegroundWindow()).c_str(), Describe(info.hwndFocus).c_str(), Describe(info.hwndActive).c_str(),
        game_setfocus_.load(), game_killfocus_.load());
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
POINT Center(HWND w) { RECT r{}; GetWindowRect(w, &r); return {(r.left + r.right) / 2, (r.top + r.bottom) / 2}; }
POINT Left(HWND w) { RECT r{}; GetWindowRect(w, &r); return {r.left + 10, (r.top + r.bottom) / 2}; }
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
    HANDLE game = CreateThread(nullptr, 0, GameMain, ready, 0, nullptr);
    WaitForSingleObject(ready, 5000);
    Foreground(game_); Sleep(300);
    HANDLE worker = CreateThread(nullptr, 0, WorkerMain, ready, 0, nullptr);
    WaitForSingleObject(ready, 5000);
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
    DebugHub::Toggle(); Sleep(900);
    Check(Keys(3) == 3, "keys reach the game after the mouse checks");
    Log("worst F24 delivery latency: %.1f ms", worst_latency_us_.load() / 1000.0);
    Check(worst_latency_us_.load() < 120000, "key delivery stays under 120 ms");

    stop_.store(true);
    PostThreadMessageW(game_tid_, WM_NULL, 0, 0);
    WaitForSingleObject(worker, 5000); WaitForSingleObject(game, 5000);
    Log("failures=%d", failures_);
    if (log_) std::fclose(log_);
    return failures_;
}
