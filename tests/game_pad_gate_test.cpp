// GamePadGate on a real PE: this executable stands in for PBZ.exe. It delay-loads XINPUT1_4.dll (like Unreal), gets
// XInputGetState / XInputGetStateEx from GetProcAddress (like SDL), and reads "DualSense" reports through ReadFile /
// GetOverlappedResult of a tracked handle (like SDL's hidapi), all through its own import table, which the gate patches.
#include "game_pad_gate.h"
#include "pad_triggers.h"
#include <windows.h>
#include <Xinput.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {
int failures = 0;
std::vector<std::string> lines;
void Check(bool ok, const char* name) { if (!ok) ++failures; std::printf("%s %s\n", ok ? "ok  " : "FAIL", name); }
void Log(const char* line) { lines.emplace_back(line); std::printf("  [log] %s\n", line); }
bool Logged(const char* part) { for (const std::string& line : lines) if (line.find(part) != std::string::npos) return true; return false; }
constexpr unsigned kView = 1u << 5, kMenu = 1u << 4;

// USB DualSense input report 0x01: left stick pushed, Cross held, PS button.
void Report(uint8_t* r) {
    std::memset(r, 0, 64);
    r[0] = 0x01; r[1] = 0x10; r[2] = 0xf0; r[3] = 0x80; r[4] = 0x80; r[8] = 0x08 | 0x20; r[10] = 0x01;
}
bool AtRest(const uint8_t* r) { return r[1] == 0x80 && r[2] == 0x80 && r[8] == 0x08 && r[9] == 0 && (r[10] & 0x07) == 0; }
}  // namespace

int main() {
    PadTriggers::Install();
    GamePadGate::Install();
    GamePadGate::SetLogger(&Log);
    Check(Logged("Unreal XInput slot hooked"), "delay-import slot of XINPUT1_4!XInputGetState found and replaced");
    Check(Logged("raw input hooked"), "GetRawInputData import replaced");

    // Unreal's path: the delay-import slot.
    XINPUT_STATE state{};
    const DWORD first = XInputGetState(0, &state);
    Check(Logged("GAME_PAD Unreal XInputGetState resolved") && Logged("Unreal XInputGetState (delay import): first call"), "a delay-imported XInputGetState call goes through the gate");
    GamePadGate::Update(true, kView | kMenu, kView | kMenu);
    std::memset(&state, 0x5a, sizeof(state));
    if (XInputGetState(0, &state) == ERROR_SUCCESS) {
        Check(state.Gamepad.wButtons == 0 && state.Gamepad.sThumbLX == 0 && state.Gamepad.sThumbLY == 0 && state.Gamepad.bLeftTrigger == 0, "panel up: the delay-import read is a pad at rest");
    } else std::printf("note XInput user 0 not connected (%lu): the at-rest check needs a pad\n", first);
    GamePadGate::Update(false, kView | kMenu, kView | kMenu);
    Check(XInputGetState(4, &state) == ERROR_BAD_ARGUMENTS || XInputGetState(4, &state) != ERROR_SUCCESS, "a bad user index still reaches XInput");

    // SDL's path: GetProcAddress (this executable's import, hooked by PadTriggers) hands out the gate.
    const HMODULE xinput = LoadLibraryW(L"XInput1_4.dll");
    using GetProc = FARPROC(WINAPI*)(HMODULE, LPCSTR);
    const auto real_get_proc = reinterpret_cast<GetProc>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetProcAddress"));
    const FARPROC by_name = GetProcAddress(xinput, "XInputGetState"), by_ordinal = GetProcAddress(xinput, reinterpret_cast<LPCSTR>(100));
    const FARPROC real_name = real_get_proc(xinput, "XInputGetState"), real_ordinal = real_get_proc(xinput, reinterpret_cast<LPCSTR>(100));
    Check(by_name && real_name && by_name != real_name, "GetProcAddress(XInputGetState) hands out the gate");
    Check(by_ordinal && real_ordinal && by_ordinal != real_ordinal, "GetProcAddress(ordinal 100, XInputGetStateEx) hands out the gate");
    Check(GetProcAddress(xinput, "XInputSetState") == real_get_proc(xinput, "XInputSetState"), "other XInput functions are untouched");
    Check(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64") == real_get_proc(GetModuleHandleW(L"kernel32.dll"), "GetTickCount64"), "other modules are untouched");
    if (by_ordinal) {
        GamePadGate::Update(true, kView | kMenu, kView | kMenu);
        std::memset(&state, 0x5a, sizeof(state));
        if (reinterpret_cast<DWORD(WINAPI*)(DWORD, XINPUT_STATE*)>(by_ordinal)(0, &state) == ERROR_SUCCESS)
            Check(state.Gamepad.wButtons == 0 && state.Gamepad.sThumbRX == 0, "panel up: the GetStateEx read is a pad at rest");
        GamePadGate::Update(false, kView | kMenu, kView | kMenu);
    }

    // SDL hidapi's path: synchronous ReadFile of a tracked handle.
    wchar_t folder[MAX_PATH]{}, file[MAX_PATH]{};
    GetTempPathW(MAX_PATH, folder); GetTempFileNameW(folder, L"pgt", 0, file);
    uint8_t report[64]{}; Report(report);
    HANDLE out = CreateFileW(file, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD n = 0;
    WriteFile(out, report, 64, &n, nullptr); WriteFile(out, report, 64, &n, nullptr); CloseHandle(out);
    HANDLE in = CreateFileW(file, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    PadTriggers::TrackForTest(in);
    GamePadGate::Update(true, kView | kMenu, kView | kMenu);
    uint8_t buffer[64]{};
    Check(ReadFile(in, buffer, 64, &n, nullptr) && n == 64 && AtRest(buffer) && buffer[0] == 0x01, "panel up: a DualSense report read with ReadFile is at rest");
    Check(Logged("through HID ReadFile"), "the first DualSense HID read is logged");
    GamePadGate::Update(false, kView | kMenu, kView | kMenu);
    Check(ReadFile(in, buffer, 64, &n, nullptr) && n == 64 && (buffer[8] & 0x20) == 0 && buffer[2] == 0xf0, "panel gone: Cross held from it stays hidden, the stick passes");
    CloseHandle(in); DeleteFileW(file);

    // Overlapped: ReadFile pending, then GetOverlappedResult (a pipe, so the read cannot finish at once).
    wchar_t pipe[96]{}; swprintf_s(pipe, L"\\\\.\\pipe\\pbz_gate_test_%lu", GetCurrentProcessId());
    HANDLE server = CreateNamedPipeW(pipe, PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED, PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 256, 256, 0, nullptr);
    HANDLE client = CreateFileW(pipe, GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    PadTriggers::TrackForTest(server);
    OVERLAPPED overlapped{}; overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::memset(buffer, 0, sizeof(buffer));
    GamePadGate::Update(true, kView | kMenu, kView | kMenu);
    const BOOL started = ReadFile(server, buffer, 64, nullptr, &overlapped);
    const DWORD error = GetLastError();
    Check(!started && error == ERROR_IO_PENDING, "the pipe read is pending (ERROR_IO_PENDING kept for the caller)");
    Report(report); WriteFile(client, report, 64, &n, nullptr);
    n = 0;
    Check(GetOverlappedResult(server, &overlapped, &n, TRUE) && n == 64 && AtRest(buffer), "panel up: an overlapped read is at rest when GetOverlappedResult reports it");
    GamePadGate::Update(false, kView | kMenu, kView | kMenu);
    // A read that fails is forgotten (no filter on a stale buffer later).
    CloseHandle(client); CloseHandle(server); CloseHandle(overlapped.hEvent);

    // Raw input stays callable through the hook.
    UINT size = 0;
    Check(GetRawInputData(nullptr, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == static_cast<UINT>(-1), "GetRawInputData on a bad handle fails as before");

    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
