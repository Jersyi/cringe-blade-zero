#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Pure parts of EnvCheck.exe (the players' environment check, Install.bat 4), kept apart so env_check_test can run
// them without a game: PE import / export tables of a DLL on disk, which DLL a version.dll is, and the startup state of
// the last game run as PBZDebugMenu.log records it.
namespace EnvCheck {
struct ImportModule {
    std::string name;
    std::vector<std::string> functions;  // imported by name
    std::vector<uint16_t> ordinals;      // imported by ordinal
    bool delay = false;                  // delay-load table
};
// x64 (PE32+) image only; false with why on anything else or a table outside the file.
bool ReadImports(const std::vector<uint8_t>& file, std::vector<ImportModule>& out, std::string& why);
bool ReadExports(const std::vector<uint8_t>& file, std::vector<std::string>& out, std::string& why);
bool IsX64Image(const std::vector<uint8_t>& file);

// Same markers as install.ps1 Test-OurDll: the full DLL carries "PBZDebugMenu", the triggers-only one "PBZTriggersOff"
// (narrow or UTF-16).
enum class DllKind { Full, TriggersOnly, Foreign };
DllKind ClassifyDll(const std::vector<uint8_t>& file);

// The last game run in PBZDebugMenu.log: the lines of the pid on the file's last line ("yyyy-mm-dd hh:mm:ss pid=N ...").
struct LastRun {
    bool found = false;
    unsigned pid = 0;
    std::string first, last;  // timestamps of that pid's first and last line
    enum class Start { Unknown, Steam, WeGame, UnsupportedBuild, UnsupportedHost, PinFailed } start = Start::Unknown;
    bool banner = false;        // UI_LOAD_BANNER success
    bool refMismatch = false;   // UE_RUNTIME reference helpers mismatch
    bool hookWaiting = false;   // GAME_THREAD_HOOK not installed after 10 minutes
    size_t lines = 0;
};
LastRun ParseLog(const std::string& text);

// Every run in PBZDebugMenu.log in order of first appearance: its pid and the timestamps ("yyyy-mm-dd hh:mm:ss", local
// time) of its first and last line. host is false for a run that logged UNSUPPORTED_HOST (another program loaded the DLL).
struct RunSpan {
    unsigned pid = 0;
    std::string first, last;
    bool host = true;
};
std::vector<RunSpan> ParseRuns(const std::string& text);

// Exploit-protection settings (an IFEO or kernel MitigationOptions value, REG_BINARY of 8 / 16 / 24 bytes) that keep a
// game from loading the unsigned version.dll next to it. Bits of the result:
enum LoadBlocker : uint32_t {
    kPreferSystem32 = 1,       // "Prefer system images": version.dll comes from System32, the mod never loads
    kMicrosoftSignedOnly = 2,  // "Code integrity guard" (Microsoft-signed only / Store): the mod is refused
    kNoRemoteImages = 4,       // "Block remote images": a game on a network drive loads no DLL from there
    kNoLowLabelImages = 8,     // "Block low integrity images"
    kNonCetBlocked = 16,       // "Hardware-enforced stack protection" set to block non-CET modules (the mod is not CET-marked)
};
uint32_t LoaderBlockers(const std::vector<uint8_t>& mitigationOptions);

// Lower-case hex SHA-256 of a file (bcrypt), streamed; empty with why on failure.
std::string Sha256File(const std::wstring& path, std::string& why);
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& out, size_t limit = 64u << 20);
}
