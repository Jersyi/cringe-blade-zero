#include "pad_triggers.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }

uint32_t Crc(const uint8_t* data, size_t size) {
    uint32_t crc = 0xffffffffu;
    const uint8_t header = 0xa2;
    auto step = [&](uint8_t byte) { crc ^= byte; for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); };
    step(header);
    for (size_t i = 0; i < size; ++i) step(data[i]);
    return ~crc;
}
void Seal(std::vector<uint8_t>& r) { const uint32_t c = Crc(r.data(), 74); std::memcpy(r.data() + 74, &c, 4); }
bool Sealed(const std::vector<uint8_t>& r) { uint32_t c = 0; std::memcpy(&c, r.data() + 74, 4); return c == Crc(r.data(), 74); }

// Effects block as a game sets it: rumble + right "weapon" + left "feedback" trigger, lightbar.
void FillEffects(uint8_t* e) {
    e[0] = 0x01 | 0x02 | 0x04 | 0x08; e[1] = 0x04; e[2] = 0x40; e[3] = 0x80;
    const uint8_t right[11]{0x25, 0x02, 0x07, 0x08}, left[11]{0x21, 0xfe, 0x03, 0xff, 0x3f};
    std::memcpy(e + 10, right, 11); std::memcpy(e + 21, left, 11);
    e[44] = 0x11; e[45] = 0x22; e[46] = 0x33;
}
bool TriggersOff(const uint8_t* e) {
    for (size_t i = 0; i < 22; ++i) if (e[10 + i] != ((i == 0 || i == 11) ? 0x05 : 0x00)) return false;
    return (e[0] & 0x0c) == 0x0c;
}
// Everything but the trigger blocks, valid_flag0 and the Bluetooth CRC must survive.
bool SameOutsideTriggers(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, size_t at, bool bt) {
    for (size_t i = 0; i < a.size(); ++i) {
        if (i == at || (i >= at + 10 && i < at + 32) || (bt && i >= 74 && i < 78)) continue;
        if (a[i] != b[i]) return false;
    }
    return true;
}

std::vector<uint8_t> Usb() { std::vector<uint8_t> r(48); r[0] = 0x02; FillEffects(&r[1]); return r; }
std::vector<uint8_t> BtSdl() { std::vector<uint8_t> r(78); r[0] = 0x31; r[1] = 0x02; FillEffects(&r[2]); Seal(r); return r; }
std::vector<uint8_t> BtLinux() { std::vector<uint8_t> r(78); r[0] = 0x31; r[1] = 0x30; r[2] = 0x10; FillEffects(&r[3]); Seal(r); return r; }

struct Received { void* self = nullptr; uint64_t pad = 0; std::array<uint8_t, 120> param{}; int calls = 0; } received;
void FakeSetTrigger(void* self, uint64_t pad, const void* param) {
    received.self = self; received.pad = pad; std::memcpy(received.param.data(), param, 120); ++received.calls;
}
void Log(const char* line) { std::printf("  log: %s\n", line); }
}  // namespace

int main(int argc, char** argv) {
    // Report rewrites.
    {
        auto r = Usb(); const auto before = r;
        Check(PadTriggers::RewriteReport(r.data(), r.size(), false) && TriggersOff(&r[1]) && SameOutsideTriggers(r, before, 1, false), "USB triggers rewritten, rest kept");
        auto quiet = Usb(); quiet[1] = 0x03; const auto q = quiet;
        Check(!PadTriggers::RewriteReport(quiet.data(), quiet.size(), false) && quiet == q, "USB report without trigger bits untouched");
        Check(PadTriggers::RewriteReport(quiet.data(), quiet.size(), true) && TriggersOff(&quiet[1]) && (quiet[1] & 0x03) == 0x03, "USB reset adds an off command");
        auto padded = Usb(); padded.resize(64);
        Check(PadTriggers::RewriteReport(padded.data(), padded.size(), false) && TriggersOff(&padded[1]), "USB report padded by hidapi");
        auto shortr = Usb(); shortr.resize(40);
        Check(!PadTriggers::RewriteReport(shortr.data(), shortr.size(), false), "Short USB report rejected");
    }
    {
        auto r = BtSdl(); const auto before = r;
        Check(PadTriggers::RewriteReport(r.data(), r.size(), false) && TriggersOff(&r[2]) && Sealed(r) && SameOutsideTriggers(r, before, 2, true), "Bluetooth (SDL layout) rewritten, CRC resealed");
        auto l = BtLinux(); const auto lb = l;
        Check(PadTriggers::RewriteReport(l.data(), l.size(), false) && TriggersOff(&l[3]) && Sealed(l) && SameOutsideTriggers(l, lb, 3, true), "Bluetooth (seq/tag layout) rewritten, CRC resealed");
        auto bad = BtSdl(); bad[74] ^= 1; const auto b = bad;
        Check(!PadTriggers::RewriteReport(bad.data(), bad.size(), true) && bad == b, "Bluetooth report with a bad CRC untouched");
        std::vector<uint8_t> ds4(32); ds4[0] = 0x05; ds4[1] = 0xff; const auto d = ds4;
        Check(!PadTriggers::RewriteReport(ds4.data(), ds4.size(), true) && ds4 == d, "Other report ids untouched");
    }
    // Steam Input slot decoding: the shipped thunk is "48 8B 01 48 FF A0 78 01 00 00" (slot 47).
    {
        const uint8_t far_jump[]{0x48, 0x8b, 0x01, 0x48, 0xff, 0xa0, 0x78, 0x01, 0x00, 0x00, 0xcc, 0xcc};
        const uint8_t near_jump[]{0x48, 0x8b, 0x01, 0x48, 0xff, 0x60, 0x50, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
        const uint8_t no_rex[]{0x48, 0x8b, 0x01, 0xff, 0x60, 0x08, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc, 0xcc};
        const uint8_t other[]{0x48, 0x83, 0xec, 0x28, 0x48, 0x8d, 0x0d, 0x25, 0x54, 0x04, 0x00, 0xe8};
        Check(PadTriggers::SteamSlotFromExport(far_jump) == 47, "disp32 thunk");
        Check(PadTriggers::SteamSlotFromExport(near_jump) == 10, "disp8 thunk");
        Check(PadTriggers::SteamSlotFromExport(no_rex) == 1, "thunk without REX jump");
        Check(PadTriggers::SteamSlotFromExport(other) == -1, "non-thunk rejected");
        if (argc > 1) {  // the game's own steam_api64.dll, mapped without running it
            HMODULE steam = LoadLibraryExA(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
            const auto* code = steam ? reinterpret_cast<const uint8_t*>(GetProcAddress(steam, "SteamAPI_ISteamInput_SetDualSenseTriggerEffect")) : nullptr;
            const int slot = code ? PadTriggers::SteamSlotFromExport(code) : -2;
            std::printf("steam_api64 SetDualSenseTriggerEffect slot %d\n", slot);
            Check(slot == 47, "shipped steam_api64 slot");
            if (steam) FreeLibrary(steam);  // never initialized: the Steam watcher must not find it
        }
    }
    char temp[MAX_PATH]{}; GetTempPathA(MAX_PATH, temp);
    const std::string ini_a = std::string(temp) + "pad_triggers_test.ini";
    const std::wstring ini(ini_a.begin(), ini_a.end());
    DeleteFileW(ini.c_str());
    PadTriggers::Install();  // patches this test executable's own import table
    PadTriggers::Attach(&Log, ini.c_str());
    Check(PadTriggers::ForcedOff(), "Forced off by default");
    // Steam Input vtable hook on a fake interface whose vtable is read-only, like steamclient64's.
    {
        auto* page = static_cast<void**>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        page[47] = reinterpret_cast<void*>(&FakeSetTrigger);
        DWORD old = 0; VirtualProtect(page, 4096, PAGE_READONLY, &old);
        struct Object { void** vtable; } object{page};
        Check(PadTriggers::HookSteamInput(&object, 47) && page[47] != reinterpret_cast<void*>(&FakeSetTrigger), "Steam vtable slot hooked");
        Check(PadTriggers::HookSteamInput(&object, 47), "Second hook of the same vtable is a no-op");
        using Fn = void (*)(void*, uint64_t, const void*);
        std::array<uint8_t, 120> weapon{}; weapon[0] = 0x02; weapon[64] = 2; weapon[72] = 2; weapon[73] = 7; weapon[74] = 8;
        reinterpret_cast<Fn>(page[47])(&object, 77, weapon.data());
        bool off = received.calls == 1 && received.self == &object && received.pad == 77 && received.param[0] == 0x03;
        for (size_t i = 1; i < 120; ++i) off = off && received.param[i] == 0;
        Check(off, "Steam trigger effect replaced by both-triggers off");
        PadTriggers::SetForcedOff(false);
        reinterpret_cast<Fn>(page[47])(&object, 78, weapon.data());
        Check(received.calls == 2 && received.param == weapon, "Steam effect passes through when not forced");
        Check(GetPrivateProfileIntW(L"Controller", L"ForceAdaptiveTriggersOff", 9, ini.c_str()) == 0, "Setting saved (0)");
        PadTriggers::SetForcedOff(true);
        Check(GetPrivateProfileIntW(L"Controller", L"ForceAdaptiveTriggersOff", 9, ini.c_str()) == 1, "Setting saved (1)");
        void* fake_data = nullptr;
        Check(!PadTriggers::HookSteamInput(&fake_data, 0), "Null vtable rejected");
        VirtualFree(page, 0, MEM_RELEASE);
    }
    // Import-table path: WriteFile on a tracked handle (a pipe stands in for the HID handle).
    {
        HANDLE read = nullptr, write = nullptr;
        CreatePipe(&read, &write, nullptr, 4096);
        PadTriggers::TrackForTest(write);
        auto r = BtSdl(); DWORD n = 0;
        Check(WriteFile(write, r.data(), static_cast<DWORD>(r.size()), &n, nullptr) && n == r.size(), "WriteFile through the hook");
        std::vector<uint8_t> got(78); ReadFile(read, got.data(), 78, &n, nullptr);
        Check(n == 78 && TriggersOff(&got[2]) && Sealed(got) && TriggersOff(&r[2]), "Tracked write rewritten (in place)");
        // A read-only caller buffer is rewritten in a copy, never written.
        auto* ro = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        const auto u = Usb(); std::memcpy(ro, u.data(), u.size());
        DWORD old = 0; VirtualProtect(ro, 4096, PAGE_READONLY, &old);
        Check(WriteFile(write, ro, 48, &n, nullptr), "Read-only buffer write");
        ReadFile(read, got.data(), 48, &n, nullptr);
        Check(n == 48 && TriggersOff(&got[1]) && std::memcmp(ro, u.data(), 48) == 0, "Read-only buffer: copy rewritten, caller memory unchanged");
        VirtualFree(ro, 0, MEM_RELEASE);
        // DeviceIoControl(IOCTL_HID_SET_OUTPUT_REPORT) and the GetProcAddress-wrapped HidD_SetOutputReport (both fail on a pipe).
        auto io = Usb();
        DeviceIoControl(write, 0x000B0195, io.data(), 48, nullptr, 0, &n, nullptr);
        Check(TriggersOff(&io[1]), "DeviceIoControl output report rewritten");
        HMODULE hid = LoadLibraryW(L"hid.dll");
        using SetOutput = BOOLEAN(__stdcall*)(HANDLE, PVOID, ULONG);
        const auto wrapped = reinterpret_cast<SetOutput>(GetProcAddress(hid, "HidD_SetOutputReport"));
        Check(wrapped && reinterpret_cast<FARPROC>(wrapped) != GetProcAddress(hid, "HidD_GetAttributes"), "HidD_SetOutputReport resolved");
        auto so = Usb();
        if (wrapped) wrapped(write, so.data(), 48);
        Check(TriggersOff(&so[1]), "HidD_SetOutputReport rewritten");
        // Untracked handles and the setting switched off pass reports through unchanged.
        PadTriggers::SetForcedOff(false);
        auto pass = Usb(); const auto p = pass;
        WriteFile(write, pass.data(), 48, &n, nullptr); ReadFile(read, got.data(), 48, &n, nullptr);
        Check(pass == p && std::memcmp(got.data(), p.data(), 48) == 0, "Not forced: unchanged");
        PadTriggers::SetForcedOff(true);
        CloseHandle(write);
        HANDLE read2 = nullptr, write2 = nullptr; CreatePipe(&read2, &write2, nullptr, 4096);
        auto other = Usb(); const auto o = other;
        WriteFile(write2, other.data(), 48, &n, nullptr); ReadFile(read2, got.data(), 48, &n, nullptr);
        Check(other == o && std::memcmp(got.data(), o.data(), 48) == 0, "Untracked (closed and reused) handle unchanged");
        CloseHandle(read); CloseHandle(read2); CloseHandle(write2);
        // A DualSense-looking path that does not exist is not tracked.
        HANDLE missing = CreateFileW(L"\\\\?\\hid#vid_054c&pid_0ce6&mi_03#0&0&0#{4d1e55b2-f16f-11cf-88cb-001111000030}", GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Check(missing == INVALID_HANDLE_VALUE, "Missing device path fails as before");
    }
    wchar_t status[200]{}; PadTriggers::Status(status, 200);
    std::printf("status: %ls\n", status);
    DeleteFileW(ini.c_str());
    std::printf("pad_triggers_test failures=%d\n", failures);
    return failures ? 1 : 0;
}
