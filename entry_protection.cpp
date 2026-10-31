#include "entry_protection.h"
#include <cstdarg>
#include <cstdio>
#include <iterator>

namespace EntryProtection {
namespace {
using ue::Object;
using ue::Name;
using ue::Call;
struct Identity { Object* object = nullptr; Name name; int32_t index = 0, serial = 0; };
struct Tag { Name name; };
static_assert(sizeof(Tag) == 8);
enum class TagState { None, Pending, Owned, Removing };
Logger logger_ = nullptr;
Identity pawn_, asc_, pause_world_, pause_instance_;
Tag tag_;
TagState tag_state_ = TagState::None;
bool actor_owned_ = false, original_damage_ = false, pause_owned_ = false, pause_pending_ = false;
int32_t original_count_ = 0, expected_removed_count_ = 0;
SRWLOCK snapshot_lock = SRWLOCK_INIT;
Snapshot snapshot_;

void Log(const char* format, ...) {
    if (!logger_) return;
    char line[400]{}; va_list args; va_start(args, format);
    vsprintf_s(line, format, args); va_end(args); logger_(line);
}
void Publish() {
    Snapshot value{actor_owned_, tag_state_ == TagState::Owned, pause_owned_,
        (actor_owned_ && tag_state_ != TagState::Owned) || tag_state_ == TagState::Pending ||
        tag_state_ == TagState::Removing || pause_pending_, original_count_};
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_ = value; ReleaseSRWLockExclusive(&snapshot_lock);
}
bool Readable(const void* pointer, size_t bytes) {
    MEMORY_BASIC_INFORMATION memory{};
    const uintptr_t address = reinterpret_cast<uintptr_t>(pointer);
    if (!pointer || !VirtualQuery(pointer, &memory, sizeof(memory)) || memory.State != MEM_COMMIT ||
        memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return address >= base && bytes <= memory.RegionSize && address - base <= memory.RegionSize - bytes;
}
bool HelpersMatch() {
    static constexpr unsigned char weak[] = {0x40, 0x53, 0x48, 0x83, 0xec, 0x20, 0x48, 0x8b,
        0x12, 0x48, 0x8b, 0xd9, 0x48, 0x85, 0xd2, 0x74};
    static constexpr unsigned char damage[] = {0x80, 0x49, 0x5a, 0x04, 0xc3};
    return ue::Base() &&
        !std::memcmp(reinterpret_cast<const void*>(ue::Base() + 0x0166f2a0), weak, sizeof(weak)) &&
        !std::memcmp(reinterpret_cast<const void*>(ue::Base() + 0x03a9cbf0), damage, sizeof(damage));
}
const unsigned char* Item(int32_t index) {
    auto* array = reinterpret_cast<unsigned char*>(ue::Base() + ue::layout::kObjectArray);
    if (index < 0 || !Readable(array, 40) || index >= ue::At<int32_t>(array, 32)) return nullptr;
    auto* table = ue::At<unsigned char**>(array, 24);
    const int32_t chunk = index / 65536;
    if (!Readable(table, (static_cast<size_t>(chunk) + 1) * sizeof(void*)) || !table[chunk]) return nullptr;
    const unsigned char* item = table[chunk] + static_cast<size_t>(index % 65536) * 24;
    return Readable(item, 24) ? item : nullptr;
}
bool Same(const Identity& id) {
    const unsigned char* item = id.object && id.serial > 0 ? Item(id.index) : nullptr;
    if (!item) return false;
    int32_t serial = 0; uint32_t flags = 0; Object* object = nullptr;
    std::memcpy(&serial, item + 16, sizeof(serial)); std::memcpy(&flags, item + 4, sizeof(flags));
    std::memcpy(&object, item + 8, sizeof(object));
    return serial == id.serial && !(flags & 0x10200000u) && object == id.object &&
        ue::Alive(object) && ue::NameOf(object) == id.name;
}
bool Capture(Object* object, Identity& result) {
    if (!object || !ue::Alive(object) || ue::IsTemplate(object) || !HelpersMatch()) return false;
    struct Weak { int32_t index = 0, serial = 0; } weak;
    reinterpret_cast<void(__fastcall*)(Weak*, Object* const*)>(ue::Base() + 0x0166f2a0)(&weak, &object);
    Identity candidate{object, ue::NameOf(object), weak.index, weak.serial};
    if (!Same(candidate)) return false;
    result = candidate; return true;
}
bool DamageLayout() {
    Object* actor = ue::FindObject("/Script/Engine.Actor");
    if (!actor || ue::Offset(actor, "bCanBeDamaged") != 0x5a) return false;
    for (auto* field = ue::At<unsigned char*>(actor, 0x50); field; field = ue::At<unsigned char*>(field, 0x10)) {
        if (!ue::NameIs(ue::At<Name>(field, 0x20), "bCanBeDamaged")) continue;
        static constexpr unsigned char expected[] = {1, 0, 4, 4};
        return ue::At<int32_t>(field, 0x34) == 1 && !std::memcmp(field + 0x70, expected, sizeof(expected));
    }
    return false;
}
Object* ASC(Object* pawn) {
    Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetAbilitySystemComponent");
    return get.Set("Actor", pawn).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
}
bool Count(int32_t& value) {
    if (!Same(asc_) || tag_.name.None()) return false;
    Call count(asc_.object, "GetGameplayTagCount");
    if (!count.Set("GameplayTag", tag_).Invoke()) return false;
    value = count.Get<int32_t>("ReturnValue");
    return count.Ok() && value >= 0 && value < 1000000;
}
bool ChangeTag(bool add) {
    if (!Same(asc_)) return false;
    const bool replicate = false;
    Call change(ue::Default("/Script/SGameUtils.Default__AbilityUtils"), add ?
        "AddLooseGameplayTagForSystem" : "RemoveLooseGameplayTagForSystem");
    return change.Set("AbilitySystem", asc_.object).Set("GameplayTag", tag_).Set("bShouldReplicate", replicate).Invoke();
}
void ClearProtection() { pawn_ = {}; asc_ = {}; tag_ = {}; tag_state_ = TagState::None; actor_owned_ = false; original_damage_ = false; original_count_ = expected_removed_count_ = 0; }
bool ReleaseProtectionImpl() {
    if (tag_state_ != TagState::None) {
        if (!Same(asc_)) tag_state_ = TagState::None; // Its weak instance no longer exists; never touch a reused slot.
        else {
            int32_t current = 0;
            if (!Count(current)) return false;
            if (tag_state_ == TagState::Removing) {
                if (current != expected_removed_count_) return false;
                tag_state_ = TagState::None;
            }
            else if (current <= original_count_) tag_state_ = TagState::None; // Our increment has already gone.
            else if (tag_state_ == TagState::Pending) return false; // Exception made ownership uncertain; do not remove another source.
            else {
                expected_removed_count_ = current - 1;
                // Mark removal before entering engine code: an exception may
                // occur after it consumed the decrement. Never decrement twice.
                tag_state_ = TagState::Removing; Publish();
                if (!ChangeTag(false)) { tag_state_ = TagState::Owned; return false; }
                // We dispatched exactly one decrement. If readback faults,
                // do not dispatch it again and remove someone else's count.
                int32_t after = 0;
                if (!Count(after) || after != expected_removed_count_) { Publish(); return false; }
                tag_state_ = TagState::None;
            }
        }
    }
    if (actor_owned_ && Same(pawn_)) {
        if (!DamageLayout()) return false;
        uint8_t& flags = ue::At<uint8_t>(pawn_.object, 0x5a);
        flags = original_damage_ ? static_cast<uint8_t>(flags | 4u) : static_cast<uint8_t>(flags & ~4u);
        if (static_cast<bool>(flags & 4u) != original_damage_) return false;
    }
    const bool had = actor_owned_ || asc_.object;
    ClearProtection(); Publish();
    if (had) Log("BOSS_ENTRY protection released; restored original Actor damage bit and removed only owned tag increment");
    return true;
}
bool ProtectImpl(Object* pawn) {
    if ((actor_owned_ || asc_.object) && (!Same(pawn_) || pawn_.object != pawn))
        if (!ReleaseProtectionImpl()) return false;
    if (!actor_owned_) {
        Object* actor = ue::FindObject("/Script/Engine.Actor");
        Identity candidate;
        if (!pawn || !ue::Alive(pawn) || !actor || !ue::IsA(pawn, actor) || !DamageLayout() || !Capture(pawn, candidate)) return false;
        pawn_ = candidate; original_damage_ = (ue::At<uint8_t>(pawn, 0x5a) & 4u) != 0;
        actor_owned_ = true;
    }
    ue::At<uint8_t>(pawn_.object, 0x5a) &= static_cast<uint8_t>(~4u);
    Publish(); // Partial protection is real even if ASC initialization is pending.
    if (!Same(pawn_) || ue::At<uint8_t>(pawn_.object, 0x5a) & 4u) return false;
    Object* current_asc = ASC(pawn_.object);
    if (asc_.object && (!Same(asc_) || current_asc != asc_.object)) {
        // Restore old ownership before following a newly initialized ASC.
        if (!ReleaseProtectionImpl()) return false;
        return ProtectImpl(pawn);
    }
    if (!asc_.object) {
        if (!Capture(current_asc, asc_)) return false;
        tag_.name = ue::MakeName(L"CharacterState.Stand.Invulnerable.Debug");
        if (tag_.name.None()) return false;
    }
    int32_t current = 0;
    if (!Count(current)) return false;
    if (tag_state_ == TagState::Removing) return false;
    if (tag_state_ == TagState::Pending) {
        if (current == original_count_ + 1) tag_state_ = TagState::Owned;
        else if (current == original_count_) tag_state_ = TagState::None;
        else return false;
    }
    if (tag_state_ == TagState::Owned) { Publish(); return current > original_count_; }
    original_count_ = current; tag_state_ = TagState::Pending; Publish();
    if (!ChangeTag(true)) { tag_state_ = TagState::None; Publish(); return false; }
    tag_state_ = TagState::Owned; Publish();
    int32_t after = 0;
    if (!Count(after) || after != original_count_ + 1) return false;
    Log("BOSS_ENTRY protection verified tag count %d->%d; Actor damage disabled; GodMode untouched", original_count_, after);
    return true;
}
Object* Gameplay() { return ue::Default("/Script/Engine.Default__GameplayStatics"); }
bool IsPaused(Object* world, bool& value) {
    Call get(Gameplay(), "IsGamePaused");
    if (!get.Set("WorldContextObject", world).Invoke()) return false;
    value = get.Get<bool>("ReturnValue"); return get.Ok();
}
bool SetPaused(Object* world, bool value) {
    Call set(Gameplay(), "SetGamePaused");
    return set.Set("WorldContextObject", world).Set("bPaused", value).Invoke() && set.Get<bool>("ReturnValue") && set.Ok();
}
void ClearPause() { pause_world_ = {}; pause_instance_ = {}; pause_owned_ = pause_pending_ = false; }
bool ReleasePauseImpl(Object* instance) {
    if (!pause_owned_ && !pause_pending_) return true;
    if (!Same(pause_world_)) { ClearPause(); Publish(); return true; }
    if (!Same(pause_instance_) || pause_instance_.object != instance) return false;
    bool current = false;
    if (!IsPaused(pause_world_.object, current)) return false;
    if (pause_pending_) {
        // A failed native dispatch may have left the world unchanged. That
        // case can be cleaned up, while an uncertain active pause is retained.
        if (current) return false;
        ClearPause(); Publish(); return true;
    }
    if (current && !SetPaused(pause_world_.object, false)) return false;
    bool after = true;
    if (!IsPaused(pause_world_.object, after) || after) return false;
    ClearPause(); Publish(); Log("BOSS_ENTRY owned pause released"); return true;
}
bool PauseImpl(Object* world, Object* instance, bool requested) {
    if (!requested) return ReleasePauseImpl(instance);
    if ((pause_owned_ || pause_pending_) && (!Same(pause_world_) || pause_world_.object != world ||
        !Same(pause_instance_) || pause_instance_.object != instance)) if (!ReleasePauseImpl(instance)) return false;
    if (pause_pending_ && !ReleasePauseImpl(instance)) return false;
    bool current = false;
    if (!world || !instance || !ue::Alive(world) || !ue::Alive(instance)) return false;
    const int32_t owner = ue::Offset(ue::ClassOf(world), "OwningGameInstance");
    if (owner < 0 || ue::At<Object*>(world, owner) != instance || !IsPaused(world, current)) return false;
    if (current) return true; // Never claim or later release an existing external pause.
    if (pause_owned_) { ClearPause(); Publish(); }
    Identity w, gi;
    if (!Capture(world, w) || !Capture(instance, gi)) return false;
    pause_world_ = w; pause_instance_ = gi; pause_pending_ = true; Publish();
    if (!SetPaused(world, true)) { ClearPause(); Publish(); return false; }
    pause_pending_ = false; pause_owned_ = true; Publish();
    bool after = false;
    if (!IsPaused(world, after) || !after) return false;
    Log("BOSS_ENTRY pause acquired from unpaused state"); return true;
}
DWORD fault_code_ = 0; void* fault_address_ = nullptr;
int FaultFilter(const EXCEPTION_POINTERS* exception) {
    fault_code_ = exception->ExceptionRecord->ExceptionCode;
    fault_address_ = exception->ExceptionRecord->ExceptionAddress;
    return EXCEPTION_EXECUTE_HANDLER;
}
void Fault(const char* action) {
    Publish(); Log("BOSS_ENTRY %s fault code=0x%08lx address=%p; ownership retained for cleanup retry", action, fault_code_, fault_address_);
}
}

void Initialize(Logger logger) {
    __try { logger_ = logger; Publish(); }
    __except (FaultFilter(GetExceptionInformation())) { Fault("initialize"); }
}
bool Protect(Object* pawn) {
    __try { const bool ok = ProtectImpl(pawn); Publish(); return ok; }
    __except (FaultFilter(GetExceptionInformation())) { Fault("protect"); return false; }
}
bool ReleaseProtection() {
    __try { const bool ok = ReleaseProtectionImpl(); Publish(); return ok; }
    __except (FaultFilter(GetExceptionInformation())) { Fault("release protection"); return false; }
}
bool Pause(Object* world, Object* instance, bool requested) {
    __try { const bool ok = PauseImpl(world, instance, requested); Publish(); return ok; }
    __except (FaultFilter(GetExceptionInformation())) { Fault("pause"); return false; }
}
bool ReleasePause(Object* instance) {
    __try { const bool ok = ReleasePauseImpl(instance); Publish(); return ok; }
    __except (FaultFilter(GetExceptionInformation())) { Fault("release pause"); return false; }
}
Snapshot GetSnapshot() {
    AcquireSRWLockShared(&snapshot_lock); const Snapshot value = snapshot_; ReleaseSRWLockShared(&snapshot_lock); return value;
}
bool HasProtection() {
    const Snapshot value = GetSnapshot(); return value.actor_protected || value.tag_added;
}
}
