#include "boss_rush.h"
#include "character_swap.h"
#include "boss_record_guard.h"
#include "boss_ai.h"
#include "entry_safety.h"
#include "entry_protection.h"
#include "entry_focus.h"
#include "npc_retarget.h"
#include "air_skills.h"
#include "motian_grab.h"
#include "weapon_presets.h"
#include "ue_runtime.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <iterator>
#include <sstream>

namespace BossRush {
namespace {
using ue::Object; using ue::Call; using ue::Name; using ue::Array;
using Transform = EntrySafety::Transform;
static_assert(sizeof(Transform) == 96);
struct DataEntry {
    const char* id; const wchar_t* name; const char* class_path;
    const wchar_t* level; Transform destination; const char* spawner;
    bool has_destination; const wchar_t* detail;
};
#include "boss_catalog.generated.h"
struct Pending { Action action; int32_t entry; Options options; };
SRWLOCK snapshot_lock = SRWLOCK_INIT, request_lock = SRWLOCK_INIT;
Snapshot snapshot_;
std::deque<Pending> requests_;
Logger logger_ = nullptr;
Object* engine_ = nullptr;
ULONGLONG next_context_ = 0;
bool initialized_ = false;
bool cleanup_pending_ = false;
enum class Phase { Idle, Loading, Positioning, Clearing, Spawning, WaitingBoss, Fighting, Won, Returning, Failed };
Phase phase_ = Phase::Idle;
bool session_ = false, returning_ = false, positioned_ = false, saw_alive_ = false, force_reset_ = false;
bool direct_spawn_attempted_ = false, direct_force_pending_ = false;
bool combat_verified_ = false;
bool bypass_spawner_ = false;
ULONGLONG next_force_attempt_ = 0;
Object* session_instance_ = nullptr;
Name session_instance_name_, veto_source_, prior_world_name_, positioned_pawn_name_;
Object* prior_world_ = nullptr; Object* positioned_pawn_ = nullptr; Object* spawner_ = nullptr; Object* boss_ = nullptr;
Name spawner_name_, boss_name_;
std::wstring original_slot_, original_level_;
Transform original_transform_;
Options options_;
int32_t original_cycle_ = 0, original_difficulty_ = 2, current_ = -1;
std::deque<int32_t> queue_;
ULONGLONG deadline_ = 0, next_encounter_tick_ = 0, stage_time_ = 0, dead_since_ = 0;
ULONGLONG next_ai_bind_ = 0, next_scene_scan_ = 0;
Object* encounter_world_ = nullptr; Name encounter_world_name_;
bool entry_armed_ = false, focus_wait_logged_ = false;
bool combat_focus_paused_ = false;
bool entry_options_applied_ = false;
ULONGLONG player_ready_since_ = 0, protection_release_at_ = 0;
Object* player_nodes_ = nullptr;
ULONGLONG next_player_wait_log_ = 0;
using NativeExec = void(__fastcall*)(Object*, void*, void*);
NativeExec advanced_original_ = nullptr;
NativeExec force_save_original_ = nullptr;
Object* advanced_function_ = nullptr, *session_save_ = nullptr;
Object* force_save_function_ = nullptr;
Name advanced_function_name_, session_save_name_;
Name force_save_function_name_;
volatile LONG advanced_protected_ = 0, advanced_rejected_ = 0, force_save_rejected_ = 0;
constexpr size_t kNativeExecOffset = 0x128;
constexpr uintptr_t kAdvancedExecRva = 0x065ff3b0;
constexpr uintptr_t kForceSaveExecRva = 0x065fc860;
constexpr size_t kSaveBusyOffset = 0x279;
constexpr int32_t kLegacySaveGuardOffset = 0x3d4;

void Logf(const char* format, ...) {
    if (!logger_) return;
    char line[1000]{}; va_list arguments; va_start(arguments, format);
    vsprintf_s(line, format, arguments); va_end(arguments); logger_(line);
}
void Status(const wchar_t* format, ...) {
    wchar_t text[640]{}; va_list arguments; va_start(arguments, format);
    vswprintf_s(text, format, arguments); va_end(arguments);
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_.status = text; ReleaseSRWLockExclusive(&snapshot_lock);
}
int32_t Offset(Object* object, const char* property) { return object ? ue::Offset(ue::ClassOf(object), property) : -1; }
Object* ObjectProperty(Object* object, const char* property) {
    const int32_t offset = Offset(object, property);
    return offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
}
struct Context { Object* world = nullptr; Object* instance = nullptr; Object* controller = nullptr; Object* pawn = nullptr; };
bool ContextNow(Context& context) {
    if (!engine_ || !ue::Alive(engine_)) engine_ = ue::FindEngine();
    Object* viewport = ObjectProperty(engine_, "GameViewport");
    context.world = ObjectProperty(viewport, "World");
    if (!context.world || !ue::Alive(context.world)) return false;
    context.instance = ObjectProperty(context.world, "OwningGameInstance");
    if (!context.instance || !ue::Alive(context.instance)) return false;
    const int32_t players_offset = Offset(context.instance, "LocalPlayers");
    if (players_offset < 0) return true;
    const Array players = ue::At<Array>(context.instance, players_offset);
    if (!players.data || players.num <= 0 || players.num > 8) return true;
    Object* local = static_cast<Object**>(players.data)[0];
    context.controller = ObjectProperty(local, "PlayerController");
    context.pawn = ObjectProperty(context.controller, "Pawn");
    if (!context.controller || !ue::Alive(context.controller)) context.controller = nullptr;
    if (!context.pawn || !ue::Alive(context.pawn)) context.pawn = nullptr;
    return true; // Stop/recovery can use the GI/save subsystem without a pawn.
}
Object* Subsystem(const char* path, Object* outer) {
    Object* cls = ue::FindObject(path), *found = nullptr;
    if (!cls || !outer) return nullptr;
    ue::ForEachObject([&](Object* object) {
        if (!ue::IsTemplate(object) && ue::Alive(object) && ue::OuterOf(object) == outer && ue::IsA(object, cls)) {
            found = object; return false;
        }
        return true;
    });
    return found;
}
bool ValidOptions(const Options& options) {
    const Snapshot current = GetSnapshot();
    if (options.override_rank && (!current.rank_supported || options.rank < current.rank_min || options.rank > current.rank_max)) return false;
    if (options.override_cycle && (options.cycle_number < 1 || options.cycle_number > current.cycle_max)) return false;
    if (options.override_difficulty && std::none_of(current.difficulties.begin(), current.difficulties.end(),
        [&](const Difficulty& value) { return value.value == options.difficulty; })) return false;
    return true;
}
bool Pop(Pending& pending) {
    AcquireSRWLockExclusive(&request_lock);
    const bool available = !requests_.empty();
    if (available) { pending = requests_.front(); requests_.pop_front(); }
    ReleaseSRWLockExclusive(&request_lock); return available;
}

std::string NameText(Name name) { char text[256]{}; ue::NameText(name, text, sizeof(text)); return text; }
std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return "<conversion failed>";
    std::string output(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), count, nullptr, nullptr);
    return output;
}
bool Same(Object* object, Name name) { return object && ue::Alive(object) && ue::NameOf(object) == name; }
bool Writable(uintptr_t address, size_t bytes) {
    MEMORY_BASIC_INFORMATION memory{};
    if (!address || !VirtualQuery(reinterpret_cast<const void*>(address), &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const DWORD protection = memory.Protect & 0xff;
    return (protection == PAGE_READWRITE || protection == PAGE_WRITECOPY) &&
        address + bytes <= reinterpret_cast<uintptr_t>(memory.BaseAddress) + memory.RegionSize;
}
bool Readable(const void* pointer, size_t bytes) {
    MEMORY_BASIC_INFORMATION memory{};
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (!pointer || !VirtualQuery(pointer, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const uintptr_t begin = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return address >= begin && bytes <= memory.RegionSize && address - begin <= memory.RegionSize - bytes;
}
int32_t PracticeVetoDepth(Object* save) {
    // Verified Enter/ExitSaveVeto native map: sparse stride40, FName@0,
    // Reason@8, Depth@24. +0x3d4 is a separate legacy save guard, not this map.
    if (!save || veto_source_.None()) return -1;
    auto* map = reinterpret_cast<unsigned char*>(save) + 0x3e8;
    if (!Readable(map, 80)) return -1;
    const int32_t count = ue::At<int32_t>(map, 8), capacity = ue::At<int32_t>(map, 12),
        bits = ue::At<int32_t>(map, 40), max_bits = ue::At<int32_t>(map, 44), free = ue::At<int32_t>(map, 52);
    if (count < 0 || count > 4096 || capacity < count || bits != count || max_bits < bits || free < 0 || free > count) return -1;
    if (!count) return 0;
    auto* data = ue::At<unsigned char*>(map, 0);
    auto* allocation = ue::At<uint32_t*>(map, 32);
    if (!allocation) allocation = reinterpret_cast<uint32_t*>(map + 16);
    if (!Readable(data, static_cast<size_t>(count) * 40) || !Readable(allocation, (static_cast<size_t>(bits) + 31) / 32 * 4)) return -1;
    int32_t depth = 0, seen = 0;
    for (int32_t i = 0; i < count; ++i) if (allocation[i / 32] & (1u << (i % 32))) {
        ++seen;
        auto* element = data + static_cast<size_t>(i) * 40;
        if (ue::At<Name>(element, 0) == veto_source_) {
            if (depth || ue::At<uint8_t>(element, 8) != 0) return -1;
            depth = ue::At<int32_t>(element, 24);
            if (depth <= 0 || depth > 100000) return -1;
        }
    }
    return seen == count - free ? depth : -1;
}
bool EnsurePracticeVeto(Object* save) {
    const int32_t before = PracticeVetoDepth(save);
    if (before == 1) return true;
    if (before != 0) { Logf("BOSS_VETO unexpected own source depth=%d; no additional Enter", before); return false; }
    const uint8_t gameplay_flow = 0;
    if (!Call(save, "EnterSaveVeto").Set("SourceID", veto_source_).Set("Reason", gameplay_flow).Invoke()) return false;
    const int32_t after = PracticeVetoDepth(save);
    Logf("BOSS_VETO own source depth=%d after EnterSaveVeto", after);
    return after == 1;
}
bool ReleasePracticeVeto(Object* save) {
    const int32_t before = PracticeVetoDepth(save);
    if (before == 0) return true;
    if (before != 1 || !Call(save, "ExitSaveVeto").Set("SourceID", veto_source_).Invoke()) return false;
    const int32_t after = PracticeVetoDepth(save);
    Logf("BOSS_VETO own source depth=%d after ExitSaveVeto", after);
    return after == 0;
}
void __fastcall PracticeAdvancedSave(Object* target, void* frame, void* result) {
    // Let the original zero-argument exec consume its own FFrame. Its native
    // implementation returns false immediately while the manager is busy.
    // This protects the reflected BossCombatRecorder write route only; direct
    // native C++ callers are not redirected by a UFunction exec replacement.
    const bool protect = InterlockedCompareExchange(&advanced_protected_, 0, 0) && target == session_save_ && Same(target, session_save_name_);
    bool changed = false;
    __try {
        if (protect && ue::At<uint8_t>(target, static_cast<int32_t>(kSaveBusyOffset)) == 0) {
            ue::At<uint8_t>(target, static_cast<int32_t>(kSaveBusyOffset)) = 1; changed = true;
        }
        advanced_original_(target, frame, result);
        if (protect && InterlockedIncrement(&advanced_rejected_) == 1) Logf("BOSS_PRACTICE reflected account immediate-save request rejected by native busy branch");
    } __finally {
        if (changed && ue::At<uint8_t>(target, static_cast<int32_t>(kSaveBusyOffset)) == 1)
            ue::At<uint8_t>(target, static_cast<int32_t>(kSaveBusyOffset)) = 0;
    }
}
void __fastcall PracticeForceSave(Object* target, void* frame, void* result) {
    // The reflected/JS ForceSave route bypasses SourceID vetoes. Let its own
    // exec parse FFrame and run the native legacy-block branch: return2 and the
    // original completion delegate(false), with no save-queue append.
    // Direct C++ calls to ForceSave are outside this exec hook's coverage.
    const bool protect = InterlockedCompareExchange(&advanced_protected_, 0, 0) &&
        target == session_save_ && Same(target, session_save_name_);
    bool changed = false;
    LONG before = -1;
    auto* guard = reinterpret_cast<volatile LONG*>(target + kLegacySaveGuardOffset);
    __try {
        if (protect) {
            before = InterlockedCompareExchange(guard, 0, 0);
            if (before >= 0 && before < 100000 && InterlockedCompareExchange(guard, before + 1, before) == before) changed = true;
        }
        force_save_original_(target, frame, result);
        if (changed && InterlockedIncrement(&force_save_rejected_) == 1)
            Logf("BOSS_PRACTICE reflected ForceSave rejected by native legacy branch; completion callback preserved");
    } __finally {
        if (changed && Same(target, session_save_name_))
            InterlockedCompareExchange(guard, before, before + 1);
    }
}
bool ProtectAdvanced(Object* save) {
    Object* function = ue::FindObject("/Script/ProjectPBZ.PBZSaveLoadSubsystem.SaveAdvancedSaveGameNow");
    Object* force_function = ue::FindObject("/Script/ProjectPBZ.PBZSaveLoadSubsystem.ForceSave");
    // No input arguments, one bool return (the exec stores AL through Result).
    if (!function || !force_function || !save || ue::At<uint16_t>(function, 0x106) != 1 ||
        ue::At<uint16_t>(force_function, 0x106) != 209 ||
        ue::At<uint8_t>(save, static_cast<int32_t>(kSaveBusyOffset)) != 0 ||
        ue::At<int32_t>(save, kLegacySaveGuardOffset) < 0 || ue::At<int32_t>(save, kLegacySaveGuardOffset) >= 100000) return false;
    const uintptr_t field = reinterpret_cast<uintptr_t>(function) + kNativeExecOffset;
    const uintptr_t force_field = reinterpret_cast<uintptr_t>(force_function) + kNativeExecOffset;
    const uintptr_t expected = ue::Base() + kAdvancedExecRva;
    const uintptr_t force_expected = ue::Base() + kForceSaveExecRva;
    if ((field & 7) || (force_field & 7) || !Writable(field, 8) || !Writable(force_field, 8) ||
        !Writable(reinterpret_cast<uintptr_t>(save) + kSaveBusyOffset, 1) ||
        !Writable(reinterpret_cast<uintptr_t>(save) + kLegacySaveGuardOffset, 4) ||
        ue::At<uintptr_t>(function, static_cast<int32_t>(kNativeExecOffset)) != expected ||
        ue::At<uintptr_t>(force_function, static_cast<int32_t>(kNativeExecOffset)) != force_expected) return false;
    advanced_original_ = reinterpret_cast<NativeExec>(expected);
    advanced_function_ = function; advanced_function_name_ = ue::NameOf(function);
    session_save_ = save; session_save_name_ = ue::NameOf(save);
    force_save_original_ = reinterpret_cast<NativeExec>(force_expected);
    force_save_function_ = force_function; force_save_function_name_ = ue::NameOf(force_function);
    InterlockedExchange(&advanced_rejected_, 0); InterlockedExchange(&advanced_protected_, 1);
    InterlockedExchange(&force_save_rejected_, 0);
    if (static_cast<uintptr_t>(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(field),
        static_cast<LONG64>(reinterpret_cast<uintptr_t>(&PracticeAdvancedSave)), static_cast<LONG64>(expected))) != expected) {
        InterlockedExchange(&advanced_protected_, 0); return false;
    }
    if (static_cast<uintptr_t>(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(force_field),
        static_cast<LONG64>(reinterpret_cast<uintptr_t>(&PracticeForceSave)), static_cast<LONG64>(force_expected))) != force_expected) {
        InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(field), static_cast<LONG64>(expected),
            static_cast<LONG64>(reinterpret_cast<uintptr_t>(&PracticeAdvancedSave)));
        InterlockedExchange(&advanced_protected_, 0); return false;
    }
    return true;
}
void UnprotectAdvanced() {
    InterlockedExchange(&advanced_protected_, 0);
    if (Same(advanced_function_, advanced_function_name_)) {
        const uintptr_t field = reinterpret_cast<uintptr_t>(advanced_function_) + kNativeExecOffset;
        if (Writable(field, 8)) InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(field),
            static_cast<LONG64>(reinterpret_cast<uintptr_t>(advanced_original_)),
            static_cast<LONG64>(reinterpret_cast<uintptr_t>(&PracticeAdvancedSave)));
    }
    if (Same(force_save_function_, force_save_function_name_)) {
        const uintptr_t field = reinterpret_cast<uintptr_t>(force_save_function_) + kNativeExecOffset;
        if (Writable(field, 8)) InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(field),
            static_cast<LONG64>(reinterpret_cast<uintptr_t>(force_save_original_)),
            static_cast<LONG64>(reinterpret_cast<uintptr_t>(&PracticeForceSave)));
    }
    advanced_function_ = nullptr; force_save_function_ = nullptr; session_save_ = nullptr;
}
ue::String String(const std::wstring& text) {
    const auto size = static_cast<int32_t>(text.size() + 1); return {text.c_str(), size, size};
}
Object* Save(const Context& context) { return Subsystem("/Script/ProjectPBZ.PBZSaveLoadSubsystem", context.instance); }
Object* Game(const Context& context) { return Subsystem("/Script/ProjectPBZ.PBZGameSystem", context.world); }
void Busy(bool value) {
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_.busy = value; ReleaseSRWLockExclusive(&snapshot_lock);
}
void PublishOptions() {
    AcquireSRWLockExclusive(&snapshot_lock);
    snapshot_.options = options_; snapshot_.current = current_; snapshot_.total = static_cast<int32_t>(queue_.size()) + snapshot_.completed;
    ReleaseSRWLockExclusive(&snapshot_lock);
}
bool InvokeInt(Object* target, const char* function, const char* parameter, int32_t value) {
    return Call(target, function).Set(parameter, value).Invoke();
}
bool ApplyOptions(const Context& context) {
    Object* game = Game(context); if (!game) return false;
    if (options_.override_cycle && !InvokeInt(game, "SetCycleCount", "InCycleCount", options_.cycle_number - 1)) return false;
    if (options_.override_difficulty) {
        const auto difficulty = static_cast<uint8_t>(options_.difficulty);
        if (!Call(game, "SetDifficultMode").Set("DifficultMode", difficulty).Invoke()) return false;
    }
    Call cycle(game, "GetCycleCount"), difficulty(game, "GetDifficultModeByte");
    if (!cycle.Invoke() || !difficulty.Invoke()) return false;
    if ((options_.override_cycle && cycle.Get<int32_t>("ReturnValue") != options_.cycle_number - 1) ||
        (options_.override_difficulty && difficulty.Get<int32_t>("ReturnValue") != options_.difficulty)) return false;
    Logf("BOSS_OPTIONS cycle_index=%d difficulty=%d (applied before encounter spawn)",
        cycle.Get<int32_t>("ReturnValue"), difficulty.Get<int32_t>("ReturnValue"));
    return true;
}
void ReleaseMovement() {
    if (entry_armed_ || !EntrySafety::HasHold()) EntrySafety::ReleasePawn();
}
bool HoldMovement(Object* pawn) { return EntrySafety::HoldPawn(pawn); }
bool BeginSession(const Context& context) {
    if (session_) return context.instance == session_instance_ && ue::NameOf(context.instance) == session_instance_name_;
    if (!context.pawn || !context.controller) return false;
    Object* save = Save(context), *game = Game(context);
    if (!save || !game || !ue::FindFunction(ue::ClassOf(save), "EnterSaveVeto") || !ue::FindFunction(ue::ClassOf(save), "LoadGame")) {
        Logf("BOSS_PREPARE failed: save/game subsystem or session API unavailable"); return false;
    }
    // Native UFunction is GetTransform; GetActorTransform is its ScriptName.
    // There is no K2_GetActorTransform in this build.
    Call slot(save, "GetContinueSlotName"), transform(context.pawn, "GetTransform"),
        cycle(game, "GetCycleCount"), difficulty(game, "GetDifficultModeByte");
    if (!slot.Ok() || !transform.Ok() || !cycle.Ok() || !difficulty.Ok()) {
        Logf("BOSS_PREPARE missing function: slot=%d transform=%d cycle=%d difficulty=%d", slot.Ok(), transform.Ok(), cycle.Ok(), difficulty.Ok()); return false;
    }
    if (!slot.Invoke() || !transform.Invoke() || !cycle.Invoke() || !difficulty.Invoke()) {
        Logf("BOSS_PREPARE native baseline call failed"); return false;
    }
    const ue::String result = slot.Get<ue::String>("ReturnValue");
    if (!result.data || result.num <= 1 || result.num > 4096) {
        Logf("BOSS_PREPARE original continue slot is empty or invalid"); ue::Free(const_cast<wchar_t*>(result.data)); return false;
    }
    original_slot_.assign(result.data, static_cast<size_t>(result.num - 1)); ue::Free(const_cast<wchar_t*>(result.data));
    original_transform_ = transform.Get<Transform>("ReturnValue");
    original_level_ = Wide(NameText(ue::NameOf(context.world)));
    original_cycle_ = cycle.Get<int32_t>("ReturnValue"); original_difficulty_ = difficulty.Get<int32_t>("ReturnValue");
    veto_source_ = ue::MakeName(L"PBZDebugDLL.BossRush");
    if (veto_source_.None() || !BossRecordGuard::Capture(save)) { Logf("BOSS_PREPARE account BossRecord baseline could not be captured"); return false; }
    if (!ProtectAdvanced(save)) { BossRecordGuard::Clear(); Logf("BOSS_PREPARE save protection not ready; original advanced exec/manager layout must match"); return false; }
    if (!EnsurePracticeVeto(save)) {
        ReleasePracticeVeto(save);
        UnprotectAdvanced(); BossRecordGuard::Clear(); return false;
    }
    session_instance_ = context.instance; session_instance_name_ = ue::NameOf(context.instance); session_ = true;
    Logf("BOSS_SESSION started; save veto held until original slot reload; original cycle=%d difficulty=%d", original_cycle_, original_difficulty_);
    return true;
}
bool OpenMap(const Context& context, const std::wstring& level, const std::wstring& game_options = {}) {
    Object* kismet = ue::Default("/Script/Engine.Default__GameplayStatics");
    const Name name = ue::MakeName(level.c_str()); const bool absolute = true;
    if (!kismet || name.None()) return false;
    // Avoid TeleportTo's ForceSave route. The pending destination is applied to
    // the loaded pawn before releasing its movement.
    Call open(kismet, "OpenLevel");
    open.Set("WorldContextObject", context.instance).Set("LevelName", name)
        .Set("bAbsolute", absolute).Set("Options", String(game_options));
    if (!open.Ok()) return false;
    if (!BossAI::ReleaseTarget()) return false;
    if (!MotianGrab::BeforeTravel() || !AirSkills::BeforeTravel() || !NpcRetarget::BeforeTravel() || !WeaponPresets::BeforeTravel()) return false;
    if (!EntryProtection::ReleasePause(context.instance) || !EntrySafety::BeforeTravel()) return false;
    CharacterSwap::BeforeTravel();
    return open.Invoke();
}
bool PrimePlayerSaveForTravel(const Context& context, std::wstring& options) {
    Object* save = Save(context); if (!save) return false;
    // Match the normal game-instance load entry (including StartGameType=Game).
    Call load(context.instance, "LoadGame");
    if (!load.Set("SlotName", String(original_slot_)).Invoke()) return false;
    unsigned char* output = load.Slot("LoadGameResultParam", 80);
    if (!output) return false;
    const uint8_t result = output[0];
    ue::String level{}, modes{}, json_text{}; void* controller = nullptr;
    std::memcpy(&level, output + 8, sizeof(level));
    std::memcpy(&modes, output + 24, sizeof(modes));
    std::memcpy(&json_text, output + 48, sizeof(json_text));
    std::memcpy(&controller, output + 72, sizeof(controller));
    const bool valid = result == 0 && level.data && level.num > 1 && level.num < 4096 &&
        modes.num >= 0 && modes.num <= 16384 && (!modes.num || modes.data);
    if (valid) options = modes.num > 0 ? std::wstring(modes.data, static_cast<size_t>(modes.num - 1)) : std::wstring{};
    ue::Free(const_cast<wchar_t*>(level.data)); ue::Free(const_cast<wchar_t*>(modes.data));
    if (controller) reinterpret_cast<void(__fastcall*)(void*)>(ue::Base() + 0x0126fc00)(controller);
    ue::Free(const_cast<wchar_t*>(json_text.data)); std::memset(output, 0, 80);
    if (!valid) { Logf("BOSS_LOAD_PRIME failed result=%u; travel cancelled", result); return false; }
    // LoadGame primes the normal character deserialization path. OpenLevel alone
    // otherwise creates a default pawn with initial inventory/skills.
    const bool protected_session = EnsurePracticeVeto(save);
    Logf("BOSS_LOAD_PRIME original slot loaded before OpenLevel; result=Success own_source_depth=%d", PracticeVetoDepth(save));
    return protected_session;
}
void Fail(const wchar_t* why) {
    ReleaseMovement(); phase_ = Phase::Failed; Busy(false);
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_.running = false; ReleaseSRWLockExclusive(&snapshot_lock);
    if (session_) Status(L"%s。练习存档保护仍保持；可点停止返回原存档。", why);
    else Status(L"%s；尚未开始练习会话。", why);
    Logf("BOSS_RUSH stopped phase=%d reason=%s", static_cast<int>(phase_), Utf8(why).c_str());
}
bool BeginDestination(const Context& context, int32_t index) {
    if (index < 0 || static_cast<size_t>(index) >= std::size(kData) || !kData[index].has_destination || !BeginSession(context)) return false;
    if (!BossAI::ReleaseTarget()) { Fail(L"无法恢复旧 Boss 的 AI 参数，已取消切换"); return false; }
    ReleaseMovement(); current_ = index; returning_ = false; positioned_ = false; positioned_pawn_ = nullptr;
    entry_armed_ = false; focus_wait_logged_ = combat_focus_paused_ = false; player_ready_since_ = protection_release_at_ = 0;
    entry_options_applied_ = false;
    player_nodes_ = nullptr;
    if (context.pawn) EntryProtection::Protect(context.pawn);
    saw_alive_ = false; dead_since_ = 0; boss_ = spawner_ = nullptr;
    direct_spawn_attempted_ = direct_force_pending_ = combat_verified_ = false; next_force_attempt_ = 0;
    encounter_world_ = nullptr; next_ai_bind_ = next_scene_scan_ = 0;
    prior_world_ = context.world; prior_world_name_ = context.world ? ue::NameOf(context.world) : Name{};
    const bool same_map = !force_reset_ && context.world && ue::NameIs(ue::NameOf(context.world), NameText(ue::MakeName(kData[index].level)).c_str());
    phase_ = same_map ? Phase::Positioning : Phase::Loading;
    deadline_ = GetTickCount64() + 120000; stage_time_ = GetTickCount64(); Busy(true); PublishOptions();
    Status(L"正在前往 %s / %s；等待地图与战斗区域加载", kData[index].name, kData[index].level);
    Logf("BOSS_TRAVEL index=%d id=%s level=%ls via=%s", index, kData[index].id, kData[index].level, same_map ? "pawn transform" : "OpenLevel (no save)");
    if (!same_map) {
        std::wstring travel_options;
        if (!PrimePlayerSaveForTravel(context, travel_options)) { Fail(L"无法加载原角色养成数据，已取消跨图"); return false; }
        if (!OpenMap(context, kData[index].level, travel_options)) { Fail(L"打开目标地图失败"); return false; }
    }
    return true;
}
bool ReturnOriginal(const Context& context) {
    if (!session_) { phase_ = Phase::Idle; Busy(false); Status(L"Boss Rush 已停止"); return true; }
    if (context.instance != session_instance_ || !Same(session_instance_, session_instance_name_)) return false;
    Object* save = Save(context); if (!save) return false;
    if (!BossAI::ReleaseTarget()) return false;
    // Use the normal game-instance entry so the returning pawn deserializes the
    // original equipment and skills instead of taking the new-game path.
    Call load(context.instance, "LoadGame");
    if (!load.Set("SlotName", String(original_slot_)).Invoke()) return false;
    Object* strukt = ue::FindObject("/Script/ProjectPBZ.LoadGameResultParam");
    const int32_t type = strukt ? ue::Offset(strukt, "ResultType") : -1;
    const int32_t level = strukt ? ue::Offset(strukt, "LevelName") : -1;
    const int32_t mode = strukt ? ue::Offset(strukt, "GameModeOptions") : -1;
    unsigned char* output = load.Slot("LoadGameResultParam", 80);
    if (!output || type != 0 || level != 8 || mode != 24 || ue::StructSize(strukt) != 80) return false;
    const uint8_t result_type = output[type];
    ue::String destination{}, modes{};
    std::memcpy(&destination, output + level, sizeof(destination));
    std::memcpy(&modes, output + mode, sizeof(modes));
    const bool valid_level = destination.data && destination.num > 1 && destination.num <= 4096;
    const bool valid_options = modes.num >= 0 && modes.num <= 16384 && (modes.num == 0 || modes.data);
    const std::wstring destination_copy = valid_level ? std::wstring(destination.data, static_cast<size_t>(destination.num - 1)) : std::wstring{};
    const std::wstring modes_copy = valid_options && modes.num > 0 ? std::wstring(modes.data, static_cast<size_t>(modes.num - 1)) : std::wstring{};
    ue::Free(const_cast<wchar_t*>(destination.data)); ue::Free(const_cast<wchar_t*>(modes.data));
    // LoadedJsonObject is an engine-created output, not a borrowed input.
    // BossRecordGuard already verified this build's shared-reference release helper.
    ue::String loaded_text{}; void* loaded_controller = nullptr;
    std::memcpy(&loaded_text, output + 48, sizeof(loaded_text));
    std::memcpy(&loaded_controller, output + 48 + 24, sizeof(loaded_controller));
    if (loaded_controller) reinterpret_cast<void(__fastcall*)(void*)>(ue::Base() + 0x0126fc00)(loaded_controller);
    ue::Free(const_cast<wchar_t*>(loaded_text.data));
    std::memset(output + 48, 0, 32);
    if (result_type != 0 || !valid_level || !valid_options || !EnsurePracticeVeto(save)) return false; // ESaveLoadResultType::Success=0
    returning_ = true; positioned_ = false; positioned_pawn_ = nullptr;
    entry_armed_ = false; focus_wait_logged_ = combat_focus_paused_ = false; player_ready_since_ = protection_release_at_ = 0; player_nodes_ = nullptr;
    prior_world_ = context.world; prior_world_name_ = context.world ? ue::NameOf(context.world) : Name{};
    phase_ = Phase::Returning; deadline_ = GetTickCount64() + 120000; stage_time_ = GetTickCount64();
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_.running = false; snapshot_.busy = true; ReleaseSRWLockExclusive(&snapshot_lock);
    Status(L"正在重新载入进入练习前的存档；完成后解除存档保护");
    // The current unsaved location may be in a different map from the slot's
    // saved location. Restore the captured map and transform together.
    return OpenMap(context, original_level_.empty() ? destination_copy : original_level_, modes_copy);
}

// The encounter/session implementation follows the individually verified native
// APIs. Keeping this dispatcher separate prevents a Boss request from disabling
// the existing Groom task after an exception.
struct Vector { double x, y, z; };
struct SoftClass { int32_t index, serial; Name package, asset; ue::String subpath; };
static_assert(sizeof(SoftClass) == 40);
bool InWorld(Object* actor, Object* world) {
    Object* outer = actor;
    Object* level_class = ue::FindObject("/Script/Engine.Level");
    for (int i = 0; outer && i < 16; ++i, outer = ue::OuterOf(outer)) {
        if (outer == world) return true;
        // Streamed WP levels can have a vestigial outer UWorld. OwningWorld is
        // the live runtime world used by ULevel::GetWorld.
        if (level_class && ue::IsA(outer, level_class)) return ObjectProperty(outer, "OwningWorld") == world;
    }
    return false;
}
Object* FindSpawner(const Context& context, const DataEntry& entry) {
    Object* cls = ue::FindObject("/Script/SGameAI.AISpawner"); if (!cls) return nullptr;
    const int32_t config = ue::Offset(cls, "Class");
    if (config < 0) return nullptr;
    const std::string path(entry.class_path), package = path.substr(0, path.find('.'));
    Object* best = nullptr; double distance = 15000.0 * 15000.0;
    ue::ForEachObject([&](Object* actor) {
        if (ue::IsTemplate(actor) || !ue::Alive(actor) || !ue::IsA(actor, cls) || !InWorld(actor, context.world)) return true;
        if (*entry.spawner && ue::NameIs(ue::NameOf(actor), entry.spawner)) { best = actor; return false; }
        const SoftClass soft = ue::At<SoftClass>(actor, config);
        if (!ue::NameIs(soft.package, package.c_str())) return true;
        Call position(actor, "K2_GetActorLocation"); if (!position.Invoke()) return true;
        const Vector value = position.Get<Vector>("ReturnValue");
        const double dx = value.x - entry.destination.translation[0], dy = value.y - entry.destination.translation[1], dz = value.z - entry.destination.translation[2];
        const double squared = dx * dx + dy * dy + dz * dz;
        if (std::isfinite(squared) && squared < distance) { distance = squared; best = actor; }
        return true;
    });
    return best;
}
Object* Spawned(Object* spawner) {
    return spawner ? ObjectProperty(spawner, "SpawnedAI") : nullptr;
}
bool ReadAttributeValue(Object* actor, const char* property, float& output) {
    Object* set = ObjectProperty(actor, "AttributeSetBase");
    Object* basic = ue::FindObject("/Script/CharacterBasics.BasicAttributeSet");
    Object* data = ue::FindObject("/Script/GameplayAbilities.GameplayAttributeData");
    const int32_t field = Offset(set, property), current = data ? ue::Offset(data, "CurrentValue") : -1;
    if (!set || !ue::Alive(set) || !basic || !ue::IsA(set, basic) || field < 0 || !data ||
        ue::StructSize(data) != 16 || current != 12 || !Readable(set + field, 16)) return false;
    output = ue::At<float>(set, field + current);
    return std::isfinite(output);
}
bool Dead(Object* actor, float& health) {
    health = -1;
    if (!actor || !ue::Alive(actor)) return false;
    Object* utility = ue::Default("/Script/SGameAI.Default__SGameAIStateUtils");
    Call dead(utility, "IsDead");
    const bool known_state = dead.Set("Pawn", actor).Invoke();
    const bool state = known_state && dead.Get<bool>("ReturnValue");
    Call value(actor, "GetHealth");
    bool known_health = value.Invoke();
    if (known_health) health = value.Get<float>("ReturnValue");
    if (!known_health || !std::isfinite(health)) {
        // NPCs use PBZCharacter.AttributeSetBase, not the player-only
        // GetHealth method. This exact live chain was verified on ChiLian:
        // BasicAttributeSet.Health is GameplayAttributeData16, CurrentValue12.
        // Do not invoke the UInterface native exec on BP-only implementations.
        known_health = ReadAttributeValue(actor, "Health", health);
    }
    if (!std::isfinite(health)) { known_health = false; health = -1; }
    // The AI life state remains independent of an unavailable/initializing
    // attribute. A failed health read must never keep a fighting pawn waiting.
    return known_state ? state : known_health && health <= 0;
}
bool HasAIController(Object* actor) {
    Object* controller = ObjectProperty(actor, "Controller");
    Object* cls = ue::FindObject("/Script/SGameAI.SGameAIController");
    return controller && ue::Alive(controller) && cls && ue::IsA(controller, cls);
}
bool TargetClass(Object* actor, const DataEntry& entry) {
    Object* cls = ue::FindObject(entry.class_path);
    if (cls) return ue::IsA(actor, cls);
    const std::string name = std::string(entry.id) + "_C";
    return ue::NameIs(ue::NameOf(ue::ClassOf(actor)), name.c_str());
}
bool LiveTarget(Object* actor, const Context& context, const DataEntry& entry, float& health) {
    if (!actor || !ue::Alive(actor) || ue::IsTemplate(actor) || !InWorld(actor, context.world) || !TargetClass(actor, entry)) return false;
    Call destroying(actor, "IsActorBeingDestroyed");
    if (!destroying.Invoke() || destroying.Get<bool>("ReturnValue")) return false;
    return !Dead(actor, health) && (health > 0 || HasAIController(actor));
}
Object* FindEncounterBoss(const Context& context, float& health, bool force_scan = false) {
    const DataEntry& entry = kData[current_];
    if (Same(boss_, boss_name_) && LiveTarget(boss_, context, entry, health)) return boss_;
    Object* linked = Same(spawner_, spawner_name_) ? Spawned(spawner_) : nullptr;
    if (LiveTarget(linked, context, entry, health)) return linked;
    const ULONGLONG now = GetTickCount64();
    if (!force_scan && now < next_scene_scan_) return nullptr;
    next_scene_scan_ = now + 500;
    Call actors(ue::Default("/Script/Engine.Default__GameplayStatics"), "GetAllActorsOfClass");
    Object* cls = ue::FindObject(entry.class_path);
    if (!cls) cls = ue::FindObject("/Script/Engine.Pawn");
    if (!actors.Set("WorldContextObject", context.instance).Set("ActorClass", cls).Invoke()) return nullptr;
    const Array list = actors.Get<Array>("OutActors");
    Object* best = nullptr; double nearest = 25000.0 * 25000.0; float best_health = -1;
    if (list.num >= 0 && list.num <= 4096 && list.max >= list.num && (!list.num || Readable(list.data, static_cast<size_t>(list.num) * sizeof(Object*)))) {
        for (int32_t i = 0; i < list.num; ++i) {
            Object* actor = static_cast<Object**>(list.data)[i]; float hp = -1;
            if (!LiveTarget(actor, context, entry, hp)) continue;
            Call position(actor, "K2_GetActorLocation"); if (!position.Invoke()) continue;
            const Vector p = position.Get<Vector>("ReturnValue");
            const double dx = p.x - entry.destination.translation[0], dy = p.y - entry.destination.translation[1], dz = p.z - entry.destination.translation[2];
            const double distance = dx * dx + dy * dy + dz * dz;
            if (std::isfinite(distance) && distance < nearest) { best = actor; nearest = distance; best_health = hp; }
        }
    }
    ue::Free(list.data); health = best_health; return best;
}
void AcceptBoss(Object* actor, float health) {
    const bool changed = !Same(boss_, boss_name_) || actor != boss_ || phase_ != Phase::Fighting;
    boss_ = actor; boss_name_ = ue::NameOf(actor); saw_alive_ = true; dead_since_ = 0;
    phase_ = Phase::Fighting; Busy(false);
    if (GetTickCount64() >= next_ai_bind_) { next_ai_bind_ = GetTickCount64() + 1000; BossAI::BindTarget(actor); }
    if (!changed) return;
    if (health > 0) Status(L"%s 已进入战斗，HP %.0f；可打开攻击欲望 / AI 参数", kData[current_].name, health);
    else Status(L"%s 的真实 AI 已识别；生命读数暂不可用，可重试或调整 AI 参数", kData[current_].name);
    Logf("BOSS_ACTIVE id=%s actor=%s health=%.3f controller=%d route=runtime actor", kData[current_].id, NameText(boss_name_).c_str(), health, HasAIController(actor));
}
Object* LoadClass(const char* object_path) {
    if (Object* cls = ue::FindObject(object_path)) return cls;
    const std::string full(object_path); const size_t dot = full.find('.');
    if (dot == std::string::npos) return nullptr;
    SoftClass soft{};
    const std::wstring package = Wide(full.substr(0, dot)), asset = Wide(full.substr(dot + 1));
    soft.package = ue::MakeName(package.c_str()); soft.asset = ue::MakeName(asset.c_str());
    Call load(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LoadClassAsset_Blocking");
    return load.Set("AssetClass", soft).Invoke() ? load.Get<Object*>("ReturnValue") : nullptr;
}
Object* AILibrary() {
    constexpr const char* path = "/SGameAI/FunctionLibrary/BP_SGameAIFunctionLibrary.BP_SGameAIFunctionLibrary_C";
    Object* cls = LoadClass(path), *result = nullptr;
    if (!cls) return nullptr;
    ue::ForEachObject([&](Object* object) {
        if (ue::Alive(object) && ue::IsDefault(object) && ue::ClassOf(object) == cls) { result = object; return false; }
        return true;
    });
    return result;
}
bool BeginDirectSpawn(const Context& context) {
    if (direct_spawn_attempted_) return false;
    direct_spawn_attempted_ = true;
    Object* library = AILibrary(), *cls = LoadClass(kData[current_].class_path);
    if (!library || !cls) { Fail(L"无法加载游戏 AI 创建接口或所选 Boss 类"); return false; }
    if (!NpcRetarget::BeforeActorClassSpawn(cls)) { Fail(L"NPC 重定向外观尚未恢复，未生成可能共享骨架的 Boss"); return false; }
    // This is the same Blueprint library used by BP_AISpawner: deferred spawn
    // plus FinishActorSpawning. No fabricated SpawnInfo or shared CDO edits.
    Call spawn(library, "SpawnAI");
    Transform spawn_transform;
    if (!EntrySafety::MakeBossTransform(cls, kData[current_].destination, spawn_transform)) {
        Fail(L"无法确认 Boss 胶囊半高，已取消不安全生成"); return false;
    }
    if (!spawn.Set("Class", cls).Set("SpawnTransform", spawn_transform)
        .Set("__WorldContext", context.instance).Invoke()) { Fail(L"游戏 SpawnAI 接口不匹配"); return false; }
    Object* actor = spawn.Get<Object*>("ReturnValue");
    if (!actor || !ue::Alive(actor) || !InWorld(actor, context.world) || !TargetClass(actor, kData[current_])) {
        Fail(L"原生 AI 创建没有返回当前地图的目标 Boss"); return false;
    }
    boss_ = actor; boss_name_ = ue::NameOf(actor); direct_force_pending_ = true;
    phase_ = Phase::WaitingBoss; stage_time_ = GetTickCount64(); deadline_ = stage_time_ + 60000;
    next_force_attempt_ = 0; Busy(false);
    Status(L"%s 已按 AI 默认配置直接创建；正在触战（不重建原剧情覆盖参数）", kData[current_].name);
    Logf("BOSS_DIRECT_SPAWN id=%s actor=%s route=default AI configuration; awaiting native ActivateAI/ForceIntoCombat", kData[current_].id, NameText(boss_name_).c_str());
    return true;
}
bool BossInitialized() {
    if (!Same(boss_, boss_name_) || !HasAIController(boss_)) return false;
    Object* controller = ObjectProperty(boss_, "Controller"), *component = ObjectProperty(controller, "SGameAIComponent");
    const int32_t initialized = Offset(component, "bInitialized"), started = Offset(component, "bAIStarted");
    return component && ObjectProperty(component, "Pawn") == boss_ && ObjectProperty(component, "AIController") == controller &&
        initialized == 705 && started == 704 && ue::At<uint8_t>(component, initialized) == 1 && ue::At<uint8_t>(component, started) == 1;
}
bool QueryGamePaused(const Context& context, bool& value) {
    Call paused(ue::Default("/Script/Engine.Default__GameplayStatics"), "IsGamePaused");
    if (!paused.Set("WorldContextObject", context.instance).Invoke()) return false;
    value = paused.Get<bool>("ReturnValue"); return true;
}
bool GamePaused(const Context& context) {
    bool value = true;
    return !QueryGamePaused(context, value) || value;
}
void UpdateEntryGuard(const Context& context) {
    if (!session_ || !context.pawn || !context.instance) return;
    if (!entry_armed_) { EntryProtection::Protect(context.pawn); return; }
    if (!EntryFocus::GameplayFocused()) {
        EntryProtection::Protect(context.pawn); protection_release_at_ = 0;
        if ((BossInitialized() || phase_ == Phase::Fighting || phase_ == Phase::Won || phase_ == Phase::Failed) &&
            !EntryProtection::Pause(context.world, context.instance, true)) {
            Status(L"暂停尚未通过验证，入场保护保持；可停止并返回"); return;
        }
        bool actual_pause = false;
        if (phase_ == Phase::Fighting && !combat_focus_paused_ && QueryGamePaused(context, actual_pause) && actual_pause) {
            combat_focus_paused_ = true;
            Status(L"%s 战斗已暂停（菜单操作或切出）；切回游戏继续", kData[current_].name);
            Logf("BOSS_FOCUS combat paused; temporary player protection retained");
        }
        return;
    }
    if (!EntryProtection::ReleasePause(context.instance)) { EntryProtection::Protect(context.pawn); return; }
    if (GamePaused(context)) { EntryProtection::Protect(context.pawn); protection_release_at_ = 0; return; }
    if (!((phase_ == Phase::Fighting && combat_verified_) || phase_ == Phase::Won)) {
        EntryProtection::Protect(context.pawn); protection_release_at_ = 0; return;
    }
    if (combat_focus_paused_ && !GamePaused(context)) {
        combat_focus_paused_ = false;
        Status(L"%s 战斗已继续；入场保护在恢复控制 2.5 秒后解除", kData[current_].name);
        Logf("BOSS_FOCUS combat resumed; grace=2500ms");
    }
    if (!EntryProtection::HasProtection()) return;
    if (!protection_release_at_) protection_release_at_ = GetTickCount64() + 2500;
    if (GetTickCount64() >= protection_release_at_) EntryProtection::ReleaseProtection();
}
void ActivateDirectBoss(const Context& context) {
    const ULONGLONG now = GetTickCount64();
    if (!direct_force_pending_ || !Same(boss_, boss_name_) || !context.pawn || now < next_force_attempt_) return;
    next_force_attempt_ = now + 1000;
    Object* library = AILibrary(); if (!library) return;
    Object* controller = ObjectProperty(boss_, "Controller");
    Object* component = ObjectProperty(controller, "SGameAIComponent");
    const int32_t initialized = Offset(component, "bInitialized"), started = Offset(component, "bAIStarted"), state_offset = Offset(component, "CurrentStateTag");
    if (!HasAIController(boss_) || ObjectProperty(component, "Pawn") != boss_ || ObjectProperty(component, "AIController") != controller ||
        initialized != 705 || started != 704 || state_offset != 560 ||
        ue::At<uint8_t>(component, initialized) != 1 || ue::At<uint8_t>(component, started) != 1) {
        Call activate(library, "ActivateAI");
        activate.Set("Pawn", boss_).Set("__WorldContext", context.instance).Invoke();
        return;
    }
    if (!entry_armed_ || !EntryFocus::GameplayFocused()) {
        EntryProtection::Protect(context.pawn); protection_release_at_ = 0;
        if (!EntryProtection::Pause(context.world, context.instance, true)) {
            Status(L"Boss 已就绪；暂停请求未完成，入场保护保持，尚未主动触战"); return;
        }
        deadline_ = now + 60000;
        if (now >= next_ai_bind_) { next_ai_bind_ = now + 1000; BossAI::BindTarget(boss_); }
        Status(L"%s 已就绪；菜单操作或切出期间暂停，切回游戏后开始战斗", kData[current_].name);
        if (!focus_wait_logged_) { focus_wait_logged_ = true; Logf("BOSS_ARMED actor=%s; waiting for gameplay focus, entry protection retained", NameText(boss_name_).c_str()); }
        return;
    }
    if (!EntryProtection::ReleasePause(context.instance) || GamePaused(context)) return;
    Call combat(library, "ForceIntoCombat");
    if (!combat.Set("Pawn", boss_).Set("TargetActor", context.pawn).Set("__WorldContext", context.instance).Invoke()) return;
    Call target(ue::Default("/Script/SGameAI.Default__SGameAIFunctionLibrary"), "GetTargetActor");
    const bool actual_target = target.Set("AIController", controller).Invoke() && target.Get<bool>("ReturnValue") && target.Get<Object*>("Target") == context.pawn;
    const std::string state = NameText(ue::At<Name>(component, state_offset));
    const bool actual_combat = state == "AI.State.Combat" || state.rfind("AI.State.Combat.", 0) == 0;
    if (actual_target && actual_combat) {
        direct_force_pending_ = false; combat_verified_ = true;
        Logf("BOSS_DIRECT_COMBAT verified actor=%s target=%s initialized=1 started=1 state=%s", NameText(boss_name_).c_str(), NameText(ue::NameOf(context.pawn)).c_str(), state.c_str());
    } else {
        Status(L"%s 的 AI 已启动；正在等待真实战斗目标读回，可重试或停止", kData[current_].name);
        Logf("BOSS_DIRECT_COMBAT pending actor=%s target_match=%d state=%s", NameText(boss_name_).c_str(), actual_target, state.c_str());
    }
}
void EngageBoss(const Context& context, Object* actor, float health) {
    if (!Same(boss_, boss_name_) || boss_ != actor || (!combat_verified_ && !direct_force_pending_)) {
        boss_ = actor; boss_name_ = ue::NameOf(actor); combat_verified_ = false;
        saw_alive_ = false; dead_since_ = 0;
        direct_force_pending_ = true; next_force_attempt_ = 0;
        phase_ = Phase::WaitingBoss; deadline_ = GetTickCount64() + 60000; Busy(false);
        Status(L"已识别 %s 的真实 Boss；正在触发并读回战斗状态，可重试或停止", kData[current_].name);
    }
    ActivateDirectBoss(context);
    if (combat_verified_) AcceptBoss(actor, health);
}
bool StreamingReady(const Context& context) {
    static Object* cached_world = nullptr, *streaming = nullptr;
    static Name cached_name;
    if (cached_world != context.world || !(cached_name == ue::NameOf(context.world)) || (streaming && !ue::Alive(streaming))) {
        cached_world = context.world; cached_name = ue::NameOf(context.world);
        streaming = Subsystem("/Script/Engine.WorldPartitionSubsystem", context.world);
    }
    if (!streaming) return GetTickCount64() > stage_time_ + 3000;
    Call ready(streaming, "IsAllStreamingCompleted");
    return ready.Invoke() && ready.Get<bool>("ReturnValue");
}
bool PlayerVitals(const Context& context, float& health, float& maximum) {
    Call hp(context.pawn, "GetHealth"), cap(context.pawn, "GetMaxHealth");
    if (!context.pawn) return false;
    if (hp.Invoke()) health = hp.Get<float>("ReturnValue");
    else if (!ReadAttributeValue(context.pawn, "Health", health)) return false;
    if (cap.Invoke()) maximum = cap.Get<float>("ReturnValue");
    else if (!ReadAttributeValue(context.pawn, "Health_Max", maximum)) return false;
    if (!std::isfinite(health) || !std::isfinite(maximum) || health <= 0 || maximum <= 0) return false;
    Call dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"), "IsDead");
    return dead.Set("Pawn", context.pawn).Invoke() && !dead.Get<bool>("ReturnValue");
}
bool PlayerDataReady(const Context& context) {
    const int32_t loading = Offset(context.instance, "IsLoadingLevel");
    float hp = 0, maximum = 0;
    auto wait = [&](const wchar_t* reason, int32_t pending = -2, int32_t nodes = -2) {
        player_ready_since_ = 0;
        Status(L"安全入场：%s；角色保持冻结和保护", reason);
        if (GetTickCount64() >= next_player_wait_log_) {
            next_player_wait_log_ = GetTickCount64() + 2000;
            Logf("BOSS_PLAYER_WAIT reason=%s health=%.3f max=%.3f pending_nodes=%d built_nodes=%d", Utf8(reason).c_str(), hp, maximum, pending, nodes);
        }
        return false;
    };
    if (loading < 0 || ue::At<uint8_t>(context.instance, loading)) return wait(L"等待地图加载流程完成");
    if (!PlayerVitals(context, hp, maximum)) return wait(L"等待主角存活体力数据完成");
    for (const char* function : {"IsMoveInputIgnored", "IsLookInputIgnored"}) {
        Call input(context.controller, function);
        if (!input.Invoke() || input.Get<bool>("ReturnValue")) return wait(L"等待主角恢复操作权");
    }
    if (!EntryProtection::Protect(context.pawn)) return wait(L"等待入场保护验证");
    {
        player_nodes_ = nullptr;
        Object* cls = ue::FindObject("/Script/ProjectPBZ.PBZAbilityNodesComponent");
        for (Object* actor : {context.pawn, context.controller}) {
            Call component(actor, "GetComponentByClass");
            if (component.Set("ComponentClass", cls).Invoke()) player_nodes_ = component.Get<Object*>("ReturnValue");
            if (player_nodes_ && ue::Alive(player_nodes_)) break;
        }
    }
    const int32_t pending = Offset(player_nodes_, "NeedLoadItems"), nodes = Offset(player_nodes_, "AbilityNodeMap");
    if (pending < 0 || nodes < 0) return wait(L"等待主角技能组件注册");
    const int32_t enabled = Offset(player_nodes_, "bEnableComponent");
    if (enabled < 0 || ue::At<uint8_t>(player_nodes_, enabled) != 1) return wait(L"等待主角技能组件启用");
    auto count = [&](int32_t offset) {
        const int32_t total = ue::At<int32_t>(player_nodes_, offset + 8), free = ue::At<int32_t>(player_nodes_, offset + 52);
        return total >= 0 && total <= 4096 && free >= 0 && free <= total ? total - free : -1;
    };
    const int32_t pending_count = count(pending), node_count = count(nodes);
    if (pending_count != 0 || node_count <= 0) return wait(L"等待技能资源完成", pending_count, node_count);
    const ULONGLONG now = GetTickCount64();
    if (!player_ready_since_) player_ready_since_ = now;
    if (now < player_ready_since_ + 500) { Status(L"安全入场：确认主角数据稳定；保护保持"); return false; }
    return true;
}
bool Heal(const Context& context) {
    float hp = 0, maximum = 0;
    if (!PlayerVitals(context, hp, maximum)) return false;
    if (!options_.heal_on_start) return true;
    Call restore(ue::Default("/Script/ProjectPBZ.Default__PBZRestoreQueryLibrary"), "ResetHealthToMax");
    bool applied = restore.Set("Target", context.pawn).Invoke() && restore.Get<bool>("ReturnValue");
    if (!applied) {
        Object* cheat = ObjectProperty(context.controller, "CheatManager");
        applied = cheat && Call(cheat, "GM_FullHeal").Invoke();
    }
    const bool verified = applied && PlayerVitals(context, hp, maximum) && hp >= maximum - 0.1f;
    Logf("BOSS_PLAYER_HEAL verified=%d health=%.3f max=%.3f", verified, hp, maximum);
    return verified;
}
void BeginReset(const Context& context) {
    spawner_ = FindSpawner(context, kData[current_]);
    spawner_name_ = spawner_ ? ue::NameOf(spawner_) : Name{};
    float health = -1; Object* existing = FindEncounterBoss(context, health, true);
    if (existing && (!force_reset_ || !spawner_ || bypass_spawner_)) {
        EngageBoss(context, existing, health);
        Logf("BOSS_ADOPT existing runtime encounter id=%s; no duplicate spawned", kData[current_].id);
        return;
    }
    if (bypass_spawner_) {
        if (spawner_) Call(spawner_, "SetShouldSpawn", "/Script/SGameAI.SGameAISpawnerInterface").Set("NewShouldSpawn", false).Invoke();
        BeginDirectSpawn(context); return;
    }
    if (!spawner_) { BeginDirectSpawn(context); return; }
    Object* target_class = LoadClass(kData[current_].class_path);
    if (!target_class || !NpcRetarget::BeforeActorClassSpawn(target_class)) {
        Fail(L"NPC 外观恢复尚未完成，已取消 Boss 重生"); return;
    }
    if (!BossAI::ReleaseTarget()) { Fail(L"旧 AI 参数尚未恢复，未重生 Boss"); return; }
    if (!Call(spawner_, "DeleteSaveData").Invoke()) { Fail(L"无法清除这次练习生成器的死亡记录"); return; }
    Call should_call(spawner_, "SetShouldSpawn", "/Script/SGameAI.SGameAISpawnerInterface");
    Call respawn(spawner_, "Respawn", "/Script/SGameAI.SGameAISpawnerInterface");
    if (should_call.Set("NewShouldSpawn", true).Invoke() && respawn.Invoke()) {
        phase_ = Phase::WaitingBoss; stage_time_ = GetTickCount64(); deadline_ = stage_time_ + 60000;
        Busy(false);
        Logf("BOSS_RESET native interface Respawn spawner=%s", NameText(spawner_name_).c_str());
        Status(L"等待原生生成器复活 %s 并完成 AI 初始化", kData[current_].name); return;
    }
    // Use the actual Blueprint entry points. Interface-only Respawn/SetShouldSpawn
    // are not in this actor's UFunction chain and must not be silently invoked as
    // if ordinary actor functions existed.
    if (!Call(spawner_, "Despawn").Invoke() || !Call(spawner_, "DeleteSaveData").Invoke()) {
        Fail(L"原生成器缺少已确认的清理接口"); return;
    }
    Object* old = Spawned(spawner_);
    if (old && ue::Alive(old)) Call(old, "K2_DestroyActor").Invoke();
    const int32_t should = Offset(spawner_, "ShouldSpawn");
    if (should < 0 || ue::At<uint8_t>(spawner_, should) > 1) { Fail(L"生成开关布局不符"); return; }
    ue::At<uint8_t>(spawner_, should) = 1;
    phase_ = Phase::Clearing; stage_time_ = GetTickCount64(); deadline_ = stage_time_ + 10000;
    Logf("BOSS_RESET spawner=%s; original save record removed only in practice memory", NameText(spawner_name_).c_str());
    Status(L"正在重置 %s 的原生成器与死亡状态", kData[current_].name);
}
void Advance(const Context& context) {
    if (!session_ || current_ < 0) { Status(L"请先选择并进入一场 Boss 战"); return; }
    if (!queue_.empty() && queue_.front() == current_) queue_.pop_front();
    if (queue_.empty()) {
        AcquireSRWLockExclusive(&snapshot_lock); snapshot_.running = false; ReleaseSRWLockExclusive(&snapshot_lock);
        phase_ = Phase::Won; Busy(false); Status(L"本轮 Boss Rush 已完成；停止可返回进入练习前的存档"); return;
    }
    force_reset_ = true;
    if (!BeginDestination(context, queue_.front()) && phase_ != Phase::Failed) Fail(L"下一位 Boss 传送失败");
}
void Execute(const Pending& pending, const Context& context) {
    if (pending.action == Action::Stop) {
        ReleaseMovement(); queue_.clear();
        if (!ReturnOriginal(context)) Fail(L"读取进入练习前的存档失败，存档保护未解除");
        return;
    }
    if (pending.action == Action::ApplyOptions) {
        options_ = pending.options; PublishOptions();
        if (session_ && !ApplyOptions(context)) { Fail(L"原生难度或周目接口拒绝参数"); return; }
        Status(L"参数已设置；Boss 的出生属性在重试或下一场生成时使用新值"); return;
    }
    if (pending.action == Action::Refresh) {
        Status(L"目录包含 %zu 个图鉴 Boss 阶段/变体；尚未定位入口的项保持禁用", std::size(kData)); return;
    }
    if (pending.action == Action::Next) { Advance(context); return; }
    if (pending.action == Action::Retry) {
        force_reset_ = true;
        if (current_ >= 0 && !BeginDestination(context, current_) && phase_ != Phase::Failed) Fail(L"重试失败"); return;
    }
    options_ = pending.options;
    bypass_spawner_ = pending.action == Action::DirectFight;
    force_reset_ = pending.action == Action::StartRush || options_.override_cycle || options_.override_difficulty;
    if (pending.action == Action::StartRush) {
        queue_.clear();
        for (size_t i = static_cast<size_t>(pending.entry); i < std::size(kData); ++i)
            if (kData[i].has_destination) queue_.push_back(static_cast<int32_t>(i));
        AcquireSRWLockExclusive(&snapshot_lock); snapshot_.running = true; snapshot_.completed = 0; ReleaseSRWLockExclusive(&snapshot_lock);
    } else {
        queue_.clear(); queue_.push_back(pending.entry);
        AcquireSRWLockExclusive(&snapshot_lock); snapshot_.running = false; snapshot_.completed = 0; ReleaseSRWLockExclusive(&snapshot_lock);
    }
    if (!BeginDestination(context, pending.entry) && phase_ != Phase::Failed) Fail(L"无法开始练习会话或目标入口尚不可用");
}
void TickEncounter(const Context& context) {
    const ULONGLONG now = GetTickCount64();
    if (now < next_encounter_tick_) return;
    next_encounter_tick_ = now + 50;
    if (phase_ == Phase::Idle || phase_ == Phase::Failed || phase_ == Phase::Won) return;
    if (!context.world || !ue::Alive(context.world)) return;
    if (encounter_world_ && phase_ != Phase::Loading && phase_ != Phase::Returning && phase_ != Phase::Positioning &&
        (context.world != encounter_world_ || !(ue::NameOf(context.world) == encounter_world_name_))) {
        Fail(L"实际地图已经改变，旧战斗跟踪已停止"); return;
    }
    if (phase_ == Phase::WaitingBoss && BossInitialized() && !EntryFocus::GameplayFocused()) deadline_ = now + 60000;
    if (phase_ == Phase::Fighting && GamePaused(context)) return;
    if (phase_ != Phase::Fighting && now > deadline_) {
        float health = -1;
        Object* actor = phase_ == Phase::WaitingBoss ? FindEncounterBoss(context, health, true) : nullptr;
        if (actor && combat_verified_) { AcceptBoss(actor, health); return; }
        Fail(L"未检测到所选 Boss 的有效战斗实例；可重试或停止返回"); return;
    }
    if (phase_ == Phase::Loading || phase_ == Phase::Returning) {
        if (context.world == prior_world_ && ue::NameOf(context.world) == prior_world_name_) return;
        if (!returning_ && !ue::NameIs(ue::NameOf(context.world), NameText(ue::MakeName(kData[current_].level)).c_str())) return;
        phase_ = Phase::Positioning; stage_time_ = now; positioned_ = false;
    }
    if (phase_ == Phase::Positioning) {
        if (!context.pawn || !context.controller) return;
        EntryProtection::Protect(context.pawn);
        if (!HoldMovement(context.pawn)) { Fail(L"无法暂停角色移动，已取消不安全入场"); return; }
        if (!positioned_ || !Same(positioned_pawn_, positioned_pawn_name_) || positioned_pawn_ != context.pawn) {
            const Transform target = EntrySafety::ParkingTransform(returning_ ? original_transform_ : kData[current_].destination);
            const bool no = false, yes = true;
            Call move(context.pawn, "K2_SetActorTransform");
            if (!HoldMovement(context.pawn) || !move.Set("NewTransform", target).Set("bSweep", no).Set("bTeleport", yes).Invoke() ||
                !move.Get<bool>("ReturnValue")) { Fail(L"角色定位或加载期间移动保护失败"); return; }
            Call check(context.pawn, "K2_GetActorLocation");
            if (!check.Invoke()) { Fail(L"无法验证角色实际位置"); return; }
            const Vector location = check.Get<Vector>("ReturnValue");
            const double dx = location.x - target.translation[0], dy = location.y - target.translation[1], dz = location.z - target.translation[2];
            if (!std::isfinite(dx + dy + dz) || dx * dx + dy * dy + dz * dz > 100) { Fail(L"角色未到达目标位置，未开始生成 Boss"); return; }
            positioned_ = true; positioned_pawn_ = context.pawn; positioned_pawn_name_ = ue::NameOf(context.pawn);
            stage_time_ = now; player_ready_since_ = 0; player_nodes_ = nullptr;
        }
        if (now < stage_time_ + 1000 || !StreamingReady(context)) { Status(L"安全入场：等待场景碰撞加载；角色保持冻结和保护"); return; }
        if (!PlayerDataReady(context)) return;
        if (!returning_ && !entry_options_applied_) {
            if (!ApplyOptions(context)) { Fail(L"设置本场难度或周目失败"); return; }
            entry_options_applied_ = true; player_ready_since_ = 0; return;
        }
        const Transform anchor = returning_ ? original_transform_ : kData[current_].destination;
        if (!EntrySafety::IsSafe() && !EntrySafety::PlacePlayer(context.pawn, context.instance, anchor, returning_)) {
            Fail(L"未找到可站立且无重叠的落点，角色保持冻结；请停止并返回"); return;
        }
        if (!returning_ && !Heal(context)) { Fail(L"体力恢复未通过读回，已取消开战"); return; }
        if (!EntrySafety::ReleasePawn()) { Status(L"已找到安全地面，等待移动状态恢复；保护保持"); return; }
        entry_armed_ = true; protection_release_at_ = 0;
        if (returning_) {
            Object* save = Save(context), *game = Game(context);
            const uint8_t difficulty = static_cast<uint8_t>(original_difficulty_);
            if (!game || !InvokeInt(game, "SetCycleCount", "InCycleCount", original_cycle_) ||
                !Call(game, "SetDifficultMode").Set("DifficultMode", difficulty).Invoke() ||
                !BossRecordGuard::Restore(save) ||
                !EntryProtection::ReleaseProtection() || !EntryProtection::ReleasePause(context.instance) ||
                !ReleasePracticeVeto(save)) { Fail(L"恢复原始参数或入场保护失败"); return; }
            UnprotectAdvanced(); BossRecordGuard::Clear();
            session_ = false; returning_ = false; entry_armed_ = false; phase_ = Phase::Idle; current_ = -1;
            Busy(false); PublishOptions(); Status(L"已返回原存档与位置，恢复原周目/难度并解除练习存档保护");
            Logf("BOSS_SESSION original slot reloaded; save veto released"); return;
        }
        encounter_world_ = context.world; encounter_world_name_ = ue::NameOf(context.world);
        BeginReset(context); return;
    }
    if (phase_ == Phase::Clearing) {
        if (!Same(spawner_, spawner_name_)) { BeginDirectSpawn(context); return; }
        if (now < stage_time_ + 250) return;
        Object* old = Spawned(spawner_);
        if (old && ue::Alive(old)) {
            Call destroying(old, "IsActorBeingDestroyed");
            if (!destroying.Invoke() || !destroying.Get<bool>("ReturnValue")) return;
        }
        Object* target_class = LoadClass(kData[current_].class_path);
        if (!target_class || !NpcRetarget::BeforeActorClassSpawn(target_class) || !Call(spawner_, "Spawn").Invoke()) {
            Fail(L"外观恢复或原生成器生成接口尚未就绪"); return;
        }
        phase_ = Phase::WaitingBoss; stage_time_ = now; deadline_ = now + 60000;
        Busy(false);
        Status(L"等待 %s 完成生成与 AI 初始化", kData[current_].name); return;
    }
    if (phase_ == Phase::WaitingBoss || phase_ == Phase::Fighting) {
        ActivateDirectBoss(context);
        float health = -1;
        if (Object* actor = FindEncounterBoss(context, health)) {
            EngageBoss(context, actor, health);
            return;
        }
        if (Same(boss_, boss_name_) && InWorld(boss_, context.world)) {
            if (Dead(boss_, health) && saw_alive_) {
                if (!dead_since_) { dead_since_ = now; Logf("BOSS_DEATH observed id=%s health=%.3f", kData[current_].id, health); }
            }
        } else if (phase_ == Phase::WaitingBoss && !Same(spawner_, spawner_name_) && !direct_spawn_attempted_) {
            BeginDirectSpawn(context); return;
        }
        // A disappearing or unloaded actor alone is not proof of a victory.
        if (dead_since_ && now >= dead_since_ + 5000) {
            AcquireSRWLockExclusive(&snapshot_lock); ++snapshot_.completed; const bool automatic = snapshot_.running && options_.auto_next; ReleaseSRWLockExclusive(&snapshot_lock);
            phase_ = Phase::Won; Status(L"%s 的目标个体已确认死亡；后续剧情阶段请手动确认，可下一位或重试", kData[current_].name);
            if (automatic) Advance(context);
        }
    }
}

void Tick() {
    if (!initialized_) return;
    if (cleanup_pending_) { cleanup_pending_ = false; ReleaseMovement(); }
    Context context;
    if (!ContextNow(context)) {
        // During world replacement the persistent GI still owns our save veto.
        // It must remain possible to process Stop and recovery timeouts.
        if (session_ && Same(session_instance_, session_instance_name_)) context.instance = session_instance_;
        else return;
    }
    if (GetTickCount64() >= next_context_) {
        next_context_ = GetTickCount64() + 1000;
        Object* game = Subsystem("/Script/ProjectPBZ.PBZGameSystem", context.world);
        Object* save = Subsystem("/Script/ProjectPBZ.PBZSaveLoadSubsystem", context.instance);
        const bool cycle = game && ue::FindFunction(ue::ClassOf(game), "SetCycleCount");
        const bool difficulty = game && ue::FindFunction(ue::ClassOf(game), "SetDifficultMode");
        AcquireSRWLockExclusive(&snapshot_lock);
        snapshot_.ready = save && game && context.pawn; snapshot_.cycle_supported = cycle; snapshot_.difficulty_supported = difficulty;
        ReleaseSRWLockExclusive(&snapshot_lock);
        if (save && game && context.pawn && phase_ == Phase::Idle)
            Status(L"已就绪：%zu 个 Boss 阶段/变体；可选择原生难度、周目和原场景练习重生", std::size(kData));
    }
    Pending pending;
    if (Pop(pending)) Execute(pending, context);
    UpdateEntryGuard(context);
    TickEncounter(context);
}
DWORD fault_code_ = 0; uintptr_t fault_address_ = 0;
int FaultFilter(const EXCEPTION_POINTERS* exception) {
    fault_code_ = exception->ExceptionRecord->ExceptionCode;
    fault_address_ = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
void GuardedTick() {
    __try { Tick(); }
    __except (FaultFilter(GetExceptionInformation())) {
        phase_ = Phase::Failed; cleanup_pending_ = true; Busy(false);
        AcquireSRWLockExclusive(&snapshot_lock); snapshot_.running = false; ReleaseSRWLockExclusive(&snapshot_lock);
        Logf("BOSS_RUSH fault code=0x%08lx address=%p; request stopped", fault_code_, reinterpret_cast<void*>(fault_address_));
        Status(L"Boss Rush 出错，已停止当前请求（0x%08lx）；原有调试功能继续运行", fault_code_);
    }
}
}

void Initialize(Logger logger, const wchar_t* directory) {
    (void)directory; logger_ = logger; BossRecordGuard::Initialize(logger);
    EntrySafety::Initialize(logger); EntryProtection::Initialize(logger);
    Snapshot state;
    for (size_t i = 0; i < std::size(kData); ++i) {
        const DataEntry& data = kData[i];
        state.entries.push_back({static_cast<int32_t>(i), data.id, data.name, data.level, data.detail, data.has_destination});
    }
    state.rank_description = L"未发现独立 Rank 接口；使用游戏原生难度、周目与已确认的强度参数";
    state.difficulties = {{1, L"入阵者 / Easy (1)"}, {2, L"破局者 / Normal (2)"}, {3, L"Hard (3)"}, {4, L"NightMare (4)"}};
    state.cycle_max = 9999;
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_ = std::move(state); ReleaseSRWLockExclusive(&snapshot_lock);
    initialized_ = true;
    Logf("BOSS_RUSH catalog=%zu initialized; engine calls only on game thread", std::size(kData));
}
Snapshot GetSnapshot() {
    AcquireSRWLockShared(&snapshot_lock); Snapshot copy = snapshot_; ReleaseSRWLockShared(&snapshot_lock); return copy;
}
bool IsTransitioning() {
    AcquireSRWLockShared(&snapshot_lock); const bool busy = snapshot_.busy; ReleaseSRWLockShared(&snapshot_lock); return busy;
}
bool AllowsAppearanceChanges() {
    if (phase_ != Phase::Idle && phase_ != Phase::WaitingBoss && phase_ != Phase::Fighting && phase_ != Phase::Won) return false;
    Context context;
    if (!ContextNow(context) || !context.pawn || !context.controller) return false;
    const int32_t loading = Offset(context.instance, "IsLoadingLevel");
    float health = 0, maximum = 0;
    if (loading < 0 || ue::At<uint8_t>(context.instance, loading) || !PlayerVitals(context, health, maximum)) return false;
    if (phase_ == Phase::Idle) return true;
    return entry_armed_ && (phase_ != Phase::WaitingBoss || BossInitialized());
}
bool Request(Action action, int32_t entry, const Options& options) {
    if (action != Action::Stop && !ValidOptions(options)) { Status(L"参数超出当前已确认的游戏接口范围，请修改后重试"); return false; }
    if ((action == Action::Teleport || action == Action::DirectFight || action == Action::StartRush) &&
        (entry < 0 || static_cast<size_t>(entry) >= std::size(kData) || !kData[entry].has_destination)) {
        Status(L"这个 Boss 的入口尚未确认，未执行传送"); return false;
    }
    AcquireSRWLockExclusive(&request_lock);
    const bool accepted = requests_.size() < 8;
    if (accepted) requests_.push_back({action, entry, options});
    ReleaseSRWLockExclusive(&request_lock);
    if (accepted) Status(L"请求已排队，等待游戏线程处理");
    return accepted;
}
void OnGameThreadGuarded() { GuardedTick(); }
bool HandleCommand(const char* verb, const char* arguments) {
    if (std::strcmp(verb, "boss") && std::strcmp(verb, "rush")) return false;
    std::istringstream input(arguments ? arguments : ""); std::string action, id; input >> action >> id;
    const Snapshot state = GetSnapshot();
    if (action == "list") {
        for (const Entry& entry : state.entries) Logf("BOSS_CATALOG %d id=%s available=%d name=%ls map=%ls", entry.index,
            entry.id.c_str(), entry.available ? 1 : 0, entry.name.c_str(), entry.map.c_str());
        return true;
    }
    if (action == "status") { Logf("BOSS_STATUS ready=%d busy=%d running=%d current=%d text=%s", state.ready, state.busy, state.running,
        state.current, Utf8(state.status).c_str()); return true; }
    int32_t selected = state.current >= 0 ? state.current : 0;
    if (!id.empty()) {
        bool found = false;
        for (const Entry& entry : state.entries) if (entry.id == id || std::to_string(entry.index) == id) { selected = entry.index; found = true; break; }
        if (!found) return false;
    }
    if (action == "teleport") return Request(Action::Teleport, selected, state.options);
    if (action == "direct") return Request(Action::DirectFight, selected, state.options);
    if (action == "start") return Request(Action::StartRush, selected, state.options);
    if (action == "next") return Request(Action::Next, -1, state.options);
    if (action == "retry") return Request(Action::Retry, -1, state.options);
    if (action == "stop") return Request(Action::Stop, -1, state.options);
    if (action == "refresh") return Request(Action::Refresh, -1, state.options);
    return false;
}
}
