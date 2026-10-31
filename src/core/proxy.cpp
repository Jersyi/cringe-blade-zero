#include <windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>
#include "exports.generated.h"
#include "game_thread.h"
#include "groom_force.h"
#include "hair_controls.h"
#include "pad_triggers.h"
#include "boss_menu.h"
#include "debug_hub.h"
#include "ui_i18n.h"
#include "ui_theme.h"
#include "ui_glass.h"
#include "ui_load_banner.h"
#include "ui_hotkeys.h"
#include "ui_cursor.h"
#include "settings_store.h"
#include "grab_extras.h"
#include "gear_widget.h"
#include "hub_pause.h"
#include "boss_rush.h"
#include "boss_ai.h"
#include "boss_ai_menu.h"
#include "entry_focus.h"
#include "npc_retarget.h"
#include "npc_menu.h"
#include "air_skills.h"
#include "air_skills_menu.h"
#include "motian_grab.h"
#include "motian_grab_menu.h"
#include "weapon_presets.h"
#include "weapon_presets_menu.h"
#include "character_swap.h"
#include "ue_runtime.h"

namespace {
HMODULE self_module = nullptr;
INIT_ONCE system_once = INIT_ONCE_STATIC_INIT;
std::array<FARPROC, kExportCount> system_functions{};
volatile LONG stop_worker = 0;
volatile LONG worker_status = 0;
constexpr uintptr_t kNamePoolRva = 0x0dd11ac0;
constexpr uintptr_t kObjectArrayRva = 0x0ddb3c40;
constexpr uintptr_t kConsoleRva = 0x0e0247c0;
constexpr size_t kDebugBoolOffset = 0x7c1;
constexpr DWORD kTimestamp = 0x6ae19d7a;
constexpr DWORD kImageSize = 0x1ec54000;
constexpr DWORD kEntrypoint = 0x1b9e0a60;
constexpr uint64_t kExeBytes = 511072344;
constexpr const char* kSha256 = "7b4fcd286a350780faf0824d73aa150b21cbf1bccde883e6a96386cbe09973b4";
std::wstring log_path;
std::wstring command_path;

void Log(const char* message) {
    if (log_path.empty()) return;
    SYSTEMTIME t{}; GetLocalTime(&t);
    char line[1024]{};
    const int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "%04u-%02u-%02u %02u:%02u:%02u pid=%lu %s\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        GetCurrentProcessId(), message);
    if (n <= 0) return;
    HANDLE file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(file, &size) && size.QuadPart >= 1024 * 1024) {
        CloseHandle(file);
        const std::wstring previous = log_path + L".previous";
        if (!MoveFileExW(log_path.c_str(), previous.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        file = CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
            nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
    }
    DWORD written = 0; WriteFile(file, line, static_cast<DWORD>(n), &written, nullptr); CloseHandle(file);
}

ULONG_PTR WINAPI MissingExport() {
    SetLastError(ERROR_PROC_NOT_FOUND);
    return 0;
}

BOOL CALLBACK LoadSystem(PINIT_ONCE, PVOID, PVOID*) {
    wchar_t directory[MAX_PATH]{};
    const UINT n = GetSystemDirectoryW(directory, MAX_PATH);
    if (n > 0 && n + 13 < MAX_PATH) {
        wcscat_s(directory, L"\\version.dll");
        HMODULE real = LoadLibraryExW(directory, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (real && real != self_module) {
            for (size_t i = 0; i < kExportCount; ++i)
                system_functions[i] = GetProcAddress(real, kExportNames[i]);
        }
    }
    for (auto& fn : system_functions)
        if (!fn) fn = reinterpret_cast<FARPROC>(&MissingExport);
    return TRUE;
}

bool Read(uintptr_t address, void* dest, size_t bytes) {
    if (address < 0x10000 || address > UINTPTR_MAX - bytes) return false;
    SIZE_T n = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        dest, bytes, &n) != FALSE && n == bytes;
}
template<class T> bool ReadValue(uintptr_t p, T& out) { return Read(p, &out, sizeof(out)); }

bool IsWritable(uintptr_t address, size_t bytes) {
    if (address > UINTPTR_MAX - bytes) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(reinterpret_cast<const void*>(address), &info, sizeof(info))) return false;
    const DWORD p = info.Protect & 0xff;
    if (info.State != MEM_COMMIT || info.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    if (p != PAGE_READWRITE && p != PAGE_WRITECOPY) return false; // Never write executable pages.
    const uintptr_t end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    return address + bytes <= end;
}

bool WriteData(uintptr_t address, const void* source, size_t size) {
    if (!IsWritable(address, size)) return false;
    SIZE_T n = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(address), source, size, &n) || n != size)
        return false;
    std::array<unsigned char, 8> verify{};
    return size <= verify.size() && Read(address, verify.data(), size) && std::memcmp(source, verify.data(), size) == 0;
}

bool HashExecutable(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || static_cast<uint64_t>(size.QuadPart) != kExeBytes) { CloseHandle(file); return false; }
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool valid = false;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0) {
        std::vector<unsigned char> buffer(1024 * 1024);
        DWORD count = 0; bool ok = true;
        while (true) {
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr)) { ok = false; break; }
            if (!count) break;
            if (BCryptHashData(hash, buffer.data(), count, 0) < 0) { ok = false; break; }
        }
        std::array<unsigned char, 32> digest{};
        if (ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0) {
            char hex[65]{};
            for (size_t i = 0; i < digest.size(); ++i) sprintf_s(hex + i * 2, 3, "%02x", digest[i]);
            valid = std::strcmp(hex, kSha256) == 0;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return valid;
}

bool KnownBuild(uintptr_t base, const wchar_t* executable) {
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!ReadValue(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 1024 * 1024 ||
        !ReadValue(base + static_cast<uintptr_t>(dos.e_lfanew), nt)) return false;
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        nt.FileHeader.TimeDateStamp != kTimestamp || nt.OptionalHeader.SizeOfImage != kImageSize ||
        nt.OptionalHeader.AddressOfEntryPoint != kEntrypoint) return false;
    return HashExecutable(executable);
}

bool FNameEquals(uintptr_t base, uint32_t id, const char* wanted) {
    uint32_t block_count = 0, cursor = 0;
    if (!ReadValue(base + kNamePoolRva + 8, block_count) || !ReadValue(base + kNamePoolRva + 12, cursor) ||
        block_count >= 8192 || cursor > 131072) return false;
    const uint32_t block = id >> 16, offset = (id & 65535u) * 2;
    if (block > block_count || offset + 2 > (block == block_count ? cursor : 131072u)) return false;
    uintptr_t pointer = 0; uint16_t header = 0;
    if (!ReadValue(base + kNamePoolRva + 16 + block * 8ull, pointer) || !ReadValue(pointer + offset, header)) return false;
    const size_t length = header >> 6;
    if (!length || length != std::strlen(wanted) || length >= 128) return false;
    const size_t byte_count = length * ((header & 1) ? 2 : 1);
    if (offset + 2 + byte_count > (block == block_count ? cursor : 131072u)) return false;
    std::array<unsigned char, 256> text{};
    if (!Read(pointer + offset + 2, text.data(), byte_count)) return false;
    if (!(header & 1)) return std::memcmp(text.data(), wanted, length) == 0;
    for (size_t i = 0; i < length; ++i)
        if (text[i * 2] != static_cast<unsigned char>(wanted[i]) || text[i * 2 + 1]) return false;
    return true;
}

struct ObjectHeader {
    uintptr_t vtable; uint32_t flags; int32_t index; uintptr_t cls;
    uint32_t name; uint32_t number; uintptr_t outer;
};
static_assert(sizeof(ObjectHeader) == 40);
struct Objects {
    uint32_t max_chunks; uint32_t num_chunks; uintptr_t chunks;
    uint32_t count; uint32_t maximum; uintptr_t preallocated;
};
static_assert(sizeof(Objects) == 32);

bool ValidCdo(uintptr_t base, uintptr_t obj, uint32_t index) {
    ObjectHeader head{}, cls{}, outer{};
    return ReadValue(obj, head) && head.index == static_cast<int32_t>(index) && (head.flags & 0x10) &&
        head.number == 0 && FNameEquals(base, head.name, "Default__PBZGameSettings") &&
        ReadValue(head.cls, cls) && cls.number == 0 && FNameEquals(base, cls.name, "PBZGameSettings") &&
        ReadValue(head.outer, outer) && FNameEquals(base, outer.name, "/Script/ProjectPBZ");
}

bool GetObjects(uintptr_t base, Objects& objects) {
    if (!ReadValue(base + kObjectArrayRva + 16, objects)) return false;
    return objects.num_chunks > 0 && objects.num_chunks <= objects.max_chunks && objects.max_chunks <= 8192 &&
        objects.count <= objects.maximum && objects.maximum <= objects.max_chunks * 65536ull &&
        objects.count <= objects.num_chunks * 65536ull;
}

bool ObjectAt(const Objects& objects, uint32_t i, uintptr_t& result) {
    uintptr_t chunk = 0;
    return i < objects.count && ReadValue(objects.chunks + (i / 65536ull) * 8, chunk) &&
        ReadValue(chunk + (i % 65536ull) * 24 + 8, result) && result != 0;
}

bool FindCdo(uintptr_t base, uintptr_t& result, uint32_t& index) {
    Objects objects{};
    if (!GetObjects(base, objects)) return false;
    // Stable native registration slots in the two independently inspected runs.
    // Both are still validated by exact name, class, package and InternalIndex.
    for (uint32_t candidate : {46646u, 46636u}) {
        uintptr_t obj = 0;
        if (ObjectAt(objects, candidate, obj) && ValidCdo(base, obj, candidate)) { result = obj; index = candidate; return true; }
    }
    std::vector<uintptr_t> chunks(objects.num_chunks);
    if (!Read(objects.chunks, chunks.data(), chunks.size() * sizeof(uintptr_t))) return false;
    std::vector<unsigned char> items(65536 * 24);
    for (uint32_t c = 0; c < objects.num_chunks; ++c) {
        const uint32_t begin = c * 65536;
        if (begin >= objects.count) break;
        const uint32_t n = (objects.count - begin < 65536 ? objects.count - begin : 65536);
        if (!n || !Read(chunks[c], items.data(), n * 24ull)) continue;
        for (uint32_t j = 0; j < n; ++j) {
            uintptr_t obj = 0; std::memcpy(&obj, items.data() + j * 24ull + 8, sizeof(obj));
            if (!obj) continue;
            ObjectHeader head{};
            if (!ReadValue(obj, head) || head.index != static_cast<int32_t>(begin + j) || !(head.flags & 0x10)) continue;
            if (ValidCdo(base, obj, begin + j)) { result = obj; index = begin + j; return true; }
        }
    }
    return false;
}

bool ConsoleData(uintptr_t base, uintptr_t& data) {
    std::array<uintptr_t, 3> wrapper{};
    if (!Read(base + kConsoleRva, wrapper.data(), sizeof(wrapper))) return false;
    if (!wrapper[1] || !wrapper[2]) return false;
    std::array<int32_t, 2> values{};
    if (!Read(wrapper[2], values.data(), sizeof(values)) ||
        values[0] < 0 || values[0] > 1 || values[1] < 0 || values[1] > 1 || !IsWritable(wrapper[2], sizeof(values))) return false;
    data = wrapper[2]; return true;
}

void LogLine(const char* message) { Log(message); }

DWORD groom_faults = 0, groom_fault_code = 0;
uintptr_t groom_fault_address = 0;
int GroomFaultFilter(const EXCEPTION_POINTERS* exception) {
    groom_fault_code = exception->ExceptionRecord->ExceptionCode;
    groom_fault_address = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
bool GuardedGroomTick() {
    __try { GroomForce::OnGameThread(); return true; }
    __except (GroomFaultFilter(GetExceptionInformation())) { return false; }
}
void ReportGroomFault() {
    __try { GroomForce::OnFault(groom_fault_code, groom_fault_address, groom_faults, groom_faults >= GameThread::kMaxFaults); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Log("GROOM fault handler failed; other game-thread tasks remain enabled"); }
}
bool ReleasePlayerAppearanceForNpc() {
    const auto grab = MotianGrab::GetSnapshot();
    return !grab.active && !grab.cleanup_pending && BossRush::AllowsAppearanceChanges() && CharacterSwap::ReleaseForExternal() && GroomForce::ReleasePlayerForExternal();
}
bool ReleaseNpcForOutfit() {
    const auto grab = MotianGrab::GetSnapshot();
    return !grab.active && !grab.cleanup_pending && BossRush::AllowsAppearanceChanges() && NpcRetarget::ReleaseForExternal();
}
void DispatchGameThread() {
    MotianGrab::OnGameThreadGuarded();
    CharacterSwap::OnGameThreadGuarded();
    NpcRetarget::OnGameThreadGuarded();
    AirSkills::OnGameThreadGuarded();
    if (!BossRush::IsTransitioning()) WeaponPresets::OnGameThreadGuarded();
    BossRush::OnGameThreadGuarded();
    if (DebugHub::TakeNativeMenuKey()) {
        INPUT chord[2]{};
        chord[0].type=INPUT_KEYBOARD; chord[0].ki.wVk='O';
        chord[1].type=INPUT_KEYBOARD; chord[1].ki.wVk='O'; chord[1].ki.dwFlags=KEYEVENTF_KEYUP;
        SendInput(2, chord, sizeof(INPUT));
        Log("DEBUG_MENU keyboard O forwarded after gameplay focus");
    }
    BossAI::OnGameThreadGuarded();
    HubPause::OnGameThread(BossRush::IsTransitioning());
    if (!BossRush::IsTransitioning() && groom_faults < GameThread::kMaxFaults && !GuardedGroomTick()) { ++groom_faults; ReportGroomFault(); }
}
void DispatchFault(DWORD code, uintptr_t address, DWORD faults, bool disabled) {
    char text[220]{};
    sprintf_s(text, "GAME_THREAD dispatcher fault %lu/%lu code=0x%08lx address=%p disabled=%d", faults,
        GameThread::kMaxFaults, code, reinterpret_cast<void*>(address), disabled ? 1 : 0);
    Log(text);
}

// The engine object exists shortly after startup; install the tick hook once.
void InstallGameThread(uintptr_t base, ue::Object* engine) {
    char why[160]{}, line[256]{};
    GameThread::SetPost(&UiCursor::AfterTick);
    if (GameThread::Install(engine, base + ue::layout::kGameEngineTick, &DispatchGameThread, &DispatchFault, why, sizeof(why)))
        sprintf_s(line, "GAME_THREAD_HOOK installed engine=%p slot=%zu", static_cast<void*>(engine), ue::layout::kTickSlot);
    else
        sprintf_s(line, "GAME_THREAD_HOOK refused (%s); game-thread features unavailable", why);
    Log(line);
}

// Lets local scripts press the panel buttons without focusing the game:
// one line "force" | "outfit" | "triggers [on|off]" (adaptive triggers; off = forced off) |
// "hair" | "bounds" | "restore" | "status" | module verbs below; the file is consumed.
void PollCommand(HairControls& controls) {
    HANDLE file = CreateFileW(command_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    char text[512]{}; DWORD n = 0;
    ReadFile(file, text, sizeof(text) - 1, &n, nullptr); CloseHandle(file);
    for (char* c = text; *c; ++c) if (*c == '\r' || *c == '\n') { *c = 0; break; }
    const char* argument = "";
    if (char* space = std::strchr(text, ' ')) { *space = 0; argument = space + 1; }
    bool ok = true;
    if (!std::strcmp(text, "force")) ok = controls.ToggleForce();
    else if (!std::strcmp(text, "outfit")) ok = controls.ToggleOutfit();
    else if (!std::strcmp(text, "hair")) ok = controls.ToggleHair();
    else if (!std::strcmp(text, "bounds")) ok = controls.ToggleBounds();
    else if (!std::strcmp(text, "restore")) controls.Restore();
    else if (!std::strcmp(text, "triggers")) PadTriggers::SetForcedOff(*argument ? !std::strcmp(argument, "off") : !PadTriggers::ForcedOff());
    else if (!std::strcmp(text, "boss") && !std::strcmp(argument, "menu")) BossMenu::Show();
    else if (!std::strcmp(text, "model") && !std::strcmp(argument, "menu")) BossMenu::ShowCharacters();
    else if (!std::strcmp(text, "model")) ok = CharacterSwap::HandleCommand(text, argument);
    else if (!std::strcmp(text, "npcretarget") && !std::strcmp(argument, "menu")) NpcMenu::Show();
    else if (!std::strcmp(text, "npcretarget")) ok = NpcRetarget::HandleCommand(text, argument);
    else if (!std::strcmp(text, "air") && !std::strcmp(argument, "menu")) AirSkillsMenu::ToggleWindow();
    else if (!std::strcmp(text, "air")) ok = AirSkills::HandleCommand(text, argument);
    else if (!std::strcmp(text, "grab") && !std::strcmp(argument, "menu")) MotianGrabMenu::ToggleWindow();
    else if (!std::strcmp(text, "grab")) ok = MotianGrab::HandleCommand(text, argument);
    else if (!std::strcmp(text, "weapons") && !std::strcmp(argument, "menu")) WeaponPresetsMenu::Show();
    else if (!std::strcmp(text, "weapons")) ok = WeaponPresets::HandleCommand(text, argument);
    else if (!std::strcmp(text, "boss") || !std::strcmp(text, "rush")) ok = BossRush::HandleCommand(text, argument);
    else if (strcmp(text, "status")) ok = false;
    wchar_t status[320]{}, outfit[200]{}; GroomForce::Status(status, 320); GroomForce::OutfitStatus(outfit, 200);
    char line[768]{}; sprintf_s(line, "COMMAND %s ok=%d hair=0x%lx force=%ls outfit=%ls", text, ok ? 1 : 0, controls.Status(), status, outfit);
    Log(line);
}

DWORD WINAPI Worker(void*) {
    InterlockedExchange(&worker_status, 1);
    wchar_t module_path[32768]{}, executable[32768]{};
    if (GetModuleFileNameW(self_module, module_path, 32768)) {
        std::wstring p(module_path); const size_t slash = p.find_last_of(L"\\/");
        log_path = p.substr(0, slash + 1) + L"PBZDebugMenu.log";
        command_path = p.substr(0, slash + 1) + L"PBZDebugMenu.command";
    }
    GetModuleFileNameW(nullptr, executable, 32768);
    const wchar_t* filename = wcsrchr(executable, L'\\'); filename = filename ? filename + 1 : executable;
    if (_wcsicmp(filename, L"PBZ.exe")) { Log("UNSUPPORTED_HOST; no data writes"); InterlockedExchange(&worker_status, 0x100); return 0; }
    // Trigger hooks need no game layout, so they keep working on builds the checks below refuse.
    PadTriggers::Attach(&LogLine, (log_path.substr(0, log_path.find_last_of(L"\\/") + 1) + L"PBZDebugMenu.ini").c_str());
    const uintptr_t base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    if (!KnownBuild(base, executable)) { Log("UNSUPPORTED_BUILD; no data writes"); InterlockedExchange(&worker_status, 0x200); return 0; }
    // The proxy is installed for the life of this process. Never unload its code
    // beneath a worker thread or an in-flight system-version forwarding call.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&Worker), &pinned)) { Log("PIN_FAILED; no data writes"); InterlockedExchange(&worker_status, 0x400); return 0; }
    Log("KNOWN_BUILD sha256 verified; waiting for native settings and console data");
    InterlockedExchange(&worker_status, 3);
    ue::Attach(base);
    GroomForce::Initialize(&LogLine, &NpcRetarget::OwnsAppearance);
    const std::wstring directory = log_path.substr(0, log_path.find_last_of(L"\\/") + 1);
    UiI18n::Initialize((directory+L"PBZDebugMenu.ini").c_str());
    UiHotkeys::Initialize((directory+L"PBZDebugMenu.ini").c_str());
    SettingsStore::Initialize((directory+L"PBZDebugMenu.ini").c_str());
    HubPause::Initialize(&LogLine);
    GrabExtras::Initialize(&LogLine);
    BossAI::Initialize(&LogLine);
    BossRush::Initialize(&LogLine, directory.c_str());
    CharacterSwap::Initialize(&LogLine, &ReleaseNpcForOutfit);
    NpcRetarget::Initialize(&LogLine, &ReleasePlayerAppearanceForNpc);
    NpcMenu::Initialize(&LogLine);
    AirSkills::Initialize(&LogLine, &EntryFocus::GameplayFocused);
    AirSkillsMenu::Initialize(&LogLine);
    MotianGrab::Initialize(&LogLine, &EntryFocus::GameplayFocused);
    MotianGrabMenu::Initialize(&LogLine);
    WeaponPresets::Initialize(&LogLine, &EntryFocus::GameplayFocused);
    DebugHub::Initialize();
    UiLoadBanner::Initialize(&LogLine);
    BossMenu::Initialize();
    BossAIMenu::Initialize();
    EntryFocus::Initialize();
    HairControls controls(base, true, &BossMenu::Show);
    DebugHub::Register(DebugHub::Page::BossRush,L"Boss 连战",&BossMenu::CreatePage);
    DebugHub::Register(DebugHub::Page::BossAI,L"Boss AI",&BossAIMenu::CreatePage);
    DebugHub::Register(DebugHub::Page::Character,L"角色替换",&BossMenu::CreateCharacterPage);
    DebugHub::Register(DebugHub::Page::Npc,L"NPC 重定向",&NpcMenu::CreatePage);
    DebugHub::Register(DebugHub::Page::AirSkills,L"空中技能 / JC",&AirSkillsMenu::CreatePage);
    DebugHub::Register(DebugHub::Page::MotianGrab,L"投技",&MotianGrabMenu::CreatePage);
    DebugHub::Register(DebugHub::Page::Groom,L"毛发 / Groom",[&controls](HWND parent) { return controls.CreatePage(parent); });
    DebugHub::Register(DebugHub::Page::WeaponPresets,L"武器预设",&WeaponPresetsMenu::CreatePage);
    UiHotkeys::Bind(UiHotkeys::Action::ToggleHub, [] { DebugHub::Toggle(); });
    UiHotkeys::Bind(UiHotkeys::Action::ShowBoss, [] { DebugHub::ShowPage(DebugHub::Page::BossRush); });
    UiHotkeys::Bind(UiHotkeys::Action::ShowAi, [] { DebugHub::ShowPage(DebugHub::Page::BossAI); });
    UiHotkeys::Bind(UiHotkeys::Action::ShowCharacter, [] { DebugHub::ShowPage(DebugHub::Page::Character); });
    UiHotkeys::Bind(UiHotkeys::Action::ShowNpc, [] { DebugHub::ShowPage(DebugHub::Page::Npc); });
    UiHotkeys::Bind(UiHotkeys::Action::ShowAir, [] { DebugHub::ShowPage(DebugHub::Page::AirSkills); });
    UiHotkeys::Bind(UiHotkeys::Action::ShowGrab, [] { DebugHub::ShowPage(DebugHub::Page::MotianGrab); });
    UiHotkeys::Bind(UiHotkeys::Action::ShowGroom, [] { DebugHub::ShowPage(DebugHub::Page::Groom); });
    UiHotkeys::Bind(UiHotkeys::Action::ShowWeapons, [] { DebugHub::ShowPage(DebugHub::Page::WeaponPresets); });
    UiHotkeys::Bind(UiHotkeys::Action::HairMode, [&controls] { controls.ToggleHair(); });
    UiHotkeys::Bind(UiHotkeys::Action::GpuBounds, [&controls] { controls.ToggleBounds(); });
    UiHotkeys::Bind(UiHotkeys::Action::ForceGroom, [&controls] { controls.ToggleForce(); });
    UiHotkeys::Bind(UiHotkeys::Action::OutfitUnlock, [&controls] { controls.ToggleOutfit(); });
    UiHotkeys::Bind(UiHotkeys::Action::AdaptiveTriggers, [&controls] { controls.ToggleTriggers(); });
    UiHotkeys::Bind(UiHotkeys::Action::GrabRelease, [] {
        if (DebugHub::ObscuresGameplay() || !EntryFocus::GameplayFocused()) return;
        MotianGrab::RequestTrigger();
    });
    GearWidget::Initialize(&LogLine);
    UiHotkeys::Bind(UiHotkeys::Action::GearUp, [] { GearWidget::Nudge(1); });
    UiHotkeys::Bind(UiHotkeys::Action::GearDown, [] { GearWidget::Nudge(-1); });
    UiHotkeys::Bind(UiHotkeys::Action::WeaponSwap, [] {
        if (DebugHub::ObscuresGameplay() || !EntryFocus::GameplayFocused()) return;
        const auto state = WeaponPresets::GetSnapshot();
        if (!state.enabled || !state.ready || state.busy || !state.can_save) return;
        WeaponPresets::RequestSwap();
    });
    DebugHub::Register(DebugHub::Page::Hotkeys, L"快捷键", &UiHotkeys::CreatePage);
    Log("DEBUG_HUB registered 9 embedded pages; hotkeys toggle the host; native O retained");
    bool hook_attempted = false,success_banner_shown=false;
    uintptr_t cdo = 0; uint32_t index = 0; bool announced = false;
    const ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    ULONGLONG next_settings = 0;
    while (!InterlockedCompareExchange(&stop_worker, 0, 0)) {
        UiHotkeys::Tick();
        DebugHub::Tick();
        UiLoadBanner::Tick();
        GearWidget::Tick();
        BossMenu::Tick();
        BossAIMenu::Tick();
        NpcMenu::Tick();
        AirSkillsMenu::Tick();
        MotianGrabMenu::Tick();
        WeaponPresets::WorkerTick();
        WeaponPresetsMenu::Tick();
        EntryFocus::Poll();
        controls.Tick();
        if (GetTickCount64() < next_settings) { Sleep(DebugHub::Visible()||DebugHub::Animating()||UiLoadBanner::Active()||GearWidget::Active()?16:25); continue; }
        next_settings = GetTickCount64() + 1000;
        if (!hook_attempted) if (ue::Object* engine = ue::FindEngine()) { InstallGameThread(base, engine); hook_attempted = true; }
        PollCommand(controls);
        if (!cdo || !ValidCdo(base, cdo, index)) { cdo = 0; FindCdo(base, cdo, index); }
        uintptr_t console = 0;
        if (cdo && ConsoleData(base, console)) {
            unsigned char enabled = 0; std::array<int32_t, 2> values{};
            if (!ReadValue(cdo + kDebugBoolOffset, enabled) || enabled > 1 || !Read(console, values.data(), sizeof(values))) {
                Log("LAYOUT_MISMATCH; worker stopped without further writes"); InterlockedExchange(&worker_status, 0x800); return 0;
            }
            bool wrote = false;
            const std::array<int32_t, 2> ones{1, 1};
            if (values != ones) { if (!WriteData(console, ones.data(), sizeof(ones))) { Log("CONSOLE_WRITE_FAILED; worker stopped"); InterlockedExchange(&worker_status, 0x1000); return 0; } wrote = true; }
            if (!enabled) {
                const unsigned char one = 1;
                if (!WriteData(cdo + kDebugBoolOffset, &one, 1)) {
                    // Restore the console values if this two-switch update was incomplete.
                    if (wrote) WriteData(console, values.data(), sizeof(values));
                    Log("SETTINGS_WRITE_FAILED; worker stopped"); InterlockedExchange(&worker_status, 0x2000); return 0;
                }
                wrote = true;
            }
            InterlockedExchange(&worker_status, 31);
            if (!success_banner_shown&&GameThread::Installed()) {
                UiLoadBanner::ShowSuccess(); success_banner_shown=true;
                Log("UI_LOAD_BANNER success; main tool window remains hidden until user request");
            }
            if (!announced || wrote) {
                char line[256]{}; sprintf_s(line, "ENABLED cdo=%p slot=%u bool=%p console=%p values=1/1", reinterpret_cast<void*>(cdo), index,
                    reinterpret_cast<void*>(cdo + kDebugBoolOffset), reinterpret_cast<void*>(console)); Log(line); announced = true;
            }
        } else if (!announced && GetTickCount64() > deadline) {
            Log("INITIALIZATION_TIMEOUT; no further attempts"); InterlockedExchange(&worker_status, 0x4000); return 0;
        }
        Sleep(DebugHub::Visible()||DebugHub::Animating()||UiLoadBanner::Active()||GearWidget::Active()?16:25);
    }
    WeaponPresets::Shutdown(); WeaponPresetsMenu::Shutdown();
    GearWidget::Shutdown(); DebugHub::Shutdown();
    UiLoadBanner::Shutdown(); UiGlass::Shutdown(); UiI18n::Shutdown(); UiTheme::Shutdown();
    return 0;
}
}

extern "C" FARPROC ResolveVersionExport(unsigned int index) {
    const DWORD saved_error = GetLastError();
    InitOnceExecuteOnce(&system_once, LoadSystem, nullptr, nullptr);
    FARPROC answer = index < system_functions.size() ? system_functions[index] : reinterpret_cast<FARPROC>(&MissingExport);
    SetLastError(saved_error);
    return answer;
}
extern "C" DWORD WINAPI PBZDebugMenuStatus() { return static_cast<DWORD>(InterlockedCompareExchange(&worker_status, 0, 0)); }

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        self_module = module;
        // DualSense trigger rewrite (pad_triggers.h): import-table entries of PBZ.exe only, before its code runs.
        wchar_t host[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, host, MAX_PATH)) {
            const wchar_t* name = wcsrchr(host, L'\\');
            if (!_wcsicmp(name ? name + 1 : host, L"PBZ.exe")) PadTriggers::Install();
        }
        // Thread execution begins after loader initialization. Heavy work and
        // LoadLibrary are deliberately absent from this loader-lock callback.
        HANDLE thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
        if (thread) CloseHandle(thread);
    } else if (reason == DLL_PROCESS_DETACH) {
        InterlockedExchange(&stop_worker, 1); // Never wait for another thread here.
    }
    return TRUE;
}
