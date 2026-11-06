#include "npc_combo.h"
#include "combo_file.h"
#include "settings_store.h"
#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <iterator>
#include <map>

namespace NpcCombo {
namespace {
SRWLOCK lock_ = SRWLOCK_INIT;
std::map<std::string, Config> configs_;
std::map<std::string, float> lengths_;
volatile LONG64 generation_ = 1;
Run run_;
WeaponInfo weapon_;
Logger logger_ = nullptr;
constexpr wchar_t kSection[] = L"NpcCombo";
constexpr wchar_t kExtension[] = L".combo";
constexpr wchar_t kDefaultFile[] = L"default";
// The active .combo set (profile -> chain text) and what is known of the folder.
struct Store {
    bool ready = false, watcher = false;
    std::wstring folder, active, status;
    ComboFile::Set set;
    std::vector<std::wstring> names;
    FILETIME written{}; ULONGLONG size = 0; bool stamped = false;  // the active file as last read or written here
    uint64_t serial = 1;
} store_;

std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }
std::string Narrow(const std::wstring& text) { std::string out; for (const wchar_t c : text) out += c < 128 ? static_cast<char>(c) : '?'; return out; }
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}
void Log(const char* format, ...) {
    if (!logger_) return;
    char text[600]{};
    va_list args; va_start(args, format); _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args); va_end(args);
    logger_(text);
}
// One INI value holds at most 2047 characters, so the old ini store kept a chain in pieces: key, key#2, key#3 ...
// Only read now, to move those chains into default.combo.
constexpr size_t kPiece = 1500, kPieces = 8;
std::wstring ReadIniText(const std::wstring& key) {
    std::wstring text = SettingsStore::Get(kSection, key.c_str(), L"");
    for (size_t i = 2; i <= kPieces && text.size() >= kPiece; ++i) {
        const std::wstring piece = SettingsStore::Get(kSection, (key + L"#" + std::to_wstring(i)).c_str(), L"");
        if (piece.empty()) break;
        text += piece;
        if (piece.size() < kPiece) break;
    }
    return text;
}
std::wstring ModuleFolder() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ModuleFolder), &module)) return L".";
    wchar_t path[MAX_PATH * 2]{};
    const DWORD size = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    std::wstring text(path, size);
    const size_t slash = text.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : text.substr(0, slash);
}
std::wstring PathOf(const std::wstring& name) { return store_.folder + L"\\" + name + kExtension; }
bool Stamp(const std::wstring& path, FILETIME& written, ULONGLONG& size) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return false;
    written = data.ftLastWriteTime; size = (static_cast<ULONGLONG>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    return true;
}
std::vector<std::wstring> ScanNames() {
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW data{};
    const HANDLE find = FindFirstFileW((store_.folder + L"\\*" + kExtension).c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return names;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = data.cFileName;
        if (name.size() <= wcslen(kExtension) || _wcsicmp(name.c_str() + name.size() - wcslen(kExtension), kExtension) != 0) continue;
        names.push_back(name.substr(0, name.size() - wcslen(kExtension)));
    } while (FindNextFileW(find, &data));
    FindClose(find);
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    return names;
}
bool ReadCombo(const std::wstring& name, ComboFile::Set& set, std::string& why) {
    const HANDLE file = CreateFileW(PathOf(name).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) { why = "cannot open"; return false; }
    LARGE_INTEGER size{};
    std::vector<uint8_t> bytes;
    bool ok = GetFileSizeEx(file, &size) && size.QuadPart > 0 && size.QuadPart < static_cast<LONGLONG>(ComboFile::kMaxPayload * 2);
    if (ok) {
        bytes.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = ::ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size();
    }
    CloseHandle(file);
    if (!ok) { why = "cannot read"; return false; }
    return ComboFile::Unpack(bytes, set, why);
}
// Written beside, then moved over the old file, so a reader never sees half a file.
bool WriteCombo(const std::wstring& name, const ComboFile::Set& set) {
    const std::vector<uint8_t> bytes = ComboFile::Pack(set);
    const std::wstring path = PathOf(name), temp = path + L".tmp";
    const HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    const bool ok = ::WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &wrote, nullptr) && wrote == bytes.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok || !MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileW(temp.c_str()); return false; }
    return true;
}
void Changed() { configs_.clear(); InterlockedIncrement64(&generation_); ++store_.serial; }
constexpr wchar_t kShowToast[] = L"ShowToast";
// Lock held. The chains kept in the ini before .combo files, as default.combo (first run only: an empty folder).
void Migrate() {
    ComboFile::Set set;
    for (const std::wstring& key : SettingsStore::Keys(kSection)) {
        if (key.find(L'#') != std::wstring::npos || !_wcsicmp(key.c_str(), L"File") || !_wcsicmp(key.c_str(), L"ShowToast")) continue;
        const std::string profile = Narrow(key);
        const std::wstring text = ReadIniText(key);
        if (ComboFile::ProfileName(profile) && !text.empty()) set[profile] = text;
    }
    const bool ok = WriteCombo(kDefaultFile, set);
    Log("NPC_COMBO file migrated profiles=%zu from PBZDebugMenu.ini into default.combo ok=%d", set.size(), ok ? 1 : 0);
}
// Lock held: the folder, the first-run migration and the active file.
void Ready() {
    if (store_.ready) return;
    store_.ready = true;
    if (store_.folder.empty()) store_.folder = ModuleFolder() + L"\\combos";
    CreateDirectoryW(store_.folder.c_str(), nullptr);
    store_.names = ScanNames();
    if (store_.names.empty()) { Migrate(); store_.names = ScanNames(); }
    std::wstring want = SettingsStore::Get(kSection, L"File", kDefaultFile);
    if (std::find(store_.names.begin(), store_.names.end(), want) == store_.names.end()) want = store_.names.empty() ? kDefaultFile : store_.names.front();
    store_.active = want;
    std::string why;
    ComboFile::Set set;
    if (ReadCombo(want, set, why)) {
        store_.set = std::move(set); store_.status.clear();
        store_.stamped = Stamp(PathOf(want), store_.written, store_.size);
        Log("NPC_COMBO file load name=%s profiles=%zu ok=1", Utf8(want).c_str(), store_.set.size());
    } else {
        store_.set.clear(); store_.status = L"文件无法读取：" + Wide(why); store_.stamped = false;
        Log("NPC_COMBO file bad name=%s why=%s (skipped; edits start a new set)", Utf8(want).c_str(), why.c_str());
    }
    Changed();
}
// Lock held.
bool WriteActive() {
    const bool ok = WriteCombo(store_.active, store_.set);
    if (ok) { store_.stamped = Stamp(PathOf(store_.active), store_.written, store_.size); store_.status.clear(); }
    else { store_.status = L"保存失败：" + PathOf(store_.active); Log("NPC_COMBO file save failed name=%s", Utf8(store_.active).c_str()); }
    return ok;
}
bool ValidName(const std::wstring& name) {
    if (name.empty() || name.size() > 40 || name == L"." || name == L"..") return false;
    for (const wchar_t c : name) if (c < 32 || wcschr(L"\\/:*?\"<>|", c)) return false;
    return true;
}
DWORD WINAPI Watch(void*) {
    std::wstring folder;
    AcquireSRWLockExclusive(&lock_); Ready(); folder = store_.folder; ReleaseSRWLockExclusive(&lock_);
    const HANDLE change = FindFirstChangeNotificationW(folder.c_str(), FALSE, FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    for (;;) {
        if (change != INVALID_HANDLE_VALUE) {
            if (WaitForSingleObject(change, 1000) == WAIT_OBJECT_0) { Sleep(150); FindNextChangeNotification(change); }  // let the writer finish
        } else Sleep(1000);
        Poll();
    }
}
}
void SetLogger(Logger logger) { logger_ = logger; }
void SetFolder(const std::wstring& folder) {
    AcquireSRWLockExclusive(&lock_); store_ = {}; store_.folder = folder; Changed(); ReleaseSRWLockExclusive(&lock_);
}
std::wstring Folder() { AcquireSRWLockExclusive(&lock_); Ready(); const std::wstring folder = store_.folder; ReleaseSRWLockExclusive(&lock_); return folder; }
Files GetFiles() {
    AcquireSRWLockExclusive(&lock_); Ready();
    Files files; files.names = store_.names; files.active = store_.active; files.status = store_.status; files.serial = store_.serial;
    ReleaseSRWLockExclusive(&lock_);
    return files;
}
bool SelectFile(const std::wstring& name) {
    AcquireSRWLockExclusive(&lock_); Ready();
    ComboFile::Set set; std::string why;
    const bool ok = ValidName(name) && ReadCombo(name, set, why);
    if (ok) {
        store_.active = name; store_.set = std::move(set); store_.status.clear();
        store_.stamped = Stamp(PathOf(name), store_.written, store_.size);
        SettingsStore::Put(kSection, L"File", name);
        Changed();
        Log("NPC_COMBO file load name=%s profiles=%zu ok=1", Utf8(name).c_str(), store_.set.size());
    } else {
        store_.status = L"无法载入：" + name + L".combo (" + Wide(why.empty() ? "bad name" : why) + L")"; ++store_.serial;
        Log("NPC_COMBO file bad name=%s why=%s (skipped)", Utf8(name).c_str(), why.empty() ? "bad name" : why.c_str());
    }
    ReleaseSRWLockExclusive(&lock_);
    return ok;
}
bool SaveFile() {
    AcquireSRWLockExclusive(&lock_); Ready();
    const bool ok = WriteActive(); store_.names = ScanNames(); ++store_.serial;
    ReleaseSRWLockExclusive(&lock_);
    return ok;
}
bool SaveAs(const std::wstring& name) {
    AcquireSRWLockExclusive(&lock_); Ready();
    bool ok = ValidName(name);
    if (ok) {
        const std::wstring was = store_.active;
        store_.active = name;
        ok = WriteActive();
        if (ok) { SettingsStore::Put(kSection, L"File", name); Log("NPC_COMBO file save-as name=%s from=%s profiles=%zu", Utf8(name).c_str(), Utf8(was).c_str(), store_.set.size()); }
        else store_.active = was;
    } else store_.status = L"文件名无效：" + name;
    store_.names = ScanNames(); ++store_.serial;
    ReleaseSRWLockExclusive(&lock_);
    return ok;
}
bool Poll() {
    AcquireSRWLockExclusive(&lock_); Ready();
    bool reloaded = false;
    const std::vector<std::wstring> names = ScanNames();
    if (names != store_.names) { store_.names = names; ++store_.serial; }
    FILETIME written{}; ULONGLONG size = 0;
    if (Stamp(PathOf(store_.active), written, size) &&
        (!store_.stamped || CompareFileTime(&written, &store_.written) != 0 || size != store_.size)) {
        ComboFile::Set set; std::string why;
        store_.written = written; store_.size = size; store_.stamped = true;  // a bad file is reported once, not every second
        if (ReadCombo(store_.active, set, why)) {
            store_.set = std::move(set); store_.status.clear(); Changed(); reloaded = true;
            Log("NPC_COMBO file reload name=%s profiles=%zu (changed on disk)", Utf8(store_.active).c_str(), store_.set.size());
        } else {
            store_.status = L"文件已变化但无法读取（保留当前设置）：" + Wide(why); ++store_.serial;
            Log("NPC_COMBO file bad name=%s why=%s (changed on disk; kept the loaded set)", Utf8(store_.active).c_str(), why.c_str());
        }
    }
    ReleaseSRWLockExclusive(&lock_);
    return reloaded;
}
void StartWatcher() {
    AcquireSRWLockExclusive(&lock_);
    const bool start = !store_.watcher; store_.watcher = true;
    ReleaseSRWLockExclusive(&lock_);
    if (!start) return;
    if (const HANDLE thread = CreateThread(nullptr, 0, Watch, nullptr, 0, nullptr)) { SetThreadPriority(thread, THREAD_PRIORITY_BELOW_NORMAL); CloseHandle(thread); }
}
Config Load(const std::string& profile) {
    if (profile.empty()) return Config{};
    AcquireSRWLockExclusive(&lock_); Ready();
    const auto found = configs_.find(profile);
    Config config;
    if (found != configs_.end()) config = found->second;
    else {
        const auto text = store_.set.find(profile);
        config = Decode(text == store_.set.end() ? std::wstring(L"E1") : text->second);
        configs_.emplace(profile, config);
    }
    // 预输入: every chain has its fixed node (older files have none; a chain saved without one gets it too).
    if (PreInputNode(config) < 0) { EnsurePreInput(config); configs_[profile] = config; }
    ReleaseSRWLockExclusive(&lock_);
    return config;
}
void Save(const std::string& profile, const Config& config) {
    if (!ComboFile::ProfileName(profile)) return;
    AcquireSRWLockExclusive(&lock_); Ready();
    configs_[profile] = config; store_.set[profile] = Encode(config);
    WriteActive(); ++store_.serial;
    ReleaseSRWLockExclusive(&lock_);
    InterlockedIncrement64(&generation_);
}
std::vector<std::pair<std::string, Config>> All() {
    AcquireSRWLockExclusive(&lock_); Ready();
    std::vector<std::pair<std::string, Config>> out;
    for (const auto& [profile, text] : store_.set) {
        const auto found = configs_.find(profile);
        out.emplace_back(profile, found != configs_.end() ? found->second : Decode(text));
    }
    ReleaseSRWLockExclusive(&lock_);
    return out;
}
uint64_t Generation() { return static_cast<uint64_t>(InterlockedCompareExchange64(&generation_, 0, 0)); }
void Publish(const std::string& profile, bool running, int node, const std::wstring& note) {
    AcquireSRWLockExclusive(&lock_);
    run_.profile = profile; run_.running = running; run_.node = node; run_.note = note; ++run_.serial;
    ReleaseSRWLockExclusive(&lock_);
}
Run GetRun() { AcquireSRWLockShared(&lock_); Run copy = run_; ReleaseSRWLockShared(&lock_); return copy; }
void RecordLength(const std::string& ability, float seconds) {
    if (!(seconds > 0)) return;
    AcquireSRWLockExclusive(&lock_); lengths_[ability] = seconds; ReleaseSRWLockExclusive(&lock_);
}
float Length(const std::string& ability) {
    AcquireSRWLockShared(&lock_);
    const auto found = lengths_.find(ability); const float value = found == lengths_.end() ? 0.f : found->second;
    ReleaseSRWLockShared(&lock_); return value;
}
void RecordWeapon(const std::wstring& name, float scale, bool ok) {
    AcquireSRWLockExclusive(&lock_);
    if (weapon_.name != name || weapon_.scale != scale || weapon_.ok != ok || !weapon_.serial) { weapon_.name = name; weapon_.scale = scale; weapon_.ok = ok; ++weapon_.serial; }
    ReleaseSRWLockExclusive(&lock_);
}
WeaponInfo LastWeapon() { AcquireSRWLockShared(&lock_); WeaponInfo copy = weapon_; ReleaseSRWLockShared(&lock_); return copy; }
bool ShowToast() { return SettingsStore::GetBool(kSection, kShowToast, true); }
void SetShowToast(bool on) {
    if (on == ShowToast()) return;
    SettingsStore::PutBool(kSection, kShowToast, on); Log("NPC_COMBO show toast=%d", on ? 1 : 0);
}
}
