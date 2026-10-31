#include "air_skills.h"
#include "ue_runtime.h"
#include "settings_store.h"
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cctype>
#include <cstring>
#include <sstream>
#include <vector>

namespace AirSkills {
namespace {
using ue::Object; using ue::Name; using ue::Call; using ue::Array;
struct Vector { double x = 0, y = 0, z = 0; };
struct Transform { double rotation[4]{}; Vector translation; double pad0 = 0; Vector scale; double pad1 = 0; };
struct Tags { Array tags, parents; };
struct Attribute { unsigned char bytes[56]{}; };
struct Identity { Object* object = nullptr; Name name; int32_t index = -1, serial = 0; };
enum class RefState { Acquiring, Owned, Releasing };
struct Strong { Identity identity; RefState state = RefState::Acquiring; };
struct Mapping { Identity action; int32_t references = 0, cached_items = 0; };
struct Clone { Identity original, copy; uint32_t changes = 0; std::vector<Mapping> mappings; };
struct Removed {
    Identity ability; int32_t offset=-1; Name tag;
    bool original_present=true,last_present=false;
};
struct Context { Object* world = nullptr, *controller = nullptr, *pawn = nullptr, *movement = nullptr, *input = nullptr, *sinput = nullptr, *asc = nullptr; };
struct CancelStamp {
    Identity ability;
    Object* asc=nullptr;
    int32_t handle=0;
    unsigned char activation[20]{};
    uint64_t task_hash=0;
    int32_t task_count=0;
    Identity tasks[256]{};
    Object* montage=nullptr;
};
struct FlagLease { CancelStamp stamp; Identity asc; bool original=false,cancel_invoked=false; };
struct PendingStep { Identity pawn,enemy; Vector start; ULONGLONG requested=0; float speed=0; std::vector<Identity> cancelled; };
struct PendingJump {
    Identity pawn; ULONGLONG requested=0; std::vector<Identity> cancelled;
    bool activated=false;
    ULONGLONG next_attempt=0;
    Vector start;
};
struct SlowFall { Identity pawn,movement; bool armed=false,owned=false,yielded=false,seen_descent=false; float original=0,last=0,cap=0; };
enum class EnemyReason { Other,NoAI,TeamQuery,NotHostile,DeadQuery,Dead,HealthLayout,HealthZero,Accepted };
struct EnemyScan {
    uint32_t pawns=0,no_ai=0,team=0,health=0,accepted=0;
    uint32_t team_query=0,not_hostile=0,dead=0,health_layout=0,health_zero=0;
};
struct NearScan { uint32_t valid=0,capsules=0,in_range=0,visible=0; float nearest=-1; bool player_ready=false; };
static_assert(sizeof(Transform) == 96 && sizeof(Tags) == 32 && sizeof(Attribute) == 56, "PBZ native layouts");

SRWLOCK state_lock = SRWLOCK_INIT, request_lock = SRWLOCK_INIT;
Snapshot state_;
Options options_, pending_;
bool has_request_ = false, initialized_ = false, last_jump_ = false, jump_baselined_ = false;
Logger logger_ = nullptr;
InputFocus input_focus_=nullptr;
Identity owner_, input_, blocked_world_;
Identity jump_identity_;
Identity combatart_action_, shachi_action_;
std::vector<Name> combatart_triggers_, shachi_triggers_;
std::vector<Name> air_skill_triggers_;
Identity light_action_,heavy_action_;
std::vector<Name> attack_triggers_;
Object* engine_ = nullptr, *jump_action_ = nullptr;
Name jump_name_;
std::vector<Clone> clones_;
std::vector<Removed> removed_;
std::vector<FlagLease> cancel_flags_;
std::vector<Identity> enemies_;
// Counted UObject strong references, not a manual RootSet flag. Only original
// world-independent native AIM assets and their transient copies are retained
// between frames. GAS instances remain weak: their ASC spec owns their lifetime.
std::vector<Strong> strong_;
bool restore_pending_ = false;
PendingStep step_;
PendingJump jump_request_;
SlowFall slow_;
Name jump_tag_;
int32_t jump_priority_=-7;
int32_t native_jump_priority_=0; // AIM_Hun_Jump's own ActionPriority (first non-zero item)
bool jump_priority_ready_=false;
EnemyScan enemy_scan_;
NearScan near_scan_;
struct GateLog { const char* reason; ULONGLONG next=0; };
std::vector<GateLog> gate_logs_;
uint64_t gate_sequence_=0;
ULONGLONG next_refresh_ = 0, next_enemies_ = 0, last_bounce_ = 0;
volatile LONG disabled_ = 0;
DWORD fault_code_ = 0;
uint32_t faults_ = 0;
using UnregisterNative=void(__fastcall*)(Object*,const void*);
UnregisterNative unregister_original_=nullptr;
unsigned char* unregister_trampoline_=nullptr;
bool unregister_installed_=false;
bool unregister_verified_=false;
DWORD unregister_entry_protection_=0;
DWORD game_thread_=0;
uint64_t binding_signature_=0;
bool binding_ready_=false;
uint32_t owned_count_=0;
const char* context_reason_=nullptr;
ULONGLONG next_context_log_=0;
bool EnsureUnregisterHook();
bool RemoveUnregisterHook();
bool RestoreSlowFall();
bool RestoreCancelFlags();
bool GameplayInputAllowed(const Context& context);

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[1024]{}; va_list args; va_start(args, format); vsprintf_s(text, format, args); va_end(args); logger_(text);
}
void Status(const wchar_t* format, ...) {
    wchar_t text[640]{}; va_list args; va_start(args, format); vswprintf_s(text, format, args); va_end(args);
    AcquireSRWLockExclusive(&state_lock); state_.status = text; ReleaseSRWLockExclusive(&state_lock);
}
bool Readable(const void* pointer, size_t bytes) {
    if (!pointer) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(pointer, &memory, sizeof(memory)) || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const auto at = reinterpret_cast<uintptr_t>(pointer), start = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return at >= start && bytes <= memory.RegionSize && at - start <= memory.RegionSize - bytes;
}
const unsigned char* Item(int32_t index) {
    auto* array = reinterpret_cast<unsigned char*>(ue::Base() + ue::layout::kObjectArray);
    if (index < 0 || !Readable(array, 40) || index >= ue::At<int32_t>(array, 32)) return nullptr;
    auto* chunks = ue::At<unsigned char**>(array, 24);
    const size_t chunk = static_cast<uint32_t>(index) >> 16;
    if (!Readable(chunks, (chunk + 1) * sizeof(void*)) || !chunks[chunk]) return nullptr;
    const unsigned char* item = chunks[chunk] + (static_cast<uint32_t>(index) & 0xffff) * 24ull;
    return Readable(item, 24) ? item : nullptr;
}
bool IdentityExists(const Identity& value) {
    const auto* item = value.object && value.serial > 0 ? Item(value.index) : nullptr;
    if (!item) return false;
    Object* object = nullptr; int32_t serial = 0;
    std::memcpy(&object, item + 8, sizeof(object)); std::memcpy(&serial, item + 16, sizeof(serial));
    return object == value.object && serial == value.serial;
}
bool Same(const Identity& value) {
    return IdentityExists(value) && ue::Alive(value.object) && ue::NameOf(value.object) == value.name;
}
bool GCHelpersMatch() {
    static constexpr unsigned char weak[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x12,0x48,0x8b,0xd9,0x48,0x85,0xd2,0x74};
    static constexpr unsigned char add[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x8b,0x41,0x0c,0x3b,0x05,0x51,0xef,0x83,0x0c,0x7d};
    static constexpr unsigned char release[] = {0x8b,0x41,0x0c,0x3b,0x05,0x17,0x35,0x82,0x0c,0x7d,0x1e,0x8b,0xc8,0x0f,0xb7,0xc0};
    return ue::Base() && !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x166f2a0),weak,sizeof(weak)) &&
        !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x1574d00),add,sizeof(add)) &&
        !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x1590740),release,sizeof(release));
}
Identity Identify(Object* object) {
    if (!ue::Alive(object) || !GCHelpersMatch()) return {};
    struct Weak { int32_t index = 0, serial = 0; } weak;
    reinterpret_cast<void(__fastcall*)(Weak*,Object* const*)>(ue::Base()+0x166f2a0)(&weak,&object);
    Identity result{object,ue::NameOf(object),weak.index,weak.serial};
    return Same(result) ? result : Identity{};
}
bool Keep(Object* object) {
    const Identity id = Identify(object); if (!Same(id)) return false;
    for (const Strong& value : strong_) if (value.identity.object == object && IdentityExists(value.identity))
        return value.state == RefState::Owned;
    if (strong_.size() >= 2048) return false;
    // Reserve the ownership record before engine code. An exception during a
    // native refcount transition is uncertain: never decrement speculatively.
    strong_.push_back({id,RefState::Acquiring});
    reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x1574d00)(object);
    strong_.back().state = RefState::Owned;
    return true;
}
bool ReleaseStrongTail(size_t retain) {
    if (!GCHelpersMatch()) return strong_.empty();
    while (strong_.size()>retain) {
        Strong& value = strong_.back();
        if (value.state != RefState::Owned || !IdentityExists(value.identity)) return false;
        // Release pending-destroy objects too, but only while their exact
        // serial/index still exists. Do not let Alive() hide our own GC hold.
        value.state = RefState::Releasing;
        reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x1590740)(value.identity.object);
        strong_.pop_back();
    }
    return true;
}
bool ReleaseStrong() { return ReleaseStrongTail(0); }
Object* TransientOuter() {
    Object* object = ue::FindObject("/Engine/Transient");
    Object* package = ue::FindObject("/Script/CoreUObject.Package");
    return ue::Alive(object) && package && ue::IsA(object,package) && !ue::OuterOf(object) ? object : nullptr;
}
bool WorldIndependentBinding(Object* binding,Object* binding_class) {
    // Exact native InputActionBinding64 contains NativeClass and Bindings of
    // plain input data/tags plus DebugAbility UClass references. Arbitrary
    // derived/runtime objects could introduce live World references; reject.
    if (!ue::Alive(binding)||ue::ClassOf(binding)!=binding_class||ue::StructSize(binding_class)!=64) return false;
    Object* package=ue::FindObject("/Script/CoreUObject.Package");
    Object* outer=ue::OuterOf(binding);
    return package&&ue::Alive(outer)&&ue::IsA(outer,package)&&!ue::OuterOf(outer);
}
Object* Type(const char* path) { return ue::FindObject(path); }
int32_t Offset(Object* object, const char* name) { return object ? ue::Offset(ue::ClassOf(object), name) : -1; }
Object* Property(Object* object, const char* name) {
    if (!ue::Alive(object)) return nullptr;
    const int32_t offset = Offset(object, name);
    return offset>=0?ue::At<Object*>(object,offset):nullptr;
}
Object* Component(Object* actor, const char* path) {
    Object* cls = Type(path); if (!cls) return nullptr;
    Call call(actor, "GetComponentByClass");
    return call.Set("ComponentClass", cls).Invoke() ? call.Get<Object*>("ReturnValue") : nullptr;
}
Object* Field(Object* cls, const char* name) {
    for (int32_t supers = 0; cls && supers < 64; ++supers, cls = ue::At<Object*>(cls, 0x40)) {
        int32_t count = 0;
        for (Object* field = ue::At<Object*>(cls, 0x50); field && ++count < 512; field = ue::At<Object*>(field, 0x10))
            if (ue::NameIs(ue::At<Name>(field, 0x20), name)) return field;
    }
    return nullptr;
}
uint64_t Kind(Object* field) { return ue::At<uint64_t>(ue::At<Object*>(field, 8), 0x18); }
bool ArrayOk(const Array& value, size_t stride, int32_t limit = 2048) {
    return value.num >= 0 && value.num <= value.max && value.max <= limit && (!value.num || Readable(value.data, static_cast<size_t>(value.num) * stride));
}
// FScriptMap sparse allocation flags; stride/value offset come from its live
// FMapProperty.MapLayout, rather than assuming a packed C++ container.
template<class Visitor> bool VisitMap(void* map, Object* property, Visitor&& visitor) {
    if (!property || Kind(property) != 0x400000000000ull || !Readable(map, 80)) return false;
    const int32_t value_offset = ue::At<int32_t>(property, 0x80), stride = ue::At<int32_t>(property, 0x8c);
    const int32_t count = ue::At<int32_t>(map, 8), capacity = ue::At<int32_t>(map, 12), bits = ue::At<int32_t>(map, 40),
        max_bits = ue::At<int32_t>(map, 44), free = ue::At<int32_t>(map, 52);
    if (stride < 8 || stride > 4096 || value_offset < 0 || value_offset >= stride || count < 0 || count > 2048 ||
        capacity < count || bits != count || max_bits < bits || free < 0 || free > count) return false;
    if (!count) return true;
    auto* data = ue::At<unsigned char*>(map, 0);
    auto* allocation = ue::At<uint32_t*>(map, 32);
    if (!allocation) allocation = static_cast<uint32_t*>(map) + 4;
    if (!Readable(data, static_cast<size_t>(count) * stride) || !Readable(allocation, (static_cast<size_t>(bits) + 31) / 32 * 4)) return false;
    for (int32_t i = 0; i < count; ++i) if (allocation[i / 32] & (1u << (i % 32))) visitor(data + static_cast<size_t>(i) * stride, value_offset);
    return true;
}
bool ContextNow(Context& context) {
    if (!ue::Alive(engine_)) engine_ = ue::FindEngine();
    auto report=[&](const char* reason,bool ready=false) {
        const ULONGLONG now=GetTickCount64();
        const bool changed=!context_reason_||std::strcmp(context_reason_,reason)!=0;
        if (changed||(!ready&&now>=next_context_log_)) {
            context_reason_=reason; next_context_log_=now+10000;
            Logf("AIR_CONTEXT stage=%s ready=%d world=%p controller=%p pawn=%p movement=%p input=%p sinput=%p asc=%p",
                reason,ready?1:0,static_cast<void*>(context.world),static_cast<void*>(context.controller),static_cast<void*>(context.pawn),static_cast<void*>(context.movement),static_cast<void*>(context.input),static_cast<void*>(context.sinput),static_cast<void*>(context.asc));
            if (!ready) Status(L"等待当前主角/组件：%hs",reason);
        }
        return ready;
    };
    if (!ue::Alive(engine_)) return report("engine_missing");
    Object* viewport=Property(engine_,"GameViewport");
    if (!ue::Alive(viewport)) return report("viewport_missing");
    context.world=Property(viewport,"World");
    if (!ue::Alive(context.world)) return report("world_missing");
    Object* instance = Property(context.world, "OwningGameInstance");
    if (!ue::Alive(instance)) return report("game_instance_missing");
    const int32_t players_offset = Offset(instance, "LocalPlayers");
    if (players_offset < 0) return report("local_players_field_missing");
    const Array players = ue::At<Array>(instance, players_offset);
    if (!ArrayOk(players,sizeof(Object*),16)||!players.num) return report("local_players_not_ready");
    context.controller = Property(static_cast<Object**>(players.data)[0], "PlayerController");
    if (!ue::Alive(context.controller)) return report("local_controller_missing");
    context.pawn = Property(context.controller, "Pawn");
    // Actual shipping hero: BP_Renewal_Hun -> BP_RenewalPlayer -> BP_Player_Base
    // -> PBZCharacter_Player -> PBZCharacter. The old ProjectPBZCharacter is an
    // unrelated legacy class and must not gate the current local player's pawn.
    Object* character=Type("/Script/ProjectPBZ.PBZCharacter_Player");
    if (!ue::Alive(context.pawn)) return report("local_pawn_missing");
    if (!character||!ue::IsA(context.pawn,character)) return report("local_pawn_not_pbz_player");
    bool current_world=false; Object* level=Type("/Script/Engine.Level"),*outer=context.pawn;
    for (int32_t depth=0;outer&&ue::Alive(outer)&&depth<32;++depth,outer=ue::OuterOf(outer)) {
        if (outer==context.world||(level&&ue::IsA(outer,level)&&Property(outer,"OwningWorld")==context.world)) { current_world=true; break; }
    }
    if (!current_world) return report("local_pawn_world_mismatch");
    context.movement = Property(context.pawn, "CharacterMovement");
    if (!context.movement) context.movement = Property(context.pawn, "CharMoveComp");
    if (!ue::Alive(context.movement)) return report("movement_missing");
    context.input = Component(context.pawn, "/Script/SGameInputSystem.AbilityInputComponent");
    if (!ue::Alive(context.input)) return report("ability_input_component_missing");
    context.sinput = Property(context.input, "SGameInput"); context.asc = Property(context.input, "AbilitySystem");
    if (!ue::Alive(context.sinput)) return report("sgame_input_missing");
    if (!ue::Alive(context.asc)) return report("ability_system_missing");
    if (Property(context.controller,"Pawn")!=context.pawn) return report("local_pawn_changed_during_lookup");
    return report("ready",true);
}
bool Ground(Name name) { return ue::NameIs(name, "CharacterState.Space.Ground"); }
bool Air(Name name) {
    char text[192]{}; ue::NameText(name, text, sizeof(text));
    return !_stricmp(text, "CharacterState.Space.Air") || !_strnicmp(text, "CharacterState.Space.Air.", 25) ||
        !_stricmp(text, "CharacterState.Space.Falling") || !_strnicmp(text, "CharacterState.Space.Falling.", 29);
}
bool Family(Name name,const char* family) {
    char text[256]{}; ue::NameText(name,text,sizeof(text)); const size_t count=std::strlen(family);
    return !_strnicmp(text,family,count)&&(!text[count]||text[count]=='.');
}
bool AirSkillAction(Object* action) {
    if (!ue::Alive(action)) return false;
    // These are the actual four skill InputActions in InputConfig_Hun. The
    // light/heavy attack actions have authored Ground/Air dispatch of their
    // own: relaxing their Ground branch makes it steal the airborne input.
    const Name name=ue::NameOf(action);
    return ue::NameIs(name,"IA_CombatArt")||ue::NameIs(name,"IA_CombatArt_Hold")||
        ue::NameIs(name,"IA_ShachiAbility")||ue::NameIs(name,"IA_ShachiAbility_Hold");
}
// Movement / defence actions keep the game's own ground and air rules: relaxing
// a stance, dodge or dash family let the grounded version answer in the air and
// the native air dodge stopped working.
bool MovementOrDefence(Name tag) {
    char text[256]{}; ue::NameText(tag, text, sizeof(text));
    for (char* c = text; *c; ++c) *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    for (const char* word : {"stance", "dodge", "dash", "evade", "roll", "step", "parry", "guard", "defen", "jump", "specialmove"})
        if (std::strstr(text, word)) return true;
    return false;
}
// Air dodge: the dash map's AirDash route has no ability anywhere in the game
// (only AIM_Hun_Dash references Ability.Action.Hun.AirDash), so the ordinary
// dodge is let into the air instead: its Ground/Air gates are relaxed in the
// dash map and on the dodge abilities, exactly as for the air skills.
bool DashFamily(Name tag) {
    return Family(tag,"Ability.Action.Hun.Dash")||Family(tag,"Ability.Action.Hun.ContinuousDash")||Family(tag,"Ability.Action.Hun.PerfectDodge")||
        Family(tag,"Ability.Action.Hun.DeflectDash")||Family(tag,"Ability.Action.Hun.SpecialDash");
}
bool DashBinding(Object* binding) {
    char name[192]{}; ue::NameText(ue::NameOf(binding),name,sizeof(name));
    const size_t length=std::strlen(name);
    return length>=5&&!_stricmp(name+length-5,"_Dash");
}
bool AirSkillAbility(Object* ability) {
    const int32_t offset=Offset(ability,"AbilityTags"); if (offset<0) return false;
    const Tags& tags=ue::At<Tags>(ability,offset); if (!ArrayOk(tags.tags,sizeof(Name))) return false;
    for (int32_t i=0;i<tags.tags.num;++i) if (DashFamily(static_cast<Name*>(tags.tags.data)[i])) return true;
    // Match the owned ability through the real currently registered skill
    // triggers, rather than changing every owned GA (including ground attacks).
    for (Name trigger:air_skill_triggers_) {
        char family[256]{}; ue::NameText(trigger,family,sizeof(family));
        // Execution shares the Shachi input but retains its ground-only rules.
        if (Family(trigger,"Ability.Action.Execution")||MovementOrDefence(trigger)) continue;
        bool matched=false;
        for (int32_t i=0;i<tags.tags.num;++i) {
            const Name tag=static_cast<Name*>(tags.tags.data)[i];
            if (MovementOrDefence(tag)) return false;
            if (Family(tag,family)) matched=true;
        }
        if (matched) return true;
    }
    return false;
}
bool ChangeTag(Tags* container, Name tag, bool add) {
    Object* library = ue::Default("/Script/GameplayTags.Default__BlueprintGameplayTagLibrary");
    if (!library || !ArrayOk(container->tags, sizeof(Name)) || !ArrayOk(container->parents, sizeof(Name))) return false;
    Call call(library, add ? "AddGameplayTag" : "RemoveGameplayTag");
    if (!call.Set("TagContainer", *container).Set("Tag", tag).Invoke()) return false;
    *container = call.Get<Tags>("TagContainer");
    return ArrayOk(container->tags, sizeof(Name)) && ArrayOk(container->parents, sizeof(Name));
}
bool HasTag(const Tags* container, Name tag) {
    if (!ArrayOk(container->tags, sizeof(Name))) return false;
    for (int32_t i = 0; i < container->tags.num; ++i) if (static_cast<Name*>(container->tags.data)[i] == tag) return true;
    return false;
}
enum class Gate { None, Required, Blocked };
Gate GateField(Name name) {
    if (ue::NameIs(name, "RequiredTags") || ue::NameIs(name, "ActivationRequiredTags") || ue::NameIs(name, "SourceRequiredTags")) return Gate::Required;
    if (ue::NameIs(name, "IgnoredTags") || ue::NameIs(name, "ActivationBlockedTags") || ue::NameIs(name, "SourceBlockedTags")) return Gate::Blocked;
    return Gate::None;
}
template<class Visitor> void WalkStruct(void* data,Object* cls,Visitor&& visitor,int32_t depth=0,std::vector<Name>* triggers=nullptr) {
    if (!cls || depth > 9) return;
    for (int32_t supers = 0; cls && supers < 64; ++supers, cls = ue::At<Object*>(cls, 0x40)) {
        int32_t count = 0;
        for (Object* f = ue::At<Object*>(cls, 0x50); f && ++count < 512; f = ue::At<Object*>(f, 0x10)) {
            const int32_t offset = ue::At<int32_t>(f, 0x44), size = ue::At<int32_t>(f, 0x34);
            if (offset < 0 || size < 1 || size > 65536) continue;
            auto* slot = static_cast<unsigned char*>(data) + offset;
            const uint64_t kind = Kind(f);
            if (kind == 0x100000ull) {
                Object* sub = ue::At<Object*>(f, 0x70);
                if (sub == Type("/Script/GameplayTags.GameplayTagContainer")) visitor(reinterpret_cast<Tags*>(slot), GateField(ue::At<Name>(f, 0x20)));
                else if (triggers && sub==Type("/Script/GameplayTags.GameplayTag") && ue::NameIs(ue::At<Name>(f,0x20),"TriggerTag")) {
                    const Name tag = *reinterpret_cast<Name*>(slot);
                    if (!tag.None() && std::find(triggers->begin(),triggers->end(),tag)==triggers->end()) triggers->push_back(tag);
                }
                else WalkStruct(slot,sub,visitor,depth+1,triggers);
            } else if (kind == 0x200000ull) {
                Object* inner = ue::At<Object*>(f, 0x78);
                if (Kind(inner) != 0x100000ull) continue;
                const int32_t stride = ue::At<int32_t>(inner, 0x34); const Array array = *reinterpret_cast<Array*>(slot);
                if (stride <= 0 || stride > 65536 || !ArrayOk(array, static_cast<size_t>(stride))) continue;
                for (int32_t i=0;i<array.num;++i) WalkStruct(static_cast<unsigned char*>(array.data)+static_cast<size_t>(i)*stride,ue::At<Object*>(inner,0x70),visitor,depth+1,triggers);
            } else if (kind == 0x400000000000ull) {
                Object* value = ue::At<Object*>(f, 0x78);
                if (Kind(value)==0x100000ull) VisitMap(slot,f,[&](unsigned char* row,int32_t value_offset) { WalkStruct(row+value_offset,ue::At<Object*>(value,0x70),visitor,depth+1,triggers); });
            }
        }
    }
}
uint32_t RelaxBinding(Object* binding, bool apply, bool dash=false) {
    Object* trigger_type=Type("/Script/SGameInputSystem.ActionAbilityTriggerData");
    if (ue::StructSize(trigger_type)!=96||ue::Offset(trigger_type,"RequiredTags")!=16||
        ue::Offset(trigger_type,"IgnoredTags")!=48||ue::Offset(trigger_type,"TriggerTag")!=80) return 0;
    uint32_t changed = 0;
    WalkStruct(binding, ue::ClassOf(binding), [&](Tags* tags, Gate gate) {
        if (gate == Gate::None || !ArrayOk(tags->tags, sizeof(Name))) return;
        const int32_t tag_offset=gate==Gate::Required?16:48;
        const auto* trigger=reinterpret_cast<const unsigned char*>(tags)-tag_offset;
        if (!Readable(trigger,96)) return;
        const Name trigger_tag=*reinterpret_cast<const Name*>(trigger+80);
        // In the dash map only the dodge routes; elsewhere every route but
        // execution and movement / defence actions.
        if (dash?!DashFamily(trigger_tag):(Family(trigger_tag,"Ability.Action.Execution")||MovementOrDefence(trigger_tag))) return;
        std::vector<Name> remove;
        for (int32_t i = 0; i < tags->tags.num; ++i) {
            const Name name = static_cast<Name*>(tags->tags.data)[i];
            if ((gate == Gate::Required && Ground(name)) || (gate == Gate::Blocked && Air(name))) remove.push_back(name);
        }
        for (Name name : remove) if (!apply || (ChangeTag(tags, name, false) && !HasTag(tags, name))) ++changed;
    });
    return changed;
}
uint32_t RelaxSkillBinding(Object* binding,Object* action,bool apply) {
    if (DashBinding(binding)) return RelaxBinding(binding,apply,true);
    return AirSkillAction(action)?RelaxBinding(binding,apply):0;
}
template<class Visitor> bool VisitLists(Object* input, Visitor&& visitor) {
    Object* property = Field(ue::ClassOf(input), "InputBindings");
    Object* list = Type("/Script/SGameInputSystem.SortedInputBindingList");
    if (!property || ue::At<int32_t>(property, 0x44) != 320 || ue::StructSize(list) != 32 || ue::Offset(list, "Bindings") != 16) return false;
    return VisitMap(input + 320, property, [&](unsigned char* row, int32_t value_offset) {
        visitor(row + value_offset, ue::At<Object*>(row, 0));
    });
}
template<class Visitor> bool VisitBindings(Object* input, Visitor&& visitor) {
    return VisitLists(input, [&](void* list, Object* action) {
        const Array bindings = ue::At<Array>(list, 16);
        if (!ArrayOk(bindings, sizeof(Object*), 256)) return;
        for (int32_t i = 0; i < bindings.num; ++i) visitor(static_cast<Object**>(bindings.data)[i], action);
    });
}
bool CacheHelpers() {
    static constexpr unsigned char add[] = {0x48, 0x85, 0xd2, 0x0f, 0x84, 0xbf, 0x03, 0, 0, 0x55, 0x57, 0x41, 0x54, 0x48, 0x8b, 0xec};
    static constexpr unsigned char remove[] = {0xe9, 0x9b, 0xd4, 0x59, 0x12};
    return !std::memcmp(reinterpret_cast<void*>(ue::Base() + 0x5ddbbf0), add, sizeof(add)) &&
        !std::memcmp(reinterpret_cast<void*>(ue::Base() + 0x5ddfbd0), remove, sizeof(remove)) &&
        ue::StructSize(Type("/Script/SGameInputSystem.ActionAbilityBindingItem")) == 152;
}
int32_t References(void* list, Object* asset) {
    const Array refs = ue::At<Array>(list, 16);
    if (!ArrayOk(refs, sizeof(Object*), 256)) return -1;
    int32_t count = 0;
    for (int32_t i = 0; i < refs.num; ++i) if (static_cast<Object**>(refs.data)[i] == asset) ++count;
    return count;
}
int32_t CachedItems(void* list, Object* asset) {
    // Native SortedList_Add constructs priority groups at list+0. Each group
    // is a UE TArray<FActionAbilityBindingItem*>; list+16 keeps UObject refs.
    const Array groups = ue::At<Array>(list, 0), items = ue::At<Array>(asset, 48);
    if (!ArrayOk(groups, 16, 256) || !ArrayOk(items, 152, 256)) return -1;
    const uintptr_t start = reinterpret_cast<uintptr_t>(items.data), end = start + static_cast<size_t>(items.num) * 152;
    int32_t count = 0;
    for (int32_t i = 0; i < groups.num; ++i) {
        const Array pointers = ue::At<Array>(static_cast<unsigned char*>(groups.data) + static_cast<size_t>(i) * 16, 0);
        if (!ArrayOk(pointers, sizeof(void*), 1024)) return -1;
        for (int32_t j = 0; j < pointers.num; ++j) {
            const auto pointer = reinterpret_cast<uintptr_t>(static_cast<void**>(pointers.data)[j]);
            if (pointer >= start && pointer < end && (pointer - start) % 152 == 0) ++count;
        }
    }
    return count;
}
bool ReplaceInList(void* list,Object* original,Object* copy) {
    if (!CacheHelpers()) return false;
        const int32_t references = References(list, original);
        if (references<0) return false;
        if (!references) return true;
        const int32_t cached = CachedItems(list, original), prior_refs = References(list, copy), prior_cached = CachedItems(list, copy);
        if (cached<0||prior_refs<0||prior_cached<0||(prior_refs&&prior_cached!=cached)) return false;
        // These are precisely the helpers used by native Register/Unregister
        // for an existing InputBindings entry. They maintain both priority
        // caches and GC references, using UE allocations. The surrounding
        // InputAction entry and SGameInput delegates stay in place.
        using Native = void(__fastcall*)(void*, Object*);
        const auto remove = reinterpret_cast<Native>(ue::Base() + 0x5ddfbd0), add = reinterpret_cast<Native>(ue::Base() + 0x5ddbbf0);
        for (int32_t i = 0; i < references; ++i) remove(list, original);
        if (References(list,original)!=0||CachedItems(list,original)!=0) return false;
        for (int32_t i = 0; i < references; ++i) add(list, copy);
        if (References(list,copy)!=prior_refs+references||CachedItems(list,copy)!=(prior_refs?prior_cached:cached)) return false;
        Logf("AIR_SKILLS native priority cache swapped refs=%d items=%d original=%p copy=%p", references, cached, static_cast<void*>(original), static_cast<void*>(copy));
        return true;
}
bool ReplaceBinding(Object* input,Object* original,Object* copy) {
    bool ok=true;
    const bool visited=VisitLists(input,[&](void* list,Object*) {
        if (!ReplaceInList(list,original,copy)) ok=false;
    });
    return ok && visited;
}
bool LifecycleBefore(Object* input,const void* map,bool* affected) {
    Object* function=ue::FindFunction(ue::ClassOf(input),"UnregisterActionInputBindings");
    Object* property=Field(function,"Bindings");
    if (!property||Kind(property)!=0x400000000000ull||ue::At<int32_t>(property,0x80)!=8||ue::At<int32_t>(property,0x8c)!=24) return false;
    bool ok=true;
    const bool visited=VisitMap(const_cast<void*>(map),property,[&](unsigned char* row,int32_t value_offset) {
        Object* action=ue::At<Object*>(row,0),*asset=ue::At<Object*>(row,value_offset);
        for (size_t i=0;i<clones_.size()&&i<128;++i) {
            const Clone& clone=clones_[i]; if (!Same(clone.original)||!Same(clone.copy)) continue;
            if (asset!=clone.original.object&&asset!=clone.copy.object) continue;
            affected[i]=true;
            if (asset==clone.copy.object) continue; // The caller already has the right runtime identity.
            if (!VisitLists(input,[&](void* list,Object* key) {
                if (key==action&&!ReplaceInList(list,clone.copy.object,clone.original.object)) ok=false;
            })) ok=false;
        }
    });
    return visited&&ok;
}
bool LifecycleAfter(Object* input,const bool* affected) {
    bool ok=true;
    for (size_t i=0;i<clones_.size()&&i<128;++i) if (affected[i]) {
        Clone& clone=clones_[i];
        if (!Same(clone.original)||!Same(clone.copy)) { ok=false; continue; }
        for (auto at=clone.mappings.begin();at!=clone.mappings.end();) {
            bool found=false; int32_t refs=0,cached=0;
            if (!VisitLists(input,[&](void* list,Object* action) {
                if (!Same(at->action)||action!=at->action.object) return;
                found=true; const int32_t original=References(list,clone.original.object),copy=References(list,clone.copy.object);
                const int32_t before=CachedItems(list,clone.original.object),after=CachedItems(list,clone.copy.object);
                if (original<0||copy<0||before<0||after<0||(!original&&before)||(!copy&&after)) { ok=false; refs=-1; return; }
                refs=original+copy; cached=original?before:after;
            })) ok=false;
            if (refs<0) { ++at; continue; }
            // The native operation determines which registrations survived.
            // Never re-add an original merely because it existed in our ledger.
            if (!found||!refs) at=clone.mappings.erase(at);
            else { at->references=refs; at->cached_items=cached; ++at; }
        }
    }
    binding_signature_=0; next_refresh_=0;
    return ok;
}
bool LifecycleBeforeGuarded(Object* input,const void* map,bool* affected) {
    __try { return LifecycleBefore(input,map,affected); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool LifecycleAfterGuarded(Object* input,const bool* affected) {
    __try { return LifecycleAfter(input,affected); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void __fastcall UnregisterHook(Object* input,const void* map) {
    bool affected[128]{};
    const bool ours=GetCurrentThreadId()==game_thread_&&Same(input_)&&input_.object==input&&!clones_.empty();
    const bool before=!ours||LifecycleBeforeGuarded(input,map,affected);
    // Always preserve the game's unregister, even if our bookkeeping failed.
    __try { unregister_original_(input,map); }
    __finally {
        const bool after=!ours||LifecycleAfterGuarded(input,affected);
        if (!before||!after||AbnormalTermination()) {
            restore_pending_=true; InterlockedExchange(&disabled_,1);
            Status(L"输入卸载调解未完整读回；已停止空中功能并保留恢复账本");
            Logf("AIR_INPUT_LIFECYCLE refused continuation before=%d after=%d; native unregister preserved",before?1:0,after?1:0);
        }
    }
}
constexpr unsigned char kUnregisterPrologue[]={0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x48,0x89,0x7c,0x24,0x18};
void JumpPatch(unsigned char* output,const void* target) {
    output[0]=0xff; output[1]=0x25; std::memset(output+2,0,4); std::memcpy(output+6,&target,sizeof(target)); output[14]=0x90;
}
bool EnsureUnregisterHook() {
    if (!game_thread_||GetCurrentThreadId()!=game_thread_||!CacheHelpers()) return false;
    if (!Same(input_)) return false;
    Object* function=ue::FindFunction(ue::ClassOf(input_.object),"UnregisterActionInputBindings"),*property=Field(function,"Bindings");
    if (!function||ue::At<uint16_t>(function,ue::layout::kFuncParmsSize)!=80||
        ue::At<uintptr_t>(function,0x128)!=ue::Base()+0x5de6870||!property||Kind(property)!=0x400000000000ull||
        ue::At<int32_t>(property,0x44)!=0||ue::At<int32_t>(property,0x34)!=80||
        ue::At<int32_t>(property,0x80)!=8||ue::At<int32_t>(property,0x8c)!=24) return false;
    auto* entry=reinterpret_cast<unsigned char*>(ue::Base()+0x5de2840);
    unsigned char patch[15]{}; JumpPatch(patch,reinterpret_cast<const void*>(&UnregisterHook));
    if (unregister_installed_) return unregister_verified_&&!std::memcmp(entry,patch,sizeof(patch));
    if (std::memcmp(entry,kUnregisterPrologue,sizeof(kUnregisterPrologue))) return false;
    if (!unregister_trampoline_) {
        auto* memory=static_cast<unsigned char*>(VirtualAlloc(nullptr,64,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));
        if (!memory) return false;
        std::memcpy(memory,kUnregisterPrologue,sizeof(kUnregisterPrologue)); JumpPatch(memory+15,entry+15);
        DWORD old=0;
        if (!VirtualProtect(memory,64,PAGE_EXECUTE_READ,&old)||!FlushInstructionCache(GetCurrentProcess(),memory,64)) { VirtualFree(memory,0,MEM_RELEASE); return false; }
        // Never free a published trampoline; an old in-flight caller may still
        // use it even after our exact entry patch has been removed.
        unregister_trampoline_=memory; unregister_original_=reinterpret_cast<UnregisterNative>(memory);
    }
    DWORD old=0; if (!VirtualProtect(entry,15,PAGE_EXECUTE_READWRITE,&old)) return false;
    unregister_entry_protection_=old;
    std::memcpy(entry,patch,sizeof(patch));
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),entry,15)!=FALSE;
    DWORD ignored=0; const bool protected_again=VirtualProtect(entry,15,old,&ignored)!=FALSE;
    unregister_installed_=true;
    unregister_verified_=flushed&&protected_again;
    Logf("AIR_INPUT_LIFECYCLE installed exact GT native entry; prologue=15 relocations=0 flush=%d protection=%d",flushed?1:0,protected_again?1:0);
    return flushed&&protected_again;
}
bool RemoveUnregisterHook() {
    if (!unregister_installed_) return true;
    if (GetCurrentThreadId()!=game_thread_) return false;
    auto* entry=reinterpret_cast<unsigned char*>(ue::Base()+0x5de2840);
    unsigned char patch[15]{}; JumpPatch(patch,reinterpret_cast<const void*>(&UnregisterHook));
    if (std::memcmp(entry,patch,sizeof(patch))) return false;
    DWORD old=0; if (!VirtualProtect(entry,15,PAGE_EXECUTE_READWRITE,&old)) return false;
    std::memcpy(entry,kUnregisterPrologue,sizeof(kUnregisterPrologue));
    const bool flushed=FlushInstructionCache(GetCurrentProcess(),entry,15)!=FALSE;
    DWORD ignored=0; const bool protected_again=VirtualProtect(entry,15,unregister_entry_protection_,&ignored)!=FALSE;
    unregister_installed_=false;
    unregister_verified_=false;
    return flushed&&protected_again;
}
bool BindingSignature(Object* input,uint64_t& hash) {
    hash=1469598103934665603ull;
    auto mix=[&](uintptr_t value) { hash^=value; hash*=1099511628211ull; };
    bool ok=true;
    const bool visited=VisitLists(input,[&](void* list,Object* action) {
        mix(reinterpret_cast<uintptr_t>(action));
        const Array refs=ue::At<Array>(list,16); if (!ArrayOk(refs,sizeof(Object*),256)) { ok=false; return; }
        mix(static_cast<uintptr_t>(refs.num));
        for (int32_t i=0;i<refs.num;++i) mix(reinterpret_cast<uintptr_t>(static_cast<Object**>(refs.data)[i]));
    });
    return visited&&ok;
}
bool RecordMappings(Object* input, Clone& clone) {
    bool ok = true;
    const bool visited = VisitLists(input,[&](void* list,Object* action) {
        const int32_t refs = References(list,clone.original.object);
        if (refs < 0) { ok = false; return; }
        if (!refs) return;
        const int32_t cached = CachedItems(list,clone.original.object), copies = References(list,clone.copy.object);
        if (cached<0||copies<0||!Same(Identify(action))) { ok=false; return; }
        auto prior = std::find_if(clone.mappings.begin(),clone.mappings.end(),[&](const Mapping& value) { return Same(value.action) && value.action.object == action; });
        if (prior == clone.mappings.end()) clone.mappings.push_back({Identify(action),refs+copies,cached});
        else { prior->references = (std::max)(prior->references,refs+copies); prior->cached_items = cached; }
    });
    return visited && ok;
}
bool RestoredMappings(Object* input, const Clone& clone) {
    bool ok = true;
    const bool visited = VisitLists(input,[&](void* list,Object* action) {
        for (const Mapping& mapping : clone.mappings) if (Same(mapping.action) && mapping.action.object == action) {
            // A removed map key is owned by the game and is not re-created.
            // For an existing key, do not silently pass a partial swap where
            // both the original reference and its copy have disappeared.
            if (References(list,clone.original.object) < mapping.references || References(list,clone.copy.object) != 0 ||
                CachedItems(list,clone.original.object) != mapping.cached_items || CachedItems(list,clone.copy.object) != 0) ok = false;
        }
    });
    return visited && ok;
}
bool JumpBinding(Object* binding) {
    char name[192]{}; ue::NameText(ue::NameOf(binding), name, sizeof(name));
    // This exact data asset is the actual user's Jump mapping; key/gamepad
    // bindings remain in the game's SGameInput component and are not assumed.
    return !_stricmp(name, "AIM_Hun_Jump");
}
template<class Visitor> bool BindingPriorities(Object* binding,Visitor&& visitor) {
    Object* item=Type("/Script/SGameInputSystem.ActionAbilityBindingItem");
    Object* priority=Field(item,"ActionPriority");
    if (!item||ue::StructSize(item)!=152||!priority||Kind(priority)!=0x80ull||
        ue::At<int32_t>(priority,0x44)!=0||ue::At<int32_t>(priority,0x34)!=4||Offset(binding,"Bindings")!=48) return false;
    const Array array=ue::At<Array>(binding,48); if (!ArrayOk(array,152,256)) return false;
    for (int32_t i=0;i<array.num;++i) visitor(ue::At<int32_t>(static_cast<unsigned char*>(array.data)+static_cast<size_t>(i)*152,0));
    return true;
}
void ObserveJump(Object* binding,Object* action) {
    if (!JumpBinding(binding)||!Same(Identify(action))) return;
    std::vector<Name> triggers;
    WalkStruct(binding,ue::ClassOf(binding),[](Tags*,Gate) {},0,&triggers);
    for (Name tag:triggers) if (ue::NameIs(tag,"Ability.Action.Hun.Jump")) {
        jump_tag_=tag; jump_action_=action; jump_name_=ue::NameOf(action); jump_identity_=Identify(action); return;
    }
}
bool SetJumpPriority(Object* copy,int32_t priority) {
    bool any=false;
    const bool ok=BindingPriorities(copy,[&](int32_t& value) { if (value) { value=priority; any=true; } });
    return ok&&any;
}
void ObserveSkillRoute(Object* binding,Object* action) {
    std::vector<Name>* triggers = nullptr;
    if (ue::NameIs(ue::NameOf(binding),"AIM_Hun_CombatArt")) { combatart_action_=Identify(action); triggers=&combatart_triggers_; }
    if (ue::NameIs(ue::NameOf(binding),"AIM_Hun_ShachiAbility")) { shachi_action_=Identify(action); triggers=&shachi_triggers_; }
    if (triggers) WalkStruct(binding,ue::ClassOf(binding),[](Tags*,Gate) {},0,triggers);
}
void ObserveAttackAction(Object* binding,Object* action) {
    if (ue::NameIs(ue::NameOf(binding),"AIM_Hun_LightAttack")) light_action_=Identify(action);
    if (ue::NameIs(ue::NameOf(binding),"AIM_Hun_HeavyAttack")) heavy_action_=Identify(action);
}
bool CloneBindings(const Context& context) {
    Object* library = ue::Default("/Script/SGameAI.Default__SGameAIFunctionLibrary");
    Object* binding_class = Type("/Script/SGameInputSystem.InputActionBinding");
    if (!library || !binding_class || !GCHelpersMatch() || ue::Offset(binding_class, "Bindings") != 48 || ue::StructSize(binding_class) != 64 || (options_.air_skills && !CacheHelpers())) return false;
    const Identity prior_jump=jump_identity_;
    jump_action_ = nullptr; jump_tag_={}; jump_priority_ready_=false;
    jump_identity_={}; native_jump_priority_=0;
    combatart_action_ = {}; shachi_action_ = {}; combatart_triggers_.clear(); shachi_triggers_.clear();
    air_skill_triggers_.clear();
    light_action_={}; heavy_action_={}; attack_triggers_.clear();
    std::vector<Object*> originals;
    std::vector<Object*> seen_assets,seen_actions;
    const bool visited = VisitBindings(context.input, [&](Object*& binding, Object* action) {
        if (!ue::Alive(binding) || !ue::IsA(binding, binding_class)) return;
        ObserveAttackAction(binding,action);
        if (std::find(seen_actions.begin(),seen_actions.end(),action)==seen_actions.end()) seen_actions.push_back(action);
        for (const Clone& clone : clones_) if (Same(clone.copy) && binding == clone.copy.object) {
            if (Same(clone.original)) ObserveAttackAction(clone.original.object,action);
            if (Same(clone.original) && std::find(seen_assets.begin(),seen_assets.end(),clone.original.object)==seen_assets.end()) seen_assets.push_back(clone.original.object);
            if (Same(clone.original)) ObserveSkillRoute(clone.original.object,action);
            if (Same(clone.original)) ObserveJump(clone.original.object,action);
            return;
        }
        if (std::find(seen_assets.begin(),seen_assets.end(),binding)==seen_assets.end()) seen_assets.push_back(binding);
        ObserveSkillRoute(binding,action);
        // The native Jump binding keeps its own priority: a copy raised above
        // every action made an airborne jump unbreakable, so the air dodge
        // could not interrupt it. The raised priority is used only by this
        // module's own jump-cancel activation (TryJump).
        if (JumpBinding(binding)) {
            ObserveJump(binding,action);
            BindingPriorities(binding,[&](int32_t& value) { if (value&&!native_jump_priority_) native_jump_priority_=value; });
            return;
        }
        if (!options_.air_skills || !RelaxSkillBinding(binding,action,false)) return;
        if (std::find(originals.begin(), originals.end(), binding) == originals.end()) originals.push_back(binding);
    });
    // The same actual input action can have per-weapon bindings in addition to
    // its default AIM. Include all of those tables, then restrict cancellation
    // to proven attack tag families below (the AIM also contains execution).
    VisitBindings(context.input,[&](Object*& binding,Object* action) {
        if (!ue::Alive(binding)) return;
        if (AirSkillAction(action)) WalkStruct(binding,ue::ClassOf(binding),[](Tags*,Gate) {},0,&air_skill_triggers_);
        if ((Same(light_action_)&&action==light_action_.object)||(Same(heavy_action_)&&action==heavy_action_.object))
            WalkStruct(binding,ue::ClassOf(binding),[](Tags*,Gate) {},0,&attack_triggers_);
    });
    std::vector<std::wstring> seen_names;
    for (Object* binding:seen_assets) {
        char text[256]{}; ue::NameText(ue::NameOf(binding),text,sizeof(text));
        wchar_t wide[256]{}; MultiByteToWideChar(CP_UTF8,0,text,-1,wide,static_cast<int>(sizeof(wide)/sizeof(wide[0]))); seen_names.emplace_back(wide);
    }
    AcquireSRWLockExclusive(&state_lock); state_.scanned_input_assets=static_cast<uint32_t>(seen_assets.size());
    state_.scanned_input_actions=static_cast<uint32_t>(seen_actions.size()); state_.input_assets=std::move(seen_names); ReleaseSRWLockExclusive(&state_lock);
    if (jump_action_!=prior_jump.object||jump_identity_.serial!=prior_jump.serial) jump_baselined_=false;
    if (!visited) return false;
    // Source priorities, never our already raised Jump copy, determine the
    // negative priority. Zero is skipped by native Add; INT_MIN cannot be used
    // because the admission path negates the requested priority.
    int32_t magnitude=1; bool priorities_ok=true;
    for (Object* binding:seen_assets) if (!BindingPriorities(binding,[&](int32_t& value) {
        if (value<=-1000000||value>=1000000) priorities_ok=false;
        else magnitude=(std::max)(magnitude,value<0?-value:value);
    })) priorities_ok=false;
    // CurrentPriority at +0x1b0 is also consumed by the exact pinned native
    // helper. Only account for it, never overwrite that unrelated action.
    if (options_.air_skills&&Readable(context.input+432,4)) {
        const int32_t current=ue::At<int32_t>(context.input,432);
        // Do not feed our own active Jump's priority back into this computation.
        if (current!=jump_priority_) {
            if (current<=-1000000||current>=1000000) priorities_ok=false;
            else magnitude=(std::max)(magnitude,current<0?-current:current);
        }
    }
    if (options_.air_skills&&!priorities_ok) return false;
    const int32_t desired_priority=-(magnitude+1);
    bool ok = true;
    Object* outer = originals.empty() ? nullptr : TransientOuter();
    if (!originals.empty() && !outer) return false;
    for (Object* binding : originals) {
        Clone* existing = nullptr;
        for (Clone& clone : clones_) if (Same(clone.original) && clone.original.object == binding && Same(clone.copy)) { existing = &clone; break; }
        if (existing) { if (!RecordMappings(context.input,*existing) || !ReplaceBinding(context.input,binding,existing->copy.object)) ok = false; continue; }
        if (clones_.size() >= 128) { ok = false; continue; }
        if (!WorldIndependentBinding(binding,binding_class)||!Keep(binding)) { ok=false; continue; }
        Call duplicate(library, "DuplicateObject");
        if (!duplicate.Set("SourceObject", binding).Set("Outer", outer).Invoke()) { ok = false; continue; }
        Object* copy = duplicate.Get<Object*>("ReturnValue");
        if (!ue::Alive(copy)||copy==binding||ue::OuterOf(copy)!=outer||!WorldIndependentBinding(copy,binding_class)||!Keep(copy)) { ok=false; continue; }
        // Duplicated reflected arrays must not share backing memory with the asset.
        const Array original_array = ue::At<Array>(binding, 48), copy_array = ue::At<Array>(copy, 48);
        if (original_array.num && original_array.data == copy_array.data) { ok = false; continue; }
        const bool jump=JumpBinding(binding);
        const uint32_t changes=jump?0:RelaxBinding(copy,true,DashBinding(binding));
        if ((jump&&!SetJumpPriority(copy,desired_priority))||(!jump&&!changes)) { ok=false; continue; }
        clones_.push_back({Identify(binding),Identify(copy),changes,{}});
        char binding_name[160]{}; ue::NameText(ue::NameOf(binding),binding_name,sizeof(binding_name));
        Logf("AIR_SKILLS cloned player input binding %s original=%p copy=%p removed_space_tags=%u", binding_name, static_cast<void*>(binding), static_cast<void*>(copy), changes);
        if (!RecordMappings(context.input,clones_.back()) || !ReplaceBinding(context.input,binding,copy)) ok = false;
    }
    if (options_.air_skills) for (Clone& clone:clones_) if (Same(clone.original)&&Same(clone.copy)&&JumpBinding(clone.original.object)) {
        bool matches=true,any=false;
        if (!BindingPriorities(clone.copy.object,[&](int32_t& value) { if (value) { any=true; if (value!=desired_priority) matches=false; } })) ok=false;
        if (!matches) {
            // Native groups cache pointers sorted by these integers. Detach the
            // copy before editing it, then rebuild through the actual helpers.
            if (!ReplaceBinding(context.input,clone.copy.object,clone.original.object)||
                !SetJumpPriority(clone.copy.object,desired_priority)||!RecordMappings(context.input,clone)||
                !ReplaceBinding(context.input,clone.original.object,clone.copy.object)) ok=false;
        }
        if (any&&ok&&Same(jump_identity_)) { jump_priority_=desired_priority; jump_priority_ready_=true; }
    }
    // The module's own jump-cancel activation uses the Jump binding's native
    // priority: the raised one (above every action) made that jump unbreakable,
    // so the air dodge could not interrupt it. The skills it cancels are ended
    // first, so the native priority is enough to start the jump.
    if (options_.air_skills&&ok&&Same(jump_identity_)&&!jump_priority_ready_) {
        const int32_t chosen=native_jump_priority_?native_jump_priority_:desired_priority;
        if (chosen!=jump_priority_) Logf("AIR_JUMP activation priority=%d (native=%d raised=%d)",chosen,native_jump_priority_,desired_priority);
        jump_priority_=chosen; jump_priority_ready_=true;
    }
    return ok && (!options_.stomp_jump || ue::Alive(jump_action_));
}
std::vector<Object*> Abilities(const Context& context,bool coverage=false) {
    std::vector<Object*> result;
    Object* container = Type("/Script/GameplayAbilities.GameplayAbilitySpecContainer"), *spec = Type("/Script/GameplayAbilities.GameplayAbilitySpec");
    const int32_t abilities = Offset(context.asc, "ActivatableAbilities"), items = ue::Offset(container, "Items"), stride = ue::StructSize(spec);
    if (abilities != 1048 || items != 264 || stride != 240) return result;
    const Array array = ue::At<Array>(context.asc, abilities + items);
    if (!ArrayOk(array, static_cast<size_t>(stride), 1024)) return result;
    std::vector<std::wstring> unadapted;
    for (int32_t i=0;i<array.num;++i) {
        const size_t prior=result.size();
        auto* row=static_cast<unsigned char*>(array.data)+static_cast<size_t>(i)*stride;
        for (const char* name : {"NonReplicatedInstances", "ReplicatedInstances"}) {
        const int32_t offset = ue::Offset(spec, name); if (offset < 0) continue;
        const Array instances=ue::At<Array>(row,offset);
        if (!ArrayOk(instances, sizeof(Object*), 64)) continue;
        for (int32_t j = 0; j < instances.num; ++j) {
            Object* ability = static_cast<Object**>(instances.data)[j];
            if (!ue::Alive(ability) || ue::IsTemplate(ability) || std::find(result.begin(), result.end(), ability) != result.end()) continue;
            Call avatar(ability, "GetAvatarActorFromActorInfo"), asc(ability, "GetAbilitySystemComponentFromActorInfo");
            if (avatar.Invoke() && asc.Invoke() && avatar.Get<Object*>("ReturnValue") == context.pawn && asc.Get<Object*>("ReturnValue") == context.asc) result.push_back(ability);
        }
        }
        if (coverage) {
            const int32_t offset=ue::Offset(spec,"Ability"); Object* cdo=offset>=0?ue::At<Object*>(row,offset):nullptr;
            const int32_t policy_offset=Offset(cdo,"InstancingPolicy");
            const uint8_t policy=policy_offset>=0?ue::At<uint8_t>(cdo,policy_offset):uint8_t{255};
            // InstancedPerExecution activation still tests its class default
            // before a new instance exists. Do not report an active old copy as
            // proof that the next execution's CDO gate has been relaxed.
            if (policy==1 && result.size()>prior) continue;
            if (!ue::Alive(cdo)||!AirSkillAbility(cdo)) continue;
            bool gated=false;
            if (ue::Alive(cdo)) for (const char* property:{"ActivationRequiredTags","ActivationBlockedTags","SourceRequiredTags","SourceBlockedTags"}) {
                const int32_t at=Offset(cdo,property); if (at<0) continue;
                const Tags& tags=ue::At<Tags>(cdo,at); if (!ArrayOk(tags.tags,sizeof(Name))) continue;
                const bool required=!std::strcmp(property,"ActivationRequiredTags")||!std::strcmp(property,"SourceRequiredTags");
                for (int32_t n=0;n<tags.tags.num;++n) if (required?Ground(static_cast<Name*>(tags.tags.data)[n]):Air(static_cast<Name*>(tags.tags.data)[n])) gated=true;
            }
            if (gated) {
                char text[256]{}; ue::NameText(ue::NameOf(ue::ClassOf(cdo)),text,sizeof(text));
                wchar_t wide[256]{}; MultiByteToWideChar(CP_UTF8,0,text,-1,wide,static_cast<int>(sizeof(wide)/sizeof(wide[0])));
                unadapted.emplace_back(std::wstring(wide)+L"：激活前仍使用CDO空间限制；未修改共享默认对象");
            }
        }
    }
    if (coverage) { AcquireSRWLockExclusive(&state_lock); state_.scanned_ability_specs=static_cast<uint32_t>(array.num); state_.unadapted_skills=std::move(unadapted); ReleaseSRWLockExclusive(&state_lock); }
    return result;
}
void RelaxAbilities(const std::vector<Object*>& abilities) {
    if (!options_.air_skills) return;
    for (Object* ability : abilities) for (const char* property : {"ActivationRequiredTags", "ActivationBlockedTags", "SourceRequiredTags", "SourceBlockedTags"}) {
        if (!AirSkillAbility(ability)) continue;
        const int32_t offset = Offset(ability, property); if (offset < 0) continue;
        auto* tags = reinterpret_cast<Tags*>(ability + offset);
        if (!ArrayOk(tags->tags, sizeof(Name))) continue;
        std::vector<Name> remove;
        const bool required = !std::strcmp(property, "ActivationRequiredTags") || !std::strcmp(property, "SourceRequiredTags");
        for (int32_t i = 0; i < tags->tags.num; ++i) {
            const Name name = static_cast<Name*>(tags->tags.data)[i];
            if ((required && Ground(name)) || (!required && Air(name))) remove.push_back(name);
        }
        if (remove.empty()) continue;
        for (Name name : remove) {
            // Record the original presence before entering engine code; a fault
            // after RemoveGameplayTag must still leave a restoration record.
            removed_.push_back({Identify(ability),offset,name});
            ChangeTag(tags,name,false);
        }
        static std::vector<Name> logged; // once per ability class
        const Name class_name=ue::NameOf(ue::ClassOf(ability));
        if (std::find(logged.begin(),logged.end(),class_name)==logged.end()&&logged.size()<256) {
            logged.push_back(class_name);
            char ability_name[160]{},tag_name[96]{}; ue::NameText(class_name,ability_name,sizeof(ability_name)); ue::NameText(remove.front(),tag_name,sizeof(tag_name));
            Logf("AIR_SKILLS relaxed ability %s %s: removed %zu (%s)",ability_name,property,remove.size(),tag_name);
        }
    }
}
bool Restore() {
    bool ok=RestoreSlowFall();
    if (!RestoreCancelFlags()) ok=false;
    std::vector<Clone> left_clones;
    std::vector<Removed> left_removed;
    if (Same(input_)) {
        for (const Clone& clone : clones_) if (Same(clone.copy)) {
            if (!Same(clone.original) || !ReplaceBinding(input_.object,clone.copy.object,clone.original.object) || !RestoredMappings(input_.object,clone)) { ok = false; left_clones.push_back(clone); }
        }
    } else if (input_.object && IdentityExists(input_) && ue::Alive(input_.object)) {
        // A live identity mismatch must not silently discard overwritten input.
        ok = false; left_clones = clones_;
    }
    for (const Removed& value : removed_) if (Same(value.ability)) {
        auto* tags = reinterpret_cast<Tags*>(value.ability.object + value.offset);
        // Weak owner + original/last per-tag value ledger. The ASC spec owns
        // live instances. Once it removes/collects one, no destroyed object is
        // restored or retained. An already re-added tag is left untouched.
        const bool present=HasTag(tags,value.tag);
        if (present==value.last_present&&value.original_present&&(!ChangeTag(tags,value.tag,true)||!HasTag(tags,value.tag))) { ok=false; left_removed.push_back(value); }
    }
    clones_=std::move(left_clones); removed_=std::move(left_removed); enemies_.clear(); enemy_scan_={}; near_scan_={};
    if (clones_.empty()&&removed_.empty()&&cancel_flags_.empty()) {
        if (!ReleaseStrong()) ok = false;
        if (strong_.empty()) {
            if (!RemoveUnregisterHook()) ok=false;
            else { owner_={}; input_={}; }
        }
    }
    jump_action_=nullptr; jump_identity_={}; last_jump_=false; jump_baselined_=false; next_refresh_=next_enemies_=0;
    combatart_action_ = {}; shachi_action_ = {};
    combatart_triggers_.clear(); shachi_triggers_.clear();
    air_skill_triggers_.clear();
    light_action_={}; heavy_action_={}; attack_triggers_.clear();
    step_ = {};
    jump_request_={}; jump_priority_ready_=false;
    binding_signature_=0; binding_ready_=false; owned_count_=0;
    AcquireSRWLockExclusive(&state_lock); state_.skills.clear(); ReleaseSRWLockExclusive(&state_lock);
    restore_pending_=!ok||!clones_.empty()||!removed_.empty()||!cancel_flags_.empty()||!strong_.empty();
    return !restore_pending_;
}
bool Position(Object* actor, Vector& value) {
    Call transform(actor, "GetTransform");
    if (!transform.Invoke()) return false;
    value = transform.Get<Transform>("ReturnValue").translation;
    return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
}
bool Capsule(Object* actor, float& radius, float& height) {
    Object* capsule = Property(actor, "CapsuleComponent");
    Call r(capsule, "GetScaledCapsuleRadius"), h(capsule, "GetScaledCapsuleHalfHeight");
    if (!r.Invoke() || !h.Invoke()) return false;
    radius = r.Get<float>("ReturnValue"); height = h.Get<float>("ReturnValue");
    return std::isfinite(radius) && std::isfinite(height) && radius > 1 && radius < 2000 && height >= radius && height < 5000;
}
bool Enemy(Object* pawn, const Context& context,EnemyReason* why=nullptr) {
    auto reject=[&](EnemyReason reason) { if (why) *why=reason; return false; };
    if (why) *why=EnemyReason::Other;
    if (!ue::Alive(pawn) || pawn == context.pawn || ue::IsTemplate(pawn)) return false;
    bool same_world = false;
    Object* outer=ue::OuterOf(pawn),*level=Type("/Script/Engine.Level");
    for (int32_t depth=0;outer&&depth<32;++depth,outer=ue::OuterOf(outer)) {
        if (!ue::Alive(outer)) break;
        if (outer==context.world || (level&&ue::IsA(outer,level)&&Property(outer,"OwningWorld")==context.world)) { same_world=true; break; }
    }
    if (!same_world) return false;
    Object* controller = Property(pawn, "Controller");
    Object* ai = Type("/Script/SGameAI.SGameAIController");
    if (!ue::Alive(controller) || !ai || !ue::IsA(controller, ai)) return reject(EnemyReason::NoAI);
    // BlueprintGetTeamAttitudeTowards is an optional Blueprint event: calling
    // it without an override returns a default enum, rather than the native
    // attitude. Use the game's actual actor-team resolver/default-team matrix.
    Object* team_api = ue::Default("/Script/SGameAI.Default__SGameAIController");
    Call own_team(team_api, "GetTeam"), other_team(team_api, "GetTeam");
    if (!own_team.Set("Actor", context.pawn).Invoke() || !other_team.Set("Actor", pawn).Invoke()) return reject(EnemyReason::TeamQuery);
    const uint8_t team1 = other_team.Get<uint8_t>("ReturnValue"), team2 = own_team.Get<uint8_t>("ReturnValue");
    Call attitude(team_api, "GetDefaultTeamAttitude");
    if (!attitude.Set("Team1", team1).Set("Team2", team2).Invoke()) return reject(EnemyReason::TeamQuery);
    if (attitude.Get<uint8_t>("ReturnValue")!=2) return reject(EnemyReason::NotHostile); // ETeamAttitude::Hostile.
    Object* states = ue::Default("/Script/SGameAI.Default__SGameAIStateUtils");
    Call dead(states, "IsDead");
    if (!dead.Set("Pawn",pawn).Invoke()) return reject(EnemyReason::DeadQuery);
    if (dead.Get<bool>("ReturnValue")) return reject(EnemyReason::Dead);
    // GetHealth is player-specific in this Shipping build. Actual NPCs share
    // PBZCharacter.AttributeSetBase -> BasicAttributeSet.Health.CurrentValue.
    Object* set = Property(pawn,"AttributeSetBase"), *basic = Type("/Script/CharacterBasics.BasicAttributeSet"),
        *data = Type("/Script/GameplayAbilities.GameplayAttributeData");
    const int32_t health = Offset(set,"Health"), current = data ? ue::Offset(data,"CurrentValue") : -1;
    if (!ue::Alive(set) || !basic || !ue::IsA(set,basic) || health < 0 || !data ||
        ue::StructSize(data) != 16 || current != 12 || !Readable(set+health,16)) return reject(EnemyReason::HealthLayout);
    const float value = ue::At<float>(set,health+current);
    if (!std::isfinite(value)||value<=0) return reject(EnemyReason::HealthZero);
    if (why) *why=EnemyReason::Accepted;
    return true;
}
void RefreshEnemies(const Context& context) {
    enemies_.clear(); enemy_scan_={};
    Object* library = ue::Default("/Script/Engine.Default__GameplayStatics");
    Object* character = Type("/Script/Engine.Pawn");
    Call actors(library, "GetAllActorsOfClass");
    if (!actors.Set("WorldContextObject", context.pawn).Set("ActorClass", character).Invoke()) return;
    const Array array = actors.Get<Array>("OutActors");
    if (ArrayOk(array, sizeof(Object*), 2048)) for (int32_t i = 0; i < array.num; ++i) {
        Object* pawn=static_cast<Object**>(array.data)[i]; if (pawn==context.pawn) continue;
        ++enemy_scan_.pawns; EnemyReason reason=EnemyReason::Other;
        if (Enemy(pawn,context,&reason)) { enemies_.push_back(Identify(pawn)); ++enemy_scan_.accepted; }
        else if (reason==EnemyReason::NoAI) ++enemy_scan_.no_ai;
        else if (reason==EnemyReason::NotHostile||reason==EnemyReason::TeamQuery) ++enemy_scan_.team;
        else if (reason==EnemyReason::DeadQuery||reason==EnemyReason::Dead||reason==EnemyReason::HealthLayout||reason==EnemyReason::HealthZero) ++enemy_scan_.health;
        if (reason==EnemyReason::TeamQuery) ++enemy_scan_.team_query;
        if (reason==EnemyReason::NotHostile) ++enemy_scan_.not_hostile;
        if (reason==EnemyReason::DeadQuery||reason==EnemyReason::Dead) ++enemy_scan_.dead;
        if (reason==EnemyReason::HealthLayout) ++enemy_scan_.health_layout;
        if (reason==EnemyReason::HealthZero) ++enemy_scan_.health_zero;
    }
    ue::Free(array.data);
}
bool ClearPath(const Context& context, Object* enemy, Vector feet, Vector target) {
    Object* library = ue::Default("/Script/Engine.Default__KismetSystemLibrary");
    Object* ignored[1] = {context.pawn}; const Array ignore{ignored, 1, 1};
    const uint8_t visibility = 0, no_debug = 0;
    Call trace(library, "LineTraceSingle");
    if (!trace.Set("WorldContextObject", context.pawn).Set("Start", feet).Set("End", target)
        .Set("TraceChannel", visibility).Set("bTraceComplex", false).Set("ActorsToIgnore", ignore)
        .Set("DrawDebugType", no_debug).Set("bIgnoreSelf", true).Invoke()) return false;
    if (!trace.Get<bool>("ReturnValue")) return true;
    const int32_t hit_size = ue::StructSize(Type("/Script/Engine.HitResult"));
    Object* gameplay = ue::Default("/Script/Engine.Default__GameplayStatics");
    Call split(gameplay, "BreakHitResult");
    unsigned char* hit = hit_size > 0 && hit_size < 512 ? trace.Slot("OutHit", static_cast<size_t>(hit_size)) : nullptr;
    unsigned char* input = hit ? split.Slot("Hit", static_cast<size_t>(hit_size)) : nullptr;
    if (!input) return false;
    std::memcpy(input, hit, static_cast<size_t>(hit_size));
    return split.Invoke() && split.Get<Object*>("HitActor") == enemy;
}
Object* NearbyEnemy(const Context& context) {
    near_scan_={};
    Vector position; float radius = 0, half = 0;
    if (!Position(context.pawn, position) || !Capsule(context.pawn, radius, half)) return nullptr;
    near_scan_.player_ready=true;
    Object* best = nullptr; double best_distance = 1e30;
    for (const Identity& enemy : enemies_) if (Same(enemy) && Enemy(enemy.object, context)) {
        ++near_scan_.valid;
        Vector location; float er = 0, eh = 0;
        if (!Position(enemy.object, location) || !Capsule(enemy.object, er, eh)) continue;
        ++near_scan_.capsules;
        const double dx=position.x-location.x,dy=position.y-location.y;
        const double spine_gap=(std::max)(0.,std::fabs(position.z-location.z)-(half-radius)-(eh-er));
        const double surface=(std::max)(0.,std::sqrt(dx*dx+dy*dy+spine_gap*spine_gap)-radius-er);
        if (near_scan_.nearest<0||surface<near_scan_.nearest) near_scan_.nearest=static_cast<float>(surface);
        if (surface>options_.enemy_radius) continue;
        ++near_scan_.in_range;
        const double spine=eh-er;
        const Vector target{location.x,location.y,(std::clamp)(position.z,location.z-spine,location.z+spine)};
        if (!ClearPath(context,enemy.object,position,target)) continue;
        ++near_scan_.visible;
        if (surface<best_distance) { best_distance=surface; best=enemy.object; }
    }
    return best;
}
bool Gravity(Object* movement,float& value,int32_t* out_offset=nullptr) {
    Object* field=Field(ue::ClassOf(movement),"GravityScale");
    if (!ue::Alive(movement)||!field||Kind(field)!=0x100ull||ue::At<int32_t>(field,0x34)!=4||ue::At<int32_t>(field,0x44)!=424) return false;
    value=ue::At<float>(movement,424); if (out_offset) *out_offset=424;
    return std::isfinite(value)&&value>=0&&value<=100;
}
bool SameFloat(float a,float b) { return !std::memcmp(&a,&b,sizeof(a)); }
bool ReleaseGravityValue() {
    if (!slow_.owned) return true;
    if (!Same(slow_.movement)) { slow_.owned=false; return true; }
    float current=0;
    if (!Gravity(slow_.movement.object,current)) return false;
    if (!SameFloat(current,slow_.last)) {
        // An external ability now owns the value. Do not restore over it and
        // do not keep multiplying its new gravity each frame.
        slow_.owned=false; slow_.yielded=true;
        Logf("AIR_JC gravity yielded to external value current=%.4f last=%.4f",current,slow_.last); return true;
    }
    ue::At<float>(slow_.movement.object,424)=slow_.original;
    if (!Gravity(slow_.movement.object,current)||!SameFloat(current,slow_.original)) return false;
    slow_.owned=false; return true;
}
bool RestoreSlowFall() {
    if (!ReleaseGravityValue()) return false;
    slow_={}; return true;
}
void ArmSlowFall(const Context& context) {
    if (!RestoreSlowFall()||!options_.slow_fall||!options_.stomp_jump) return;
    slow_.pawn=Identify(context.pawn); slow_.movement=Identify(context.movement); slow_.armed=true;
}
float ReadDownCap(const Context& context) {
    Call get(context.movement,"GetPhysicsVolume");
    if (!get.Slot("ReturnValue",8)||!get.Invoke()) return 0;
    Object* volume=get.Get<Object*>("ReturnValue"),*field=ue::Alive(volume)?Field(ue::ClassOf(volume),"TerminalVelocity"):nullptr;
    if (!field||Kind(field)!=0x100ull||ue::At<int32_t>(field,0x34)!=4) return 0;
    const int32_t offset=ue::At<int32_t>(field,0x44); if (offset<0) return 0;
    const float terminal=ue::At<float>(volume,offset);
    return std::isfinite(terminal)&&terminal>0&&terminal<=1e6f?terminal*.75f:0.f;
}
void UpdateSlowFall(const Context& context) {
    if (!slow_.armed) return;
    if (!options_.slow_fall||!options_.stomp_jump||!Same(slow_.pawn)||slow_.pawn.object!=context.pawn||
        !Same(slow_.movement)||slow_.movement.object!=context.movement) { RestoreSlowFall(); return; }
    Call falling(context.movement,"IsFalling"),dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");
    if (!falling.Invoke()||!falling.Get<bool>("ReturnValue")||!dead.Set("Pawn",context.pawn).Invoke()||dead.Get<bool>("ReturnValue")) { RestoreSlowFall(); return; }
    Object* velocity=Field(ue::ClassOf(context.movement),"Velocity");
    if (!velocity||ue::At<int32_t>(velocity,0x44)!=216||ue::At<int32_t>(velocity,0x34)!=24) return;
    Vector speed=ue::At<Vector>(context.movement,216); if (!std::isfinite(speed.z)) return;
    if (speed.z>=0) {
        // A landing and the next normal jump can both happen between our GT
        // samples. Once this JC has descended, a new nonnegative trajectory
        // ends the entire cycle even when IsFalling never sampled false.
        if (slow_.seen_descent) {
            const bool restored=RestoreSlowFall(); if (!restored) restore_pending_=true;
            Logf("AIR_JC post-descent new rise disarm=%d; owned gravity restored or yielded, normal jump height untouched",restored?1:0);
        } else if (!ReleaseGravityValue()) restore_pending_=true;
        return; // Ascent never receives slower gravity.
    }
    slow_.seen_descent=true;
    if (!GameplayInputAllowed(context)) return;
    float current=0; if (!Gravity(context.movement,current)) return;
    if (slow_.owned&&!SameFloat(current,slow_.last)) {
        slow_.owned=false; slow_.yielded=true;
        Logf("AIR_JC slow fall stopped: external gravity write current=%.4f own=%.4f",current,slow_.last);
    }
    if (slow_.yielded) return;
    if (!slow_.owned) {
        slow_.original=current; slow_.last=current*.75f;
        slow_.owned=true; // Record ownership before the write for exception recovery.
        ue::At<float>(context.movement,424)=slow_.last;
        if (!Gravity(context.movement,current)||!SameFloat(current,slow_.last)) { restore_pending_=true; return; }
        Logf("AIR_JC descent gravity %.4f -> %.4f; ascent untouched",slow_.original,slow_.last);
    }
    slow_.cap=ReadDownCap(context);
    // A cap is a bound, not repeated multiplicative damping. The shared
    // PhysicsVolume is read only; only this avatar's velocity may be clipped.
    if (slow_.cap>0&&speed.z < -slow_.cap) { speed.z=-slow_.cap; ue::At<Vector>(context.movement,216)=speed; }
}
bool MakeAttribute(const wchar_t* text, Attribute& attribute) {
    Object* library = ue::Default("/Script/ProjectPBZ.Default__PBZBlueprintFunctionLibrary");
    Object* cls = Type("/Script/CharacterBasics.BasicAttributeSet");
    const int32_t count = static_cast<int32_t>(std::wcslen(text) + 1); const ue::String name{text, count, count};
    Call make(library, "MakeAttribute");
    if (!make.Set("AttributeName", name).Set("Set", cls).Invoke()) return false;
    attribute = make.Get<Attribute>("ReturnValue");
    return ue::At<Object*>(attribute.bytes, 16) != nullptr;
}
void FreeAttribute(Attribute& attribute) {
    // FGameplayAttribute: FString@0; FFieldPath@16 (resolved pointer, weak
    // owner, TArray<FName>@32); UObject attribute owner@48. Both allocations
    // originate in native MakeAttribute and use FMemory::Free.
    ue::Free(ue::At<void*>(attribute.bytes, 0)); ue::Free(ue::At<void*>(attribute.bytes, 32)); attribute = {};
}
bool Refill(const Context& context, float& full) {
    Object* type = Type("/Script/GameplayAbilities.GameplayAttribute");
    if (ue::StructSize(type) != 56 || ue::Offset(type, "AttributeName") != 0 || ue::Offset(type, "Attribute") != 16 || ue::Offset(type, "AttributeOwner") != 48) return false;
    Attribute current, maximum;
    const bool made_current = MakeAttribute(L"Shachi", current), made_maximum = MakeAttribute(L"Shachi_Max", maximum);
    bool success = false; full = 0;
    if (made_current && made_maximum) {
        Object* library = ue::Default("/Script/ProjectPBZ.Default__PBZBlueprintFunctionLibrary");
        Call get(library, "GetAttributeFromCharacter");
        if (get.Set("Character", context.pawn).Set("Attribute", maximum).Invoke()) {
            full = get.Get<float>("ReturnValue");
            Object* utils = ue::Default("/Script/SGameAttributes.Default__AttributeUtils");
            Call set(utils, "ModifyCharacterAttributeValue"); const uint8_t override_op = 3;
            if (std::isfinite(full) && full > 0 && full < 1e9f && set.Set("Character", context.pawn).Set("Attribute", current).Set("ModiferOp", override_op).Set("Value", full).Invoke()) {
                Call check(library, "GetAttributeFromCharacter");
                success = check.Set("Character", context.pawn).Set("Attribute", current).Invoke() && std::fabs(check.Get<float>("ReturnValue") - full) < 0.1f;
            }
        }
    }
    FreeAttribute(current); FreeAttribute(maximum); return success;
}
ResourceReward IncrementResource(const Context& context,const wchar_t* resource,const wchar_t* limit) {
    ResourceReward result;
    Object* type=Type("/Script/GameplayAbilities.GameplayAttribute");
    if (ue::StructSize(type)!=56||ue::Offset(type,"AttributeName")!=0||ue::Offset(type,"Attribute")!=16||ue::Offset(type,"AttributeOwner")!=48) return result;
    Attribute current,maximum;
    const bool made_current=MakeAttribute(resource,current),made_maximum=MakeAttribute(limit,maximum);
    if (made_current&&made_maximum) {
        auto read=[&](const Attribute& attribute,float& value) {
            Call get(context.asc,"GetGameplayAttributeValue");
            if (!get.Slot("bFound",1)||!get.Slot("ReturnValue",4)||!get.Set("Attribute",attribute).Invoke()||!get.Get<bool>("bFound")) return false;
            value=get.Get<float>("ReturnValue"); return std::isfinite(value)&&value>=0&&value<1e9f;
        };
        if (read(current,result.before)&&read(maximum,result.maximum)) {
            result.known=true; result.after=result.before;
            // The two quantities are independent attributes, not cooldown
            // charges or permanent capacity. At cap, do not dispatch a write
            // that could clamp an existing over-cap quantity downwards.
            const float target=result.before>=result.maximum?result.before:(std::min)(result.maximum,result.before+1.f);
            const float amount=target-result.before;
            // Large debug counts can have float spacing greater than one.
            // Never claim +1 when it rounded to zero, or dispatch a +2 write.
            bool applied=result.before>=result.maximum||(amount>0&&amount<=1.f);
            if (applied&&amount>0) {
                Call add(ue::Default("/Script/SGameAttributes.Default__AttributeUtils"),"ModifyCharacterAttributeValue");
                applied=add.Set("Character",context.pawn).Set("Attribute",current).Set("ModiferOp",uint8_t{0}).Set("Value",amount).Invoke();
            }
            float max_after=0;
            result.verified=applied&&read(current,result.after)&&read(maximum,max_after)&&SameFloat(max_after,result.maximum)&&
                std::fabs(result.after-target)<0.001f&&result.after>=result.before;
        }
    }
    FreeAttribute(current); FreeAttribute(maximum); return result;
}
std::wstring ObjectText(Object* object) {
    char text[256]{}; if (object) ue::NameText(ue::NameOf(object),text,sizeof(text));
    wchar_t wide[256]{}; MultiByteToWideChar(CP_UTF8,0,text,-1,wide,static_cast<int>(sizeof(wide)/sizeof(wide[0])));
    return wide;
}
std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    if (value.size()>0x7fffffff) return "<text_too_long>";
    const int length=static_cast<int>(value.size());
    const int bytes=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),length,nullptr,0,nullptr,nullptr);
    if (bytes<=0) return "<invalid_utf16>";
    std::string result(static_cast<size_t>(bytes),'\0');
    if (WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),length,result.data(),bytes,nullptr,nullptr)!=bytes) return "<utf8_conversion_failed>";
    return result;
}
void JcGate(const char* reason,const wchar_t* message,Object* ability=nullptr,Name tag={}) {
    ++gate_sequence_;
    char tag_text[192]{}; if (!tag.None()) ue::NameText(tag,tag_text,sizeof(tag_text));
    const std::wstring object=ue::Alive(ability)?ObjectText(ue::ClassOf(ability)):L"";
    wchar_t text[512]{};
    swprintf_s(text,L"%s%s%s%hs",message,object.empty()?L"":L"：",object.c_str(),tag_text);
    AcquireSRWLockExclusive(&state_lock); state_.jc_status=text; const uint32_t edges=state_.jump_edges; ReleaseSRWLockExclusive(&state_lock);
    const ULONGLONG now=GetTickCount64();
    auto gate=std::find_if(gate_logs_.begin(),gate_logs_.end(),[&](const GateLog& item) { return !std::strcmp(item.reason,reason); });
    if (gate==gate_logs_.end()) gate_logs_.push_back({reason,now+1000});
    else { if (now<gate->next) return; gate->next=now+1000; }
    Logf("AIR_JC_GATE reason=%s air=%d jc=%d edges=%u scanned=%u accepted=%u ai_reject=%u team_reject=%u health_reject=%u team_api_fail=%u nonhostile=%u dead=%u health_layout_fail=%u health_zero=%u live=%u capsules=%u in_range=%u visible=%u nearest_cm=%.1f radius_cm=%.1f ability=%s tag=%s",
        reason,options_.air_skills?1:0,options_.stomp_jump?1:0,edges,enemy_scan_.pawns,enemy_scan_.accepted,enemy_scan_.no_ai,enemy_scan_.team,enemy_scan_.health,
        enemy_scan_.team_query,enemy_scan_.not_hostile,enemy_scan_.dead,enemy_scan_.health_layout,enemy_scan_.health_zero,
        near_scan_.valid,near_scan_.capsules,near_scan_.in_range,near_scan_.visible,near_scan_.nearest,options_.enemy_radius,Utf8(object).c_str(),tag_text);
}
void NoNearEnemy() {
    if (!near_scan_.player_ready) JcGate("player_capsule_unavailable",L"JC 未触发：主角的碰撞胶囊/位置未就绪");
    else if (enemies_.empty()) JcGate("enemy_filter_empty",L"JC 未触发：未识别到存活敌对 AI；请查看队伍/生命过滤计数");
    else if (!near_scan_.valid) JcGate("enemy_no_longer_valid",L"JC 未触发：候选敌人已失效或队伍/生命条件变化");
    else if (!near_scan_.capsules) JcGate("capsule_unavailable",L"JC 未触发：主角或敌人的碰撞胶囊/位置未就绪");
    else if (!near_scan_.in_range) JcGate("outside_radius",L"JC 未触发：最近敌人仍超出近敌范围（按三维胶囊表面距离）");
    else JcGate("line_of_sight_blocked",L"JC 未触发：附近敌人被场景碰撞遮挡或可见性查询失败");
}
Object* SkillRoute(Object* ability) {
    const int32_t offset = Offset(ability,"AbilityTags"); if (offset < 0) return nullptr;
    const Tags& tags = ue::At<Tags>(ability,offset);
    if (!ArrayOk(tags.tags,sizeof(Name))) return nullptr;
    auto matches=[&](const std::vector<Name>& triggers) {
        for (Name trigger:triggers) {
            Call has(ue::Default("/Script/GameplayTags.Default__BlueprintGameplayTagLibrary"),"HasTag");
            if (has.Set("TagContainer",tags).Set("Tag",trigger).Set("bExactMatch",false).Invoke() && has.Get<bool>("ReturnValue")) return true;
        }
        return false;
    };
    if (Same(combatart_action_) && matches(combatart_triggers_)) return combatart_action_.object;
    if (Same(shachi_action_) && matches(shachi_triggers_)) return shachi_action_.object;
    return nullptr;
}
struct ChargeConfig { unsigned char bytes[16]{}; int32_t maximum = 0; };
bool ReadChargeConfig(Object* ability,ChargeConfig& result) {
    Object* field = Field(ue::ClassOf(ability),"Cooldown Data");
    if (!field || Kind(field)!=0x100000ull || ue::At<int32_t>(field,0x34)!=16) return false;
    Object* type = ue::At<Object*>(field,0x70);
    const int32_t offset = ue::At<int32_t>(field,0x44);
    // Exact generated Blueprint struct and member names from the cooked asset.
    if (offset<0 || ue::StructSize(type)!=16 ||
        ue::Offset(type,"Cooldown_2_C711E94E40D3CE24089AD884023A69F9")!=0 ||
        ue::Offset(type,"Charges_5_33805C064A3D0291CC57F6BD10DAD3E7")!=8 || !Readable(ability+offset,16)) return false;
    std::memcpy(result.bytes,ability+offset,16);
    const double cooldown = ue::At<double>(result.bytes,0); result.maximum = ue::At<int32_t>(result.bytes,8);
    return std::isfinite(cooldown) && cooldown>=0 && cooldown<1e12 && result.maximum>=0 && result.maximum<=1024;
}
bool ReadCharges(Object* ability,SkillState& result) {
    ChargeConfig config; if (!ReadChargeConfig(ability,config)) return false;
    Call check(ability,"Get Current Cooldown");
    if (!check.Slot("Current Cooldown",8) || !check.Slot("Current Charges",4) || !check.Invoke()) return false;
    result.cooldown = check.Get<double>("Current Cooldown"); result.charges = check.Get<int32_t>("Current Charges"); result.maximum_charges = config.maximum;
    result.restored = std::isfinite(result.cooldown) && std::fabs(result.cooldown)<0.001 && result.charges==result.maximum_charges;
    return std::isfinite(result.cooldown) && result.cooldown>=0 && result.charges>=0 && result.charges<=result.maximum_charges;
}
void RefreshSkillStates(const std::vector<Object*>& abilities) {
    std::vector<SkillState> skills;
    for (Object* ability:abilities) if (Object* action=SkillRoute(ability)) {
        SkillState value; value.ability = ObjectText(ue::ClassOf(ability)); value.input_action = ObjectText(action);
        if (ReadCharges(ability,value)) skills.push_back(std::move(value));
    }
    AcquireSRWLockExclusive(&state_lock); state_.skills = std::move(skills); ReleaseSRWLockExclusive(&state_lock);
}
struct Restored { uint32_t cooldowns=0, charges=0, failed=0; };
bool GuardDodgeAbility(Object* ability) {
    Object* base=Type("/PBZCombatAsset/Prototypes/Abilities/Base/BP_SpecialDash_Base.BP_SpecialDash_Base_C");
    return base&&ue::Alive(ability)&&ue::IsA(ability,base);
}
Restored Cooldowns(const Context& context) {
    Restored result;
    std::vector<Object*> effects;
    std::vector<Object*> cooldown_verified, gas_abilities;
    const auto abilities = Abilities(context);
    for (Object* ability : abilities) {
        if (GuardDodgeAbility(ability)) continue; // Its independent JC reward owns this cooldown.
        // The base Blueprint consumes a time mark and charges, not a GAS GE.
        // Restore Cooldown subtracts Seconds; a full refill preserves its configured
        // cooldown/charge count rather than replacing those gameplay parameters.
        // The decoded getter explicitly treats MarkTime < 0 as all charges
        // ready. Cross that sentinel through its native Blueprint entry point;
        // do not create a huge elapsed-time quotient that can overflow FFloor.
        Object* mark = Field(ue::ClassOf(ability), "Cooldown Mark Time");
        if (mark && Kind(mark) == 0x100000000ull && ue::At<int32_t>(mark, 0x34) == 8) {
            const double before = ue::At<double>(ability, ue::At<int32_t>(mark, 0x44));
            ChargeConfig original;
            bool verified=false;
            if (std::isfinite(before) && std::fabs(before) < 1e12 && ReadChargeConfig(ability,original)) {
                Call restore(ability, "Restore Cooldown"); const double seconds = (std::max)(0.0, before) + 1.0;
                if (restore.Set("Seconds", seconds).Invoke()) {
                    SkillState check; ChargeConfig after;
                    verified = ReadCharges(ability,check) && check.restored && ReadChargeConfig(ability,after) &&
                        !std::memcmp(original.bytes,after.bytes,sizeof(original.bytes));
                    if (verified) { ++result.cooldowns; cooldown_verified.push_back(ability); if (original.maximum>0) ++result.charges; }
                }
            }
            if (!verified) ++result.failed;
        }
        Object* effect = Property(ability, "CooldownGameplayEffectClass");
        if (effect&&effect!=Type("/PBZCombatAsset/Prototypes/GameplayEffects/GE_SpecialDashCooldown.GE_SpecialDashCooldown_C")) {
            gas_abilities.push_back(ability);
            if (std::find(effects.begin(),effects.end(),effect)==effects.end()) effects.push_back(effect);
        }
    }
    for (Object* effect : effects) {
        Call remove(context.asc, "RemoveActiveGameplayEffectBySourceEffect"); const int32_t all_stacks = -1;
        // This exact build's function is void. Count only an independent
        // post-removal cooldown getter, never a fabricated return/count.
        remove.Set("GameplayEffect",effect).Set("InstigatorAbilitySystemComponent",context.asc).Set("StacksToRemove",all_stacks).Invoke();
    }
    for (Object* ability:gas_abilities) {
        Call remaining(ability,"GetCooldownTimeRemaining");
        const bool known=remaining.Slot("ReturnValue",4) && remaining.Invoke();
        const float value=known?remaining.Get<float>("ReturnValue"):-1.f;
        if (known && std::isfinite(value) && std::fabs(value)<0.001f) {
            if (std::find(cooldown_verified.begin(),cooldown_verified.end(),ability)==cooldown_verified.end()) ++result.cooldowns;
        } else ++result.failed;
    }
    RefreshSkillStates(abilities);
    return result;
}
struct WeaponSwitchReset { bool verified=false; uint32_t slots=0; float before=0,after=0; };
bool WeaponSlotTags(Object* weapons,std::vector<Name>& slots) {
    Object* property=Field(ue::ClassOf(weapons),"RuntimeWeaponData");
    if (!property||ue::At<int32_t>(property,0x44)!=248||Kind(property)!=0x400000000000ull||ue::At<int32_t>(property,0x34)!=80) return false;
    Object* key=ue::At<Object*>(property,0x70);
    if (!key||Kind(key)!=0x100000ull||ue::At<int32_t>(key,0x34)!=8||ue::At<Object*>(key,0x70)!=Type("/Script/GameplayTags.GameplayTag")) return false;
    return VisitMap(weapons+248,property,[&](unsigned char* row,int32_t) {
        const Name tag=ue::At<Name>(row,0); if (!tag.None()) slots.push_back(tag);
    })&&!slots.empty();
}
WeaponSwitchReset ResetWeaponSwitch(const Context& context) {
    WeaponSwitchReset result;
    Object* weapons=Component(context.pawn,"/Script/SGameCombatSystem.CharacterWeaponsComponent");
    Object* config=weapons?Field(ue::ClassOf(weapons),"ActorConfig"):nullptr;
    if (!config||Kind(config)!=0x10000ull||ue::At<int32_t>(config,0x44)!=232||ue::At<int32_t>(config,0x34)!=8||!ue::Alive(Property(weapons,"ActorConfig"))) return result;
    Call implements(ue::Default("/Script/Engine.Default__KismetSystemLibrary"),"DoesImplementInterface");
    Object* config_interface=Type("/Script/SGameCombatSystem.CharacterWeaponsActorConfig");
    if (!config_interface||!implements.Set("TestObject",Property(weapons,"ActorConfig")).Set("Interface",config_interface).Invoke()||!implements.Get<bool>("ReturnValue")) return result;
    const Identity id=Identify(weapons); if (!Same(id)) return result;
    Call owner(weapons,"GetOwner"); if (!owner.Invoke()||owner.Get<Object*>("ReturnValue")!=context.pawn) return result;
    std::vector<Name> slots; if (!WeaponSlotTags(weapons,slots)) return result;
    const auto read=[&](Name tag,float& value) {
        Call get(weapons,"GetCurrentChangeWeaponCooldown");
        if (!Same(id)||!get.Slot("ReturnValue",4)||!get.Set("SlotTag",tag).Invoke()) return false;
        value=get.Get<float>("ReturnValue"); return std::isfinite(value);
    };
    for (Name tag:slots) { float value=0; if (!read(tag,value)) return result; result.before=(std::max)(result.before,value); }
    if (!Call(weapons,"ResetChangeWeaponCooldowns").Invoke()||!Same(id)) return result;
    std::vector<Name> after; if (!WeaponSlotTags(weapons,after)||after.size()!=slots.size()) return result;
    for (size_t i=0;i<slots.size();++i) if (!(slots[i]==after[i])) return result;
    for (Name tag:slots) { float value=0; if (!read(tag,value)) return result; result.after=(std::max)(result.after,value); if (value>0.001f) return result; ++result.slots; }
    result.verified=true; return result;
}
struct GuardDodgeReset { bool known=false,verified=false; int32_t before=0,after=0; };
GuardDodgeReset ResetGuardDodge(const Context& context) {
    GuardDodgeReset result;
    // The cooked BP_SpecialDash_Base explicitly applies this effect to its owner;
    // K2_CanActivateAbility tests SpecialCooldown. Its GAS CooldownGameplayEffectClass
    // is null, so GetCooldownTimeRemaining alone would falsely report success.
    Object* effect=Type("/PBZCombatAsset/Prototypes/GameplayEffects/GE_SpecialDashCooldown.GE_SpecialDashCooldown_C");
    if (!effect||!ue::Alive(context.asc)) return result;
    const auto abilities=Abilities(context);
    if (std::none_of(abilities.begin(),abilities.end(),&GuardDodgeAbility)) return result;
    const Identity asc=Identify(context.asc); if (!Same(asc)) return result;
    const Name tag=ue::MakeName(L"CharacterState.AbilityState.Dash.SpecialCooldown"); if (tag.None()) return result;
    const auto count=[&](int32_t& value) {
        Call get(context.asc,"GetGameplayEffectCount");
        if (!Same(asc)||!get.Slot("ReturnValue",4)||!get.Set("SourceGameplayEffect",effect)
            .Set("OptionalInstigatorFilterComponent",context.asc).Set("bEnforceOnGoingCheck",false).Invoke()) return false;
        value=get.Get<int32_t>("ReturnValue"); return value>=0&&value<1024;
    };
    const auto blocked=[&](bool& value) {
        Call has(context.asc,"HasMatchingGameplayTag","/Script/GameplayTags.GameplayTagAssetInterface");
        if (!Same(asc)||!has.Slot("ReturnValue",1)||!has.Set("TagToCheck",tag).Invoke()) return false;
        value=has.Get<bool>("ReturnValue"); return true;
    };
    bool before_tag=false,after_tag=true;
    if (!count(result.before)||!blocked(before_tag)) return result;
    result.known=true;
    if (result.before) {
        Call remove(context.asc,"RemoveActiveGameplayEffectBySourceEffect"); const int32_t all=-1;
        if (!remove.Set("GameplayEffect",effect).Set("InstigatorAbilitySystemComponent",context.asc).Set("StacksToRemove",all).Invoke()) return result;
    }
    // Do not remove generic Dash.Cooldown, loose tags, invulnerability or foreign effects.
    result.verified=count(result.after)&&blocked(after_tag)&&result.after==0&&!after_tag;
    Logf("AIR_JC_GUARD_DODGE known=%d verified=%d self_effects=%d->%d special_tag=%d->%d",
        result.known?1:0,result.verified?1:0,result.before,result.after,before_tag?1:0,after_tag?1:0);
    return result;
}
struct RewardResult { float shachi_max=0; bool shachi=false; ResourceReward pure,shadow; Restored skills; WeaponSwitchReset weapon; GuardDodgeReset guard; };
RewardResult ApplyRewards(const Context& context) {
    RewardResult result;
    if (options_.reward_shachi) result.shachi=Refill(context,result.shachi_max);
    if (options_.reward_pure_shachi) result.pure=IncrementResource(context,L"UltimateChargeCount",L"UltimateChargeCount_Max");
    if (options_.reward_spirit_shadow) result.shadow=IncrementResource(context,L"SpiritShadow",L"SpiritShadow_Max");
    if (options_.reward_skills) result.skills=Cooldowns(context);
    if (options_.reward_weapon_switch) result.weapon=ResetWeaponSwitch(context);
    if (options_.reward_guard_dodge) result.guard=ResetGuardDodge(context);
    return result;
}
bool FullBool(Object* object,const char* property,bool& value) {
    Object* field=Field(ue::ClassOf(object),property);
    static constexpr unsigned char layout[]={1,0,1,255};
    if (!field||Kind(field)!=0x20000ull||ue::At<int32_t>(field,0x34)!=1||std::memcmp(field+0x70,layout,sizeof(layout))) return false;
    const int32_t offset=ue::At<int32_t>(field,0x44); if (offset<0) return false;
    value=ue::At<uint8_t>(object,offset)!=0; return true;
}
bool PlayerSkill(Object* ability) {
    if (ue::NameIs(ue::NameOf(ue::ClassOf(ability)),"GA_Hun_Jump_C")) return false;
    const int32_t offset=Offset(ability,"AbilityTags"); if (offset<0) return false;
    const Tags& tags=ue::At<Tags>(ability,offset); if (!ArrayOk(tags.tags,sizeof(Name))) return false;
    // All current player action skills, not a weapon/attack-name allowlist.
    // Passive/resource/system GAs without Action identity are left running.
    for (int32_t i=0;i<tags.tags.num;++i) if (Family(static_cast<Name*>(tags.tags.data)[i],"Ability.Action")) return true;
    Object* action=Type("/MagCombatAbilities/Abilities/BP_ActionAbilityBase.BP_ActionAbilityBase_C");
    Object* global=Type("/MagCombatAbilities/Abilities/BP_GlobalActionAbility_Base.BP_GlobalActionAbility_Base_C");
    return (action&&ue::IsA(ability,action))||(global&&ue::IsA(ability,global));
}
bool SpecFor(const Context& context,Object* ability,int32_t* handle=nullptr,bool active_only=true) {
    Object* spec=Type("/Script/GameplayAbilities.GameplayAbilitySpec"),*container=Type("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t specs=Offset(context.asc,"ActivatableAbilities"),items=ue::Offset(container,"Items"),active=ue::Offset(spec,"ActiveCount"),base=ue::Offset(spec,"Ability");
    if (specs!=1048||items!=264||ue::StructSize(spec)!=240||active!=40||base!=16) return false;
    const Array array=ue::At<Array>(context.asc,specs+items); if (!ArrayOk(array,240,1024)) return false;
    for (int32_t i=0;i<array.num;++i) {
        auto* row=static_cast<unsigned char*>(array.data)+static_cast<size_t>(i)*240;
        if (!ue::At<Object*>(row,base)||(active_only&&!ue::At<uint8_t>(row,active))) continue;
        for (const char* name:{"NonReplicatedInstances","ReplicatedInstances"}) {
            const int32_t offset=ue::Offset(spec,name); if (offset<0) continue;
            const Array instances=ue::At<Array>(row,offset); if (!ArrayOk(instances,sizeof(Object*),64)) return false;
            for (int32_t j=0;j<instances.num;++j) if (static_cast<Object**>(instances.data)[j]==ability) {
                if (handle) *handle=ue::At<int32_t>(row,12); return true;
            }
        }
    }
    return false;
}
bool AnimatingAbility(const Context& context,Object*& ability) {
    ability=nullptr;
    Object* field=Field(ue::ClassOf(context.asc),"LocalAnimMontageInfo");
    if (!field||Kind(field)!=0x100000ull||ue::At<int32_t>(field,0x44)!=2016||ue::At<int32_t>(field,0x34)!=40) return false;
    Object* type=ue::At<Object*>(field,0x70),*animating=Field(type,"AnimatingAbility");
    if (!animating||Kind(animating)!=0x8000000ull||ue::At<int32_t>(animating,0x44)!=28||ue::At<int32_t>(animating,0x34)!=8) return false;
    const int32_t index=ue::At<int32_t>(context.asc,2044),serial=ue::At<int32_t>(context.asc,2048);
    if (index<0||serial<=0) return true;
    const auto* item=Item(index); if (!item) return true;
    Object* object=nullptr; int32_t current=0; std::memcpy(&object,item+8,sizeof(object)); std::memcpy(&current,item+16,sizeof(current));
    if (current==serial&&ue::Alive(object)) ability=object;
    return true;
}
bool CancelEnded(const Identity& identity) {
    if (!IdentityExists(identity)) return true; // A collected ended instance cannot commit a late End callback.
    if (!Same(identity)) return false;
    bool active=true,ending=true;
    if (!FullBool(identity.object,"bIsActive",active)||!FullBool(identity.object,"bIsAbilityEnding",ending)||active||ending) return false;
    const int32_t tasks=Offset(identity.object,"ActiveTasks"); if (tasks!=768) return false;
    const Array array=ue::At<Array>(identity.object,tasks);
    return ArrayOk(array,sizeof(Object*),1024)&&array.num==0;
}
bool SafeStepState(const Context& context,Name* protected_tag=nullptr) {
    if (protected_tag) *protected_tag={};
    Object* state=Component(context.pawn,"/Script/SGameActorState.ActorStateComponent");
    Call current(state,"GetCurrentStatesContainer");
    if (!ue::Alive(state)||!current.Slot("CurrentStates",32)||!current.Invoke()) return false;
    Tags tags=current.Get<Tags>("CurrentStates");
    bool safe=ArrayOk(tags.tags,sizeof(Name))&&ArrayOk(tags.parents,sizeof(Name));
    // These state families are present in the game's authored compiled state
    // asset. Action.Phase and Jumping are deliberately not blocked.
    constexpr const char* denied[]={"CharacterState.Dead","CharacterState.FallDown","CharacterState.HitReact",
        "CharacterState.Narrative","CharacterState.Native.Sequence","CharacterState.SpecialMove","CharacterState.StoryCameraLock",
        "CharacterState.InVolume.SpecialMove"};
    if (safe) for (int32_t i=0;i<tags.tags.num;++i) for (const char* family:denied) if (Family(static_cast<Name*>(tags.tags.data)[i],family)) {
        safe=false; if (protected_tag) *protected_tag=static_cast<Name*>(tags.tags.data)[i];
    }
    ue::Free(tags.tags.data); ue::Free(tags.parents.data);
    return safe;
}
bool NormalJumpAllowed(const Context& context,bool require_ground=true) {
    Object* state=Component(context.pawn,"/Script/SGameActorState.ActorStateComponent");
    Call current(state,"GetCurrentStatesContainer");
    if (!ue::Alive(state)||!current.Slot("CurrentStates",32)||!current.Invoke()) return false;
    Tags tags=current.Get<Tags>("CurrentStates");
    bool allowed=ArrayOk(tags.tags,sizeof(Name))&&ArrayOk(tags.parents,sizeof(Name)),ground=false;
    if (allowed) for (int32_t i=0;i<tags.tags.num;++i) {
        const Name tag=static_cast<Name*>(tags.tags.data)[i];
        if (Family(tag,"CharacterState.Space.Ground")) ground=true;
        // Exact ordinary Jump branch's authored IgnoredTags. Evaluate after
        // cancel cleanup, so a skill-owned Forbid.Jump can end normally first.
        if (Family(tag,"CharacterState.Forbid.Jump")||Family(tag,"CharacterState.InVolume.SpecialMove")) allowed=false;
    }
    ue::Free(tags.tags.data); ue::Free(tags.parents.data); return allowed&&(!require_ground||ground);
}
int FaultFilter(EXCEPTION_POINTERS* exception);
bool InvokeCancelGuarded(Object* ability) {
    __try { return Call(ability,"K2_CancelAbility").Invoke(); }
    __except (FaultFilter(GetExceptionInformation())) {
        Logf("AIR_JC native cancel fault=%lu; immediate temporary-ref release required",fault_code_); return false;
    }
}
bool Stamp(const Context& context,Object* ability,CancelStamp& stamp) {
    stamp.ability=Identify(ability); stamp.asc=context.asc;
    if (!Same(stamp.ability)||!SpecFor(context,ability,&stamp.handle)) return false;
    Object* info=Field(ue::ClassOf(ability),"CurrentActivationInfo");
    if (!info||ue::At<int32_t>(info,0x44)!=212||ue::At<int32_t>(info,0x34)!=20) return false;
    std::memcpy(stamp.activation,ability+212,20);
    const int32_t tasks=Offset(ability,"ActiveTasks"); if (tasks!=768) return false;
    const Array array=ue::At<Array>(ability,tasks); if (!ArrayOk(array,sizeof(Object*),256)) return false;
    stamp.task_count=array.num; stamp.task_hash=1469598103934665603ull;
    for (int32_t i=0;i<array.num;++i) {
        stamp.tasks[i]=Identify(static_cast<Object**>(array.data)[i]); if (!Same(stamp.tasks[i])) return false;
        stamp.task_hash^=reinterpret_cast<uintptr_t>(stamp.tasks[i].object); stamp.task_hash*=1099511628211ull;
        stamp.task_hash^=static_cast<uint32_t>(stamp.tasks[i].serial); stamp.task_hash*=1099511628211ull;
    }
    stamp.montage=Property(ability,"CurrentMontage"); return true;
}
bool RestoreCancelFlag(const Context& context,const CancelStamp& stamp,bool original,bool cancel_invoked) {
    if (!Same(stamp.ability)) return true;
    Object* ability=stamp.ability.object; bool current=false,active=true,ending=true;
    if (!FullBool(ability,"bIsCancelable",current)) return false;
    if (current==original) return true;
    if (!FullBool(ability,"bIsActive",active)||!FullBool(ability,"bIsAbilityEnding",ending)) return false;
    // Authority PredictionKey/ActivationInfo can repeat. Once cancellation was
    // dispatched, *any* still-active state may be a synchronous OnEnd restart;
    // never push the old false flag into it, even if all stamps happen to match.
    if (cancel_invoked&&(active||ending)) return false;
    const bool same_activation=!std::memcmp(stamp.activation,ability+212,20);
    if ((active||ending)&&!same_activation) { Logf("AIR_JUMP cancel flag retained: active activation info changed"); return false; }
    int32_t handle=0; const bool referenced=ue::Alive(context.asc)&&SpecFor(context,ability,&handle,false);
    if ((active||ending)&&referenced&&handle!=stamp.handle) { Logf("AIR_JUMP cancel flag retained: active spec handle changed"); return false; }
    if (active||ending) {
        CancelStamp now;
        if (!Stamp(context,ability,now)||!stamp.task_count||stamp.task_count!=now.task_count||stamp.task_hash!=now.task_hash||stamp.montage!=now.montage) {
            Logf("AIR_JUMP cancel flag retained: active activation/task identity changed or unproven"); return false;
        }
        for (int32_t i=0;i<stamp.task_count;++i) if (!Same(stamp.tasks[i])||stamp.tasks[i].object!=now.tasks[i].object||
            stamp.tasks[i].index!=now.tasks[i].index||stamp.tasks[i].serial!=now.tasks[i].serial) return false;
        Call asc(ability,"GetAbilitySystemComponentFromActorInfo");
        if (!asc.Invoke()||asc.Get<Object*>("ReturnValue")!=stamp.asc) return false;
    }
    Call restore(ability,"SetCanBeCanceled");
    if (!restore.Set("bCanBeCanceled",original).Invoke()||!FullBool(ability,"bIsCancelable",current)) return false;
    return current==original;
}
bool RestoreFlagGuarded(const FlagLease& lease) {
    __try {
        Context context; context.asc=Same(lease.asc)?lease.asc.object:nullptr;
        return RestoreCancelFlag(context,lease.stamp,lease.original,lease.cancel_invoked);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool RestoreCancelFlags() {
    bool ok=true;
    for (auto at=cancel_flags_.begin();at!=cancel_flags_.end();) {
        if (RestoreFlagGuarded(*at)) at=cancel_flags_.erase(at);
        else { ok=false; ++at; }
    }
    return ok;
}
enum class CancelResult { Done,Wait,Failed };
bool QuietCancelSetter(const Context& context,Object* ability) {
    if (!ue::Alive(ability)||!ue::Alive(context.asc)) return false;
    auto* ability_table=ue::At<unsigned char*>(ability,0),*asc_table=ue::At<unsigned char*>(context.asc,0);
    if (!ability_table||!asc_table) return false;
    return Readable(ability_table+0x330,8)&&Readable(asc_table+0x798,8)&&
        ue::At<uintptr_t>(ability_table,0x330)==ue::Base()+0x4c2b900&&
        ue::At<uintptr_t>(asc_table,0x798)==ue::Base()+0x125b360&&
        !std::memcmp(reinterpret_cast<const void*>(ue::Base()+0x125b360),"\xc2\x00\x00",3);
}
CancelResult ForceCancelOne(const Context& context,const CancelStamp& stamp) {
    Object* ability=stamp.ability.object; bool original=false,changed=false,restored=true,cancel_invoked=false;
    CancelResult result=CancelResult::Failed;
    if (!Same(stamp.ability)||!FullBool(ability,"bIsCancelable",original)) return result;
    if (ue::At<uint8_t>(ability,908)) return CancelResult::Wait;
    // Exact PBZ ASC's setter notification is a verified empty ret. A derived
    // replacement is not assumed safe: refuse before temporarily changing a
    // false flag if its notification can execute unknown gameplay callbacks.
    if (!original&&!QuietCancelSetter(context,ability)) { Status(L"当前技能取消标志回调未适配；未强制取消或弹跳"); JcGate("cancel_setter_gate_mismatch",L"JC 等待：当前不可取消技能的原生回调未适配",ability); return result; }
    __try {
        if (!original) {
            cancel_flags_.push_back({stamp,Identify(context.asc),original});
            changed=true; // Mark before engine code: the native notification can re-enter gameplay.
            Call allow(ability,"SetCanBeCanceled"); bool current=false;
            if (!allow.Set("bCanBeCanceled",true).Invoke()||!FullBool(ability,"bIsCancelable",current)||!current) __leave;
        }
        if (!Same(stamp.ability)||std::memcmp(stamp.activation,ability+212,20)) __leave;
        {
            int32_t handle=0;
            Call avatar(ability,"GetAvatarActorFromActorInfo"),asc(ability,"GetAbilitySystemComponentFromActorInfo");
            if (!SpecFor(context,ability,&handle)||handle!=stamp.handle||!avatar.Invoke()||!asc.Invoke()||
                avatar.Get<Object*>("ReturnValue")!=context.pawn||asc.Get<Object*>("ReturnValue")!=context.asc||!SafeStepState(context)) __leave;
        }
        if (ue::At<uint8_t>(ability,908)) { result=CancelResult::Wait; __leave; }
        cancel_invoked=true;
        for (FlagLease& lease:cancel_flags_) if (lease.stamp.ability.object==stamp.ability.object&&lease.stamp.ability.serial==stamp.ability.serial&&lease.stamp.handle==stamp.handle) lease.cancel_invoked=true;
        if (!InvokeCancelGuarded(ability)) __leave;
        result=CancelEnded(stamp.ability)?CancelResult::Done:CancelResult::Wait;
        if (result==CancelResult::Wait) JcGate("native_cancel_cleanup_pending",L"JC 等待：原生取消已调用，但技能仍活动或任务尚未清理",ability);
    }
    __finally {
        if (changed) {
            restored=RestoreCancelFlag(context,stamp,original,cancel_invoked);
            if (restored) cancel_flags_.erase(std::remove_if(cancel_flags_.begin(),cancel_flags_.end(),[&](const FlagLease& lease) {
                return lease.stamp.ability.object==stamp.ability.object&&lease.stamp.ability.serial==stamp.ability.serial&&lease.stamp.handle==stamp.handle;
            }),cancel_flags_.end());
        }
    }
    if (!restored) { JcGate("cancel_flag_restore_pending",L"JC 停止：技能重启或临时取消标志未能安全恢复；禁止旅行",ability); restore_pending_=true; return CancelResult::Failed; }
    return result;
}
CancelResult CancelTargets(const Context& context,const std::vector<Object*>& targets,std::vector<Identity>& cancelled) {
    CancelResult result=CancelResult::Done;
    for (Object* ability:targets) {
        bool active=false;
        if (!FullBool(ability,"bIsActive",active)) { result=CancelResult::Failed; break; }
        if (!active) continue; // Another skill's End delegate may already have ended it.
        CancelStamp stamp;
        if (!Stamp(context,ability,stamp)) { JcGate("activation_stamp_failed",L"JC 未触发：技能激活/任务身份无法核实",ability); result=CancelResult::Failed; break; }
        result=ForceCancelOne(context,stamp);
        if (result==CancelResult::Failed) break;
        if (std::none_of(cancelled.begin(),cancelled.end(),[&](const Identity& id) { return id.object==stamp.ability.object&&id.serial==stamp.ability.serial; })) cancelled.push_back(stamp.ability);
        if (result==CancelResult::Wait) break;
        AcquireSRWLockExclusive(&state_lock); ++state_.jump_cancels; ReleaseSRWLockExclusive(&state_lock);
        Logf("AIR_JUMP native skill cancel completed; original flag restored class=%s",Utf8(ObjectText(ue::ClassOf(ability))).c_str());
    }
    if (result==CancelResult::Done) for (const Identity& id:cancelled) if (!CancelEnded(id)) {
        JcGate("old_cleanup_pending",L"JC 等待：旧技能结束/任务清理尚未完成",id.object); return CancelResult::Wait;
    }
    // End callbacks may synchronously start a different owned skill. Do not
    // launch through it just because every member of the old snapshot ended.
    if (result==CancelResult::Done) for (Object* ability:Abilities(context)) if (PlayerSkill(ability)) {
        bool active=false,ending=false;
        if (!FullBool(ability,"bIsActive",active)||!FullBool(ability,"bIsAbilityEnding",ending)) return CancelResult::Failed;
        if (active||ending) { JcGate("new_active_skill",L"JC 等待：结束回调又启动了技能",ability); return CancelResult::Wait; }
    }
    return result;
}
CancelResult CancelTargetsGuarded(const Context& context,const std::vector<Object*>& targets,std::vector<Identity>& cancelled) {
    __try { return CancelTargets(context,targets,cancelled); }
    __except (FaultFilter(GetExceptionInformation())) {
        Logf("AIR_JUMP guarded cancel fault=%lu; releasing temporary skill holds",fault_code_); return CancelResult::Failed;
    }
}
CancelResult CancelSkillsForJump(const Context& context,std::vector<Identity>& cancelled) {
    if (!SafeStepState(context)) { Status(L"跳跃保留当前受击/倒地/剧情/特殊移动状态，或状态布局未就绪"); return CancelResult::Failed; }
    const auto abilities=Abilities(context); std::vector<Object*> targets;
    for (Object* ability:abilities) {
        bool active=false,ending=false,cancelable=false;
        if (!FullBool(ability,"bIsActive",active)) return CancelResult::Failed;
        if (!active||!SpecFor(context,ability)||!PlayerSkill(ability)) continue;
        if (!FullBool(ability,"bIsAbilityEnding",ending)||!FullBool(ability,"bIsCancelable",cancelable)) return CancelResult::Failed;
        if (ending||ue::At<uint8_t>(ability,908)) { JcGate(ending?"skill_ending":"skill_scope_locked",L"JC 等待：技能正在结束或原生作用域尚未解锁",ability); return CancelResult::Wait; }
        targets.push_back(ability);
    }
    Object* animating=nullptr;
    if (!AnimatingAbility(context,animating)) { Status(L"跳跃动画能力布局未通过校验；未取消动作"); JcGate("animating_layout_mismatch",L"JC 未触发：动画能力布局未通过校验"); return CancelResult::Failed; }
    if (animating&&std::find(targets.begin(),targets.end(),animating)==targets.end()) {
        // Ordinary Jump can keep its own animation. Protect any unrelated
        // non-skill montage instead of stopping an animation in isolation.
        bool owned=false;
        if (ue::NameIs(ue::NameOf(ue::ClassOf(animating)),"GA_Hun_Jump_C")) {
            Call avatar(animating,"GetAvatarActorFromActorInfo"),asc(animating,"GetAbilitySystemComponentFromActorInfo");
            owned=avatar.Invoke()&&asc.Invoke()&&avatar.Get<Object*>("ReturnValue")==context.pawn&&asc.Get<Object*>("ReturnValue")==context.asc;
        }
        if (!owned) { Status(L"跳跃保留当前非技能系统动画；未取消或发放资源"); JcGate("protected_animating_ability",L"JC 未触发：保留当前非技能系统动画",animating); return CancelResult::Failed; }
    }
    const size_t prior=strong_.size();
    for (Object* ability:targets) if (!Keep(ability)) { if (!ReleaseStrongTail(prior)) restore_pending_=true; return CancelResult::Failed; }
    CancelResult result=CancelTargetsGuarded(context,targets,cancelled);
    // This boundary catches faults inside native cleanup before releasing the
    // temporary (same-call only) world-owned GAS references.
    if (!ReleaseStrongTail(prior)) { restore_pending_=true; result=CancelResult::Failed; }
    return result;
}
bool GameplayInputAllowed(const Context& context) {
    if (!input_focus_||!input_focus_()) return false;
    Call paused(ue::Default("/Script/Engine.Default__GameplayStatics"),"IsGamePaused");
    return paused.Set("WorldContextObject",context.pawn).Invoke()&&!paused.Get<bool>("ReturnValue");
}
void FinishStep(const Context& context) {
    if (!step_.requested) return;
    if (!Same(step_.pawn) || step_.pawn.object!=context.pawn) { step_={}; return; }
    Call dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");
    if (!dead.Set("Pawn",context.pawn).Invoke() || dead.Get<bool>("ReturnValue")) { JcGate("post_launch_player_dead",L"JC 未奖励：弹跳后主角死亡或生命状态未就绪"); step_={}; return; }
    if (!SafeStepState(context)) { JcGate("post_launch_protected_state",L"JC 未奖励：弹跳后进入受击/剧情/特殊移动状态"); step_={}; return; }
    for (const Identity& cancelled:step_.cancelled) if (!CancelEnded(cancelled)) {
        Status(L"JC旧攻击结束清理未完成；未发放杀气或技能充能"); step_={}; return;
    }
    Call paused(ue::Default("/Script/Engine.Default__GameplayStatics"),"IsGamePaused");
    if (!paused.Set("WorldContextObject",context.pawn).Invoke()) return;
    if (paused.Get<bool>("ReturnValue")) { step_.requested=GetTickCount64(); return; }
    Vector position; Call velocity(context.pawn,"GetVelocity"), falling(context.movement,"IsFalling");
    const bool rising=Position(context.pawn,position) && velocity.Invoke() && falling.Invoke() && falling.Get<bool>("ReturnValue") &&
        std::isfinite(velocity.Get<Vector>("ReturnValue").z) && velocity.Get<Vector>("ReturnValue").z>50 && position.z>step_.start.z+1;
    if (!rising) {
        if (GetTickCount64()-step_.requested>500) {
            Status(L"JC原生弹跳请求后未确认上升；未发放杀气或技能次数");
            Logf("AIR_STOMP rejected: no real post-launch ascent; no resource refund"); step_={};
            JcGate("no_actual_ascent",L"JC 未奖励：原生弹跳排队后未确认实际上升");
        }
        return;
    }
    // LaunchCharacter only queues movement. Grant resources after a later
    // frame proves real upward movement, not merely a successful dispatch.
    if (!GameplayInputAllowed(context)) { step_.requested=GetTickCount64(); return; }
    const PendingStep completed=step_; step_={};
    const RewardResult rewards=ApplyRewards(context);
    const float full=rewards.shachi_max; const bool refill=rewards.shachi;
    const ResourceReward& pure=rewards.pure; const ResourceReward& shadow=rewards.shadow; const Restored& restored=rewards.skills;
    AcquireSRWLockExclusive(&state_lock);
    ++state_.bounces; state_.cooldown_restores+=restored.cooldowns; state_.restored_charges+=restored.charges;
    state_.failed_restores+=restored.failed;
    if (options_.reward_shachi) { state_.last_shachi_max=full; state_.last_shachi=refill?full:0; }
    if (options_.reward_pure_shachi) state_.pure_shachi=pure;
    if (options_.reward_spirit_shadow) state_.spirit_shadow=shadow;
    if (options_.reward_weapon_switch) {
        state_.last_weapon_switch_verified=rewards.weapon.verified; state_.last_weapon_switch_slots=rewards.weapon.slots;
        if (rewards.weapon.verified) ++state_.weapon_switch_resets; else ++state_.weapon_switch_failures;
    }
    if (options_.reward_guard_dodge) {
        state_.guard_dodge_known=rewards.guard.known; state_.last_guard_dodge_verified=rewards.guard.verified;
        state_.guard_dodge_effects_before=rewards.guard.before; state_.guard_dodge_effects_after=rewards.guard.after;
        if (rewards.guard.verified) ++state_.guard_dodge_resets; else ++state_.guard_dodge_failures;
    }
    ReleaseSRWLockExclusive(&state_lock);
    ArmSlowFall(context);
    JcGate("ascent_verified",L"JC 已确认实际上升；仅发放已勾选奖励，结果见读回");
    Status(L"JC已确认；杀气回满%s，精纯杀气 %.0f→%.0f/%.0f%s，意识之影 %.0f→%.0f/%.0f%s；CD %u/充能 %u/未通过 %u",
        !options_.reward_shachi?L"已关闭":refill?L"成功":L"未完成",pure.before,pure.after,pure.maximum,!options_.reward_pure_shachi?L"（关闭）":pure.verified?L"":L"（未通过）",
        shadow.before,shadow.after,shadow.maximum,!options_.reward_spirit_shadow?L"（关闭）":shadow.verified?L"":L"（未通过）",restored.cooldowns,restored.charges,restored.failed);
    Logf("AIR_STOMP verified player=%p enemy=%p ascent=%.3f zspeed=%.3f requested_z=%.1f shachi_refilled=%d max=%.3f cooldown_verified=%u charges_verified=%u failed=%u",static_cast<void*>(context.pawn),static_cast<void*>(completed.enemy.object),position.z-completed.start.z,velocity.Get<Vector>("ReturnValue").z,completed.speed,refill?1:0,full,restored.cooldowns,restored.charges,restored.failed);
    Logf("AIR_JC_RESOURCES pure_known=%d verified=%d before=%.3f after=%.3f max=%.3f added=%.3f shadow_known=%d verified=%d before=%.3f after=%.3f max=%.3f added=%.3f max_attributes_never_targeted=1",
        pure.known?1:0,pure.verified?1:0,pure.before,pure.after,pure.maximum,pure.after-pure.before,
        shadow.known?1:0,shadow.verified?1:0,shadow.before,shadow.after,shadow.maximum,shadow.after-shadow.before);
    Logf("AIR_JC_REWARDS shachi_enabled=%d pure_enabled=%d shadow_enabled=%d skills_cd_charges_enabled=%d weapon_switch_enabled=%d weapon_verified=%d slots=%u before_max_cd=%.4f after_max_cd=%.4f",
        options_.reward_shachi?1:0,options_.reward_pure_shachi?1:0,options_.reward_spirit_shadow?1:0,options_.reward_skills?1:0,options_.reward_weapon_switch?1:0,
        rewards.weapon.verified?1:0,rewards.weapon.slots,rewards.weapon.before,rewards.weapon.after);
}
void TryJump(const Context& context) {
    if (!(options_.air_skills||options_.stomp_jump)||!Same(jump_identity_)||jump_identity_.object!=jump_action_||!(ue::NameOf(jump_action_)==jump_name_)) {
        JcGate("jump_action_missing",L"JC 等待：未识别到当前真实跳跃输入动作"); last_jump_=false; jump_baselined_=false; return;
    }
    Call holding(context.sinput, "IsHolding");
    if (!holding.Set("InputAction",jump_action_).Invoke()) { JcGate("jump_input_query_failed",L"JC 等待：无法读取游戏当前跳跃输入"); return; }
    const bool down = holding.Get<bool>("ReturnValue");
    if (!GameplayInputAllowed(context)) {
        // Adaptation/restoration may continue while menus are open; input and
        // refunds may not. Synchronize the actual game input without creating
        // an edge, and suspend a queued step's wall-clock confirmation limit.
        last_jump_=down; jump_baselined_=true;
        if (step_.requested) step_.requested=GetTickCount64();
        if (jump_request_.requested) jump_request_.requested=GetTickCount64();
        if (down||jump_request_.requested) JcGate("gameplay_unfocused_or_paused",L"JC 等待：游戏暂停或焦点在菜单/其他程序");
        return;
    }
    if (!jump_baselined_) { last_jump_ = down; jump_baselined_ = true; return; }
    const bool edge = down && !last_jump_; last_jump_ = down;
    if (step_.requested) FinishStep(context);
    if (edge) {
        AcquireSRWLockExclusive(&state_lock); ++state_.jump_edges; ReleaseSRWLockExclusive(&state_lock);
        if (!options_.stomp_jump) JcGate("jc_switch_off",L"近敌 JC 当前未开启；需勾选第二项并点击应用（空中技能开关独立）");
        else if (!NearbyEnemy(context)) NoNearEnemy();
        // A new physical press owns a bounded request. GAS scope-lock queues
        // are not used: a temporarily forced cancelability flag must not be
        // restored before a deferred native cancellation runs.
        if (options_.air_skills||(options_.stomp_jump&&NearbyEnemy(context))) {
            // An accepted native Jump owns this press until ascent/timeout;
            // do not overwrite its receipt with an unrelated button edge.
            if (!jump_request_.activated) jump_request_={Identify(context.pawn),GetTickCount64(),{}};
        }
    }
    if (!jump_request_.requested) return;
    if (!Same(jump_request_.pawn)||jump_request_.pawn.object!=context.pawn||GetTickCount64()-jump_request_.requested>500) {
        JcGate("jump_request_timeout_or_owner_changed",L"JC 请求超过 500ms 或主角发生变化；未弹跳或发资源");
        jump_request_={}; return;
    }
    Call dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");
    if (!dead.Set("Pawn",context.pawn).Invoke()||dead.Get<bool>("ReturnValue")) { JcGate("player_dead_or_unknown",L"JC 未触发：主角死亡或生命状态未就绪"); jump_request_={}; return; }
    Name protected_tag;
    if (!SafeStepState(context,&protected_tag)) { JcGate("protected_actor_state",L"JC 未触发：保留受击/倒地/剧情/特殊移动状态；",nullptr,protected_tag); jump_request_={}; return; }
    if (jump_request_.activated) {
        Vector position; Call velocity(context.pawn,"GetVelocity"),falling(context.movement,"IsFalling");
        if (Position(context.pawn,position)&&velocity.Invoke()&&falling.Invoke()&&falling.Get<bool>("ReturnValue")&&
            std::isfinite(velocity.Get<Vector>("ReturnValue").z)&&velocity.Get<Vector>("ReturnValue").z>50&&position.z>jump_request_.start.z+1) {
            Logf("AIR_JUMP original Jump actual ascent verified dz=%.3f zspeed=%.3f cancelled_skills=%u; no JC reward",position.z-jump_request_.start.z,velocity.Get<Vector>("ReturnValue").z,static_cast<unsigned>(jump_request_.cancelled.size()));
            JcGate("native_jump_ascent_verified",L"跳跃打断已完成：原生跳跃实际上升已确认；未发放 JC 奖励");
            jump_request_={};
        }
        return;
    }
    Object* enemy=options_.stomp_jump?NearbyEnemy(context):nullptr;
    if (!options_.air_skills&&!enemy) { NoNearEnemy(); jump_request_={}; return; }
    const uint64_t before_cancel_gate=gate_sequence_;
    const CancelResult result=CancelSkillsForJump(context,jump_request_.cancelled);
    if (result==CancelResult::Wait) return;
    if (result==CancelResult::Failed) { if (gate_sequence_==before_cancel_gate) JcGate("skill_cancel_rejected",L"JC 未触发：当前技能取消/身份检查未通过"); jump_request_={}; return; }
    if (!GameplayInputAllowed(context)||!SafeStepState(context)) { jump_request_={}; return; }
    // End delegates can teleport either actor or change collision/occlusion.
    // Only the post-cleanup live proximity may authorize the extra launch.
    enemy=options_.stomp_jump?NearbyEnemy(context):nullptr;
    if (!enemy) {
        if (options_.stomp_jump) NoNearEnemy();
        // A jump-cancel must actually enter the game's Jump ability. Previously
        // we cancelled an airborne skill then returned here because movement
        // was not Ground, leaving only idle. A proven skill cancellation now
        // authorizes that one native jump in air too; an empty-air press that
        // cancelled nothing still cannot create an unlimited extra jump.
        const bool cancelled_skill=!jump_request_.cancelled.empty();
        if (!options_.air_skills||!jump_priority_ready_) { jump_request_={}; return; }
        if (!NormalJumpAllowed(context,!cancelled_skill)) {
            if (cancelled_skill) JcGate("native_jump_waiting_state_cleanup",L"跳跃打断等待原生技能清理跳跃限制；保留本次跳跃请求");
            else jump_request_={};
            return;
        }
        if (GetTickCount64()>=jump_request_.next_attempt) {
            bool active_jump=false;
            for (Object* ability:Abilities(context)) if (ue::NameIs(ue::NameOf(ue::ClassOf(ability)),"GA_Hun_Jump_C")) {
                bool active=false; if (FullBool(ability,"bIsActive",active)&&active&&SpecFor(context,ability)) active_jump=true;
            }
            Vector start; if (!Position(context.pawn,start)) { jump_request_={}; return; }
            bool accepted=active_jump;
            if (!active_jump) {
                Call jump(context.input,"TryActivateExternalActionAbilityByTag");
                accepted=jump.Set("AbilityTag",jump_tag_).Set("InputAction",jump_action_).Set("ActionPriority",jump_priority_).Invoke()&&jump.Get<bool>("ReturnValue");
                Logf("AIR_JUMP original Jump activation accepted=%d priority=%d cancelled_skills=%u; no resource refund",accepted?1:0,jump_priority_,static_cast<unsigned>(jump_request_.cancelled.size()));
            }
            jump_request_.next_attempt=GetTickCount64()+25;
            if (accepted) {
                // This is a new ordinary/cancel jump, not the earlier JC's
                // launch. Its ascent cannot prove an older pending JC reward,
                // and it must not inherit that JC's owned fall adjustment.
                step_={};
                if (!RestoreSlowFall()) restore_pending_=true;
                jump_request_.activated=true; jump_request_.start=start;
                JcGate("native_jump_activated",L"已进入原生跳跃；等待实际位移确认，未发放 JC 奖励");
            } else {
                JcGate("native_jump_activation_pending",L"跳跃技能尚未接受；保留本次跳跃请求并等待原生清理");
            }
        }
        return;
    }
    Vector position; const Identity player=Identify(context.pawn),target=Identify(enemy);
    const int32_t pending=Offset(context.movement,"PendingLaunchVelocity");
    if (!Same(player)||!Same(target)||pending!=1496||!Enemy(enemy,context)||!Position(context.pawn,position)) { JcGate("launch_identity_or_layout_failed",L"JC 未触发：弹跳前身份/位置/速度字段未通过校验"); jump_request_={}; return; }
    // A repeated nearby press replaces our previous fall adjustment; it never
    // multiplies an existing 0.75 value or reduces the ascending trajectory.
    if (!RestoreSlowFall()) { restore_pending_=true; jump_request_={}; return; }
    const Vector launch{0, 0, options_.bounce_speed};
    if (!Call(context.pawn,"LaunchCharacter").Set("LaunchVelocity",launch).Set("bXYOverride",false).Set("bZOverride",true).Invoke()) { JcGate("native_launch_failed",L"JC 未触发：原生弹跳调用未接受"); jump_request_={}; return; }
    const Vector queued=ue::At<Vector>(context.movement,pending);
    if (!std::isfinite(queued.z) || std::fabs(queued.z-options_.bounce_speed)>0.1) {
        Status(L"JC弹跳排队未通过读回；未发放杀气或次数"); jump_request_={}; return;
    }
    last_bounce_ = GetTickCount64();
    step_={player,target,position,last_bounce_,options_.bounce_speed,std::move(jump_request_.cancelled)};
    jump_request_={};
    JcGate("launch_queued",L"JC 已请求弹跳，等待下一帧确认实际上升后发放资源");
    Logf("AIR_STOMP requested player=%p enemy=%p native_pending_z=%.3f; awaiting actual ascent",static_cast<void*>(context.pawn),static_cast<void*>(enemy),queued.z);
}
void Publish(bool ready, uint32_t abilities) {
    uint32_t changes = static_cast<uint32_t>(removed_.size());
    for (const Clone& clone : clones_) if (Same(clone.copy)) changes += clone.changes;
    AcquireSRWLockExclusive(&state_lock);
    state_.ready = ready; state_.active = ready && (options_.air_skills || options_.stomp_jump); state_.options = options_;
    state_.cloned_bindings = static_cast<uint32_t>(clones_.size()); state_.relaxed_tags = changes; state_.owned_abilities = abilities;
    state_.jump_input_ready=Same(jump_identity_)&&input_focus_!=nullptr;
    state_.jump_priority_ready=jump_priority_ready_; state_.jump_priority=jump_priority_;
    state_.slow_fall_active=slow_.owned; state_.gravity_control_yielded=slow_.yielded;
    state_.base_gravity=slow_.original; state_.current_gravity=slow_.owned?slow_.last:slow_.original; state_.down_speed_cap=slow_.cap;
    state_.strong_references = static_cast<uint32_t>(strong_.size()); state_.restore_pending = restore_pending_;
    state_.stomp_targets = static_cast<uint32_t>(enemies_.size());
    state_.scanned_pawns=enemy_scan_.pawns; state_.enemy_ai_rejected=enemy_scan_.no_ai;
    state_.enemy_team_rejected=enemy_scan_.team; state_.enemy_health_rejected=enemy_scan_.health;
    state_.nearest_enemy_surface=near_scan_.nearest;
    ReleaseSRWLockExclusive(&state_lock);
}
// "air tags": one-shot dump of the player's ASC tags and actor states (diagnostics).
volatile LONG tag_dump_=0;
void DumpTagList(const char* label,const Tags& tags) {
    std::string line;
    for (int32_t i=0;i<tags.tags.num&&ArrayOk(tags.tags,sizeof(Name));++i) {
        char text[160]{}; ue::NameText(static_cast<Name*>(tags.tags.data)[i],text,sizeof(text));
        if (!line.empty()) line+=' ';
        line+=text;
        if (line.size()>800) { Logf("AIR_TAGS %s %s",label,line.c_str()); line.clear(); }
    }
    Logf("AIR_TAGS %s %s (count=%d)",label,line.c_str(),tags.tags.num);
}
std::string TagListText(const Tags& tags) {
    std::string text;
    for (int32_t i=0;i<tags.tags.num&&ArrayOk(tags.tags,sizeof(Name));++i) { char name[128]{}; ue::NameText(static_cast<Name*>(tags.tags.data)[i],name,sizeof(name)); if (!text.empty()) text+=','; text+=name; }
    return text.empty()?std::string("-"):text;
}
// Dash / dodge routing: every trigger of AIM_Hun_Dash with its required and
// ignored tags, and the player's owned dash / dodge abilities with their tags.
void DumpDash(const Context& c) {
    VisitBindings(c.input,[&](Object*& binding,Object*) {
        char name[160]{}; ue::NameText(ue::NameOf(binding),name,sizeof(name));
        static Object* last=nullptr; if (binding==last||!std::strstr(name,"Dash")) return; last=binding;
        WalkStruct(binding,ue::ClassOf(binding),[&](Tags* tags,Gate gate) {
            if (gate!=Gate::Required) return;
            const auto* trigger=reinterpret_cast<const unsigned char*>(tags)-16;
            if (!Readable(trigger,96)) return;
            char tag[128]{}; ue::NameText(*reinterpret_cast<const Name*>(trigger+80),tag,sizeof(tag));
            const Tags* ignored=reinterpret_cast<const Tags*>(trigger+48);
            Logf("AIR_TAGS route %s trigger=%s required=%s ignored=%s",name,tag,TagListText(*tags).c_str(),TagListText(*ignored).c_str());
        });
    });
    Object* spec=Type("/Script/GameplayAbilities.GameplayAbilitySpec"),*container=Type("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    if (Offset(c.asc,"ActivatableAbilities")!=1048||!container||ue::Offset(container,"Items")!=264||ue::StructSize(spec)!=240) return;
    const Array array=ue::At<Array>(c.asc,1048+264); if (!ArrayOk(array,240,4096)) return;
    for (int32_t i=0;i<array.num;++i) {
        Object* cdo=ue::At<Object*>(static_cast<unsigned char*>(array.data)+static_cast<size_t>(i)*240,16);
        if (!ue::Alive(cdo)) continue;
        char name[160]{}; ue::NameText(ue::NameOf(ue::ClassOf(cdo)),name,sizeof(name));
        char lower[160]{}; for (int k=0;name[k]&&k<159;++k) lower[k]=static_cast<char>(std::tolower(static_cast<unsigned char>(name[k])));
        if (!std::strstr(lower,"dash")&&!std::strstr(lower,"dodge")&&!std::strstr(lower,"air")) continue;
        const int32_t offset=Offset(cdo,"AbilityTags"); if (offset<0) continue;
        const int32_t blocked=Offset(cdo,"ActivationBlockedTags"),required=Offset(cdo,"ActivationRequiredTags");
        Logf("AIR_TAGS ability %s tags=%s required=%s blocked=%s",name,TagListText(ue::At<Tags>(cdo,offset)).c_str(),
            required>=0?TagListText(ue::At<Tags>(cdo,required)).c_str():"?",blocked>=0?TagListText(ue::At<Tags>(cdo,blocked)).c_str():"?");
    }
}
void DumpTags(const Context& c) {
    DumpDash(c);
    Call owned(c.asc,"GetOwnedGameplayTags","/Script/GameplayTags.GameplayTagAssetInterface");
    if (owned.Slot("TagContainer",32)&&owned.Invoke()) {
        const Tags tags=owned.Get<Tags>("TagContainer"); DumpTagList("asc",tags); ue::Free(tags.tags.data); ue::Free(tags.parents.data);
    } else Logf("AIR_TAGS asc unavailable");
    Call states(Component(c.pawn,"/Script/SGameActorState.ActorStateComponent"),"GetCurrentStatesContainer");
    if (states.Slot("CurrentStates",32)&&states.Invoke()) {
        const Tags tags=states.Get<Tags>("CurrentStates"); DumpTagList("states",tags); ue::Free(tags.tags.data); ue::Free(tags.parents.data);
    } else Logf("AIR_TAGS states unavailable");
    for (const wchar_t* name:{L"CharacterState.Talent.EnableAirDash",L"CharacterState.DisableAirDash",L"CharacterState.Space.Ground",L"CharacterState.Space.Air"}) {
        Call count(c.asc,"GetGameplayTagCount"); char text[96]{}; WideCharToMultiByte(CP_UTF8,0,name,-1,text,sizeof(text),nullptr,nullptr);
        if (count.Set("GameplayTag",ue::MakeName(name)).Invoke()) Logf("AIR_TAGS count %s=%d",text,count.Get<int32_t>("ReturnValue"));
        else Logf("AIR_TAGS count %s unavailable",text);
    }
}
void Tick() {
    if (!initialized_) return;
    bool changed = false; Options requested;
    AcquireSRWLockExclusive(&request_lock);
    if (has_request_) { requested = pending_; has_request_ = false; changed = true; }
    ReleaseSRWLockExclusive(&request_lock);
    if (changed) {
        const bool restored = Restore(); options_ = requested; blocked_world_ = {};
        if (!restored) {
            options_ = {}; Publish(false, 0);
            Logf("AIR_SKILLS prior player restoration incomplete; retained live ownership records for retry; requested changes not applied");
            Status(L"旧主角的空间限制未完整恢复；已停止新操作并保留恢复记录，可关闭功能重试或重新载入场景"); return;
        }
        Status(options_.air_skills || options_.stomp_jump ? L"等待主角；空中释放/跳跃打断与近敌 JC 独立控制，保留死亡及剧情状态" : L"已关闭空中技能和近敌 JC；输入、技能限制、临时取消标志与下降重力已恢复");
        Logf("AIR_OPTIONS applied air=%d jc=%d slow=%d radius_cm=%.1f bounce_z=%.1f",options_.air_skills?1:0,options_.stomp_jump?1:0,options_.slow_fall?1:0,options_.enemy_radius,options_.bounce_speed);
        Logf("AIR_REWARD_OPTIONS shachi=%d pure=%d shadow=%d skills_cd_charges=%d weapon_switch=%d",options_.reward_shachi?1:0,options_.reward_pure_shachi?1:0,options_.reward_spirit_shadow?1:0,options_.reward_skills?1:0,options_.reward_weapon_switch?1:0);
        JcGate(options_.stomp_jump?"enabled_waiting_jump":"jc_switch_off",options_.stomp_jump?L"近敌 JC 已开启：等待在游戏中重新按下跳跃":L"近敌 JC 已关闭；空中技能独立，需勾选第二项并点击应用");
    }
    if (restore_pending_) {
        const bool restored=Restore(); options_={}; Publish(false,0);
        Status(restored?L"临时修改已安全恢复；功能已关闭，可重新开启":L"仍有临时状态待安全恢复；已停止新输入/取消/弹跳，禁止旅行"); return;
    }
    if (InterlockedExchange(&tag_dump_,0)) { Context probe; if (ContextNow(probe)) DumpTags(probe); else Logf("AIR_TAGS player context not ready"); }
    if (InterlockedCompareExchange(&disabled_,0,0) || (!options_.air_skills && !options_.stomp_jump)) { Publish(false,0); return; }
    Context context;
    if (!ContextNow(context)) { Publish(false, 0); return; }
    if (Same(blocked_world_)) { if (blocked_world_.object == context.world) return; blocked_world_ = {}; }
    if (owner_.object && (!Same(owner_) || owner_.object != context.pawn || !Same(input_) || input_.object != context.input)) {
        if (!Restore()) { options_ = {}; Publish(false, 0); Status(L"主角变化时恢复旧限制未完成，已停止空中功能并保留恢复记录"); return; }
    }
    if (!owner_.object) { owner_ = Identify(context.pawn); input_ = Identify(context.input); }
    if (options_.air_skills&&!EnsureUnregisterHook()) {
        options_.air_skills=false; const bool restored=Restore(); Publish(false,owned_count_);
        Status(restored?L"原生输入卸载保护未能安全安装；已拒绝空中技能并恢复原状态":L"输入卸载保护安装/恢复未完成；空中功能停止，保留恢复记录"); return;
    }
    const ULONGLONG now = GetTickCount64();
    uint64_t signature=0;
    const bool signature_ok=BindingSignature(context.input,signature),refresh=now>=next_refresh_;
    const bool mappings_changed=!signature_ok||signature!=binding_signature_;
    if (mappings_changed||refresh) {
        binding_ready_=signature_ok&&CloneBindings(context);
        if (!BindingSignature(context.input,binding_signature_)) { binding_signature_=0; binding_ready_=false; }
        if (!binding_ready_) {
            const bool restored=Restore(); options_={}; Publish(false,0);
            Status(restored?L"输入复制/优先级或缓存未完整读回；已关闭并恢复":L"输入布局不匹配且尚有恢复记录；停止功能并禁止旅行"); return;
        }
        if (changed) Status(L"空中技能已适配当前输入；跳跃优先级使用私有副本，近敌 JC 等待真实按键和实际弹起");
    }
    if (refresh||mappings_changed) {
        next_refresh_=now+500;
        const auto abilities=Abilities(context,true); if (binding_ready_) RelaxAbilities(abilities); RefreshSkillStates(abilities); owned_count_=static_cast<uint32_t>(abilities.size());
    }
    if (options_.stomp_jump && now >= next_enemies_) { next_enemies_ = now + 500; RefreshEnemies(context); }
    UpdateSlowFall(context);
    if (binding_ready_) TryJump(context);
    Publish(binding_ready_,owned_count_);
}
int FaultFilter(EXCEPTION_POINTERS* exception) { fault_code_ = exception->ExceptionRecord->ExceptionCode; return EXCEPTION_EXECUTE_HANDLER; }
bool Recover() {
    __try { return Restore(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        // Keep all native strong references and mutation records on uncertainty.
        // Discarding them would both leak ownership and make safe travel lie.
        restore_pending_ = true; return false;
    }
}
void GuardedTick() {
    __try { Tick(); }
    __except (FaultFilter(GetExceptionInformation())) {
        ++faults_; Recover(); options_ = {};
        if (faults_ >= 3) InterlockedExchange(&disabled_, 1);
        Publish(false, 0); Status(L"空中技能模块错误 0x%08lx，已停止并尝试恢复", fault_code_);
        Logf("AIR_SKILLS fault=%lu count=%u disabled=%d", fault_code_, faults_, InterlockedCompareExchange(&disabled_, 0, 0) ? 1 : 0);
    }
}
bool Travel() {
    Context context; const bool has_context = ContextNow(context);
    const bool restored = Restore();
    if (restored && has_context) blocked_world_ = Identify(context.world);
    if (!restored) options_ = {};
    Publish(false, 0);
    Status(restored ? L"切图前已恢复旧主角限制并释放强引用；等待新场景后重新应用所选空中功能" : L"切图前恢复未完成；已拒绝旅行并保留原对象/修改记录，关闭功能后可重试");
    return restored;
}
}

// PBZDebugMenu.ini [AirSkills]: every toggle and value, restored at startup.
void SaveOptions(const Options& o) {
    const wchar_t* section=L"AirSkills";
    SettingsStore::PutBool(section,L"AirSkills",o.air_skills); SettingsStore::PutBool(section,L"NearEnemyJC",o.stomp_jump);
    SettingsStore::PutBool(section,L"SlowFall",o.slow_fall); SettingsStore::PutBool(section,L"RewardShachi",o.reward_shachi);
    SettingsStore::PutBool(section,L"RewardPureShachi",o.reward_pure_shachi); SettingsStore::PutBool(section,L"RewardSpiritShadow",o.reward_spirit_shadow);
    SettingsStore::PutBool(section,L"RewardSkills",o.reward_skills); SettingsStore::PutBool(section,L"RewardWeaponSwitch",o.reward_weapon_switch);
    SettingsStore::PutBool(section,L"RewardGuardDodge",o.reward_guard_dodge);
    SettingsStore::PutFloat(section,L"BounceSpeed",o.bounce_speed); SettingsStore::PutFloat(section,L"EnemyRadius",o.enemy_radius);
}
void Initialize(Logger logger,InputFocus can_gameplay_input) {
    logger_=logger; input_focus_=can_gameplay_input; initialized_=true;
    Logf("AIR_SKILLS initialized off; independent toggles; gameplay_focus_callback=%d",input_focus_?1:0);
    const wchar_t* section=L"AirSkills";
    if (SettingsStore::Get(section,L"AirSkills",L"").empty()) return;
    Options o;
    o.air_skills=SettingsStore::GetBool(section,L"AirSkills",o.air_skills); o.stomp_jump=SettingsStore::GetBool(section,L"NearEnemyJC",o.stomp_jump);
    o.slow_fall=SettingsStore::GetBool(section,L"SlowFall",o.slow_fall); o.reward_shachi=SettingsStore::GetBool(section,L"RewardShachi",o.reward_shachi);
    o.reward_pure_shachi=SettingsStore::GetBool(section,L"RewardPureShachi",o.reward_pure_shachi); o.reward_spirit_shadow=SettingsStore::GetBool(section,L"RewardSpiritShadow",o.reward_spirit_shadow);
    o.reward_skills=SettingsStore::GetBool(section,L"RewardSkills",o.reward_skills); o.reward_weapon_switch=SettingsStore::GetBool(section,L"RewardWeaponSwitch",o.reward_weapon_switch);
    o.reward_guard_dodge=SettingsStore::GetBool(section,L"RewardGuardDodge",o.reward_guard_dodge);
    o.bounce_speed=SettingsStore::GetFloat(section,L"BounceSpeed",o.bounce_speed); o.enemy_radius=SettingsStore::GetFloat(section,L"EnemyRadius",o.enemy_radius);
    if (RequestOptions(o)) {
        AcquireSRWLockExclusive(&state_lock); state_.options=o; ReleaseSRWLockExclusive(&state_lock);
        Logf("AIR_SKILLS restored saved options air=%d jc=%d slow=%d",o.air_skills?1:0,o.stomp_jump?1:0,o.slow_fall?1:0);
    }
}
bool RequestOptions(const Options& options) {
    if ((InterlockedCompareExchange(&disabled_,0,0) && (options.air_skills||options.stomp_jump)) || !std::isfinite(options.bounce_speed) || options.bounce_speed < 100 || options.bounce_speed > 3000 ||
        !std::isfinite(options.enemy_radius) || options.enemy_radius < 10 || options.enemy_radius > 1500) return false;
    AcquireSRWLockExclusive(&request_lock); pending_ = options; has_request_ = true; ReleaseSRWLockExclusive(&request_lock);
    SaveOptions(options); return true;
}
Snapshot GetSnapshot() { AcquireSRWLockShared(&state_lock); Snapshot result = state_; ReleaseSRWLockShared(&state_lock); return result; }
void OnGameThreadGuarded() {
    const DWORD current=GetCurrentThreadId();
    if (!game_thread_) game_thread_=current;
    if (current==game_thread_) GuardedTick();
}
bool BeforeTravel() {
    __try { return Travel(); }
    __except (FaultFilter(GetExceptionInformation())) {
        Recover(); options_ = {}; Publish(false,0);
        Logf("AIR_SKILLS pretravel restore fault=%lu; travel refused",fault_code_); return false;
    }
}
bool HandleCommand(const char* verb, const char* arguments) {
    if (std::strcmp(verb, "air")) return false;
    std::istringstream input(arguments ? arguments : ""); std::string action; input >> action;
    if (action == "tags") { InterlockedExchange(&tag_dump_,1); return true; }
    if (action == "status") {
        const Snapshot value=GetSnapshot();
        Logf("AIR_STATUS %s air=%d jc=%d slow=%d bindings=%u tags=%u abilities=%u jump=%d priority=%d priority_ready=%d cancels=%u bounces=%u charges=%u gravity=%.3f/%.3f cap=%.1f gravity_owned=%d yielded=%d strong=%u restore_pending=%d",Utf8(value.status).c_str(),value.options.air_skills?1:0,value.options.stomp_jump?1:0,value.options.slow_fall?1:0,value.cloned_bindings,value.relaxed_tags,value.owned_abilities,value.jump_input_ready?1:0,value.jump_priority,value.jump_priority_ready?1:0,value.jump_cancels,value.bounces,value.restored_charges,value.base_gravity,value.current_gravity,value.down_speed_cap,value.slow_fall_active?1:0,value.gravity_control_yielded?1:0,value.strong_references,value.restore_pending?1:0);
        Logf("AIR_JC_STATUS %s edges=%u enemy_candidates=%u/%u ai_rejected=%u team_rejected=%u health_rejected=%u nearest_cm=%.1f radius_cm=%.1f",Utf8(value.jc_status).c_str(),value.jump_edges,value.stomp_targets,value.scanned_pawns,value.enemy_ai_rejected,value.enemy_team_rejected,value.enemy_health_rejected,value.nearest_enemy_surface,value.options.enemy_radius);
        Logf("AIR_JC_RESOURCE_STATUS pure_known=%d verified=%d before=%.3f after=%.3f max=%.3f shadow_known=%d verified=%d before=%.3f after=%.3f max=%.3f",
            value.pure_shachi.known?1:0,value.pure_shachi.verified?1:0,value.pure_shachi.before,value.pure_shachi.after,value.pure_shachi.maximum,
            value.spirit_shadow.known?1:0,value.spirit_shadow.verified?1:0,value.spirit_shadow.before,value.spirit_shadow.after,value.spirit_shadow.maximum);
        return true;
    }
    Options options = GetSnapshot().options;
    if (action == "off") options.air_skills = options.stomp_jump = false;
    else if (action == "on") options.air_skills = true;
    else if (action == "stomp") { int enabled = -1; input >> enabled; if (enabled != 0 && enabled != 1) return false; options.stomp_jump = enabled != 0; }
    else if (action == "slow") { int enabled=-1; input>>enabled; if (enabled!=0&&enabled!=1) return false; options.slow_fall=enabled!=0; }
    else if (action == "radius") { if (!(input>>options.enemy_radius)) return false; }
    else if (action == "reward") {
        std::string name; int enabled=-1; if (!(input>>name>>enabled)||(enabled!=0&&enabled!=1)) return false;
        const bool value=enabled!=0;
        if (name=="all") options.reward_shachi=options.reward_pure_shachi=options.reward_spirit_shadow=options.reward_skills=options.reward_weapon_switch=options.reward_guard_dodge=value;
        else if (name=="shachi") options.reward_shachi=value;
        else if (name=="pure") options.reward_pure_shachi=value;
        else if (name=="shadow") options.reward_spirit_shadow=value;
        else if (name=="skills") options.reward_skills=value;
        else if (name=="weapon") options.reward_weapon_switch=value;
        else if (name=="guard") options.reward_guard_dodge=value;
        else return false;
    }
    else return false;
    return RequestOptions(options);
}
}
