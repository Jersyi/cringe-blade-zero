#include "ui_cursor.h"
#include "ui_host.h"
#include "ui_hotkeys.h"
#include <tlhelp32.h>
#include <atomic>
#include <cstring>
#include <cstdio>
#include <cwchar>

namespace UiCursor {
namespace {
std::atomic<bool> open_{false}, hold_{false};
LONG hooks_ready_ = 0;
int forced_ = 0;
HWND subclass_hwnd_ = nullptr;
WNDPROC old_proc_ = nullptr;
void (*log_)(const char*) = nullptr;
std::atomic<DWORD> worker_{0};
int focus_logs_ = 0;
// Click diagnostics: the game frames' gaps (game thread) and the hub pump's slowest message (UI worker), in us.
std::atomic<long long> frame_last_us_{0}, frame_gap_max_us_{0};
long long dispatch_max_us_ = 0; UINT dispatch_max_msg_ = 0; wchar_t dispatch_max_class_[64]{};
unsigned hub_downs_ = 0, click_logs_ = 0; bool button_was_ = false; ULONGLONG follow_at_ = 0; unsigned follow_n_ = 0;
long long NowUs() {
    static LARGE_INTEGER frequency{};
    if (!frequency.QuadPart) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    return frequency.QuadPart ? now.QuadPart * 1000000 / frequency.QuadPart : 0;
}

using ShowFn = int (WINAPI*)(BOOL);
using CursorFn = HCURSOR (WINAPI*)(HCURSOR);
using PosFn = BOOL (WINAPI*)(int, int);
using GetFn = BOOL (WINAPI*)(LPPOINT);
using ClipFn = BOOL (WINAPI*)(const RECT*);
using CaptureFn = HWND (WINAPI*)(HWND);
using ReleaseFn = BOOL (WINAPI*)();
using SendFn = LRESULT (WINAPI*)(HWND, UINT, WPARAM, LPARAM);
ShowFn real_show_ = nullptr;
CursorFn real_cursor_ = nullptr;
PosFn real_pos_ = nullptr;
GetFn real_get_ = nullptr;
ClipFn real_clip_ = nullptr;
CaptureFn real_capture_ = nullptr;
ReleaseFn real_release_ = nullptr;
SendFn real_send_ = nullptr;

bool Captured() { return open_.load(std::memory_order_relaxed) || hold_.load(std::memory_order_relaxed); }

// UE's raw-input worker (WindowsApplication.UseWorkerThreadForRawInput, on in this game): while the game holds
// the mouse (mouse look) the game thread sends WM_APP + 1 to the worker's message-only "UnrealRawInputWindow",
// which registers the mouse to itself with RIDEV_NOLEGACY. Then no window of the process gets mouse messages
// (the hub could not be clicked) and mouse look reads the worker, past GameProc. While the hub is open the
// worker is told to let go with its own WM_APP + 2; when the hub closes it takes the mouse again (WM_APP + 1)
// if the game's last request was to take it. The game's requests are watched in its SendMessageW.
constexpr UINT kRawTake = WM_APP + 1, kRawDrop = WM_APP + 2;
std::atomic<HWND> raw_worker_{nullptr};
std::atomic<int> raw_wanted_{-1};      // the game's last request to the worker: 1 take, 0 drop, -1 none seen
std::atomic<bool> raw_parked_{false};  // the worker let go of the mouse for the hub (or the game's take was held back)
bool raw_direct_ = false;              // game thread: the worker did not let go, the registration was removed directly
RAWINPUTDEVICE raw_saved_{};
int raw_logs_ = 0;
bool RawWorkerWindow(HWND window) {
    if (!window || !IsWindow(window)) return false;
    wchar_t name[64]{};
    return GetClassNameW(window, name, 64) && !wcscmp(name, L"UnrealRawInputWindow");
}
// The process's raw mouse registration (usage page 1, usage 2), if any.
bool RawMouse(RAWINPUTDEVICE* out) {
    RAWINPUTDEVICE devices[32]{};
    UINT count = 32;
    const UINT got = GetRegisteredRawInputDevices(devices, &count, sizeof(RAWINPUTDEVICE));
    if (got == static_cast<UINT>(-1)) return false;
    for (UINT i = 0; i < got && i < 32; ++i)
        if (devices[i].usUsagePage == 1 && devices[i].usUsage == 2) { if (out) *out = devices[i]; return true; }
    return false;
}
bool NoLegacy(const RAWINPUTDEVICE& device) { return (device.dwFlags & RIDEV_NOLEGACY) == RIDEV_NOLEGACY; }
void SendWorker(HWND window, UINT message) {
    DWORD_PTR result = 0;
    SendMessageTimeoutW(window, message, 0, 0, SMTO_NORMAL | SMTO_ABORTIFHUNG, 250, &result);
}
void RawLog(const char* text) { if (log_ && raw_logs_ < 24) { ++raw_logs_; log_(text); } }
// Game thread, every frame while the hub is open.
void ParkRawMouse() {
    RAWINPUTDEVICE mouse{};
    if (!RawMouse(&mouse) || !NoLegacy(mouse)) return;
    char line[260]{};
    wchar_t name[64]{}; if (mouse.hwndTarget) GetClassNameW(mouse.hwndTarget, name, 64);
    if (RawWorkerWindow(mouse.hwndTarget)) {
        raw_worker_.store(mouse.hwndTarget, std::memory_order_relaxed);
        raw_wanted_.store(1, std::memory_order_relaxed);  // the worker registers only on the game's take
        SendWorker(mouse.hwndTarget, kRawDrop);
        RAWINPUTDEVICE after{};
        if (!(RawMouse(&after) && NoLegacy(after))) {
            raw_parked_.store(true, std::memory_order_relaxed);
            _snprintf_s(line, sizeof(line), _TRUNCATE,
                "RAW_INPUT hub open: UE raw-input worker held the mouse (flags=0x%lx) -> told to let go; mouse messages back, mouse look stopped", mouse.dwFlags);
            RawLog(line);
            return;
        }
    }
    // Not the worker, or it did not let go: remove the mouse registration itself and put it back on close.
    RAWINPUTDEVICE remove{};
    remove.usUsagePage = 1; remove.usUsage = 2; remove.dwFlags = RIDEV_REMOVE; remove.hwndTarget = nullptr;
    const BOOL ok = RegisterRawInputDevices(&remove, 1, sizeof(remove));
    const DWORD error = ok ? 0 : GetLastError();
    if (ok && !raw_direct_) { raw_saved_ = mouse; raw_direct_ = true; }
    _snprintf_s(line, sizeof(line), _TRUNCATE, "RAW_INPUT hub open: mouse registered RIDEV_NOLEGACY (flags=0x%lx target=%ls) -> registration removed ok=%d err=%lu",
        mouse.dwFlags, name, ok ? 1 : 0, error);
    RawLog(line);
}
// Game thread, the first frame after the hub closed.
void UnparkRawMouse() {
    char line[200]{};
    if (raw_parked_.exchange(false, std::memory_order_relaxed)) {
        const HWND worker = raw_worker_.load(std::memory_order_relaxed);
        const int wanted = raw_wanted_.load(std::memory_order_relaxed);
        const bool take = wanted == 1 && RawWorkerWindow(worker);
        if (take) SendWorker(worker, kRawTake);
        RAWINPUTDEVICE mouse{};
        const bool held = RawMouse(&mouse) && NoLegacy(mouse);
        _snprintf_s(line, sizeof(line), _TRUNCATE, "RAW_INPUT hub closed: worker given the mouse back=%d (game's last request=%d) held=%d", take ? 1 : 0, wanted,
            held ? 1 : 0);
        RawLog(line);
    }
    if (raw_direct_) {
        raw_direct_ = false;
        BOOL ok = TRUE;
        if (!RawMouse(nullptr) && (!raw_saved_.hwndTarget || IsWindow(raw_saved_.hwndTarget))) ok = RegisterRawInputDevices(&raw_saved_, 1, sizeof(raw_saved_));
        _snprintf_s(line, sizeof(line), _TRUNCATE, "RAW_INPUT hub closed: mouse registration restored flags=0x%lx ok=%d", raw_saved_.dwFlags, ok ? 1 : 0);
        RawLog(line);
    }
}
bool ContainsI(const wchar_t* text, const wchar_t* needle) {
    if (!text || !needle) return false;
    const size_t span = wcslen(needle);
    for (const wchar_t* at = text; *at; ++at)
        if (!_wcsnicmp(at, needle, span)) return true;
    return false;
}
int SonyKind(const wchar_t* name) {
    if (!ContainsI(name, L"054C")) return 0;
    if (ContainsI(name, L"PID_0CE6") || ContainsI(name, L"PID&0CE6") || ContainsI(name, L"PID_0DF2") || ContainsI(name, L"PID&0DF2")) return 1;
    if (ContainsI(name, L"PID_05C4") || ContainsI(name, L"PID&05C4") || ContainsI(name, L"PID_09CC") || ContainsI(name, L"PID&09CC") ||
        ContainsI(name, L"PID_0BA0") || ContainsI(name, L"PID&0BA0")) return 2;
    return 0;
}
struct Known { HANDLE device = nullptr; int kind = 0; };
Known known_[12]{};
int KindOf(HANDLE device) {
    if (!device) return 0;
    for (const Known& item : known_) if (item.device == device) return item.kind;
    wchar_t name[300]{};
    UINT bytes = sizeof(name);
    int kind = -1;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name, &bytes) != static_cast<UINT>(-1)) kind = SonyKind(name);
    for (Known& item : known_) if (!item.device) { item.device = device; item.kind = kind; break; }
    return kind;
}
void Snoop(HRAWINPUT input, HANDLE device) {
    const int kind = KindOf(device);
    if (kind <= 0) return;
    UINT size = 0;
    if (GetRawInputData(input, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) != 0 || !size || size > 2048) return;
    unsigned char buffer[2048]{};
    if (GetRawInputData(input, RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER)) != size) return;
    const auto* raw = reinterpret_cast<const RAWINPUT*>(buffer);
    const DWORD count = raw->data.hid.dwCount;
    const DWORD each = raw->data.hid.dwSizeHid;
    if (!each || count == 0 || count > 16 || each * count > 1800) return;
    const auto* data = raw->data.hid.bRawData;
    for (DWORD i = 0; i < count; ++i) UiHotkeys::NoteSonyReport(kind, data + static_cast<size_t>(i) * each, static_cast<int>(each));
}
// Game thread. Activation changes are rare; a change made by the UI worker is a bug
// (UiHost::GuardActivation), so each one is logged with the thread that caused it.
void NoteActivation(WPARAM active, LPARAM thread) {
    if (!log_ || focus_logs_ >= 40) return;
    ++focus_logs_;
    const DWORD other = static_cast<DWORD>(thread), worker = worker_.load(std::memory_order_relaxed);
    char line[200]{};
    _snprintf_s(line, sizeof(line), _TRUNCATE, "UI_FOCUS game %s by tid=%lu%s", active ? "activated" : "deactivated", other,
        other && other == worker ? " (hub worker)" : "");
    log_(line);
}
bool ButtonUp(UINT message) {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP || message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}
LRESULT CALLBACK GameProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_ACTIVATEAPP) NoteActivation(wparam, lparam);
    if (message == WM_INPUT) {
        RAWINPUTHEADER header{};
        UINT bytes = sizeof(header);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_HEADER, &header, &bytes, sizeof(header)) != static_cast<UINT>(-1)) {
            if (header.dwType == RIM_TYPEHID) Snoop(reinterpret_cast<HRAWINPUT>(lparam), header.hDevice);
            else if (header.dwType == RIM_TYPEMOUSE && Captured()) return DefWindowProcW(hwnd, message, wparam, lparam);
        }
    } else if (Captured() && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST && !ButtonUp(message)) {
        // Releases still reach the game: a button held when the hub opened must not stay down in Unreal.
        return 0;
    }
    if (!old_proc_ || old_proc_ == GameProc) return DefWindowProcW(hwnd, message, wparam, lparam);
    return CallWindowProcW(old_proc_, hwnd, message, wparam, lparam);
}
int WINAPI ShowHook(BOOL show) {
    if (Captured() && !show) return -1;
    return real_show_ ? real_show_(show) : FALSE;
}
HCURSOR WINAPI CursorHook(HCURSOR cursor) {
    if (Captured() && !cursor) cursor = LoadCursorW(nullptr, IDC_ARROW);
    return real_cursor_ ? real_cursor_(cursor) : nullptr;
}
BOOL WINAPI PosHook(int x, int y) {
    if (Captured()) return TRUE;
    return real_pos_ ? real_pos_(x, y) : FALSE;
}
BOOL WINAPI GetHook(LPPOINT point) {
    if (!point) return FALSE;
    if (Captured()) {
        const RECT screen = UiHost::ClientScreen();
        if (screen.right > screen.left && screen.bottom > screen.top) {
            point->x = (screen.left + screen.right) / 2;
            point->y = (screen.top + screen.bottom) / 2;
            return TRUE;
        }
    }
    return real_get_ ? real_get_(point) : FALSE;
}
BOOL WINAPI ClipHook(const RECT* rect) {
    if (Captured()) {
        const RECT screen = UiHost::ClientScreen();
        if (screen.right > screen.left && real_clip_) return real_clip_(&screen);
        return real_clip_ ? real_clip_(nullptr) : FALSE;
    }
    return real_clip_ ? real_clip_(rect) : FALSE;
}
// The hub's windows share the game thread's input queue, so the game's mouse
// capture (Slate viewport / mouse look) would take the mouse from an open
// drop-down list, a slider or scroll-bar drag. While the hub is open the game
// neither takes capture nor releases one held by another thread.
bool ForeignCapture(HWND current) { return current && GetWindowThreadProcessId(current, nullptr) != GetCurrentThreadId(); }
HWND WINAPI CaptureHook(HWND window) {
    if (Captured()) return GetCapture();
    return real_capture_ ? real_capture_(window) : nullptr;
}
BOOL WINAPI ReleaseHook() {
    if (Captured() && ForeignCapture(GetCapture())) return TRUE;
    return real_release_ ? real_release_() : FALSE;
}
// The game's requests to its raw-input worker. A take while the hub is open is only remembered and owed for
// the close (the worker returns 0 for it and the game does not wait on it); a drop always goes through: the
// game waits on the worker's event after it.
LRESULT WINAPI SendHook(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if ((message == kRawTake || message == kRawDrop) && RawWorkerWindow(window)) {
        raw_worker_.store(window, std::memory_order_relaxed);
        raw_wanted_.store(message == kRawTake ? 1 : 0, std::memory_order_relaxed);
        if (message == kRawTake && open_.load(std::memory_order_relaxed)) {
            raw_parked_.store(true, std::memory_order_relaxed);
            return 0;
        }
    }
    return real_send_ ? real_send_(window, message, wparam, lparam) : 0;
}
void WriteSlot(void** slot, void* hook, void** saved) {
    if (!slot || *slot == hook) return;
    if (saved && !*saved) *saved = *slot;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
}
void PatchModule(HMODULE module, HMODULE self, bool game) {
    if (!module || module == self) return;
    const auto* base = reinterpret_cast<const unsigned char*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return;
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress || !dir.Size) return;
    auto* desc = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(const_cast<unsigned char*>(base + dir.VirtualAddress));
    for (; desc->Name; ++desc) {
        const char* dll = reinterpret_cast<const char*>(base + desc->Name);
        if (_stricmp(dll, "USER32.dll")) continue;
        if (!desc->OriginalFirstThunk || !desc->FirstThunk) continue;
        auto* name = reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<unsigned char*>(base + desc->OriginalFirstThunk));
        auto* slot = reinterpret_cast<IMAGE_THUNK_DATA*>(const_cast<unsigned char*>(base + desc->FirstThunk));
        for (; name->u1.AddressOfData; ++name, ++slot) {
            if (IMAGE_SNAP_BY_ORDINAL(name->u1.Ordinal)) continue;
            const auto* imported = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + name->u1.AddressOfData);
            void** target = reinterpret_cast<void**>(&slot->u1.Function);
            if (!std::strcmp(imported->Name, "ShowCursor")) WriteSlot(target, reinterpret_cast<void*>(&ShowHook), reinterpret_cast<void**>(&real_show_));
            else if (!std::strcmp(imported->Name, "SetCursor")) WriteSlot(target, reinterpret_cast<void*>(&CursorHook), reinterpret_cast<void**>(&real_cursor_));
            else if (!std::strcmp(imported->Name, "SetCursorPos")) WriteSlot(target, reinterpret_cast<void*>(&PosHook), reinterpret_cast<void**>(&real_pos_));
            else if (!std::strcmp(imported->Name, "GetCursorPos")) WriteSlot(target, reinterpret_cast<void*>(&GetHook), reinterpret_cast<void**>(&real_get_));
            else if (!std::strcmp(imported->Name, "ClipCursor")) WriteSlot(target, reinterpret_cast<void*>(&ClipHook), reinterpret_cast<void**>(&real_clip_));
            else if (!std::strcmp(imported->Name, "SetCapture")) WriteSlot(target, reinterpret_cast<void*>(&CaptureHook), reinterpret_cast<void**>(&real_capture_));
            else if (!std::strcmp(imported->Name, "ReleaseCapture")) WriteSlot(target, reinterpret_cast<void*>(&ReleaseHook), reinterpret_cast<void**>(&real_release_));
            else if (game && !std::strcmp(imported->Name, "SendMessageW")) WriteSlot(target, reinterpret_cast<void*>(&SendHook), reinterpret_cast<void**>(&real_send_));
        }
    }
}
void EnsureHooks() {
    if (InterlockedCompareExchange(&hooks_ready_, 1, 0) != 0) return;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(&EnsureHooks), &self);
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snap == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry{sizeof(entry)};
    // Game and third-party modules only. System DLLs (comctl32, uxtheme, ...) run
    // our own controls and must see the real cursor functions.
    wchar_t windows[MAX_PATH]{}; const UINT windows_length = GetWindowsDirectoryW(windows, MAX_PATH);
    const HMODULE exe = GetModuleHandleW(nullptr);
    for (BOOL more = Module32FirstW(snap, &entry); more; more = Module32NextW(snap, &entry)) {
        if (windows_length && !_wcsnicmp(entry.szExePath, windows, windows_length)) continue;
        PatchModule(entry.hModule, self, entry.hModule == exe);  // SendMessageW only in the game's exe (its raw-input worker)
    }
    CloseHandle(snap);
}
void SubclassGame() {
    const HWND game = UiHost::Game();
    if (!game || game == subclass_hwnd_) return;
    const auto current = reinterpret_cast<WNDPROC>(GetWindowLongPtrW(game, GWLP_WNDPROC));
    if (current == GameProc) { subclass_hwnd_ = game; return; }
    old_proc_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(game, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&GameProc)));
    subclass_hwnd_ = game;
}
}
void SetLogger(void (*log)(const char*)) { log_ = log; }
void Sync(bool open) {
    worker_.store(GetCurrentThreadId(), std::memory_order_relaxed);
    EnsureHooks();
    if (open) hold_.store(true, std::memory_order_relaxed);
    open_.store(open, std::memory_order_relaxed);
}
void AfterTick() {
    {
        const long long now = NowUs(), last = frame_last_us_.exchange(now, std::memory_order_relaxed);
        if (last && now > last && now - last > frame_gap_max_us_.load(std::memory_order_relaxed)) frame_gap_max_us_.store(now - last, std::memory_order_relaxed);
    }
    EnsureHooks();
    SubclassGame();
    if (open_.load(std::memory_order_relaxed)) {
        CURSORINFO info{sizeof(info)};
        if (forced_ < 8 && (!GetCursorInfo(&info) || !(info.flags & CURSOR_SHOWING))) {
            if (real_show_) real_show_(TRUE);
            else ShowCursor(TRUE);
            ++forced_;
        }
        if (real_cursor_) real_cursor_(LoadCursorW(nullptr, IDC_ARROW));
        else SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        // Only the game's own mouse capture (mouse look). Hub windows share this
        // thread's input queue: releasing their capture closed drop-downs at once
        // and swallowed checkbox / button clicks and scroll-bar drags.
        if (const HWND capture = GetCapture(); capture && !ForeignCapture(capture)) ReleaseCapture();
        const RECT screen = UiHost::ClientScreen();
        if (screen.right > screen.left) {
            if (real_clip_) real_clip_(&screen);
            else ClipCursor(&screen);
        }
        ParkRawMouse();
        return;
    }
    if (!hold_.load(std::memory_order_relaxed)) return;
    UnparkRawMouse();
    while (forced_ > 0) {
        if (real_show_) real_show_(FALSE);
        else ShowCursor(FALSE);
        --forced_;
    }
    hold_.store(false, std::memory_order_relaxed);
}
void NoteDispatch(HWND hub, const MSG& message, double ms) {
    const long long us = static_cast<long long>(ms * 1000.0);
    if (us > dispatch_max_us_) {
        dispatch_max_us_ = us; dispatch_max_msg_ = message.message; dispatch_max_class_[0] = 0;
        if (message.hwnd) GetClassNameW(message.hwnd, dispatch_max_class_, 64);
    }
    if (hub && (message.message == WM_LBUTTONDOWN || message.message == WM_NCLBUTTONDOWN || message.message == WM_LBUTTONDBLCLK) &&
        (message.hwnd == hub || IsChild(hub, message.hwnd))) ++hub_downs_;
}
void ProbeClicks(HWND hub) {
    if (!log_ || !hub) return;
    const ULONGLONG now = GetTickCount64();
    char line[900]{};
    if (follow_at_ && now >= follow_at_) {
        // What became of the press 400 ms on: did the hub's pump get the button-down, and was anything slow.
        const long long gap = frame_gap_max_us_.exchange(0, std::memory_order_relaxed), age = NowUs() - frame_last_us_.load(std::memory_order_relaxed);
        _snprintf_s(line, sizeof(line), _TRUNCATE, "HUB_CLICK n=%u after 400ms: hubButtonDowns=%u pumpSlowest=%.1fms (msg 0x%x %ls) gameFrameGapMax=%.1fms gameFrameAge=%.1fms",
            follow_n_, hub_downs_, dispatch_max_us_ / 1000.0, dispatch_max_msg_, dispatch_max_class_, gap / 1000.0, age / 1000.0);
        log_(line); follow_at_ = 0;
    }
    const bool down = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    const bool pressed = down && !button_was_;
    button_was_ = down;
    if (!pressed || click_logs_ >= 40) return;
    ++click_logs_;
    POINT at{}; GetCursorPos(&at);
    const HWND under = WindowFromPoint(at);
    wchar_t under_class[64]{}; if (under) GetClassNameW(under, under_class, 64);
    const DWORD under_tid = under ? GetWindowThreadProcessId(under, nullptr) : 0;
    const HWND game = UiHost::Game();
    const DWORD game_tid = game ? GetWindowThreadProcessId(game, nullptr) : 0;
    const char* whose = !under ? "none" : (under == hub || IsChild(hub, under)) ? "hub" : under == game ? "game" :
                        under_tid == GetCurrentThreadId() ? "worker-other" : under_tid == game_tid ? "game-other" : "foreign";
    RECT box{}; GetWindowRect(hub, &box);
    RECT clip{}; GetClipCursor(&clip);
    CURSORINFO cursor{sizeof(cursor)}; GetCursorInfo(&cursor);
    GUITHREADINFO gui{sizeof(gui)}; if (game_tid) GetGUIThreadInfo(game_tid, &gui);
    wchar_t capture_class[64]{}; if (gui.hwndCapture) GetClassNameW(gui.hwndCapture, capture_class, 64);
    const HWND parent = GetParent(hub);
    RAWINPUTDEVICE raw{}; const bool raw_on = RawMouse(&raw);
    wchar_t raw_class[64]{}; if (raw_on && raw.hwndTarget) GetClassNameW(raw.hwndTarget, raw_class, 64);
    _snprintf_s(line, sizeof(line), _TRUNCATE,
        "HUB_CLICK n=%u at=%ld,%ld under=%ls (%s tid=%lu) hub=%ld,%ld-%ld,%ld enabled=%d visible=%d ex=0x%llx childOfGame=%d clip=%ld,%ld-%ld,%ld cursorShown=%d "
        "gameCapture=%p(%ls) gameActive=%p gameFocus=%p gameFlags=0x%lx workerCapture=%p gate=%d hubButtonDownsBefore=%u rawMouse=%d flags=0x%lx(%ls)",
        click_logs_, at.x, at.y, under_class, whose, under_tid, box.left, box.top, box.right, box.bottom, IsWindowEnabled(hub) ? 1 : 0, IsWindowVisible(hub) ? 1 : 0,
        static_cast<unsigned long long>(GetWindowLongPtrW(hub, GWL_EXSTYLE)), parent && parent == game ? 1 : 0, clip.left, clip.top, clip.right, clip.bottom,
        (cursor.flags & CURSOR_SHOWING) ? 1 : 0, static_cast<void*>(gui.hwndCapture), capture_class, static_cast<void*>(gui.hwndActive), static_cast<void*>(gui.hwndFocus),
        gui.flags, static_cast<void*>(GetCapture()), Captured() ? 1 : 0, hub_downs_, raw_on ? 1 : 0, raw_on ? raw.dwFlags : 0ul, raw_class);
    log_(line);
    hub_downs_ = 0; dispatch_max_us_ = 0; dispatch_max_msg_ = 0; dispatch_max_class_[0] = 0; frame_gap_max_us_.store(0, std::memory_order_relaxed);
    follow_at_ = now + 400; follow_n_ = click_logs_;
}
}
