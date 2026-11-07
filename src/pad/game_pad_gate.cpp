#include "game_pad_gate.h"
#include "pad_gate.h"
#include "pad_triggers.h"
#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <cwctype>

namespace GamePadGate {
namespace {
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, void*);
using RawDataFn = UINT(WINAPI*)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
struct XState { DWORD packet; PadGate::XPad pad; };
static_assert(sizeof(XState) == 16, "XINPUT_STATE layout");

void (*volatile log_)(const char*) = nullptr;
std::atomic<bool> blocked_{false};
std::atomic<unsigned> xbox_chord_{0}, ps_chord_{0};
SRWLOCK lock_ = SRWLOCK_INIT;

void Logf(const char* format, ...) {
    const auto sink = log_;
    if (!sink) return;
    char line[512]{};
    va_list args; va_start(args, format); _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args); va_end(args);
    sink(line);
}
bool Once(volatile LONG& flag) { return !InterlockedExchange(&flag, 1); }

// ---- XInput ----------------------------------------------------------------------------------------------------
// Unreal reads through the delay-import slot; SDL through the pointers GetProcAddress gave it.
enum Source { kDelay, kByName, kEx, kSources };
const char* const kSourceName[kSources] = {"Unreal XInputGetState (delay import)", "SDL XInputGetState", "SDL XInputGetStateEx"};
XInputGetStateFn volatile real_[kSources]{};
struct Track { DWORD real_packet = 0, offset = 0; PadGate::XPad shown{}; };
struct User { PadGate::Gate gate; Track track[kSources]; };
User users_[4];
volatile LONG said_call_[kSources]{}, said_user_[4]{}, said_press_ = 0, said_hidden_x_ = 0, said_resolve_ = 0, said_slot_ = 0;
ULONG_PTR* delay_slot_ = nullptr;

DWORD Gate(Source source, DWORD user, void* state) {
    const XInputGetStateFn real = real_[source];
    if (!real) return ERROR_DEVICE_NOT_CONNECTED;
    const DWORD result = real(user, state);
    if (Once(said_call_[source])) Logf("GAME_PAD %s: first call, user %lu -> %s", kSourceName[source], user,
        result == ERROR_SUCCESS ? "connected" : result == ERROR_DEVICE_NOT_CONNECTED ? "not connected" : "error");
    if (result != ERROR_SUCCESS || !state || user >= 4) return result;
    auto* s = static_cast<XState*>(state);
    if (Once(said_user_[user])) Logf("GAME_PAD the game sees XInput user %lu connected (%s)", user, kSourceName[source]);
    const unsigned held = PadGate::FromXInput(s->pad.buttons, s->pad.left, s->pad.right);
    if (held && Once(said_press_)) Logf("GAME_PAD first XInput press the game read: buttons 0x%04x, user %lu (%s)", s->pad.buttons, user, kSourceName[source]);
    const bool blocked = blocked_.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&lock_);
    User& u = users_[user];
    PadGate::XPad pad = s->pad;
    PadGate::FilterXInput(u.gate, pad, blocked, xbox_chord_.load(std::memory_order_relaxed), GetTickCount64());
    // SDL skips a state whose packet number it has seen: what this gate shows changing on its own (a replayed tap
    // ending, the panel going away) gets a packet number of its own.
    Track& t = u.track[source];
    if (s->packet != t.real_packet) t.real_packet = s->packet;
    else if (std::memcmp(&pad, &t.shown, sizeof(pad))) ++t.offset;
    t.shown = pad;
    s->pad = pad;
    s->packet += t.offset;
    ReleaseSRWLockExclusive(&lock_);
    if (blocked && held && Once(said_hidden_x_)) Logf("GAME_PAD panel up: XInput pad shown to the game at rest (%s)", kSourceName[source]);
    return result;
}
DWORD WINAPI ByNameGetState(DWORD user, void* state) { return Gate(kByName, user, state); }
DWORD WINAPI ExGetState(DWORD user, void* state) { return Gate(kEx, user, state); }
DWORD WINAPI DelayGetState(DWORD user, void* state) {
    if (!real_[kDelay]) {
        HMODULE module = LoadLibraryW(L"XInput1_4.dll");
        FARPROC real = module ? GetProcAddress(module, "XInputGetState") : nullptr;
        // Where GetProcAddress itself is gated (a host that hooks this module's imports too), take the real one.
        if (real == reinterpret_cast<FARPROC>(&ByNameGetState)) real = reinterpret_cast<FARPROC>(real_[kByName]);
        InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&real_[kDelay]), reinterpret_cast<void*>(real), nullptr);
        if (Once(said_resolve_)) Logf(real ? "GAME_PAD Unreal XInputGetState resolved from XInput1_4.dll" : "GAME_PAD XInput1_4.dll missing: Unreal reads no XInput pad");
    }
    return Gate(kDelay, user, state);
}
bool XInputModule(HMODULE module) {
    wchar_t path[MAX_PATH]{};
    if (!module || !GetModuleFileNameW(module, path, MAX_PATH)) return false;
    const wchar_t* name = std::wcsrchr(path, L'\\');
    return !_wcsnicmp(name ? name + 1 : path, L"xinput", 6);
}
volatile LONG said_proc_[kSources]{};
FARPROC ProcFilter(HMODULE module, LPCSTR name, FARPROC real) {
    if (!real) return real;
    const uintptr_t id = reinterpret_cast<uintptr_t>(name);
    Source source;
    if (id == 100) source = kEx;
    else if (id > 0xffff && !std::strcmp(name, "XInputGetState")) source = kByName;
    else return real;
    if (!XInputModule(module)) return real;
    void* const before = InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(&real_[source]), reinterpret_cast<void*>(real), nullptr);
    if (before && before != reinterpret_cast<void*>(real)) {
        Logf("GAME_PAD %s asked again from another XInput DLL; that one is not gated", kSourceName[source]);
        return real;
    }
    if (Once(said_proc_[source])) {
        wchar_t path[MAX_PATH]{}; GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* file = std::wcsrchr(path, L'\\');
        Logf("GAME_PAD %s handed out through the gate (%ls)", kSourceName[source], file ? file + 1 : path);
    }
    return source == kEx ? reinterpret_cast<FARPROC>(&ExGetState) : reinterpret_cast<FARPROC>(&ByNameGetState);
}
// The executable's delay-import slot of (dll, function), imported by name or by its ordinal (PBZ.exe: by name).
ULONG_PTR* DelaySlot(HMODULE exe, const char* dll, const char* function, WORD ordinal) {
    auto* base = reinterpret_cast<uint8_t*>(exe);
    const auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
    const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (!dir.VirtualAddress) return nullptr;
    for (auto* d = reinterpret_cast<IMAGE_DELAYLOAD_DESCRIPTOR*>(base + dir.VirtualAddress); d->DllNameRVA; ++d) {
        if (!d->Attributes.RvaBased || _stricmp(reinterpret_cast<const char*>(base + d->DllNameRVA), dll)) continue;
        auto* names = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportNameTableRVA);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA*>(base + d->ImportAddressTableRVA);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) { if (IMAGE_ORDINAL(names->u1.Ordinal) == ordinal) return &slots->u1.Function; continue; }
            if (std::strcmp(reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData)->Name, function)) continue;
            return &slots->u1.Function;
        }
    }
    return nullptr;
}
bool PutSlot(ULONG_PTR* slot, ULONG_PTR value) {
    DWORD protect = 0;
    if (!slot || !VirtualProtect(slot, sizeof(*slot), PAGE_READWRITE, &protect)) return false;
    InterlockedExchangePointer(reinterpret_cast<void* volatile*>(slot), reinterpret_cast<void*>(value));
    VirtualProtect(slot, sizeof(*slot), protect, &protect);
    return true;
}

// ---- DualSense -------------------------------------------------------------------------------------------------
struct HidStream { const void* key = nullptr; PadGate::Gate gate; };
HidStream hid_reads_[8], raw_devices_[8];
volatile LONG said_hid_read_ = 0, said_hidden_hid_ = 0, said_raw_ = 0;
PadGate::Gate& StreamFor(HidStream (&streams)[8], const void* key) {
    for (HidStream& stream : streams) if (stream.key == key) return stream.gate;
    for (HidStream& stream : streams) if (!stream.key) { stream.key = key; return stream.gate; }
    static unsigned next = 0;
    HidStream& reused = streams[next++ % 8];
    reused = {}; reused.key = key;
    return reused.gate;
}
void FilterReport(HidStream (&streams)[8], const void* key, uint8_t* report, size_t size, const char* path) {
    unsigned held = 0;
    if (!PadGate::DualSenseButtons(report, size, held)) return;
    const bool blocked = blocked_.load(std::memory_order_relaxed);
    AcquireSRWLockExclusive(&lock_);
    PadGate::FilterDualSense(StreamFor(streams, key), report, size, blocked, ps_chord_.load(std::memory_order_relaxed), GetTickCount64());
    ReleaseSRWLockExclusive(&lock_);
    if (blocked && held && Once(said_hidden_hid_)) Logf("GAME_PAD panel up: DualSense shown to the game at rest (%s)", path);
}
void HidRead(HANDLE handle, uint8_t* report, DWORD size) {
    if (!report || !size) return;
    if (Once(said_hid_read_)) Logf("GAME_PAD the game reads the DualSense through HID ReadFile (report 0x%02x, %lu bytes)", report[0], size);
    FilterReport(hid_reads_, handle, report, size, "HID read");
}

// Raw input: which devices are DualSense, by name.
struct RawKind { HANDLE device = nullptr; int kind = 0; };  // 1 DualSense, -1 other
RawKind raw_kinds_[16];
int RawKindOf(HANDLE device) {
    AcquireSRWLockShared(&lock_);
    int known = 0;
    for (const RawKind& item : raw_kinds_) if (item.device == device) { known = item.kind; break; }
    ReleaseSRWLockShared(&lock_);
    if (known) return known;
    wchar_t name[300]{};
    UINT chars = static_cast<UINT>(sizeof(name) / sizeof(name[0]));
    int kind = -1;
    if (GetRawInputDeviceInfoW(device, RIDI_DEVICENAME, name, &chars) != static_cast<UINT>(-1)) {
        for (wchar_t& c : name) c = static_cast<wchar_t>(std::towlower(c));
        if (std::wcsstr(name, L"054c") && (std::wcsstr(name, L"pid_0ce6") || std::wcsstr(name, L"pid&0ce6") ||
            std::wcsstr(name, L"pid_0df2") || std::wcsstr(name, L"pid&0df2"))) kind = 1;
    }
    AcquireSRWLockExclusive(&lock_);
    bool stored = false;
    for (RawKind& item : raw_kinds_) if (item.device == device || !item.device) { item = {device, kind}; stored = true; break; }
    if (!stored) raw_kinds_[reinterpret_cast<uintptr_t>(device) % 16] = {device, kind};
    ReleaseSRWLockExclusive(&lock_);
    return kind;
}
RawDataFn real_raw_ = nullptr;
UINT WINAPI HookRawData(HRAWINPUT input, UINT command, LPVOID data, PUINT size, UINT header_size) {
    const UINT result = real_raw_ ? real_raw_(input, command, data, size, header_size) : static_cast<UINT>(-1);
    if (command != RID_INPUT || !data || result == static_cast<UINT>(-1) || result < sizeof(RAWINPUTHEADER) + 8) return result;
    auto* raw = static_cast<RAWINPUT*>(data);
    if (raw->header.dwType != RIM_TYPEHID) return result;
    const DWORD error = GetLastError();
    const DWORD each = raw->data.hid.dwSizeHid, count = raw->data.hid.dwCount;
    const size_t room = result - offsetof(RAWINPUT, data.hid.bRawData);
    if (each && count && static_cast<size_t>(each) * count <= room && RawKindOf(raw->header.hDevice) == 1) {
        if (Once(said_raw_)) Logf("GAME_PAD the game reads DualSense raw input reports (report 0x%02x, %lu bytes)", raw->data.hid.bRawData[0], each);
        for (DWORD i = 0; i < count; ++i)
            FilterReport(raw_devices_, raw->header.hDevice, raw->data.hid.bRawData + static_cast<size_t>(i) * each, each, "raw input");
    }
    SetLastError(error);
    return result;
}
}  // namespace

void Install() {
    static volatile LONG done = 0;
    if (InterlockedExchange(&done, 1)) return;
    const HMODULE exe = GetModuleHandleW(nullptr);
    delay_slot_ = DelaySlot(exe, "XINPUT1_4.dll", "XInputGetState", 2);
    if (delay_slot_) PutSlot(delay_slot_, reinterpret_cast<ULONG_PTR>(&DelayGetState));
    PadTriggers::SetProcFilter(&ProcFilter);
    PadTriggers::InstallReadHooks(&HidRead);
    PadTriggers::PatchImport(exe, "user32.dll", "GetRawInputData", reinterpret_cast<void*>(&HookRawData), reinterpret_cast<void**>(&real_raw_));
    if (!real_raw_) real_raw_ = reinterpret_cast<RawDataFn>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetRawInputData"));
}
void SetLogger(void (*log)(const char*)) {
    log_ = log;
    Logf("GAME_PAD gate: Unreal XInput slot %s, raw input %s; SDL XInput %s / %s so far",
        delay_slot_ ? "hooked" : "absent", real_raw_ ? "hooked" : "absent",
        real_[kByName] ? "gated" : "not asked", real_[kEx] ? "gated" : "not asked");
}
void Update(bool blocked, unsigned xbox_chord, unsigned ps_chord) {
    blocked_.store(blocked, std::memory_order_relaxed);
    xbox_chord_.store(xbox_chord & 0xFFFFu, std::memory_order_relaxed);
    ps_chord_.store(ps_chord & 0xFFFFu, std::memory_order_relaxed);
    if (delay_slot_ && *reinterpret_cast<volatile ULONG_PTR*>(delay_slot_) != reinterpret_cast<ULONG_PTR>(&DelayGetState)) {
        const bool put = PutSlot(delay_slot_, reinterpret_cast<ULONG_PTR>(&DelayGetState));
        if (Once(said_slot_)) Logf("GAME_PAD the delay loader rewrote the XInputGetState slot; %s", put ? "gate put back" : "could not put the gate back");
    }
}
}  // namespace GamePadGate
