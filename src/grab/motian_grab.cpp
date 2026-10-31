#include "motian_grab.h"
#include "grab_extras.h"
#include "grab_weapons.generated.h"
#include "settings_store.h"
#include "ui_hotkeys.h"
#include "ue_runtime.h"
#include "weapon_presets.h"
#include "grab_state_leases.h"
#include "grab_huashan_adapter.h"
#include "grab_effect_guard.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <iterator>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace MotianGrab {
namespace {
using ue::Object; using ue::Name; using ue::Call; using ue::Array;
struct Vector { double x=0,y=0,z=0; };
struct Rotation { double pitch=0,yaw=0,roll=0; };
struct TraceColor { float r=0,g=0,b=0,a=0; };
struct Hit { unsigned char bytes[256]{}; };
struct Floor { unsigned char bytes[272]{}; };
struct Alignment {
    bool enabled=false;
    Vector original,target,desired;
    Rotation rotation;
    float half=0,radius=0;
    double distance=0;
};
struct Tags { Array tags,parents; };
struct Key { Name name; uintptr_t detail=0,controller=0; };
struct Weak { int32_t index=-1,serial=0; };
struct CurrentInfo { Weak executor,target; float start=0; uint8_t is_target=0,pad[3]{}; int32_t execution=0,executed=0; };
struct AnimData { Object* source=nullptr,*victim=nullptr,*source_object=nullptr; };
struct Interface { Object* object=nullptr; void* address=nullptr; };
struct SoftPath { Weak cache; Name package,asset; ue::String sub; };
struct Identity { Object* object=nullptr; Name name; Weak weak; };
enum class RefPhase { Acquiring, Owned, Releasing };
struct ClassLease { Identity identity; RefPhase phase=RefPhase::Acquiring; };
struct PreparedTransition {
    Identity source,victim,source_cdo,victim_cdo;
    int32_t from_phase=0;bool reverse=true;
    uint32_t grace_ms=1500,max_duration_ms=20000;bool requires_combat_components=true;
};
struct Prepared {
    int32_t variant=-1; size_t step=0; bool ready=false,failed=false,uncertain=false,contract_valid=false;
    Identity source,victim,bullet,event,source_cdo,victim_cdo;
    Name success_timeline;
    std::vector<ClassLease> assets;
    std::vector<Identity> dependencies;
    std::vector<PreparedTransition> transitions;
    Identity guard_effect;
    // Source Boss weapon classes for the grab stand-in (cosmetic; never blocks the move).
    std::vector<Identity> weapons; std::vector<const GrabWeaponData*> weapon_rows;
    std::vector<Identity> proxies; // pose-proxy mesh per weapon row (empty when unavailable)
    float last_ms=0; double total_ms=0,max_ms=0; bool diagnosed=false; uint32_t profile_reports=0; std::wstring status;
};
struct Context { Object* world=nullptr,*pawn=nullptr,*controller=nullptr,*mesh=nullptr,*movement=nullptr,*asc=nullptr,*execution=nullptr; };
struct DependencyData { const char* path;bool is_class; };
struct TransitionData {
    const char* source;const char* victim;int32_t from_phase=0;bool reverse=true;
    uint32_t grace_ms=1500,max_duration_ms=20000;bool requires_combat_components=true;
};
struct VariantData {
    const char* id; const wchar_t* name; const char* source; const char* victim; const char* bullet;
    const char* boss_id="MoTian"; const wchar_t* boss_name=L"魔渊";
    const wchar_t* success_timeline=L"GrabSuccess";
    double authored_distance=0;
    uint32_t max_duration_ms=15000;
    bool supported=true;
    const wchar_t* reason=L"已核定的原生成对能力；仍需游戏内逐招测试";
    const DependencyData* dependencies=nullptr; size_t dependency_count=0;
    const TransitionData* transitions=nullptr;size_t transition_count=0;
    uint8_t alignment_mode=0; // 0: audited Line recipe; 1: preserve authored native bases.
    float max_range=400,recommended_range=280;
    GrabStateLeases::Policy source_policy,victim_policy;
    bool translate_player_requirement=false,huashan_domain_bridge=false;
    const char* guard_effect=nullptr;uint32_t deferred_grace_ms=0;
    uint8_t event_kind=0; // 0 audited Grab event; 1 exact MengHua01 PlayExecution recipe.
    bool requires_qte_task_owner=false;
    bool verify_source_end_walking=false;
};
struct BossData { const char* id; const wchar_t* name; const wchar_t* status; };
#include "motian_grab_data.generated.h"
struct Spec { int32_t handle=0; Identity cdo; uint8_t active=0; std::vector<Identity> instances;bool pending=false; };
struct Grant {
    Identity asc,cdo; int32_t handle=0; std::vector<Identity> instances;
    bool native_receipt=false; uint32_t cancel_rounds=0; ULONGLONG next_cancel=0; int last_state=-1;
    Identity expected_avatar;CurrentInfo expected_info;uint32_t phase=0;
};
struct PhaseReceipt {
    Identity executor,target,execution,source_asc,target_asc,source_class,victim_class;
    CurrentInfo info;uint32_t definition=0;ULONGLONG observed=0;bool refreshed=false,admitted=false;
};
constexpr const wchar_t* kCleanupTags[]={L"CharacterState.Stand.Invulnerable.Execution",L"CharacterState.Immortal.Execution",
    L"CharacterState.Forbid.Ability.Focusing.Execution",L"CharacterState.HitReact.State.Executing"};
struct CleanupProof { Identity actor,asc,capsule; bool tags[std::size(kCleanupTags)]{}; uint8_t pawn_response=0; bool check_collision=true; };
struct Session {
    bool redirect=false; // started by a redirected 黑伤 execution, not the grab chord
    Identity pawn,target,world,source_class,victim_class,source_asc,target_asc,execution;
    std::vector<int32_t> source_before,target_before;
    std::vector<Grant> grants;
    CleanupProof source_proof,target_proof;
    ULONGLONG began=0,asymmetric_since=0; bool started=false,paired=false,uncertain=false,receipts_bound=false;
    // The executor ran its authored timeline to the end (the victim may still be getting up).
    bool source_completed=false;
    int32_t source_handle=0,victim_handle=0; int cleanup_state=-1;
    int32_t variant=-1;uint32_t max_duration_ms=15000;
    std::vector<PhaseReceipt> phases;
    std::vector<PreparedTransition> transitions;
    int32_t phase=0,pending_transition=-1;ULONGLONG transition_since=0;
    Identity target_execution;
    ULONGLONG broker_clock=0,broker_last_tick=0,settle_since=0;uint32_t settle_frames=0;
    double broker_world_time=0,broker_elapsed=0;bool broker_time_initialized=false;
    uint64_t broker_steps=0,settle_last_step=0;
    GrabStateLeases::Ledger state_lease;
    GrabHuashanAdapter::Ledger huashan_lease;
    GrabEffectGuard::Ledger effect_guard;
    uint32_t deferred_grace_ms=0;
    Identity source_movement;bool verify_source_end_walking=false;uint8_t source_mode_before=0;
};
static_assert(sizeof(Vector)==24 && sizeof(Rotation)==24 && sizeof(Hit)==256 && sizeof(Floor)==272 && sizeof(TraceColor)==16 &&
    sizeof(Tags)==32 && sizeof(Key)==24 && sizeof(CurrentInfo)==32 && sizeof(AnimData)==24 && sizeof(SoftPath)==40);
SRWLOCK state_lock=SRWLOCK_INIT,request_lock=SRWLOCK_INIT;
Snapshot state_;
Options options_,pending_;
Logger logger_=nullptr; InputFocus input_focus_=nullptr;
Object* engine_=nullptr;
Session session_;
Prepared prepared_;
// Second slot: MoTian's full grab kept loaded for 黑伤 executions beside the menu
// move. prepared_ is always the slot being prepared or run; run_variant_ is its move.
Prepared spare_;
int32_t run_variant_=0,execution_variant_=-1;
bool slot_swapped_=false; // prepared_ holds the execution move until its session ends
// A 黑伤 execution that was cancelled and waits for both sides to finish ending.
struct Redirect {
    bool pending=false; ULONGLONG deadline=0;
    Identity target,executor,victim;
    char ability[128]{};
};
Redirect redirect_;
Identity ignored_execution_; // an execution left native (no eligible target); skipped until it ends
Object* forced_target_=nullptr; bool redirect_attempt_=false;
bool tang_equipped_=false; ULONGLONG next_weapon_read_=0;
std::vector<ClassLease> class_leases_;
std::unordered_map<std::string,Identity> metadata_;
std::unordered_map<std::wstring,Name> names_;
bool preparing_lookup_=false,trigger_lookup_=false;
uint64_t weak_assign_calls_=0,virtual_query_calls_=0;
bool initialized_=false,has_options_=false,trigger_=false,cancel_=false,cleanup_pending_=false;
int32_t trigger_variant_=-1;
bool key_baseline_=false,armed_=false,last_top_=false,last_right_=false;
ULONGLONG top_time_=0,right_time_=0,last_attempt_=0,next_poll_=0,next_cleanup_=0,manual_deadline_=0;
int32_t manual_variant_=-1;
ULONGLONG combo_deadline_=0,next_state_diagnostic_=0;
Identity combo_owner_;
Key top_,right_;
volatile LONG disabled_=0;
uint32_t faults_=0; DWORD fault_code_=0; uintptr_t fault_address_=0;
const char* stage_="idle";
// Why the last owned-grant admission or target eligibility check failed (logged on failure).
const char* admission_reason_="";
const char* reject_reason_="";
char reject_detail_[200]{};
const char* input_gate_="off";
const char* alignment_gate_="idle";
void ResetKeys();
Object* Type(const char* path);
Object* Default(const char* path);
Name CachedName(const wchar_t* text);

void Logf(const char* format,...) {
    if (!logger_) return; char value[1200]{}; va_list args; va_start(args,format); _vsnprintf_s(value,sizeof(value),_TRUNCATE,format,args); va_end(args); logger_(value);
}
void LogPrepareReason(const char* label,const std::wstring& reason) {
    char utf8[512]{};
    if(!reason.empty())WideCharToMultiByte(CP_UTF8,0,reason.c_str(),-1,utf8,static_cast<int>(sizeof(utf8)),nullptr,nullptr);
    Logf("MOTIAN_GRAB_PREPARE %s failed: %s",label,utf8[0]?utf8:"(no reason)");
}
struct PhaseTimer {
    int phase=0; LARGE_INTEGER begin{};
    explicit PhaseTimer(int value):phase(value) { QueryPerformanceCounter(&begin); }
    ~PhaseTimer() {
        static LARGE_INTEGER frequency{};
        static uint32_t slow[3]{}; static double maximum[3]{}; static ULONGLONG next=0;
        if (!frequency.QuadPart&&!QueryPerformanceFrequency(&frequency)) return;
        LARGE_INTEGER end{}; QueryPerformanceCounter(&end);
        const double ms=1000.*static_cast<double>(end.QuadPart-begin.QuadPart)/static_cast<double>(frequency.QuadPart);
        if (ms>8) { ++slow[phase]; maximum[phase]=(std::max)(maximum[phase],ms); }
        const ULONGLONG now=GetTickCount64();
        if (now>=next&&(slow[0]||slow[1]||slow[2])) {
            next=now+2000;
            Logf("MOTIAN_GRAB_GT_SLOW thresholdMs=8 start=%u/%.3fms cleanup=%u/%.3fms poll=%u/%.3fms (count/max)",
                slow[0],maximum[0],slow[1],maximum[1],slow[2],maximum[2]);
            for (int i=0;i<3;++i) { slow[i]=0; maximum[i]=0; }
        }
    }
};
struct StartSegments {
    int phase=0; double ms[8]{}; LARGE_INTEGER previous{},frequency{};
    uint64_t weak_before=weak_assign_calls_,query_before=virtual_query_calls_;
    bool first_profiles=prepared_.profile_reports<2;
    StartSegments() { QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&previous); }
    void Next(int next) {
        LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
        if (frequency.QuadPart) ms[phase]+=1000.*static_cast<double>(now.QuadPart-previous.QuadPart)/frequency.QuadPart;
        previous=now; phase=next;
    }
    ~StartSegments() {
        Next(phase); double total=0; for (double value:ms) total+=value;
        static ULONGLONG next_log=0; const ULONGLONG now=GetTickCount64();
        if (first_profiles||(total>8&&now>=next_log)) {
            if (first_profiles) ++prepared_.profile_reports;
            next_log=now+2000;
            Logf("MOTIAN_GRAB_START_TIMING totalMs=%.3f cacheAssets=%.3f targets=%.3f specs=%.3f alignPreflight=%.3f nativeAndReceipt=%.3f refreshAndLogs=%.3f placement=%.3f successAndLogs=%.3f",
                total,ms[0],ms[1],ms[2],ms[3],ms[4],ms[5],ms[6],ms[7]);
            Logf("MOTIAN_GRAB_START_OPS weakAssign=%llu virtualQuery=%llu",static_cast<unsigned long long>(weak_assign_calls_-weak_before),static_cast<unsigned long long>(virtual_query_calls_-query_before));
        }
    }
};
void Status(const wchar_t* format,...) {
    wchar_t value[600]{}; va_list args; va_start(args,format); vswprintf_s(value,format,args); va_end(args);
    AcquireSRWLockExclusive(&state_lock); state_.status=value; ReleaseSRWLockExclusive(&state_lock);
}
std::string Text(Object* object) {
    if (!object) return "-"; char value[192]{}; ue::NameText(ue::NameOf(object),value,sizeof(value)); return value;
}
std::wstring Wide(const std::string& text) {
    const int count=MultiByteToWideChar(CP_UTF8,0,text.c_str(),-1,nullptr,0);
    if (count<=1) return {}; std::wstring value(static_cast<size_t>(count),L'\0');
    MultiByteToWideChar(CP_UTF8,0,text.c_str(),-1,value.data(),count); value.pop_back(); return value;
}
bool Readable(const void* pointer,size_t bytes) {
    if (!pointer) return false; MEMORY_BASIC_INFORMATION page{};
    ++virtual_query_calls_;
    if (!VirtualQuery(pointer,&page,sizeof(page)) || page.State!=MEM_COMMIT || (page.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
    const auto at=reinterpret_cast<uintptr_t>(pointer),start=reinterpret_cast<uintptr_t>(page.BaseAddress);
    return at>=start && bytes<=page.RegionSize && at-start<=page.RegionSize-bytes;
}
bool ArrayOk(const Array& array,size_t stride,int32_t limit=2048) {
    return array.num>=0 && array.num<=array.max && array.max<=limit && (!array.num || Readable(array.data,static_cast<size_t>(array.num)*stride));
}
const Object* Item(int32_t index) {
    auto* array=reinterpret_cast<Object*>(ue::Base()+ue::layout::kObjectArray);
    if (index<0 || !Readable(array,40) || index>=ue::At<int32_t>(array,32)) return nullptr;
    Object** chunks=ue::At<Object**>(array,24); const uint32_t chunk=static_cast<uint32_t>(index)>>16;
    if (!Readable(chunks,(static_cast<size_t>(chunk)+1)*8) || !chunks[chunk]) return nullptr;
    Object* item=chunks[chunk]+(static_cast<uint32_t>(index)&65535)*24ull;
    return Readable(item,24)?item:nullptr;
}
Object* Resolve(Weak weak) {
    const Object* item=weak.serial>0?Item(weak.index):nullptr;
    if (!item || ue::At<int32_t>(const_cast<Object*>(item),16)!=weak.serial) return nullptr;
    Object* object=ue::At<Object*>(const_cast<Object*>(item),8); return ue::Alive(object)?object:nullptr;
}
Identity Id(Object* object) {
    Identity id; if (!ue::Alive(object)) return id;
    // A freshly loaded Class/CDO/ability may not yet have a GUObjectItem
    // serial. Reading zero is not a weak identity. Use the exact native
    // FWeakObjectPtr assignment to allocate its serial before recording it.
    static constexpr unsigned char code[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x12,0x48,0x8b,0xd9,0x48,0x85,0xd2,0x74};
    const uintptr_t entry=ue::Base()+0x166f2a0;
    if (!ue::Base() || !Readable(reinterpret_cast<void*>(entry),sizeof(code)) || std::memcmp(reinterpret_cast<void*>(entry),code,sizeof(code))) return id;
    ++weak_assign_calls_;
    reinterpret_cast<void(__fastcall*)(Weak*,Object* const*)>(entry)(&id.weak,&object);
    if (Resolve(id.weak)!=object) return {};
    id.object=object; id.name=ue::NameOf(object); return id;
}
bool Same(const Identity& id) { return id.object && Resolve(id.weak)==id.object && ue::NameOf(id.object)==id.name; }
bool RawIdentity(const Identity& id) {
    const Object* item=id.object&&id.weak.serial>0?Item(id.weak.index):nullptr;
    return item && ue::At<Object*>(const_cast<Object*>(item),8)==id.object && ue::At<int32_t>(const_cast<Object*>(item),16)==id.weak.serial;
}
bool ClassRefHelpersMatch() {
    static constexpr unsigned char add[]={0x40,0x53,0x48,0x83,0xec,0x20,0x8b,0x41,0x0c,0x3b,0x05,0x51,0xef,0x83,0x0c,0x7d};
    static constexpr unsigned char release[]={0x8b,0x41,0x0c,0x3b,0x05,0x17,0x35,0x82,0x0c,0x7d,0x1e,0x8b,0xc8,0x0f,0xb7,0xc0};
    return ue::Base() && !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x1574d00),add,sizeof(add)) &&
        !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x1590740),release,sizeof(release));
}
bool AcquireClass(Object* cls) {
    if (!ClassRefHelpersMatch() || !ue::Alive(cls)) return false;
    Object* meta=Type("/Script/CoreUObject.Class"),*package=Type("/Script/CoreUObject.Package"),*outer=ue::OuterOf(cls);
    // Only cooked package-independent UClasses. Never retain an Actor, ASC,
    // component, ability instance, or another object whose Outer holds a world.
    if (!meta || !package || !ue::IsA(cls,meta) || !ue::Alive(outer) || !ue::IsA(outer,package) || ue::OuterOf(outer)) return false;
    Identity id=Id(cls); if (!Same(id)) return false;
    for (const ClassLease& lease:class_leases_) if (lease.identity.object==cls && RawIdentity(lease.identity)) return lease.phase==RefPhase::Owned;
    if (class_leases_.size()>=2) return false;
    class_leases_.push_back({id,RefPhase::Acquiring}); cleanup_pending_=true;
    // Record uncertainty before the native transition. An exception cannot be
    // retried/decremented speculatively; the retained ledger blocks new work.
    reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x1574d00)(cls);
    class_leases_.back().phase=RefPhase::Owned;
    Logf("MOTIAN_GRAB_CLASS acquired class=%s serial=%d count=%zu world-independent=1",Text(cls).c_str(),id.weak.serial,class_leases_.size());
    return true;
}
bool ReleaseClasses() {
    if (class_leases_.empty()) return true;
    if (!ClassRefHelpersMatch()) return false;
    while (!class_leases_.empty()) {
        ClassLease& lease=class_leases_.back();
        if (lease.phase!=RefPhase::Owned || !RawIdentity(lease.identity)) {
            Logf("MOTIAN_GRAB_CLASS release uncertain; phase=%u rawIdentity=%d; exact ledger retained",static_cast<unsigned>(lease.phase),RawIdentity(lease.identity)?1:0);
            return false;
        }
        Object* cls=lease.identity.object;
        Logf("MOTIAN_GRAB_CLASS release class=%s serial=%d",Text(cls).c_str(),lease.identity.weak.serial);
        lease.phase=RefPhase::Releasing;
        reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x1590740)(cls);
        class_leases_.pop_back();
    }
    return true;
}
bool HasResources() { return session_.began || !class_leases_.empty() || session_.state_lease.HasPending() || session_.huashan_lease.HasPending()||session_.effect_guard.HasPending(); }
Object* Type(const char* path) {
    const auto found=metadata_.find(path);
    if (found!=metadata_.end()&&Same(found->second)) return found->second.object;
    if (trigger_lookup_) return nullptr; // A trigger never performs a cold global lookup.
    Object* object=ue::FindObject(path); const Identity id=Id(object);
    if (Same(id)) metadata_[path]=id; return object;
}
Object* Default(const char* path) { return Type(path); }
Name CachedName(const wchar_t* text) {
    const auto found=names_.find(text); if (found!=names_.end()) return found->second;
    if (trigger_lookup_) return {};
    const Name name=ue::MakeName(text); if (!name.None()) names_[text]=name; return name;
}
Object* Field(Object* cls,const char* name) {
    for (int32_t depth=0;cls && depth<64;++depth,cls=ue::At<Object*>(cls,ue::layout::kSuper)) {
        int32_t count=0;
        for (Object* field=ue::At<Object*>(cls,0x50);field && ++count<2048;field=ue::At<Object*>(field,0x10)) {
            if (!Readable(field,0x78)) return nullptr;
            if (ue::NameIs(ue::At<Name>(field,0x20),name)) return field;
        }
    }
    return nullptr;
}
int32_t Offset(Object* object,const char* name) { return ue::Alive(object)?ue::Offset(ue::ClassOf(object),name):-1; }
template<class T> T Property(Object* object,const char* name) {
    const int32_t offset=Offset(object,name); return offset>=0 && Readable(object+offset,sizeof(T))?ue::At<T>(object,offset):T{};
}
bool Bool(Object* object,const char* name,bool& value) {
    Object* field=ue::Alive(object)?Field(ue::ClassOf(object),name):nullptr;
    if (!field || ue::At<uint8_t>(field,0x70)!=1) return false;
    const int32_t offset=ue::At<int32_t>(field,0x44); const uint8_t byte=ue::At<uint8_t>(field,0x71),mask=ue::At<uint8_t>(field,0x73);
    if (offset<0 || byte>7 || !mask || !Readable(object+offset+byte,1)) return false;
    value=(ue::At<uint8_t>(object,offset+byte)&mask)!=0; return true;
}
Object* Component(Object* actor,const char* path) {
    Object* cls=Type(path); if (!cls) return nullptr;
    Call get(actor,"GetComponentByClass"); return get.Set("ComponentClass",cls).Invoke()?get.Get<Object*>("ReturnValue"):nullptr;
}
Object* Asc(Object* actor) {
    Call get(Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"),"GetAbilitySystemComponent");
    return get.Set("Actor",actor).Invoke()?get.Get<Object*>("ReturnValue"):nullptr;
}
bool ExecutionContext(Object* actor,Object* execution,Object* asc) {
    if (!ue::Alive(execution)||!ue::Alive(asc)||Offset(execution,"CurrentExecutionInfo")!=0x318 ||
        !Readable(execution+0x310,8) || ue::At<Object*>(execution,0x310)!=asc) return false;
    Call owner(execution,"GetOwner"),get(actor,"GetExecutionComponent","/Script/SGameExecution.ExecutionComponentHolder"),
        config(actor,"GetExecutionActorConfig","/Script/SGameExecution.ExecutionComponentHolder");
    if (!owner.Invoke()||owner.Get<Object*>("ReturnValue")!=actor || !get.Invoke()||get.Get<Object*>("ReturnValue")!=execution ||
        !config.Slot("ReturnValue",16)||!config.Invoke()) return false;
    const Interface holder=config.Get<Interface>("ReturnValue");
    return ue::Alive(holder.object);
}
bool KnownMontagePath(Object* mesh) {
    // Exact PBZ paths only. Both audited implementations check non-null
    // skeletons/slot setup rather than target CompatibleSkeletons membership.
    // A foreign AnimInstance override must not inherit that static proof.
    Call get(mesh,"GetAnimInstance");if(!get.Slot("ReturnValue",sizeof(Object*))||!get.Invoke())return false;
    Object* instance=get.Get<Object*>("ReturnValue");if(!ue::Alive(instance)||!Readable(instance,0x30)||!ue::At<Object*>(instance,0x28))return false;
    const auto* table=ue::At<uintptr_t*>(instance,0);if(!Readable(table,0x300))return false;
    const uintptr_t method=table[0x2f8/8],base=ue::Base();
    static constexpr unsigned char native[]={0x48,0x8b,0xc4,0x48,0x89,0x58,0x18,0x48,0x89,0x78,0x20,0x48,0x89,0x50,0x10,0x55};
    static constexpr unsigned char game[]={0x48,0x89,0x5c,0x24,0x08,0x57,0x48,0x83,0xec,0x50,0x0f,0xb6,0x84,0x24,0x90,0x00};
    const unsigned char* expected=method==base+0x3b21580?native:method==base+0x6751970?game:nullptr;
    return base&&expected&&Readable(reinterpret_cast<const void*>(method),16)&&!std::memcmp(reinterpret_cast<const void*>(method),expected,16);
}
bool InWorld(Object* object,Object* world) {
    Object* level=Type("/Script/Engine.Level");
    for (int i=0;ue::Alive(object) && i<32;++i,object=ue::OuterOf(object))
        if (object==world || (level && ue::IsA(object,level) && Property<Object*>(object,"OwningWorld")==world)) return true;
    return false;
}
bool ContextNow(Context& c) {
    if (!ue::Alive(engine_)) engine_=ue::FindEngine();
    c.world=Property<Object*>(Property<Object*>(engine_,"GameViewport"),"World");
    Object* gi=Property<Object*>(c.world,"OwningGameInstance"); const Array players=Property<Array>(gi,"LocalPlayers");
    if (!ue::Alive(c.world) || !ArrayOk(players,8,16) || !players.num) return false;
    c.controller=Property<Object*>(static_cast<Object**>(players.data)[0],"PlayerController");
    c.pawn=Property<Object*>(c.controller,"Pawn");
    Object* player=Type("/Script/ProjectPBZ.PBZCharacter_Player");
    if (!ue::Alive(c.pawn) || !player || !ue::IsA(c.pawn,player) || !InWorld(c.pawn,c.world)) return false;
    c.mesh=Property<Object*>(c.pawn,"Mesh"); c.movement=Property<Object*>(c.pawn,"CharacterMovement"); c.asc=Asc(c.pawn);
    c.execution=Component(c.pawn,"/Script/SGameExecution.ExecutionComponent");
    return ue::Alive(c.controller) && ue::Alive(c.mesh) && ue::Alive(c.movement) && ue::Alive(c.asc) && ue::Alive(c.execution);
}
bool Position(Object* actor,Vector& value) {
    Call get(actor,"K2_GetActorLocation"); if (!get.Slot("ReturnValue",24) || !get.Invoke()) return false;
    value=get.Get<Vector>("ReturnValue"); return std::isfinite(value.x+value.y+value.z);
}
double Separation(Vector a,Vector b) { return std::hypot(std::hypot(a.x-b.x,a.y-b.y),a.z-b.z); }
void AlignmentStatus(bool corrected,const wchar_t* text) {
    AcquireSRWLockExclusive(&state_lock); state_.position_corrected=corrected; state_.alignment_status=text; ReleaseSRWLockExclusive(&state_lock);
}
bool AlignmentFailure(const wchar_t* text) {
    char reason[1800]{};
    if (!WideCharToMultiByte(CP_UTF8,0,text,-1,reason,static_cast<int>(sizeof(reason)),nullptr,nullptr)) strcpy_s(reason,"UTF8 conversion failed");
    Logf("MOTIAN_GRAB_ALIGNMENT refused stage=%s gate=%s reason=%s",stage_,alignment_gate_,reason);
    AlignmentStatus(false,text); Status(L"投技位置修正未通过：%s；未进入成功动作",text); return false;
}
bool AlignmentGate(const char* reason) { alignment_gate_=reason; return false; }
double AuthoredDistance(int32_t variant) {
    // Each pair supplies its own audited success/victim first-key spacing.
    // A missing measurement never falls back to MoTian's alignment.
    return variant>=0 && static_cast<size_t>(variant)<std::size(kVariants)?kVariants[variant].authored_distance:0;
}
bool CapsuleSize(Object* actor,float& half,float& radius) {
    Object* capsule=Property<Object*>(actor,"CapsuleComponent");
    Call h(capsule,"GetScaledCapsuleHalfHeight"),r(capsule,"GetScaledCapsuleRadius");
    if (!h.Invoke()||!r.Invoke()) return AlignmentGate("capsule.native_getters");
    half=h.Get<float>("ReturnValue"); radius=r.Get<float>("ReturnValue");
    if (!std::isfinite(half)||!std::isfinite(radius)||radius<1||radius>150||half<radius||half>300) {
        Logf("MOTIAN_GRAB_ALIGNMENT capsule actor=%s half=%.6f radius=%.6f",Text(actor).c_str(),half,radius);
        return AlignmentGate("capsule.physical_bounds");
    }
    return true;
}
bool FloorAt(Object* movement,Vector center) {
    Object* type=Type("/Script/Engine.FindFloorResult"),*hit_type=Type("/Script/Engine.HitResult");
    if (!type||!hit_type||ue::StructSize(type)!=sizeof(Floor)||ue::StructSize(hit_type)!=sizeof(Hit)) return AlignmentGate("floor.result_layout");
    Call get(movement,"K2_FindFloor");
    if (!get.Slot("FloorResult",sizeof(Floor))||!get.Set("CapsuleLocation",center).Invoke()) return AlignmentGate("floor.K2_FindFloor");
    Floor floor=get.Get<Floor>("FloorResult");
    // FBoolProperty masks, not raw nonzero bytes (several result flags share
    // a byte in this exact Shipping build).
    const auto flag=[&](Object* owner,const char* name,const unsigned char* data,size_t length,bool& value) {
        Object* field=Field(owner,name); if (!field||ue::At<uint8_t>(field,0x70)!=1) return false;
        const int32_t offset=ue::At<int32_t>(field,0x44); const uint8_t extra=ue::At<uint8_t>(field,0x71),mask=ue::At<uint8_t>(field,0x73);
        if (offset<0||extra>7||!mask||static_cast<size_t>(offset)+extra>=length) return false;
        value=(data[offset+extra]&mask)!=0; return true;
    };
    bool blocking=false,walkable=false,penetrating=true;
    if (!flag(type,"bBlockingHit",floor.bytes,sizeof(Floor),blocking)||!flag(type,"bWalkableFloor",floor.bytes,sizeof(Floor),walkable)) return AlignmentGate("floor.bool_metadata");
    if (!blocking||!walkable) {
        Logf("MOTIAN_GRAB_ALIGNMENT floor movement=%s at=(%.3f,%.3f,%.3f) blocking=%d walkable=%d",Text(movement).c_str(),center.x,center.y,center.z,blocking?1:0,walkable?1:0);
        return AlignmentGate("floor.not_walkable");
    }
    Object* distance=Field(type,"FloorDist"),*hit=Field(type,"HitResult"); if (!distance||!hit) return AlignmentGate("floor.scalar_metadata");
    const int32_t d=ue::At<int32_t>(distance,0x44),size=ue::At<int32_t>(distance,0x34),h=ue::At<int32_t>(hit,0x44);
    if (d<0||d+size>static_cast<int32_t>(sizeof(Floor))||(size!=4&&size!=8)||h<0||h+sizeof(Hit)>sizeof(Floor)||ue::At<int32_t>(hit,0x34)!=sizeof(Hit)) return AlignmentGate("floor.scalar_layout");
    const double gap=size==4?ue::At<float>(floor.bytes,d):ue::At<double>(floor.bytes,d);
    if (!flag(hit_type,"bStartPenetrating",floor.bytes+h,sizeof(Hit),penetrating)) return AlignmentGate("floor.penetration_metadata");
    if (!std::isfinite(gap)||gap<-1||gap>12||penetrating) {
        Logf("MOTIAN_GRAB_ALIGNMENT floor movement=%s gap=%.6f penetrating=%d at=(%.3f,%.3f,%.3f)",Text(movement).c_str(),gap,penetrating?1:0,center.x,center.y,center.z);
        return AlignmentGate("floor.distance_or_penetration");
    }
    return true;
}
bool CapsuleClear(Object* context,Vector center,float half,float radius,const Array& ignore) {
    Call trace(Default("/Script/Engine.Default__KismetSystemLibrary"),"CapsuleTraceSingle");
    if (!trace.Set("WorldContextObject",context).Set("Start",center).Set("End",center).Set("Radius",radius).Set("HalfHeight",half)
        .Set("TraceChannel",uint8_t{0}).Set("bTraceComplex",false).Set("ActorsToIgnore",ignore).Set("DrawDebugType",uint8_t{0})
        .Set("bIgnoreSelf",true).Set("TraceColor",TraceColor{}).Set("TraceHitColor",TraceColor{}).Set("DrawTime",0.f).Invoke()) return AlignmentGate("capsule_trace.native_call");
    if (trace.Get<bool>("ReturnValue")) {
        Logf("MOTIAN_GRAB_ALIGNMENT capsule blocked at=(%.3f,%.3f,%.3f) half=%.3f radius=%.3f pairIgnored=%d",center.x,center.y,center.z,half,radius,ignore.num);
        return AlignmentGate("capsule_trace.blocking_environment");
    }
    return true;
}
struct FloorIgnore {
    Array pawns;
    ~FloorIgnore() { ue::Free(pawns.data); }
    bool Load(Object* context,Object* source,Object* target) {
        Call list(Default("/Script/Engine.Default__GameplayStatics"),"GetAllActorsOfClass");
        if (!list.Set("WorldContextObject",context).Set("ActorClass",Type("/Script/Engine.Pawn")).Invoke()) return AlignmentGate("pawn_list.native_call");
        pawns=list.Get<Array>("OutActors"); if (!ArrayOk(pawns,8,8192)) return AlignmentGate("pawn_list.array_layout");
        bool a=false,b=false; for (int32_t i=0;i<pawns.num;++i) { Object* p=static_cast<Object**>(pawns.data)[i]; a=a||p==source; b=b||p==target; }
        return (a&&(!target||b))||AlignmentGate("pawn_list.pair_not_in_world");
    }
};
bool GroundAt(Object* context,Vector xy,double expected,const Array& ignore,double slope,Vector& point,Vector& normal) {
    Call trace(Default("/Script/Engine.Default__KismetSystemLibrary"),"LineTraceSingle");
    if (!trace.Set("WorldContextObject",context).Set("Start",Vector{xy.x,xy.y,expected+60}).Set("End",Vector{xy.x,xy.y,expected-150})
        .Set("TraceChannel",uint8_t{0}).Set("bTraceComplex",false).Set("ActorsToIgnore",ignore).Set("DrawDebugType",uint8_t{0})
        .Set("bIgnoreSelf",true).Set("TraceColor",TraceColor{}).Set("TraceHitColor",TraceColor{}).Set("DrawTime",0.f).Invoke()) return AlignmentGate("ground.line_trace_native");
    if (!trace.Get<bool>("ReturnValue")) {
        Logf("MOTIAN_GRAB_ALIGNMENT ground missing at=(%.3f,%.3f) expectedZ=%.3f trace=[%.3f,%.3f]",xy.x,xy.y,expected,expected+60,expected-150);
        return AlignmentGate("ground.no_hit");
    }
    const Hit hit=trace.Get<Hit>("OutHit"); Call split(Default("/Script/Engine.Default__GameplayStatics"),"BreakHitResult");
    if (!split.Ok()) split=Call(Default("/Script/Engine.Default__KismetSystemLibrary"),"BreakHitResult");
    if (!split.Slot("bBlockingHit",1)||!split.Slot("bInitialOverlap",1)||!split.Slot("ImpactPoint",sizeof(Vector))||!split.Slot("ImpactNormal",sizeof(Vector))||
        !split.Set("Hit",hit).Invoke()) return AlignmentGate("ground.BreakHitResult");
    if (!split.Get<bool>("bBlockingHit")||split.Get<bool>("bInitialOverlap")) return AlignmentGate("ground.nonblocking_or_penetrating");
    point=split.Get<Vector>("ImpactPoint"); normal=split.Get<Vector>("ImpactNormal");
    if (!std::isfinite(point.x+point.y+point.z+normal.x+normal.y+normal.z)||normal.z<slope||std::fabs(point.z-expected)>20) {
        Logf("MOTIAN_GRAB_ALIGNMENT ground rejected at=(%.3f,%.3f,%.3f) expectedZ=%.3f normal=(%.6f,%.6f,%.6f) requiredZ=%.6f",point.x,point.y,point.z,expected,normal.x,normal.y,normal.z,slope);
        return AlignmentGate("ground.slope_or_level");
    }
    return true;
}
bool WalkableSlope(Object* movement,double& slope) {
    // GetWalkableFloorZ is a C++ inline/Blueprint display name. The exact
    // reflected UFunction in this game is K2_GetWalkableFloorZ.
    Call get(movement,"K2_GetWalkableFloorZ");
    if (!get.Slot("ReturnValue",sizeof(float))||!get.Invoke()) return AlignmentGate("slope.K2_GetWalkableFloorZ");
    const float value=get.Get<float>("ReturnValue");
    if (!std::isfinite(value)||value<0||value>1) return AlignmentGate("slope.invalid_value");
    slope=(std::max)(.7,static_cast<double>(value)); return true;
}
bool PrepareAlignment(const Context& c,Object* target,Alignment& plan,const Array* current_pawns=nullptr) {
    alignment_gate_="idle";
    AlignmentStatus(false,options_.position_correction?L"正在检查原始配对间距与安全地面":L"位置修正已关闭；仅使用原生 MotionWarping");
    if (!options_.position_correction) return true;
    Object* movement=Property<Object*>(target,"CharacterMovement"); float target_half=0,target_radius=0;
    stage_="alignment source position";
    if (!Position(c.pawn,plan.original)) return AlignmentFailure(L"无法获取施放者位置");
    stage_="alignment target position";
    if (!Position(target,plan.target)) return AlignmentFailure(L"无法获取受投者位置");
    stage_="alignment source capsule";
    if (!CapsuleSize(c.pawn,plan.half,plan.radius)) return AlignmentFailure(L"施放者胶囊资料不完整");
    stage_="alignment target capsule";
    if (!CapsuleSize(target,target_half,target_radius)) return AlignmentFailure(L"受投者胶囊资料不完整");
    stage_="alignment source floor";
    if (!FloorAt(c.movement,plan.original)) return AlignmentFailure(L"施放者脚下尚非有效可行走地面");
    stage_="alignment target floor";
    if (!FloorAt(movement,plan.target)) return AlignmentFailure(L"受投者脚下尚非有效可行走地面");
    if(kVariants[run_variant_].alignment_mode==1){
        AlignmentStatus(false,L"此招按原生执行者 / 目标坐标基准定位；已验证双方地面，不做额外传送或转向");return true;
    }
    stage_="alignment source rotation";
    Call rotation(c.pawn,"K2_GetActorRotation");
    if (!rotation.Slot("ReturnValue",sizeof(Rotation))||!rotation.Invoke()) return AlignmentFailure(L"无法捕获施放者原方向");
    plan.rotation=rotation.Get<Rotation>("ReturnValue");
    const double dx=plan.target.x-plan.original.x,dy=plan.target.y-plan.original.y,length=std::hypot(dx,dy);
    plan.distance=AuthoredDistance(run_variant_);
    stage_="alignment authored spacing";
    if (!std::isfinite(length)||length<1||plan.distance<1||!std::isfinite(plan.rotation.pitch+plan.rotation.yaw+plan.rotation.roll))
        return AlignmentFailure(L"双方重合或原始配对数据不可用");
    plan.desired={plan.target.x-dx/length*plan.distance,plan.target.y-dy/length*plan.distance,0};
    if (std::hypot(plan.desired.x-plan.original.x,plan.desired.y-plan.original.y)>450) return AlignmentFailure(L"单次位移超过安全范围");
    stage_="alignment world pawn list";
    FloorIgnore ignore;
    if (!current_pawns&&!ignore.Load(c.pawn,c.pawn,target)) return AlignmentFailure(L"当前场景 Pawn 列表不可验证");
    const Array& ignored_pawns=current_pawns?*current_pawns:ignore.pawns;
    stage_="alignment native walkable slope";
    double slope=0; if (!WalkableSlope(c.movement,slope)) return AlignmentFailure(L"原生可行走坡度不可验证");
    Vector target_floor,target_normal,source_floor,source_normal;
    stage_="alignment target ground trace";
    if (!GroundAt(c.pawn,plan.target,plan.target.z-target_half,ignored_pawns,slope,target_floor,target_normal))
        return AlignmentFailure(L"受投者脚底地面追踪 / 坡度未通过");
    stage_="alignment executor ground trace";
    if (!GroundAt(c.pawn,plan.desired,target_floor.z,ignored_pawns,slope,source_floor,source_normal))
        return AlignmentFailure(L"施放者候选落点地面追踪 / 坡度未通过");
    stage_="alignment pair floor level";
    if (std::fabs(source_floor.z-target_floor.z)>12) {
        Logf("MOTIAN_GRAB_ALIGNMENT floor level source=%.6f target=%.6f delta=%.6f limit=12",source_floor.z,target_floor.z,source_floor.z-target_floor.z);
        return AlignmentFailure(L"配对脚底不在同一地面，邻近台阶 / 悬空");
    }
    plan.desired.z=source_floor.z+plan.half+plan.radius*(1./source_normal.z-1.)+4.;
    if(Separation(plan.desired,plan.original)>450)return AlignmentFailure(L"地面校正后的三维位移超过 450 cm，未传送");
    Object* pair[]={c.pawn,target}; const Array pair_ignore{pair,2,2};
    stage_="alignment candidate environment capsule";
    // Before GEd activates, the pair may still block one another. The authored
    // short spacing can overlap those two capsules; preflight excludes only
    // the pair, then K2_FindFloor validates the actual post-activation fit.
    if (!CapsuleClear(c.pawn,plan.desired,plan.half,plan.radius,pair_ignore))
        return AlignmentFailure(L"原始配对落点被环境阻挡");
    plan.enabled=true;
    Logf("MOTIAN_GRAB_ALIGNMENT planned variant=%s authoredDistance=%.6f from=(%.3f,%.3f,%.3f) to=(%.3f,%.3f,%.3f) target=(%.3f,%.3f,%.3f) floorDelta=%.3f; target/yaw unchanged",
        kVariants[run_variant_].id,plan.distance,plan.original.x,plan.original.y,plan.original.z,plan.desired.x,plan.desired.y,plan.desired.z,
        plan.target.x,plan.target.y,plan.target.z,source_floor.z-target_floor.z);
    return true;
}
bool ApplyAlignment(const Context& c,Object* target,const Alignment& plan) {
    if (!plan.enabled) return true;
    Vector current_target; if (!Position(target,current_target)||Separation(current_target,plan.target)>2) return AlignmentFailure(L"原生成对激活期间目标位置变化；取消本次并重新瞄准");
    Call teleport(c.pawn,"K2_TeleportTo");
    const bool moved=teleport.Set("DestLocation",plan.desired).Set("DestRotation",plan.rotation).Invoke()&&teleport.Get<bool>("ReturnValue");
    Vector actual; Object* pair[]={c.pawn,target}; const Array ignore{pair,2,2};
    const bool positioned=moved&&Position(c.pawn,actual)&&std::hypot(actual.x-plan.desired.x,actual.y-plan.desired.y)<=2&&std::fabs(actual.z-plan.desired.z)<=8&&
        FloorAt(c.movement,actual)&&CapsuleClear(c.pawn,actual,plan.half,plan.radius,ignore);
    if (!positioned) {
        // No timeline has entered the success phase. Roll back only this one
        // placement, immediately on the same GT; never undo authored movement
        // after a cast, and never move the victim.
        bool rollback=true;
        if (moved) {
            Call back(c.pawn,"K2_TeleportTo"); Vector restored;
            rollback=back.Set("DestLocation",plan.original).Set("DestRotation",plan.rotation).Invoke()&&back.Get<bool>("ReturnValue")&&
                Position(c.pawn,restored)&&Separation(restored,plan.original)<=8&&FloorAt(c.movement,restored);
        }
        Logf("MOTIAN_GRAB_ALIGNMENT refused nativeFit=%d rollback=%d; pair cancellation requested, no success timeline",moved?1:0,rollback?1:0);
        return AlignmentFailure(rollback?L"原生碰撞适配未保留配对间距，已撤销本次定位":L"原生落点 / 回退读回未通过，已请求成对取消");
    }
    if (!Call(c.movement,"StopMovementImmediately").Invoke()||!Call(c.movement,"ClearAccumulatedForces").Invoke())
        return AlignmentFailure(L"定位后无法确认清除原移动惯性，已请求取消");
    AlignmentStatus(true,L"已按原招式首帧间距定位施放者一次；目标与双方方向未改，后续交原生 MotionWarping");
    Logf("MOTIAN_GRAB_ALIGNMENT placed actual=(%.3f,%.3f,%.3f) pairDistance=%.6f expected=%.6f walkable=1 singlePlacement=1",
        actual.x,actual.y,actual.z,std::hypot(actual.x-current_target.x,actual.y-current_target.y),plan.distance);
    return true;
}
bool AliveHealth(Object* pawn) {
    Call dead(Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");
    if (!dead.Set("Pawn",pawn).Invoke() || dead.Get<bool>("ReturnValue")) return false;
    Object* set=Property<Object*>(pawn,"AttributeSetBase"),*data=Type("/Script/GameplayAbilities.GameplayAttributeData");
    const int32_t health=Offset(set,"Health"),current=data?ue::Offset(data,"CurrentValue"):-1;
    if (!ue::Alive(set) || health<0 || !data || ue::StructSize(data)!=16 || current!=12 || !Readable(set+health,16)) return false;
    const float hp=ue::At<float>(set,health+current); return std::isfinite(hp) && hp>0;
}
bool ReadCleanupProof(Object* actor,Object* asc,CleanupProof& proof) {
    proof.actor=Id(actor); proof.asc=Id(asc); proof.capsule=Id(Property<Object*>(actor,"CapsuleComponent"));
    if (!Same(proof.actor)||!Same(proof.asc)||!Same(proof.capsule)) return false;
    Call response(proof.capsule.object,"GetCollisionResponseToChannel");
    if (!response.Set("Channel",uint8_t{2}).Invoke()) return false;
    proof.pawn_response=response.Get<uint8_t>("ReturnValue");
    for (size_t i=0;i<std::size(kCleanupTags);++i) {
        Call tag(asc,"HasMatchingGameplayTag","/Script/GameplayTags.GameplayTagAssetInterface");
        if (!tag.Set("TagToCheck",CachedName(kCleanupTags[i])).Invoke()) return false;
        proof.tags[i]=tag.Get<bool>("ReturnValue");
    }
    return true;
}
bool VerifyCleanupProof(const CleanupProof& original) {
    if (!Same(original.actor)) return Resolve(original.actor.weak)==nullptr;
    if(session_.began&&(!Same(session_.world)||!InWorld(original.actor.object,session_.world.object)))return true;
    if (!Same(original.capsule)) return false;
    CleanupProof actual;
    if(Same(original.asc)){
        if (!ReadCleanupProof(original.actor.object,original.asc.object,actual)) return false;
        for (size_t i=0;i<std::size(kCleanupTags);++i) if (actual.tags[i]!=original.tags[i]) return false;
    }else{
        // Tags belonged to the retired ASC. Still prove the live capsule's
        // physical baseline without calling into that obsolete component.
        if(Property<Object*>(original.actor.object,"CapsuleComponent")!=original.capsule.object)return false;
        Call response(original.capsule.object,"GetCollisionResponseToChannel");
        if(!response.Set("Channel",uint8_t{2}).Invoke())return false;actual.pawn_response=response.Get<uint8_t>("ReturnValue");
    }
    if (original.check_collision && actual.pawn_response!=original.pawn_response) {
        Call dead(Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");
        if (!dead.Set("Pawn",original.actor.object).Invoke()||!dead.Get<bool>("ReturnValue")) return false;
    }
    return true;
}
bool Family(Name name,const char* prefix) {
    char text[256]{}; ue::NameText(name,text,sizeof(text)); const size_t n=std::strlen(prefix);
    return !_strnicmp(text,prefix,n) && (!text[n] || text[n]=='.');
}
// relaxed: a grab target under "ignore invulnerability" may be hit-reacting or
// in a boss special move. knockdown: the player may also be knocked down (the
// grab triggers from any combat state). Death and story states always refuse.
bool SafeState(Object* pawn,bool relaxed=false,bool knockdown=false) {
    Call current(Component(pawn,"/Script/SGameActorState.ActorStateComponent"),"GetCurrentStatesContainer");
    if (!current.Slot("CurrentStates",32) || !current.Invoke()) return false;
    const Tags tags=current.Get<Tags>("CurrentStates"); bool allowed=ArrayOk(tags.tags,8)&&ArrayOk(tags.parents,8);
    constexpr const char* blocked[]={"CharacterState.Dead","CharacterState.FallDown","CharacterState.HitReact","CharacterState.Narrative",
        "CharacterState.Native.Sequence","CharacterState.SpecialMove","CharacterState.StoryCameraLock"};
    if (allowed) for (int32_t i=0;i<tags.tags.num;++i) for (const char* name:blocked)
        if (Family(static_cast<Name*>(tags.tags.data)[i],name)) {
            if (relaxed&&(!std::strcmp(name,"CharacterState.HitReact")||!std::strcmp(name,"CharacterState.SpecialMove"))) continue;
            if (knockdown&&!std::strcmp(name,"CharacterState.FallDown")) continue;
            if (allowed) ue::NameText(static_cast<Name*>(tags.tags.data)[i],reject_detail_,sizeof(reject_detail_));
            allowed=false;
            if (GetTickCount64()>=next_state_diagnostic_) {
                next_state_diagnostic_=GetTickCount64()+1000;
                char actual[200]{}; ue::NameText(static_cast<Name*>(tags.tags.data)[i],actual,sizeof(actual));
                Logf("MOTIAN_GRAB_STATE refused actor=%s tag=%s",Text(pawn).c_str(),actual);
            }
        }
    ue::Free(tags.tags.data); ue::Free(tags.parents.data); return allowed;
}
bool InputGate(const char* reason,const wchar_t* message,bool allowed=false) {
    if (std::strcmp(input_gate_,reason)) {
        input_gate_=reason; Logf("MOTIAN_GRAB_INPUT gate=%s allowed=%d",reason,allowed?1:0);
        Status(L"%s",message);
    }
    return allowed;
}
bool InputBaseAllowed(const Context& c) {
    if (!input_focus_ || !input_focus_()) return InputGate("game_focus",L"等待回到游戏窗口；菜单 / 失焦时不触发投技");
    if (!AliveHealth(c.pawn)) return InputGate("player_not_alive",L"当前主角生命状态不允许投技");
    // Hit-stun, knock-down and special moves no longer hold the grab back.
    if (!SafeState(c.pawn,true,true)) return InputGate("protected_player_state",L"受击、倒地、剧情或特殊移动中，等待恢复后投技");
    bool cinematic=false;
    if (Bool(c.controller,"bCinematicMode",cinematic) && cinematic) return InputGate("cinematic",L"等待退出过场控制");
    Call paused(Default("/Script/Engine.Default__GameplayStatics"),"IsGamePaused");
    if (!paused.Set("WorldContextObject",c.pawn).Invoke() || paused.Get<bool>("ReturnValue")) return InputGate("paused",L"暂停时不触发投技");
    return true;
}
bool InputAllowed(const Context& c) {
    // Hit-stun and knock-down ignore move input; the grab may still start then.
    const uint8_t mode=Property<uint8_t>(c.movement,"MovementMode");
    if (mode!=1 && mode!=2) return InputGate("not_grounded",L"当前投技要求站在地面；落地后按三角 + 圈");
    return InputGate("ready",L"输入已就绪：附近人形敌人，三角 + 圈触发",true);
}
bool Specs(Object* asc,std::vector<Spec>& result,int32_t only_handle=0,bool headers_only=false) {
    Object* type=Type("/Script/GameplayAbilities.GameplayAbilitySpec"),*container=Type("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t abilities=Offset(asc,"ActivatableAbilities"),items=container?ue::Offset(container,"Items"):-1;
    if (!type || ue::StructSize(type)!=240 || abilities!=1048 || items!=264 || ue::Offset(type,"Handle")!=12 ||
        ue::Offset(type,"Ability")!=16 || ue::Offset(type,"ActiveCount")!=40) return false;
    const Array array=ue::At<Array>(asc,abilities+items); if (!ArrayOk(array,240,1024)) return false;
    Object* owner_class=ue::ClassOf(asc); const Name owner_name=ue::NameOf(asc);
    for (int32_t i=0;i<array.num;++i) {
        Object* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*240; Spec spec;
        spec.handle=ue::At<int32_t>(row,12); if (only_handle&&spec.handle!=only_handle) continue;
        Object* cdo=ue::At<Object*>(row,16); spec.active=ue::At<uint8_t>(row,40);
        if (headers_only) {
            // Baseline ownership needs handles only. ExecutionBusy inspects
            // tags only for active rows, on this same GT; no unrelated CDO or
            // instance becomes a weak identity or a retained object.
            if (!spec.handle||(spec.active&&!ue::Alive(cdo))) return false;
            spec.cdo.object=cdo; result.push_back(spec); continue;
        }
        spec.cdo=Id(cdo);
        if (!spec.handle || !Same(spec.cdo)) return false;
        for (const char* name:{"NonReplicatedInstances","ReplicatedInstances"}) {
            const int32_t offset=ue::Offset(type,name); if (offset<0) return false;
            const Array list=ue::At<Array>(row,offset); if (!ArrayOk(list,8,64)) return false;
            for (int32_t j=0;j<list.num;++j) {
                const Identity id=Id(static_cast<Object**>(list.data)[j]);
                if (Same(id) && std::none_of(spec.instances.begin(),spec.instances.end(),[&](const Identity& old){return old.object==id.object;})) spec.instances.push_back(id);
            }
        }
        result.push_back(spec);
        if (only_handle) break;
    }
    const Array after=ue::At<Array>(asc,abilities+items);
    return ue::Alive(asc)&&ue::ClassOf(asc)==owner_class&&ue::NameOf(asc)==owner_name&&
        after.data==array.data&&after.num==array.num&&after.max==array.max;
}
bool ExecutionBusy(const std::vector<Spec>& specs) {
    for (const Spec& spec:specs) if (spec.active) {
        if (Offset(spec.cdo.object,"AbilityTags")<0) return true;
        const Tags tags=Property<Tags>(spec.cdo.object,"AbilityTags"); if (!ArrayOk(tags.tags,8)) return true;
        for (int32_t i=0;i<tags.tags.num;++i) if (Family(static_cast<Name*>(tags.tags.data)[i],"Ability.Action.Execution") ||
            Family(static_cast<Name*>(tags.tags.data)[i],"Ability.Executed")) return true;
    }
    return false;
}
Object* Load(const char* path,bool is_class) {
    if (!preparing_lookup_||trigger_lookup_) return nullptr;
    if (Object* object=ue::FindObject(path)) return object;
    const std::string text(path); const size_t dot=text.find_last_of('.'); if (dot==std::string::npos) return nullptr;
    const std::wstring package=Wide(text.substr(0,dot)),asset=Wide(text.substr(dot+1)); SoftPath soft;
    soft.package=CachedName(package.c_str()); soft.asset=CachedName(asset.c_str());
    Call load(Default("/Script/Engine.Default__KismetSystemLibrary"),is_class?"LoadClassAsset_Blocking":"LoadAsset_Blocking");
    if (!load.Set(is_class?"AssetClass":"Asset",soft).Invoke()) return nullptr;
    return load.Get<Object*>("ReturnValue");
}
bool InstancedClass(Object* cls,const char* path) {
    const std::string text(path); const size_t dot=text.find_last_of('.'); if (dot==std::string::npos) return false;
    Object* cdo=ue::FindObject((text.substr(0,dot+1)+"Default__"+text.substr(dot+1)).c_str());
    if (!ue::Alive(cdo)||!ue::IsDefault(cdo)||ue::ClassOf(cdo)!=cls||Offset(cdo,"InstancingPolicy")<0) return false;
    const uint8_t policy=Property<uint8_t>(cdo,"InstancingPolicy"); return policy==1||policy==2;
}
bool ClassListed(Object* actor,const char* const* list,size_t count) {
    for (Object* cls=ue::ClassOf(actor);cls;cls=ue::At<Object*>(cls,0x40)) for (size_t i=0;i<count;++i) {
        const char* dot=std::strrchr(list[i],'.'); if (!dot || !ue::NameIs(ue::NameOf(cls),dot+1)) continue;
        const std::string package(list[i],static_cast<size_t>(dot-list[i]));
        if (ue::NameIs(ue::NameOf(ue::OuterOf(cls)),package.c_str())) return true;
    }
    return false;
}
bool Reject(const char* reason) { reject_reason_=reason; return false; }
// Humanoid mob rigs whose body is built differently from Hun's (the crow's pelvis
// is turned 180 degrees): the borrowed victim animation would not map by bone name.
constexpr const char* kRigMismatchClasses[]={
    "/PBZCombatAsset/ActorData/Mobs/017_IllnessCrow/BP_Monster_IllnessCrow_Claw.BP_Monster_IllnessCrow_Claw_C",
};
// boss: set for listed bosses. Ordinary mobs may be grabbed while hit-reacting or in a
// special move (they spend most of a fight there); bosses need "ignore invulnerability".
// precheck: target of a native execution still running, so its state, movement and
// busy checks wait until that execution has been cancelled.
bool Enemy(Object* actor,const Context& c,Object*& asc,Object*& execution,bool& boss,bool precheck=false) {
    reject_reason_=""; reject_detail_[0]=0;
    if (!ue::Alive(actor) || ue::IsTemplate(actor) || actor==c.pawn || !InWorld(actor,c.world)) return Reject("not_in_world");
    boss=ClassListed(actor,kDeniedBossClasses,std::size(kDeniedBossClasses)); bool flag=false;
    if (Bool(actor,"bIsBoss",flag) && flag) boss=true;
    if ((boss && !options_.allow_bosses) || (!boss && !ClassListed(actor,kOrdinaryEnemyClasses,std::size(kOrdinaryEnemyClasses)))) return Reject("class_not_listed");
    if (ClassListed(actor,kRigMismatchClasses,std::size(kRigMismatchClasses))) return Reject("rig_mismatch");
    Object* controller=Property<Object*>(actor,"Controller"),*ai=Type("/Script/SGameAI.SGameAIController");
    if (!ai || !ue::Alive(controller) || !ue::IsA(controller,ai)) return Reject("no_ai_controller");
    if (!AliveHealth(actor)) return Reject("dead_or_no_health");
    const bool relaxed=!boss||GrabExtras::GetOptions().ignore_invulnerable||redirect_attempt_;
    if (!precheck && !SafeState(actor,relaxed,redirect_attempt_)) return Reject("protected_state");
    Object* movement=Property<Object*>(actor,"CharacterMovement"); const uint8_t mode=Property<uint8_t>(movement,"MovementMode");
    if (!ue::Alive(movement)||(!precheck&&mode!=1&&mode!=2)) { sprintf_s(reject_detail_,"movement_mode=%u",mode); return Reject("not_walking"); }
    Object* teams=Default("/Script/SGameAI.Default__SGameAIController");
    Call own(teams,"GetTeam"),other(teams,"GetTeam"),attitude(teams,"GetDefaultTeamAttitude");
    if (!own.Set("Actor",c.pawn).Invoke() || !other.Set("Actor",actor).Invoke() ||
        !attitude.Set("Team1",other.Get<uint8_t>("ReturnValue")).Set("Team2",own.Get<uint8_t>("ReturnValue")).Invoke() || attitude.Get<uint8_t>("ReturnValue")!=2) return false;
    Object* mesh=Property<Object*>(actor,"Mesh");
    for (const wchar_t* bone:{L"root",L"head",L"hand_l",L"hand_r",L"foot_l",L"foot_r"}) {
        Call index(mesh,"GetBoneIndex");
        if (!index.Set("BoneName",CachedName(bone)).Invoke() || index.Get<int32_t>("ReturnValue")<0) { WideCharToMultiByte(CP_UTF8,0,bone,-1,reject_detail_,sizeof(reject_detail_),nullptr,nullptr); return Reject("humanoid_bones"); }
    }
    Object* capsule=Property<Object*>(actor,"CapsuleComponent");
    Call height(capsule,"GetScaledCapsuleHalfHeight"),radius(capsule,"GetScaledCapsuleRadius");
    if (!height.Invoke() || !radius.Invoke()) return false;
    const float h=height.Get<float>("ReturnValue"),r=radius.Get<float>("ReturnValue");
    if (!std::isfinite(h+r) || h<40 || h>160 || r<15 || r>100 || h<r) { sprintf_s(reject_detail_,"capsule=%.0fx%.0f",h,r); return Reject("body_size"); }
    execution=Component(actor,"/Script/SGameExecution.ExecutionComponent"); asc=Asc(actor);
    if (!ue::Alive(execution) || !ue::Alive(asc) || !ue::Alive(Component(actor,"/Script/SGameCombatSystem.HitreactComponent")) ||
        !ue::Alive(Component(actor,"/Script/SGameLocomotionDriver.CharacterLocomotionComponent"))) return Reject("missing_components");
    for (const char* path:{"/Script/SGameExecution.ExecutionComponentHolder","/Script/SGameCombatSystem.HitreactComponentHolder",
        "/Script/SGameActorState.ActorStateComponentHolderInterface","/Script/SGameUtils.CharacterGeneralComponentsHolder"}) {
        Object* interface_class=Type(path);
        Call implements(Default("/Script/Engine.Default__KismetSystemLibrary"),"DoesImplementInterface");
        if (!interface_class || !implements.Set("TestObject",actor).Set("Interface",interface_class).Invoke()||!implements.Get<bool>("ReturnValue")) return Reject("missing_interface");
    }
    if (!ExecutionContext(actor,execution,asc)) return Reject("execution_context");
    std::vector<Spec> specs; if (!Specs(asc,specs,0,true)) return Reject("ability_specs");
    if (!precheck && ExecutionBusy(specs)) return Reject("already_executing");
    return true;
}
// relaxed: skip the authored ignored tags except death and executions (ordinary mobs,
// "ignore invulnerability", redirected executions). precheck: skip them all, since the
// target is still inside the native execution being replaced.
bool OriginalGrabTags(Object* event,Object* asc,bool relaxed,bool precheck=false) {
    for (const char* property:{"Target Required Tags","Target Ignored Tags"}) if (Offset(event,property)<0) return false;
    Tags required=Property<Tags>(event,"Target Required Tags");const Tags ignored=Property<Tags>(event,"Target Ignored Tags");
    if (!ArrayOk(required.tags,8)||!ArrayOk(required.parents,8)||!ArrayOk(ignored.tags,8)||!ArrayOk(ignored.parents,8)) return false;
    std::vector<Name> translated;
    if(kVariants[run_variant_].translate_player_requirement){
        // Exact MengHua02 receiver audit: replace only its Player admission
        // with Enemy's existing hostile-humanoid test. Never add Player tags.
        bool removed=false;
        for(int32_t i=0;i<required.tags.num;++i){const Name tag=static_cast<Name*>(required.tags.data)[i];
            if(ue::NameIs(tag,"CharacterType.Player")){removed=true;continue;}translated.push_back(tag);}
        if(!removed)return false;
        required.tags={translated.data(),static_cast<int32_t>(translated.size()),static_cast<int32_t>(translated.size())};required.parents={};
    }
    Call all(asc,"HasAllMatchingGameplayTags","/Script/GameplayTags.GameplayTagAssetInterface");
    if (!all.Set("TagContainer",required).Invoke() || !all.Get<bool>("ReturnValue")) return Reject("missing_required_tag");
    // Each authored ignored tag is checked on its own so the refusal names it.
    // Relaxed admission lets the grab through every ignored tag except death and
    // an execution already in progress on the target.
    for (int32_t i=0;!precheck&&i<ignored.tags.num;++i) {
        const Name tag=static_cast<Name*>(ignored.tags.data)[i];
        if (relaxed&&!Family(tag,"CharacterState.Dead")&&!Family(tag,"Ability.Action.Execution")&&!Family(tag,"Ability.Executed")) continue;
        Call has(asc,"HasMatchingGameplayTag","/Script/GameplayTags.GameplayTagAssetInterface");
        if (!has.Set("TagToCheck",tag).Invoke()) return Reject("tag_query");
        if (has.Get<bool>("ReturnValue")) { ue::NameText(tag,reject_detail_,sizeof(reject_detail_)); return Reject("ignored_tag"); }
    }
    return true;
}
bool ReceiverComponents(Object* actor) {
    Call input(actor,"GetAbilityInputComponent","/Script/SGameInputSystem.AbilityInputComponentHolder"),weapons(actor,"GetCharacterWeaponsComponent","/Script/SGameCombatSystem.CharacterWeaponsHolder");
    return input.Invoke()&&weapons.Invoke()&&ue::Alive(input.Get<Object*>("ReturnValue"))&&ue::Alive(weapons.Get<Object*>("ReturnValue"))&&
        ue::IsA(input.Get<Object*>("ReturnValue"),Type("/Script/SGameInputSystem.AbilityInputComponent"))&&
        ue::IsA(weapons.Get<Object*>("ReturnValue"),Type("/Script/SGameCombatSystem.CharacterWeaponsComponent"));
}
bool QteReceiverReady(Object* actor) {
    Object* tasks=Component(actor,"/Script/GameplayTasks.GameplayTasksComponent");Call owner(tasks,"GetOwner");
    return ue::Alive(tasks)&&ue::IsA(tasks,Type("/Script/GameplayTasks.GameplayTasksComponent"))&&owner.Invoke()&&owner.Get<Object*>("ReturnValue")==actor;
}
Object* GrabEvent(Object* bullet,Object* victim_class,Name success_timeline,uint8_t kind=0,const char* expected_event=nullptr) {
    if(kind>1)return nullptr;
    Object* cls=Type(kind?"/SGameExecution/Bullet/Events/BP_BulletEvent_PlayExecution.BP_BulletEvent_PlayExecution_C":"/SGameExecution/Bullet/Events/BP_BulletEvent_Grab.BP_BulletEvent_Grab_C");
    Object* pre=ue::Alive(bullet)?Field(ue::ClassOf(bullet),"PreAttackEvents"):nullptr;
    if (!cls||!pre||ue::At<int32_t>(pre,0x34)!=48) return nullptr;
    Object* data_type=ue::At<Object*>(pre,0x70); Object* events=data_type?Field(data_type,"Events"):nullptr;
    if (!events||ue::StructSize(data_type)!=48||ue::At<int32_t>(events,0x44)!=32||ue::At<int32_t>(events,0x34)!=16) return nullptr;
    const int32_t pre_offset=ue::At<int32_t>(pre,0x44);
    if (pre_offset!=0x1a8||!Readable(bullet+pre_offset,48)) return nullptr;
    const Array list=ue::At<Array>(bullet,pre_offset+32); if (!ArrayOk(list,8,128)) return nullptr;
    Object* result=nullptr;
    for (int32_t i=0;i<list.num;++i) {
        Object* event=static_cast<Object**>(list.data)[i];
        if (!ue::Alive(event)||ue::OuterOf(event)!=bullet||!ue::IsA(event,cls)||Property<Object*>(event,"Executed Ability")!=victim_class) continue;
        if(expected_event&&!ue::NameIs(ue::NameOf(event),expected_event))continue;
        if(!kind){Object* success=Field(ue::ClassOf(event),"Success Timeline");
            if(!success||ue::At<int32_t>(success,0x34)!=static_cast<int32_t>(sizeof(Name))||success_timeline.None()||!(Property<Name>(event,"Success Timeline")==success_timeline))continue;
        }else{
            Object* source=Field(ue::ClassOf(event),"Execution Ability");
            if(!source||ue::At<int32_t>(source,0x34)!=8||Property<Object*>(event,"Execution Ability"))continue;
        }
        if (result) return nullptr; result=event;
    }
    return result;
}
constexpr const char* kPreparationMetadata[]={
    "/Script/CoreUObject.Class",
    "/Script/CoreUObject.Package",
    "/Script/Engine.Actor",
    "/Script/Engine.Default__GameplayStatics",
    "/Script/Engine.Default__KismetStringLibrary",
    "/Script/Engine.Default__KismetSystemLibrary",
    "/Script/Engine.FindFloorResult",
    "/Script/Engine.HitResult",
    "/Script/Engine.Level",
    "/Script/Engine.Pawn",
    "/Script/Engine.World",
    "/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary",
    "/Script/GameplayAbilities.GameplayAbilitySpec",
    "/Script/GameplayAbilities.GameplayAbilitySpecContainer",
    "/Script/GameplayAbilities.GameplayAttributeData",
    "/Script/GameplayTags.GameplayTagAssetInterface",
    "/Script/InputCore.Key",
    "/Script/ProjectPBZ.PBZCharacter_Player",
    "/Script/SGameAI.Default__SGameAIController",
    "/Script/SGameAI.Default__SGameAIStateUtils",
    "/Script/SGameAI.SGameAIController",
    "/Script/SGameActorState.ActorStateComponent",
    "/Script/SGameActorState.ActorStateComponentHolderInterface",
    "/Script/SGameCombatSystem.HitreactComponent",
    "/Script/SGameCombatSystem.HitreactComponentHolder",
    "/Script/SGameCombatSystem.TargetingComponentHolder",
    "/Script/SGameCombatSystem.TargetingComponent",
    "/Script/SGameCombatSystem.CharacterWeaponsHolder",
    "/Script/SGameCombatSystem.CharacterWeaponsComponent",
    "/Script/SGameInputSystem.AbilityInputComponentHolder",
    "/Script/SGameInputSystem.AbilityInputComponent",
    "/Script/GameplayTasks.GameplayTasksComponent",
    "/Script/SGameExecution.CurrentExecutionInfo",
    "/Script/SGameExecution.ExecutionAbilityInterface",
    "/Script/SGameExecution.ExecutionAnimData",
    "/Script/SGameExecution.ExecutionComponent",
    "/Script/SGameExecution.ExecutionComponentHolder",
    "/Script/SGameExecution.GrabExecutionAbilityInterface",
    "/Script/SGameLocomotionDriver.CharacterLocomotionComponent",
    "/Script/SGameUtils.CharacterGeneralComponentsHolder",
    "/Script/SGameUtils.Default__AbilityUtils",
};
bool ReleasePrepared() {
    if (prepared_.assets.empty()) { prepared_={}; return true; }
    if (!ClassRefHelpersMatch()) return false;
    while (!prepared_.assets.empty()) {
        ClassLease& lease=prepared_.assets.back();
        if (lease.phase!=RefPhase::Owned||!RawIdentity(lease.identity)) {
            if (!prepared_.uncertain) Logf("MOTIAN_GRAB_PREPARE release uncertain; asset ledger retained");
            prepared_.uncertain=true; prepared_.ready=false; return false;
        }
        Object* object=lease.identity.object; lease.phase=RefPhase::Releasing;
        reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x1590740)(object);
        prepared_.assets.pop_back();
    }
    Logf("MOTIAN_GRAB_PREPARE asset refs released independently of session refs"); prepared_={}; return true;
}
bool PinPrepared(Object* object,bool is_class) {
    if (!ClassRefHelpersMatch()||!ue::Alive(object)) return false;
    for(const ClassLease& lease:prepared_.assets)if(lease.identity.object==object&&RawIdentity(lease.identity))return lease.phase==RefPhase::Owned;
    constexpr size_t kMaxPreparedAssets=64;
    if(prepared_.assets.size()>=kMaxPreparedAssets)return false;
    Object* outer=ue::OuterOf(object),*package=Type("/Script/CoreUObject.Package");
    if (!package||!ue::Alive(outer)||!ue::IsA(outer,package)||ue::OuterOf(outer)) return false;
    if (is_class&&!ue::IsA(object,Type("/Script/CoreUObject.Class"))) return false;
    // The caller passes only the fixed package-root class/BulletData paths.
    // A world or live Actor is never a valid prepared asset.
    if (ue::IsA(object,Type("/Script/Engine.Actor"))||ue::IsA(object,Type("/Script/Engine.World"))) return false;
    const Identity id=Id(object); if (!Same(id)) return false;
    prepared_.assets.push_back({id,RefPhase::Acquiring});
    reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x1574d00)(object);
    prepared_.assets.back().phase=RefPhase::Owned; return true;
}
std::vector<const GrabWeaponData*> WeaponRows(const char* variant_id) {
    std::vector<const GrabWeaponData*> rows;
    for (const GrabWeaponData& row:kGrabWeapons) if (variant_id&&!std::strcmp(row.variant,variant_id)) rows.push_back(&row);
    return rows;
}
Object* AbilityCDO(const char* path) {
    const std::string text(path); const size_t dot=text.find_last_of('.');
    return dot==std::string::npos?nullptr:ue::FindObject((text.substr(0,dot+1)+"Default__"+text.substr(dot+1)).c_str());
}
bool DefinitionSupported(const VariantData& v) {
    if(!(v.supported&&v.id&&*v.id&&v.source&&v.source[0]=='/'&&v.victim&&v.victim[0]=='/'&&v.bullet&&v.bullet[0]=='/'&&v.success_timeline&&*v.success_timeline&&_wcsicmp(v.success_timeline,L"None")&&
        std::isfinite(v.authored_distance)&&v.authored_distance>=0&&v.authored_distance<=100000&&v.alignment_mode<=1&&v.event_kind<=1&&
        (v.alignment_mode==1||(v.authored_distance>0&&v.authored_distance<=3200))&&
        std::isfinite(v.max_range+v.recommended_range)&&v.max_range>=80&&v.max_range<=3200&&v.recommended_range>=80&&v.recommended_range<=v.max_range&&
        v.max_duration_ms>=1000&&v.max_duration_ms<=120000&&v.dependency_count<=61&&(!v.dependency_count||v.dependencies)&&
        v.transition_count<=4&&(!v.transition_count||v.transitions)))return false;
    for(size_t i=0;i<v.dependency_count;++i)if(!v.dependencies[i].path||v.dependencies[i].path[0]!='/')return false;
    for(size_t i=0;i<v.transition_count;++i){const auto& t=v.transitions[i];if(!t.source||t.source[0]!='/'||!t.victim||t.victim[0]!='/'||t.from_phase<0||t.from_phase>3||t.grace_ms<100||t.grace_ms>3000||t.max_duration_ms<1000||t.max_duration_ms>60000)return false;}
    if(v.translate_player_requirement&&(std::strcmp(v.id,"MengHua:GA_MengHua_GEx_02:BD_MengHua_GEx_02:BP_BulletEvent_Grab_C_0")||
        std::strcmp(v.source,"/PBZCombatAsset/ActorData/Mobs/078_MengHua/Ability/GA_MengHua_GEx_02.GA_MengHua_GEx_02_C")||
        std::strcmp(v.victim,"/PBZCombatAsset/ActorData/Mobs/078_MengHua/Ability/GA_MengHua_GEd_02.GA_MengHua_GEd_02_C")))return false;
    if(v.huashan_domain_bridge&&std::strcmp(v.id,"HuaShan:GA_HuaShan_Troop_GEx_01:BD_HuaShan_Troop_GEx_01:BP_BulletEvent_Grab_C_0"))return false;
    if(v.event_kind&&(std::strcmp(v.id,"MengHua:GA_MengHua_GEx_01:BD_MengHua_GEx_01:BP_BulletEvent_PlayExecution_C_0")||
        std::wcscmp(v.success_timeline,L"GrabSuccess")||v.alignment_mode!=1||
        std::strcmp(v.source,"/PBZCombatAsset/ActorData/Mobs/078_MengHua/Ability/GA_MengHua_GEx_01.GA_MengHua_GEx_01_C")||
        std::strcmp(v.victim,"/PBZCombatAsset/ActorData/Mobs/078_MengHua/Ability/GA_MengHua_GEd_01.GA_MengHua_GEd_01_C")))return false;
    if(v.guard_effect){
        if(std::strcmp(v.id,"XuKong:GA_XuKong_GEx_01:GA_XuKong_CA_Grab:BP_BulletEvent_Grab_C_1")||
            std::strcmp(v.guard_effect,"/PBZCombatAsset/ActorData/Mobs/078_MengHua/GameplayEffects/GE_MengHua_XuKong_LockHealth.GE_MengHua_XuKong_LockHealth_C")||
            v.deferred_grace_ms<100||v.deferred_grace_ms>3000)return false;
    }else if(v.deferred_grace_ms)return false;
    if(v.verify_source_end_walking&&std::strcmp(v.id,"MadDoctor:GA_MadDoctor_CA_01_Gex:BD_GA_MadDoctor_CA_01_Gex:BP_BulletEvent_Grab_C_0"))return false;
    if(v.transition_count){const auto& t=v.transitions[0];
        const bool zuo=!std::strcmp(v.id,"ZuoShang:GA_ZuoShang_P2_Skill_20:BulletData_ZuoShang_P2_Skill_20_GEx_01:BP_BulletEvent_Grab_C_0")&&
            !std::strcmp(v.source,"/PBZCombatAsset/ActorData/Mobs/076_ZuoShang_P2/Ability/GA_ZuoShang_P2_Skill_20.GA_ZuoShang_P2_Skill_20_C")&&
            !std::strcmp(v.victim,"/PBZCombatAsset/ActorData/Mobs/076_ZuoShang_P2/Ability/GA_ZuoShang_P2_Skill_20_GEd.GA_ZuoShang_P2_Skill_20_GEd_C")&&
            !std::strcmp(t.source,"/PBZCombatAsset/ActorData/Players/Hun/Abilities/Common/ExE/GA_Hun_BEx_ZuoSHang_P2_Skill_20.GA_Hun_BEx_ZuoSHang_P2_Skill_20_C")&&
            !std::strcmp(t.victim,"/PBZCombatAsset/ActorData/Mobs/054_ZuoShang/Ability/Abilities/GA_Hun_BEd_ZuoShang_P2_Skill_20.GA_Hun_BEd_ZuoShang_P2_Skill_20_C");
        const bool lion=!std::strcmp(v.id,"LionDancer:GA_LionDancer_Gex_01:BulletData_LionDancer_GEx_01:BP_BulletEvent_Grab_C_0")&&
            !std::strcmp(v.source,"/PBZCombatAsset/ActorData/Mobs/LionDancer/Ability/GA_LionDancer_Gex_01.GA_LionDancer_Gex_01_C")&&
            !std::strcmp(v.victim,"/PBZCombatAsset/ActorData/Mobs/LionDancer/Ability/GA_LionDancer_GEd_01.GA_LionDancer_GEd_01_C")&&
            !std::strcmp(t.source,"/PBZCombatAsset/ActorData/Players/Hun/Abilities/Common/ExE/LionDancer/GA_Hun_BDEx_Common_LionDancer_01.GA_Hun_BDEx_Common_LionDancer_01_C")&&
            !std::strcmp(t.victim,"/PBZCombatAsset/ActorData/Players/Hun/Abilities/Common/ExE/LionDancer/GA_Hun_BDEd_Common_LionDancer_01.GA_Hun_BDEd_Common_LionDancer_01_C");
        if(v.transition_count!=1||(!zuo&&!lion)||std::wcscmp(v.success_timeline,L"GrabSuccess")||t.from_phase!=0||!t.reverse)return false;
    }
    return true;
}
bool MoveSupported(int32_t index) {
    return index>=0&&static_cast<size_t>(index)<std::size(kVariants)&&DefinitionSupported(kVariants[index]);
}
bool TimelineQueryLayout(Object* function) {
    return ue::Alive(function)&&ue::At<uint16_t>(function,ue::layout::kFuncParmsSize)==16&&
        ue::Offset(function,"TimelineName")==0&&ue::Offset(function,"ReturnValue")==8;
}
// FindFunction returns the first name match. This build has two GetTimelineByName
// natives; only the Lego scanner (parms 16, TimelineName@0, ReturnValue@8) is valid.
Object* FindTimelineQuery(Object* source) {
    int depth=0;
    for(Object* cls=ue::ClassOf(source);ue::Alive(cls)&&++depth<64;cls=ue::At<Object*>(cls,ue::layout::kSuper)){
        int count=0;
        for(Object* function=ue::At<Object*>(cls,ue::layout::kChildren);ue::Alive(function)&&++count<512;function=ue::At<Object*>(function,ue::layout::kFieldNext))
            if(ue::NameIs(ue::NameOf(function),"GetTimelineByName")&&TimelineQueryLayout(function))return function;
    }
    return nullptr;
}
bool ExecutionContract(Object* source,Object* victim,bool grab) {
    if(!ue::Alive(source)||!ue::Alive(victim)||Offset(source,"Execution Component")<0)return false;
    Call source_info(source,"GetCurrentExecutionInfo","/Script/SGameExecution.ExecutionAbilityInterface"),
        victim_info(victim,"GetCurrentExecutionInfo","/Script/SGameExecution.ExecutionAbilityInterface"),
        refresh(source,"InitializeExecutionInfo");
    if(!source_info.Slot("Info",sizeof(CurrentInfo))||!victim_info.Slot("Info",sizeof(CurrentInfo))||!refresh.Ok())return false;
    if(!grab)return true;
    Call success(source,"PlayGrabTimeline","/Script/SGameExecution.GrabExecutionAbilityInterface"),
        timeline(source,FindTimelineQuery(source)),playing(source,"IsPlayingTimeline");
    return success.Slot("StartTimeSeconds",sizeof(float))&&success.Slot("TimelineName",sizeof(Name))&&
        timeline.Slot("TimelineName",sizeof(Name))&&timeline.Slot("ReturnValue",sizeof(Object*))&&
        playing.Slot("Timeline",sizeof(Object*))&&playing.Slot("ReturnValue",sizeof(bool));
}
bool InvokeNamedTimeline(Object* source,Object* function,Name success) {
    if(!TimelineQueryLayout(function))return false;
    Call call(source,function);
    return call.Set("TimelineName",success).Invoke()&&ue::Alive(call.Get<Object*>("ReturnValue"));
}
bool SourceSuccessTimeline(Object* source,Name success) {
    if(!ue::Alive(source)||success.None())return false;
    bool called=false;int depth=0;
    for(Object* cls=ue::ClassOf(source);ue::Alive(cls)&&++depth<64;cls=ue::At<Object*>(cls,ue::layout::kSuper)){
        int count=0;
        for(Object* function=ue::At<Object*>(cls,ue::layout::kChildren);ue::Alive(function)&&++count<512;function=ue::At<Object*>(function,ue::layout::kFieldNext)){
            if(!ue::NameIs(ue::NameOf(function),"GetTimelineByName")||!TimelineQueryLayout(function))continue;
            called=true;
            if(InvokeNamedTimeline(source,function,success))return true;
        }
    }
    // Same scan as the Lego native: ability.Timelines at +0x400, object FName at +0x18.
    if(!Readable(reinterpret_cast<unsigned char*>(source)+0x400,16)){
        Logf("MOTIAN_GRAB_PREPARE success timeline unreadable called=%d",called?1:0);return false;
    }
    const Array list=ue::At<Array>(source,0x400);
    if(!ArrayOk(list,8,64)){
        Logf("MOTIAN_GRAB_PREPARE success timeline array invalid called=%d num=%d",called?1:0,list.num);return false;
    }
    for(int32_t i=0;i<list.num;++i){
        Object* item=static_cast<Object**>(list.data)[i];
        if(ue::Alive(item)&&ue::NameOf(item)==success)return true;
    }
    Logf("MOTIAN_GRAB_PREPARE success timeline missing on CDO called=%d count=%d",called?1:0,list.num);
    return false;
}
bool WarmTransitions(const VariantData& variant) {
    prepared_.transitions.clear();
    if(variant.transition_count||variant.deferred_grace_ms){Call time(Default("/Script/Engine.Default__GameplayStatics"),"GetTimeSeconds");
        if(!time.Slot("WorldContextObject",8)||!time.Slot("ReturnValue",8))return false;}
    for(size_t i=0;i<variant.transition_count;++i){const auto& data=variant.transitions[i];PreparedTransition t;
        Object* source=ue::FindObject(data.source),*victim=ue::FindObject(data.victim);
        const auto pinned=[](Object* o){return std::any_of(prepared_.assets.begin(),prepared_.assets.end(),[&](const ClassLease& p){return p.phase==RefPhase::Owned&&p.identity.object==o&&Same(p.identity);});};
        if(!pinned(source)||!pinned(victim)||!InstancedClass(source,data.source)||!InstancedClass(victim,data.victim))return false;
        t.source=Id(source);t.victim=Id(victim);t.source_cdo=Id(AbilityCDO(data.source));t.victim_cdo=Id(AbilityCDO(data.victim));
        if(!Same(t.source)||!Same(t.victim)||!Same(t.source_cdo)||!Same(t.victim_cdo)||!ExecutionContract(t.source_cdo.object,t.victim_cdo.object,false))return false;
        for(Object* cdo:{t.source_cdo.object,t.victim_cdo.object}){Call source_object(cdo,"GetCurrentSourceObject");if(!source_object.Slot("ReturnValue",8))return false;}
        t.from_phase=data.from_phase;t.reverse=data.reverse;t.grace_ms=data.grace_ms;t.max_duration_ms=data.max_duration_ms;t.requires_combat_components=data.requires_combat_components;prepared_.transitions.push_back(t);
    }
    return true;
}
bool PreparedReady() {
    if(!std::all_of(prepared_.dependencies.begin(),prepared_.dependencies.end(),[](const Identity& id){return Same(id);}))return false;
    if(!std::all_of(prepared_.transitions.begin(),prepared_.transitions.end(),[](const PreparedTransition& t){return Same(t.source)&&Same(t.victim)&&Same(t.source_cdo)&&Same(t.victim_cdo);}))return false;
    return MoveSupported(run_variant_)&&(!kVariants[run_variant_].guard_effect||Same(prepared_.guard_effect))&&prepared_.ready&&prepared_.contract_valid&&prepared_.variant==run_variant_&&!prepared_.uncertain&&Same(prepared_.source)&&Same(prepared_.victim)&&
        prepared_.transitions.size()==kVariants[run_variant_].transition_count&&
        Same(prepared_.bullet)&&Same(prepared_.event)&&Same(prepared_.source_cdo)&&Same(prepared_.victim_cdo)&&
        ue::OuterOf(prepared_.event.object)==prepared_.bullet.object&&Property<Object*>(prepared_.event.object,"Executed Ability")==prepared_.victim.object&&
        !prepared_.success_timeline.None()&&(kVariants[run_variant_].event_kind==1?!Property<Object*>(prepared_.event.object,"Execution Ability"):Property<Name>(prepared_.event.object,"Success Timeline")==prepared_.success_timeline)&&
        ue::ClassOf(prepared_.source_cdo.object)==prepared_.source.object&&ue::ClassOf(prepared_.victim_cdo.object)==prepared_.victim.object;
}
// wanted: this slot should hold run_variant_; otherwise its assets are released.
void AdvancePreparation(bool wanted) {
    if (HasResources()) return; // Session proof/cleanup retains its own ledger.
    if (!wanted||prepared_.variant!=run_variant_) {
        if (!ReleasePrepared()) return;
        if (!wanted) return;
        prepared_.variant=run_variant_;
    }
    if(!MoveSupported(run_variant_)){
        prepared_.failed=true;prepared_.ready=false;
        prepared_.status=kVariants[run_variant_].reason;
        return;
    }
    if (prepared_.failed||prepared_.uncertain||PreparedReady()) return;
    prepared_.ready=false; preparing_lookup_=true;
    struct LookupReset { ~LookupReset() { preparing_lookup_=false; } } reset;
    LARGE_INTEGER begin{},end{},frequency{}; QueryPerformanceFrequency(&frequency); QueryPerformanceCounter(&begin);
    const size_t step=prepared_.step; bool ok=false; const char* label="metadata";
    std::wstring specific;
    const size_t count=std::size(kPreparationMetadata);
    const VariantData& variant=kVariants[run_variant_];
    if (!step) prepared_.weapon_rows=WeaponRows(variant.id);
    const size_t weapon_steps=prepared_.weapon_rows.size(),total=count+5+variant.dependency_count+weapon_steps;
    if (!step&&!ue::Alive(engine_)) engine_=ue::FindEngine(); // Preparation, never the trigger frame.
    if (step<count) ok=Type(kPreparationMetadata[step])!=nullptr;
    else if (step==count) {
        label="names"; ok=true;
        for (const wchar_t* name:{L"root",L"head",L"hand_l",L"hand_r",L"foot_l",L"foot_r",L"Gamepad_FaceButton_Top",L"Gamepad_FaceButton_Right",variant.success_timeline})
            ok=!CachedName(name).None()&&ok;
        for (const wchar_t* name:kCleanupTags) ok=!CachedName(name).None()&&ok;
        prepared_.success_timeline=CachedName(variant.success_timeline);ok=!prepared_.success_timeline.None()&&ok;
    } else {
        if (step==count+1) { label="source asset"; Object* value=Load(variant.source,true); ok=PinPrepared(value,true); if (ok) prepared_.source=Id(value); }
        else if (step==count+2) { label="victim asset"; Object* value=Load(variant.victim,true); ok=PinPrepared(value,true); if (ok) prepared_.victim=Id(value); }
        else if (step==count+3) { label="bullet asset"; Object* value=Load(variant.bullet,false); ok=PinPrepared(value,false); if (ok) prepared_.bullet=Id(value); }
        else if(step>=count+4&&step<count+4+variant.dependency_count){
            label="authored dependency";const DependencyData& dep=variant.dependencies[step-count-4];
            Object* value=dep.path&&*dep.path?Load(dep.path,dep.is_class):nullptr;ok=PinPrepared(value,dep.is_class);
            if(ok){const Identity id=Id(value);ok=Same(id);if(ok)prepared_.dependencies.push_back(id);}
        }
        else if(step>=count+4+variant.dependency_count&&step<count+4+variant.dependency_count+weapon_steps){
            // Cosmetic: a missing weapon class is logged and skipped, never a preparation failure.
            label="source boss weapon";const GrabWeaponData* row=prepared_.weapon_rows[step-count-4-variant.dependency_count];
            Object* value=Load(row->actor_class,true);
            // One entry per row, empty when unavailable, so weapons[i] always matches weapon_rows[i].
            Identity id{};
            if(value&&PinPrepared(value,true))id=Id(value);
            if(!Same(id)){id={};Logf("MOTIAN_GRAB_PREPARE boss weapon %s unavailable; grab keeps working without it",row->actor_class);}
            prepared_.weapons.push_back(id);
            Identity proxy{};
            if(row->proxy){Object* mesh=Load(row->proxy,false);if(mesh&&PinPrepared(mesh,false))proxy=Id(mesh);
                if(!Same(proxy)){proxy={};Logf("MOTIAN_GRAB_PREPARE boss pose proxy %s unavailable; weapon falls back to the player's hand",row->proxy);}}
            prepared_.proxies.push_back(proxy);
            ok=true;
        }
        else if (step==count+4+variant.dependency_count+weapon_steps) {
            label="owned events and class defaults";
            const char* suffix=std::strrchr(variant.id,':');
            Object* event=GrabEvent(prepared_.bullet.object,prepared_.victim.object,prepared_.success_timeline,variant.event_kind,suffix?suffix+1:nullptr);
            prepared_.event=Id(event); prepared_.source_cdo=Id(AbilityCDO(variant.source)); prepared_.victim_cdo=Id(AbilityCDO(variant.victim));
            const bool identities=Same(prepared_.event)&&Same(prepared_.source_cdo)&&Same(prepared_.victim_cdo)&&
                InstancedClass(prepared_.source.object,variant.source)&&InstancedClass(prepared_.victim.object,variant.victim);
            const bool contract=identities&&ExecutionContract(prepared_.source_cdo.object,prepared_.victim_cdo.object,true);
            const bool warm=contract&&WarmTransitions(variant);
            ok=warm;
            if(!identities){specific=L"投技事件或能力默认对象未对齐";Logf("MOTIAN_GRAB_PREPARE owned events identity failed");}
            else if(!contract){specific=L"处决时间轴接口布局不匹配";Logf("MOTIAN_GRAB_PREPARE execution contract slots failed");}
            else if(!warm){specific=L"阶段切换预热未通过";Logf("MOTIAN_GRAB_PREPARE warm transitions failed");}
            if(ok&&variant.event_kind==1){
                // This event has NO Success Timeline property. The explicit
                // post-hit recipe is proved on the actual source CDO instead.
                ok=SourceSuccessTimeline(prepared_.source_cdo.object,prepared_.success_timeline);
                if(!ok){specific=L"施技默认对象上没有 GrabSuccess 时间轴";Logf("MOTIAN_GRAB_PREPARE success timeline proof failed");}
            }
            if(ok&&(variant.source_policy.Any()||variant.victim_policy.Any())){
                ok=GrabStateLeases::Prepare(specific);
                if(!ok)LogPrepareReason("native state lease metadata",specific);
            }
            if(ok&&variant.huashan_domain_bridge){
                ok=GrabHuashanAdapter::Prepare(specific);
                if(!ok)LogPrepareReason("native DomainCenter bridge metadata",specific);
            }
            if(ok&&variant.guard_effect){
                Object* effect=ue::FindObject(variant.guard_effect);prepared_.guard_effect=Id(effect);
                ok=Same(prepared_.guard_effect)&&std::any_of(prepared_.assets.begin(),prepared_.assets.end(),[&](const ClassLease& lease){return lease.phase==RefPhase::Owned&&lease.identity.object==effect&&Same(lease.identity);});
                if(ok)ok=GrabEffectGuard::Prepare(effect,specific);
                if(!ok){if(specific.empty())specific=L"延迟效果守卫元数据未通过";LogPrepareReason("native deferred effect guard metadata",specific);}
            }
            if (ok) prepared_.ready=prepared_.contract_valid=true;
        }
    }
    QueryPerformanceCounter(&end);
    prepared_.last_ms=frequency.QuadPart?static_cast<float>(1000.*static_cast<double>(end.QuadPart-begin.QuadPart)/frequency.QuadPart):0;
    prepared_.total_ms+=prepared_.last_ms; prepared_.max_ms=(std::max)(prepared_.max_ms,static_cast<double>(prepared_.last_ms));
    if (!ok) {
        prepared_.failed=true;
        prepared_.status=specific.empty()?L"资源准备失败；本步骤未通过。请关闭后重开查看日志；未进入战斗触发。":specific;
    } else {
        ++prepared_.step;
        wchar_t summary[300]{};
        swprintf_s(summary,L"%s：%zu/%zu 步；累计 %.1f ms，最大单步 %.1f ms。%s",prepared_.ready?L"已预载":L"分步准备中",prepared_.step,total,prepared_.total_ms,prepared_.max_ms,prepared_.ready?L"可以重按三角 + 圈；空闲保留资产引用":L"请等已预载后重按，触发帧不加载");
        prepared_.status=summary;
    }
    Logf("MOTIAN_GRAB_PREPARE step=%zu/%zu label=%s ok=%d stepMs=%.3f assetRefs=%zu ready=%d lookup=%s",
        step,total,label,ok?1:0,prepared_.last_ms,prepared_.assets.size(),prepared_.ready?1:0,step<count?kPreparationMetadata[step]:"asset/name validation");
}
// The execution move shares the menu slot when both are the same move.
bool ExecutionShared() { return execution_variant_>=0&&execution_variant_==options_.variant; }
// Both slots advance one step per tick: the menu move while grabs are on, and
// MoTian's full grab while 黑伤 executions are redirected.
void PrepareSlots() {
    const bool redirect=options_.tang_execution&&execution_variant_>=0;
    run_variant_=options_.variant; AdvancePreparation(options_.enabled||(redirect&&ExecutionShared()));
    std::swap(prepared_,spare_); run_variant_=execution_variant_>=0?execution_variant_:options_.variant;
    AdvancePreparation(redirect&&!ExecutionShared());
    std::swap(prepared_,spare_); run_variant_=options_.variant;
}
// Run with prepared_ holding the execution move (swapped in until its session ends).
void UseExecutionSlot() {
    if (!slot_swapped_&&!ExecutionShared()) { std::swap(prepared_,spare_); slot_swapped_=true; }
    run_variant_=execution_variant_;
}
void RestoreSlot() {
    if (HasResources()) return;
    if (slot_swapped_) { std::swap(prepared_,spare_); slot_swapped_=false; }
    run_variant_=options_.variant;
}
bool SlotReady(int32_t variant) {
    const int32_t saved=run_variant_; bool ready=false; run_variant_=variant;
    if (prepared_.variant==variant) ready=PreparedReady();
    else if (spare_.variant==variant) { std::swap(prepared_,spare_); ready=PreparedReady(); std::swap(prepared_,spare_); }
    run_variant_=saved; return ready;
}
bool PointDistance(const Context& c,Object* target,double& distance,const Vector* known_player=nullptr) {
    Vector a=known_player?*known_player:Vector{},b; if ((!known_player&&!Position(c.pawn,a))||!Position(target,b)) return false;
    Object* own_capsule=Property<Object*>(c.pawn,"CapsuleComponent"),*other_capsule=Property<Object*>(target,"CapsuleComponent");
    Call r1(own_capsule,"GetScaledCapsuleRadius"),r2(other_capsule,"GetScaledCapsuleRadius");
    if (!r1.Invoke()||!r2.Invoke()) return false;
    const float r=r1.Get<float>("ReturnValue")+r2.Get<float>("ReturnValue");
    const double dx=a.x-b.x,dy=a.y-b.y,dz=a.z-b.z;
    distance=std::max(0.,std::sqrt(dx*dx+dy*dy)-r);
    if (!std::isfinite(distance) || std::fabs(dz)>120) return false;
    Call sight(c.controller,"LineOfSightTo");
    return sight.Set("Other",target).Set("ViewPoint",Vector{}).Set("bAlternateChecks",false).Invoke() && sight.Get<bool>("ReturnValue");
}
bool Info(Object* ability,CurrentInfo& info) {
    Call get(ability,"GetCurrentExecutionInfo","/Script/SGameExecution.ExecutionAbilityInterface");
    if (!get.Slot("Info",sizeof(info)) || !get.Invoke()) return false;
    info=get.Get<CurrentInfo>("Info"); return true;
}
bool PairInfo(Object* ability,bool is_target) {
    CurrentInfo info;
    return Info(ability,info) && Resolve(info.executor)==session_.pawn.object && Resolve(info.target)==session_.target.object &&
        (info.is_target!=0)==is_target;
}
bool Avatar(Object* ability,Object* actor) {
    Call get(ability,"GetAvatarActorFromActorInfo"); return get.Invoke() && get.Get<Object*>("ReturnValue")==actor;
}
bool ASCQuiescent(Object* asc) {
    // Exact Give/Unlock PE: a local moved-out pending batch is invisible in
    // both arrays while ASC+11f8 records it. Never infer retirement in that gap.
    return ue::Alive(asc)&&Offset(asc,"ActivatableAbilities")==1048&&Readable(asc,0x1208)&&
        ue::At<int32_t>(asc,0x578)==0&&ue::At<int32_t>(asc,0x1200)==0;
}
bool PendingArray(Object* asc,Array& array) {
    if(!ASCQuiescent(asc))return false;
    Object* secondary=ue::At<Object*>(asc,0x778);const int32_t num=ue::At<int32_t>(asc,0x780),max=ue::At<int32_t>(asc,0x784);
    if(!secondary&&max>2)return false;
    array={secondary?secondary:asc+0x598,num,max};return ArrayOk(array,240,1024);
}
bool PendingBaseline(Object* asc,std::vector<int32_t>& handles) {
    Array array;if(!PendingArray(asc,array))return false;
    for(int32_t i=0;i<array.num;++i){Object* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*240;
        const int32_t handle=ue::At<int32_t>(row,12);if(handle<=0||!ue::Alive(ue::At<Object*>(row,16)))return false;
        if(std::find(handles.begin(),handles.end(),handle)==handles.end())handles.push_back(handle);
    }
    return true;
}
bool PendingSpec(Object* asc,int32_t handle,const Identity& cdo,Spec& out,bool& found) {
    found=false;Array array;if(!PendingArray(asc,array))return false;
    for(int32_t i=0;i<array.num;++i){Object* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*240;if(ue::At<int32_t>(row,12)!=handle)continue;
        if(found||!Same(cdo)||ue::At<Object*>(row,16)!=cdo.object)return false;
        out.handle=handle;out.cdo=cdo;out.active=ue::At<uint8_t>(row,40);out.pending=true;found=true;
    }
    return true;
}
void CaptureHandles(const std::vector<Spec>& specs,std::vector<int32_t>& handles) {
    for (const Spec& spec:specs) handles.push_back(spec.handle);
}
bool Ended(const Identity& identity);
bool IsActive(Object* ability,bool& active);
bool SameIdentity(const Identity& a,const Identity& b) {
    return a.object==b.object && a.weak.index==b.weak.index && a.weak.serial==b.weak.serial && a.name==b.name;
}
void PairDiagnostics(Object* ability,bool victim,const char* phase) {
    CurrentInfo info; const bool got=Info(ability,info);
    Call avatar(ability,"GetAvatarActorFromActorInfo"); const bool avatar_ok=avatar.Invoke();
    Object* actual=avatar_ok?avatar.Get<Object*>("ReturnValue"):nullptr;
    Object* cached=Property<Object*>(ability,"Execution Component");
    Logf("MOTIAN_GRAB_PAIR phase=%s side=%s instance=%s avatarOk=%d avatar=%s expected=%s cachedComponent=%p expectedComponent=%p",
        phase,victim?"victim":"executor",Text(ability).c_str(),avatar_ok?1:0,Text(actual).c_str(),
        Text(victim?session_.target.object:session_.pawn.object).c_str(),cached,session_.execution.object);
    Logf("MOTIAN_GRAB_PAIR phase=%s infoOk=%d executorWeak=%d/%d executor=%s targetWeak=%d/%d target=%s role=%u handles=%d/%d expected=%d/%d",
        phase,got?1:0,info.executor.index,info.executor.serial,Text(Resolve(info.executor)).c_str(),info.target.index,info.target.serial,
        Text(Resolve(info.target)).c_str(),info.is_target,info.execution,info.executed,session_.source_handle,session_.victim_handle);
}
bool BindNativeGrants() {
    // Native PlayExact writes only the executor component. It passes the
    // victim Info directly to GEd; targetComponent.CurrentInfo is not a receipt.
    if (!Same(session_.execution)||Offset(session_.execution.object,"CurrentExecutionInfo")!=0x318||!Readable(session_.execution.object+0x318,sizeof(CurrentInfo))) {
        session_.uncertain=true; return false;
    }
    const CurrentInfo receipt=ue::At<CurrentInfo>(session_.execution.object,0x318);
    Logf("MOTIAN_GRAB_RECEIPT executor=%s target=%s role=%u handles=%d/%d component=%p",
        Text(Resolve(receipt.executor)).c_str(),Text(Resolve(receipt.target)).c_str(),receipt.is_target,receipt.execution,receipt.executed,session_.execution.object);
    if (Resolve(receipt.executor)!=session_.pawn.object||Resolve(receipt.target)!=session_.target.object||receipt.is_target) {
        session_.uncertain=true; return false;
    }
    session_.source_handle=receipt.execution; session_.victim_handle=receipt.executed;
    PhaseReceipt phase;phase.executor=session_.pawn;phase.target=session_.target;phase.execution=session_.execution;
    phase.source_asc=session_.source_asc;phase.target_asc=session_.target_asc;phase.source_class=session_.source_class;phase.victim_class=session_.victim_class;
    phase.info=receipt;phase.observed=GetTickCount64();
    session_.phases.push_back(std::move(phase));
    for (bool source:{true,false}) {
        const Identity& asc=source?session_.source_asc:session_.target_asc;
        const Identity& cls=source?session_.source_class:session_.victim_class;
        const auto& baseline=source?session_.source_before:session_.target_before;
        const int32_t handle=source?receipt.execution:receipt.executed;
        if (handle<=0) continue; // Native source CanActivate failure can still grant a victim.
        if (!Same(asc)||!Same(cls)||std::find(baseline.begin(),baseline.end(),handle)!=baseline.end()) { session_.uncertain=true; return false; }
        std::vector<Spec> specs;
        if (!Specs(asc.object,specs,handle)) { session_.uncertain=true; return false; }
        const auto found=std::find_if(specs.begin(),specs.end(),[&](const Spec& row){return row.handle==handle;});
        Spec pending;bool pending_found=false;const Spec* actual=found==specs.end()?nullptr:&*found;
        if(!actual&&(!session_.transitions.empty()||session_.deferred_grace_ms)){
            const Identity& cdo=source?prepared_.source_cdo:prepared_.victim_cdo;
            if(!PendingSpec(asc.object,handle,cdo,pending,pending_found)){session_.uncertain=true;return false;}
            if(pending_found)actual=&pending;
        }
        if (!actual) continue; // One-shot activation can synchronously End/remove itself.
        if (!Same(actual->cdo)||ue::ClassOf(actual->cdo.object)!=cls.object) { session_.uncertain=true; return false; }
        Grant grant{asc,actual->cdo,handle,actual->instances}; grant.native_receipt=true;
        grant.expected_avatar=source?session_.pawn:session_.target;grant.expected_info=receipt;grant.expected_info.is_target=source?0:1;grant.phase=0;
        session_.grants.push_back(std::move(grant));
        Logf("MOTIAN_GRAB_GRANT native-owned side=%s handle=%d activeCount=%u instances=%zu; success not yet admitted",
            source?"executor":"victim",handle,actual->active,actual->instances.size());
    }
    session_.receipts_bound=true; return true;
}
bool Admission(const char* reason) { admission_reason_=reason; return false; }
bool OwnedSpec(Grant& grant,Spec& out,bool& found) {
    found=false; if (!Same(grant.asc)) return true;
    if (!grant.native_receipt) return Admission("no_native_receipt");
    if (!Same(grant.cdo)) return Admission("ability_cdo_identity");
    if((!session_.transitions.empty()||session_.deferred_grace_ms)&&!ASCQuiescent(grant.asc.object))return Admission("asc_not_quiescent");
    Object* type=Type("/Script/GameplayAbilities.GameplayAbilitySpec"),*container=Type("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    if (!type||!container||ue::StructSize(type)!=240||Offset(grant.asc.object,"ActivatableAbilities")!=1048||ue::Offset(container,"Items")!=264||
        ue::Offset(type,"Handle")!=12||ue::Offset(type,"Ability")!=16||ue::Offset(type,"ActiveCount")!=40) return Admission("spec_layout");
    const Array array=ue::At<Array>(grant.asc.object,1048+264);
    if (!ArrayOk(array,240,4096)) { sprintf_s(reject_detail_,"num=%d max=%d",array.num,array.max); return Admission("spec_array"); }
    for (int32_t i=0;i<array.num;++i) {
        Object* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*240;
        if (ue::At<int32_t>(row,12)!=grant.handle) continue;
        if (ue::At<Object*>(row,16)!=grant.cdo.object) return Admission("spec_cdo_mismatch");
        found=true; out.handle=grant.handle; out.cdo=grant.cdo; out.active=ue::At<uint8_t>(row,40);
        for (const char* name:{"NonReplicatedInstances","ReplicatedInstances"}) {
            const int32_t offset=ue::Offset(type,name); if (offset<0) return Admission("instance_layout");
            const Array list=ue::At<Array>(row,offset); if (!ArrayOk(list,8,64)) return Admission("instance_array");
            for (int32_t j=0;j<list.num;++j) {
                Object* object=static_cast<Object**>(list.data)[j];
                const auto known=std::find_if(grant.instances.begin(),grant.instances.end(),[&](const Identity& id){return id.object==object&&Same(id);});
                const Identity id=known==grant.instances.end()?Id(object):*known;
                if (!Same(id)) continue;
                if (known==grant.instances.end()) grant.instances.push_back(id);
                if (std::none_of(out.instances.begin(),out.instances.end(),[&](const Identity& old){return SameIdentity(old,id);})) out.instances.push_back(id);
            }
        }
        return true;
    }
    return session_.transitions.empty()&&!session_.deferred_grace_ms?true:PendingSpec(grant.asc.object,grant.handle,grant.cdo,out,found);
}
bool DiscoverGrants(bool source) {
    // Monitoring validates only immutable native-receipt handles; no new
    // same-class grant can be admitted after PlayExact returns.
    if (!session_.receipts_bound||session_.uncertain) return Admission("receipts_unbound_or_uncertain");
    for (Grant& grant:session_.grants) {
        if (grant.asc.object!=(source?session_.source_asc.object:session_.target_asc.object)) continue;
        Spec spec; bool found=false; if (!OwnedSpec(grant,spec,found)) return false;
        if (!found) continue;
        for (const Identity& instance:spec.instances) {
            if (Ended(instance)) continue;
            bool ending=false,active=false;
            if (!Bool(instance.object,"bIsAbilityEnding",ending)||!IsActive(instance.object,active)) return Admission("instance_state_read");
            if (ending||!active) continue;
            CurrentInfo info;
            const bool roles=Same(grant.expected_avatar)&&Avatar(instance.object,grant.expected_avatar.object)&&Info(instance.object,info)&&
                Resolve(info.executor)==Resolve(grant.expected_info.executor)&&Resolve(info.target)==Resolve(grant.expected_info.target)&&
                (info.is_target!=0)==(grant.expected_info.is_target!=0)&&info.execution==grant.expected_info.execution&&info.executed==grant.expected_info.executed;
            if (!roles) {
                if (grant.last_state!=6) PairDiagnostics(instance.object,grant.expected_info.is_target!=0,"strict-admission-failed");
                grant.last_state=6; return Admission("roles_mismatch"); // Owned, cancellable; mutable Info is not ownership uncertainty.
            }
        }
    }
    return true;
}
bool IsActive(Object* ability,bool& active) {
    Call get(Default("/Script/SGameUtils.Default__AbilityUtils"),"IsAbilityActive");
    if (!get.Set("Ability",ability).Invoke()) return false; active=get.Get<bool>("ReturnValue"); return true;
}
bool Ended(const Identity& identity) {
    if (!Same(identity)) return Resolve(identity.weak)==nullptr;
    bool active=true,ending=true;
    if (!Bool(identity.object,"bIsActive",active)||!Bool(identity.object,"bIsAbilityEnding",ending)||active||ending||Offset(identity.object,"ActiveTasks")<0) return false;
    const Array tasks=Property<Array>(identity.object,"ActiveTasks"); return ArrayOk(tasks,8,1024)&&tasks.num==0;
}
bool PairReceipt(const CurrentInfo& a,const CurrentInfo& b,bool role) {
    return a.executor.index==b.executor.index&&a.executor.serial==b.executor.serial&&a.target.index==b.target.index&&a.target.serial==b.target.serial&&
        a.execution==b.execution&&a.executed==b.executed&&(a.is_target!=0)==role&&std::isfinite(a.start)&&std::fabs(a.start-b.start)<.001f;
}
bool BrokerTime(bool& advancing) {
    advancing=false;if(session_.transitions.empty()&&!session_.deferred_grace_ms)return true;
    if(!Same(session_.world))return true;
    // Exact reflected ABI is double (not float). World time advances only on
    // world ticks; repeated Cleanup/BeforeTravel calls and paused wall gaps
    // must not count as a completed DelayNextTick callback opportunity.
    Call time(Default("/Script/Engine.Default__GameplayStatics"),"GetTimeSeconds");
    if(!time.Slot("WorldContextObject",8)||!time.Slot("ReturnValue",8)||!time.Set("WorldContextObject",session_.world.object).Invoke())return false;
    const double now=time.Get<double>("ReturnValue");if(!std::isfinite(now)||now<0)return false;
    if(!session_.broker_time_initialized){session_.broker_world_time=now;session_.broker_time_initialized=true;return true;}
    if(now<session_.broker_world_time){session_.broker_world_time=now;session_.settle_since=0;session_.settle_frames=0;session_.settle_last_step=session_.broker_steps;return true;}
    if(now>session_.broker_world_time){
        session_.broker_elapsed+=now-session_.broker_world_time;session_.broker_world_time=now;
        session_.broker_clock=static_cast<ULONGLONG>(std::floor(session_.broker_elapsed*1000.));
        ++session_.broker_steps;advancing=true;
    }
    return true;
}
bool CandidateSpec(const Identity& asc,const Identity& cls,const Identity& cdo,int32_t handle,Spec& out,bool& present) {
    present=false;if(!Same(asc)||!Same(cls)||!Same(cdo)||!ASCQuiescent(asc.object))return false;
    std::vector<Spec> rows;if(!Specs(asc.object,rows,handle))return false;
    Object* type=Type("/Script/GameplayAbilities.GameplayAbilitySpec"),*field=type?Field(type,"SourceObject"):nullptr;
    if(!field||ue::Offset(type,"SourceObject")!=32||ue::At<int32_t>(field,0x34)!=8||!Readable(ue::At<Object*>(field,8),0x20)||ue::At<uint64_t>(ue::At<Object*>(field,8),0x18)!=0x8000000)return false;
    const auto null_source=[&](const Array& array){for(int32_t i=0;i<array.num;++i){Object* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*240;
        if(ue::At<int32_t>(row,12)==handle)return ue::At<uint64_t>(row,32)==0;}return false;};
    if(!rows.empty()){
        if(rows.size()!=1||!Same(rows[0].cdo)||rows[0].cdo.object!=cdo.object||ue::ClassOf(cdo.object)!=cls.object)return false;
        if(!null_source(ue::At<Array>(asc.object,1048+264)))return false;
        out=std::move(rows[0]);present=true;return true;
    }
    if(!PendingSpec(asc.object,handle,cdo,out,present))return false;
    if(present){Array pending;if(!PendingArray(asc.object,pending)||!null_source(pending))return false;}
    return true;
}
bool ExclusiveExecution(const Identity& asc,int32_t candidate,bool wait_tasks=false) {
    std::vector<Spec> rows;if(!Same(asc)||!ASCQuiescent(asc.object)||!Specs(asc.object,rows,0,true))return false;
    const auto own=[&](int32_t handle){return handle==candidate||std::any_of(session_.grants.begin(),session_.grants.end(),[&](const Grant& g){return SameIdentity(g.asc,asc)&&g.handle==handle;});};
    for(const Spec& row:rows)if(row.active&&!own(row.handle)&&ExecutionBusy(std::vector<Spec>{row}))return false;
    if(wait_tasks)for(const Spec& row:rows)if(!own(row.handle)&&!row.active){
        Spec probe=row;probe.active=1;if(!ExecutionBusy(std::vector<Spec>{probe}))continue;
        // Count zero can precede native End/tasks retirement. Observe those
        // foreign instances read-only; never adopt, cancel, or clear them.
        std::vector<Spec> full;if(!Specs(asc.object,full,row.handle)||full.size()!=1)return false;
        for(const Identity& instance:full.front().instances)if(!Ended(instance))return false;
    }
    Array pending;if(!PendingArray(asc.object,pending))return false;
    for(int32_t i=0;i<pending.num;++i){Object* row=static_cast<Object*>(pending.data)+static_cast<size_t>(i)*240;
        const int32_t handle=ue::At<int32_t>(row,12);if(own(handle))continue;
        Spec spec;spec.handle=handle;spec.cdo.object=ue::At<Object*>(row,16);spec.active=1;
        if(ExecutionBusy(std::vector<Spec>{spec}))return false;
    }
    return true;
}
bool FreshHandle(const Identity& asc,int32_t handle) {
    const auto& before=asc.object==session_.source_asc.object?session_.source_before:session_.target_before;
    return handle>0&&std::find(before.begin(),before.end(),handle)==before.end()&&
        std::none_of(session_.grants.begin(),session_.grants.end(),[&](const Grant& g){return g.handle==handle;});
}
bool OldPhaseRetiring(uint32_t phase) {
    for(Grant& grant:session_.grants)if(grant.phase==phase){
        Spec spec;bool present=false;if(!OwnedSpec(grant,spec,present)||(present&&spec.pending))return false;
        bool observed=false;
        for(const Identity& instance:grant.instances)if(Same(instance)){
            bool active=false,ending=false;if(!IsActive(instance.object,active)||!Bool(instance.object,"bIsAbilityEnding",ending))return false;
            if(active&&!ending)return false;observed=true;
        }
        if(present&&spec.active&&!observed)return false;
    }
    return true;
}
bool PollTransitions() {
    if(session_.transitions.empty()||!session_.began||!session_.receipts_bound)return true;
    bool advancing=false;if(!BrokerTime(advancing))return false;
    if(!Same(session_.pawn)||!Same(session_.target)||!Same(session_.world)||!Same(session_.source_asc)||!Same(session_.target_asc))return true;
    if(!ASCQuiescent(session_.source_asc.object)||!ASCQuiescent(session_.target_asc.object))return false;
    // Native clears phase0 and performs the reversed PlayExact in one callback.
    // Observe only this root recipe's single audited transition, never grant it.
    for(size_t ti=0;ti<session_.transitions.size();++ti){const PreparedTransition& t=session_.transitions[ti];
        if(t.from_phase!=session_.phase||session_.phases.empty()||!session_.phases.back().admitted)continue;
        const Identity& executor=t.reverse?session_.target:session_.pawn;
        const Identity& target=t.reverse?session_.pawn:session_.target;
        const Identity& ec=t.reverse?session_.target_execution:session_.execution;
        const Identity& source_asc=t.reverse?session_.target_asc:session_.source_asc;
        const Identity& victim_asc=t.reverse?session_.source_asc:session_.target_asc;
        if(!Same(ec)||Offset(ec.object,"CurrentExecutionInfo")!=0x318||!Readable(ec.object+0x318,sizeof(CurrentInfo)))return false;
        const CurrentInfo info=ue::At<CurrentInfo>(ec.object,0x318);
        if(Resolve(info.executor)!=executor.object||Resolve(info.target)!=target.object||info.is_target||!std::isfinite(info.start)||std::fabs(info.start)>.001f)continue;
        Spec source,victim;bool has_source=false,has_victim=false;
        if(info.execution<=0||info.executed<=0)continue;
        // A previous cast can leave a stale component Info. It has no live
        // rows, is not ours, and is never adopted merely by matching classes.
        if(!CandidateSpec(source_asc,t.source,t.source_cdo,info.execution,source,has_source)||
            !CandidateSpec(victim_asc,t.victim,t.victim_cdo,info.executed,victim,has_victim))return false;
        if(!has_source&&!has_victim)continue;
        if(!FreshHandle(source_asc,info.execution)||!FreshHandle(victim_asc,info.executed)||info.execution==info.executed||
            !ExclusiveExecution(source_asc,info.execution)||!ExclusiveExecution(victim_asc,info.executed))return false;
        // Native End may leave old tasks alive while the next pair is already
        // playing. Keep both ledgers; only require the old activations retire.
        if(!OldPhaseRetiring(static_cast<uint32_t>(t.from_phase)))return true;
        PhaseReceipt phase;phase.executor=executor;phase.target=target;phase.execution=ec;phase.source_asc=source_asc;phase.target_asc=victim_asc;
        phase.source_class=t.source;phase.victim_class=t.victim;phase.info=info;phase.definition=static_cast<uint32_t>(ti);phase.observed=session_.broker_clock;
        session_.phases.push_back(phase);session_.pending_transition=static_cast<int32_t>(ti);session_.transition_since=session_.broker_clock;
        const uint32_t phase_index=static_cast<uint32_t>(session_.phases.size()-1);
        for(bool src:{true,false}){const Spec& spec=src?source:victim;if(!(src?has_source:has_victim))continue;
            Grant grant{src?source_asc:victim_asc,spec.cdo,spec.handle,spec.instances};grant.native_receipt=true;
            grant.expected_avatar=src?executor:target;grant.expected_info=info;grant.expected_info.is_target=src?0:1;grant.phase=phase_index;
            session_.grants.push_back(std::move(grant));
        }
        Logf("MOTIAN_GRAB_PHASE observed exact reversed native receipt phase=%u handles=%d/%d pending=%d/%d",phase_index,info.execution,info.executed,source.pending?1:0,victim.pending?1:0);
        break;
    }
    if(session_.pending_transition<0)return true;
    PhaseReceipt& phase=session_.phases.back();const auto& t=session_.transitions[static_cast<size_t>(session_.pending_transition)];
    if(!Same(phase.execution)||!PairReceipt(ue::At<CurrentInfo>(phase.execution.object,0x318),phase.info,false))return false;
    // A native Give can register either side later. Revisit the immutable
    // handles, not an open class scan, on every bounded pending observation.
    for(bool src:{true,false}){
        const Identity& asc=src?phase.source_asc:phase.target_asc;const Identity& cls=src?phase.source_class:phase.victim_class;
        const Identity& cdo=src?t.source_cdo:t.victim_cdo;const int32_t handle=src?phase.info.execution:phase.info.executed;
        if(std::any_of(session_.grants.begin(),session_.grants.end(),[&](const Grant& g){return SameIdentity(g.asc,asc)&&g.handle==handle;}))continue;
        Spec spec;bool present=false;if(!CandidateSpec(asc,cls,cdo,handle,spec,present))return false;if(!present)continue;
        if(!FreshHandle(asc,handle)||!ExclusiveExecution(asc,handle))return false;
        Grant grant{asc,spec.cdo,handle,spec.instances};grant.native_receipt=true;grant.expected_avatar=src?phase.executor:phase.target;
        grant.expected_info=phase.info;grant.expected_info.is_target=src?0:1;grant.phase=static_cast<uint32_t>(session_.phases.size()-1);session_.grants.push_back(std::move(grant));
    }
    Object* source_instance=nullptr,*victim_instance=nullptr;
    for(Grant& grant:session_.grants)if(grant.phase==session_.phases.size()-1){
        Spec spec;bool present=false;if(!OwnedSpec(grant,spec,present))return false;
        if(!present||spec.pending)continue;
        for(const Identity& instance:spec.instances)if(Same(instance)&&!Ended(instance)){
            bool active=false;if(!IsActive(instance.object,active))return false;if(!active)continue;
            if(!Avatar(instance.object,grant.expected_avatar.object))return false;
            Call source_object(instance.object,"GetCurrentSourceObject");if(!source_object.Slot("ReturnValue",8)||!source_object.Invoke()||source_object.Get<Object*>("ReturnValue"))return false;
            Object*& selected=grant.expected_info.is_target?victim_instance:source_instance;
            if(selected&&selected!=instance.object)return false;selected=instance.object;
        }
    }
    if(source_instance&&victim_instance){
        if(!phase.refreshed){if(Property<Object*>(source_instance,"Execution Component")!=phase.execution.object||!Call(source_instance,"InitializeExecutionInfo").Invoke())return false;phase.refreshed=true;}
        CurrentInfo source,victim;
        if(!Info(source_instance,source)||!Info(victim_instance,victim)||!PairReceipt(source,phase.info,false)||!PairReceipt(victim,phase.info,true))return false;
        phase.admitted=true;session_.phase=static_cast<int32_t>(session_.phases.size()-1);session_.pending_transition=-1;
        session_.settle_since=0;session_.settle_frames=0;session_.max_duration_ms=(std::max)(session_.max_duration_ms,t.max_duration_ms);
        Status(L"已核定原生反制二阶段：双方角色交换，结束后统一恢复本次状态");
        return true;
    }
    if(advancing&&session_.broker_clock-session_.transition_since>t.grace_ms){
        Status(L"原生反制阶段未形成完整双能力，已保留精确句柄并请求取消");return false;
    }
    return true;
}
bool LeasedActorsIdle() {
    if(!session_.state_lease.HasPending()&&!session_.huashan_lease.HasPending()&&!session_.effect_guard.HasPending())return true;
    for(const Identity& actor:{session_.pawn,session_.target})if(Same(actor)&&Same(session_.world)&&InWorld(actor.object,session_.world.object)){
        Call get(Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"),"GetAbilitySystemComponent");
        if(!get.Set("Actor",actor.object).Invoke())return false;
        Object* asc=get.Get<Object*>("ReturnValue");if(!ue::Alive(asc))continue;
        // Unclaimed foreign counter/third executions remain untouched. They
        // also delay restoring component state leased by this older session.
        const Identity identity=Id(asc);if(!Same(identity)||!ExclusiveExecution(identity,0,true))return false;
    }
    return true;
}
bool DeferredSettled() {
    if(session_.transitions.empty()&&!session_.deferred_grace_ms)return true;
    // The old TimerManager can still resolve actors migrated to another world.
    // Keep its raw effect-class/counter assets pinned while its world and any
    // captured actor generation survive, even if InWorld(old) is now false.
    if(!Same(session_.world)||(!Same(session_.pawn)&&!Same(session_.target)))return true;
    bool advancing=false;if(!BrokerTime(advancing))return false;
    if(!session_.settle_since){session_.settle_since=session_.broker_clock+1;session_.settle_frames=0;session_.settle_last_step=session_.broker_steps;}
    if(session_.broker_steps>session_.settle_last_step){++session_.settle_frames;session_.settle_last_step=session_.broker_steps;}
    uint32_t grace=session_.deferred_grace_ms;for(const auto& t:session_.transitions)grace=(std::max)(grace,t.grace_ms);
    return session_.broker_clock+1-session_.settle_since>=grace&&session_.settle_frames>=2;
}
bool RetireForeignAdapterEpochs(std::wstring& reason) {
    const bool world=Same(session_.world);
    const bool source=world&&Same(session_.pawn)&&InWorld(session_.pawn.object,session_.world.object);
    const bool victim=world&&Same(session_.target)&&InWorld(session_.target.object,session_.world.object);
    if(!source)GrabStateLeases::Retire(session_.state_lease.source);
    if(!victim)GrabStateLeases::Retire(session_.state_lease.victim);
    // Hua's own World/GU guard retires its old warp record without touching a
    // newly owned world's component. Invoke only after old native tasks ended.
    if(!victim&&!GrabHuashanAdapter::Finalize(session_.huashan_lease,reason,true))return false;
    return true;
}
bool VerifySourceWalkingEnd() {
    if(!session_.verify_source_end_walking)return true;
    if(!Same(session_.pawn)||!Same(session_.world)||!InWorld(session_.pawn.object,session_.world.object))return true;
    Call dead(Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");if(!dead.Set("Pawn",session_.pawn.object).Invoke())return false;
    if(dead.Get<bool>("ReturnValue"))return true;
    Object* set=Property<Object*>(session_.pawn.object,"AttributeSetBase"),*data=Type("/Script/GameplayAbilities.GameplayAttributeData");
    const int32_t health=Offset(set,"Health");
    if(!ue::Alive(set)||health<0||!data||ue::StructSize(data)!=16||ue::Offset(data,"CurrentValue")!=12||!Readable(set+health,16))return false;
    const float hp=ue::At<float>(set,health+12);if(!std::isfinite(hp))return false;if(hp<=0)return true;
    if(!Same(session_.source_movement)||Property<Object*>(session_.pawn.object,"CharacterMovement")!=session_.source_movement.object)return true;
    uint8_t mode=0;Object* field=Field(ue::ClassOf(session_.source_movement.object),"MovementMode");
    if(!field||ue::At<int32_t>(field,0x34)!=1)return false;
    const int32_t offset=ue::At<int32_t>(field,0x44);if(offset<0||!Readable(session_.source_movement.object+offset,1))return false;
    mode=ue::At<uint8_t>(session_.source_movement.object,offset);
    if(mode!=1){Status(L"原生投技移动模式尚未回到 Walking；保留记录，等待原生落地收尾");return false;}
    Vector position;return Position(session_.pawn.object,position)&&FloorAt(session_.source_movement.object,position);
}
bool Cleanup(bool cancel) {
    stage_="cleanup";
    if (!session_.began) {
        const bool released=ReleaseClasses(); cleanup_pending_=!released;
        if (released) { session_={}; ResetKeys(); }
        return released;
    }
    PhaseTimer timer(1);
    cleanup_pending_=true;
    // Even cancel/travel must first observe a QTE callback already dispatched
    // by the engine. It can introduce new exact grants in reversed roles.
    if(!PollTransitions()&&!session_.transitions.empty())cancel=true;
    if (!Same(session_.source_asc) && !Same(session_.target_asc)) {
        if(session_.state_lease.HasPending()||session_.huashan_lease.HasPending()||session_.effect_guard.HasPending()){
            // A retired ASC is not evidence that a still-live actor's native
            // tasks ended. Retain the lease; do not cancel through a dead ASC.
            for(const Grant& grant:session_.grants)for(const Identity& instance:grant.instances)if(!Ended(instance))return false;
            if(!DeferredSettled())return false;
            std::wstring epoch_reason;if(!RetireForeignAdapterEpochs(epoch_reason))return false;
            if(!LeasedActorsIdle())return false;
            std::wstring reason;
            if(!GrabHuashanAdapter::Finalize(session_.huashan_lease,reason,true)||!GrabStateLeases::VerifyOrRestore(session_.state_lease,reason,true)){
                Status(L"%s",reason.c_str());return false;
            }
            if(!GrabEffectGuard::Verify(session_.effect_guard,reason,true,true)){Status(L"%s",reason.c_str());return false;}
            if(!VerifySourceWalkingEnd())return false;
        }
        if (!ReleaseClasses()) return false;
        GrabExtras::Finish(false);
        session_={}; cleanup_pending_=false; ResetKeys(); return true;
    }
    // Ownership was captured exactly once after native activation. Cleanup
    // never re-admits from mutable GA Info and never scans unrelated skills.
    if (!session_.receipts_bound||session_.uncertain) {
        if (session_.cleanup_state!=9) Logf("MOTIAN_GRAB_END receipt proof unavailable/uncertain; retained without repeated admission or native cancellation");
        session_.cleanup_state=9; return false;
    }
    bool waiting_native=false;
    for (Grant& grant:session_.grants) {
        Spec spec; bool present=false;
        if (!OwnedSpec(grant,spec,present)) {
            if (grant.last_state!=8) Logf("MOTIAN_GRAB_END owned handle=%d identity/layout changed; no cancellation or Clear",grant.handle);
            grant.last_state=8; return false;
        }
        bool all_ended=true; const ULONGLONG now=GetTickCount64();
        if(present&&spec.pending){
            if(!cancel)return false;
            if(!ASCQuiescent(grant.asc.object)||!Call(grant.asc.object,"ClearAbility").Set("Handle",grant.handle).Invoke())return false;
            Spec after;bool remains=false;if(!OwnedSpec(grant,after,remains)||remains)return false;
            present=false;
        }
        const bool cancel_round=cancel&&grant.cancel_rounds<4&&now>=grant.next_cancel;
        bool attempted=false;
        for (const Identity& instance:grant.instances) {
            if (!Same(instance)) continue;
            bool active=false,ending=false;
            if (!IsActive(instance.object,active)||!Bool(instance.object,"bIsAbilityEnding",ending)) return false;
            if (active&&!ending&&cancel_round&&Same(grant.asc)) {
                if (!attempted) {
                    attempted=true; ++grant.cancel_rounds; grant.next_cancel=now+(1000ull<<(grant.cancel_rounds-1));
                }
                Call(instance.object,"K2_CancelAbility").Invoke();
            }
            all_ended=Ended(instance)&&all_ended;
        }
        if (!all_ended||(present&&spec.active)) {
            const int state=grant.cancel_rounds>=4?4:1;
            if (grant.last_state!=state) Logf("MOTIAN_GRAB_END waiting handle=%d activeCount=%u cancelRounds=%u; native End required, no pair re-admission",
                grant.handle,present?spec.active:0,grant.cancel_rounds);
            grant.last_state=state;waiting_native=true;continue;
        }
        grant.last_state=2;
    }
    if(waiting_native)return false;
    if(!DeferredSettled())return false;
    std::wstring adapter_reason;
    if(!RetireForeignAdapterEpochs(adapter_reason))return false;
    if(!LeasedActorsIdle()){Status(L"检测到未归属本次投技的原生执行；保留状态记录，等待其结束后恢复");return false;}
    if(!GrabHuashanAdapter::Finalize(session_.huashan_lease,adapter_reason,true)||!GrabStateLeases::VerifyOrRestore(session_.state_lease,adapter_reason,true)){
        Status(L"%s",adapter_reason.c_str());return false;
    }
    if(!GrabEffectGuard::Verify(session_.effect_guard,adapter_reason,true,true)){Status(L"%s",adapter_reason.c_str());return false;}
    if(!VerifySourceWalkingEnd())return false;
    if (!VerifyCleanupProof(session_.source_proof)||!VerifyCleanupProof(session_.target_proof)) {
        if (session_.cleanup_state!=3) Logf("MOTIAN_GRAB_END native End completed but tags/capsule baseline pending; no raw writes");
        session_.cleanup_state=3; return false;
    }
    for (size_t i=session_.grants.size();i-- >0;) {
        Grant& grant=session_.grants[i];
        Spec spec; bool present=false; if (!OwnedSpec(grant,spec,present)) return false;
        if (present) {
            if (spec.active||!Call(grant.asc.object,"ClearAbility").Set("Handle",grant.handle).Invoke()) return false;
            Spec after; bool remains=false; if (!OwnedSpec(grant,after,remains)||remains) return false;
        }
        session_.grants.erase(session_.grants.begin()+i);
    }
    Logf("MOTIAN_GRAB cleanup verified; only new exact source/target grant handles retired");
    if (!ReleaseClasses()) return false;
    GrabExtras::Finish(session_.paired&&(!cancel||session_.source_completed));
    session_={}; cleanup_pending_=false; ResetKeys(); stage_="idle"; return true;
}
void Publish(bool ready=false) {
    AcquireSRWLockExclusive(&state_lock); state_.options=options_; state_.ready=ready&&PreparedReady();
    state_.prepared=options_.enabled&&PreparedReady(); state_.preparing=options_.enabled&&!prepared_.ready&&!prepared_.failed&&!prepared_.uncertain;
    state_.prepared_asset_refs=static_cast<uint32_t>(prepared_.assets.size()); state_.preparation_last_ms=prepared_.last_ms; state_.preparation_status=prepared_.status;
    state_.active=HasResources(); state_.paired=session_.paired; state_.cleanup_pending=cleanup_pending_||prepared_.uncertain;
    state_.retained_classes=static_cast<uint32_t>(class_leases_.size());
    state_.disabled=InterlockedCompareExchange(&disabled_,0,0)!=0;
    state_.execution_prepared=options_.tang_execution&&SlotReady(execution_variant_);
    state_.target=Same(session_.target)?Wide(Text(session_.target.object)):L"";
    state_.ability=Same(session_.source_class)?Wide(Text(session_.source_class.object)):L"";
    ReleaseSRWLockExclusive(&state_lock);
}
bool HuaLanding(Object* source,Object* victim,const GrabHuashanAdapter::Vector& raw,std::wstring& reason) {
    Context c;Vector origin;float half=0,radius=0;double slope=0;
    const Vector point{raw.x,raw.y,raw.z};
    if(!ContextNow(c)||c.pawn!=source||!InWorld(victim,c.world)||!Position(victim,origin)||Separation(origin,point)>800.01||
        !CapsuleSize(victim,half,radius)||!WalkableSlope(Property<Object*>(victim,"CharacterMovement"),slope)){
        reason=L"场地投摔落点与本次角色、距离或胶囊状态不符";return false;
    }
    FloorIgnore ignore;if(!ignore.Load(source,source,victim)){reason=L"场地投摔无法核定当前场景地面";return false;}
    Object* pair[]={source,victim};const Array pair_ignore{pair,2,2};
    // Assets do not establish whether raw Z denotes feet or capsule center.
    // Preserve it; use independently verified floor/clearance hypotheses only.
    for(double expected:{raw.z,raw.z-half}){Vector floor,normal;
        if(!GroundAt(source,point,expected,ignore.pawns,slope,floor,normal))continue;
        const Vector center{raw.x,raw.y,floor.z+half+radius*(1./normal.z-1.)+4.};
        if(CapsuleClear(source,center,half,radius,pair_ignore))return true;
    }
    reason=L"场地投摔原始落点没有可验证的安全地面与胶囊空间";return false;
}
// The grab may start from hit-stun or a knock-down: the player's own hit-react /
// fall / get-up abilities are cancelled first (K2_CancelAbility, the ability's
// own end path) so they do not block or fight the executor's montage.
void CancelOwnReactions(const Context& c) {
    std::vector<Spec> specs; if (!Specs(c.asc,specs,0,false)) return;
    uint32_t cancelled=0;
    for (const Spec& spec:specs) if (spec.active) for (const Identity& instance:spec.instances) if (Same(instance)) {
        char name[160]{}; ue::NameText(ue::NameOf(ue::ClassOf(instance.object)),name,sizeof(name));
        for (char* at=name;*at;++at) *at=static_cast<char>(std::tolower(static_cast<unsigned char>(*at)));
        if (!std::strstr(name,"hitreact")&&!std::strstr(name,"hit_react")&&!std::strstr(name,"falldown")&&!std::strstr(name,"knockdown")&&
            !std::strstr(name,"getup")&&!std::strstr(name,"get_up")&&!std::strstr(name,"stagger")&&!std::strstr(name,"stun")) continue;
        bool active=false; if (!IsActive(instance.object,active)||!active) continue;
        if (Call(instance.object,"K2_CancelAbility").Invoke()) ++cancelled;
    }
    if (cancelled) Logf("MOTIAN_GRAB cancelled %u own hit-react / knock-down abilities before the grab",cancelled);
}
bool StartAttempt(const Context& c) {
    StartSegments timing;
    stage_="target gate";
    if (session_.began || !InputBaseAllowed(c) || !InputAllowed(c)) return false;
    if (!ExecutionContext(c.pawn,c.execution,c.asc)) { Status(L"主角原执行组件 / ActorConfig / ASC 未就绪，未施放"); return false; }
    const VariantData& variant=kVariants[run_variant_];
    if (!PreparedReady()) { Status(L"当前招式资源尚未预载；请等菜单显示已预载后重按三角 + 圈"); return false; }
    Object* source=prepared_.source.object;
    if (!AcquireClass(source)) { Status(L"无法安全保活原投技类，未施放"); return false; }
    Object* victim=prepared_.victim.object;
    if (!AcquireClass(victim)) { Status(L"无法安全保活配对受投类，未施放"); return false; }
    Object* bullet=prepared_.bullet.object;
    Object* event=prepared_.event.object;
    if (!source || !victim || !bullet || !event || !Same(prepared_.source_cdo)||!Same(prepared_.victim_cdo)) {
        Status(L"投技原能力 / 成对受投 / BulletEvent / 实例化策略资料未加载完整，未施放"); return false;
    }
    Object* anim_type=Type("/Script/SGameExecution.ExecutionAnimData"),*info_type=Type("/Script/SGameExecution.CurrentExecutionInfo");
    if (!anim_type || ue::StructSize(anim_type)!=24 || ue::Offset(anim_type,"ExecutionAbility")!=0 || ue::Offset(anim_type,"ExecutedAbility")!=8 ||
        ue::Offset(anim_type,"SourceObject")!=16 || !info_type || ue::StructSize(info_type)!=32 || ue::Offset(info_type,"Executor")!=0 ||
        ue::Offset(info_type,"Target")!=8 || ue::Offset(info_type,"IsTarget")!=20 || ue::Offset(info_type,"ExecutionHandle")!=24 || ue::Offset(info_type,"ExecutedHandle")!=28) {
        Status(L"原生 ExecutionInfo 布局与当前模块不符，未施放"); return false;
    }
    std::vector<Spec> own_specs; if (!Specs(c.asc,own_specs,0,true)||ExecutionBusy(own_specs)) return false;
    timing.Next(1);
    // A redirected execution names its target; the move's own range then applies.
    Object* target=nullptr,*target_asc=nullptr,*target_execution=nullptr,*near_boss=nullptr;
    double best=forced_target_?static_cast<double>(variant.max_range):options_.range;
    const char* near_reason=""; char near_detail[200]{};
    // The closest listed enemy that was refused, so a failed grab says why.
    Object* refused=nullptr; double refused_distance=1e30; const char* refused_reason=""; char refused_detail[200]{};
    Object* pawn_class=Type("/Script/Engine.Pawn");
    FloorIgnore scene_pawns;
    if (!scene_pawns.Load(c.pawn,c.pawn,nullptr)) return false;
    Vector player_position; if (!Position(c.pawn,player_position)) return false;
    const auto consider=[&](Object* actor){
        if (!ue::Alive(actor)||ue::IsTemplate(actor)||!pawn_class||!ue::IsA(actor,pawn_class)||!InWorld(actor,c.world)) return true;
        const Vector a=player_position; Vector b; if (!Position(actor,b)) return true;
        const double dx=a.x-b.x,dy=a.y-b.y,dz=a.z-b.z;const double center_limit=std::min(3400.,best+200.);
        if (dx*dx+dy*dy+dz*dz>center_limit*center_limit) return true;
        if(options_.position_correction&&variant.alignment_mode==0&&std::fabs(std::hypot(dx,dy)-variant.authored_distance)>450)return true;
        if (actor!=c.pawn && InWorld(actor,c.world) && ClassListed(actor,kDeniedBossClasses,std::size(kDeniedBossClasses))) near_boss=actor;
        Object* asc=nullptr,*execution=nullptr; double distance=0; bool boss=false;
        bool eligible=Enemy(actor,c,asc,execution,boss);
        eligible=eligible&&OriginalGrabTags(event,asc,!boss||GrabExtras::GetOptions().ignore_invulnerable||redirect_attempt_);
        if (eligible && !(PointDistance(c,actor,distance,&player_position) && distance<=best)) eligible=Reject("distance_or_visibility");
        if (eligible) { best=distance; target=actor; target_asc=asc;target_execution=execution; }
        if (actor==near_boss) { near_reason=reject_reason_; strcpy_s(near_detail,reject_detail_); }
        if (!eligible&&std::strcmp(reject_reason_,"class_not_listed")&&std::strcmp(reject_reason_,"not_in_world")&&dx*dx+dy*dy<refused_distance) {
            refused=actor; refused_distance=dx*dx+dy*dy; refused_reason=reject_reason_; strcpy_s(refused_detail,reject_detail_);
        }
        return true;
    };
    if (forced_target_) consider(forced_target_);
    else for (int32_t i=0;i<scene_pawns.pawns.num;++i) consider(static_cast<Object**>(scene_pawns.pawns.data)[i]);
    if (!target&&refused)
        Logf("MOTIAN_GRAB target rejected nearest=%s distance=%.0f reason=%s detail=%s forced=%d",Text(refused).c_str(),std::sqrt(refused_distance),
            refused_reason[0]?refused_reason:"unknown",refused_detail,forced_target_?1:0);
    if (!target) {
        if (near_boss) {
            Status(options_.allow_bosses?L"附近 Boss 未通过人形体型、受投状态、原投技标签、距离或可见性检查":L"人形 Boss 投技选项已关闭；可在投技菜单开启");
            Logf("MOTIAN_GRAB boss target rejected=%s allow_bosses=%d ignore_invulnerable=%d reason=%s detail=%s",Text(near_boss).c_str(),
                options_.allow_bosses?1:0,GrabExtras::GetOptions().ignore_invulnerable?1:0,near_reason[0]?near_reason:"unknown",near_detail);
        } else Status(L"近处没有可受投的人形敌人（队伍 / 生命 / 可见性 / 受投条件 / 距离）");
        return false;
    }
    timing.Next(2);
    if(variant.requires_qte_task_owner){
        if(!QteReceiverReady(target)){
            Status(L"原生 QTE 受技者缺少有效任务拥有组件，未开始投技");return false;
        }
    }
    if(variant.translate_player_requirement&&!ReceiverComponents(target)){
        Status(L"此受技适配要求敌人具备实际输入和武器组件，未施放");return false;
    }
    if(!KnownMontagePath(c.mesh)||!KnownMontagePath(Property<Object*>(target,"Mesh"))){
        Status(L"双方当前动画播放入口未通过已审计路径校验，未施放投技");return false;
    }
    for(const PreparedTransition& t:prepared_.transitions)if(t.requires_combat_components){
        Object* actor=t.reverse?target:c.pawn;
        Call targeting(actor,"GetTargetingComponent","/Script/SGameCombatSystem.TargetingComponentHolder"),weapons(actor,"GetCharacterWeaponsComponent","/Script/SGameCombatSystem.CharacterWeaponsHolder");
        if(!targeting.Invoke()||!weapons.Invoke()||!ue::Alive(targeting.Get<Object*>("ReturnValue"))||!ue::Alive(weapons.Get<Object*>("ReturnValue"))||
            !ue::IsA(targeting.Get<Object*>("ReturnValue"),Type("/Script/SGameCombatSystem.TargetingComponent"))||
            !ue::IsA(weapons.Get<Object*>("ReturnValue"),Type("/Script/SGameCombatSystem.CharacterWeaponsComponent"))){
            Status(L"后续原生反制阶段缺少有效锁定 / 武器组件，未开始投技");return false;
        }
    }
    std::vector<Spec> target_specs; if (!Specs(target_asc,target_specs,0,true)) return false;
    CleanupProof source_proof,target_proof;
    if (!ReadCleanupProof(c.pawn,c.asc,source_proof)||!ReadCleanupProof(target,target_asc,target_proof)) {
        Status(L"无法核对原胶囊 / 投技状态基线，未施放"); return false;
    }
    if (target_proof.pawn_response!=2) {
        Status(L"目标碰撞基线暂不适配：Pawn 通道响应 %u，需 Blocking(2)；未施放",target_proof.pawn_response);
        Logf("MOTIAN_GRAB refused target=%s PawnResponse=%u (native victim End restores Blocking)",Text(target).c_str(),target_proof.pawn_response);
        return false;
    }
    Alignment alignment; stage_="alignment preflight";
    timing.Next(3);
    if (!PrepareAlignment(c,target,alignment,&scene_pawns.pawns)) return false;
    GrabStateLeases::Ledger state_lease;GrabHuashanAdapter::Ledger hua_lease;GrabEffectGuard::Ledger effect_guard;std::wstring adapter_reason;
    if(!GrabStateLeases::Capture(c.pawn,target,variant.source_policy,variant.victim_policy,state_lease,adapter_reason)){
        Status(L"%s",adapter_reason.c_str());return false;
    }
    if(variant.huashan_domain_bridge&&!GrabHuashanAdapter::Capture(c.pawn,target,hua_lease,adapter_reason,HuaLanding)){
        Status(L"%s",adapter_reason.c_str());return false;
    }
    if(variant.guard_effect&&!GrabEffectGuard::Capture(c.pawn,c.asc,target,target_asc,c.world,prepared_.guard_effect.object,effect_guard,adapter_reason)){
        Status(L"%s",adapter_reason.c_str());return false;
    }
    timing.Next(2);
    // Only the victim base changes pawn-channel collision. A native dodge
    // triggered by Circle may legitimately restore the executor's collision
    // while Execution cancels that old action; do not pin its transient value.
    source_proof.check_collision=false;
    Call play(c.execution,"PlayExactExecution");
    if (!play.Slot("AnimData",24)||!play.Slot("Target",8)||!play.Slot("StartTime",4)) return false;
    session_.pawn=Id(c.pawn); session_.target=Id(target); session_.world=Id(c.world);
    session_.source_class=Id(source); session_.victim_class=Id(victim); session_.source_asc=Id(c.asc); session_.target_asc=Id(target_asc);
    session_.execution=Id(c.execution);
    if(variant.transition_count)session_.target_execution=Id(target_execution);
    session_.variant=run_variant_;session_.redirect=redirect_attempt_;session_.max_duration_ms=variant.max_duration_ms;
    session_.deferred_grace_ms=variant.deferred_grace_ms;
    session_.verify_source_end_walking=variant.verify_source_end_walking;
    if(variant.verify_source_end_walking){session_.source_movement=Id(c.movement);session_.source_mode_before=Property<uint8_t>(c.movement,"MovementMode");}
    session_.transitions=prepared_.transitions;
    session_.state_lease=std::move(state_lease);session_.huashan_lease=std::move(hua_lease);
    session_.effect_guard=std::move(effect_guard);
    session_.source_proof=source_proof; session_.target_proof=target_proof;
    CaptureHandles(own_specs,session_.source_before); CaptureHandles(target_specs,session_.target_before);
    if((!session_.transitions.empty()||session_.deferred_grace_ms)&&(!PendingBaseline(c.asc,session_.source_before)||!PendingBaseline(target_asc,session_.target_before))){
        session_={};Status(L"原生能力队列仍在提交，未开始多阶段投技");return false;
    }
    if (!Same(session_.pawn)||!Same(session_.target)||!Same(session_.world)||!Same(session_.source_class)||!Same(session_.victim_class)||
        !Same(session_.source_asc)||!Same(session_.target_asc)||!Same(session_.execution)||!Same(session_.source_proof.actor)||!Same(session_.source_proof.asc)||
        !Same(session_.source_proof.capsule)||!Same(session_.target_proof.actor)||!Same(session_.target_proof.asc)||!Same(session_.target_proof.capsule)||
        (variant.transition_count&&!Same(session_.target_execution))) {
        session_={}; Status(L"投技关键对象弱引用身份未建立，未调用原生激活");
        Logf("MOTIAN_GRAB key weak identities not ready before native call; no ability grant performed"); return false;
    }
    if(variant.verify_source_end_walking&&(!Same(session_.source_movement)||(session_.source_mode_before!=1&&session_.source_mode_before!=2))){
        session_={};Status(L"此招的原生移动模式基线不是 Walking / NavWalking，未施放");return false;
    }
    if (!InputBaseAllowed(c) || !InputAllowed(c) || !ue::Alive(c.controller) ||
        Property<Object*>(c.controller,"Pawn")!=c.pawn ||
        Property<Object*>(Property<Object*>(engine_,"GameViewport"),"World")!=c.world) {
        session_={}; Status(L"加载投技后焦点、角色或场景已变化，未激活双方能力"); return false;
    }
    CancelOwnReactions(c);
    session_.began=GetTickCount64(); cleanup_pending_=true; stage_="native paired activation";
    const AnimData data{source,victim,nullptr};
    timing.Next(4);
    LARGE_INTEGER native_begin{},native_end{},receipt_end{},native_frequency{};
    QueryPerformanceFrequency(&native_frequency); QueryPerformanceCounter(&native_begin);
    const bool invoked=play.Set("AnimData",data).Set("Target",target).Set("StartTime",0.0f).Invoke();
    QueryPerformanceCounter(&native_end);
    const bool receipts=invoked&&BindNativeGrants();
    QueryPerformanceCounter(&receipt_end);
    const double play_ms=native_frequency.QuadPart?1000.*static_cast<double>(native_end.QuadPart-native_begin.QuadPart)/native_frequency.QuadPart:0;
    const double receipt_ms=native_frequency.QuadPart?1000.*static_cast<double>(receipt_end.QuadPart-native_end.QuadPart)/native_frequency.QuadPart:0;
    if (prepared_.profile_reports<2||play_ms+receipt_ms>8)
        Logf("MOTIAN_GRAB_NATIVE_TIMING playExactMs=%.3f receiptRowsMs=%.3f",play_ms,receipt_ms);
    if (!invoked) session_.receipts_bound=true; // ProcessEvent was not entered.
    if (!invoked || !receipts) {
        Logf("MOTIAN_GRAB_START native receipt not confirmed invoked=%d receipt=%d grants=%zu uncertain=%d; exact owned cancellation requested",
            invoked?1:0,receipts?1:0,session_.grants.size(),session_.uncertain?1:0);
        Cleanup(true); Status(L"原生成对激活读回失败，已请求精确句柄清理"); return false;
    }
    Object* source_instance=nullptr,*victim_instance=nullptr;
    timing.Next(5);
    for (const Grant& grant:session_.grants) for (const Identity& instance:grant.instances) if (Same(instance)) {
        bool active=false; if (!IsActive(instance.object,active)||!active) continue;
        if (grant.asc.object==c.asc) source_instance=instance.object; else if (grant.asc.object==target_asc) victim_instance=instance.object;
    }
    if(variant.huashan_domain_bridge&&victim_instance&&!GrabHuashanAdapter::ObserveVictim(session_.huashan_lease,victim_instance,session_.source_handle,session_.victim_handle,adapter_reason)){
        Cleanup(true);Status(L"%s",adapter_reason.c_str());return false;
    }
    if (!source_instance || !victim_instance) {
        Logf("MOTIAN_GRAB_START source-only/inactive pair: sourceActive=%d victimActive=%d grants=%zu; normal cancel requested",
            source_instance?1:0,victim_instance?1:0,session_.grants.size());
        Cleanup(true); Status(L"原能力未同时激活，未把 Invoke 当投技成功；已请求取消"); return false;
    }
    if(variant.huashan_domain_bridge&&!GrabHuashanAdapter::Observe(session_.huashan_lease,source_instance,victim_instance,adapter_reason)){
        Cleanup(true);Status(L"%s",adapter_reason.c_str());return false;
    }
    int32_t source_handle=0,victim_handle=0;
    for (const Grant& grant:session_.grants) {
        if (grant.asc.object==c.asc) source_handle=grant.handle;
        if (grant.asc.object==target_asc) victim_handle=grant.handle;
    }
    // PlayExact grants/activates the executor before writing ExecutedHandle.
    // Its BP activation copies CurrentExecutionInfo synchronously at that point.
    // The stock hit route later refreshes that copy; do the same proven pure
    // assignment after both native grants, before starting the success graph.
    CurrentInfo executor_info,victim_info;
    const bool diagnose=!prepared_.diagnosed;
    if (source_instance&&diagnose) PairDiagnostics(source_instance,false,"before-refresh");
    if (victim_instance&&diagnose) PairDiagnostics(victim_instance,true,"native-victim");
    stage_="refresh owned source execution info";
    const bool component_matches=source_instance&&Property<Object*>(source_instance,"Execution Component")==session_.execution.object;
    if (!source_handle || !victim_handle || !component_matches || !Call(source_instance,"InitializeExecutionInfo").Invoke() ||
        !Info(source_instance,executor_info)||!Info(victim_instance,victim_info) ||
        executor_info.execution!=source_handle || executor_info.executed!=victim_handle ||
        victim_info.execution!=source_handle || victim_info.executed!=victim_handle) {
        Logf("MOTIAN_GRAB_START handle sync failed expected=%d/%d source=%d/%d victim=%d/%d",source_handle,victim_handle,
            executor_info.execution,executor_info.executed,victim_info.execution,victim_info.executed);
        Logf("MOTIAN_GRAB_START refresh cachedComponentMatches=%d; no raw component/Info replacement",component_matches?1:0);
        Cleanup(true); Status(L"原执行者 / 受投者双句柄同步未通过，已请求取消"); return false;
    }
    if (diagnose) PairDiagnostics(source_instance,false,"after-refresh");
    if(!session_.phases.empty())session_.phases.front().refreshed=true;
    stage_="strict owned pair admission";
    admission_reason_=""; reject_detail_[0]=0;
    const bool source_admitted=DiscoverGrants(true),victim_admitted=source_admitted&&DiscoverGrants(false);
    if (!source_admitted||!victim_admitted) {
        Logf("MOTIAN_GRAB_START admission failed side=%s reason=%s detail=%s",source_admitted?"victim":"executor",
            admission_reason_[0]?admission_reason_:"unknown",reject_detail_);
        Cleanup(true); Status(L"双能力对象已精确记录，但原生角色 / 信息读回不符，已请求成对取消"); return false;
    }
    stage_="single authored pair alignment";
    timing.Next(6);
    if (!Same(session_.pawn)||!Same(session_.target)||!Same(session_.world)) {
        Logf("MOTIAN_GRAB_START pawn/target/world identity lost before placement"); Cleanup(true); return false;
    }
    if (!ApplyAlignment(c,target,alignment)) { Cleanup(true); return false; }
    // The stock BulletEvent deliberately replaces grab with NonPlayerGE for
    // AI victims. Use the native paired-execution path, then the exact same
    // authored success-timeline entry used by the player-victim hit route.
    // PlayGrabTimeline starts with the stock RefreshExecutionData before
    // replacing the timeline, so success warping captures the corrected pair.
    // No CDO/event mutation, fake damage, synthetic Hit event or montage-only play.
    const Name success=prepared_.success_timeline;
    timing.Next(7);
    stage_="native grab success timeline";
    Call grab(source_instance,"PlayGrabTimeline","/Script/SGameExecution.GrabExecutionAbilityInterface"),timeline(source_instance,"GetTimelineByName");
    Call playing(source_instance,"IsPlayingTimeline");
    if (!grab.Set("StartTimeSeconds",0.0f).Set("TimelineName",success).Invoke() ||
        !timeline.Set("TimelineName",success).Invoke() || !timeline.Get<Object*>("ReturnValue") ||
        !playing.Set("Timeline",timeline.Get<Object*>("ReturnValue")).Invoke() || !playing.Get<bool>("ReturnValue") ||
        !PairInfo(source_instance,false)||!PairInfo(victim_instance,true)) {
        Logf("MOTIAN_GRAB_START success timeline/roles not confirmed; source=%s victim=%s; native cancellation requested",
            Text(source_instance).c_str(),Text(victim_instance).c_str());
        Cleanup(true); Status(L"原投技成功 Timeline / 双向配对读回失败，已请求取消"); return false;
    }
    if(variant.huashan_domain_bridge&&!GrabHuashanAdapter::Apply(session_.huashan_lease,source_instance,victim_instance,adapter_reason)){
        Cleanup(true);Status(L"%s",adapter_reason.c_str());return false;
    }
    session_.started=true; session_.paired=true; cleanup_pending_=false; prepared_.diagnosed=true;
    GrabExtras::Weapon shown[GrabExtras::kMaxWeapons]; size_t shown_count=0;
    for (size_t i=0;i<prepared_.weapons.size()&&i<prepared_.weapon_rows.size()&&shown_count<GrabExtras::kMaxWeapons;++i) {
        if (!Same(prepared_.weapons[i])) continue;
        shown[shown_count].actor_class=prepared_.weapons[i].object; shown[shown_count].class_path=prepared_.weapon_rows[i]->actor_class;
        shown[shown_count].socket=prepared_.weapon_rows[i]->socket;
        shown[shown_count].rest=prepared_.weapon_rows[i]->has_rest?prepared_.weapon_rows[i]->rest:nullptr;
        if(i<prepared_.proxies.size()&&Same(prepared_.proxies[i])){shown[shown_count].proxy_mesh=prepared_.proxies[i].object;shown[shown_count].proxy_scale=prepared_.weapon_rows[i]->proxy_scale;}
        ++shown_count;
    }
    GrabExtras::Begin(c.pawn,target,shown,shown_count,variant.id);
    if(!session_.phases.empty())session_.phases.front().admitted=true;
    AcquireSRWLockExclusive(&state_lock); ++state_.activations; ++state_.paired_hits; ReleaseSRWLockExclusive(&state_lock);
    Status(L"%s：原生施放 / 受投双能力已配对；伤害与位移由原 Timeline 负责",variant.name);
    Logf("MOTIAN_GRAB paired variant=%s executor=%s target=%s source=%s victim=%s distance=%.1f grants=%zu native-current-info-readback=1",
        variant.id,Text(c.pawn).c_str(),Text(target).c_str(),Text(source_instance).c_str(),Text(victim_instance).c_str(),best,session_.grants.size());
    stage_="idle"; return true;
}
bool Start(const Context& c) {
    if (HasResources()) return false;
    PhaseTimer timer(0);
    trigger_lookup_=true;
    struct TriggerReset { ~TriggerReset() { trigger_lookup_=false; } } reset;
    const bool ok=StartAttempt(c);
    if (!ok) {
        Logf("MOTIAN_GRAB_START failed stage=%s began=%d grants=%zu uncertain=%d retainedClasses=%zu; success not reported",
            stage_,session_.began?1:0,session_.grants.size(),session_.uncertain?1:0,class_leases_.size());
        if (!session_.began) {
            if (!ReleaseClasses()) Status(L"投技准备失败；类引用释放尚未确认，保留清理账本");
            cleanup_pending_=!class_leases_.empty();
        }
    }
    return ok;
}
void ResetKeys() { key_baseline_=false; armed_=false; last_top_=last_right_=false; top_time_=right_time_=0; combo_deadline_=0; combo_owner_={}; }
bool Combo(const Context& c) {
    if (top_.name.None()) { top_.name=CachedName(L"Gamepad_FaceButton_Top"); right_.name=CachedName(L"Gamepad_FaceButton_Right"); }
    if (ue::StructSize(Type("/Script/InputCore.Key"))!=24 || top_.name.None()||right_.name.None()) return false;
    Call top(c.controller,"IsInputKeyDown"),right(c.controller,"IsInputKeyDown");
    if (!top.Set("Key",top_).Invoke()||!right.Set("Key",right_).Invoke()) { ResetKeys(); return false; }
    const bool a=top.Get<bool>("ReturnValue"),b=right.Get<bool>("ReturnValue"); const ULONGLONG now=GetTickCount64();
    if (!key_baseline_) {
        key_baseline_=true; armed_=!a&&!b; last_top_=a; last_right_=b;
        Logf("MOTIAN_GRAB_INPUT baseline triangle=%d circle=%d armed=%d",a?1:0,b?1:0,armed_?1:0); return false;
    }
    if (a!=last_top_ || b!=last_right_) Logf("MOTIAN_GRAB_INPUT keys triangle=%d circle=%d armed=%d",a?1:0,b?1:0,armed_?1:0);
    if (a&&!last_top_) top_time_=now; if (b&&!last_right_) right_time_=now;
    const bool together=a&&b&&(!last_top_||!last_right_),edge=together&&armed_;
    if (!a&&!b) armed_=true; else if (together) armed_=false;
    last_top_=a; last_right_=b;
    const ULONGLONG delta=top_time_>right_time_?top_time_-right_time_:right_time_-top_time_;
    return edge && top_time_ && right_time_ && delta<=220;
}
// ---- 黑伤 executions -> MoTian's full grab ----------------------------------
// The native execution is cancelled the frame after it starts (victim side first,
// so a side that refuses leaves both untouched); once both have ended, the full
// grab runs on the same target through the normal paired path.
constexpr const char* kTangWeapons[]={"Tang","Tang_HorseCartCombat","TangPrologue"};
void ExecutionStatus(const wchar_t* format,...) {
    wchar_t value[300]{}; va_list args; va_start(args,format); vswprintf_s(value,format,args); va_end(args);
    AcquireSRWLockExclusive(&state_lock); state_.execution_status=value; ReleaseSRWLockExclusive(&state_lock);
}
void ReadTangEquipped() {
    if (GetTickCount64()<next_weapon_read_) return;
    next_weapon_read_=GetTickCount64()+2000;
    std::string id; tang_equipped_=WeaponPresets::CurrentMainWeaponId(id)&&
        std::any_of(std::begin(kTangWeapons),std::end(kTangWeapons),[&](const char* weapon){return !_stricmp(id.c_str(),weapon);});
}
// 黑伤's own paired and quick executions; the per-enemy common executions only while
// 黑伤 is the equipped main weapon (read only then, at most every 2 s, since the
// first read per world scans for the backpack). Boss-duel (BDEx) and shachi
// executions stay native.
bool TangExecution(const char* name) {
    if (!_strnicmp(name,"GA_Hun_BEx_Tang_",16)||!_strnicmp(name,"GA_SimpleExecution_Tang",23)) return true;
    if (_strnicmp(name,"GA_Hun_BEx_Common_",18)) return false;
    ReadTangEquipped(); return tang_equipped_;
}
bool ActiveTangExecution(const Context& c,Identity& instance,char* name,size_t size) {
    std::vector<Spec> specs; if (!Specs(c.asc,specs,0,true)) return false;
    for (const Spec& spec:specs) {
        if (!spec.active||!ue::Alive(spec.cdo.object)) continue;
        ue::NameText(ue::NameOf(ue::ClassOf(spec.cdo.object)),name,size);
        if (!TangExecution(name)) continue;
        std::vector<Spec> full; if (!Specs(c.asc,full,spec.handle,false)||full.empty()) return false;
        for (const Identity& id:full.front().instances) { bool active=false; if (Same(id)&&IsActive(id.object,active)&&active) { instance=id; return true; } }
    }
    return false;
}
// Admission that can be judged while the target is still inside the native execution.
bool RedirectTarget(const Context& c,Object* actor) {
    Object* asc=nullptr,*execution=nullptr; bool boss=false; double distance=0;
    redirect_attempt_=true;
    bool ok=Enemy(actor,c,asc,execution,boss,true)&&OriginalGrabTags(prepared_.event.object,asc,true,true);
    if (ok&&!(PointDistance(c,actor,distance)&&distance<=kVariants[run_variant_].max_range)) ok=Reject("distance_or_visibility");
    redirect_attempt_=false; return ok;
}
// Quick executions are not paired: take the closest enemy the grab can replace them on.
Object* NearestRedirectTarget(const Context& c) {
    FloorIgnore scene; if (!scene.Load(c.pawn,c.pawn,nullptr)) return nullptr;
    Vector player; if (!Position(c.pawn,player)) return nullptr;
    Object* best=nullptr; double closest=1e30;
    for (int32_t i=0;i<scene.pawns.num;++i) {
        Object* actor=static_cast<Object**>(scene.pawns.data)[i]; Vector at;
        if (actor==c.pawn||!Position(actor,at)) continue;
        const double d=std::hypot(at.x-player.x,at.y-player.y);
        if (d<closest&&d<=kVariants[run_variant_].max_range+150&&RedirectTarget(c,actor)) { closest=d; best=actor; }
    }
    return best;
}
// Cancels a native ability instance; true once it has ended or is ending.
bool CancelInstance(const Identity& instance) {
    if (!Same(instance)) return true;
    Call(instance.object,"K2_CancelAbility").Invoke();
    bool active=true,ending=false;
    return IsActive(instance.object,active)&&(!active||(Bool(instance.object,"bIsAbilityEnding",ending)&&ending));
}
bool FinishRedirect(const Context& c) {
    Object* target=Same(redirect_.target)?redirect_.target.object:nullptr;
    if (!target||GetTickCount64()>redirect_.deadline) {
        Logf("TANG_EXECUTION redirect expired ability=%s target=%d executorEnded=%d victimEnded=%d",redirect_.ability,target?1:0,
            Ended(redirect_.executor)?1:0,redirect_.victim.object?(Ended(redirect_.victim)?1:0):1);
        ExecutionStatus(L"处决已取消，但目标或双方状态未在 1.5 秒内就绪，魔渊 · 完整擒拿未开始");
        redirect_={}; return false;
    }
    // Both native sides must have fully ended before the pair is reused.
    if (!Ended(redirect_.executor)||(redirect_.victim.object&&!Ended(redirect_.victim))) return true;
    std::vector<Spec> own,theirs; Object* target_asc=Asc(target);
    if (!Specs(c.asc,own,0,true)||ExecutionBusy(own)||!ue::Alive(target_asc)||!Specs(target_asc,theirs,0,true)||ExecutionBusy(theirs)) return true;
    if (!InputBaseAllowed(c)||!InputAllowed(c)) return true;
    UseExecutionSlot(); forced_target_=target; redirect_attempt_=true;
    const bool ok=Start(c);
    forced_target_=nullptr; redirect_attempt_=false;
    if (!session_.began) RestoreSlot();
    char ability[128]{}; strcpy_s(ability,redirect_.ability); redirect_={};
    if (ok) {
        AcquireSRWLockExclusive(&state_lock); ++state_.executions_redirected; ReleaseSRWLockExclusive(&state_lock);
        ExecutionStatus(L"已将 %hs 改为魔渊 · 完整擒拿",ability);
        Logf("TANG_EXECUTION redirected ability=%s target=%s",ability,Text(target).c_str());
    } else {
        ExecutionStatus(L"处决已取消，但魔渊 · 完整擒拿未能开始（%hs）",stage_);
        Logf("TANG_EXECUTION grab did not start after cancel ability=%s stage=%s",ability,stage_);
    }
    return true;
}
// True while a redirect owns this tick (the grab chord waits for it).
bool RedirectExecution(const Context& c) {
    if (!options_.tang_execution||execution_variant_<0) { redirect_={}; ignored_execution_={}; return false; }
    if (redirect_.pending) return FinishRedirect(c);
    if (HasResources()) return false;
    Identity instance; char name[128]{};
    if (!ActiveTangExecution(c,instance,name,sizeof(name))) { ignored_execution_={}; return false; }
    if (SameIdentity(instance,ignored_execution_)) return false;
    ignored_execution_=instance; // judged once per execution
    if (!SlotReady(execution_variant_)) {
        Logf("TANG_EXECUTION kept native ability=%s reason=grab_not_prepared",name);
        ExecutionStatus(L"魔渊 · 完整擒拿尚未预载，本次处决保持原样"); return false;
    }
    CurrentInfo info; Object* target=Info(instance.object,info)&&Resolve(info.executor)==c.pawn?Resolve(info.target):nullptr;
    UseExecutionSlot();
    const bool paired=target!=nullptr;
    if (!paired) target=NearestRedirectTarget(c);
    const bool ok=target&&(!paired||RedirectTarget(c,target));
    RestoreSlot();
    if (!ok) {
        Logf("TANG_EXECUTION kept native ability=%s target=%s reason=%s detail=%s",name,Text(target).c_str(),
            target?(reject_reason_[0]?reject_reason_:"unknown"):"no_target",reject_detail_);
        ExecutionStatus(L"%hs 的目标不能使用魔渊 · 完整擒拿（%hs），保持原处决",name,target?reject_reason_:"no_target");
        return false;
    }
    // Victim first: if its executed ability refuses to end, nothing has changed yet.
    Identity victim;
    if (paired&&info.executed) {
        Object* target_asc=Asc(target); std::vector<Spec> rows;
        if (ue::Alive(target_asc)&&Specs(target_asc,rows,info.executed,false)&&!rows.empty())
            for (const Identity& id:rows.front().instances) { bool active=false; if (Same(id)&&IsActive(id.object,active)&&active) victim=id; }
        if (victim.object&&!CancelInstance(victim)) {
            Logf("TANG_EXECUTION kept native ability=%s reason=victim_not_cancelable",name);
            ExecutionStatus(L"受处决方的能力不可取消，保持原处决"); return false;
        }
    }
    if (!CancelInstance(instance)) Logf("TANG_EXECUTION executor %s did not end on cancel; waiting",name);
    redirect_={}; redirect_.pending=true; redirect_.deadline=GetTickCount64()+1500;
    redirect_.target=Id(target); redirect_.executor=instance; redirect_.victim=victim; strcpy_s(redirect_.ability,name);
    Logf("TANG_EXECUTION cancel ability=%s target=%s paired=%d victim=%d",name,Text(target).c_str(),paired?1:0,victim.object?1:0);
    return true;
}
// PBZDebugMenu.ini [Grab]: the move is stored by id so a catalog change
// never silently selects a different move.
void SaveOptions(const Options& o) {
    SettingsStore::PutBool(L"Grab",L"Enabled",o.enabled);
    SettingsStore::PutBool(L"Grab",L"Bosses",o.allow_bosses);
    SettingsStore::PutBool(L"Grab",L"Position",o.position_correction);
    SettingsStore::PutFloat(L"Grab",L"Range",o.range);
    SettingsStore::PutBool(L"Grab",L"TangExecution",o.tang_execution);
    if(o.variant>=0&&o.variant<static_cast<int32_t>(std::size(kVariants)))SettingsStore::Put(L"Grab",L"Move",Wide(kVariants[o.variant].id));
}
void Tick() {
    if (!initialized_) return;
    bool manual=false,cancel=false;int32_t expected_variant=-1;
    AcquireSRWLockExclusive(&request_lock);
    if (has_options_) {
        if(options_.variant!=pending_.variant){manual_deadline_=0;manual_variant_=-1;}
        options_=pending_; has_options_=false; ResetKeys(); input_gate_="options_changed";
        SaveOptions(options_);
        Logf("MOTIAN_GRAB_OPTIONS enabled=%d humanoid_bosses=%d positionCorrection=%d variant=%s range=%.1f tangExecution=%d",options_.enabled?1:0,options_.allow_bosses?1:0,options_.position_correction?1:0,kVariants[options_.variant].id,options_.range,options_.tang_execution?1:0);
    }
    manual=trigger_;expected_variant=trigger_variant_;trigger_=false;trigger_variant_=-1;cancel=cancel_; cancel_=false; ReleaseSRWLockExclusive(&request_lock);
    const bool explicit_cancel=cancel;
    if(manual&&expected_variant>=0&&expected_variant!=options_.variant){manual=false;manual_deadline_=0;manual_variant_=-1;Status(L"所选投技尚未应用；本次请求已拒绝，请先应用设置");}
    if(manual&&HasResources()){manual=false;Status(L"当前投技尚未完整结束；未排队下一招");}
    if (manual) { manual_deadline_=GetTickCount64()+10000;manual_variant_=options_.variant;Status(L"已排队一次投技：10 秒内回到游戏；仍需通过焦点、生命与目标门控"); }
    if (cancel) {manual_deadline_=0;manual_variant_=-1;}
    // Grabs and the 黑伤 execution redirect share this module; a chord grab still
    // ends when grabs are switched off.
    const bool module_on=options_.enabled||options_.tang_execution;
    if (!module_on || InterlockedCompareExchange(&disabled_,0,0)) cancel=true;
    if (!options_.enabled&&session_.began&&!session_.redirect) cancel=true;
    if (!options_.enabled) { if (manual) Status(L"请先开启投技并应用设置"); manual_deadline_=0;manual_variant_=-1; }
    // Default-off does no context enumeration or native calls.
    if (!module_on&&!HasResources()&&prepared_.assets.empty()&&prepared_.variant<0&&spare_.assets.empty()&&spare_.variant<0&&!slot_swapped_&&!manual) { Publish(); return; }
    if (!session_.began && !class_leases_.empty()) {
        if (GetTickCount64()>=next_cleanup_) { next_cleanup_=GetTickCount64()+500; Cleanup(true); }
        Publish(); return;
    }
    if (session_.began) {
        stage_="monitor paired execution";
        GrabExtras::Tick();
        if (InterlockedCompareExchange(&disabled_,0,0) && !explicit_cancel) { Publish(); return; }
        const bool old_world=!Same(session_.world)||!Same(session_.pawn)||!Same(session_.target);
        if(!PollTransitions())cleanup_pending_=true;
        if(session_.deferred_grace_ms){bool advancing=false;if(!BrokerTime(advancing))cleanup_pending_=true;}
        const bool expired=session_.transitions.empty()&&!session_.deferred_grace_ms?GetTickCount64()-session_.began>session_.max_duration_ms:session_.broker_clock>session_.max_duration_ms;
        if (cancel||old_world||expired||cleanup_pending_) {
            if (!explicit_cancel && GetTickCount64()<next_cleanup_) { Publish(); return; }
            next_cleanup_=GetTickCount64()+500;
            // A paired grab that ran to its time limit, or whose monitoring ended,
            // is a finished grab; only a cancel or a lost pawn / target / world is not.
            if (session_.paired&&!cancel&&!old_world&&!session_.source_completed) {
                session_.source_completed=true; GrabExtras::SourceEnded();
                Logf("MOTIAN_GRAB finished by %s; grab extras settled",expired?"time limit":"monitor end");
            }
            if (Cleanup(true)) Status(L"投技会话已取消并清理原生能力句柄");
            else Status(L"投技取消 / 结束尚未完整，保留原生句柄记录并阻止切图");
        } else if (GetTickCount64()>=next_poll_) {
            PhaseTimer timer(2);
            next_poll_=GetTickCount64()+100;
            admission_reason_="";
            const bool admitted=DiscoverGrants(true)&&DiscoverGrants(false);
            if (!admitted) {
                // The executor's grab may simply have ended; count that before cleanup.
                bool source_running=false,read=true;
                for (const Grant& g:session_.grants) if (g.asc.object==session_.source_asc.object) for (const Identity& i:g.instances) if (Same(i)) {
                    bool active=false; if (!IsActive(i.object,active)) read=false; else source_running=source_running||active;
                }
                if (session_.paired&&read&&!source_running) { session_.source_completed=true; GrabExtras::SourceEnded(); }
                Logf("MOTIAN_GRAB monitor admission ended reason=%s sourceEnded=%d",admission_reason_[0]?admission_reason_:"unknown",read&&!source_running?1:0);
                cleanup_pending_=true;
            }
            else {
                bool running=false,source_running=false,victim_running=false;
                for (const Grant& g:session_.grants) for (const Identity& i:g.instances) if (Same(i)) {
                    bool active=false; if (!IsActive(i.object,active)) cleanup_pending_=true; else {
                        running=running||active;
                        if (g.asc.object==session_.source_asc.object) source_running=source_running||active;
                        else victim_running=victim_running||active;
                    }
                }
                if (!cleanup_pending_&&session_.paired&&!source_running) { session_.source_completed=true; GrabExtras::SourceEnded(); }
                if (!running && Cleanup(false)) Status(L"原投技结束，临时能力句柄已清理");
                else if (running && source_running!=victim_running) {
                    if (!session_.asymmetric_since) session_.asymmetric_since=GetTickCount64();
                    if (GetTickCount64()-session_.asymmetric_since>300) {
                        // Executor done while the victim still plays its get-up: the normal end of a grab.
                        if (!source_running && victim_running) session_.source_completed=true;
                        if (Cleanup(true)) Status(L"投技一侧提前结束，已正常取消另一侧并核对清理");
                    }
                } else session_.asymmetric_since=0;
            }
        }
        Publish(); return;
    }
    RestoreSlot(); // a finished redirect hands prepared_ back to the menu move
    PrepareSlots();
    if (cancel) { redirect_={}; Publish(); return; }
    Context c; const bool ready=ContextNow(c);
    if (ready&&RedirectExecution(c)) { ResetKeys(); Publish(ready); return; }
    if (!options_.enabled||!PreparedReady()) {
        ResetKeys(); Publish(ready);
        if (manual) { manual_deadline_=0;manual_variant_=-1;Status(L"资源准备中 / 未就绪；请等已预载后重新请求投技"); }
        return;
    }
    Publish(ready);
    if (manual_deadline_ && (GetTickCount64()>manual_deadline_||manual_variant_!=options_.variant)) { manual_deadline_=0;manual_variant_=-1;Status(L"排队的投技已过期或招式选择已变化；未施放"); }
    if (!ready) { InputGate("context_not_ready",L"等待当前主角、能力系统和投技组件就绪"); ResetKeys(); return; }
    if (!InputBaseAllowed(c)) { ResetKeys(); return; }
    const bool edge=!UiHotkeys::Bound(UiHotkeys::Action::GrabRelease) && Combo(c);
    if (edge) {
        combo_owner_=Id(c.pawn); combo_deadline_=GetTickCount64()+700;
        Logf("MOTIAN_GRAB_INPUT chord latched for700ms before native dodge/input-ignore gate");
    }
    if (combo_deadline_ && (!Same(combo_owner_) || combo_owner_.object!=c.pawn || GetTickCount64()>combo_deadline_)) {
        combo_deadline_=0; combo_owner_={};
    }
    if (!InputAllowed(c)) return;
    manual=manual_deadline_!=0;
    const bool combo=combo_deadline_!=0;
    if ((combo||manual) && GetTickCount64()-last_attempt_>800) {
        manual_deadline_=0;manual_variant_=-1;combo_deadline_=0; combo_owner_={}; last_attempt_=GetTickCount64();
        AcquireSRWLockExclusive(&state_lock); ++state_.attempts; ReleaseSRWLockExclusive(&state_lock);
        Logf("MOTIAN_GRAB request combo=%d manual=%d",combo?1:0,manual?1:0);
        Start(c); Publish(ready);
    }
}
void SafeFinishExtras() { __try { GrabExtras::Finish(false); } __except(EXCEPTION_EXECUTE_HANDLER) {} }
int FaultFilter(EXCEPTION_POINTERS* exception) {
    fault_code_=exception->ExceptionRecord->ExceptionCode;
    fault_address_=reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress); return EXCEPTION_EXECUTE_HANDLER;
}
}
void Initialize(Logger logger,InputFocus focus) {
    logger_=logger; input_focus_=focus; initialized_=true;
    AcquireSRWLockExclusive(&state_lock); state_.variants.clear();state_.bosses.clear();
    for(size_t i=0;i<std::size(kVariants);++i){
        const VariantData& v=kVariants[i];Variant move;move.id=Wide(v.id);move.name=v.name;move.index=static_cast<int32_t>(i);
        move.boss_id=Wide(v.boss_id);move.boss_name=v.boss_name;move.supported=MoveSupported(move.index);move.reason=v.reason;
        move.max_range=v.max_range;move.recommended_range=v.recommended_range;
        move.detail=move.reason;state_.variants.push_back(std::move(move));
    }
    for(const BossData& data:kBosses){
        BossInfo boss;boss.id=Wide(data.id);boss.name=data.name;boss.status=data.status;
        for(const Variant& move:state_.variants)if(move.boss_id==boss.id){++boss.move_count;boss.supported=boss.supported||move.supported;}
        state_.bosses.push_back(std::move(boss));
    }
    ReleaseSRWLockExclusive(&state_lock);
    for(size_t i=0;i<std::size(kVariants);++i)if(!std::strcmp(kVariants[i].boss_id,"MoTian")&&!std::strcmp(kVariants[i].id,"02"))execution_variant_=static_cast<int32_t>(i);
    Publish();
    if(SettingsStore::Get(L"Grab",L"Move",L"").empty()){
        // No saved move: defaults, except a saved choice for 黑伤 executions.
        Options o;o.tang_execution=SettingsStore::GetBool(L"Grab",L"TangExecution",true);RequestOptions(o);
    }
    const std::wstring saved=SettingsStore::Get(L"Grab",L"Move",L"");
    for(size_t i=0;!saved.empty()&&i<std::size(kVariants);++i){
        if(Wide(kVariants[i].id)!=saved)continue;
        Options o;o.variant=static_cast<int32_t>(i);
        o.enabled=SettingsStore::GetBool(L"Grab",L"Enabled",false)&&MoveSupported(o.variant);
        o.allow_bosses=SettingsStore::GetBool(L"Grab",L"Bosses",true);
        o.position_correction=SettingsStore::GetBool(L"Grab",L"Position",true);
        o.range=std::clamp(SettingsStore::GetFloat(L"Grab",L"Range",kVariants[i].recommended_range),80.f,kVariants[i].max_range);
        o.tang_execution=SettingsStore::GetBool(L"Grab",L"TangExecution",true);
        if(RequestOptions(o)){
            AcquireSRWLockExclusive(&state_lock);state_.options=o;ReleaseSRWLockExclusive(&state_lock);
            Logf("MOTIAN_GRAB restored saved options enabled=%d move=%s range=%.1f",o.enabled?1:0,kVariants[i].id,o.range);
        }
        break;
    }
    Logf("MOTIAN_GRAB initialized default-off; PS5 triangle+circle=Gamepad_FaceButton_Top+Right; no shared CDO changes");
}
bool RequestOptions(const Options& options) {
    if (options.variant<0 || options.variant>=static_cast<int32_t>(std::size(kVariants)) || !std::isfinite(options.range)) return false;
    AcquireSRWLockExclusive(&request_lock); pending_=options; pending_.range=std::clamp(pending_.range,80.f,kVariants[options.variant].max_range); has_options_=true; ReleaseSRWLockExclusive(&request_lock); return true;
}
bool RequestTrigger() { return RequestTrigger(-1); }
bool RequestTrigger(int32_t expected_global_index) {
    if(expected_global_index< -1||expected_global_index>=static_cast<int32_t>(std::size(kVariants)))return false;
    AcquireSRWLockExclusive(&request_lock);trigger_=true;trigger_variant_=expected_global_index;ReleaseSRWLockExclusive(&request_lock);return true;
}
void RequestCancel() { AcquireSRWLockExclusive(&request_lock); cancel_=true; ReleaseSRWLockExclusive(&request_lock); }
Snapshot GetSnapshot() { AcquireSRWLockShared(&state_lock); Snapshot copy=state_; ReleaseSRWLockShared(&state_lock); return copy; }
void OnGameThreadGuarded() {
    __try { Tick(); }
    __except(FaultFilter(GetExceptionInformation())) {
        ++faults_; trigger_lookup_=preparing_lookup_=false; forced_target_=nullptr; redirect_attempt_=false; redirect_={};
        if (!prepared_.assets.empty()) prepared_.uncertain=true;
        if (!spare_.assets.empty()) spare_.uncertain=true;
        cleanup_pending_=HasResources()||prepared_.uncertain; ResetKeys();
        if (faults_>=3) InterlockedExchange(&disabled_,1);
        SafeFinishExtras();
        Logf("MOTIAN_GRAB fault count=%u code=%lu address=%p stage=%s; exact grant cleanup retained",faults_,fault_code_,reinterpret_cast<void*>(fault_address_),stage_);
        Status(L"投技原生调用异常，已停止新施放；保留清理记录"); Publish();
    }
}
bool BeforeTravel() {
    bool result=false;
    __try {
        ResetKeys(); manual_deadline_=0;manual_variant_=-1; redirect_={}; ignored_execution_={};
        result=Cleanup(true);
        if (result) { RestoreSlot(); result=ReleasePrepared(); }
        if (result) { std::swap(prepared_,spare_); result=ReleasePrepared(); std::swap(prepared_,spare_); }
        Publish();
    }
    __except(FaultFilter(GetExceptionInformation())) { cleanup_pending_=HasResources(); result=false; }
    return result;
}
bool HandleCommand(const char* verb,const char* arguments) {
    if (_stricmp(verb,"motiangrab") && _stricmp(verb,"grab")) return false;
    std::istringstream input(arguments?arguments:""); std::string action; input>>action;
    if (action=="on"||action=="off") { Options o=GetSnapshot().options; o.enabled=action=="on"; RequestOptions(o); }
    else if (action=="bosses") {
        int value=-1; input>>value; if (value!=0 && value!=1) return false;
        Options o=GetSnapshot().options; o.allow_bosses=value!=0; RequestOptions(o);
    }
    else if (action=="position") {
        int value=-1; input>>value; if (value!=0 && value!=1) return false;
        Options o=GetSnapshot().options; o.position_correction=value!=0; RequestOptions(o);
    }
    else if (action=="tang") {
        int value=-1; input>>value; if (value!=0 && value!=1) return false;
        Options o=GetSnapshot().options; o.tang_execution=value!=0; RequestOptions(o);
    }
    else if (action=="trigger") RequestTrigger();
    else if (action=="cancel"||action=="cleanup") RequestCancel();
    else if (action=="variant") {
        std::string id; input>>id; Options o=GetSnapshot().options;
        for (size_t i=0;i<std::size(kVariants);++i) if (!_stricmp(id.c_str(),kVariants[i].id)) { o.variant=static_cast<int32_t>(i); RequestOptions(o); break; }
    } else if (action=="list") {
        for (const VariantData& v:kVariants) Logf("MOTIAN_GRAB boss=%s variant=%s supported=%d source=%s victim=%s",v.boss_id,v.id,v.supported?1:0,v.source,v.victim);
    } else if (action=="status"||action.empty()) {
        const Snapshot s=GetSnapshot(); Logf("MOTIAN_GRAB status enabled=%d bosses=%d positionCorrection=%d corrected=%d ready=%d active=%d paired=%d cleanup=%d retainedClasses=%u variant=%s requests=%u activations=%u",
            s.options.enabled?1:0,s.options.allow_bosses?1:0,s.options.position_correction?1:0,s.position_corrected?1:0,s.ready?1:0,s.active?1:0,s.paired?1:0,s.cleanup_pending?1:0,s.retained_classes,kVariants[s.options.variant].id,s.attempts,s.activations);
        char alignment[1800]{};
        if (WideCharToMultiByte(CP_UTF8,0,s.alignment_status.c_str(),-1,alignment,static_cast<int>(sizeof(alignment)),nullptr,nullptr))
            Logf("MOTIAN_GRAB alignment_status=%s",alignment);
    } else Logf("MOTIAN_GRAB usage: grab on|off|bosses 0/1|position 0/1|tang 0/1|variant <move-id>|trigger|cancel|status|list");
    return true;
}
}
