// EnvCheck.exe: the players' environment check and repair for Cringe Blade Zero (Install.bat 4, or double-click).
// Checks Windows, the runtimes the game needs (VC++ 2015-2022 x64, Direct3D 12), every system DLL / function the mod
// imports, controllers and fonts, what in Windows can stop the DLL loading (KnownDLLs, Exploit protection, Smart App
// Control, ASR, security software, Code Integrity / Defender refusals), then each game install: build, installed DLL,
// misplaced copies, other loaders, download mark, write access, settings, .combo files and the last run's log; last,
// whether the game really loaded the mod (the running game's modules, or its own last run against the mod log).
// Problems that can be fixed get a numbered repair.
//
// Static CRT and only kernel32 / advapi32 / bcrypt loaded at start (everything else delay-loaded from System32, and
// /DEPENDENTLOADFLAG keeps the static imports in System32 too), so it runs where the runtimes are missing, and a
// version.dll next to it (the package folder has one) is never loaded into it.
//
// EnvCheck.exe [--game <PBZ.exe or folder>]... [--package <dir>] [--fix all|<id>[,<id>]] [--dry-run] [--no-pause]
//              [--lang zh|en] [--report <file>]
// Exit code: 0 nothing to fix, 1 notes only, 2 problems.
#include "env_checks.h"
#include "combo_file.h"
#include "known_exes.h"
#include <windows.h>
#include <aclapi.h>
#include <conio.h>
#include <d3d12.h>
#include <dxgi.h>
#include <objbase.h>
#include <sddl.h>
#include <setupapi.h>
#include <shellapi.h>
#include <softpub.h>
#include <tlhelp32.h>
#include <urlmon.h>
#include <wincrypt.h>
#include <wintrust.h>
#include <winevt.h>
#include <xinput.h>
#include <algorithm>
#include <cstdarg>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace {
// ---- Text -------------------------------------------------------------------------------------------------------
bool zh = false;
const wchar_t* T(const wchar_t* chinese, const wchar_t* english) { return zh ? chinese : english; }

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring Lower(std::wstring text) {
    for (wchar_t& c : text) c = static_cast<wchar_t>(std::towlower(c));
    return text;
}
std::wstring Format(const wchar_t* format, ...) {
    wchar_t text[2048]{};
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(text, _TRUNCATE, format, args);
    va_end(args);
    return text;
}

// ---- Paths ------------------------------------------------------------------------------------------------------
std::wstring Join(const std::wstring& a, const std::wstring& b) { return a.empty() || a.back() == L'\\' ? a + b : a + L'\\' + b; }
std::wstring Parent(const std::wstring& path) {
    const size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? std::wstring() : path.substr(0, at);
}
std::wstring Leaf(const std::wstring& path) {
    const size_t at = path.find_last_of(L"\\/");
    return at == std::wstring::npos ? path : path.substr(at + 1);
}
bool IsFile(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
bool IsDir(const std::wstring& path) {
    const DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
unsigned long long FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return 0;
    return (static_cast<unsigned long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
}
// Absolute, with 8.3 short names expanded (process and event-log paths are always long).
std::wstring FullPath(const std::wstring& path) {
    wchar_t out[MAX_PATH * 4]{}, full[MAX_PATH * 4]{};
    const DWORD n = GetFullPathNameW(path.c_str(), static_cast<DWORD>(std::size(out)), out, nullptr);
    if (!n || n >= std::size(out)) return path;
    const DWORD m = GetLongPathNameW(out, full, static_cast<DWORD>(std::size(full)));
    return m && m < std::size(full) ? std::wstring(full) : std::wstring(out);
}
std::vector<std::wstring> List(const std::wstring& dir, const wchar_t* pattern, bool dirs) {
    std::vector<std::wstring> out;
    WIN32_FIND_DATAW data{};
    HANDLE find = FindFirstFileW(Join(dir, pattern).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (isDir != dirs || !wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
        out.push_back(data.cFileName);
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(out.begin(), out.end());
    return out;
}
std::wstring ExePath() {
    wchar_t path[MAX_PATH * 4]{};
    GetModuleFileNameW(nullptr, path, static_cast<DWORD>(std::size(path)));
    return path;
}
std::wstring System32() {
    wchar_t path[MAX_PATH]{};
    GetSystemDirectoryW(path, MAX_PATH);
    return path;
}
std::wstring Stamp() {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    return Format(L"%04u%02u%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

// ---- Time (FILETIME, UTC, as one number; 0 = unknown) ------------------------------------------------------------
using Time64 = unsigned long long;
constexpr Time64 kSecond = 10000000ull;
Time64 ToTime(const FILETIME& t) { return (static_cast<Time64>(t.dwHighDateTime) << 32) | t.dwLowDateTime; }
// The mod's log stamps: "yyyy-mm-dd hh:mm:ss", local time.
Time64 FromLogStamp(const std::string& s) {
    SYSTEMTIME local{}, utc{};
    FILETIME ft{};
    if (sscanf_s(s.c_str(), "%hu-%hu-%hu %hu:%hu:%hu", &local.wYear, &local.wMonth, &local.wDay, &local.wHour, &local.wMinute, &local.wSecond) != 6 ||
        !TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) || !SystemTimeToFileTime(&utc, &ft)) return 0;
    return ToTime(ft);
}
std::wstring LocalText(Time64 t) {
    const FILETIME ft{static_cast<DWORD>(t), static_cast<DWORD>(t >> 32)};
    SYSTEMTIME utc{}, local{};
    if (!t || !FileTimeToSystemTime(&ft, &utc) || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local)) return L"?";
    return Format(L"%04u-%02u-%02u %02u:%02u", local.wYear, local.wMonth, local.wDay, local.wHour, local.wMinute);
}
Time64 WrittenAt(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    return GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ? ToTime(data.ftLastWriteTime) : 0;
}
// When a file was put in place: copying keeps the source's write time, so its creation and change times count too.
Time64 PlacedAt(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 0;
    FILE_BASIC_INFO info{};
    const bool ok = GetFileInformationByHandleEx(file, FileBasicInfo, &info, sizeof(info)) != 0;
    CloseHandle(file);
    if (!ok) return 0;
    return std::max({static_cast<Time64>(info.CreationTime.QuadPart), static_cast<Time64>(info.LastWriteTime.QuadPart), static_cast<Time64>(info.ChangeTime.QuadPart)});
}

// ---- Registry ---------------------------------------------------------------------------------------------------
std::wstring RegString(HKEY root, const std::wstring& key, const wchar_t* name, REGSAM view = 0) {
    wchar_t value[2048]{};
    DWORD size = sizeof(value) - sizeof(wchar_t);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : 0),
                     nullptr, value, &size) != ERROR_SUCCESS) return {};
    return value;
}
DWORD RegDword(HKEY root, const std::wstring& key, const wchar_t* name, DWORD fallback, REGSAM view = 0) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(root, key.c_str(), name, RRF_RT_REG_DWORD | (view == KEY_WOW64_32KEY ? RRF_SUBKEY_WOW6432KEY : view == KEY_WOW64_64KEY ? RRF_SUBKEY_WOW6464KEY : 0),
                     nullptr, &value, &size) != ERROR_SUCCESS) return fallback;
    return value;
}
std::vector<std::wstring> RegSubkeys(HKEY root, const std::wstring& key, REGSAM view) {
    std::vector<std::wstring> out;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ | view, &handle) != ERROR_SUCCESS) return out;
    wchar_t name[256];
    for (DWORD i = 0;; ++i) {
        DWORD size = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(handle, i, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(handle);
    return out;
}
// Raw bytes of a binary / QWORD / DWORD value (exploit-protection MitigationOptions).
std::vector<uint8_t> RegBytes(HKEY root, const std::wstring& key, const wchar_t* name) {
    constexpr DWORD kTypes = RRF_RT_REG_BINARY | RRF_RT_REG_QWORD | RRF_RT_REG_DWORD;
    DWORD size = 0;
    if (RegGetValueW(root, key.c_str(), name, kTypes, nullptr, nullptr, &size) != ERROR_SUCCESS || !size) return {};
    std::vector<uint8_t> out(size);
    if (RegGetValueW(root, key.c_str(), name, kTypes, nullptr, out.data(), &size) != ERROR_SUCCESS) return {};
    out.resize(size);
    return out;
}
// Name and text of every string value of a key.
std::vector<std::pair<std::wstring, std::wstring>> RegStrings(HKEY root, const std::wstring& key) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return out;
    wchar_t name[512];
    wchar_t data[1024];
    for (DWORD i = 0;; ++i) {
        DWORD nameSize = static_cast<DWORD>(std::size(name)), dataSize = sizeof(data) - sizeof(wchar_t), type = 0;
        const LSTATUS status = RegEnumValueW(handle, i, name, &nameSize, nullptr, &type, reinterpret_cast<BYTE*>(data), &dataSize);
        if (status == ERROR_MORE_DATA) continue;
        if (status != ERROR_SUCCESS) break;
        if (type != REG_SZ && type != REG_EXPAND_SZ) continue;
        data[dataSize / sizeof(wchar_t)] = L'\0';
        out.emplace_back(name, data);
    }
    RegCloseKey(handle);
    return out;
}
std::vector<std::wstring> RegValueNames(HKEY root, const std::wstring& key) {
    std::vector<std::wstring> out;
    HKEY handle = nullptr;
    if (RegOpenKeyExW(root, key.c_str(), 0, KEY_READ, &handle) != ERROR_SUCCESS) return out;
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD size = static_cast<DWORD>(std::size(name));
        if (RegEnumValueW(handle, i, name, &size, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        out.push_back(name);
    }
    RegCloseKey(handle);
    return out;
}

// ---- Output -----------------------------------------------------------------------------------------------------
enum class Level { Ok, Info, Warn, Fail };
struct Out {
    HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
    bool console = false;
    WORD normal = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    std::wstring report;
    int warns = 0, fails = 0;
    Out() {
        DWORD mode = 0;
        console = GetConsoleMode(handle, &mode) != 0;
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (console && GetConsoleScreenBufferInfo(handle, &info)) normal = info.wAttributes;
    }
    void Write(const std::wstring& text, WORD color = 0) {
        if (console) {
            if (color) SetConsoleTextAttribute(handle, color);
            DWORD written = 0;
            WriteConsoleW(handle, text.c_str(), static_cast<DWORD>(text.size()), &written, nullptr);
            if (color) SetConsoleTextAttribute(handle, normal);
        } else {
            const std::string bytes = Utf8(text);
            DWORD written = 0;
            WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
        }
        report += text;
    }
    void Line(const std::wstring& text = L"", WORD color = 0) { Write(text + L"\r\n", color); }
    void Section(const std::wstring& title) { Line(); Line(title, FOREGROUND_GREEN | FOREGROUND_RED | FOREGROUND_INTENSITY); }
    void Item(Level level, const std::wstring& title, const std::wstring& detail) {
        static const WORD colors[] = {FOREGROUND_GREEN | FOREGROUND_INTENSITY, FOREGROUND_BLUE | FOREGROUND_GREEN | FOREGROUND_INTENSITY,
                                      FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY, FOREGROUND_RED | FOREGROUND_INTENSITY};
        static const wchar_t* zhTags[] = {L"[正常]", L"[信息]", L"[注意]", L"[问题]"};
        static const wchar_t* enTags[] = {L"[ OK ]", L"[INFO]", L"[WARN]", L"[FAIL]"};
        const int i = static_cast<int>(level);
        if (level == Level::Warn) ++warns;
        if (level == Level::Fail) ++fails;
        Write(L"  ");
        Write(zh ? zhTags[i] : enTags[i], colors[i]);
        Line(L" " + title + (detail.empty() ? L"" : (zh ? L"：" : L": ") + detail));
    }
    void Hint(const std::wstring& text) { Line(L"         " + text, FOREGROUND_INTENSITY); }
};

// ---- Options and state ------------------------------------------------------------------------------------------
struct Options {
    std::vector<std::wstring> games;
    std::wstring package, report, fix, grantSid;
    bool dryRun = false, noPause = false;
} options;

struct Game { std::wstring exe, folder, source; };
struct Package { std::wstring dir, full, triggers; std::string fullSha, triggersSha; } package;

struct Fix {
    std::wstring id, title;
    bool confirm = false;  // asks Y/N first (downloads, permission changes, long repairs)
    std::function<bool(Out&)> run;
};
std::vector<Fix> fixes;
void AddFix(const std::wstring& id, const std::wstring& title, bool confirm, std::function<bool(Out&)> run) {
    for (const Fix& f : fixes) if (f.id == id) return;
    fixes.push_back({id, title, confirm, std::move(run)});
}

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elevation{};
    DWORD size = 0;
    const bool ok = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) &&
                    GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    if (token) CloseHandle(token);
    return ok && elevation.TokenIsElevated;
}
std::wstring UserSid() {
    HANDLE token = nullptr;
    std::wstring out;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return out;
    BYTE buffer[256]{};
    DWORD size = 0;
    if (GetTokenInformation(token, TokenUser, buffer, sizeof(buffer), &size)) {
        LPWSTR text = nullptr;
        if (ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buffer)->User.Sid, &text)) { out = text; LocalFree(text); }
    }
    CloseHandle(token);
    return out;
}

// One key, lower-cased; Esc gives 27.
wchar_t ReadKey() {
    wchar_t c = static_cast<wchar_t>(_getwch());
    if (c == 0 || c == 0xE0) { _getwch(); return 0; }
    return static_cast<wchar_t>(std::towlower(c));
}
bool Confirm(Out& out, const std::wstring& question) {
    out.Write(L"  " + question + T(L"（Y 确认 / 其他键取消）", L" (Y to confirm, any other key cancels) "));
    const wchar_t c = ReadKey();
    out.Line(c == L'y' ? L"Y" : L"N");
    return c == L'y';
}

// ---- Discovery --------------------------------------------------------------------------------------------------
std::wstring Trim(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'"' || s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == L' ' || s[i] == L'"')) ++i;
    return s.substr(i);
}
// PBZ.exe of a game folder: platform\steam, another platform\<store>, or the folder itself (install.ps1 Find-GameExe).
std::wstring FindGameExe(std::wstring root) {
    root = Trim(root);
    if (root.empty()) return {};
    for (wchar_t& c : root) if (c == L'/') c = L'\\';
    if (Lower(Leaf(root)) == L"pbz.exe") return IsFile(root) ? FullPath(root) : std::wstring();
    std::vector<std::wstring> candidates{Join(root, L"platform\\steam\\PBZ.exe")};
    for (const std::wstring& store : List(Join(root, L"platform"), L"*", true)) candidates.push_back(Join(Join(Join(root, L"platform"), store), L"PBZ.exe"));
    candidates.push_back(Join(root, L"PBZ.exe"));
    for (const std::wstring& c : candidates) if (IsFile(c)) return FullPath(c);
    return {};
}
// Values of `"key"  "value"` pairs in a Steam .vdf / .acf.
std::vector<std::wstring> VdfValues(const std::wstring& path, const std::string& key) {
    std::vector<std::wstring> out;
    std::vector<uint8_t> bytes;
    if (!EnvCheck::ReadFileBytes(path, bytes, 4u << 20)) return out;
    const std::string text(bytes.begin(), bytes.end()), quoted = "\"" + key + "\"";
    for (size_t at = text.find(quoted); at != std::string::npos; at = text.find(quoted, at + 1)) {
        const size_t open = text.find('"', at + quoted.size());
        if (open == std::string::npos) break;
        std::string value;
        size_t i = open + 1;
        for (; i < text.size() && text[i] != '"' && text[i] != '\n'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            value.push_back(text[i]);
        }
        out.push_back(Wide(value));
    }
    return out;
}
constexpr const wchar_t* kAppId = L"4115450";
std::vector<Game> FindGames() {
    std::vector<Game> games;
    auto add = [&](const std::wstring& exe, const std::wstring& source) {
        if (exe.empty()) return;
        for (const Game& g : games) if (Lower(g.exe) == Lower(exe)) return;
        games.push_back({exe, Parent(exe), source});
    };
    for (const std::wstring& path : options.games) {
        const std::wstring exe = FindGameExe(path);
        if (exe.empty()) continue;
        add(exe, T(L"命令行指定", L"given on the command line"));
    }
    if (!options.games.empty()) return games;
    const std::wstring uninstall = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall";
    for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
        add(FindGameExe(RegString(HKEY_LOCAL_MACHINE, uninstall + L"\\Steam App " + kAppId, L"InstallLocation", view)), T(L"Steam 卸载项", L"Steam uninstall entry"));
    add(FindGameExe(RegString(HKEY_CURRENT_USER, uninstall + L"\\Steam App " + kAppId, L"InstallLocation")), T(L"Steam 卸载项", L"Steam uninstall entry"));
    std::vector<std::wstring> steams{RegString(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath"),
                                     RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_32KEY),
                                     RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Valve\\Steam", L"InstallPath", KEY_WOW64_64KEY)};
    for (std::wstring steam : steams) {
        if (steam.empty()) continue;
        for (wchar_t& c : steam) if (c == L'/') c = L'\\';
        std::vector<std::wstring> libraries{steam};
        for (const std::wstring& library : VdfValues(Join(steam, L"steamapps\\libraryfolders.vdf"), "path")) libraries.push_back(library);
        for (const std::wstring& library : libraries) {
            const std::vector<std::wstring> dirs = VdfValues(Join(library, std::wstring(L"steamapps\\appmanifest_") + kAppId + L".acf"), "installdir");
            if (!dirs.empty()) add(FindGameExe(Join(Join(library, L"steamapps\\common"), dirs[0])), T(L"Steam 库", L"Steam library"));
        }
    }
    // Other stores (WeGame): an uninstall entry named after the game, if the store writes one.
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        for (REGSAM view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY})
            for (const std::wstring& entry : RegSubkeys(root, uninstall, view)) {
                const std::wstring key = uninstall + L"\\" + entry, name = RegString(root, key, L"DisplayName", view);
                if (name.find(L"影之刃零") == std::wstring::npos && Lower(name).find(L"phantom blade zero") == std::wstring::npos) continue;
                add(FindGameExe(RegString(root, key, L"InstallLocation", view)), T(L"卸载项 ", L"uninstall entry ") + name);
            }
    // WeGame libraries (no uninstall entry for every game): <drive>\WeGameApps\<game> or \WeGameApps\rail_apps\<game>.
    wchar_t drives[512]{};
    GetLogicalDriveStringsW(static_cast<DWORD>(std::size(drives) - 1), drives);
    for (const wchar_t* drive = drives; *drive; drive += wcslen(drive) + 1) {
        if (GetDriveTypeW(drive) != DRIVE_FIXED) continue;
        for (const wchar_t* base : {L"WeGameApps", L"Program Files (x86)\\WeGameApps", L"Program Files\\WeGameApps"}) {
            const std::wstring library = Join(drive, base);
            if (!IsDir(library)) continue;
            for (const std::wstring& first : List(library, L"*", true)) {
                const std::wstring dir = Join(library, first);
                add(FindGameExe(dir), T(L"WeGame 游戏库", L"WeGame library"));
                for (const std::wstring& second : List(dir, L"*", true)) add(FindGameExe(Join(dir, second)), T(L"WeGame 游戏库", L"WeGame library"));
            }
        }
    }
    return games;
}

// Names of the running processes, lower-case.
std::vector<std::wstring> ProcessNames() {
    std::vector<std::wstring> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snap, &entry); more; more = Process32NextW(snap, &entry)) out.push_back(Lower(entry.szExeFile));
    CloseHandle(snap);
    return out;
}

// A running PBZ.exe and the version.dll it actually loaded (the module list of an elevated game is closed to a
// non-elevated tool: modulesRead false).
struct Running { DWORD pid = 0; std::wstring exe, versionDll; bool modulesRead = false; };
std::vector<Running> RunningGames() {
    std::vector<Running> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{sizeof(entry)};
    for (BOOL more = Process32FirstW(snap, &entry); more; more = Process32NextW(snap, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"PBZ.exe")) continue;
        Running r;
        r.pid = entry.th32ProcessID;
        if (HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, r.pid)) {
            wchar_t path[MAX_PATH * 4]{};
            DWORD size = static_cast<DWORD>(std::size(path));
            if (QueryFullProcessImageNameW(process, 0, path, &size)) r.exe = path;
            CloseHandle(process);
        }
        HANDLE modules = INVALID_HANDLE_VALUE;
        for (int attempt = 0; attempt < 5 && modules == INVALID_HANDLE_VALUE; ++attempt) {
            modules = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, r.pid);
            if (modules == INVALID_HANDLE_VALUE && GetLastError() != ERROR_BAD_LENGTH) break;  // ERROR_BAD_LENGTH: loader busy, retry
        }
        if (modules != INVALID_HANDLE_VALUE) {
            r.modulesRead = true;
            MODULEENTRY32W m{sizeof(m)};
            for (BOOL ok = Module32FirstW(modules, &m); ok; ok = Module32NextW(modules, &m))
                if (!_wcsicmp(m.szModule, L"version.dll")) { r.versionDll = m.szExePath; break; }
            CloseHandle(modules);
        }
        out.push_back(r);
    }
    CloseHandle(snap);
    return out;
}
bool GameRunningIn(const std::wstring& folder) {
    for (const Running& r : RunningGames()) if (!r.exe.empty() && Lower(Parent(r.exe)) == Lower(folder)) return true;
    return false;
}
// DLL repairs need the game closed: waits for it (Enter re-checks, Esc gives up).
bool WaitGameClosed(Out& out, const std::wstring& folder) {
    while (GameRunningIn(folder)) {
        out.Line(L"  " + std::wstring(T(L"游戏正在运行，请先退出游戏，然后按 Enter 继续（Esc 取消）。", L"The game is running. Quit it, then press Enter (Esc cancels).")),
                 FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        if (ReadKey() == 27) return false;
    }
    return true;
}

// ---- Repairs ----------------------------------------------------------------------------------------------------
std::wstring Quote(const std::wstring& s) { return L"\"" + s + L"\""; }
// Runs this program elevated for one repair (UAC prompt) and waits for it.
bool RelaunchElevated(Out& out, const std::wstring& fixId, const std::vector<Game>& games) {
    std::wstring args = std::wstring(L"--lang ") + (zh ? L"zh" : L"en") + L" --no-pause --fix " + fixId + L" --grant-sid " + UserSid();
    if (!package.dir.empty()) args += L" --package " + Quote(package.dir);
    for (const Game& g : games) args += L" --game " + Quote(g.exe);
    const std::wstring exe = ExePath();
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"runas";
    info.lpFile = exe.c_str();
    info.lpParameters = args.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        out.Line(L"  " + std::wstring(T(L"没有取得管理员权限。", L"Administrator rights were not granted.")));
        return false;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(info.hProcess, &code);
    CloseHandle(info.hProcess);
    return code == 0;
}
bool AccessDenied(DWORD error) { return error == ERROR_ACCESS_DENIED || error == ERROR_PRIVILEGE_NOT_HELD || error == ERROR_WRITE_PROTECT; }

bool CopyVerified(Out& out, const std::wstring& from, const std::wstring& to, const std::string& sha, DWORD& error) {
    error = 0;
    if (!CopyFileW(from.c_str(), to.c_str(), FALSE)) { error = GetLastError(); return false; }
    DeleteFileW((to + L":Zone.Identifier").c_str());  // CopyFile carries the package's download mark over
    std::string why;
    if (EnvCheck::Sha256File(to, why) != sha) {
        out.Line(L"  " + std::wstring(T(L"校验失败：写入的文件与安装包不一致。", L"Verification failed: the written file differs from the package.")));
        return false;
    }
    return true;
}
// Installs the package's full DLL next to PBZ.exe; a foreign version.dll is backed up first (install.ps1 does the same).
bool InstallFull(Out& out, const Game& game, const std::vector<Game>& games, const std::wstring& fixId) {
    const std::wstring target = Join(game.folder, L"version.dll");
    out.Line(L"  " + std::wstring(T(L"安装完整版 MOD → ", L"Installing the full mod → ")) + target);
    if (options.dryRun) return true;
    if (!WaitGameClosed(out, game.folder)) return false;
    DWORD error = 0;
    std::vector<uint8_t> bytes;
    if (IsFile(target) && EnvCheck::ReadFileBytes(target, bytes, 256u << 20) && EnvCheck::ClassifyDll(bytes) == EnvCheck::DllKind::Foreign) {
        const std::wstring backup = target + L".backup_" + Stamp();
        if (!CopyFileW(target.c_str(), backup.c_str(), TRUE)) error = GetLastError();
        else out.Line(L"  " + std::wstring(T(L"原有的 version.dll 不属于豪之刃，已备份为 ", L"The existing version.dll belongs to another mod; backed up as ")) + Leaf(backup));
    }
    if (!error && CopyVerified(out, package.full, target, package.fullSha, error)) return true;
    if (AccessDenied(error) && !IsElevated()) return RelaunchElevated(out, fixId, games);
    if (error) out.Line(L"  " + Format(T(L"复制失败（错误 %lu）。", L"Copy failed (error %lu)."), error));
    return false;
}
// Moves a copy of the mod that the game never loads (wrong folder) out of the way.
bool MoveAside(Out& out, const std::wstring& path, const std::vector<Game>& games, const std::wstring& fixId) {
    const std::wstring to = path + L".misplaced_" + Stamp();
    out.Line(L"  " + std::wstring(T(L"改名 ", L"Renaming ")) + path + L" → " + Leaf(to));
    if (options.dryRun) return true;
    if (MoveFileW(path.c_str(), to.c_str())) return true;
    const DWORD error = GetLastError();
    if (AccessDenied(error) && !IsElevated()) return RelaunchElevated(out, fixId, games);
    out.Line(L"  " + Format(T(L"改名失败（错误 %lu）。", L"Rename failed (error %lu)."), error));
    return false;
}
// Removes the "downloaded from the internet" mark (the Zone.Identifier stream) that unpacking a downloaded ZIP leaves.
bool Unblock(Out& out, const std::wstring& file, const std::vector<Game>& games, const std::wstring& fixId) {
    out.Line(L"  " + std::wstring(T(L"去掉“来自网络”的标记：", L"Removing the downloaded-from-the-internet mark: ")) + file);
    if (options.dryRun) return true;
    if (DeleteFileW((file + L":Zone.Identifier").c_str())) return true;
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return true;
    if (AccessDenied(error) && !IsElevated()) return RelaunchElevated(out, fixId, games);
    out.Line(L"  " + Format(T(L"失败（错误 %lu）。", L"Failed (error %lu)."), error));
    return false;
}
bool ClearReadOnly(Out& out, const std::vector<std::wstring>& files) {
    bool ok = true;
    for (const std::wstring& f : files) {
        out.Line(L"  " + std::wstring(T(L"取消只读 ", L"Clearing read-only on ")) + f);
        if (options.dryRun) continue;
        const DWORD a = GetFileAttributesW(f.c_str());
        ok = a != INVALID_FILE_ATTRIBUTES && SetFileAttributesW(f.c_str(), a & ~FILE_ATTRIBUTE_READONLY) && ok;
    }
    return ok;
}
// Bad .combo files go to combos\broken (kept, no longer read or reported).
bool QuarantineCombos(Out& out, const std::wstring& combos, const std::vector<std::wstring>& names) {
    const std::wstring broken = Join(combos, L"broken");
    if (!options.dryRun) CreateDirectoryW(broken.c_str(), nullptr);
    bool ok = true;
    for (const std::wstring& name : names) {
        const std::wstring to = Join(broken, name + L"." + Stamp());
        out.Line(L"  " + std::wstring(T(L"移到 combos\\broken：", L"Moving to combos\\broken: ")) + name);
        if (!options.dryRun) ok = MoveFileW(Join(combos, name).c_str(), to.c_str()) && ok;
    }
    return ok;
}
// Gives the signed-in user modify rights on the PBZ.exe folder (inherited by its files), so the mod can save its
// settings, log and .combo files there. Needs administrator rights; the elevated copy is told the user's SID.
bool GrantFolder(Out& out, const std::wstring& folder, const std::vector<Game>& games, const std::wstring& fixId) {
    const std::wstring sidText = options.grantSid.empty() ? UserSid() : options.grantSid;
    out.Line(L"  " + std::wstring(T(L"给当前用户授予修改权限：", L"Granting the current user modify rights on ")) + folder);
    if (options.dryRun) return true;
    if (!IsElevated()) return RelaunchElevated(out, fixId, games);
    PSID sid = nullptr;
    if (!ConvertStringSidToSidW(sidText.c_str(), &sid)) { out.Line(L"  SID?"); return false; }
    PACL old = nullptr, updated = nullptr;
    PSECURITY_DESCRIPTOR sd = nullptr;
    DWORD error = GetNamedSecurityInfoW(folder.c_str(), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, &old, nullptr, &sd);
    if (error == ERROR_SUCCESS) {
        EXPLICIT_ACCESSW access{};
        access.grfAccessPermissions = FILE_GENERIC_READ | FILE_GENERIC_WRITE | FILE_GENERIC_EXECUTE | DELETE;
        access.grfAccessMode = GRANT_ACCESS;
        access.grfInheritance = SUB_CONTAINERS_AND_OBJECTS_INHERIT;
        access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
        access.Trustee.TrusteeType = TRUSTEE_IS_USER;
        access.Trustee.ptstrName = static_cast<LPWSTR>(sid);
        error = SetEntriesInAclW(1, &access, old, &updated);
        if (error == ERROR_SUCCESS)
            error = SetNamedSecurityInfoW(const_cast<LPWSTR>(folder.c_str()), SE_FILE_OBJECT, DACL_SECURITY_INFORMATION, nullptr, nullptr, updated, nullptr);
    }
    if (updated) LocalFree(updated);
    if (sd) LocalFree(sd);
    LocalFree(sid);
    if (error) out.Line(L"  " + Format(T(L"授权失败（错误 %lu）。", L"Granting failed (error %lu)."), error));
    return error == ERROR_SUCCESS;
}
// Microsoft's signature on a downloaded installer: a valid Authenticode chain whose signer is Microsoft Corporation.
bool SignedByMicrosoft(const std::wstring& path) {
    WINTRUST_FILE_INFO file{sizeof(file)};
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA data{sizeof(data)};
    data.dwUIChoice = WTD_UI_NONE;
    data.fdwRevocationChecks = WTD_REVOKE_NONE;
    data.dwUnionChoice = WTD_CHOICE_FILE;
    data.pFile = &file;
    data.dwStateAction = WTD_STATEACTION_VERIFY;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    bool ok = WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data) == ERROR_SUCCESS;
    if (ok) {
        ok = false;
        if (CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(data.hWVTStateData))
            if (CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0))
                if (signer->csCertChain && signer->pasCertChain && signer->pasCertChain[0].pCert) {
                    wchar_t name[256]{};
                    CertGetNameStringW(signer->pasCertChain[0].pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name, static_cast<DWORD>(std::size(name)));
                    ok = !wcscmp(name, L"Microsoft Corporation");
                }
    }
    data.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(static_cast<HWND>(INVALID_HANDLE_VALUE), &action, &data);
    return ok;
}
// VC++ 2015-2022 x64 from Microsoft's permanent link, signature checked, run passive (it asks for UAC itself).
bool InstallVcRedist(Out& out) {
    constexpr const wchar_t* kUrl = L"https://aka.ms/vs/17/release/vc_redist.x64.exe";
    wchar_t temp[MAX_PATH]{};
    GetTempPathW(MAX_PATH, temp);
    const std::wstring path = Join(temp, L"CringeBladeZero_vc_redist.x64.exe");
    out.Line(L"  " + std::wstring(T(L"从微软官方下载：", L"Downloading from Microsoft: ")) + kUrl);
    if (options.dryRun) return true;
    DeleteFileW(path.c_str());
    const HRESULT hr = URLDownloadToFileW(nullptr, kUrl, path.c_str(), 0, nullptr);
    if (FAILED(hr) || !IsFile(path)) { out.Line(L"  " + Format(T(L"下载失败（0x%08lx），请检查网络。", L"Download failed (0x%08lx); check the network."), static_cast<unsigned long>(hr))); return false; }
    if (!SignedByMicrosoft(path)) {
        out.Line(L"  " + std::wstring(T(L"下载的文件没有有效的微软签名，已删除，未运行。", L"The download has no valid Microsoft signature; deleted, not run.")));
        DeleteFileW(path.c_str());
        return false;
    }
    out.Line(L"  " + std::wstring(T(L"签名有效（Microsoft Corporation），开始安装，请在弹出的窗口中确认…", L"Signature valid (Microsoft Corporation); installing, confirm the prompt that appears…")));
    SHELLEXECUTEINFOW info{sizeof(info)};
    info.fMask = SEE_MASK_NOCLOSEPROCESS;
    info.lpVerb = L"open";
    info.lpFile = path.c_str();
    info.lpParameters = L"/install /passive /norestart";
    info.nShow = SW_SHOWNORMAL;
    DWORD code = 1;
    if (ShellExecuteExW(&info) && info.hProcess) {
        WaitForSingleObject(info.hProcess, INFINITE);
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
    }
    DeleteFileW(path.c_str());
    if (code == 0 || code == 1638) { out.Line(L"  " + std::wstring(T(L"运行库已安装。", L"The runtime is installed."))); return true; }
    if (code == 3010) { out.Line(L"  " + std::wstring(T(L"运行库已安装，重启电脑后生效。", L"The runtime is installed; restart Windows to finish."))); return true; }
    out.Line(L"  " + Format(T(L"安装程序返回 %lu。", L"The installer returned %lu."), code));
    return false;
}
bool OpenUrl(Out& out, const wchar_t* url) {
    out.Line(L"  " + std::wstring(T(L"打开 ", L"Opening ")) + url);
    if (options.dryRun) return true;
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL)) > 32;
}
// System File Checker in an elevated window that stays open.
bool RunSfc(Out& out) {
    out.Line(L"  " + std::wstring(T(L"以管理员身份运行 sfc /scannow（约 10–30 分钟，在新窗口里进行）", L"Running sfc /scannow as administrator (10-30 minutes, in a new window)")));
    if (options.dryRun) return true;
    const std::wstring cmd = Join(System32(), L"cmd.exe");
    return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"runas", cmd.c_str(), L"/k sfc /scannow", nullptr, SW_SHOWNORMAL)) > 32;
}

// ---- System checks ----------------------------------------------------------------------------------------------
DWORD windowsBuild = 0;
void CheckWindows(Out& out) {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    if (auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"))) get(&v);
    windowsBuild = v.dwBuildNumber;
    const std::wstring key = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion";
    const std::wstring display = RegString(HKEY_LOCAL_MACHINE, key, L"DisplayVersion"), edition = RegString(HKEY_LOCAL_MACHINE, key, L"EditionID");
    const std::wstring name = v.dwMajorVersion >= 10 ? (v.dwBuildNumber >= 22000 ? L"Windows 11" : L"Windows 10") : Format(L"Windows %lu.%lu", v.dwMajorVersion, v.dwMinorVersion);
    const std::wstring text = name + (edition.empty() ? L"" : L" " + edition) + (display.empty() ? L"" : L" " + display) + Format(L" (build %lu, x64)", v.dwBuildNumber);
    if (v.dwMajorVersion >= 10 && v.dwBuildNumber >= 17763) out.Item(Level::Ok, L"Windows", text);
    else if (v.dwMajorVersion >= 10) {
        out.Item(Level::Warn, L"Windows", text);
        out.Hint(T(L"Windows 10 1809 之前的版本未测试，建议通过 Windows 更新升级。", L"Windows 10 before 1809 is untested; update through Windows Update."));
    } else {
        out.Item(Level::Fail, L"Windows", text);
        out.Hint(T(L"游戏和本 MOD 需要 Windows 10 或 11（64 位）。", L"The game and this mod need Windows 10 or 11 (64-bit)."));
    }
}

struct FileVersion { bool found = false; WORD a = 0, b = 0, c = 0, d = 0; std::wstring Text() const { return Format(L"%u.%u.%u.%u", a, b, c, d); } };
FileVersion VersionOf(const std::wstring& path) {
    FileVersion out;
    using SizeFn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    using InfoFn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    using QueryFn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    // The system version.dll, never the mod's proxy that may sit next to this program.
    static HMODULE module = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!module || !IsFile(path)) return out;
    auto size = reinterpret_cast<SizeFn>(GetProcAddress(module, "GetFileVersionInfoSizeW"));
    auto info = reinterpret_cast<InfoFn>(GetProcAddress(module, "GetFileVersionInfoW"));
    auto query = reinterpret_cast<QueryFn>(GetProcAddress(module, "VerQueryValueW"));
    if (!size || !info || !query) return out;
    DWORD handle = 0;
    const DWORD bytes = size(path.c_str(), &handle);
    if (!bytes) return out;
    std::vector<uint8_t> data(bytes);
    VS_FIXEDFILEINFO* fixed = nullptr;
    UINT length = 0;
    if (!info(path.c_str(), 0, bytes, data.data()) || !query(data.data(), L"\\", reinterpret_cast<void**>(&fixed), &length) || !fixed) return out;
    out.found = true;
    out.a = HIWORD(fixed->dwFileVersionMS); out.b = LOWORD(fixed->dwFileVersionMS);
    out.c = HIWORD(fixed->dwFileVersionLS); out.d = LOWORD(fixed->dwFileVersionLS);
    return out;
}
// VS 2022 17.10 (14.40) changed std::mutex; programs built with it crash in mutex code on an older msvcp140.dll, and a
// current UE game is built with 14.4x, so anything older is flagged.
void CheckVcRuntime(Out& out) {
    const std::wstring sys = System32();
    const FileVersion runtime = VersionOf(Join(sys, L"vcruntime140.dll")), runtime1 = VersionOf(Join(sys, L"vcruntime140_1.dll")), cpp = VersionOf(Join(sys, L"msvcp140.dll"));
    const std::wstring key = L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x64";
    std::wstring registered = RegString(HKEY_LOCAL_MACHINE, key, L"Version", KEY_WOW64_64KEY);
    if (registered.empty()) registered = RegString(HKEY_LOCAL_MACHINE, key, L"Version", KEY_WOW64_32KEY);
    const std::wstring title = T(L"VC++ 2015–2022 运行库 (x64)", L"VC++ 2015-2022 runtime (x64)");
    const std::wstring why = T(L"（游戏需要；本 MOD 静态链接，不依赖它）", L" (the game needs it; the mod links it statically)");
    auto addFix = [] { AddFix(L"vcredist", T(L"下载并安装最新 VC++ 2015–2022 x64 运行库（微软官方，校验签名）", L"Download and install the latest VC++ 2015-2022 x64 runtime (Microsoft, signature checked)"), true, InstallVcRedist); };
    if (!runtime.found || !runtime1.found || !cpp.found) {
        std::wstring missing;
        if (!runtime.found) missing += L" vcruntime140.dll";
        if (!runtime1.found) missing += L" vcruntime140_1.dll";
        if (!cpp.found) missing += L" msvcp140.dll";
        out.Item(Level::Fail, title, std::wstring(T(L"缺少", L"missing")) + missing + why);
        addFix();
        return;
    }
    const FileVersion& oldest = (cpp.a * 100 + cpp.b) < (runtime.a * 100 + runtime.b) ? cpp : runtime;
    const std::wstring text = cpp.Text() + (registered.empty() ? L"" : L" (" + registered + L")") + why;
    if (oldest.a > 14 || (oldest.a == 14 && oldest.b >= 40)) out.Item(Level::Ok, title, text);
    else {
        out.Item(Level::Warn, title, text);
        out.Hint(T(L"版本低于 14.40：用新版 VS 编译的游戏在旧运行库上可能启动即崩溃，建议更新。", L"Older than 14.40: games built with current Visual Studio can crash on it; update."));
        addFix();
    }
    const std::wstring x86 = RegString(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\VisualStudio\\14.0\\VC\\Runtimes\\x86", L"Version", KEY_WOW64_32KEY);
    out.Item(Level::Info, T(L"VC++ 2015–2022 运行库 (x86)", L"VC++ 2015-2022 runtime (x86)"),
             x86.empty() ? std::wstring(T(L"未安装（游戏和 MOD 都不需要）", L"not installed (neither the game nor the mod needs it)")) : x86);
}

void CheckDirect3D(Out& out) {
    using FactoryFn = HRESULT(WINAPI*)(REFIID, void**);
    using DeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
    HMODULE dxgi = LoadLibraryExW(L"dxgi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    HMODULE d3d12 = LoadLibraryExW(L"d3d12.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto factoryFn = dxgi ? reinterpret_cast<FactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    auto deviceFn = d3d12 ? reinterpret_cast<DeviceFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    const std::wstring title = L"Direct3D 12";
    if (!factoryFn || !deviceFn) {
        out.Item(Level::Fail, title, T(L"系统缺少 d3d12.dll / dxgi.dll", L"d3d12.dll / dxgi.dll missing"));
        out.Hint(T(L"需要 Windows 10/11 和支持 DirectX 12 的显卡。", L"Needs Windows 10/11 and a DirectX 12 graphics card."));
        return;
    }
    IDXGIFactory1* factory = nullptr;
    std::wstring good, other;
    if (SUCCEEDED(factoryFn(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) && factory) {
        IDXGIAdapter1* adapter = nullptr;
        for (UINT i = 0; SUCCEEDED(factory->EnumAdapters1(i, &adapter)) && adapter; ++i) {
            DXGI_ADAPTER_DESC1 desc{};
            adapter->GetDesc1(&desc);
            if (!(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) {
                const std::wstring name = Format(L"%s (%llu MB)", desc.Description, static_cast<unsigned long long>(desc.DedicatedVideoMemory >> 20));
                if (SUCCEEDED(deviceFn(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), nullptr))) { if (good.empty()) good = name; }
                else if (other.empty()) other = name;
            }
            adapter->Release();
        }
        factory->Release();
    }
    if (!good.empty()) out.Item(Level::Ok, title, good);
    else {
        out.Item(Level::Fail, title, other.empty() ? std::wstring(T(L"没有找到显卡", L"no graphics card found")) : other + T(L"：不支持特性级别 12_0", L": no feature level 12_0"));
        out.Hint(T(L"更新显卡驱动；集成显卡和独立显卡并存时，在显卡控制面板里让游戏使用独立显卡。", L"Update the graphics driver; with two GPUs, set the game to the dedicated one."));
        AddFix(L"driver", T(L"打开 Windows 更新检查驱动更新", L"Open Windows Update to look for driver updates"), false,
               [](Out& o) { return OpenUrl(o, L"ms-settings:windowsupdate"); });
    }
}

// Every system DLL and function the mod imports, resolved from System32 the way the game process will.
void CheckModImports(Out& out, const std::wstring& dll) {
    std::vector<uint8_t> bytes;
    std::vector<EnvCheck::ImportModule> modules;
    std::string why;
    const std::wstring title = T(L"MOD 所需系统组件", L"System parts the mod uses");
    if (!EnvCheck::ReadFileBytes(dll, bytes, 256u << 20) || !EnvCheck::ReadImports(bytes, modules, why)) {
        out.Item(Level::Warn, title, std::wstring(T(L"无法读取 ", L"cannot read ")) + dll + L" " + Wide(why));
        return;
    }
    size_t functions = 0;
    std::vector<std::wstring> missing;
    for (const EnvCheck::ImportModule& m : modules) {
        // Compression API (cabinet.dll) and some newer exports only exist from Windows 8 / 10 on; missing ones show up here.
        HMODULE module = LoadLibraryExW(Wide(m.name).c_str(), nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { missing.push_back(Wide(m.name)); continue; }
        for (const std::string& f : m.functions) { ++functions; if (!GetProcAddress(module, f.c_str())) missing.push_back(Wide(m.name + "!" + f)); }
        for (uint16_t o : m.ordinals) { ++functions; if (!GetProcAddress(module, MAKEINTRESOURCEA(o))) missing.push_back(Wide(m.name) + Format(L"!#%u", o)); }
    }
    // The proxy forwards its version.dll exports to the real System32\version.dll (its own ones start with PBZ).
    std::vector<std::string> all, exports;
    HMODULE systemVersion = LoadLibraryExW(L"version.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (EnvCheck::ReadExports(bytes, all, why))
        for (const std::string& e : all) {
            if (!e.compare(0, 3, "PBZ")) continue;
            exports.push_back(e);
            if (!systemVersion || !GetProcAddress(systemVersion, e.c_str())) missing.push_back(L"version.dll!" + Wide(e));
        }
    if (missing.empty()) {
        out.Item(Level::Ok, title, Format(T(L"%zu 个系统库、%zu 个函数全部可用，转发目标 version.dll 的 %zu 个导出齐全", L"%zu system DLLs, %zu functions all present; %zu version.dll exports to forward present"),
                                          modules.size(), functions, exports.size()));
        return;
    }
    std::wstring list;
    for (size_t i = 0; i < missing.size() && i < 8; ++i) list += (i ? L", " : L"") + missing[i];
    if (missing.size() > 8) list += Format(L" … (+%zu)", missing.size() - 8);
    out.Item(Level::Fail, title, std::wstring(T(L"缺少 ", L"missing ")) + list);
    out.Hint(T(L"Windows 版本过旧或系统文件损坏。先装好 Windows 更新，再运行系统文件修复。", L"Windows is too old or its files are damaged. Install Windows updates, then repair system files."));
    AddFix(L"sfc", T(L"运行系统文件修复 sfc /scannow（管理员，约 10–30 分钟）", L"Run System File Checker sfc /scannow (administrator, 10-30 minutes)"), true, RunSfc);
}

void CheckInput(Out& out) {
    using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
    HMODULE xinput = LoadLibraryExW(L"XInput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const wchar_t* name = L"XInput1_4.dll";
    if (!xinput) { xinput = LoadLibraryExW(L"XInput1_3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); name = L"XInput1_3.dll"; }
    auto getState = xinput ? reinterpret_cast<GetStateFn>(GetProcAddress(xinput, "XInputGetState")) : nullptr;
    int pads = 0;
    for (DWORD i = 0; getState && i < XUSER_MAX_COUNT; ++i) { XINPUT_STATE s{}; if (getState(i, &s) == ERROR_SUCCESS) ++pads; }
    // Sony pads by HID path (DualSense 0CE6, DualSense Edge 0DF2, DualShock 4 05C4 / 09CC); several collections per pad.
    static const GUID kHid = {0x4D1E55B2, 0xF16F, 0x11CF, {0x88, 0xCB, 0x00, 0x11, 0x11, 0x00, 0x00, 0x30}};
    std::set<std::wstring> sony;
    HDEVINFO set = SetupDiGetClassDevsW(&kHid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set != INVALID_HANDLE_VALUE) {
        SP_DEVICE_INTERFACE_DATA data{sizeof(data)};
        for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &kHid, i, &data); ++i) {
            BYTE buffer[1024]{};
            auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buffer);
            detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
            if (!SetupDiGetDeviceInterfaceDetailW(set, &data, detail, sizeof(buffer), nullptr, nullptr)) continue;
            const std::wstring path = Lower(detail->DevicePath);
            if (path.find(L"vid_054c") == std::wstring::npos) continue;
            if (path.find(L"pid_0ce6") == std::wstring::npos && path.find(L"pid_0df2") == std::wstring::npos &&
                path.find(L"pid_05c4") == std::wstring::npos && path.find(L"pid_09cc") == std::wstring::npos) continue;
            const size_t hash = path.rfind(L'#');
            const size_t col = path.find(L"&col");
            sony.insert(path.substr(0, std::min(hash, col)));
        }
        SetupDiDestroyDeviceInfoList(set);
    }
    if (!getState) {
        out.Item(Level::Warn, T(L"手柄 (XInput)", L"Controllers (XInput)"), T(L"系统缺少 XInput，手柄快捷键不可用", L"XInput missing; pad hotkeys will not work"));
        return;
    }
    out.Item(Level::Info, T(L"手柄", L"Controllers"),
             Format(T(L"%s 可用；已连接 Xbox 类手柄 %d 个、PlayStation 手柄 %zu 个（键盘照常可用）", L"%s present; %d Xbox-style and %zu PlayStation pads connected (keyboard always works)"),
                    name, pads, sony.size()));
}

bool FontInstalled(const wchar_t* face) {
    const std::wstring want = Lower(face);
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER})
        for (const std::wstring& v : RegValueNames(root, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Fonts"))
            if (Lower(v).find(want) != std::wstring::npos) return true;
    return false;
}
void CheckFonts(Out& out) {
    const bool yahei = FontInstalled(L"Microsoft YaHei UI"), segoe = FontInstalled(L"Segoe UI"),
               icons = FontInstalled(L"Segoe Fluent Icons") || FontInstalled(L"Segoe MDL2 Assets");
    std::wstring missing;
    if (!yahei) missing += L" Microsoft YaHei UI";
    if (!segoe) missing += L" Segoe UI";
    if (!icons) missing += L" Segoe Fluent Icons / MDL2 Assets";
    if (missing.empty()) { out.Item(Level::Ok, T(L"界面字体", L"Panel fonts"), L"Microsoft YaHei UI, Segoe UI, " + std::wstring(FontInstalled(L"Segoe Fluent Icons") ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets")); return; }
    out.Item(Level::Warn, T(L"界面字体", L"Panel fonts"), std::wstring(T(L"缺少", L"missing")) + missing);
    out.Hint(T(L"面板文字或图标会显示为方框。中文字体随“中文(简体)”语言包安装：设置 → 时间和语言 → 语言。", L"Panel text or icons may show as boxes. Chinese fonts come with the Chinese (Simplified) language pack: Settings → Time & language → Language."));
    AddFix(L"fonts", T(L"打开“语言”设置（安装中文语言包以获得中文字体）", L"Open Language settings (the Chinese language pack brings the Chinese fonts)"), false,
           [](Out& o) { return OpenUrl(o, L"ms-settings:regionlanguage"); });
}

// ---- Security and loading ---------------------------------------------------------------------------------------
// The game loads the mod only because Windows searches PBZ.exe's own folder first for version.dll. These settings
// change that, or refuse the unsigned DLL, while every file check above still looks fine.
void OpenSecurityFix() {
    AddFix(L"appbrowser", T(L"打开 Windows 安全中心 → 应用和浏览器控制（智能应用控制、Exploit protection）", L"Open Windows Security → App & browser control (Smart App Control, Exploit protection)"), false,
           [](Out& o) { return OpenUrl(o, L"windowsdefender://appbrowser"); });
}
std::wstring BlockerNames(uint32_t bits) {
    std::wstring out;
    auto add = [&](uint32_t bit, const wchar_t* chinese, const wchar_t* english) {
        if (bits & bit) out += (out.empty() ? L"" : (zh ? L"、" : L", ")) + std::wstring(T(chinese, english));
    };
    add(EnvCheck::kPreferSystem32, L"“首选系统映像”", L"\"Prefer system images\"");
    add(EnvCheck::kMicrosoftSignedOnly, L"“代码完整性保护（仅允许 Microsoft 签名的映像）”", L"\"Code integrity guard (Microsoft-signed images only)\"");
    add(EnvCheck::kNoRemoteImages, L"“阻止远程映像”", L"\"Block remote images\"");
    add(EnvCheck::kNoLowLabelImages, L"“阻止低完整性映像”", L"\"Block low integrity images\"");
    add(EnvCheck::kNonCetBlocked, L"“硬件强制堆栈保护”（阻止不兼容的模块）", L"\"Hardware-enforced stack protection\" (blocking incompatible modules)");
    return out;
}
bool OnNetworkDrive(const std::wstring& folder) {
    if (!folder.compare(0, 2, L"\\\\")) return true;
    return folder.size() >= 3 && GetDriveTypeW(folder.substr(0, 3).c_str()) == DRIVE_REMOTE;
}
void CheckLoaderPolicy(Out& out, const std::vector<Game>& games) {
    const std::wstring title = T(L"DLL 加载策略", L"DLL load policy");
    const int before = out.warns + out.fails;
    // KnownDLLs: a listed name always comes from System32. Windows does not list version.dll; "optimizers" sometimes do.
    const std::wstring sessionManager = L"SYSTEM\\CurrentControlSet\\Control\\Session Manager";
    std::wstring excluded;
    {
        wchar_t list[4096]{};
        DWORD size = sizeof(list) - 2 * sizeof(wchar_t);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, sessionManager.c_str(), L"ExcludeFromKnownDlls", RRF_RT_REG_MULTI_SZ, nullptr, list, &size) == ERROR_SUCCESS)
            for (const wchar_t* item = list; *item; item += wcslen(item) + 1) excluded += L"|" + Lower(item) + L"|";
    }
    for (const auto& [name, data] : RegStrings(HKEY_LOCAL_MACHINE, sessionManager + L"\\KnownDLLs")) {
        if (Lower(data) != L"version.dll" || excluded.find(L"|version.dll|") != std::wstring::npos) continue;
        out.Item(Level::Fail, title, T(L"version.dll 被加入了系统的 KnownDLLs，游戏只会加载系统自带的 version.dll，MOD 永远不会被加载",
                                       L"version.dll was added to the system KnownDLLs; the game always gets the system version.dll and never the mod"));
        out.Hint(std::wstring(T(L"Windows 默认没有这一项（通常是“系统优化”工具加的）：删除注册表 HKLM\\", L"Windows does not list it by default (usually an \"optimizer\" adds it): delete HKLM\\")) +
                 sessionManager + L"\\KnownDLLs → " + name + T(L"，然后重启电脑（需要管理员）。", L", then restart Windows (administrator)."));
    }
    // Exploit protection: PBZ.exe by name, by full path (UseFilter subkeys), and the system-wide default.
    const std::wstring ifeo = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Image File Execution Options\\PBZ.exe";
    auto report = [&](const std::vector<uint8_t>& value, const std::wstring& where) {
        uint32_t bits = EnvCheck::LoaderBlockers(value);
        // Remote / low-label blocking only matters for a game on a network drive or a DLL with a low label (rare).
        bool remote = false;
        for (const Game& g : games) remote = remote || OnNetworkDrive(g.folder);
        if (!remote) bits &= ~static_cast<uint32_t>(EnvCheck::kNoRemoteImages);
        bits &= ~static_cast<uint32_t>(EnvCheck::kNoLowLabelImages);
        if (!bits) return;
        out.Item(Level::Fail, title, where + T(L" 开启了 ", L" turns on ") + BlockerNames(bits) + T(L"，MOD 不会被加载", L"; the mod will not load"));
        out.Hint(T(L"Windows 安全中心 → 应用和浏览器控制 → Exploit protection 设置 → 程序设置 → PBZ.exe → 编辑，把这些项改回“替代系统设置”关闭。",
                   L"Windows Security → App & browser control → Exploit protection settings → Program settings → PBZ.exe → Edit, and turn these off."));
        OpenSecurityFix();
    };
    report(RegBytes(HKEY_LOCAL_MACHINE, ifeo, L"MitigationOptions"), T(L"Exploit protection 程序设置（PBZ.exe）", L"Exploit protection program settings (PBZ.exe)"));
    for (const std::wstring& sub : RegSubkeys(HKEY_LOCAL_MACHINE, ifeo, 0)) {
        const std::wstring path = RegString(HKEY_LOCAL_MACHINE, ifeo + L"\\" + sub, L"FilterFullPath");
        report(RegBytes(HKEY_LOCAL_MACHINE, ifeo + L"\\" + sub, L"MitigationOptions"),
               std::wstring(T(L"Exploit protection 程序设置（", L"Exploit protection program settings (")) + (path.empty() ? sub : path) + T(L"）", L")"));
    }
    report(RegBytes(HKEY_LOCAL_MACHINE, sessionManager + L"\\kernel", L"MitigationOptions"), T(L"Exploit protection 系统设置", L"Exploit protection system settings"));
    const std::wstring debugger = RegString(HKEY_LOCAL_MACHINE, ifeo, L"Debugger");
    if (!debugger.empty()) {
        out.Item(Level::Warn, title, std::wstring(T(L"启动 PBZ.exe 时会改为运行 ", L"starting PBZ.exe runs this instead: ")) + debugger + T(L"（映像劫持）", L" (Image File Execution Options)"));
        out.Hint(std::wstring(T(L"不是你装的就删除注册表 HKLM\\", L"If you did not set it up, delete HKLM\\")) + ifeo + T(L" 的 Debugger 值（需要管理员）。", L" → Debugger (administrator)."));
    }
    // Smart App Control (Windows 11): 1 on, 2 evaluation. When on it refuses unsigned DLLs without reputation, so the
    // mod; as a static import of PBZ.exe the game then fails to start or starts without it.
    const DWORD sac = RegDword(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\CI\\Policy", L"VerifiedAndReputablePolicyState", 0);
    if (sac == 1) {
        out.Item(Level::Fail, T(L"智能应用控制", L"Smart App Control"), T(L"已开启：会阻止没有数字签名的 DLL（包括本 MOD），游戏会无法启动或不加载 MOD",
                                                                     L"on: it blocks unsigned DLLs (this mod included); the game fails to start or starts without the mod"));
        out.Hint(T(L"Windows 安全中心 → 应用和浏览器控制 → 智能应用控制设置 → 关闭。注意：较早的 Windows 11 关闭后不能再自行开启，请自行权衡。",
                   L"Windows Security → App & browser control → Smart App Control settings → Off. On older Windows 11 builds it cannot be turned back on; your call."));
        OpenSecurityFix();
    } else if (sac == 2) {
        out.Item(Level::Info, T(L"智能应用控制", L"Smart App Control"), T(L"评估模式：目前不拦截；Windows 以后自动开启时会阻止本 MOD", L"evaluation: not blocking now; if Windows switches it on later it will block the mod"));
    }
    // Attack surface reduction "Block executable files unless they meet a prevalence, age, or trusted list criterion"
    // covers DLLs too: a fresh mod build never meets it. 1 block, 6 warn, 2 audit.
    const wchar_t* kRule = L"01443614-cd74-433a-b99e-2ecdc07bfc25";
    for (const wchar_t* key : {L"SOFTWARE\\Policies\\Microsoft\\Windows Defender\\Windows Defender Exploit Guard\\ASR\\Rules",
                               L"SOFTWARE\\Microsoft\\Windows Defender\\Windows Defender Exploit Guard\\ASR\\Rules"}) {
        DWORD mode = RegDword(HKEY_LOCAL_MACHINE, key, kRule, 0);
        if (!mode) mode = static_cast<DWORD>(_wtoi(RegString(HKEY_LOCAL_MACHINE, key, kRule).c_str()));
        if (mode != 1 && mode != 6) continue;
        out.Item(mode == 1 ? Level::Fail : Level::Warn, title, T(L"攻击面减少规则“阻止不常见的可执行文件”已开启，会拦截本 MOD", L"the attack surface reduction rule for uncommon executables is on and blocks the mod"));
        out.Hint(T(L"通常是单位或管理员的策略：请管理员把游戏目录加入排除项。", L"Usually an organisation policy: ask the administrator to exclude the game folder."));
        break;
    }
    if (out.warns + out.fails == before)
        out.Item(Level::Ok, title, T(L"没有阻止 MOD 加载的系统设置（KnownDLLs、Exploit protection、映像劫持、智能应用控制、ASR）",
                                     L"nothing in Windows stops the mod from loading (KnownDLLs, Exploit protection, IFEO, Smart App Control, ASR)"));
}

// Antivirus products registered with Windows Security Center, plus suites known to block "DLL hijacking" by process.
bool thirdPartyAv = false;
void CheckSecuritySoftware(Out& out) {
    std::vector<std::wstring> names;
    auto known = [&](const wchar_t* token) {
        for (const std::wstring& n : names) if (Lower(n).find(Lower(token)) != std::wstring::npos) return true;
        return false;
    };
    const std::wstring key = L"SOFTWARE\\Microsoft\\Security Center\\Provider\\Av";
    for (const std::wstring& sub : RegSubkeys(HKEY_LOCAL_MACHINE, key, KEY_WOW64_64KEY)) {
        const std::wstring name = RegString(HKEY_LOCAL_MACHINE, key + L"\\" + sub, L"DISPLAYNAME");
        if (!name.empty() && std::find(names.begin(), names.end(), name) == names.end()) names.push_back(name);
    }
    struct Suite { const wchar_t* exe; const wchar_t* chinese; const wchar_t* english; const wchar_t* tokenZh; const wchar_t* tokenEn; };
    static const Suite kSuites[] = {
        {L"360tray.exe", L"360安全卫士", L"360 Total Security", L"360", L"360"},
        {L"zhudongfangyu.exe", L"360安全卫士", L"360 Total Security", L"360", L"360"},
        {L"360sd.exe", L"360杀毒", L"360 Antivirus", L"360", L"360"},
        {L"qqpctray.exe", L"腾讯电脑管家", L"Tencent PC Manager", L"电脑管家", L"tencent"},
        {L"qqpcrtp.exe", L"腾讯电脑管家", L"Tencent PC Manager", L"电脑管家", L"tencent"},
        {L"hipstray.exe", L"火绒安全软件", L"Huorong", L"火绒", L"huorong"},
        {L"hipsdaemon.exe", L"火绒安全软件", L"Huorong", L"火绒", L"huorong"},
        {L"usysdiag.exe", L"火绒安全软件", L"Huorong", L"火绒", L"huorong"},
        {L"kxetray.exe", L"金山毒霸", L"Kingsoft Antivirus", L"金山", L"kingsoft"},
        {L"kxescore.exe", L"金山毒霸", L"Kingsoft Antivirus", L"金山", L"kingsoft"},
    };
    const std::vector<std::wstring> processes = ProcessNames();
    for (const Suite& s : kSuites)
        if (std::find(processes.begin(), processes.end(), s.exe) != processes.end() && !known(s.tokenZh) && !known(s.tokenEn))
            names.push_back(T(s.chinese, s.english));
    thirdPartyAv = false;
    std::wstring list;
    for (const std::wstring& n : names) {
        list += (list.empty() ? L"" : (zh ? L"、" : L", ")) + n;
        const std::wstring l = Lower(n);
        if (l.find(L"defender") == std::wstring::npos && l.find(L"windows") == std::wstring::npos) thirdPartyAv = true;
    }
    out.Item(Level::Info, T(L"安全软件", L"Security software"), list.empty() ? std::wstring(T(L"没有检测到", L"none detected")) : list);
    if (thirdPartyAv)
        out.Hint(T(L"部分安全软件（如 360、腾讯电脑管家）会把 version.dll 当作“DLL 劫持”拦截或隔离。MOD 没有加载时，在它的信任区 / 白名单里加入游戏目录（PBZ.exe 所在文件夹），再重新安装 MOD。",
                   L"Some security suites treat version.dll as \"DLL hijacking\" and block or quarantine it. If the mod does not load, add the game folder (where PBZ.exe is) to its trusted list, then reinstall the mod."));
}

// Refusals Windows logged in the last 30 days that name version.dll and the game (or the mod's package): Code
// Integrity 3033 (a signing-level mitigation such as Microsoft-signed-only) and 3077 (an application control policy,
// Smart App Control included), Microsoft Defender detections 1116 / 1117 and attack-surface-reduction blocks 1121.
// Both logs are readable by interactive users.
struct Refusal { Time64 when = 0; unsigned id = 0; };
// false when the log exists but cannot be read.
bool ScanEventLog(const wchar_t* channel, const wchar_t* query, const std::vector<std::wstring>& needles, std::vector<Refusal>& found) {
    EVT_HANDLE results = EvtQuery(nullptr, channel, query, EvtQueryChannelPath | EvtQueryReverseDirection);
    if (!results) {
        return GetLastError() == ERROR_EVT_CHANNEL_NOT_FOUND;  // no such log: nothing to find
    }
    EVT_HANDLE events[64];
    DWORD returned = 0;
    std::vector<wchar_t> buffer(8192);
    size_t scanned = 0;
    while (scanned < 4000 && found.size() < 16 && EvtNext(results, static_cast<DWORD>(std::size(events)), events, 3000, 0, &returned)) {
        for (DWORD i = 0; i < returned; ++i) {
            DWORD used = 0, count = 0;
            bool ok = EvtRender(nullptr, events[i], EvtRenderEventXml, static_cast<DWORD>(buffer.size() * sizeof(wchar_t)), buffer.data(), &used, &count) != 0;
            if (!ok && GetLastError() == ERROR_INSUFFICIENT_BUFFER) {
                buffer.resize(used / sizeof(wchar_t) + 16);
                ok = EvtRender(nullptr, events[i], EvtRenderEventXml, static_cast<DWORD>(buffer.size() * sizeof(wchar_t)), buffer.data(), &used, &count) != 0;
            }
            EvtClose(events[i]);
            if (!ok) continue;
            const std::wstring xml = Lower(std::wstring(buffer.data(), wcsnlen(buffer.data(), buffer.size())));
            if (xml.find(L"version.dll") == std::wstring::npos) continue;
            bool ours = false;
            for (const std::wstring& n : needles) ours = ours || xml.find(n) != std::wstring::npos;
            if (!ours) continue;
            Refusal r;
            const size_t id = xml.find(L"<eventid");
            if (id != std::wstring::npos) {
                const size_t close = xml.find(L'>', id);
                if (close != std::wstring::npos) r.id = static_cast<unsigned>(wcstoul(xml.c_str() + close + 1, nullptr, 10));
            }
            const size_t at = xml.find(L"systemtime='");
            SYSTEMTIME utc{};
            FILETIME ft{};
            if (at != std::wstring::npos &&
                swscanf_s(xml.c_str() + at + 12, L"%hu-%hu-%hut%hu:%hu:%hu", &utc.wYear, &utc.wMonth, &utc.wDay, &utc.wHour, &utc.wMinute, &utc.wSecond) == 6 &&
                SystemTimeToFileTime(&utc, &ft)) r.when = ToTime(ft);
            found.push_back(r);
        }
        scanned += returned;
    }
    EvtClose(results);
    return true;
}
void CheckSecurityEvents(Out& out, const std::vector<Game>& games) {
    const std::wstring title = T(L"拦截记录", L"Blocking events");
    // The game's folders without the drive (Code Integrity writes \Device\HarddiskVolumeN\...), the process and the package.
    std::vector<std::wstring> needles{L"pbz.exe", L"cringeblade"};
    for (const Game& g : games) {
        std::wstring tail = Lower(g.folder);
        if (tail.size() > 2 && tail[1] == L':') tail = tail.substr(2);
        needles.push_back(tail + L"\\version.dll");
    }
    constexpr const wchar_t* kWindow = L" and TimeCreated[timediff(@SystemTime) <= 2592000000]]]";
    std::vector<Refusal> integrity, defender;
    const bool integrityRead = ScanEventLog(L"Microsoft-Windows-CodeIntegrity/Operational",
                                            (std::wstring(L"*[System[(EventID=3033 or EventID=3077)") + kWindow).c_str(), needles, integrity);
    const bool defenderRead = ScanEventLog(L"Microsoft-Windows-Windows Defender/Operational",
                                           (std::wstring(L"*[System[(EventID=1116 or EventID=1117 or EventID=1121)") + kWindow).c_str(), needles, defender);
    if (!integrityRead && !defenderRead) {
        out.Item(Level::Info, title, T(L"无法读取系统日志（以管理员身份运行本工具可以读取）", L"cannot read the system logs (run this tool as administrator)"));
        return;
    }
    if (integrity.empty() && defender.empty()) {
        out.Item(Level::Ok, title, std::wstring(T(L"最近 30 天 Windows 没有拦截过 MOD（代码完整性、Microsoft Defender）", L"Windows blocked nothing of the mod in the last 30 days (Code Integrity, Microsoft Defender)")) +
                                       (thirdPartyAv ? T(L"；第三方安全软件的记录要在它自己的日志里查看", L"; third-party security software keeps its own log") : L""));
        return;
    }
    // Newest of each kind (the scan runs newest first).
    std::set<unsigned> shown;
    for (const Refusal& r : integrity) {
        if (!shown.insert(r.id).second) continue;
        if (r.id == 3033) {
            out.Item(Level::Fail, title, Format(T(L"%s 代码完整性阻止了游戏加载 version.dll（事件 3033：只允许 Microsoft 签名的模块）", L"%s Code Integrity stopped the game loading version.dll (event 3033: Microsoft-signed modules only)"), LocalText(r.when).c_str()));
            out.Hint(T(L"Exploit protection 程序设置里 PBZ.exe 开启了“代码完整性保护”，把它关闭。", L"Exploit protection has \"Code integrity guard\" on for PBZ.exe; turn it off."));
        } else {
            out.Item(Level::Fail, title, Format(T(L"%s 代码完整性阻止了游戏加载 version.dll（事件 3077：智能应用控制或应用控制策略）", L"%s Code Integrity stopped the game loading version.dll (event 3077: Smart App Control or an application control policy)"), LocalText(r.when).c_str()));
            out.Hint(T(L"关闭智能应用控制；单位电脑请联系管理员。", L"Turn off Smart App Control; on a managed PC ask the administrator."));
        }
        OpenSecurityFix();
    }
    for (const Refusal& r : defender) {
        if (!shown.insert(r.id).second) continue;
        if (r.id == 1121) out.Item(Level::Fail, title, Format(T(L"%s 攻击面减少规则拦截了 version.dll（事件 1121）", L"%s an attack surface reduction rule blocked version.dll (event 1121)"), LocalText(r.when).c_str()));
        else out.Item(Level::Fail, title, Format(T(L"%s Microsoft Defender 把 version.dll 判为威胁（事件 %u）", L"%s Microsoft Defender flagged version.dll as a threat (event %u)"), LocalText(r.when).c_str(), r.id));
        out.Hint(T(L"在“保护历史记录”里允许 / 还原它，并把游戏目录加入排除项，然后重新安装 MOD。", L"Allow / restore it under Protection history, exclude the game folder, then reinstall the mod."));
        AddFix(L"defender", T(L"打开 Windows 安全中心 → 病毒和威胁防护", L"Open Windows Security → Virus & threat protection"), false,
               [](Out& o) { return OpenUrl(o, L"windowsdefender://threat"); });
    }
}

// ---- Game checks ------------------------------------------------------------------------------------------------
// PBZDebugMenu.log next to PBZ.exe; it rotates into .previous at 1 MB, so that part comes first.
std::string ModLog(const std::wstring& folder) {
    std::vector<uint8_t> current, previous;
    const std::wstring path = Join(folder, L"PBZDebugMenu.log");
    if (!EnvCheck::ReadFileBytes(path, current, 32u << 20)) return {};
    if (EnvCheck::ReadFileBytes(path + L".previous", previous, 32u << 20)) {
        previous.push_back('\n');
        current.insert(current.begin(), previous.begin(), previous.end());
    }
    return std::string(current.begin(), current.end());
}
void CheckGame(Out& out, const Game& game, size_t index, const std::vector<Game>& games, const std::vector<Running>& running) {
    const std::wstring n = Format(L"%zu", index + 1);
    out.Section(Format(T(L"游戏 %zu：%s（%s）", L"Game %zu: %s (%s)"), index + 1, game.folder.c_str(), game.source.c_str()));

    // Build.
    const unsigned long long size = FileSize(game.exe);
    const KnownExe* match = nullptr;
    for (const KnownExe& k : kKnownExes) {
        if (k.bytes != size) continue;
        out.Line(L"  " + std::wstring(T(L"正在校验 PBZ.exe…", L"Checking PBZ.exe…")), FOREGROUND_INTENSITY);
        std::string why;
        if (EnvCheck::Sha256File(game.exe, why) == k.sha256) match = &k;
        break;
    }
    const bool steam = Lower(game.folder).find(L"\\platform\\steam") != std::wstring::npos;
    if (match) out.Item(Level::Ok, T(L"游戏版本", L"Game build"), Format(T(L"%s 版，与本 MOD 匹配", L"%s, matches this mod"), match->name));
    else {
        out.Item(Level::Fail, T(L"游戏版本", L"Game build"), T(L"与本 MOD 不匹配（只有关闭自适应扳机可用）", L"does not match this mod (only the adaptive-trigger switch works)"));
        out.Hint(steam ? T(L"游戏刚更新时请等待 MOD 更新；文件损坏时可让 Steam 验证游戏文件。", L"After a game update wait for a mod update; for damaged files let Steam verify the game.")
                       : T(L"游戏刚更新时请等待 MOD 更新；文件损坏时在游戏平台里修复或重新安装游戏。", L"After a game update wait for a mod update; for damaged files repair or reinstall the game in its store."));
        if (steam) AddFix(L"verify", T(L"让 Steam 验证游戏文件完整性", L"Let Steam verify the game files"), false, [](Out& o) { return OpenUrl(o, L"steam://validate/4115450"); });
    }

    if (OnNetworkDrive(game.folder)) {
        out.Item(Level::Warn, T(L"安装位置", L"Install location"), T(L"游戏在网络驱动器上；开启“阻止远程映像”时不会加载 MOD，建议装在本地硬盘", L"the game is on a network drive; with \"Block remote images\" the mod will not load, a local disk is better"));
    }

    // Installed DLL.
    const std::wstring target = Join(game.folder, L"version.dll");
    std::vector<uint8_t> bytes;
    const bool canInstall = !package.full.empty() && Lower(package.dir) != Lower(game.folder);
    auto installFix = [&](const std::wstring& title) {
        if (!canInstall) return;
        const std::wstring id = L"install" + n;
        AddFix(id, title, false, [game, games, id](Out& o) { return InstallFull(o, game, games, id); });
    };
    if (!IsFile(target)) {
        out.Item(Level::Warn, T(L"MOD 安装", L"Mod install"), T(L"PBZ.exe 旁边没有 version.dll，未安装", L"no version.dll next to PBZ.exe; not installed"));
        installFix(Format(T(L"安装完整版 MOD 到游戏 %s", L"Install the full mod into game %s"), n.c_str()));
    } else if (!EnvCheck::ReadFileBytes(target, bytes, 256u << 20)) {
        out.Item(Level::Fail, T(L"MOD 安装", L"Mod install"), T(L"version.dll 无法读取（被占用或没有权限）", L"version.dll cannot be read (locked or no access)"));
    } else {
        std::string why;
        const std::string sha = EnvCheck::Sha256File(target, why);
        const EnvCheck::DllKind kind = EnvCheck::ClassifyDll(bytes);
        const std::wstring shortSha = Wide(sha.substr(0, 8)) + L"…";
        if (!EnvCheck::IsX64Image(bytes)) {
            out.Item(Level::Fail, T(L"MOD 安装", L"Mod install"), T(L"version.dll 不是 64 位 DLL，游戏无法加载", L"version.dll is not a 64-bit DLL; the game cannot load it"));
            installFix(Format(T(L"用安装包里的完整版替换游戏 %s 的 version.dll", L"Replace game %s's version.dll with the package's full version"), n.c_str()));
        } else if (kind == EnvCheck::DllKind::Foreign) {
            out.Item(Level::Warn, T(L"MOD 安装", L"Mod install"), std::wstring(T(L"version.dll 属于其他 MOD（", L"version.dll belongs to another mod (")) + shortSha + L")");
            installFix(Format(T(L"备份其他 MOD 的 version.dll 并安装完整版（游戏 %s）", L"Back up the other mod's version.dll and install the full version (game %s)"), n.c_str()));
        } else if (kind == EnvCheck::DllKind::TriggersOnly) {
            out.Item(Level::Info, T(L"MOD 安装", L"Mod install"),
                     std::wstring(T(L"精简版（只关闭自适应扳机）", L"triggers-only (adaptive triggers off)")) + (sha == package.triggersSha ? T(L"，本包版本", L", this package") : L"") + L" " + shortSha);
        } else if (!package.fullSha.empty() && sha == package.fullSha) {
            out.Item(Level::Ok, T(L"MOD 安装", L"Mod install"), std::wstring(T(L"完整版，与本包一致 ", L"full version, same as this package ")) + shortSha);
        } else {
            out.Item(package.fullSha.empty() ? Level::Ok : Level::Info, T(L"MOD 安装", L"Mod install"),
                     std::wstring(package.fullSha.empty() ? T(L"完整版 ", L"full version ") : T(L"完整版，但不是本包的版本 ", L"full version, not this package's build ")) + shortSha);
            installFix(Format(T(L"更新游戏 %s 的 MOD 为本包版本", L"Update game %s's mod to this package's build"), n.c_str()));
        }
        // Unpacking a downloaded ZIP marks every file as "from the internet"; some security software refuses such DLLs.
        std::vector<uint8_t> zone;
        if (kind != EnvCheck::DllKind::Foreign && EnvCheck::ReadFileBytes(target + L":Zone.Identifier", zone, 64u << 10)) {
            const std::string text(zone.begin(), zone.end());
            const size_t at = text.find("ZoneId=");
            const int zoneId = at == std::string::npos ? 3 : std::atoi(text.c_str() + at + 7);
            if (zoneId >= 3) {
                out.Item(Level::Warn, T(L"下载标记", L"Download mark"), T(L"version.dll 带有“来自网络”的标记，部分安全软件会因此拦截它", L"version.dll is marked as downloaded from the internet; some security software blocks it for that"));
                const std::wstring id = L"unblock" + n;
                AddFix(id, Format(T(L"去掉游戏 %s 的 version.dll 上的“来自网络”标记", L"Remove the downloaded-from-the-internet mark from game %s's version.dll"), n.c_str()), false,
                       [target, games, id](Out& o) { return Unblock(o, target, games, id); });
            }
        }
    }

    // Copies of the mod where the game never loads them.
    std::vector<std::wstring> misplaced;
    std::wstring root = game.folder;
    if (Lower(Leaf(Parent(root))) == L"platform") root = Parent(Parent(root));
    std::vector<std::wstring> places{root, Join(root, L"ProjectPBZ\\Binaries\\Win64"), Join(root, L"Engine\\Binaries\\Win64")};
    for (const std::wstring& store : List(Join(root, L"platform"), L"*", true)) places.push_back(Join(Join(root, L"platform"), store));
    for (const std::wstring& dir : places) {
        if (Lower(dir) == Lower(game.folder)) continue;
        const std::wstring dll = Join(dir, L"version.dll");
        std::vector<uint8_t> b;
        if (IsFile(dll) && EnvCheck::ReadFileBytes(dll, b, 256u << 20) && EnvCheck::ClassifyDll(b) != EnvCheck::DllKind::Foreign) misplaced.push_back(dll);
    }
    for (size_t i = 0; i < misplaced.size(); ++i) {
        out.Item(Level::Warn, T(L"放错位置的 MOD", L"Misplaced mod copy"), misplaced[i] + T(L"（游戏不会加载这里的 DLL）", L" (the game never loads a DLL there)"));
        const std::wstring id = Format(L"misplaced%s_%zu", n.c_str(), i + 1), path = misplaced[i];
        AddFix(id, std::wstring(T(L"把放错位置的副本改名停用：", L"Rename the misplaced copy out of the way: ")) + path, false,
               [path, games, id](Out& o) { return MoveAside(o, path, games, id); });
    }

    // Other loaders next to PBZ.exe.
    std::wstring loaders;
    for (const wchar_t* name : {L"dxgi.dll", L"d3d11.dll", L"d3d12.dll", L"dinput8.dll", L"winmm.dll", L"xinput1_3.dll", L"xinput1_4.dll", L"dwmapi.dll",
                                L"winhttp.dll", L"wininet.dll", L"dsound.dll", L"opengl32.dll", L"bink2w64.dll.bak"})
        if (IsFile(Join(game.folder, name))) loaders += std::wstring(loaders.empty() ? L"" : L", ") + name;
    if (IsDir(Join(game.folder, L"ue4ss")) || IsFile(Join(game.folder, L"UE4SS.dll"))) loaders += std::wstring(loaders.empty() ? L"" : L", ") + L"UE4SS";
    if (IsFile(Join(game.folder, L"ReShade.ini"))) loaders += std::wstring(loaders.empty() ? L"" : L", ") + L"ReShade";
    for (const std::wstring& dll : {L"msvcp140.dll", L"vcruntime140.dll", L"vcruntime140_1.dll"}) {
        const FileVersion v = VersionOf(Join(game.folder, dll));
        if (v.found && (v.a < 14 || (v.a == 14 && v.b < 40))) {
            out.Item(Level::Warn, T(L"游戏目录里的旧运行库", L"Old runtime in the game folder"), dll + L" " + v.Text());
            out.Hint(T(L"同目录的运行库优先于系统运行库被加载；版本过旧可能导致崩溃，可改名停用后重试。", L"A runtime next to PBZ.exe is used before the system one; an old one can crash the game. Rename it and retry."));
        }
    }
    if (loaders.empty()) out.Item(Level::Ok, T(L"其他 MOD 加载器", L"Other mod loaders"), T(L"没有", L"none"));
    else {
        out.Item(Level::Info, T(L"其他 MOD 加载器", L"Other mod loaders"), loaders);
        out.Hint(T(L"一般可以共存；如果游戏启动崩溃，先把它们移走再试。", L"Usually fine together; if the game crashes at start, move them away and retry."));
    }

    // Write access: the mod saves PBZDebugMenu.ini / .log and combos\ next to PBZ.exe.
    const std::wstring probe = Join(game.folder, Format(L".envcheck_%lu.tmp", GetCurrentProcessId()));
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    const DWORD writeError = h == INVALID_HANDLE_VALUE ? GetLastError() : 0;
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!writeError) out.Item(Level::Ok, T(L"写入权限", L"Write access"), T(L"可以在 PBZ.exe 所在文件夹保存设置、日志和派生文件", L"settings, log and .combo files can be saved next to PBZ.exe"));
    else {
        out.Item(Level::Fail, T(L"写入权限", L"Write access"), Format(T(L"无法在 PBZ.exe 所在文件夹写入（错误 %lu），设置和派生无法保存", L"cannot write next to PBZ.exe (error %lu); settings and chains cannot be saved"), writeError));
        if (RegDword(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows Defender\\Windows Defender Exploit Guard\\Controlled Folder Access", L"EnableControlledFolderAccess", 0) == 1)
            out.Hint(T(L"“受控文件夹访问”已开启，可能拦截写入：Windows 安全中心 → 病毒和威胁防护 → 勒索软件防护。", L"Controlled folder access is on and may block writes: Windows Security → Ransomware protection."));
        const std::wstring id = L"grant" + n, folder = game.folder;
        AddFix(id, Format(T(L"给当前用户授予游戏 %s 的 PBZ.exe 所在文件夹修改权限（需要管理员）", L"Give the current user modify rights on game %s's PBZ.exe folder (administrator)"), n.c_str()), true,
               [folder, games, id](Out& o) { return GrantFolder(o, folder, games, id); });
    }
    std::vector<std::wstring> readOnly;
    const std::wstring combos = Join(game.folder, L"combos");
    std::vector<std::wstring> files{Join(game.folder, L"PBZDebugMenu.ini"), Join(game.folder, L"PBZDebugMenu.log")};
    for (const std::wstring& c : List(combos, L"*.combo", false)) files.push_back(Join(combos, c));
    for (const std::wstring& f : files) {
        const DWORD a = GetFileAttributesW(f.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) readOnly.push_back(f);
    }
    if (!readOnly.empty()) {
        out.Item(Level::Warn, T(L"只读文件", L"Read-only files"), Format(T(L"%zu 个设置 / 日志 / 派生文件是只读的，修改无法保存", L"%zu settings / log / chain files are read-only; changes cannot be saved"), readOnly.size()));
        AddFix(L"readonly" + n, Format(T(L"取消游戏 %s 中这些文件的只读属性", L"Clear read-only on these files of game %s"), n.c_str()), false,
               [readOnly](Out& o) { return ClearReadOnly(o, readOnly); });
    }

    // Settings and chain files.
    const std::wstring ini = Join(game.folder, L"PBZDebugMenu.ini");
    out.Item(Level::Info, T(L"设置文件", L"Settings file"),
             IsFile(ini) ? Format(T(L"PBZDebugMenu.ini，%llu 字节", L"PBZDebugMenu.ini, %llu bytes"), FileSize(ini))
                         : std::wstring(T(L"还没有（第一次进游戏时自动生成）", L"none yet (made on the first game start)")));
    const std::vector<std::wstring> comboFiles = List(combos, L"*.combo", false);
    if (comboFiles.empty()) out.Item(Level::Info, T(L"派生文件", L"Chain files"), T(L"combos 文件夹里还没有 .combo 文件", L"no .combo files in combos yet"));
    else if (windowsBuild < 9200) out.Item(Level::Info, T(L"派生文件", L"Chain files"), Format(T(L"%zu 个（这个 Windows 版本无法校验）", L"%zu (cannot be checked on this Windows)"), comboFiles.size()));
    else {
        std::vector<std::wstring> bad;
        std::wstring reasons;
        size_t profiles = 0;
        for (const std::wstring& c : comboFiles) {
            std::vector<uint8_t> b;
            ComboFile::Set set;
            std::string why;
            if (EnvCheck::ReadFileBytes(Join(combos, c), b, 16u << 20) && ComboFile::Unpack(b, set, why)) { profiles += set.size(); continue; }
            if (why.empty()) why = "unreadable";
            bad.push_back(c);
            reasons += (reasons.empty() ? L"" : L"; ") + c + L" (" + Wide(why) + L")";
        }
        if (bad.empty()) out.Item(Level::Ok, T(L"派生文件", L"Chain files"), Format(T(L"%zu 个 .combo 文件全部完好，共 %zu 套派生", L"%zu .combo files all good, %zu chain sets"), comboFiles.size(), profiles));
        else {
            out.Item(Level::Warn, T(L"派生文件", L"Chain files"), Format(T(L"%zu 个文件损坏，游戏不会读取：", L"%zu damaged, the game skips them: "), bad.size()) + reasons);
            AddFix(L"combos" + n, Format(T(L"把游戏 %s 损坏的 .combo 文件移到 combos\\broken", L"Move game %s's damaged .combo files to combos\\broken"), n.c_str()), false,
                   [combos, bad](Out& o) { return QuarantineCombos(o, combos, bad); });
        }
    }

    // The last run as the mod's log records it.
    const std::string logText = ModLog(game.folder);
    if (logText.empty()) {
        out.Item(Level::Info, T(L"上次运行（MOD 日志）", L"Last run (mod log)"), T(L"没有日志（装好后还没进过游戏）", L"no log (the game has not run with the mod yet)"));
        return;
    }
    const EnvCheck::LastRun run = EnvCheck::ParseLog(logText);
    if (!run.found) { out.Item(Level::Info, T(L"上次运行（MOD 日志）", L"Last run (mod log)"), T(L"日志里没有可识别的记录", L"nothing recognisable in the log")); return; }
    bool alive = false;
    for (const Running& r : running) if (r.pid == run.pid) alive = true;
    const std::wstring when = Format(L"%s – %s, pid %u%s", Wide(run.first).c_str(), Wide(run.last.size() > 11 ? run.last.substr(11) : run.last).c_str(), run.pid,
                                     alive ? T(L"（运行中）", L" (running)") : L"");
    using S = EnvCheck::LastRun::Start;
    switch (run.start) {
    case S::Steam:
    case S::WeGame:
        if (run.refMismatch || run.hookWaiting) {
            out.Item(Level::Warn, T(L"上次运行（MOD 日志）", L"Last run (mod log)"), when + T(L"：已加载，但部分功能自检未通过", L": loaded, but some self-checks failed"));
            out.Hint(T(L"把 PBZDebugMenu.log 发给作者。", L"Send PBZDebugMenu.log to the author."));
        } else out.Item(Level::Ok, T(L"上次运行（MOD 日志）", L"Last run (mod log)"), when + (run.banner ? T(L"：MOD 正常加载", L": the mod loaded normally") : T(L"：版本校验通过", L": build verified")));
        break;
    case S::UnsupportedBuild:
        out.Item(Level::Fail, T(L"上次运行（MOD 日志）", L"Last run (mod log)"), when + T(L"：游戏版本不受支持，MOD 只保留了关闭自适应扳机", L": unsupported game build; only the adaptive-trigger switch ran"));
        break;
    case S::UnsupportedHost:
        out.Item(Level::Warn, T(L"上次运行（MOD 日志）", L"Last run (mod log)"), when + T(L"：version.dll 被别的程序加载（不是 PBZ.exe）", L": version.dll was loaded by another program (not PBZ.exe)"));
        break;
    case S::PinFailed:
        out.Item(Level::Fail, T(L"上次运行（MOD 日志）", L"Last run (mod log)"), when + T(L"：MOD 无法常驻内存（可能被安全软件拦截）", L": the mod could not stay loaded (security software?)"));
        out.Hint(T(L"把游戏目录加入安全软件的信任列表后重试。", L"Add the game folder to your security software's exclusions and retry."));
        break;
    default:
        out.Item(Level::Info, T(L"上次运行（MOD 日志）", L"Last run (mod log)"), when);
        break;
    }
}

// ---- Did the mod load -------------------------------------------------------------------------------------------
// Every check above can pass while the game still runs without the mod (another PBZ.exe launched, a security suite or
// a Windows setting refusing the DLL). Two direct answers: the version.dll a running game actually loaded, and else
// whether the game's own last run left a trace in the mod's log.
void HintWhyNotLoaded(Out& out) {
    out.Hint(T(L"常见原因：① 启动的是另一个位置的 PBZ.exe（另一个游戏平台或另一份安装），把实际启动的 PBZ.exe 拖到 EnvCheck.exe 上再检查；",
               L"Usual causes: (1) a different PBZ.exe was started (another store or copy): drag the PBZ.exe you start onto EnvCheck.exe;"));
    out.Hint(T(L"② 安全软件拦截或隔离了 version.dll：在它的信任区加入游戏目录后重新安装 MOD；",
               L"(2) security software blocked or quarantined version.dll: trust the game folder in it and reinstall the mod;"));
    out.Hint(T(L"③ Windows 安全中心的智能应用控制或 Exploit protection（PBZ.exe 的“首选系统映像”）阻止了它。",
               L"(3) Smart App Control or Exploit protection (\"Prefer system images\" for PBZ.exe) in Windows Security stopped it."));
}
void CheckModLoaded(Out& out, const std::vector<Game>& games, const std::vector<Running>& running) {
    out.Section(T(L"MOD 是否加载", L"Did the mod load"));
    const std::wstring title = T(L"正在运行的游戏", L"Running game");
    for (const Running& r : running) {
        const std::wstring who = Format(L"pid %lu", r.pid) + (r.exe.empty() ? L"" : L", " + r.exe);
        if (!r.modulesRead) {
            out.Item(Level::Info, title, who + T(L"：无法读取它加载的模块", L": its modules cannot be read"));
            out.Hint(T(L"游戏以管理员身份运行时，右键 EnvCheck.exe →“以管理员身份运行”再检查。", L"If the game runs as administrator, right-click EnvCheck.exe → Run as administrator."));
            continue;
        }
        const std::wstring folder = Parent(r.exe), local = Join(folder, L"version.dll");
        if (!r.versionDll.empty() && Lower(Parent(r.versionDll)) == Lower(folder)) {
            std::vector<uint8_t> bytes;
            const EnvCheck::DllKind kind = EnvCheck::ReadFileBytes(r.versionDll, bytes, 256u << 20) ? EnvCheck::ClassifyDll(bytes) : EnvCheck::DllKind::Foreign;
            if (kind == EnvCheck::DllKind::Full) out.Item(Level::Ok, title, who + T(L"：MOD 已加载", L": the mod is loaded"));
            else if (kind == EnvCheck::DllKind::TriggersOnly) out.Item(Level::Ok, title, who + T(L"：精简版已加载", L": the triggers-only version is loaded"));
            else out.Item(Level::Info, title, who + T(L"：加载的是其他 MOD 的 version.dll", L": it loaded another mod's version.dll"));
        } else if (!IsFile(local)) {
            out.Item(Level::Warn, title, who + T(L"：PBZ.exe 旁边没有 version.dll，MOD 没有安装在这份游戏里", L": no version.dll next to this PBZ.exe; the mod is not installed in this copy"));
        } else {
            out.Item(Level::Fail, title, who + T(L"：加载的是 ", L": it loaded ") + (r.versionDll.empty() ? std::wstring(T(L"（没有 version.dll）", L"(no version.dll)")) : r.versionDll) +
                                             T(L"，没有加载 PBZ.exe 旁边的 MOD", L", not the mod next to PBZ.exe"));
            out.Hint(T(L"游戏启动后才装的 MOD 要重启游戏才生效；否则是 Windows 或安全软件让游戏改用了系统的 version.dll：",
                       L"A mod installed after the game started needs a game restart; otherwise Windows or security software made the game use the system version.dll:"));
            HintWhyNotLoaded(out);
        }
    }
    if (running.empty()) {
        // UE writes %LOCALAPPDATA%\ProjectPBZ\Saved\Logs\ProjectPBZ.log until the game exits and keeps the previous ones as
        // ProjectPBZ-backup-*.log (written when that run ended). A run with the mod logs from its first second, so the
        // newest run (after the previous exit, up to the last exit) must have mod log lines after the previous exit.
        const std::wstring title2 = T(L"游戏上次运行", L"Last game run");
        wchar_t localAppData[MAX_PATH]{};
        GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
        const std::wstring logs = Join(localAppData, L"ProjectPBZ\\Saved\\Logs");
        const Time64 lastExit = WrittenAt(Join(logs, L"ProjectPBZ.log"));
        Time64 previousExit = 0, installed = 0, lastModLine = 0;
        for (const std::wstring& name : List(logs, L"ProjectPBZ-backup-*.log", false)) previousExit = std::max(previousExit, WrittenAt(Join(logs, name)));
        bool full = false;
        for (const Game& g : games) {
            std::vector<uint8_t> bytes;
            const std::wstring dll = Join(g.folder, L"version.dll");
            if (!EnvCheck::ReadFileBytes(dll, bytes, 256u << 20) || EnvCheck::ClassifyDll(bytes) != EnvCheck::DllKind::Full) continue;
            full = true;
            installed = std::max(installed, PlacedAt(dll));
            for (const EnvCheck::RunSpan& span : EnvCheck::ParseRuns(ModLog(g.folder)))
                if (span.host) lastModLine = std::max(lastModLine, FromLogStamp(span.last));
        }
        if (!full) {
            // Nothing to compare: no install has the full mod (the per-game sections say so).
        } else if (!lastExit) {
            out.Item(Level::Info, title2, T(L"这个 Windows 用户还没有运行过游戏", L"this Windows user has not run the game yet"));
        } else if (lastExit < installed) {
            out.Item(Level::Info, title2, Format(T(L"%s 结束，早于 MOD 安装（%s），装好后还没有运行过", L"ended %s, before the mod was installed (%s); not run since"),
                                                 LocalText(lastExit).c_str(), LocalText(installed).c_str()));
        } else if (lastModLine > previousExit + 2 * kSecond) {
            out.Item(Level::Ok, title2, Format(T(L"%s 结束，MOD 已加载", L"ended %s with the mod loaded"), LocalText(lastExit).c_str()));
        } else {
            out.Item(Level::Fail, title2, Format(T(L"%s 结束，这次运行 MOD 没有加载（MOD 日志里没有这次运行）", L"ended %s and the mod did not load (the mod log has no trace of that run)"),
                                                 LocalText(lastExit).c_str()));
            HintWhyNotLoaded(out);
        }
        out.Hint(T(L"想直接确认：启动游戏、停在标题画面，再运行本工具，它会读取游戏实际加载的 version.dll。", L"To know for sure: start the game, stay at the title screen and run this tool again; it reads the version.dll the game actually loaded."));
    }
}

// ---- Run ----------------------------------------------------------------------------------------------------------
void FindPackage() {
    package = {};
    package.dir = options.package.empty() ? Parent(ExePath()) : FullPath(options.package);
    std::vector<uint8_t> bytes;
    std::string why;
    const std::wstring full = Join(package.dir, L"version.dll"), triggers = Join(package.dir, L"triggers_only\\version.dll");
    if (EnvCheck::ReadFileBytes(full, bytes, 256u << 20) && EnvCheck::ClassifyDll(bytes) == EnvCheck::DllKind::Full) { package.full = full; package.fullSha = EnvCheck::Sha256File(full, why); }
    if (EnvCheck::ReadFileBytes(triggers, bytes, 256u << 20) && EnvCheck::ClassifyDll(bytes) == EnvCheck::DllKind::TriggersOnly) { package.triggers = triggers; package.triggersSha = EnvCheck::Sha256File(triggers, why); }
}

std::vector<Game> RunChecks(Out& out) {
    fixes.clear();
    out.warns = out.fails = 0;
    out.Line(T(L"豪之刃 · Cringe Blade Zero 环境检查与修复", L"Cringe Blade Zero environment check and repair"), FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    out.Line(Format(T(L"构建 %hs · %s", L"Build %hs · %s"), __DATE__, Stamp().c_str()), FOREGROUND_INTENSITY);
    FindPackage();
    out.Section(T(L"系统", L"System"));
    CheckWindows(out);
    CheckVcRuntime(out);
    CheckDirect3D(out);
    std::wstring dll = package.full;
    const std::vector<Running> running = RunningGames();
    std::vector<Game> games = FindGames();
    // A running PBZ.exe is the one that matters, wherever it is (also how a copy the registry misses gets checked).
    for (const Running& r : running) {
        if (r.exe.empty()) continue;
        bool listed = false;
        for (const Game& g : games) listed = listed || Lower(g.exe) == Lower(r.exe);
        if (!listed) games.push_back({r.exe, Parent(r.exe), T(L"正在运行", L"running")});
    }
    if (dll.empty())
        for (const Game& g : games) {
            std::vector<uint8_t> b;
            const std::wstring installed = Join(g.folder, L"version.dll");
            if (EnvCheck::ReadFileBytes(installed, b, 256u << 20) && EnvCheck::ClassifyDll(b) == EnvCheck::DllKind::Full) { dll = installed; break; }
        }
    if (!dll.empty()) CheckModImports(out, dll);
    else out.Item(Level::Info, T(L"MOD 所需系统组件", L"System parts the mod uses"), T(L"没有找到 MOD 的 DLL，跳过", L"no mod DLL found; skipped"));
    CheckInput(out);
    CheckFonts(out);

    out.Section(T(L"安全与加载", L"Security and loading"));
    CheckLoaderPolicy(out, games);
    CheckSecuritySoftware(out);
    CheckSecurityEvents(out, games);

    if (games.empty()) {
        out.Section(T(L"游戏", L"Game"));
        out.Item(Level::Fail, T(L"游戏目录", L"Game folder"), T(L"没有在注册表、Steam 库和 WeGame 游戏库里找到《影之刃零》", L"Phantom Blade Zero was not found in the registry, Steam or WeGame libraries"));
        out.Hint(T(L"把 PBZ.exe 拖到 EnvCheck.exe 上，或运行 EnvCheck.exe --game \"<PBZ.exe 所在文件夹>\"。", L"Drag PBZ.exe onto EnvCheck.exe, or run EnvCheck.exe --game \"<PBZ.exe folder>\"."));
    }
    for (size_t i = 0; i < games.size(); ++i) CheckGame(out, games[i], i, games, running);
    CheckModLoaded(out, games, running);

    out.Line();
    if (!out.fails && !out.warns) out.Line(T(L"结论：一切正常，可以直接进游戏。", L"Result: everything is in order; start the game."), FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    else out.Line(Format(T(L"结论：%d 个问题、%d 个注意事项。", L"Result: %d problem(s), %d note(s)."), out.fails, out.warns),
                  out.fails ? FOREGROUND_RED | FOREGROUND_INTENSITY : FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    return games;
}

void SaveReport(Out& out) {
    std::wstring path = options.report.empty() ? Join(Parent(ExePath()), L"EnvCheck_report.txt") : options.report;
    std::string bytes = "\xEF\xBB\xBF" + Utf8(out.report);
    for (int attempt = 0; attempt < 2; ++attempt) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr);
            CloseHandle(h);
            out.Line(std::wstring(T(L"报告已保存：", L"Report saved: ")) + path, FOREGROUND_INTENSITY);
            return;
        }
        wchar_t temp[MAX_PATH]{};
        GetTempPathW(MAX_PATH, temp);
        path = Join(temp, L"CringeBladeZero_EnvCheck_report.txt");
    }
}

bool RunFix(Out& out, const Fix& fix) {
    out.Line();
    out.Line(L"» " + fix.title, FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    if (fix.confirm && !options.dryRun && options.fix.empty() && !Confirm(out, T(L"确定执行？", L"Go ahead?"))) return false;
    const bool ok = fix.run(out);
    if (options.dryRun) { out.Line(T(L"  （试运行，没有做任何改动）", L"  (dry run, nothing changed)"), FOREGROUND_INTENSITY); return ok; }
    out.Line(ok ? T(L"  ✓ 完成", L"  ✓ Done") : T(L"  ✗ 没有完成", L"  ✗ Not done"), ok ? FOREGROUND_GREEN | FOREGROUND_INTENSITY : FOREGROUND_RED | FOREGROUND_INTENSITY);
    return ok;
}

int Main(int argc, wchar_t** argv) {
    zh = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);  // URL download, ShellExecuteEx
    for (int i = 1; i < argc; ++i) {
        const std::wstring a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::wstring(argv[++i]) : std::wstring(); };
        if (a == L"--game") options.games.push_back(next());
        else if (a == L"--package") options.package = next();
        else if (a == L"--fix") options.fix = next();
        else if (a == L"--report") options.report = next();
        else if (a == L"--grant-sid") options.grantSid = next();
        else if (a == L"--lang") zh = next() == L"zh";
        else if (a == L"--dry-run") options.dryRun = true;
        else if (a == L"--no-pause") options.noPause = true;
        else if (!a.empty() && a[0] != L'-') options.games.push_back(a);  // PBZ.exe or a folder dropped on the program
    }
    (void)com;
    if (GetConsoleWindow()) SetConsoleTitleW(T(L"豪之刃 · 环境检查与修复", L"Cringe Blade Zero · environment check"));
    DWORD processes[2]{};
    const bool ownConsole = GetConsoleProcessList(processes, 2) == 1;

    Out out;
    std::vector<Game> games = RunChecks(out);

    // Non-interactive repairs: --fix all | id[,id] (also how the elevated copy runs one repair).
    if (!options.fix.empty()) {
        bool ok = true;
        size_t done = 0;
        for (const Fix& f : std::vector<Fix>(fixes)) {
            const std::wstring list = L"," + options.fix + L",";
            if (options.fix != L"all" && list.find(L"," + f.id + L",") == std::wstring::npos) continue;
            ok = RunFix(out, f) && ok;
            ++done;
        }
        if (!done) out.Line(T(L"没有需要执行的修复。", L"No repair to run."));
        SaveReport(out);
        if (ownConsole && !options.noPause) { out.Line(T(L"按任意键退出。", L"Press any key to exit.")); ReadKey(); }
        return ok ? 0 : 2;
    }

    const bool interactive = out.console && GetConsoleWindow();
    while (interactive && !fixes.empty()) {
        out.Line();
        out.Line(T(L"可以修复（按编号执行，A 全部，R 重新检查，0 / Esc 退出）：", L"Repairs (number to run, A all, R re-check, 0 / Esc quit):"), FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
        for (size_t i = 0; i < fixes.size() && i < 9; ++i) out.Line(Format(L"  %zu  %s", i + 1, fixes[i].title.c_str()));
        const wchar_t key = ReadKey();
        if (key == 27 || key == L'0' || key == L'q') break;
        std::vector<Fix> chosen;
        if (key == L'a') chosen = fixes;
        else if (key >= L'1' && key <= L'9' && static_cast<size_t>(key - L'1') < fixes.size()) chosen.push_back(fixes[key - L'1']);
        else if (key != L'r') continue;
        for (const Fix& f : chosen) RunFix(out, f);
        out.Line();
        out.Line(T(L"重新检查…", L"Checking again…"), FOREGROUND_INTENSITY);
        out.report.clear();
        games = RunChecks(out);
    }
    SaveReport(out);
    if (ownConsole && !options.noPause) { out.Line(T(L"按任意键退出。", L"Press any key to exit.")); ReadKey(); }
    return out.fails ? 2 : out.warns ? 1 : 0;
}
}

int wmain(int argc, wchar_t** argv) {
    // Every later LoadLibrary searches System32 only (a version.dll next to this program is the mod's proxy).
    // Looked up, not imported: an unpatched Windows 7 lacks it and should still get its report.
    using SetDirsFn = BOOL(WINAPI*)(DWORD);
    if (auto set = reinterpret_cast<SetDirsFn>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetDefaultDllDirectories"))) set(LOAD_LIBRARY_SEARCH_SYSTEM32);
    return Main(argc, argv);
}
