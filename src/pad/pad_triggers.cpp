#include "pad_triggers.h"
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>
#include <initializer_list>

namespace PadTriggers {
namespace {
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
using WriteFileFn = BOOL(WINAPI*)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
using DeviceIoControlFn = BOOL(WINAPI*)(HANDLE, DWORD, LPVOID, DWORD, LPVOID, DWORD, LPDWORD, LPOVERLAPPED);
using CloseHandleFn = BOOL(WINAPI*)(HANDLE);
using GetProcFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using SetOutputReportFn = BOOLEAN(__stdcall*)(HANDLE, PVOID, ULONG);
using SetTriggerFn = void (*)(void*, uint64_t, const void*);

// ScePadTriggerEffectParam (Steamworks isteamdualsense.h): triggerMask, padding[7], command[L2, R2] {mode, padding[4], data[48]}.
struct TriggerParam { uint8_t mask; uint8_t padding[7]; struct { uint32_t mode; uint8_t padding[4]; uint8_t data[48]; } command[2]; };
static_assert(sizeof(TriggerParam) == 120);

// DualSense output report: USB id 0x02 (48 bytes, effects at 1); Bluetooth id 0x31 (78 bytes, effects at 2 after the
// 0x02 byte SDL and DS5W send, or at 3 after Linux's seq/tag pair, CRC32 of 0xA2 + bytes 0..73 at 74).
constexpr DWORD kSetOutputReportIoctl = 0x000B0195;  // IOCTL_HID_SET_OUTPUT_REPORT
constexpr uint8_t kTriggerBits = 0x04 | 0x08;        // valid_flag0: right, left trigger effect
constexpr size_t kRightTrigger = 10, kLeftTrigger = 21, kTriggerBytes = 11;
constexpr uint8_t kTriggerOff = 0x05;                // Sony's "off": no resistance, trigger stop at rest
constexpr size_t kUsbSize = 48, kBtSize = 78, kBtCrc = 74;

CreateFileWFn real_create = nullptr;
WriteFileFn real_write = nullptr;
DeviceIoControlFn real_ioctl = nullptr;
CloseHandleFn real_close = nullptr;
GetProcFn real_get_proc = nullptr;
SetOutputReportFn volatile real_set_output = nullptr;
SetTriggerFn real_set_trigger = nullptr;
int steam_slot = -1, installed = 0;
SRWLOCK steam_lock = SRWLOCK_INIT;

void (*volatile log_)(const char*) = nullptr;
wchar_t ini_[MAX_PATH]{};
volatile LONG forced_off = 1, reset_pending = 1;
volatile LONG opened = 0, reports = 0, rewritten = 0, unknown = 0, steam_calls = 0, steam_hooked = 0;
constexpr int kPads = 16;
void* volatile pads[kPads]{};
volatile LONG pad_count = 0;
thread_local uint8_t scratch[1024];

void Logf(const char* format, ...) {
    const auto sink = log_;
    if (!sink) return;
    char line[512]{}; va_list args; va_start(args, format); _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args); va_end(args);
    sink(line);
}
bool CopyGuarded(void* out, const void* from, size_t size) {
    __try { std::memcpy(out, from, size); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Writable(const void* p, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protect = info.Protect & 0xff;
    return (protect == PAGE_READWRITE || protect == PAGE_WRITECOPY || protect == PAGE_EXECUTE_READWRITE) &&
        reinterpret_cast<uintptr_t>(p) + size <= reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
}
bool ExecutableImage(const void* p) {
    MEMORY_BASIC_INFORMATION info{};
    return p && VirtualQuery(p, &info, sizeof(info)) && info.State == MEM_COMMIT && info.Type == MEM_IMAGE &&
        (info.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY));
}

bool Tracked(HANDLE h) {
    if (!pad_count || !h || h == INVALID_HANDLE_VALUE) return false;
    for (void* p : pads) if (p == h) return true;
    return false;
}
bool Track(HANDLE h) {
    for (auto& p : pads)
        if (!InterlockedCompareExchangePointer(&p, h, nullptr)) { InterlockedIncrement(&pad_count); InterlockedExchange(&reset_pending, 1); return true; }
    return false;
}
void Untrack(HANDLE h) {
    for (auto& p : pads)
        if (p == h && InterlockedCompareExchangePointer(&p, nullptr, h) == h) { InterlockedDecrement(&pad_count); return; }
}
// \\?\hid#vid_054c&pid_0ce6... (USB) or \\?\hid#{00001124-...}_vid&0002054c_pid&0ce6... (Bluetooth); 0df2 = Edge.
bool DualSensePath(const wchar_t* path) {
    if (!path || _wcsnicmp(path, L"\\\\?\\hid#", 8)) return false;
    wchar_t lower[512]{};
    for (size_t i = 0; i + 1 < 512 && path[i]; ++i) lower[i] = static_cast<wchar_t>(std::towlower(path[i]));
    return std::wcsstr(lower, L"054c") && (std::wcsstr(lower, L"pid_0ce6") || std::wcsstr(lower, L"pid&0ce6") ||
        std::wcsstr(lower, L"pid_0df2") || std::wcsstr(lower, L"pid&0df2"));
}

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
    for (size_t i = 0; i < size; ++i) { crc ^= data[i]; for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return crc;
}
uint32_t BtCrc(const uint8_t* report) { const uint8_t header = 0xa2; return ~Crc32(report, kBtCrc, Crc32(&header, 1, 0xffffffffu)); }
// Offset of the effects block, 0 for anything else (other report ids, audio reports, bad CRC).
size_t EffectsOffset(const uint8_t* r, size_t size, bool& bluetooth) {
    bluetooth = false;
    if (size >= kUsbSize && r[0] == 0x02) return 1;
    if (size < kBtSize || r[0] != 0x31) return 0;
    uint32_t stored = 0; std::memcpy(&stored, r + kBtCrc, sizeof(stored));
    if (stored != BtCrc(r)) return 0;
    bluetooth = true;
    return r[1] == 0x02 ? 2 : r[2] == 0x10 ? 3 : 0;
}

// The buffer to pass on: the caller's own (edited in place when it is writable, so overlapped writes keep their
// buffer), a rewritten per-thread copy, or the original untouched.
const void* Process(const void* data, DWORD size) {
    if (!data || !size || !InterlockedCompareExchange(&forced_off, 0, 0)) return data;
    InterlockedIncrement(&reports);
    if (size > sizeof(scratch) || !CopyGuarded(scratch, data, size)) return data;
    bool bluetooth = false;
    if (!EffectsOffset(scratch, size, bluetooth)) {
        if (InterlockedIncrement(&unknown) == 1) Logf("PAD unrecognized DualSense output report id 0x%02x, %lu bytes (passed on)", scratch[0], size);
        return data;
    }
    if (!RewriteReport(scratch, size, InterlockedExchange(&reset_pending, 0) != 0)) return data;
    if (InterlockedIncrement(&rewritten) == 1) Logf("PAD first trigger effect rewritten to off (%s report, %lu bytes)", bluetooth ? "Bluetooth" : "USB", size);
    if (Writable(data, size) && CopyGuarded(const_cast<void*>(data), scratch, size)) return data;
    return scratch;
}

HANDLE WINAPI HookCreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE source) {
    const HANDLE h = real_create(path, access, share, security, disposition, flags, source);
    if (h != INVALID_HANDLE_VALUE && path && path[0] == L'\\' && DualSensePath(path)) {
        const DWORD error = GetLastError();
        const bool ok = Track(h);
        if (InterlockedIncrement(&opened) <= 8) Logf("PAD DualSense opened handle=%p access=0x%lx%s %ls", h, access, ok ? "" : " (NOT tracked: table full)", path);
        SetLastError(error);
    }
    return h;
}
BOOL WINAPI HookCloseHandle(HANDLE h) {
    if (pad_count) Untrack(h);
    return real_close(h);
}
BOOL WINAPI HookWriteFile(HANDLE h, LPCVOID data, DWORD size, LPDWORD written, LPOVERLAPPED overlapped) {
    if (Tracked(h)) data = Process(data, size);
    return real_write(h, data, size, written, overlapped);
}
BOOL WINAPI HookDeviceIoControl(HANDLE h, DWORD code, LPVOID in, DWORD in_size, LPVOID out, DWORD out_size, LPDWORD returned, LPOVERLAPPED overlapped) {
    if (code == kSetOutputReportIoctl && Tracked(h)) {
        if (in && in_size) in = const_cast<void*>(Process(in, in_size));
        else out = const_cast<void*>(Process(out, out_size));
    }
    return real_ioctl(h, code, in, in_size, out, out_size, returned, overlapped);
}
BOOLEAN __stdcall HookSetOutputReport(HANDLE h, PVOID report, ULONG size) {
    if (Tracked(h)) report = const_cast<void*>(Process(report, size));
    return real_set_output(h, report, size);
}
FARPROC WINAPI HookGetProcAddress(HMODULE module, LPCSTR name) {
    const FARPROC real = real_get_proc(module, name);
    if (real && reinterpret_cast<uintptr_t>(name) > 0xffff && !std::strcmp(name, "HidD_SetOutputReport")) {
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&real_set_output), reinterpret_cast<void*>(real), nullptr);
        if (reinterpret_cast<FARPROC>(real_set_output) == real) return reinterpret_cast<FARPROC>(&HookSetOutputReport);
    }
    return real;
}

void HookSetTrigger(void* self, uint64_t pad, const void* param) {
    if (param && InterlockedCompareExchange(&forced_off, 0, 0)) {
        TriggerParam off{}; off.mask = 0x03;  // L2 | R2, both commands SCE_PAD_TRIGGER_EFFECT_MODE_OFF
        if (InterlockedIncrement(&steam_calls) == 1) Logf("PAD first Steam Input trigger effect replaced by off");
        real_set_trigger(self, pad, &off);
        return;
    }
    real_set_trigger(self, pad, param);
}

// The executable's delay-import entry for (dll, function): the module handle the delay loader stored (set only after
// LoadLibrary returned, so the DLL is initialized) and the entry's current value (the function once resolved).
bool DelayImport(HMODULE exe, const char* dll, const char* function, HMODULE& module, void*& entry) {
    auto* base = reinterpret_cast<uint8_t*>(exe);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress); d->DllNameRVA; ++d) {
        if (!d->Attributes.RvaBased || _stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), dll)) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportNameTableRVA);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            if (std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, function)) continue;
            module = *reinterpret_cast<HMODULE volatile*>(base + d->ModuleHandleRVA);
            entry = reinterpret_cast<void*>(*reinterpret_cast<volatile ULONG_PTR*>(&slots->u1.Function));
            return true;
        }
    }
    return false;
}
bool InModule(HMODULE module, const void* p) {
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + reinterpret_cast<const IMAGE_DOS_HEADER*>(base)->e_lfanew);
    return p >= base && p < base + nt->OptionalHeader.SizeOfImage;
}
// PBZ.exe delay-loads steam_api64.dll, so its Steam Input object cannot be caught at load. Once the game's frame loop
// calls SteamAPI_RunCallbacks (Steam is up), ask steam_api64's own accessor for the interface: there is one
// implementation, so hooking its vtable slot covers the game's calls too.
DWORD WINAPI SteamWatcher(void*) {
    const HMODULE exe = GetModuleHandleW(nullptr);
    HMODULE steam = nullptr; void* callbacks = nullptr;
    const bool delayed = DelayImport(exe, "steam_api64.dll", "SteamAPI_RunCallbacks", steam, callbacks);
    // A static import is already initialized here: this thread starts after the loader finished.
    if (!delayed && !GetModuleHandleW(L"steam_api64.dll")) { Logf("PAD no steam_api64.dll in this process; Steam Input path unused"); return 0; }
    for (const ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000; GetTickCount64() < deadline; Sleep(500)) {
        if (delayed && !(DelayImport(exe, "steam_api64.dll", "SteamAPI_RunCallbacks", steam, callbacks) && steam && InModule(steam, callbacks))) continue;
        if (!delayed) steam = GetModuleHandleW(L"steam_api64.dll");
        if (steam_slot < 0) {
            const auto* code = reinterpret_cast<const uint8_t*>(GetProcAddress(steam, "SteamAPI_ISteamInput_SetDualSenseTriggerEffect"));
            uint8_t bytes[12]{};
            if (code && CopyGuarded(bytes, code, sizeof(bytes))) steam_slot = SteamSlotFromExport(bytes);
            if (steam_slot < 0) { Logf("PAD steam_api64 SetDualSenseTriggerEffect thunk not recognized; Steam Input path not hooked"); return 0; }
        }
        using Accessor = void* (*)();
        Accessor accessor = nullptr;
        for (const char* name : {"SteamAPI_SteamInput_v007", "SteamAPI_SteamInput_v008", "SteamAPI_SteamInput_v006", "SteamAPI_SteamInput_v009"})
            if ((accessor = reinterpret_cast<Accessor>(GetProcAddress(steam, name))) != nullptr) break;
        if (!accessor) { Logf("PAD steam_api64 has no SteamInput accessor; Steam Input path not hooked"); return 0; }
        if (void* object = accessor()) { HookSteamInput(object, steam_slot); return 0; }
    }
    Logf("PAD Steam not running after 10 minutes; Steam Input path not hooked");
    return 0;
}

// Replaces (dll, function) entries in one module's import address table; the first entry's old value is the original.
int PatchImports(HMODULE module, const char* dll, const char* function, void* hook, void** original) {
    auto* base = reinterpret_cast<uint8_t*>(module);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    int patched = 0;
    for (auto* d = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), dll)) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), function)) continue;
            if (slots->u1.Function == reinterpret_cast<ULONG_PTR>(hook)) continue;
            DWORD protect = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void*), PAGE_READWRITE, &protect)) continue;
            if (!*original) *original = reinterpret_cast<void*>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONG_PTR>(hook);
            VirtualProtect(&slots->u1.Function, sizeof(void*), protect, &protect);
            ++patched;
        }
    }
    return patched;
}
template<class Fn> int Patch(HMODULE exe, std::initializer_list<const char*> dlls, const char* function, Fn hook, Fn& original) {
    int n = 0;
    for (const char* dll : dlls) n += PatchImports(exe, dll, function, reinterpret_cast<void*>(hook), reinterpret_cast<void**>(&original));
    if (!original) original = reinterpret_cast<Fn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), function));
    return n;
}
}  // namespace

bool RewriteReport(uint8_t* r, size_t size, bool force_reset) {
    bool bluetooth = false;
    const size_t at = EffectsOffset(r, size, bluetooth);
    if (!at) return false;
    uint8_t* effects = r + at;
    if (!(effects[0] & kTriggerBits) && !force_reset) return false;
    effects[0] |= kTriggerBits;
    std::memset(effects + kRightTrigger, 0, 2 * kTriggerBytes);
    effects[kRightTrigger] = kTriggerOff; effects[kLeftTrigger] = kTriggerOff;
    if (bluetooth) { const uint32_t crc = BtCrc(r); std::memcpy(r + kBtCrc, &crc, sizeof(crc)); }
    return true;
}
int SteamSlotFromExport(const uint8_t* c) {
    // mov rax,[rcx] ; jmp qword ptr [rax+disp8 | disp32]
    if (!c || c[0] != 0x48 || c[1] != 0x8b || c[2] != 0x01) return -1;
    const uint8_t* j = c[3] == 0x48 ? c + 4 : c + 3;
    if (j[0] != 0xff) return -1;
    int32_t disp = -1;
    if (j[1] == 0x60) disp = static_cast<int8_t>(j[2]);
    else if (j[1] == 0xa0) std::memcpy(&disp, j + 2, sizeof(disp));
    return disp >= 0 && disp % 8 == 0 && disp / 8 < 256 ? disp / 8 : -1;
}
bool HookSteamInput(void* object, int slot) {
    if (!object || slot < 0 || slot >= 256) return false;
    AcquireSRWLockExclusive(&steam_lock);
    bool ok = false;
    void** vtable = nullptr; void* current = nullptr;
    if (CopyGuarded(&vtable, object, sizeof(vtable)) && vtable && CopyGuarded(&current, vtable + slot, sizeof(current))) {
        if (current == reinterpret_cast<void*>(&HookSetTrigger)) ok = true;
        else if (!ExecutableImage(current)) Logf("PAD Steam Input slot %d does not point at code; not hooked", slot);
        else if (real_set_trigger && reinterpret_cast<void*>(real_set_trigger) != current) Logf("PAD second Steam Input implementation %p; not hooked", current);
        else {
            DWORD protect = 0;
            if (VirtualProtect(vtable + slot, sizeof(void*), PAGE_READWRITE, &protect)) {
                real_set_trigger = reinterpret_cast<SetTriggerFn>(current);
                vtable[slot] = reinterpret_cast<void*>(&HookSetTrigger);
                VirtualProtect(vtable + slot, sizeof(void*), protect, &protect);
                InterlockedIncrement(&steam_hooked);
                Logf("PAD Steam Input SetDualSenseTriggerEffect hooked (object %p, slot %d)", object, slot);
                ok = true;
            }
        }
    }
    ReleaseSRWLockExclusive(&steam_lock);
    return ok;
}
void TrackForTest(HANDLE handle) { Track(handle); }

void Install() {
    if (installed) return;
    const HMODULE exe = GetModuleHandleW(nullptr);
    int n = 0;
    n += Patch(exe, {"kernel32.dll", "api-ms-win-core-file-l1-1-0.dll", "api-ms-win-core-file-l1-2-0.dll", "kernelbase.dll"}, "CreateFileW", &HookCreateFileW, real_create);
    n += Patch(exe, {"kernel32.dll", "api-ms-win-core-file-l1-1-0.dll", "api-ms-win-core-file-l1-2-0.dll", "kernelbase.dll"}, "WriteFile", &HookWriteFile, real_write);
    n += Patch(exe, {"kernel32.dll", "api-ms-win-core-io-l1-1-0.dll", "kernelbase.dll"}, "DeviceIoControl", &HookDeviceIoControl, real_ioctl);
    n += Patch(exe, {"kernel32.dll", "api-ms-win-core-handle-l1-1-0.dll", "kernelbase.dll"}, "CloseHandle", &HookCloseHandle, real_close);
    n += Patch(exe, {"kernel32.dll", "api-ms-win-core-libraryloader-l1-2-0.dll", "api-ms-win-core-libraryloader-l1-1-0.dll", "kernelbase.dll"},
        "GetProcAddress", &HookGetProcAddress, real_get_proc);
    installed = n ? n : -1;
}
void Attach(void (*log)(const char*), const wchar_t* ini_path) {
    // No ini (the standalone triggers-only DLL): always forced off, nothing saved.
    if (ini_path) wcsncpy_s(ini_, ini_path, _TRUNCATE);
    const bool off = !ini_[0] || GetPrivateProfileIntW(L"Controller", L"ForceAdaptiveTriggersOff", 1, ini_) != 0;
    InterlockedExchange(&forced_off, off ? 1 : 0);
    log_ = log;
    Logf("PAD adaptive triggers %s; %d import entries hooked (CreateFileW, WriteFile, DeviceIoControl, CloseHandle, GetProcAddress)",
        off ? "FORCED OFF" : "left to the game", installed);
    static volatile LONG watching = 0;
    if (!InterlockedExchange(&watching, 1))
        if (HANDLE thread = CreateThread(nullptr, 0, SteamWatcher, nullptr, 0, nullptr)) CloseHandle(thread);
}
bool ForcedOff() { return InterlockedCompareExchange(&forced_off, 0, 0) != 0; }
void SetForcedOff(bool off) {
    InterlockedExchange(&forced_off, off ? 1 : 0);
    if (off) InterlockedExchange(&reset_pending, 1);
    if (ini_[0]) WritePrivateProfileStringW(L"Controller", L"ForceAdaptiveTriggersOff", off ? L"1" : L"0", ini_);
    Logf("PAD adaptive triggers %s", off ? "FORCED OFF" : "left to the game");
}
void Status(wchar_t* out, size_t size) {
    _snwprintf_s(out, size, _TRUNCATE, L"Adaptive triggers: %s\r\nDualSense handles %ld, reports %ld, rewritten %ld; Steam Input %s, replaced %ld",
        ForcedOff() ? L"FORCED OFF" : L"game", pad_count, reports, rewritten, steam_hooked ? L"hooked" : L"not seen", steam_calls);
}
}  // namespace PadTriggers
