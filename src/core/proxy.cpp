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
#include "game_pad_gate.h"
#include "boss_menu.h"
#include "debug_hub.h"
#include "ui_i18n.h"
#include "ui_theme.h"
#include "ui_glass.h"
#include "ui_load_banner.h"
#include "ui_hotkeys.h"
#include "ui_cursor.h"
#include "ui_host.h"
#include "settings_store.h"
#include "grab_extras.h"
#include "gear_widget.h"
#include "skill_toast.h"
#include "hub_pause.h"
#include "title_control.h"
#include "boss_rush.h"
#include "boss_ai.h"
#include "boss_ai_menu.h"
#include "test_dummy.h"
#include "boss_assist.h"
#include "move_preview.h"
#include "entry_focus.h"
#include "npc_retarget.h"
#include "npc_menu.h"
#include "combo_menu.h"
#include "net_steam.h"
#include "net_puppet.h"
#include "net_page.h"
#include "air_skills.h"
#include "air_skills_menu.h"
#include "motian_grab.h"
#include "motian_grab_menu.h"
#include "weapon_presets.h"
#include "weapon_presets_menu.h"
#include "debug_supplement.h"
#include "supplement_menu.h"
#include "donate_page.h"
#include "character_swap.h"
#include "ue_runtime.h"
#include "game_build.h"

namespace {
HMODULE self_module = nullptr;
INIT_ONCE system_once = INIT_ONCE_STATIC_INIT;
std::array<FARPROC, kExportCount> system_functions{};
volatile LONG stop_worker = 0;
volatile LONG worker_status = 0;
constexpr DWORD kTimestamp = 0x6ae7ae3f;
constexpr DWORD kImageSize = 0x1e0bb000;
constexpr DWORD kEntrypoint = 0x18db0d40;
constexpr uint64_t kExeBytes = 498913272;
constexpr const char* kSha256 = "4415933ce33476f14c837abb61a1b60af8b78cd81783896501913a7aac69e423";
// WeGame: the same changelist without Denuvo (game_build.h); update_port.py build wegame rewrites these.
constexpr DWORD kWeGameTimestamp = 0x6ae7ae5b;
constexpr DWORD kWeGameImageSize = 0x1b948000;
constexpr DWORD kWeGameEntrypoint = 0x120f71e0;
constexpr uint64_t kWeGameExeBytes = 457545208;
constexpr const char* kWeGameSha256 = "32f8ffaf347c7f458e1607d6655135e5f400a81a3c656934ec5121619c1f9017";
struct KnownExe { GameBuild::Id id; DWORD timestamp, image_size, entrypoint; uint64_t bytes; const char* sha256; };
constexpr KnownExe kKnownExes[] = {
    {GameBuild::Id::Steam, kTimestamp, kImageSize, kEntrypoint, kExeBytes, kSha256},
    {GameBuild::Id::WeGame, kWeGameTimestamp, kWeGameImageSize, kWeGameEntrypoint, kWeGameExeBytes, kWeGameSha256},
};
std::wstring log_path;
std::wstring command_path;

void Log(const char* message) {
    if (log_path.empty()) return;
    SYSTEMTIME t{}; GetLocalTime(&t);
    char line[1024]{};
    int n = _snprintf_s(line, sizeof(line), _TRUNCATE,
        "%04u-%02u-%02u %02u:%02u:%02u pid=%lu %s\r\n",
        t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond,
        GetCurrentProcessId(), message);
    // _TRUNCATE returns -1 on a cut line: keep the cut text with its line end instead of dropping the line.
    if (n < 0) { n = static_cast<int>(sizeof(line)) - 1; line[n - 2] = '\r'; line[n - 1] = '\n'; }
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
        wcsncat_s(directory, L"\\version.dll", _TRUNCATE);
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

bool HashExecutable(const wchar_t* path, const KnownExe& exe) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || static_cast<uint64_t>(size.QuadPart) != exe.bytes) { CloseHandle(file); return false; }
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
            for (size_t i = 0; i < digest.size(); ++i) _snprintf_s(hex + i * 2, 3, _TRUNCATE, "%02x", digest[i]);
            valid = std::strcmp(hex, exe.sha256) == 0;
        }
    }
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return valid;
}

// The build whose header facts the running image has, once the file's size and SHA-256 confirm it; else nullptr.
const KnownExe* KnownBuild(uintptr_t base, const wchar_t* executable) {
    IMAGE_DOS_HEADER dos{}; IMAGE_NT_HEADERS64 nt{};
    if (!ReadValue(base, dos) || dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 || dos.e_lfanew > 1024 * 1024 ||
        !ReadValue(base + static_cast<uintptr_t>(dos.e_lfanew), nt)) return nullptr;
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) return nullptr;
    for (const KnownExe& exe : kKnownExes)
        if (nt.FileHeader.TimeDateStamp == exe.timestamp && nt.OptionalHeader.SizeOfImage == exe.image_size &&
            nt.OptionalHeader.AddressOfEntryPoint == exe.entrypoint) return HashExecutable(executable, exe) ? &exe : nullptr;
    return nullptr;
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
bool ReleasePlayerAppearanceForNpc(bool chain, const char** why) {
    const auto grab = MotianGrab::GetSnapshot();
    const char* gate = nullptr;
    auto fail = [&](const char* reason) { if (why) *why = reason; return false; };
    // A grab whose hero side is over (a cut, or its target still getting up / returning to its baseline) leaves the
    // hero free (the chain's next look).
    if ((grab.active || grab.cleanup_pending) && !grab.holding && !grab.hero_free) return fail(grab.active ? "grab-active" : "grab-cleanup");
    if (chain ? !BossRush::AllowsChainLook(&gate) : !BossRush::AllowsAppearanceChanges()) return fail(gate ? gate : "boss-rush (loading / transition / player dead)");
    if (!CharacterSwap::ReleaseForExternal()) return fail("character-swap (another outfit preview holds the look)");
    if (!GroomForce::ReleasePlayerForExternal()) return fail("force-groom");
    if (why) *why = gate;  // a note on success ("dead-flag-ignored") or nullptr
    return true;
}
bool ReleaseNpcForOutfit() {
    const auto grab = MotianGrab::GetSnapshot();
    return !grab.active && !grab.cleanup_pending && BossRush::AllowsAppearanceChanges() && NpcRetarget::ReleaseForExternal();
}
void DispatchGameThread() {
    ue::MarkGameThread();
    MotianGrab::OnGameThreadGuarded();
    CharacterSwap::OnGameThreadGuarded();
    NpcRetarget::OnGameThreadGuarded();
    AirSkills::OnGameThreadGuarded();
    if (!BossRush::IsTransitioning()) WeaponPresets::OnGameThreadGuarded();
    BossRush::OnGameThreadGuarded();
    BossAI::OnGameThreadGuarded();
    if (!BossRush::IsTransitioning()) TestDummy::OnGameThreadGuarded();  // 测试木桩 (招式派生 page)
    if (!BossRush::IsTransitioning()) BossAssist::OnGameThreadGuarded();  // BOSS 援护
    if (!BossRush::IsTransitioning()) MovePreview::OnGameThreadGuarded();  // 招式预览 (招式派生 page)
    NetSteam::OnGameThreadGuarded();                                          // 联机测试: Steam room and messages
    NetPuppet::OnGameThreadGuarded(BossRush::IsTransitioning());              // 联机测试: the puppet
    DebugSupplement::OnGameThreadGuarded();
    // A 招式预览 needs the world running; it holds the hero and the enemies still itself (MovePreview::HoldsGameplay).
    HubPause::OnGameThread(BossRush::IsTransitioning() || MovePreview::NeedsWorld() || DebugSupplement::NeedsWorld());
    TitleControl::OnGameThread();
    if (!BossRush::IsTransitioning() && groom_faults < GameThread::kMaxFaults && !GuardedGroomTick()) { ++groom_faults; ReportGroomFault(); }
}
void DispatchFault(DWORD code, uintptr_t address, DWORD faults, bool disabled) {
    char text[220]{};
    _snprintf_s(text, sizeof(text), _TRUNCATE, "GAME_THREAD dispatcher fault %lu/%lu code=0x%08lx address=%p disabled=%d", faults,
        GameThread::kMaxFaults, code, reinterpret_cast<void*>(address), disabled ? 1 : 0);
    Log(text);
}

// The engine object exists shortly after startup; install the tick hook once.
void InstallGameThread(uintptr_t base, ue::Object* engine) {
    char why[160]{}, line[256]{};
    GameThread::SetPost(&UiCursor::AfterTick);
    if (GameThread::Install(engine, base + ue::layout::kGameEngineTick, &DispatchGameThread, &DispatchFault, why, sizeof(why)))
        _snprintf_s(line, sizeof(line), _TRUNCATE, "GAME_THREAD_HOOK installed engine=%p slot=%zu", static_cast<void*>(engine), ue::layout::kTickSlot);
    else
        _snprintf_s(line, sizeof(line), _TRUNCATE, "GAME_THREAD_HOOK refused (%s); game-thread features unavailable", why);
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
    else if (!std::strcmp(text, "supplement") && !std::strcmp(argument, "menu")) ok = DebugSupplement::Present() && (DebugHub::ShowPage(DebugHub::Page::Supplement), true);
    else if (!std::strcmp(text, "supplement")) ok = DebugSupplement::HandleCommand(text, argument);
    else if (!std::strcmp(text, "boss") || !std::strcmp(text, "rush")) ok = BossRush::HandleCommand(text, argument);
    else if (!std::strcmp(text, "title")) ok = TitleControl::HandleCommand(text, argument);
    else if (!std::strcmp(text, "combo")) ok = ComboMenu::HandleCommand(argument);  // tests: the 招式派生 page
    else if (!std::strcmp(text, "net")) ok = NetPage::HandleCommand(argument);        // 联机测试
    else if (strcmp(text, "status")) ok = false;
    wchar_t status[320]{}, outfit[200]{}; GroomForce::Status(status, 320); GroomForce::OutfitStatus(outfit, 200);
    char line[768]{}; _snprintf_s(line, sizeof(line), _TRUNCATE, "COMMAND %s ok=%d hair=0x%lx force=%ls outfit=%ls", text, ok ? 1 : 0, controls.Status(), status, outfit);
    Log(line);
}

// A game build the checks refuse (usually a game update): nothing touches its memory, but once the game window is up
// the player sees why the tools are missing. The trigger hooks attached before the check keep working.
// The loops pump this thread's messages: the game's thread sends to the banner (activation, its owner moving or being
// destroyed) and would block until this thread takes them.
void PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
void NotifyUnsupported(const std::wstring& directory) {
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&NotifyUnsupported), &pinned)) return;
    UiI18n::Initialize((directory + L"PBZDebugMenu.ini").c_str());
    UiHost::GuardActivation(&LogLine);
    UiLoadBanner::Initialize(&LogLine);
    const ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    while (!InterlockedCompareExchange(&stop_worker, 0, 0) && GetTickCount64() < deadline) {
        PumpMessages(); UiHost::Tick();
        if (UiHost::InGame()) { UiLoadBanner::ShowUnsupported(); break; }
        Sleep(500);
    }
    while (!InterlockedCompareExchange(&stop_worker, 0, 0) && (UiLoadBanner::Pending() || UiLoadBanner::Active())) {
        PumpMessages(); UiLoadBanner::Tick();
        Sleep(UiLoadBanner::Active() ? 16 : 100);
    }
    UiLoadBanner::Shutdown(); UiI18n::Shutdown();
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
    const KnownExe* build = KnownBuild(base, executable);
    if (!build) {
        Log("UNSUPPORTED_BUILD; no data writes");
        InterlockedExchange(&worker_status, 0x200);
        NotifyUnsupported(log_path.substr(0, log_path.find_last_of(L"\\/") + 1));
        return 0;
    }
    // The proxy is installed for the life of this process. Never unload its code
    // beneath a worker thread or an in-flight system-version forwarding call.
    HMODULE pinned = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
        reinterpret_cast<LPCWSTR>(&Worker), &pinned)) { Log("PIN_FAILED; no data writes"); InterlockedExchange(&worker_status, 0x400); return 0; }
    // Before any feature reads the game: self-checks of protector-wrapped entries take this build's bytes.
    GameBuild::Set(build->id);
    Log(build->id == GameBuild::Id::Steam ? "KNOWN_BUILD sha256 verified" : "KNOWN_BUILD sha256 verified build=WeGame");
    InterlockedExchange(&worker_status, 3);
    ue::Attach(base);
    GroomForce::Initialize(&LogLine, &NpcRetarget::OwnsAppearance);
    const std::wstring directory = log_path.substr(0, log_path.find_last_of(L"\\/") + 1);
    UiI18n::Initialize((directory+L"PBZDebugMenu.ini").c_str());
    UiHotkeys::SetLogger(&LogLine);
    GamePadGate::SetLogger(&LogLine);
    UiHotkeys::Initialize((directory+L"PBZDebugMenu.ini").c_str());
    SettingsStore::Initialize((directory+L"PBZDebugMenu.ini").c_str());
    HubPause::Initialize(&LogLine);
    TitleControl::Initialize(&LogLine);
    GrabExtras::Initialize(&LogLine);
    // With an NPC skin on, a grab's boss weapon follows the NPC display's hands.
    GrabExtras::SetSkinDisplay(&NpcRetarget::GrabDisplay);
    BossAI::Initialize(&LogLine);
    TestDummy::Initialize(&LogLine);
    BossAssist::Initialize(&LogLine);
    MovePreview::Initialize(&LogLine);
    NetSteam::Initialize(&LogLine);
    NetPuppet::Initialize(&LogLine);
    BossRush::Initialize(&LogLine, directory.c_str());
    CharacterSwap::Initialize(&LogLine, &ReleaseNpcForOutfit);
    NpcRetarget::Initialize(&LogLine, &ReleasePlayerAppearanceForNpc);
    NpcMenu::Initialize(&LogLine);
    AirSkills::Initialize(&LogLine, &EntryFocus::GameplayFocused);
    AirSkillsMenu::Initialize(&LogLine);
    MotianGrab::Initialize(&LogLine, &EntryFocus::GameplayFocused);
    MotianGrabMenu::Initialize(&LogLine);
    WeaponPresets::Initialize(&LogLine, &EntryFocus::GameplayFocused);
    // Optional: only acts when the supplement container sits under the game's Paks folder.
    DebugSupplement::Initialize(&LogLine, std::wstring(executable, static_cast<size_t>(filename - executable)).c_str());
    UiHost::GuardActivation(&LogLine);
    UiCursor::SetLogger(&LogLine);
    ComboMenu::SetLogger(&LogLine);
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
    DebugHub::Register(DebugHub::Page::Combo,L"招式派生",&ComboMenu::CreatePage);
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
    UiHotkeys::Bind(UiHotkeys::Action::NpcSkill, [] {
        if (DebugHub::ObscuresGameplay() || !EntryFocus::GameplayFocused()) return;
        NpcRetarget::RequestSkill();
    });
    UiHotkeys::Bind(UiHotkeys::Action::WeaponSwap, [] {
        if (DebugHub::ObscuresGameplay() || !EntryFocus::GameplayFocused()) return;
        const auto state = WeaponPresets::GetSnapshot();
        if (!state.enabled || !state.ready || state.busy || !state.can_save) return;
        WeaponPresets::RequestSwap();
    });
    DebugHub::Register(DebugHub::Page::Hotkeys, L"快捷键", &UiHotkeys::CreatePage);
    // The supplement page exists only with the supplement container; the hub lists registered pages only.
    if (DebugSupplement::Present()) DebugHub::Register(DebugHub::Page::Supplement, L"调试补充", &SupplementMenu::CreatePage);
    DebugHub::Register(DebugHub::Page::Online, L"联机测试", &NetPage::CreatePage);
    DebugHub::Register(DebugHub::Page::Donate, L"Donate", &DonatePage::CreatePage);
    Log(DebugSupplement::Present() ? "DEBUG_HUB registered 13 embedded pages (supplement present); hotkeys toggle the host"
                                   : "DEBUG_HUB registered 12 embedded pages; hotkeys toggle the host");
    bool hook_attempted = false,ready = false,timeout_logged = false;
    // The 11-04 game update removed the native debug menu (DebugObj.js, BP_CollectHelp, PBZ.DebugMenu.Enable,
    // GM_* bodies), so the two debug switches are no longer written; the tools need only the game-thread hook.
    const ULONGLONG deadline = GetTickCount64() + 10 * 60 * 1000;
    ULONGLONG next_settings = 0;
    while (!InterlockedCompareExchange(&stop_worker, 0, 0)) {
        UiHotkeys::Tick();
        DebugHub::Tick();
        // The game reads its pads at rest while the panel is up; the panel chord never reaches it.
        GamePadGate::Update(DebugHub::ObscuresGameplay(), UiHotkeys::Get(UiHotkeys::Action::ToggleHub, UiHotkeys::Device::Xbox).buttons,
            UiHotkeys::Get(UiHotkeys::Action::ToggleHub, UiHotkeys::Device::PlayStation).buttons);
        UiLoadBanner::Tick();
        GearWidget::Tick();
        SkillToast::Tick();
        BossMenu::Tick();
        BossAIMenu::Tick();
        NpcMenu::Tick();
        ComboMenu::Tick();
        AirSkillsMenu::Tick();
        MotianGrabMenu::Tick();
        WeaponPresets::WorkerTick();
        WeaponPresetsMenu::Tick();
        SupplementMenu::Tick();
        DonatePage::Tick();
        NetPage::Tick();
        EntryFocus::Poll();
        controls.Tick();
        if (GetTickCount64() < next_settings) { Sleep(DebugHub::Visible()||DebugHub::Animating()||UiLoadBanner::Active()||GearWidget::Active()||SkillToast::Active()?16:25); continue; }
        next_settings = GetTickCount64() + 1000;
        if (!hook_attempted) if (ue::Object* engine = ue::FindEngine()) { InstallGameThread(base, engine); hook_attempted = true; }
        PollCommand(controls);
        if (!ready && GameThread::Installed()) {
            ready = true; InterlockedExchange(&worker_status, 31);
            // Air skills, grabs and NPC skills fail quietly without these (11-04 port missed them once).
            if (!ue::ReferenceCodeMatches()) Log("UE_RUNTIME reference helpers mismatch: engine AddRef/ReleaseRef differ from the pinned build; air skills, grabs and NPC skills cannot keep objects");
            UiLoadBanner::ShowSuccess();
            Log("UI_LOAD_BANNER success; main tool window remains hidden until user request");
        } else if (!ready && !timeout_logged && GetTickCount64() > deadline) {
            timeout_logged = true; Log("GAME_THREAD_HOOK not installed after 10 minutes; still waiting");
        }
        Sleep(DebugHub::Visible()||DebugHub::Animating()||UiLoadBanner::Active()||GearWidget::Active()||SkillToast::Active()?16:25);
    }
    WeaponPresets::Shutdown(); WeaponPresetsMenu::Shutdown(); SupplementMenu::Shutdown(); DonatePage::Shutdown(); NetPage::Shutdown(); ComboMenu::Shutdown();
    SkillToast::Shutdown(); GearWidget::Shutdown(); DebugHub::Shutdown();
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
            // GamePadGate (game_pad_gate.h): the same kind of entries for the game's pad reads (XInput, DualSense).
            if (!_wcsicmp(name ? name + 1 : host, L"PBZ.exe")) { PadTriggers::Install(); GamePadGate::Install(); }
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
