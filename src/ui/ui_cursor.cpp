#include "ui_cursor.h"
#include "ui_host.h"
#include "ui_hotkeys.h"
#include <tlhelp32.h>
#include <atomic>
#include <cstring>
#include <cwchar>

namespace UiCursor {
namespace {
std::atomic<bool> open_{false}, hold_{false};
LONG hooks_ready_ = 0;
int forced_ = 0;
HWND subclass_hwnd_ = nullptr;
WNDPROC old_proc_ = nullptr;

using ShowFn = int (WINAPI*)(BOOL);
using CursorFn = HCURSOR (WINAPI*)(HCURSOR);
using PosFn = BOOL (WINAPI*)(int, int);
using GetFn = BOOL (WINAPI*)(LPPOINT);
using ClipFn = BOOL (WINAPI*)(const RECT*);
using CaptureFn = HWND (WINAPI*)(HWND);
using ReleaseFn = BOOL (WINAPI*)();
ShowFn real_show_ = nullptr;
CursorFn real_cursor_ = nullptr;
PosFn real_pos_ = nullptr;
GetFn real_get_ = nullptr;
ClipFn real_clip_ = nullptr;
CaptureFn real_capture_ = nullptr;
ReleaseFn real_release_ = nullptr;

bool Captured() { return open_.load(std::memory_order_relaxed) || hold_.load(std::memory_order_relaxed); }
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
LRESULT CALLBACK GameProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT) {
        RAWINPUTHEADER header{};
        UINT bytes = sizeof(header);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_HEADER, &header, &bytes, sizeof(header)) != static_cast<UINT>(-1)) {
            if (header.dwType == RIM_TYPEHID) Snoop(reinterpret_cast<HRAWINPUT>(lparam), header.hDevice);
            else if (header.dwType == RIM_TYPEMOUSE && Captured()) return 0;
        }
    } else if (Captured() && message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
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
void WriteSlot(void** slot, void* hook, void** saved) {
    if (!slot || *slot == hook) return;
    if (saved && !*saved) *saved = *slot;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &old)) return;
    InterlockedExchangePointer(slot, hook);
    VirtualProtect(slot, sizeof(void*), old, &old);
}
void PatchModule(HMODULE module, HMODULE self) {
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
    for (BOOL more = Module32FirstW(snap, &entry); more; more = Module32NextW(snap, &entry)) {
        if (windows_length && !_wcsnicmp(entry.szExePath, windows, windows_length)) continue;
        PatchModule(entry.hModule, self);
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
void Sync(bool open) {
    EnsureHooks();
    if (open) hold_.store(true, std::memory_order_relaxed);
    open_.store(open, std::memory_order_relaxed);
}
void AfterTick() {
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
        return;
    }
    if (!hold_.load(std::memory_order_relaxed)) return;
    while (forced_ > 0) {
        if (real_show_) real_show_(FALSE);
        else ShowCursor(FALSE);
        --forced_;
    }
    hold_.store(false, std::memory_order_relaxed);
}
}
