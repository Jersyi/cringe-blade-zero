#include "npc_retarget.h"
#include "ue_runtime.h"
#include "npc_grip_math.h"
#include "npc_pose_transfer.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace NpcRetarget {
namespace {
using ue::Object; using ue::Call; using ue::Name; using ue::Array;
using Transform=NpcPoseTransfer::Transform;
struct Vector { double x = 0, y = 0, z = 0; };
static_assert(sizeof(Transform) == 96 && sizeof(Vector) == 24);
struct SoftPath { int32_t index = 0, serial = 0; Name package, asset; ue::String sub; };
static_assert(sizeof(SoftPath) == 40);
struct BoneFallback { const char* bone; const char* ancestor; Transform relative; };
struct GripBasis { const char* bone; const char* hand; Transform source_to_hand, target_to_hand; };
struct GripRemapBasis { const char* profile; const char* bone; double q0[4]; Transform target_mesh_ref,source_palm_hand,target_palm_hand_ref; double hand_basis[4]; const char* palm; };
#include "npc_grip_remap.generated.h"
struct ForearmBasis { const char* profile;const char* bone;const char* actor_class;const char* mesh;double rotation[4]; };
#include "npc_forearm_basis.generated.h"
struct BodySkeletonBasis { const char* profile;const char* bone;double q0[4]; };
#include "npc_body_basis.generated.h"
struct Profile {
    const char* id; const wchar_t* name; const char* donor; const char* body; const char* skeleton; int32_t bones;
    const uint16_t* shared; size_t shared_count; const char* const* parts; size_t part_count;
    const BoneFallback* fallbacks; size_t fallback_count; const GripBasis* grips; size_t grip_count;
    const char* head; const char* head_postprocess; const uint16_t* facial_bones; size_t facial_count; int32_t head_bones; const wchar_t* detail;
};
#include "npc_retarget_data.generated.h"
#include "npc_weapon_routes.generated.h"
constexpr uintptr_t kArrayAddRva = 0x04057060, kArrayRemoveRva = 0x04057d50;
// Set only after exact-PE proof. Array edits alone do not update Shipping SocketMap.
constexpr uintptr_t kRebuildSocketMapRva = 0x044d9d80;
constexpr uintptr_t kSocketTransformRva = 0x03da8000;
constexpr uintptr_t kSocketInfoRva = 0x03da78e0;
constexpr uintptr_t kGetMeshReferenceRva=0x044d2ff0;
constexpr size_t kSocketTransformSlot = 0x518 / sizeof(uintptr_t);
struct NativeSignature { uintptr_t rva; unsigned char code[16]; };
constexpr NativeSignature kNativeSignatures[] = {
    {kArrayAddRva, {0x48,0x8b,0xc4,0x53,0x56,0x41,0x55,0x48,0x83,0xec,0x50,0x4d,0x8b,0xe8,0x48,0x8b}},
    {kArrayRemoveRva, {0x48,0x85,0xc9,0x0f,0x84,0xae,0x01,0x00,0x00,0x48,0x89,0x6c,0x24,0x18,0x48,0x89}},
    {kRebuildSocketMapRva, {0x40,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8d,0x6c,0x24,0xc9,0x48,0x81,0xec,0xe0}},
    {kSocketTransformRva, {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24}},
    {kSocketInfoRva, {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x56,0x41}},
    {kGetMeshReferenceRva,{0x40,0x53,0x48,0x83,0xec,0x20,0xba,0x04,0x00,0x00,0x00,0x41,0xb8,0x03,0x00,0x00}},
    {0x1268710, {0x48,0x8b,0x05,0x31,0xb1,0xa2,0x0c,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}},
    {0x1f27f90, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x81,0xec,0x80,0x00}},
    {0x3d9c240, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48}},
    {0x5c84580, {0x48,0x89,0x5c,0x24,0x20,0x57,0x48,0x83,0xec,0x20,0x48,0x83,0xb9,0x50,0x10,0x00}},
};
constexpr const char* kSourceSkeleton = "/HuaShira/Characters/Hero/Hero_Hun/Hun_Skin_Skeleton.Hun_Skin_Skeleton";
SRWLOCK snapshot_lock = SRWLOCK_INIT, request_lock = SRWLOCK_INIT;
Snapshot snapshot_;
Logger logger_ = nullptr;
ReleaseAppearance release_appearance_ = nullptr;
Object* engine_ = nullptr;
bool initialized_ = false, active_ = false, cleanup_pending_ = false, primary_restored_ = false, primary_restore_started_ = false;
bool source_driver_mode_=false,source_settings_owned_=false,source_visible_written_=false,source_hidden_written_=false;
volatile LONG disabled_ = 0;
volatile LONG experimental_ = 0;
int32_t pending_action_ = -1, pending_entry_ = -1, waiting_action_ = -1, waiting_entry_ = -1, selected_ = -1;
ULONGLONG next_tick_ = 0, wait_deadline_ = 0;
DWORD faults_ = 0, fault_code_ = 0;
uintptr_t fault_address_ = 0;
const char* stage_ = "idle";
std::wstring failure_text_;

enum class PerfKind : size_t { BodyPre, FacePre, BodyCache, Grip, ForearmPoll, Maintenance, Count };
struct PerfBucket { volatile LONG64 calls=0,total=0,maximum=0; };
std::array<PerfBucket,static_cast<size_t>(PerfKind::Count)> perf_{};
LONGLONG perf_frequency_=0;ULONGLONG perf_next_report_=0;volatile LONG perf_enabled_=0;
LONGLONG PerfStart(){if(!InterlockedCompareExchange(&perf_enabled_,0,0))return 0;LARGE_INTEGER t{};QueryPerformanceCounter(&t);return t.QuadPart;}
void PerfEnd(PerfKind kind,LONGLONG start){if(!start||!InterlockedCompareExchange(&perf_enabled_,0,0)||!perf_frequency_)return;LARGE_INTEGER now{};QueryPerformanceCounter(&now);if(now.QuadPart<start)return;const LONG64 elapsed=now.QuadPart-start;auto& p=perf_[static_cast<size_t>(kind)];InterlockedIncrement64(&p.calls);InterlockedAdd64(&p.total,elapsed);
    LONG64 previous=InterlockedCompareExchange64(&p.maximum,0,0);while(previous<elapsed){const LONG64 actual=InterlockedCompareExchange64(&p.maximum,elapsed,previous);if(actual==previous)break;previous=actual;}}
struct PerfScope { PerfKind kind;LONGLONG start;explicit PerfScope(PerfKind value):kind(value),start(PerfStart()){}~PerfScope(){PerfEnd(kind,start);} };
void Logf(const char* format,...);
void PerfReport(){if(!InterlockedCompareExchange(&perf_enabled_,0,0)||!perf_frequency_||GetTickCount64()<perf_next_report_)return;perf_next_report_=GetTickCount64()+5000;
    const char* labels[]={"body_native_pre_plus_owned","head_native_pre_plus_filter","owned_body_cache","managed_grip_extra","forearm_poll","maintenance_250ms"};
    for(size_t i=0;i<perf_.size();++i){auto& p=perf_[i];const LONG64 calls=InterlockedExchange64(&p.calls,0),total=InterlockedExchange64(&p.total,0),maximum=InterlockedExchange64(&p.maximum,0);if(calls)Logf("NPC_PERF segment=%s calls=%lld avg_us=%.3f max_us=%.3f elapsed_qpc=1",labels[i],calls,static_cast<double>(total)*1e6/static_cast<double>(perf_frequency_)/static_cast<double>(calls),static_cast<double>(maximum)*1e6/static_cast<double>(perf_frequency_));}}

bool SetPerfEnabled(bool enabled){if(enabled){LARGE_INTEGER frequency{};if(!QueryPerformanceFrequency(&frequency)||frequency.QuadPart<=0)return false;perf_frequency_=frequency.QuadPart;perf_next_report_=GetTickCount64()+5000;}
    InterlockedExchange(&perf_enabled_,0);for(auto& p:perf_){InterlockedExchange64(&p.calls,0);InterlockedExchange64(&p.total,0);InterlockedExchange64(&p.maximum,0);}InterlockedExchange(&perf_enabled_,enabled?1:0);
    Logf("NPC_PERF %s default_off=1 report_interval_ms=5000",enabled?"enabled":"disabled");return true;}

struct Identity { Object* object = nullptr; Name name;int32_t index=-1,serial=0; };
bool FaceReadable(const void* pointer,size_t bytes);
bool RawIdentity(Identity identity,int32_t index,int32_t serial);
struct Visibility { Identity component; bool visible = false, hidden = false; bool shadows_owned=false,cast_shadow=false,hidden_shadow=false,visible_written=false,hidden_written=false; };
#pragma warning(push)
#pragma warning(disable: 4324) // Native FTransform requires 16-byte parameter alignment.
struct SocketAdded {
    Identity owner, socket, skeleton; Name socket_name, bone_name; int32_t index = -1; Transform local;
    const GripBasis* grip = nullptr; Transform source_local; Name raw_bone_alias;
};
struct GripSocket {
    Name name, hand, raw_bone, palm; Object* socket=nullptr; Transform source_local, source_ref, target_ref, reference_local,source_palm_hand; double q0[4]{},hand_basis[4]{};
};
struct ForearmBinding {
    const ForearmBasis* basis=nullptr;Name bone;bool enabled=false;const char* reason="not-equipped";
    Identity actor,mesh;int32_t actor_index=-1,actor_serial=0,mesh_index=-1,mesh_serial=0;
    int32_t leader=-1,anim_class=-1,anim_instance=-1,hidden=-1;uint8_t hidden_byte=0,hidden_mask=0;
};
struct GripAnchor { int32_t target=-1,palm=-1,target_palm=-1;double basis[4]{0,0,0,1};bool hand=false,forearm=false; };
struct GripHook {
    Identity component; uintptr_t* original = nullptr; uintptr_t* table = nullptr;
    std::array<GripSocket,32> sockets{}; size_t count=0; volatile LONG queries=0, failures=0;
    std::array<ForearmBinding,2> forearms{};
    std::vector<GripAnchor> anchors;
};
#pragma warning(pop)
struct SkeletonEdit { Identity skeleton; uintptr_t data = 0; bool old_flag = false; std::vector<uint8_t> original, written; };
struct FaceHook {
    Identity component, instance; Object* node = nullptr; int32_t instance_index = -1, instance_serial = 0;
    int32_t component_index = -1, component_serial = 0, runtime_bones = 0;
    uintptr_t* original = nullptr; std::unique_ptr<uintptr_t[]> table;
    const Profile* profile = nullptr; uint8_t old_curve = 0, old_attributes = 0;
    void* map_data = nullptr; int32_t map_slots = 0; uint64_t source_mesh = 0, target_mesh = 0;
    std::array<int32_t,861> saved{}; volatile LONG failed = 0; uint32_t epochs = 0;
    bool map_written = false; const char* map_state = "unseen"; const char* restore_reason = nullptr; ULONGLONG restore_log_time = 0;
    bool body=false;uint64_t old_source_input=0,source_input=0,source_asset_input=0;
    std::vector<int32_t> body_saved;
};
std::vector<std::unique_ptr<FaceHook>> face_hooks_;
std::vector<std::unique_ptr<uintptr_t[]>> retired_face_tables_;
std::vector<Visibility> face_visibility_;
DWORD face_thread_ = 0;
ULONGLONG face_wait_started_ = 0;
struct AnimOptions { Object* override_class = nullptr; int32_t lod = -1; bool disabled = false; };
AnimOptions primary_written_options_;
bool primary_options_written_ = false;
Identity pawn_, primary_, backup_, target_mesh_, original_class_;
Identity display_,source_asset_;
struct SourceSettings { bool visible=true,hidden=false,uro=false,cast_shadow=true,hidden_shadow=false;uint8_t tick_option=0;bool tick_enabled=false; };
SourceSettings source_settings_;
struct PoseSession {
    Identity source,display,source_asset,target_asset;std::array<int32_t,6> generations{};NpcPoseTransfer::Plan plan;NpcPoseTransfer::Cache cache;
    volatile LONG failed=0;const char* why="not-evaluated";
};
std::unique_ptr<PoseSession> pose_session_;
SRWLOCK pose_lock_=SRWLOCK_INIT;
std::vector<Identity> added_;
std::vector<Visibility> hidden_;
std::vector<SocketAdded> sockets_;
std::vector<Identity> socket_maps_;
SkeletonEdit skeleton_edit_;
SRWLOCK grip_lock_ = SRWLOCK_INIT;
std::unique_ptr<GripHook> grip_hook_;
// Like the engine Tick hook, retired vtable storage stays valid until process
// exit: a native caller may already have loaded the old table before restore.
std::vector<void*> retired_grip_tables_;

Object* Class(const char* path) { return ue::FindObject(path); }
bool Same(Identity identity) { return RawIdentity(identity,identity.index,identity.serial)&&ue::Alive(identity.object); }
Identity Id(Object* object) {
    Identity result;result.object=object;if(!object||!ue::Base()||!FaceReadable(object,0x28))return result;result.name=ue::NameOf(object);result.index=ue::At<int32_t>(object,ue::layout::kObjIndex);
    if(result.index<0||result.index>=ue::detail::ObjectCount()||ue::detail::ObjectAt(result.index)!=object)return result;
    auto* array=reinterpret_cast<Object*>(ue::Base()+ue::layout::kObjectArray);Object** chunks=ue::At<Object**>(array,24);const uint32_t chunk=static_cast<uint32_t>(result.index)>>16;
    if(!FaceReadable(chunks,(static_cast<size_t>(chunk)+1)*8)||!chunks[chunk])return result;Object* item=chunks[chunk]+(static_cast<uint32_t>(result.index)&65535)*24ull;
    if(FaceReadable(item,24))result.serial=ue::At<int32_t>(item,16);
    // Newly deferred components may not yet have any weak references. Let the
    // native FWeakObjectPtr constructor allocate its serial; never invent one.
    if(result.serial<=0){static constexpr unsigned char weak[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x12,0x48,0x8b,0xd9,0x48,0x85,0xd2,0x74};void* native=reinterpret_cast<void*>(ue::Base()+0x166f2a0);
        if(FaceReadable(native,sizeof(weak))&&!std::memcmp(native,weak,sizeof(weak))){int32_t value[2]{};reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(native)(value,&object);if(value[0]==result.index)result.serial=value[1];}}
    return result;
}
bool HasResources() {
    return pose_session_ || display_.object || primary_.object || backup_.object || !added_.empty() || !hidden_.empty() || !sockets_.empty() || skeleton_edit_.skeleton.object || !face_hooks_.empty() || grip_hook_;
}
std::string Text(Name name) { char text[192]{}; ue::NameText(name, text, sizeof(text)); return text; }
std::string Text(Object* object) { return object ? Text(ue::NameOf(object)) : "-"; }
std::string Lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
void Logf(const char* format, ...) {
    if (!logger_) return;
    char line[1000]{}; va_list arguments; va_start(arguments, format); vsprintf_s(line, format, arguments); va_end(arguments); logger_(line);
}
void Status(const wchar_t* format, ...) {
    wchar_t text[640]{}; va_list arguments; va_start(arguments, format); vswprintf_s(text, format, arguments); va_end(arguments);
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_.status = text; ReleaseSRWLockExclusive(&snapshot_lock);
}
std::string Utf8(const wchar_t* text) {
    if (!text || !*text) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (count <= 1) return {};
    std::string value(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, 0, text, -1, value.data(), count, nullptr, nullptr)) return {};
    value.pop_back(); return value;
}
bool Fail(const wchar_t* message) { failure_text_ = message; Status(L"NPC 重定向已停止：%s", message); Logf("NPC_RETARGET refused at %s: %s", stage_, Utf8(message).c_str()); return false; }
bool NativeHelpersMatch() {
    if (!ue::Base()) return false;
    for (const NativeSignature& signature : kNativeSignatures)
        if (std::memcmp(reinterpret_cast<const void*>(ue::Base() + signature.rva), signature.code, sizeof(signature.code))) return false;
    return true;
}
void Publish(bool busy = false) {
    AcquireSRWLockExclusive(&snapshot_lock);
    snapshot_.active = active_; snapshot_.busy = busy; snapshot_.parts = static_cast<int32_t>(added_.size());
    snapshot_.cleanup_pending = cleanup_pending_; snapshot_.disabled = InterlockedCompareExchange(&disabled_, 0, 0) != 0;
    snapshot_.appearance_gate = release_appearance_ != nullptr;
    snapshot_.experimental = InterlockedCompareExchange(&experimental_, 0, 0) != 0;
    snapshot_.sockets = grip_hook_?static_cast<int32_t>(grip_hook_->count):0; snapshot_.retargeted_bones = active_ && selected_ >= 0 ? static_cast<int32_t>(source_driver_mode_&&pose_session_?pose_session_->plan.mapped:kProfiles[selected_].shared_count) : 0;
    snapshot_.selected = active_ && selected_ >= 0 ? kProfiles[selected_].name : L"";
    ReleaseSRWLockExclusive(&snapshot_lock);
}
uintptr_t Field(Object* strukt, const char* property) {
    for (Object* type = strukt; type; type = ue::At<Object*>(type, ue::layout::kSuper)) {
        uintptr_t field = ue::At<uintptr_t>(type, ue::layout::kChildProperties);
        for (size_t guard = 0; field && guard < 4096; ++guard) {
            if (ue::NameIs(ue::At<Name>(reinterpret_cast<void*>(field), ue::layout::kPropName), property)) return field;
            field = ue::At<uintptr_t>(reinterpret_cast<void*>(field), ue::layout::kPropNext);
        }
    }
    return 0;
}
int32_t Offset(Object* object, const char* property) { return object ? ue::Offset(ue::ClassOf(object), property) : -1; }
template<class T> T Property(Object* object, const char* property) {
    const int32_t offset = Offset(object, property);
    return offset >= 0 ? ue::At<T>(object, offset) : T{};
}
template<class T> bool Put(Object* object, const char* property, const T& value) {
    const uintptr_t field = object ? Field(ue::ClassOf(object), property) : 0;
    if (!field || ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropElementSize) != static_cast<int32_t>(sizeof(T))) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    std::memcpy(object + offset, &value, sizeof(value)); return true;
}
bool Bool(Object* object, const char* property, bool& value) {
    const uintptr_t field = object ? Field(ue::ClassOf(object), property) : 0;
    if (!field || ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x70) != 1) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    const uint8_t byte_offset = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x71), mask = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x73);
    if (byte_offset > 7 || !mask) return false;
    value = (ue::At<uint8_t>(object, offset + byte_offset) & mask) != 0; return true;
}
bool PutBool(Object* object, const char* property, bool value) {
    const uintptr_t field = object ? Field(ue::ClassOf(object), property) : 0;
    if (!field || ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x70) != 1) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    const uint8_t byte_offset = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x71), byte_mask = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x72), mask = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x73);
    if (byte_offset > 7 || !mask || !byte_mask) return false;
    uint8_t& byte = ue::At<uint8_t>(object, offset + byte_offset);
    byte = static_cast<uint8_t>((byte & ~mask) | (value ? byte_mask : 0)); return true;
}
bool VisibilityOf(Object* object, bool& visible, bool& hidden) { return Bool(object, "bVisible", visible) && Bool(object, "bHiddenInGame", hidden); }
bool SetVisible(Object* object, bool visible, bool hidden) {
    Call show(object, "SetVisibility"), hide(object, "SetHiddenInGame");
    return show.Set("bNewVisibility", visible).Set("bPropagateToChildren", false).Invoke() &&
        hide.Set("NewHidden", hidden).Set("bPropagateToChildren", false).Invoke();
}
bool SetVisibleOwned(Object* object,bool visible,bool hidden,bool& visible_written,bool& hidden_written) {
    visible_written=true;if(!Call(object,"SetVisibility").Set("bNewVisibility",visible).Set("bPropagateToChildren",false).Invoke())return false;
    hidden_written=true;return Call(object,"SetHiddenInGame").Set("NewHidden",hidden).Set("bPropagateToChildren",false).Invoke();
}
bool RestoreVisibleOwned(Object* object,bool original_visible,bool original_hidden,bool& visible_written,bool& hidden_written) {
    bool visible=false,hidden=false;if(!VisibilityOf(object,visible,hidden))return false;
    if(visible_written){if(!visible&&!Call(object,"SetVisibility").Set("bNewVisibility",original_visible).Set("bPropagateToChildren",false).Invoke())return false;visible_written=false;}
    if(hidden_written){if(hidden&&!Call(object,"SetHiddenInGame").Set("NewHidden",original_hidden).Set("bPropagateToChildren",false).Invoke())return false;hidden_written=false;}return true;
}
Object* Kismet() { return ue::Default("/Script/Engine.Default__KismetSystemLibrary"); }
Object* Math() { return ue::Default("/Script/Engine.Default__KismetMathLibrary"); }
Object* SkelMesh(Object* component) {
    Call get(component, "GetSkeletalMeshAsset");
    return get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
}
Object* AnimClass(Object* component) { Call get(component, "GetAnimClass"); return get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr; }
Object* AnimInstance(Object* component) { Call get(component, "GetAnimInstance"); return get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr; }
bool SetMesh(Object* component, Object* mesh) {
    return Call(component, "SetSkeletalMeshAsset").Set("NewMesh", mesh).Invoke() && SkelMesh(component) == mesh;
}
bool SetPhysics(Object* component, Object* physics) {
    return Call(component, "SetPhysicsAsset").Set("NewPhysicsAsset", physics).Set("bForceReInit", true).Invoke();
}
bool ReadAnimOptions(Object* component, AnimOptions& options) {
    if (Offset(component, "OverridePostProcessAnimBP") < 0 || Offset(component, "PostProcessAnimBPLODThreshold") < 0 ||
        !Bool(component, "bDisablePostProcessBlueprint", options.disabled)) return false;
    options.override_class = Property<Object*>(component, "OverridePostProcessAnimBP");
    options.lod = Property<int32_t>(component, "PostProcessAnimBPLODThreshold");
    return !options.override_class || ue::Alive(options.override_class);
}
bool SameAnimOptions(const AnimOptions& a, const AnimOptions& b) {
    return a.override_class == b.override_class && a.lod == b.lod && a.disabled == b.disabled;
}
bool OwnedAnimOptions(const AnimOptions& current, const AnimOptions& original, const AnimOptions& written) {
    // A failed native setter may have completed only part of the write. Permit
    // our original/written values independently, but preserve foreign values.
    return (current.override_class == original.override_class || current.override_class == written.override_class) &&
        (current.lod == original.lod || current.lod == written.lod) &&
        (current.disabled == original.disabled || current.disabled == written.disabled);
}
bool WriteAnimOptions(Object* component, const AnimOptions& options) {
    // Native setters own animation-instance lifecycle. Do not copy transient
    // AnimInstance pointers or a template's node/proxy/DNA buffers.
    return Call(component, "SetOverridePostProcessAnimBP").Set("InPostProcessAnimBlueprint", options.override_class)
        .Set("ReinitAnimInstances", false).Invoke() &&
        Call(component, "SetDisablePostProcessBlueprint").Set("bInDisablePostProcess", options.disabled).Invoke() &&
        Put(component, "PostProcessAnimBPLODThreshold", options.lod);
}
bool ReinitializeAnimation(Object* component) {
    return Call(component, "SetAnimationMode").Set("InAnimationMode", uint8_t{0}).Set("bForceInitAnimScriptInstance", true).Invoke();
}
Object* EffectivePostClass(Object* component) {
    Object* override_class = Property<Object*>(component, "OverridePostProcessAnimBP");
    return override_class ? override_class : Property<Object*>(SkelMesh(component), "PostProcessAnimBlueprint");
}
bool VerifyPostProcess(Object* component, const AnimOptions& expected, const char* role) {
    AnimOptions actual;
    if (!ReadAnimOptions(component, actual) || !SameAnimOptions(actual, expected)) return false;
    Object* expected_class = EffectivePostClass(component), *instance = Property<Object*>(component, "PostProcessAnimInstance");
    // Disabled components can still hold an initialized instance, but a live
    // instance may never retain the previous asset's post-process class.
    const bool matches = expected_class ? (ue::Alive(instance) && ue::ClassOf(instance) == expected_class) : instance == nullptr;
    Logf("NPC_RETARGET_POST role=%s component=%s mesh=%s override=%s expected=%s instance=%s actualClass=%s disabled=%d lod=%d verified=%d",
        role, Text(component).c_str(), Text(SkelMesh(component)).c_str(), Text(actual.override_class).c_str(), Text(expected_class).c_str(),
        Text(instance).c_str(), Text(instance ? ue::ClassOf(instance) : nullptr).c_str(), actual.disabled ? 1 : 0, actual.lod, matches ? 1 : 0);
    return matches;
}
bool HasNoLeader(Object* component) {
    const int32_t offset = Offset(component, "LeaderPoseComponent");
    if (offset < 0) return false;
    // Reflected FWeakObjectPtr: null may use index 0 or INDEX_NONE; no serial
    // means it cannot identify a live leader. Never resolve raw UObject refs.
    const int32_t index = ue::At<int32_t>(component, offset), serial = ue::At<int32_t>(component, offset + 4);
    return serial == 0 && (index == 0 || index == -1);
}
bool AnimationApiReady(Object* component) {
    Call override_post(component, "SetOverridePostProcessAnimBP"), disable(component, "SetDisablePostProcessBlueprint"),
        mode(component, "SetAnimationMode"), prerequisite(component, "AddTickPrerequisiteComponent");
    return override_post.Slot("InPostProcessAnimBlueprint", sizeof(Object*)) && override_post.Slot("ReinitAnimInstances", sizeof(bool)) &&
        disable.Slot("bInDisablePostProcess", sizeof(bool)) && mode.Slot("InAnimationMode", sizeof(uint8_t)) &&
        mode.Slot("bForceInitAnimScriptInstance", sizeof(bool)) && prerequisite.Slot("PrerequisiteComponent", sizeof(Object*));
}
bool TransformOf(Object* component, Transform& result) {
    Call get(component, "GetRelativeTransform"); if (!get.Slot("ReturnValue", sizeof(result)) || !get.Invoke()) return false;
    result = get.Get<Transform>("ReturnValue"); return true;
}
bool Compose(const Transform& a, const Transform& b, Transform& result) {
    Call compose(Math(), "ComposeTransforms");
    if (!compose.Set("A", a).Set("B", b).Invoke()) return false;
    result = compose.Get<Transform>("ReturnValue"); return true;
}
bool Relative(const Transform& a, const Transform& b, Transform& result) {
    Call relative(Math(), "MakeRelativeTransform");
    if (!relative.Set("A", a).Set("RelativeTo", b).Invoke()) return false;
    result = relative.Get<Transform>("ReturnValue"); return true;
}
bool TransformNear(const Transform& expected, const Transform& actual) {
    double dot = 0;
    for (size_t i = 0; i < 4; ++i) dot += expected.rotation[i] * actual.rotation[i];
    if (std::abs(std::abs(dot) - 1) > 1e-5) return false;
    for (size_t i = 0; i < 3; ++i)
        if (std::abs(expected.translation[i] - actual.translation[i]) > 1e-7 || std::abs(expected.scale[i] - actual.scale[i]) > 1e-7) return false;
    return true;
}
std::vector<Object*> Components(Object* actor, Object* cls) {
    Call get(actor, "K2_GetComponentsByClass"); std::vector<Object*> result;
    if (!get.Set("ComponentClass", cls).Invoke()) return result;
    const Array array = get.Get<Array>("ReturnValue");
    if (array.num >= 0 && array.num < 2048 && (array.num == 0 || array.data))
        for (int32_t i = 0; i < array.num; ++i) if (ue::Alive(static_cast<Object**>(array.data)[i])) result.push_back(static_cast<Object**>(array.data)[i]);
    ue::Free(array.data); return result;
}
struct Context { Object* world = nullptr; Object* pawn = nullptr; Object* mesh = nullptr; };
Object* ObjectWorld(Object* object) {
    Object* world_class = Class("/Script/Engine.World"), *level_class = Class("/Script/Engine.Level");
    for (size_t i = 0; ue::Alive(object) && i < 32; ++i, object = ue::OuterOf(object)) {
        if (ue::IsA(object, world_class)) return object;
        if (ue::IsA(object, level_class)) return Property<Object*>(object, "OwningWorld");
    }
    return nullptr;
}
Object* ForeignSkeletonUser(const Context& context, Object* skeleton) {
    Object* conflict = nullptr;
    Object* skinned_class = Class("/Script/Engine.SkinnedMeshComponent"), *skeletal_class = Class("/Script/Engine.SkeletalMeshComponent");
    ue::ForEachObject([&](Object* component) {
        if (ue::IsTemplate(component) || !ue::Alive(component) || !ue::IsA(component, skinned_class)) return true;
        Object* asset = ue::IsA(component, skeletal_class) ? SkelMesh(component) : Property<Object*>(component, "SkinnedAsset");
        if (!asset || Property<Object*>(asset, "Skeleton") != skeleton) return true;
        Call get_owner(component, "GetOwner");
        Object* owner = get_owner.Invoke() ? get_owner.Get<Object*>("ReturnValue") : nullptr;
        // Include hidden/unregistered live components conservatively: they may
        // become active without another Apply. Never mistake the unregistered
        // original backup or our new followers for a foreign actor.
        if (owner == context.pawn || component == context.mesh || component == backup_.object ||
            std::any_of(added_.begin(), added_.end(), [&](Identity id) { return Same(id) && id.object == component; })) return true;
        if (ObjectWorld(owner ? owner : component) != context.world) return true;
        conflict = component; return false;
    });
    return conflict;
}
bool ContextNow(Context& context) {
    if (!ue::Alive(engine_)) engine_ = ue::FindEngine();
    Object* viewport = Property<Object*>(engine_, "GameViewport");
    context.world = Property<Object*>(viewport, "World");
    if (!ue::Alive(context.world)) return false;
    Object* instance = Property<Object*>(context.world, "OwningGameInstance");
    const Array players = Property<Array>(instance, "LocalPlayers");
    if (players.num < 1 || players.num > 16 || !players.data) return false;
    Object* local = static_cast<Object**>(players.data)[0], *controller = Property<Object*>(local, "PlayerController");
    context.pawn = Property<Object*>(controller, "Pawn"); context.mesh = Property<Object*>(context.pawn, "Mesh");
    return ue::Alive(context.pawn) && ue::Alive(context.mesh) && SkelMesh(context.mesh) && AnimClass(context.mesh);
}
bool Idle(const Context& context) {
    Object* instance = AnimInstance(context.mesh);
    if (!instance) return false;
    Call montage(instance, "IsAnyMontagePlaying");
    if (!montage.Slot("ReturnValue", sizeof(bool)) || !montage.Invoke() || montage.Get<bool>("ReturnValue")) return false;
    Call velocity(context.pawn, "GetVelocity");
    if (!velocity.Invoke()) return false;
    const Vector v = velocity.Get<Vector>("ReturnValue");
    if (!std::isfinite(v.x + v.y + v.z) || v.x*v.x + v.y*v.y + v.z*v.z > 100) return false;
    Object* movement = Property<Object*>(context.pawn, "CharacterMovement");
    const uint8_t mode = Property<uint8_t>(movement, "MovementMode");
    if (mode != 1 && mode != 2) return false;
    bool sequence_playing = false;
    Object* sequence_class = Class("/Script/LevelSequence.LevelSequenceActor");
    if (sequence_class) ue::ForEachObject([&](Object* actor) {
        if (ue::IsTemplate(actor) || !ue::Alive(actor) || !ue::IsA(actor, sequence_class)) return true;
        Object* outer = ue::OuterOf(actor); bool same_world = false;
        for (size_t i = 0; outer && i < 8; ++i, outer = ue::OuterOf(outer)) if (outer == context.world) { same_world = true; break; }
        if (!same_world) return true;
        Object* player = Property<Object*>(actor, "SequencePlayer");
        if (!ue::Alive(player)) return true;
        Call playing(player, "IsPlaying");
        if (playing.Invoke() && playing.Get<bool>("ReturnValue")) { sequence_playing = true; return false; }
        return true;
    });
    if (sequence_playing) return false;
    // The primary being hidden indicates a cinematic/preview or a special pawn state.
    bool visible = false, hidden = true;
    return VisibilityOf(context.mesh, visible, hidden) && ((visible && !hidden) ||
        (source_driver_mode_ && source_settings_owned_ && Same(primary_) && context.mesh==primary_.object));
}
Object* Add(Object* actor, Object* cls, const Transform& relative = {}) {
    Call add(actor, "AddComponentByClass");
    if (!add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", relative).Set("bDeferredFinish", true).Invoke()) return nullptr;
    return add.Get<Object*>("ReturnValue");
}
bool Destroy(Object* component) {
    if (!ue::Alive(component)) return true;
    return Call(component, "K2_DestroyComponent").Set("Object", component).Invoke() && !ue::Alive(component);
}
bool AttachFinish(Object* actor, Object* component, Object* parent, Name socket, const Transform& relative) {
    Call attach(component, "K2_AttachToComponent");
    if (!attach.Set("Parent", parent).Set("SocketName", socket).Set("LocationRule", uint8_t{0}).Set("RotationRule", uint8_t{0}).Set("ScaleRule", uint8_t{0}).Set("bWeldSimulatedBodies", false).Invoke() || !attach.Get<bool>("ReturnValue")) return false;
    if (!Call(actor, "FinishAddComponent").Set("Component", component).Set("bManualAttachment", true).Set("RelativeTransform", relative).Invoke()) return false;
    Transform actual;
    if (Property<Object*>(component, "AttachParent") != parent || !(Property<Name>(component, "AttachSocketName") == socket) || !TransformOf(component, actual)) return false;
    double rotation_dot = 0;
    for (size_t i = 0; i < 4; ++i) rotation_dot += relative.rotation[i] * actual.rotation[i];
    if (std::abs(std::abs(rotation_dot) - 1) > 1e-5) return false;
    for (size_t i = 0; i < 3; ++i)
        if (std::abs(relative.translation[i] - actual.translation[i]) > 0.01 || std::abs(relative.scale[i] - actual.scale[i]) > 1e-5) return false;
    return true;
}
bool CopyMaterials(Object* from, Object* to) {
    const int32_t offset = Offset(to, "OverrideMaterials");
    if (offset < 0) return false;
    const Array source = Property<Array>(from, "OverrideMaterials");
    if (source.num < 0 || source.num > 128 || (source.num && !source.data)) return false;
    // Keep the UE allocation. Backup owns the original references; SetMaterial
    // uses the engine allocator if the destination needs more capacity.
    ue::At<Array>(to, offset).num = 0;
    for (int32_t i = 0; i < source.num; ++i)
        if (!Call(to, "SetMaterial").Set("ElementIndex", i).Set("Material", static_cast<Object**>(source.data)[i]).Invoke()) return false;
    Array& dest = ue::At<Array>(to, offset);
    if (dest.max < source.num) return false;
    // Preserve trailing null override slots too; they are valid UObject nulls.
    if (source.num) std::memcpy(dest.data, source.data, source.num * sizeof(Object*));
    dest.num = source.num; return true;
}
[[maybe_unused]] bool Backup(const Context& context) {
    AnimOptions options;
    if (!ReadAnimOptions(context.mesh, options)) return Fail(L"无法读取原主角 PostProcess 设置，未开始替换");
    Object* backup = Add(context.pawn, Class("/Script/Engine.SkeletalMeshComponent"));
    if (!backup) return Fail(L"无法创建 GC 持有原网格的备份组件");
    backup_ = Id(backup);
    if (!SetMesh(backup, SkelMesh(context.mesh)) || !CopyMaterials(context.mesh, backup) || !Put(backup, "AnimClass", AnimClass(context.mesh)) ||
        !Put(backup, "PhysicsAssetOverride", Property<Object*>(context.mesh, "PhysicsAssetOverride")) ||
        !Put(backup, "OverridePostProcessAnimBP", options.override_class) || !PutBool(backup, "bDisablePostProcessBlueprint", options.disabled) ||
        !Put(backup, "PostProcessAnimBPLODThreshold", options.lod)) return false;
    SetVisible(backup, false, true);
    Call(backup, "SetComponentTickEnabled").Set("bEnabled", false).Invoke();
    // Intentionally do not FinishAddComponent: PostCreateBlueprintComponent put
    // it in the actor's reflected BlueprintCreatedComponents array. It holds the
    // original assets/classes without registering a second PBZ AnimInstance.
    return true;
}
[[maybe_unused]] bool PatchSkeleton(Object* skeleton, const Profile& profile) {
    if (!ue::Alive(skeleton) || ue::Offset(Class("/Script/Engine.Skeleton"), "BoneTree") != 0x38 ||
        ue::Offset(Class("/Script/Engine.Skeleton"), "bUseRetargetModesFromCompatibleSkeleton") != 0x1a0) return Fail(L"Skeleton 原生字段布局不符");
    const Array tree = Property<Array>(skeleton, "BoneTree");
    if (!tree.data || tree.num != profile.bones || tree.max < tree.num || tree.max > 16384) return Fail(L"目标 Skeleton 骨数与审计资料不符");
    auto* data = static_cast<uint8_t*>(tree.data);
    skeleton_edit_.skeleton = Id(skeleton); skeleton_edit_.data = reinterpret_cast<uintptr_t>(data);
    if (!Bool(skeleton, "bUseRetargetModesFromCompatibleSkeleton", skeleton_edit_.old_flag)) return false;
    skeleton_edit_.original.assign(data, data + tree.num); skeleton_edit_.written = skeleton_edit_.original;
    for (uint8_t mode : skeleton_edit_.original) if (mode > 4) return Fail(L"目标 Skeleton 含未知重定向模式");
    for (size_t i = 0; i < profile.shared_count; ++i) {
        const uint16_t index = profile.shared[i]; if (index >= tree.num) return false;
        skeleton_edit_.written[index] = index == 0 ? 0 : 3;
    }
    if (!PutBool(skeleton, "bUseRetargetModesFromCompatibleSkeleton", false)) return Fail(L"无法设置目标骨架的原生重定向开关");
    std::memcpy(data, skeleton_edit_.written.data(), tree.num);
    Logf("NPC_RETARGET native AnimationRelative applied bones=%zu target=%s flag=0", profile.shared_count, Text(skeleton).c_str()); return true;
}
bool RestoreSkeleton() {
    if (!skeleton_edit_.skeleton.object) return true;
    if (!Same(skeleton_edit_.skeleton)) { skeleton_edit_ = SkeletonEdit{}; return true; }
    bool complete = true;
    if (Same(skeleton_edit_.skeleton)) {
        Object* skeleton = skeleton_edit_.skeleton.object;
        const Array tree = Property<Array>(skeleton, "BoneTree");
        if (reinterpret_cast<uintptr_t>(tree.data) == skeleton_edit_.data && static_cast<size_t>(tree.num) == skeleton_edit_.original.size()) {
            auto* modes = static_cast<uint8_t*>(tree.data);
            for (size_t i = 0; i < skeleton_edit_.original.size(); ++i) {
                if (skeleton_edit_.original[i] == skeleton_edit_.written[i]) continue;
                if (modes[i] == skeleton_edit_.written[i]) modes[i] = skeleton_edit_.original[i];
                if (modes[i] == skeleton_edit_.original[i]) skeleton_edit_.written[i] = skeleton_edit_.original[i];
                else complete = false;
            }
        } else { Logf("NPC_RETARGET bone array identity changed; restore skipped"); complete = false; }
        // A false original flag was never changed, so do not undo a later
        // external true value. For an owned true->false edit, true is already
        // the original state and false is the one state we may restore.
        if (skeleton_edit_.old_flag) {
            bool flag = false;
            if (!Bool(skeleton, "bUseRetargetModesFromCompatibleSkeleton", flag)) complete = false;
            else if (!flag && !PutBool(skeleton, "bUseRetargetModesFromCompatibleSkeleton", true)) complete = false;
        }
    }
    if (complete) skeleton_edit_ = SkeletonEdit{};
    else Logf("NPC_RETARGET skeleton restore unresolved; retained owned mode ledger for retry");
    return complete;
}
bool ArrayField(Object* owner, const char* name, uintptr_t& field, Array*& array) {
    field = owner ? Field(ue::ClassOf(owner), name) : 0;
    if (!field || ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropElementSize) != static_cast<int32_t>(sizeof(Array))) return false;
    const uintptr_t inner = ue::At<uintptr_t>(reinterpret_cast<void*>(field), 0x78);
    if (!inner || ue::At<int32_t>(reinterpret_cast<void*>(inner), ue::layout::kPropElementSize) != static_cast<int32_t>(sizeof(Object*))) return false;
    array = &ue::At<Array>(owner, ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset));
    return array->num >= 0 && array->num <= array->max && array->max <= 65536 && (!array->num || array->data);
}
bool SocketExists(Object* component, Name name) {
    Call exists(component, "DoesSocketExist");
    return exists.Set("InSocketName", name).Invoke() && exists.Get<bool>("ReturnValue");
}
bool RebuildSockets(Object* mesh) {
    if (!kRebuildSocketMapRva) return false;
    reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base() + kRebuildSocketMapRva)(mesh); return true;
}
bool QuiesceSkeleton(Object* skeleton) {
    Object* component_class = Class("/Script/Engine.SkeletalMeshComponent");
    if (!NativeHelpersMatch() || !component_class || ue::StructSize(component_class) < 0xdf0) return false;
    bool ok = true; size_t waited = 0;
    // Exact copy of the wait/complete sequence in this build's native
    // USkeletalMeshComponent::SetSkeletalMesh (3dbb2aa..3dbb2e5). Complete is
    // virtual because the budgeted component wraps it for its timing metrics.
    ue::ForEachObject([&](Object* component) {
        if (ue::IsTemplate(component) || !ue::Alive(component) || !ue::IsA(component, component_class)) return true;
        Object* mesh = SkelMesh(component);
        if (!mesh || Property<Object*>(mesh, "Skeleton") != skeleton) return true;
        void* task = ue::At<void*>(component, 0xde8);
        if (!task) return true;
        if (ue::At<DWORD>(task, 0x20) == GetCurrentThreadId()) { ok = false; return false; }
        const uintptr_t method = ue::At<uintptr_t*>(component, 0)[0xd00 / sizeof(uintptr_t)];
        if (method != ue::Base() + 0x3d9c240 && method != ue::Base() + 0x5c84580) { ok = false; return false; }
        void* graph = reinterpret_cast<void*(__fastcall*)()>(ue::Base() + 0x1268710)();
        if (!graph) { ok = false; return false; }
        reinterpret_cast<void(__fastcall*)(void*, const void*, uint32_t)>(ue::Base() + 0x1f27f90)(graph, component + 0xde8, 1);
        reinterpret_cast<void(__fastcall*)(Object*, bool)>(method)(component, true);
        if (ue::At<void*>(component, 0xde8)) { ok = false; return false; }
        ++waited; return true;
    });
    if (waited) Logf("NPC_RETARGET synchronized %zu target-skeleton animation tasks without resetting instances", waited);
    return ok;
}
bool FaceReadable(const void* pointer, size_t bytes) {
    if (!pointer) return false;
    MEMORY_BASIC_INFORMATION memory{};
    if (!VirtualQuery(pointer, &memory, sizeof(memory)) || memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    const uintptr_t at = reinterpret_cast<uintptr_t>(pointer), start = reinterpret_cast<uintptr_t>(memory.BaseAddress);
    return at >= start && bytes <= memory.RegionSize && at - start <= memory.RegionSize - bytes;
}
bool ReadMeshReferences(Object* mesh,std::vector<NpcPoseTransfer::Bone>& bones,size_t& raw_count) {
    if(!ue::Alive(mesh)||!ue::IsA(mesh,Class("/Script/Engine.SkeletalMesh"))||!NativeHelpersMatch())return false;
    const uintptr_t method=ue::At<uintptr_t*>(mesh,0)[0x320/8];if(method!=ue::Base()+kGetMeshReferenceRva)return false;
    Object* ref=reinterpret_cast<Object*(__fastcall*)(Object*)>(method)(mesh);if(ref!=mesh+0x2e0||!FaceReadable(ref,0x40))return false;
    const Array raw=ue::At<Array>(ref,0),info=ue::At<Array>(ref,0x20),pose=ue::At<Array>(ref,0x30);
    if(raw.num<1||raw.num>info.num||info.num>2048||info.max<info.num||info.max>4096||pose.num!=info.num||pose.max<pose.num||pose.max>4096||
        !FaceReadable(raw.data,static_cast<size_t>(raw.num)*12)||!FaceReadable(info.data,static_cast<size_t>(info.num)*12)||!FaceReadable(pose.data,static_cast<size_t>(pose.num)*96))return false;
    bones.clear();bones.reserve(static_cast<size_t>(info.num));
    for(int32_t i=0;i<info.num;++i){Object* at=static_cast<Object*>(info.data)+static_cast<size_t>(i)*12;NpcPoseTransfer::Bone bone;
        std::memcpy(&bone.name,at,8);bone.parent=ue::At<int32_t>(at,8);std::memcpy(&bone.ref,static_cast<Object*>(pose.data)+static_cast<size_t>(i)*96,96);
        const Name name=ue::At<Name>(at,0);if(name.None()||!NpcGripMath::Valid(bone.ref)||(i==0?bone.parent!=-1:(bone.parent<0||bone.parent>=i)))return false;
        if(i<raw.num&&std::memcmp(at,static_cast<Object*>(raw.data)+static_cast<size_t>(i)*12,12))return false;bones.push_back(bone);}
    raw_count=static_cast<size_t>(raw.num);return true;
}
bool PreparePoseSession(Object* source,Object* target_asset,const Profile& profile) {
    auto session=std::make_unique<PoseSession>();session->source=Id(source);session->source_asset=Id(SkelMesh(source));session->target_asset=Id(target_asset);
    std::vector<NpcPoseTransfer::Bone> source_refs,target_refs;size_t source_raw=0,target_raw=0;std::string why;
    if(!ReadMeshReferences(session->source_asset.object,source_refs,source_raw)||!ReadMeshReferences(target_asset,target_refs,target_raw)||target_raw!=static_cast<size_t>(profile.bones))return false;
    std::vector<Transform> source_cs,target_cs;if(!NpcPoseTransfer::ReferenceCS(source_refs,source_cs,why)||!NpcPoseTransfer::ReferenceCS(target_refs,target_cs,why))return false;
    std::map<uint64_t,size_t> source_names;for(size_t i=0;i<source_refs.size();++i)source_names.emplace(source_refs[i].name,i);
    std::map<std::string,std::array<double,4>> skeleton_bases;
    for(const BodySkeletonBasis& row:kBodySkeletonBases)if(Lower(row.profile)==Lower(profile.id)){std::array<double,4> q{};std::copy(std::begin(row.q0),std::end(row.q0),q.begin());skeleton_bases.emplace(Lower(row.bone),q);}
    std::vector<std::array<double,4>> q0(target_refs.size(),{0,0,0,1});
    for(size_t t=0;t<target_raw;++t){auto found=source_names.find(target_refs[t].name);if(found==source_names.end())continue;Name name{};std::memcpy(&name,&target_refs[t].name,8);auto basis=skeleton_bases.find(Lower(Text(name)));
        if(basis==skeleton_bases.end())return false;q0[t]=basis->second;}
    if(!NpcPoseTransfer::Build(session->plan,std::move(source_refs),std::move(target_refs),q0,why,target_raw)){Logf("NPC_BODY plan refused reason=%s",why.c_str());return false;}
    Object* identities[]={session->source.object,session->source_asset.object,session->target_asset.object};for(size_t i=0;i<3;++i)reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166f2a0)(&session->generations[i*2],&identities[i]);
    NpcPoseTransfer::Prepare(session->plan,session->cache);
    Logf("NPC_BODY prepared sourceRaw=%zu sourceRuntime=%zu targetRaw=%zu targetRuntime=%zu mapped=%zu sourceGeneration=%s target=%s sharedAssetWrites=0",
        source_raw,session->plan.source.size(),target_raw,session->plan.target.size(),session->plan.mapped,Text(session->source_asset.object).c_str(),Text(target_asset).c_str());
    AcquireSRWLockExclusive(&pose_lock_);pose_session_=std::move(session);ReleaseSRWLockExclusive(&pose_lock_);return true;
}
bool SourceCurrentCS(Object* source,const Transform*& transforms,size_t& count) {
    if(!source||!FaceReadable(source,0x650))return false;const int32_t read=ue::At<int32_t>(source,0x640);if(read<0||read>1)return false;
    const Array current=ue::At<Array>(source,0x5f8+read*16);
    if(current.num<1||current.num>2048||current.max<current.num||current.max>4096||!FaceReadable(current.data,static_cast<size_t>(current.num)*96))return false;
    transforms=static_cast<const Transform*>(current.data);count=static_cast<size_t>(current.num);return true;
}
bool UpdatePoseCache(const Transform* source,size_t count) {
    if(!pose_session_)return false;const char* why="ok";
    if(!NpcPoseTransfer::Evaluate(pose_session_->plan,source,count,pose_session_->cache,why)){pose_session_->why=why;InterlockedExchange(&pose_session_->failed,1);return false;}
    pose_session_->why="ok";return true;
}
bool RawIdentity(Identity identity, int32_t index, int32_t serial) {
    if (!identity.object || index<0 || serial<=0 || !FaceReadable(identity.object,0x28) ||
        index>=ue::detail::ObjectCount() || ue::detail::ObjectAt(index)!=identity.object) return false;
    auto* array=reinterpret_cast<Object*>(ue::Base()+ue::layout::kObjectArray);
    Object** chunks=ue::At<Object**>(array,24); const uint32_t chunk=static_cast<uint32_t>(index)>>16;
    if (!FaceReadable(chunks,(static_cast<size_t>(chunk)+1)*8)||!chunks[chunk]) return false;
    Object* item=chunks[chunk]+(static_cast<uint32_t>(index)&65535)*24ull;
    return FaceReadable(item,24) && ue::At<int32_t>(item,16)==serial && ue::NameOf(identity.object)==identity.name;
}
bool PoseIdentityValid() {
    if(!pose_session_)return false;const auto& p=*pose_session_;return RawIdentity(p.source,p.generations[0],p.generations[1])&&RawIdentity(p.source_asset,p.generations[2],p.generations[3])&&RawIdentity(p.target_asset,p.generations[4],p.generations[5]);
}
bool FaceRawInstance(const FaceHook& hook) { return RawIdentity(hook.instance,hook.instance_index,hook.instance_serial); }
bool FaceQuiesce(Object* component) {
    if (!Same(Id(component)) || !NativeHelpersMatch()) return false;
    void* task = ue::At<void*>(component, 0xde8); if (!task) return true;
    if (!FaceReadable(task, 0x24) || ue::At<DWORD>(task, 0x20) == GetCurrentThreadId()) return false;
    const uintptr_t method = ue::At<uintptr_t*>(component, 0)[0xd00 / 8];
    if (method != ue::Base() + 0x3d9c240 && method != ue::Base() + 0x5c84580) return false;
    void* graph = reinterpret_cast<void*(__fastcall*)()>(ue::Base() + 0x1268710)(); if (!graph) return false;
    reinterpret_cast<void(__fastcall*)(void*, const void*, uint32_t)>(ue::Base() + 0x1f27f90)(graph, component + 0xde8, 1);
    reinterpret_cast<void(__fastcall*)(Object*, bool)>(method)(component, true);
    return !ue::At<void*>(component, 0xde8);
}
bool FaceNativeMatch() {
    static constexpr unsigned char pre[] = {0x40,0x53,0x55,0x56,0x48,0x83,0xec,0x40,0x48,0x8b,0xf1,0x48,0x8b,0xea,0x48,0x8b};
    static constexpr unsigned char evaluate[] = {0x4c,0x8b,0xdc,0x56,0x57,0x41,0x54,0x48,0x81,0xec,0xc0,0,0,0,0x48,0x8b};
    static constexpr unsigned char weak[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x12,0x48,0x8b,0xd9,0x48,0x85,0xd2,0x74};
    return ue::Base() && !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x35d6120),pre,sizeof(pre)) &&
        !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x35bd160),evaluate,sizeof(evaluate)) &&
        !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x166f2a0),weak,sizeof(weak));
}
bool FaceMap(FaceHook& hook, bool restore) {
    Object* node = hook.node; const Profile& profile = *hook.profile;
    auto refuse=[&](const char* state) { hook.map_state=state; return false; };
    if (restore && !hook.map_written) { hook.map_state="no-owned-map-writes"; return true; }
    if (profile.head_bones != 861 || profile.facial_count != 843 || !FaceReadable(node, 344)) return refuse("node-layout");
    // Exact PE: BoneMapToSource@48, TSet pair stride16, int key@0/value@4.
    // Change values only. UE still owns allocation, hash chains, keys/capacity.
    Object* map = node + 0x48; void* data = ue::At<void*>(map,0);
    const int32_t slots = ue::At<int32_t>(map,8), capacity = ue::At<int32_t>(map,12), bits = ue::At<int32_t>(map,40),
        max_bits = ue::At<int32_t>(map,44), free = ue::At<int32_t>(map,52);
    const uint64_t source_mesh = ue::At<uint64_t>(node,0x2c), target_mesh = ue::At<uint64_t>(node,0x3c);
    const bool same_epoch=data==hook.map_data && slots==hook.map_slots && source_mesh==hook.source_mesh && target_mesh==hook.target_mesh;
    // Native may replace or clear the map while retaining this AnimInstance.
    // That new allocation/mesh epoch contains no writes owned by this ledger.
    // Check identity before shape, including a valid native empty-map reset.
    if (restore && !same_epoch) { hook.map_written=false; hook.map_state="native-map-epoch-retired"; return true; }
    const int32_t expected=hook.runtime_bones?hook.runtime_bones:profile.head_bones;
    if (expected<profile.head_bones || expected>2048 || slots<expected || slots>2048 || capacity<slots || capacity>4096 || bits!=slots || max_bits<bits ||
        free<0 || slots-free!=expected || !FaceReadable(data,static_cast<size_t>(slots)*16)) return refuse("map-shape");
    const uint32_t* allocated = ue::At<uint32_t*>(map,32);
    if (!allocated) { if (bits>128) return refuse("map-inline-bitmap"); allocated=reinterpret_cast<uint32_t*>(map+16); }
    if (!FaceReadable(allocated,((static_cast<size_t>(bits)+31)/32)*4)) return refuse("map-bitmap");
    std::array<bool,861> facial{}; std::array<bool,2048> seen{}; std::array<Object*,861> entries{};
    for (size_t i=0;i<profile.facial_count;++i) { if (profile.facial_bones[i]>=861) return refuse("profile-facial-index"); facial[profile.facial_bones[i]]=true; }
    size_t filtered=0;
    for (int32_t i=0;i<slots;++i) if (allocated[i/32] & (1u << (i%32))) {
        Object* entry = static_cast<Object*>(data)+static_cast<size_t>(i)*16;
        const int32_t key=ue::At<int32_t>(entry,0), value=ue::At<int32_t>(entry,4);
        if (key<0 || key>=expected || seen[key]) return refuse("map-duplicate-or-range"); seen[key]=true;
        // Runtime reference skeletons can append virtual bones. Keep them and
        // their source mapping intact; only the 843 audited facial keys change.
        if (key>=profile.head_bones) continue;
        if (!facial[key]) { if (!restore && key<10 && value<0) return refuse("head-neck-source-missing"); continue; }
        entries[key]=entry; ++filtered;
    }
    if (filtered!=843 || !std::all_of(seen.begin(),seen.begin()+expected,[](bool value){return value;})) return refuse("map-incomplete");
    // Validate every key/hash/bitmap before committing any facial write. A
    // failed initial installation must have no half-written map to unwind.
    if (!restore && !same_epoch) {
        hook.saved.fill(std::numeric_limits<int32_t>::min()); hook.map_data=data; hook.map_slots=slots;
        hook.source_mesh=source_mesh; hook.target_mesh=target_mesh;
    }
    for (size_t key=0;key<entries.size();++key) if (Object* entry=entries[key]) {
        int32_t& value=ue::At<int32_t>(entry,4);
        if (restore) {
            if (value == -1 && hook.saved[key] != std::numeric_limits<int32_t>::min()) value=hook.saved[key];
        } else {
            if (value != -1 || hook.saved[key] == std::numeric_limits<int32_t>::min()) hook.saved[key]=value;
            value=-1; // Evaluate's explicit negative-index branch keeps this bone's mesh reference pose.
        }
    }
    hook.map_written=!restore; hook.map_state=restore?"owned-map-restored":"facial-filtered"; return true;
}
bool BodyMap(FaceHook& hook,bool restore) {
    if(restore&&!hook.map_written)return true;if(!pose_session_||!FaceReadable(hook.node,344))return false;
    Object* map=hook.node+0x48;void* data=ue::At<void*>(map,0);const int32_t slots=ue::At<int32_t>(map,8),capacity=ue::At<int32_t>(map,12),bits=ue::At<int32_t>(map,40),maxbits=ue::At<int32_t>(map,44),free=ue::At<int32_t>(map,52);
    const uint64_t source=ue::At<uint64_t>(hook.node,0x2c),target=ue::At<uint64_t>(hook.node,0x3c);const bool same=data==hook.map_data&&slots==hook.map_slots&&source==hook.source_mesh&&target==hook.target_mesh;
    if(restore&&!same){hook.map_written=false;hook.map_state="body-map-epoch-retired";return true;}
    const auto& plan=pose_session_->plan;const size_t count=plan.target.size();if(count!=static_cast<size_t>(hook.runtime_bones)||hook.body_saved.size()!=count||slots<static_cast<int32_t>(count)||slots>2048||capacity<slots||capacity>4096||bits!=slots||maxbits<bits||free<0||slots-free!=static_cast<int32_t>(count)||!FaceReadable(data,static_cast<size_t>(slots)*16))return false;
    const uint32_t* allocated=ue::At<uint32_t*>(map,32);if(!allocated){if(bits>128)return false;allocated=reinterpret_cast<uint32_t*>(map+16);}if(!FaceReadable(allocated,((static_cast<size_t>(bits)+31)/32)*4))return false;
    std::array<bool,2048> seen{};std::array<Object*,2048> virtuals{};
    for(int32_t i=0;i<slots;++i)if(allocated[i/32]&(1u<<(i%32))){Object* row=static_cast<Object*>(data)+static_cast<size_t>(i)*16;const int32_t key=ue::At<int32_t>(row,0),value=ue::At<int32_t>(row,4);
        if(key<0||static_cast<size_t>(key)>=count||seen[static_cast<size_t>(key)])return false;seen[static_cast<size_t>(key)]=true;
        if(static_cast<size_t>(key)>=plan.target_raw)virtuals[static_cast<size_t>(key)]=row;
        else if(!restore&&value!=plan.target_to_source[static_cast<size_t>(key)])return false;}
    if(!std::all_of(seen.begin(),seen.begin()+count,[](bool b){return b;}))return false;
    if(!restore&&!same){std::fill(hook.body_saved.begin(),hook.body_saved.end(),std::numeric_limits<int32_t>::min());hook.map_data=data;hook.map_slots=slots;hook.source_mesh=source;hook.target_mesh=target;}
    bool wrote=false;for(size_t i=plan.target_raw;i<count;++i)if(virtuals[i]){int32_t& value=ue::At<int32_t>(virtuals[i],4);if(restore){if(value==-1&&hook.body_saved[i]!=std::numeric_limits<int32_t>::min())value=hook.body_saved[i];}
        else{if(value!=-1||hook.body_saved[i]==std::numeric_limits<int32_t>::min())hook.body_saved[i]=value;value=-1;wrote=true;}}
    hook.map_written=!restore&&wrote;hook.map_state=restore?"body-virtual-map-restored":"body-raw-map-verified-virtual-ref";return true;
}
bool RewriteBodyCache(FaceHook& hook) {
    if(!PoseIdentityValid()||!Same(pose_session_->source)||!Same(pose_session_->source_asset)||!Same(pose_session_->target_asset)||
        SkelMesh(pose_session_->source.object)!=pose_session_->source_asset.object||SkelMesh(hook.component.object)!=pose_session_->target_asset.object)return false;
    if(!BodyMap(hook,false))return false;
    const Array cache=ue::At<Array>(hook.node,0xe8);if(cache.num<1||cache.max<cache.num||cache.max>4096||!FaceReadable(cache.data,static_cast<size_t>(cache.num)*96))return false;
    const Transform* current=nullptr;size_t count=0;if(!SourceCurrentCS(pose_session_->source.object,current,count)||count!=static_cast<size_t>(cache.num))return false;
    bool ok=false;AcquireSRWLockExclusive(&pose_lock_);
    __try { ok=UpdatePoseCache(current,count);if(ok)std::memcpy(cache.data,pose_session_->cache.synthetic_source.data(),static_cast<size_t>(cache.num)*96); }
    __finally { ReleaseSRWLockExclusive(&pose_lock_); }return ok;
}
void FacePreUpdateBody(Object* node, Object* instance) {
    FaceHook* hook = nullptr;
    for (const auto& candidate:face_hooks_) if (candidate->node==node && candidate->instance.object==instance) { hook=candidate.get(); break; }
    if (!hook) { reinterpret_cast<void(__fastcall*)(Object*,Object*)>(ue::Base()+0x35d6120)(node,instance); return; }
    if (GetCurrentThreadId()!=face_thread_) { InterlockedExchange(&hook->failed,1); return; }
    if(hook->source_input)ue::At<uint64_t>(node,0x10)=hook->source_input;
    // PreUpdate is the engine's GT stage before parallel graph evaluation.
    // Reassert before native caching; then filter every rebuilt map afterward.
    ue::At<uint8_t>(node,0x18) &= static_cast<uint8_t>(~2u);
    ue::At<uint8_t>(node,0x19) = 0;
    reinterpret_cast<void(__fastcall*)(Object*,Object*)>(ue::Base()+0x35d6120)(node,instance);
    if(hook->source_input&&(ue::At<uint64_t>(node,0x24)!=hook->source_input||ue::At<uint64_t>(node,0x34)!=hook->source_asset_input)){InterlockedExchange(&hook->failed,1);return;}
    const LONGLONG cache_started=hook->body?PerfStart():0;const bool mapped=hook->body?RewriteBodyCache(*hook):FaceMap(*hook,false);if(hook->body)PerfEnd(PerfKind::BodyCache,cache_started);
    if(!mapped){InterlockedExchange(&hook->failed,1);return;}
    if(hook->body){if(++hook->epochs==1)Logf("NPC_BODY CopyPose private owned cache retarget active mapped=%zu targetRaw=%zu targetRuntime=%zu nativeSourceUnchanged=1",pose_session_->plan.mapped,pose_session_->plan.target_raw,pose_session_->plan.target.size());return;}
    if (++hook->epochs==1) Logf("NPC_FACE_NEUTRAL source=%s mappedFacial=0 meshRefFacial=843 headNeckCopied=10 runtimeBones=%d rawBones=%d curves=0 attributes=0 private-node=1",hook->profile->id,hook->runtime_bones,hook->profile->head_bones);
}
void __fastcall FacePreUpdate(Object* node, Object* instance) {
    const LONGLONG started=PerfStart();bool found=false,body=false;if(started)for(const auto& candidate:face_hooks_)if(candidate->node==node){found=true;body=candidate->body;break;}
    __try { FacePreUpdateBody(node,instance); }
    __except(EXCEPTION_EXECUTE_HANDLER) {
        for (const auto& candidate:face_hooks_) if (candidate->node==node) InterlockedExchange(&candidate->failed,1);
    }
    if(found)PerfEnd(body?PerfKind::BodyPre:PerfKind::FacePre,started);
}
bool InstallFaceHook(Object* component, const Profile& profile,bool body=false) {
    if (!FaceNativeMatch() || !FaceQuiesce(component)) return false;
    Object* instance=AnimInstance(component), *type=Class("/Script/AnimGraphRuntime.AnimNode_CopyPoseFromMesh");
    const uintptr_t field=instance?Field(ue::ClassOf(instance),"AnimGraphNode_CopyPoseFromMesh"):0;
    if (!instance || ue::IsTemplate(instance) || !type || ue::StructSize(type)!=344 || ue::Offset(type,"SourceMeshComponent")!=0x10 ||
        ue::Offset(type,"bCopyCurves")!=0x18 || ue::Offset(type,"bCopyCustomAttributes")!=0x19 ||
        !field || ue::At<Object*>(reinterpret_cast<void*>(field),0x70)!=type || ue::At<int32_t>(reinterpret_cast<void*>(field),0x34)!=344) return false;
    Object* node=instance+ue::At<int32_t>(reinterpret_cast<void*>(field),0x44);
    uintptr_t* original=ue::At<uintptr_t*>(node,0);
    if (original!=reinterpret_cast<uintptr_t*>(ue::Base()+0xae01460) || original[3]!=ue::Base()+0x35bd160 ||
        original[8]!=ue::Base()+0x35d6120 || original[14]!=ue::Base()+0x35a63f0 ||
        !FaceReadable(original-1,21*8) || (ue::At<uint8_t>(node,0x1a)&1)) return false;
    auto hook=std::make_unique<FaceHook>(); hook->component=Id(component); hook->instance=Id(instance);hook->body=body;
    int32_t weak[2]{};
    reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166f2a0)(weak,&instance);
    hook->instance_index=weak[0]; hook->instance_serial=weak[1]; hook->node=node; hook->original=original; hook->profile=&profile;
    if (!FaceRawInstance(*hook)) return false;
    reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166f2a0)(weak,&component);
    hook->component_index=weak[0]; hook->component_serial=weak[1];
    Call bone_count(component,"GetNumBones");
    if (!bone_count.Slot("ReturnValue",sizeof(int32_t)) || !bone_count.Invoke()) return false;
    hook->runtime_bones=bone_count.Get<int32_t>("ReturnValue");
    if (hook->runtime_bones<(body?profile.bones:profile.head_bones) || hook->runtime_bones>2048) return false;
    if(body){if(!pose_session_||static_cast<size_t>(hook->runtime_bones)!=pose_session_->plan.target.size())return false;hook->body_saved.assign(static_cast<size_t>(hook->runtime_bones),std::numeric_limits<int32_t>::min());}
    if(source_driver_mode_){Object* source=body?primary_.object:display_.object;if(!ue::Alive(source))return false;hook->old_source_input=ue::At<uint64_t>(node,0x10);
        reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166f2a0)(weak,&source);std::memcpy(&hook->source_input,weak,8);
        Object* asset=SkelMesh(source);reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166f2a0)(weak,&asset);std::memcpy(&hook->source_asset_input,weak,8);}
    hook->old_curve=ue::At<uint8_t>(node,0x18)&2; hook->old_attributes=ue::At<uint8_t>(node,0x19);
    if (hook->old_attributes>1) return false;
    hook->saved.fill(std::numeric_limits<int32_t>::min()); hook->table=std::make_unique<uintptr_t[]>(21);
    std::memcpy(hook->table.get(),original-1,21*8); hook->table[1+8]=reinterpret_cast<uintptr_t>(&FacePreUpdate);
    face_thread_=GetCurrentThreadId(); FaceHook* record=hook.get(); face_hooks_.push_back(std::move(hook));
    ue::At<uintptr_t*>(node,0)=record->table.get()+1;
    FacePreUpdate(node,instance); // Build/filter while registered and still hidden, before any new parallel evaluation.
    if (record->epochs!=1 || InterlockedCompareExchange(&record->failed,0,0))
        Logf("NPC_FACE_NEUTRAL install refused source=%s runtimeBones=%d map=%s ownedMapWrites=%d",profile.id,record->runtime_bones,record->map_state,record->map_written?1:0);
    return record->epochs==1 && !InterlockedCompareExchange(&record->failed,0,0);
}
bool FaceRestorePending(FaceHook& hook, const char* reason) {
    const ULONGLONG now=GetTickCount64();
    if (!hook.restore_reason || std::strcmp(hook.restore_reason,reason) || now-hook.restore_log_time>=2000) {
        hook.restore_reason=reason; hook.restore_log_time=now;
        Logf("NPC_FACE_NEUTRAL restore pending reason=%s component=%s instance=%s map=%s ownedMapWrites=%d",reason,
            Text(hook.component.name).c_str(),Text(hook.instance.name).c_str(),hook.map_state,hook.map_written?1:0);
    }
    return false;
}
void RetireFaceHook(size_t index) {
    if (face_hooks_[index]->table) retired_face_tables_.push_back(std::move(face_hooks_[index]->table));
    face_hooks_.erase(face_hooks_.begin()+index);
}
bool RestoreFaceHooks() {
    for (size_t i=face_hooks_.size();i-- >0;) {
        FaceHook& hook=*face_hooks_[i];
        if (!FaceRawInstance(hook)) { RetireFaceHook(i); continue; }
        // IsValid/Alive can reject an old, detached pending-GC instance even
        // though its GU slot and node are still physically present. Restore
        // that exact old instance, never the component's replacement instance.
        if (!FaceReadable(hook.node,344)) return FaceRestorePending(hook,"old-node-unreadable");
        const bool component_raw=RawIdentity(hook.component,hook.component_index,hook.component_serial);
        if (component_raw) {
            if (!FaceReadable(hook.component.object,0xdf0)) return FaceRestorePending(hook,"component-task-unreadable");
            if (ue::At<void*>(hook.component.object,0xde8) &&
                (!Same(hook.component) || !FaceQuiesce(hook.component.object))) return FaceRestorePending(hook,"component-task-pending");
        }
        // An owned component whose GU slot retired has already passed native
        // destruction/task completion. No engine call on that stale object.
        uintptr_t* current=ue::At<uintptr_t*>(hook.node,0);
        if (!Same(hook.instance) && current!=hook.table.get()+1 && current!=hook.original)
            return FaceRestorePending(hook,"pending-GC-node-table-changed");
        if (!(hook.body?BodyMap(hook,true):FaceMap(hook,true))) return FaceRestorePending(hook,"owned-map-restore");
        if(hook.source_input&&ue::At<uint64_t>(hook.node,0x10)==hook.source_input)ue::At<uint64_t>(hook.node,0x10)=hook.old_source_input;
        uint8_t& curves=ue::At<uint8_t>(hook.node,0x18); if (!(curves&2)) curves|=hook.old_curve;
        if (!ue::At<uint8_t>(hook.node,0x19)) ue::At<uint8_t>(hook.node,0x19)=hook.old_attributes;
        const char* table_result=current==hook.original?"already-original":"foreign-table-preserved";
        if (current==hook.table.get()+1) {
            void* previous=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(hook.node),hook.original,current);
            table_result=previous==current?"owned-CAS-restored":"foreign-CAS-race-preserved";
        }
        Logf("NPC_FACE_NEUTRAL restored instance=%s map=%s vtable=%s",Text(hook.instance.name).c_str(),hook.map_state,table_result);
        RetireFaceHook(i);
    }
    face_visibility_.clear(); return true;
}
bool RefreshFaceVisibility() {
    for (const auto& hook:face_hooks_) {
        if (InterlockedCompareExchange(&hook->failed,0,0) || !Same(hook->component) || !Same(hook->instance) ||
            AnimInstance(hook->component.object)!=hook->instance.object || ue::At<uintptr_t*>(hook->node,0)!=hook->table.get()+1) return false;
        if (!face_visibility_.empty()) {
            if (hook->epochs<2) return GetTickCount64()-face_wait_started_<3000;
            if (!FaceQuiesce(hook->component.object)) return GetTickCount64()-face_wait_started_<3000;
        }
    }
    if (face_visibility_.empty()) return true;
    for (const Visibility& visibility:face_visibility_) if (Same(visibility.component)) {
        Object* component=visibility.component.object;
        if (!SetVisible(component,visibility.visible,visibility.hidden)) return false;
        if (visibility.visible&&!visibility.hidden) {
            if (ue::IsA(component,Class("/Script/HairStrandsCore.GroomComponent")))
                reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+ue::layout::kApplyGroomHairMode)(component);
            else if (ue::IsA(component,Class("/Script/Engine.SkinnedMeshComponent")))
                reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+ue::layout::kApplySkinnedHairMode)(component);
        }
    }
    face_visibility_.clear(); Logf("NPC_FACE_NEUTRAL visible after native parallel evaluation completion; head/children follow NPC mesh-ref + own DNA");
    return true;
}
const BoneFallback* Fallback(const Profile& profile, Name bone) {
    for (size_t i = 0; i < profile.fallback_count; ++i) if (ue::NameIs(bone, profile.fallbacks[i].bone)) return &profile.fallbacks[i];
    return nullptr;
}
const GripBasis* Grip(const Profile& profile, Name bone) {
    for (size_t i = 0; i < profile.grip_count; ++i) if (ue::NameIs(bone, profile.grips[i].bone)) return &profile.grips[i];
    return nullptr;
}
Name RawGripName(Name bone);
using NativeSocketTransform = Transform*(__fastcall*)(Object*, Transform*, Name, uint32_t);
using NativeSocketInfo = Object*(__fastcall*)(Object*, Name, Transform*, int32_t*);
NativeSocketTransform OriginalSocketTransform() { return reinterpret_cast<NativeSocketTransform>(ue::Base()+kSocketTransformRva); }
NativeSocketInfo OriginalSocketInfo() { return reinterpret_cast<NativeSocketInfo>(ue::Base()+kSocketInfoRva); }

int32_t PlanBone(const std::vector<NpcPoseTransfer::Bone>& bones,const char* name) {
    for(size_t i=0;i<bones.size();++i){Name n;std::memcpy(&n,&bones[i].name,8);if(ue::NameIs(n,name))return static_cast<int32_t>(i);}return -1;
}
bool GeometryForAnchor(const NpcPoseTransfer::Plan& plan,int32_t si,int32_t ti,bool hand,bool forearm,double* basis) {
    const char* suffix="r";Name n;std::memcpy(&n,&plan.source[static_cast<size_t>(si)].name,8);const std::string name=Lower(Text(n));if(!name.empty()&&name.back()=='l')suffix="l";
    std::array<std::string,4> points;
    if(hand)points={std::string("middle_metacarpal_")+suffix,std::string("middle_01_")+suffix,std::string("index_01_")+suffix,std::string("pinky_01_")+suffix};
    else if(forearm)points={name,std::string("hand_")+suffix,std::string("index_01_")+suffix,std::string("pinky_01_")+suffix};
    else points={"spine_01","spine_05","clavicle_l","clavicle_r"};
    double frames[2][3][3]{};
    for(size_t side=0;side<2;++side){const auto& bones=side?plan.target:plan.source;const auto& cs=side?plan.target_ref_cs:plan.source_ref_cs;const int32_t anchor=side?ti:si;Transform p[4];
        for(size_t k=0;k<4;++k){const int32_t index=PlanBone(bones,points[k].c_str());if(index<0||!NpcGripMath::Relative(cs[static_cast<size_t>(index)],cs[static_cast<size_t>(anchor)],p[k]))return false;}
        if(!NpcGripMath::GeometryFrame(p[0].translation,p[1].translation,p[2].translation,p[3].translation,frames[side]))return false;}
    return NpcGripMath::GeometryBasis(frames[0],frames[1],basis);
}
bool ObjectHasExactPath(Object* object,const char* path) {
    // The object is already in hand. Calling FindObject for an absent weapon
    // class here would scan the whole GU array for every child on every poll.
    if(!ue::Alive(object)||!path)return false;const char* dot=std::strrchr(path,'.');if(!dot||!ue::NameIs(ue::NameOf(object),dot+1))return false;
    Object* package=ue::OuterOf(object);if(!ue::Alive(package)||ue::OuterOf(package))return false;
    const size_t length=static_cast<size_t>(dot-path);if(length>=512)return false;char package_name[512]{};std::memcpy(package_name,path,length);
    return ue::NameIs(ue::NameOf(package),package_name);
}
void RefreshForearmBindings() {
    PerfScope timing(PerfKind::ForearmPoll);
    if(!grip_hook_||!Same(primary_))return;
    std::array<ForearmBinding,2> bindings;Object* owner_component=nullptr;AcquireSRWLockShared(&grip_lock_);if(grip_hook_){bindings=grip_hook_->forearms;owner_component=grip_hook_->component.object;}ReleaseSRWLockShared(&grip_lock_);if(!owner_component)return;
    for(ForearmBinding& binding:bindings){binding.enabled=false;binding.reason="not-equipped";if(!binding.basis)continue;
        const Array children=Property<Array>(primary_.object,"AttachChildren");if(children.num<0||children.num>512||children.max<children.num||!children.data)continue;
        for(int32_t i=0;i<children.num;++i){Object* child=static_cast<Object**>(children.data)[i];if(!ue::Alive(child))continue;Call owner(child,"GetOwner");if(!owner.Invoke())continue;Object* actor=owner.Get<Object*>("ReturnValue");
            if(!ue::Alive(actor)||!ObjectHasExactPath(ue::ClassOf(actor),binding.basis->actor_class))continue;Object* mesh=Property<Object*>(actor,"WeaponMesh");bool visible=false,hidden=true;
            if(!ue::Alive(mesh)||!ObjectHasExactPath(SkelMesh(mesh),binding.basis->mesh)||!VisibilityOf(mesh,visible,hidden)||!visible||hidden||!HasNoLeader(mesh)||AnimClass(mesh)||AnimInstance(mesh))continue;
            Call count(mesh,"GetNumBones");if(!count.Invoke()||count.Get<int32_t>("ReturnValue")!=2)continue;
            binding.actor=Id(actor);binding.mesh=Id(mesh);binding.enabled=true;binding.reason="exact-rigid-2bone-glove";binding.leader=Offset(mesh,"LeaderPoseComponent");binding.anim_class=Offset(mesh,"AnimClass");binding.anim_instance=Offset(mesh,"AnimScriptInstance");break;}}
    AcquireSRWLockExclusive(&grip_lock_);if(grip_hook_&&grip_hook_->component.object==owner_component)grip_hook_->forearms=bindings;ReleaseSRWLockExclusive(&grip_lock_);
}
bool RigidForearmEnabled(const GripHook& hook,int32_t source) {
    Name name;std::memcpy(&name,&pose_session_->plan.source[static_cast<size_t>(source)].name,8);
    for(const ForearmBinding& b:hook.forearms)if(b.enabled&&b.bone==name&&Same(b.actor)&&Same(b.mesh)&&b.leader>=0&&b.anim_class>=0&&b.anim_instance>=0){const uint64_t leader=ue::At<uint64_t>(b.mesh.object,b.leader);
        if((leader==0||leader==0x00000000ffffffffull)&&!ue::At<Object*>(b.mesh.object,b.anim_class)&&!ue::At<Object*>(b.mesh.object,b.anim_instance))return true;}return false;
}
bool AdjustGripSocket(GripHook& hook,Object* self,Name name,uint32_t space,Transform& out,const Transform& component_space,const Transform& native_mount) {
    if(space>2||!pose_session_)return true;if(!PoseIdentityValid()||!NpcGripMath::Valid(component_space))return false;bool managed=false;for(size_t i=0;i<hook.count;++i)if(hook.sockets[i].name==name){managed=true;break;}if(!managed)return true;
    Transform local;int32_t bone=-1;OriginalSocketInfo()(self,name,&local,&bone);
    const auto& plan=pose_session_->plan;if(bone<0||static_cast<size_t>(bone)>=plan.source.size())return true;
    int32_t anchor=bone;bool hand_chain=false;
    for(int32_t b=bone;b>=0;b=plan.source[static_cast<size_t>(b)].parent)if(hook.anchors[static_cast<size_t>(b)].hand){anchor=b;hand_chain=true;break;}
    if(!hand_chain)while(anchor>=0&&hook.anchors[static_cast<size_t>(anchor)].target<0)anchor=plan.source[static_cast<size_t>(anchor)].parent;
    if(anchor<0)return true;const GripAnchor& row=hook.anchors[static_cast<size_t>(anchor)];
    if(row.forearm&&!RigidForearmEnabled(hook,anchor))return true;
    const Transform* source=nullptr;size_t count=0;if(!SourceCurrentCS(self,source,count))return false;
    bool ok=false;AcquireSRWLockExclusive(&pose_lock_);
    __try {if(UpdatePoseCache(source,count)){
        Transform result;
        ok=NpcGripMath::MapSourceMountPose(source,pose_session_->cache.target_cs.data(),static_cast<size_t>(anchor),static_cast<size_t>(row.target),row.palm,row.target_palm,native_mount,row.basis,component_space,result);
        if(ok){out=result;InterlockedIncrement(&hook.queries);}} }
    __finally {ReleaseSRWLockExclusive(&pose_lock_);}return ok;
}
Transform* __fastcall GripSocketTransform(Object* self,Transform* out,Name name,uint32_t space) {
    OriginalSocketTransform()(self,out,name,space);if(space>2)return out;
    bool managed=false;AcquireSRWLockShared(&grip_lock_);if(grip_hook_&&grip_hook_->component.object==self)for(size_t i=0;i<grip_hook_->count;++i)if(grip_hook_->sockets[i].name==name){managed=true;break;}ReleaseSRWLockShared(&grip_lock_);if(!managed)return out;
    const LONGLONG started=PerfStart();Transform component_space,native_mount=*out;if(space!=2){OriginalSocketTransform()(self,&native_mount,name,2);OriginalSocketTransform()(self,&component_space,Name{},space);}
    AcquireSRWLockShared(&grip_lock_);
    __try {__try {if(grip_hook_&&grip_hook_->component.object==self&&!AdjustGripSocket(*grip_hook_,self,name,space,*out,component_space,native_mount))InterlockedIncrement(&grip_hook_->failures);}
        __except(EXCEPTION_EXECUTE_HANDLER){if(grip_hook_&&grip_hook_->component.object==self)InterlockedIncrement(&grip_hook_->failures);}}
    __finally {ReleaseSRWLockShared(&grip_lock_);}PerfEnd(PerfKind::Grip,started);return out;
}
bool InstallGripHook(Object* component,const Profile& profile) {
    if(grip_hook_||!pose_session_||!FaceQuiesce(component))return false;
    auto hook=std::make_unique<GripHook>();hook->component=Id(component);const auto& plan=pose_session_->plan;hook->anchors.resize(plan.source.size());
    double torso_root[4],torso_cs[4],inv_root[4],tmp[4];if(!GeometryForAnchor(plan,0,0,false,false,torso_root))return false;NpcGripMath::Inverse(plan.source_ref_cs[0].rotation,inv_root);NpcGripMath::Mul(plan.target_ref_cs[0].rotation,torso_root,tmp);NpcGripMath::Mul(tmp,inv_root,torso_cs);
    for(size_t si=0;si<plan.source.size();++si){GripAnchor& a=hook->anchors[si];a.target=plan.source_to_target[si];if(a.target<0)continue;Name name;std::memcpy(&name,&plan.source[si].name,8);
        a.hand=ue::NameIs(name,"hand_l")||ue::NameIs(name,"hand_r");a.forearm=ue::NameIs(name,"lowerarm_l")||ue::NameIs(name,"lowerarm_r");
        if(a.hand||a.forearm){if(!GeometryForAnchor(plan,static_cast<int32_t>(si),a.target,a.hand,a.forearm,a.basis))return false;}else{double qi[4],qt[4];NpcGripMath::Inverse(plan.target_ref_cs[static_cast<size_t>(a.target)].rotation,qi);NpcGripMath::Mul(qi,torso_cs,qt);NpcGripMath::Mul(qt,plan.source_ref_cs[si].rotation,a.basis);}
        if(a.hand){const char* palm=ue::NameIs(name,"hand_r")?"middle_01_r":"middle_01_l";a.palm=PlanBone(plan.source,palm);a.target_palm=PlanBone(plan.target,palm);if(a.palm<0||a.target_palm<0)return false;}}
    for(const char* route:kWeaponAttachmentNames){if(hook->count>=hook->sockets.size())return false;const std::wstring wide(route,route+std::strlen(route));const Name name=ue::MakeName(wide.c_str());
        if(!SocketExists(component,name)){Logf("NPC_GRIP source mesh lacks optional mount=%s; route left native",route);continue;}hook->sockets[hook->count++].name=name;}
    for(const ForearmBasis& b:kForearmBases)if(Lower(b.profile)==Lower(profile.id)){const size_t side=std::strstr(b.bone,"_r")?0:1;hook->forearms[side].basis=&b;const std::wstring wide(b.bone,b.bone+std::strlen(b.bone));hook->forearms[side].bone=ue::MakeName(wide.c_str());}
    if(!hook->count)return false;uintptr_t* original=ue::At<uintptr_t*>(component,0);size_t count=0;
    if(!FaceReadable(original-1,(kSocketTransformSlot+2)*8)||original[kSocketTransformSlot]!=ue::Base()+kSocketTransformRva)return false;
    while(count<1024&&original[count]>=ue::Base()+0x1000&&original[count]<ue::Base()+0xa600000)++count;
    if(count<=kSocketTransformSlot||!FaceReadable(original-1,(count+1)*8))return false;
    auto* table=static_cast<uintptr_t*>(VirtualAlloc(nullptr,(count+1)*8,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!table)return false;
    std::memcpy(table,original-1,(count+1)*8);table[1+kSocketTransformSlot]=reinterpret_cast<uintptr_t>(&GripSocketTransform);hook->original=original;hook->table=table+1;retired_grip_tables_.push_back(table);
    AcquireSRWLockExclusive(&grip_lock_);grip_hook_=std::move(hook);const bool installed=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(component),grip_hook_->table,original)==original;if(!installed)grip_hook_.reset();ReleaseSRWLockExclusive(&grip_lock_);
    if(installed){RefreshForearmBindings();Logf("NPC_GRIP source driver current-pose adapter installed routes=%zu nativeChildPreserved=1 targetAssetsMutated=0",grip_hook_->count);}return installed;
}
bool RestoreGripHook() {
    AcquireSRWLockExclusive(&grip_lock_);
    if (!grip_hook_) { ReleaseSRWLockExclusive(&grip_lock_); return true; }
    GripHook& hook=*grip_hook_;
    const char* vtable_result="component-retired";
    if (Same(hook.component)) {
        uintptr_t* current=ue::At<uintptr_t*>(hook.component.object,0);
        if (current==hook.table) {
            void* previous=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(hook.component.object),hook.original,hook.table);
            vtable_result=previous==hook.table?"owned-CAS-restored":"foreign-CAS-race-preserved";
        } else vtable_result=current==hook.original?"already-original":"foreign-table-preserved";
        // A foreign private table is preserved. If it copied this function, its
        // calls are now safe native passthrough because the session is retired.
    }
    Logf("NPC_GRIP restored current-pose adapter queries=%ld failures=%ld vtable=%s",hook.queries,hook.failures,vtable_result);
    grip_hook_.reset(); ReleaseSRWLockExclusive(&grip_lock_); return true;
}
Name RawGripName(Name bone) {
    const std::string text="PBZ_NpcGripRaw_"+Text(bone); const std::wstring wide(text.begin(),text.end());
    return ue::MakeName(wide.c_str());
}
bool AddSocket(Name socket_name, Name old_bone, Transform local, Object* target_mesh, Object* target_skeleton, Object* probe, const Profile& profile, bool raw_alias=false) {
    if (socket_name.None() || old_bone.None()) return false;
    Name bone = old_bone;
    // Resolve a source-only child before selecting its live grip driver. In
    // particular weapon_r_child is an identity child of WEAPON_R in Hun and is
    // absent from both NPC meshes; treating it as a static non-grip socket left
    // weapons using that direct bone name outside the dynamic adapter.
    if (const BoneFallback* fallback = Fallback(profile,old_bone)) {
        const std::wstring ancestor(fallback->ancestor,fallback->ancestor+std::strlen(fallback->ancestor));bone=ue::MakeName(ancestor.c_str());
        if (bone.None() || !Compose(local,fallback->relative,local)) return false;
    }
    const Transform source_local=local;
    const GripBasis* grip = raw_alias?nullptr:Grip(profile,bone);
    if (grip) {
        Transform source_hand;
        if (!Compose(local, grip->source_to_hand, source_hand) || !Relative(source_hand, grip->target_to_hand, local)) return false;
        Logf("NPC_RETARGET grip basis socket=%s bone=%s anchor=%s (source hand-relative ref -> target hand-relative ref)", Text(socket_name).c_str(), Text(old_bone).c_str(), grip->hand);
    }
    Call bone_index(probe, "GetBoneIndex");
    if (!bone_index.Set("BoneName", bone).Invoke() || bone_index.Get<int32_t>("ReturnValue") < 0) return false;
    Call spawn(ue::Default("/Script/Engine.Default__GameplayStatics"), "SpawnObject");
    Object* socket = spawn.Set("ObjectClass", Class("/Script/Engine.SkeletalMeshSocket")).Set("Outer", target_mesh).Invoke() ? spawn.Get<Object*>("ReturnValue") : nullptr;
    if (!socket || !Put(socket, "SocketName", socket_name) || !Put(socket, "BoneName", bone)) return false;
    Call split(Math(), "BreakTransform");
    if (!split.Set("InTransform", local).Invoke() || !Put(socket, "RelativeLocation", split.Get<Vector>("Location")) ||
        !Put(socket, "RelativeRotation", split.Get<Vector>("Rotation")) || !Put(socket, "RelativeScale", split.Get<Vector>("Scale")) || !PutBool(socket, "bForceAlwaysAnimated", true)) return false;
    uintptr_t field = 0; Array* array = nullptr;
    // The Shipping map inserts mesh sockets before skeleton sockets and only
    // falls back to a bone when no socket was found. A same-name mesh socket
    // therefore corrects native direct WEAPON_R as well as *_Socket users,
    // without rewriting any weapon's dynamic attachment or relative transform.
    Object* owner = grip || raw_alias ? target_mesh : target_skeleton;
    if (!ArrayField(owner, "Sockets", field, array)) return false;
    for (int32_t i = 0; i < array->num; ++i) {
        Object* prior = static_cast<Object**>(array->data)[i];
        if (ue::Alive(prior) && Property<Name>(prior, "SocketName") == socket_name) return Fail(L"目标网格已有同名自定义武器 Socket，未覆盖外部编辑");
    }
    const int32_t before = array->num;
    sockets_.reserve(sockets_.size() + 1);
    const int32_t at = reinterpret_cast<int32_t(__fastcall*)(void*, const void*, const void*)>(ue::Base() + kArrayAddRva)(array, reinterpret_cast<void*>(field), &socket);
    // Record an engine-owned insertion before checking the expected return so
    // an unexpected index cannot leave an untracked live socket behind.
    for (int32_t i = 0; i < array->num && array->num <= array->max && array->num < 65536; ++i)
        if (static_cast<Object**>(array->data)[i] == socket) { sockets_.push_back({Id(owner), Id(socket), Id(target_skeleton), socket_name, bone, i, local}); break; }
    if (at != before || array->num != before + 1 || static_cast<Object**>(array->data)[before] != socket) return false;
    if (grip) {
        SocketAdded& added=sockets_.back(); added.grip=grip; added.source_local=source_local; added.raw_bone_alias=RawGripName(bone);
        if (added.raw_bone_alias.None()) return false;
    }
    Logf("NPC_RETARGET temporary socket %s sourceBone=%s targetBone=%s owner=%s", Text(socket_name).c_str(), Text(old_bone).c_str(), Text(bone).c_str(), grip || raw_alias ? "target-mesh grip override" : "target-skeleton missing-bone fallback"); return true;
}
[[maybe_unused]] bool CopySockets(Object* old_mesh, Object* target_mesh, Object* probe, const Profile& profile) {
    Object* source_skeleton = Property<Object*>(old_mesh, "Skeleton"), *target_skeleton = Property<Object*>(target_mesh, "Skeleton");
    if (source_skeleton != ue::FindObject(kSourceSkeleton)) return Fail(L"当前主角不是已审计的 Hun 源骨架");
    std::vector<Object*> sources; std::vector<Name> seen;
    for (Object* owner : {old_mesh, source_skeleton}) {
        uintptr_t field = 0; Array* array = nullptr;
        if (!ArrayField(owner, "Sockets", field, array)) return Fail(L"源网格 / Skeleton Socket 数组布局不符");
        for (int32_t i = 0; i < array->num; ++i) {
            Object* socket = static_cast<Object**>(array->data)[i];
            if (!ue::Alive(socket)) continue;
            const Name name = Property<Name>(socket, "SocketName");
            if (name.None() || std::any_of(seen.begin(), seen.end(), [&](Name prior) { return prior == name; })) continue;
            seen.push_back(name);
            if (Grip(profile, Property<Name>(socket, "BoneName")) || !SocketExists(probe, name)) sources.push_back(socket);
        }
    }
    if (!sources.empty() && !kRebuildSocketMapRva) return Fail(L"尚未验证 Shipping SocketMap 刷新入口，未修改主网格或武器挂点");
    for (Object* socket : sources) {
        Call get(socket, "GetSocketLocalTransform");
        if (!get.Invoke() || !AddSocket(Property<Name>(socket, "SocketName"), Property<Name>(socket, "BoneName"), get.Get<Transform>("ReturnValue"), target_mesh, target_skeleton, probe, profile)) return Fail(L"无法安全构建临时武器 Socket");
    }
    for (size_t i = 0; i < profile.grip_count; ++i) {
        const char* text = profile.grips[i].bone; const std::wstring wide(text, text + std::strlen(text));
        const Name bone = ue::MakeName(wide.c_str());
        if (bone.None() || !AddSocket(bone, bone, Transform{}, target_mesh, target_skeleton, probe, profile)) return Fail(L"无法构建直接武器骨挂接的受管网格 Socket");
        const Name alias=RawGripName(bone);
        if (alias.None() || !AddSocket(alias,bone,Transform{},target_mesh,target_skeleton,probe,profile,true)) return Fail(L"无法构建当前姿势武器骨的独立查询挂点");
        const char* hand_text=profile.grips[i].hand;
        const std::wstring hand_wide(hand_text,hand_text+std::strlen(hand_text)); const Name hand=ue::MakeName(hand_wide.c_str());
        const GripRemapBasis* basis=nullptr;
        for (const GripRemapBasis& candidate:kGripRemapBasis)
            if (Lower(candidate.profile)==Lower(profile.id) && ue::NameIs(bone,candidate.bone)) { basis=&candidate;break; }
        if (!basis) return Fail(L"武器手掌适配的原生参考资料缺失");
        const std::wstring palm_wide(basis->palm,basis->palm+std::strlen(basis->palm));const Name palm=ue::MakeName(palm_wide.c_str());
        for (Name anchor:{hand,palm}) {
            const Name query=RawGripName(anchor);
            if (anchor.None() || query.None() || !AddSocket(query,anchor,Transform{},target_mesh,target_skeleton,probe,profile,true)) return Fail(L"无法构建当前手掌姿势的独立查询挂点");
        }
    }
    // Some configurations name a bone directly rather than its *_Socket.
    // In particular weapon_r_child and Pt_LongBladeScabbard_Back are missing
    // in the NPC meshes. The list comes only from native typed display fields;
    // footstep/trail names in a package's NameMap are not attachment evidence.
    for (const char* route:kWeaponDirectAttachmentBones) {
        const std::wstring wide(route,route+std::strlen(route));const Name name=ue::MakeName(wide.c_str());
        const bool added=std::any_of(sockets_.begin(),sockets_.end(),[&](const SocketAdded& socket){return socket.socket_name==name;});
        if (name.None() || (!added && !SocketExists(probe,name) && !AddSocket(name,name,Transform{},target_mesh,target_skeleton,probe,profile)))
            return Fail(L"武器配置使用的直接骨挂点未能完成原生适配");
    }
    if (!sockets_.empty()) {
        Object* mesh_class = Class("/Script/Engine.SkeletalMesh");
        ue::ForEachObject([&](Object* mesh) {
            if (!ue::IsTemplate(mesh) && ue::Alive(mesh) && ue::IsA(mesh, mesh_class) && Property<Object*>(mesh, "Skeleton") == target_skeleton) {
                if (RebuildSockets(mesh)) socket_maps_.push_back(Id(mesh));
            }
            return true;
        });
        if (std::none_of(socket_maps_.begin(), socket_maps_.end(), [&](Identity identity) { return identity.object == target_mesh; })) return false;
    }
    for (const SocketAdded& added : sockets_) {
        if (!SocketExists(probe, added.socket_name)) return Fail(L"临时 Socket 未通过原生查找验证");
        Call get(probe, "GetSocketTransform");
        if (!get.Set("InSocketName", added.socket_name).Set("TransformSpace", uint8_t{3}).Invoke()) return Fail(L"原生 Socket 局部空间读回失败");
        const Transform actual = get.Get<Transform>("ReturnValue"); double dot = 0;
        for (size_t i = 0; i < 4; ++i) dot += actual.rotation[i] * added.local.rotation[i];
        if (std::abs(std::abs(dot) - 1) > 1e-5) return Fail(L"Socket 查询仍使用原骨/原挂点，武器旋转补偿未生效");
        for (size_t i = 0; i < 3; ++i)
            if (std::abs(actual.translation[i] - added.local.translation[i]) > 0.01 || std::abs(actual.scale[i] - added.local.scale[i]) > 1e-5) return Fail(L"Socket 原生读回未匹配本方临时挂点变换");
        Logf("NPC_RETARGET_SOCKET_READBACK name=%s bone=%s owner=%s T=(%.6f,%.6f,%.6f) Q=(%.8f,%.8f,%.8f,%.8f) verified=1", Text(added.socket_name).c_str(), Text(added.bone_name).c_str(), Text(added.owner.object).c_str(), actual.translation[0], actual.translation[1], actual.translation[2], actual.rotation[0], actual.rotation[1], actual.rotation[2], actual.rotation[3]);
    }
    return true;
}
bool RestoreSockets() {
    bool complete = true;
    std::vector<Object*> skeletons;
    for (const SocketAdded& added : sockets_) if (Same(added.skeleton) && std::find(skeletons.begin(), skeletons.end(), added.skeleton.object) == skeletons.end()) skeletons.push_back(added.skeleton.object);
    for (size_t i = sockets_.size(); i-- > 0;) {
        const SocketAdded& added = sockets_[i];
        if (!Same(added.owner)) { sockets_.erase(sockets_.begin() + i); continue; }
        uintptr_t field = 0; Array* array = nullptr;
        if (!Same(added.socket) || !ArrayField(added.owner.object, "Sockets", field, array) || array->num != added.index + 1 ||
            static_cast<Object**>(array->data)[added.index] != added.socket.object || !(Property<Name>(added.socket.object, "SocketName") == added.socket_name) ||
            !(Property<Name>(added.socket.object, "BoneName") == added.bone_name)) {
            Logf("NPC_RETARGET socket list changed externally; owned-tail removal skipped"); complete = false; continue;
        }
        Call local(added.socket.object, "GetSocketLocalTransform"); bool animated = false;
        if (!local.Invoke() || !TransformNear(added.local, local.Get<Transform>("ReturnValue")) ||
            !Bool(added.socket.object, "bForceAlwaysAnimated", animated) || !animated) {
            Logf("NPC_RETARGET socket local transform/flag changed externally; retained owned socket ledger"); complete = false; continue;
        }
        reinterpret_cast<void(__fastcall*)(void*, const void*, int32_t)>(ue::Base() + kArrayRemoveRva)(array, reinterpret_cast<void*>(field), added.index);
        if (array->num == added.index) sockets_.erase(sockets_.begin() + i);
        else { Logf("NPC_RETARGET engine socket removal did not change expected Num; retained ledger"); complete = false; }
    }
    for (Identity mesh : socket_maps_) if (Same(mesh)) RebuildSockets(mesh.object);
    // Assets loaded during the session may have cached our skeleton sockets
    // after the initial scan. Include them when retiring that shared edit.
    if (!skeletons.empty()) ue::ForEachObject([&](Object* mesh) {
        if (!ue::IsTemplate(mesh) && ue::Alive(mesh) && ue::IsA(mesh, Class("/Script/Engine.SkeletalMesh")) &&
            std::find(skeletons.begin(), skeletons.end(), Property<Object*>(mesh, "Skeleton")) != skeletons.end()) RebuildSockets(mesh);
        return true;
    });
    if (sockets_.empty()) socket_maps_.clear();
    return complete;
}

#pragma warning(push)
#pragma warning(disable: 4324) // Contains a native 16-byte-aligned FTransform parameter.
struct Part {
    Object* source = nullptr; Object* created = nullptr; std::string variable, parent; Name socket; Transform relative;
    bool attachment_known = false, finished = false, head_branch = false;
};
#pragma warning(pop)
std::string Variable(Object* object) {
    std::string value = Lower(Text(object)); const std::string suffix = "_gen_variable";
    if (value.size() > suffix.size() && value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) value.resize(value.size() - suffix.size());
    return value;
}
Object* LoadDonor(const Profile& profile) {
    const std::string package(profile.donor), leaf = package.substr(package.find_last_of('/') + 1) + "_C";
    Object* cls = ue::FindObject((package + "." + leaf).c_str());
    if (!cls) {
        SoftPath path; const std::wstring p(package.begin(), package.end()), a(leaf.begin(), leaf.end());
        path.package = ue::MakeName(p.c_str()); path.asset = ue::MakeName(a.c_str());
        Call load(Kismet(), "LoadClassAsset_Blocking");
        unsigned char* slot = load.Slot("AssetClass", sizeof(path));
        if (!slot) return nullptr;
        std::memcpy(slot, &path, sizeof(path));
        if (load.Invoke()) cls = load.Get<Object*>("ReturnValue");
    }
    return cls;
}
bool SourceOwner(Object* object, Object* cls, Object* cdo) {
    for (size_t guard = 0; object && guard < 8; ++guard, object = ue::OuterOf(object)) if (object == cls || object == cdo) return true;
    return false;
}
bool LoadParts(Object* cls, Object* cdo, const Profile& profile, std::vector<Part>& parts) {
    for (size_t i = 0; i < profile.part_count; ++i) {
        Object* found = nullptr;
        ue::ForEachObject([&](Object* object) {
            if (ue::IsTemplate(object) && ue::Alive(object) && ue::NameIs(ue::NameOf(object), profile.parts[i]) && SourceOwner(object, cls, cdo)) { found = object; return false; }
            return true;
        });
        if (!found || !ue::IsA(found, Class("/Script/Engine.SceneComponent"))) return Fail(L"NPC 部件模板缺失或不是场景组件");
        Part part; part.source = found; part.variable = Variable(found);
        if (!TransformOf(found, part.relative)) return Fail(L"无法读取 NPC 部件的原始相对变换");
        parts.push_back(part);
    }
    // Read the cooked SCS graph, including socket names and parent-node links;
    // template AttachParent alone is not populated for every SCS component.
    std::map<Object*, std::string> variables;
    std::vector<Object*> nodes;
    for (Object* type = cls; type; type = ue::At<Object*>(type, ue::layout::kSuper)) {
        Object* scs = Property<Object*>(type, "SimpleConstructionScript");
        const Array all = Property<Array>(scs, "AllNodes");
        if (all.num < 0 || all.num > 1024 || (all.num && !all.data)) return false;
        for (int32_t i = 0; i < all.num; ++i) {
            Object* node = static_cast<Object**>(all.data)[i]; if (!ue::Alive(node)) continue;
            variables[node] = Lower(Text(Property<Name>(node, "InternalVariableName"))); nodes.push_back(node);
        }
    }
    for (Object* node : nodes) {
        const std::string variable = variables[node];
        auto part = std::find_if(parts.begin(), parts.end(), [&](const Part& value) { return value.variable == variable; });
        if (part != parts.end()) {
            const Name parent = Property<Name>(node, "ParentComponentOrVariableName");
            part->parent = parent.None() ? "" : Lower(Text(parent)); part->socket = Property<Name>(node, "AttachToName");
            part->attachment_known = true;
        }
    }
    // AllNodes is parent-before-child in both exact donor BPs. Do not let a
    // later child's empty external-parent field erase its real SCS edge.
    // Internal links are authoritative and applied only after all own records.
    for (Object* node : nodes) {
        const std::string variable = variables[node];
        const Array children = Property<Array>(node, "ChildNodes");
        if (children.num < 0 || children.num > 1024 || (children.num && !children.data)) return false;
        for (int32_t i = 0; i < children.num; ++i) {
            const auto child = variables.find(static_cast<Object**>(children.data)[i]);
            if (child == variables.end()) continue;
            for (Part& value : parts) if (value.variable == child->second) { value.parent = variable; value.attachment_known = true; }
        }
    }
    Object* source_primary = Property<Object*>(cdo, "Mesh"), *source_root = Property<Object*>(cdo, "RootComponent");
    for (Part& part : parts) if (!part.attachment_known) {
        Object* parent = Property<Object*>(part.source, "AttachParent");
        if (parent && parent == source_primary) { part.parent = "mesh"; part.attachment_known = true; }
        else if (parent && parent == source_root) { part.parent = "rootcomponent"; part.attachment_known = true; }
        else if (parent) {
            const auto found = std::find_if(parts.begin(), parts.end(), [&](const Part& value) { return value.source == parent; });
            if (found != parts.end()) { part.parent = found->variable; part.attachment_known = true; }
        }
        if (!part.attachment_known && ue::IsA(part.source, Class("/Script/HairStrandsCore.GroomComponent"))) {
            Object* binding = Property<Object*>(part.source, "BindingAsset"), *target = Property<Object*>(binding, "TargetSkeletalMesh");
            if (target && SkelMesh(source_primary) == target) { part.parent = "mesh"; part.attachment_known = true; }
            else for (const Part& candidate : parts) if (target && SkelMesh(candidate.source) == target) {
                part.parent = candidate.variable; part.attachment_known = true; break;
            }
        }
        if (!part.attachment_known) {
            Logf("NPC_RETARGET unverified attachment graph part=%s", part.variable.c_str());
            return Fail(L"NPC 部件缺少经过核定的 SCS 挂接信息");
        }
    }
    return true;
}
bool ValidateSourceFace(const Profile& profile, const std::vector<Part>& parts) {
    const auto head = std::find_if(parts.begin(), parts.end(), [](const Part& part) { return part.variable == "meshhead"; });
    if (head == parts.end()) return Fail(L"原 NPC 头部组件缺失");
    Object* mesh = SkelMesh(head->source), *post_class = EffectivePostClass(head->source);
    AnimOptions options;
    Object* copy_pose = Class("/HuaShira/Characters/Hero/Hero_Hun/Hun_CopyPose.Hun_CopyPose_C");
    if (!mesh || mesh != ue::FindObject(profile.head) || !post_class || post_class != ue::FindObject(profile.head_postprocess) ||
        !ReadAnimOptions(head->source, options) || options.disabled || !copy_pose || AnimClass(head->source) != copy_pose)
        return Fail(L"NPC Head 的原网格 / CopyPose / 独立 PostProcess 与精确包资料不一致");
    Object* dna_class = Class("/Script/RigLogicModule.DNAAsset"), *dna = nullptr;
    if (Offset(mesh, "AssetUserData") < 0 || !dna_class) return Fail(L"当前运行时缺少 Head AssetUserData / DNA 元数据");
    const Array user_data = Property<Array>(mesh, "AssetUserData");
    if (user_data.num < 0 || user_data.num > 128 || user_data.max < user_data.num || (user_data.num && !user_data.data)) return false;
    for (int32_t i = 0; i < user_data.num; ++i) {
        Object* data = static_cast<Object**>(user_data.data)[i];
        if (ue::Alive(data) && ue::IsA(data, dna_class) && ue::OuterOf(data) == mesh) dna = data;
    }
    Logf("NPC_RETARGET_FACE_SOURCE id=%s head=%s animClass=%s postClass=%s DNA=%s DNAOuter=%s disabled=%d lod=%d",
        profile.id, Text(mesh).c_str(), Text(AnimClass(head->source)).c_str(), Text(post_class).c_str(), Text(dna).c_str(),
        Text(dna ? ue::OuterOf(dna) : nullptr).c_str(), options.disabled ? 1 : 0, options.lod);
    return dna != nullptr || Fail(L"NPC 原 Head 没有自身 DNAAsset，未用 Hun 面部替代");
}
void CopyRenderFlags(Object* from, Object* to) {
    for (const char* flag : {"CastShadow", "bCastDynamicShadow", "bCastStaticShadow", "bRenderInMainPass", "bRenderInDepthPass", "bVisibleInRayTracing", "bReceivesDecals", "bUseAttachParentBound"}) {
        bool value = false; if (Bool(from, flag, value)) PutBool(to, flag, value);
    }
}
bool LightingChannels(Object* component, std::array<bool,3>& channels) {
    const uintptr_t field=component?Field(ue::ClassOf(component),"LightingChannels"):0;
    Object* type=Class("/Script/Engine.LightingChannels");
    if (!field || !type || ue::At<Object*>(reinterpret_cast<void*>(field),0x70)!=type ||
        ue::At<int32_t>(reinterpret_cast<void*>(field),ue::layout::kPropElementSize)!=1 || ue::StructSize(type)!=1) return false;
    Object* data=component+ue::At<int32_t>(reinterpret_cast<void*>(field),ue::layout::kPropOffset);
    const char* names[]={"bChannel0","bChannel1","bChannel2"};
    for (size_t i=0;i<channels.size();++i) {
        const uintptr_t member=Field(type,names[i]);
        if (!member || ue::At<uint8_t>(reinterpret_cast<void*>(member),0x70)!=1 ||
            ue::At<int32_t>(reinterpret_cast<void*>(member),ue::layout::kPropOffset)!=0 || ue::At<uint8_t>(reinterpret_cast<void*>(member),0x71)!=0) return false;
        const uint8_t mask=ue::At<uint8_t>(reinterpret_cast<void*>(member),0x73);
        if (mask!=(1u<<i)) return false;
        channels[i]=(data[0]&mask)!=0;
    }
    return true;
}
bool MatchPlayerLighting(Object* player_mesh, Object* component) {
    std::array<bool,3> expected{},actual{};
    if (!LightingChannels(player_mesh,expected)||!LightingChannels(component,actual)) return false;
    if (expected==actual) return true;
    // PBZ's player body/head use channels 0+1; new Engine components default
    // to channel 0. Copying only CastShadow made replacement heads miss the
    // player-only fill lights. Use the setter so registered render proxies are
    // dirtied too; never change scene lights, exposure or skin materials.
    if (!Call(component,"SetLightingChannels").Set("bChannel0",expected[0]).Set("bChannel1",expected[1]).Set("bChannel2",expected[2]).Invoke() ||
        !LightingChannels(component,actual) || expected!=actual) return false;
    Logf("NPC_LIGHT channels component=%s player=(%d,%d,%d) native-readback=1",Text(component).c_str(),actual[0]?1:0,actual[1]?1:0,actual[2]?1:0);
    return true;
}
bool RefreshOwnedLighting() {
    if (!Same(primary_)) return false;
    Object* primitive=Class("/Script/Engine.PrimitiveComponent"); if (!primitive) return false;
    for (const Identity& part:added_) if (Same(part) && ue::IsA(part.object,primitive) && !MatchPlayerLighting(primary_.object,part.object)) return false;
    return true;
}
Object* BindingParent(Object* binding, Object* primary, const std::vector<Part>& parts) {
    Object* target = Property<Object*>(binding, "TargetSkeletalMesh");
    if (target && SkelMesh(primary) == target) return primary;
    for (const Part& part : parts) if (part.created && target && SkelMesh(part.created) == target) return part.created;
    return nullptr;
}
bool Configure(Object* source, Object* dest, Object* primary, Object* parent, const std::vector<Part>& parts) {
    const bool groom = ue::IsA(source, Class("/Script/HairStrandsCore.GroomComponent"));
    if (groom) {
        Object* binding = Property<Object*>(source, "BindingAsset");
        Object* binding_parent = BindingParent(binding, primary, parts);
        if (!binding_parent || binding_parent != parent) return Fail(L"Groom 原挂接父组件与绑定目标不同，未更换父坐标系或套用错误相对变换");
        for (const char* property : {"GroomAsset", "BindingAsset", "PhysicsAsset"}) if (!Put(dest, property, Property<Object*>(source, property))) return false;
        const int32_t settings = Offset(source, "SimulationSettings"), out_settings = Offset(dest, "SimulationSettings");
        Object* settings_class = Class("/Script/HairStrandsCore.HairSimulationSettings"), *setup_class = Class("/Script/HairStrandsCore.HairSimulationSetup");
        if (settings < 0 || out_settings < 0 || ue::StructSize(settings_class) != 0x90 ||
            ue::Offset(settings_class, "SimulationSetup") != 8 || ue::Offset(setup_class, "LocalBone") != 16) return false;
        const ue::String local_bone = ue::At<ue::String>(source, settings + 0x18);
        if (local_bone.num < 0 || local_bone.num > 4096 || local_bone.max < local_bone.num || (local_bone.num && !local_bone.data)) return false;
        ue::String owned_bone;
        if (local_bone.num > 1) {
            // The reflected string library returns a separate UE-owned FString.
            // Borrow A/B only for the call; transfer just ReturnValue to the new
            // component so template and clone never share a destructed buffer.
            Call clone(ue::Default("/Script/Engine.Default__KismetStringLibrary"), "Concat_StrStr");
            if (!clone.Set("A", local_bone).Set("B", ue::String{}).Invoke()) return false;
            owned_bone = clone.Get<ue::String>("ReturnValue");
            if (owned_bone.num != local_bone.num || !owned_bone.data || owned_bone.data == local_bone.data) {
                if (owned_bone.data && owned_bone.data != local_bone.data) ue::Free(const_cast<wchar_t*>(owned_bone.data));
                return false;
            }
        }
        ue::Free(const_cast<wchar_t*>(ue::At<ue::String>(dest, out_settings + 0x18).data));
        std::memcpy(dest + out_settings, source + settings, 0x90);
        std::memcpy(dest + out_settings + 0x18, &owned_bone, sizeof(owned_bone));
        for (const char* property : {"bUseCards", "bForceLocalSpaceSimulation"}) { bool value = false; if (Bool(source, property, value)) PutBool(dest, property, value); }
        if (!PutBool(dest, "bIsMainHair", true)) return false;
    } else if (ue::IsA(source, Class("/Script/ChaosClothAssetEngine.ChaosClothComponent"))) {
        Object* asset = Property<Object*>(source, "SkinnedAsset");
        if (!asset) { Call get(source, "GetClothAsset"); if (get.Invoke()) asset = get.Get<Object*>("ReturnValue"); }
        if (!asset) return false;
        Call set(dest, "SetClothAsset");
        if (!set.Set("InClothAsset", asset).Invoke()) return false;
    } else if (ue::IsA(source, Class("/Script/Engine.SkeletalMeshComponent"))) {
        Object* asset = SkelMesh(source);
        if (!asset) return true;
        AnimOptions options;
        if (!ReadAnimOptions(source, options) || !WriteAnimOptions(dest, options) || !SetMesh(dest, asset)) return false;
        Object* own_class = AnimClass(source);
        if (own_class && !Call(dest, "SetAnimInstanceClass").Set("NewClass", own_class).Invoke()) return false;
    } else if (ue::IsA(source, Class("/Script/Engine.StaticMeshComponent"))) {
        Object* asset = Property<Object*>(source, "StaticMesh");
        if (!asset || !Call(dest, "SetStaticMesh").Set("NewMesh", asset).Invoke()) return false;
    } else return false;
    if (!CopyMaterials(source, dest)) return false;
    CopyRenderFlags(source, dest);
    if (!MatchPlayerLighting(primary,dest)) return Fail(L"NPC 部件未能同步主角的原生照明通道，已请求恢复");
    for (const char* property : {"bAbsoluteLocation", "bAbsoluteRotation", "bAbsoluteScale"}) {
        bool absolute = false;
        if (!Bool(source, property, absolute) || !PutBool(dest, property, absolute)) return false;
    }
    if (!Call(dest, "SetCollisionEnabled").Set("NewType", uint8_t{0}).Invoke() ||
        !Call(dest, "SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents", false).Invoke()) return false;
    if (!groom && ue::IsA(dest, Class("/Script/Engine.SkinnedMeshComponent"))) {
        const bool owns_animation = ue::IsA(source, Class("/Script/Engine.SkeletalMeshComponent")) && AnimClass(source);
        Object* pose_parent = parent;
        for (size_t guard = 0; pose_parent && guard < 32 && !ue::IsA(pose_parent, Class("/Script/Engine.SkinnedMeshComponent")); ++guard)
            pose_parent = Property<Object*>(pose_parent, "AttachParent");
        if (!pose_parent) pose_parent = primary;
        // Authored CopyPose components must run their own graph and Head's
        // RigLogic post-process. LeaderPose bypasses that identity chain.
        Object* leader = owns_animation ? nullptr : pose_parent;
        if (!Call(dest, "SetLeaderPoseComponent").Set("NewLeaderBoneComponent", leader).Set("bForceUpdate", true).Set("bInFollowerShouldTickPose", false).Invoke() ||
            !Call(dest, "AddTickPrerequisiteComponent").Set("PrerequisiteComponent", pose_parent).Invoke()) return false;
        bool influenced = false;
        if (Bool(source, "bInfluencedByMainHairMode", influenced)) PutBool(dest, "bInfluencedByMainHairMode", influenced);
    }
    return true;
}
bool Assemble(const Context& context, Object* source_primary, const Profile& profile, std::vector<Part>& parts) {
    Transform body_relative; if (!TransformOf(source_primary, body_relative)) return false;
    size_t remaining = parts.size();
    for (size_t pass = 0; remaining && pass <= parts.size(); ++pass) {
        size_t progress = 0;
        for (Part& part : parts) {
            if (part.finished) continue;
            Object* parent = context.mesh; Transform relative = part.relative; part.head_branch=part.variable=="meshhead";
            if (!part.parent.empty() && part.parent != "mesh" && part.parent != "charactermesh0") {
                const auto found = std::find_if(parts.begin(), parts.end(), [&](const Part& candidate) { return candidate.variable == part.parent; });
                if (found != parts.end()) { if (!found->finished) continue; parent = found->created; part.head_branch=part.head_branch||found->head_branch; }
                else if (part.parent == "capsulecomponent" || part.parent == "rootcomponent" || part.parent == "defaultsceneroot") {
                    if (!Relative(part.relative, body_relative, relative)) return false;
                } else return Fail(L"NPC 部件的父组件未被完整解析");
            } else if (part.parent.empty() && !Relative(part.relative, body_relative, relative)) return false;
            const bool groom = ue::IsA(part.source, Class("/Script/HairStrandsCore.GroomComponent"));
            if (groom && !BindingParent(Property<Object*>(part.source, "BindingAsset"), context.mesh, parts)) continue;
            part.created = Add(context.pawn, ue::ClassOf(part.source), relative);
            if (!part.created) return false;
            added_.push_back(Id(part.created));
            if (part.head_branch && !SetVisible(part.created,false,true)) return false;
            if (!Configure(part.source, part.created, context.mesh, parent, parts)) return Fail(L"NPC 部件的原生装配接口未能完成");
            if (part.head_branch && ue::IsA(part.created,Class("/Script/Engine.SkeletalMeshComponent")) &&
                (!Put(part.created,"VisibilityBasedAnimTickOption",uint8_t{0}) || !PutBool(part.created,"bEnableUpdateRateOptimizations",false)))
                return Fail(L"Head 首次隐藏评估所需的动画 tick 设置未能验证");
            if (!AttachFinish(context.pawn, part.created, parent, part.socket, relative)) return false;
            if (part.variable=="meshhead" && !InstallFaceHook(part.created,profile)) return Fail(L"自有 Head 的面部 CopyPose 隔离未能通过原生布局 / 私有映射校验");
            if (ue::IsA(part.source, Class("/Script/Engine.SkeletalMeshComponent")) && SkelMesh(part.source)) {
                AnimOptions options;
                Object* source_class = AnimClass(part.source), *instance = AnimInstance(part.created);
                AnimOptions actual_options;
                if (!ReadAnimOptions(part.source, options) || !ReadAnimOptions(part.created, actual_options) || !SameAnimOptions(options, actual_options) ||
                    (source_class && (!VerifyPostProcess(part.created, options, part.variable.c_str()) || AnimClass(part.created) != source_class ||
                        !ue::Alive(instance) || ue::ClassOf(instance) != source_class || !HasNoLeader(part.created))))
                    return Fail(L"NPC 部件原生动画 / Head PostProcess 实例读回失败，已请求恢复");
                Logf("NPC_RETARGET_ANIM part=%s ownClass=%s ownInstance=%s leaderCleared=%d sourceParent=%s",
                    part.variable.c_str(), Text(AnimClass(part.created)).c_str(), Text(instance).c_str(), HasNoLeader(part.created) ? 1 : 0, Text(parent).c_str());
            }
            Logf("NPC_RETARGET_FRAME part=%s parent=%s socket=%s T=(%.6f,%.6f,%.6f) Q=(%.8f,%.8f,%.8f,%.8f) S=(%.6f,%.6f,%.6f) native-readback=1",
                part.variable.c_str(), Text(parent).c_str(), Text(part.socket).c_str(), relative.translation[0], relative.translation[1], relative.translation[2],
                relative.rotation[0], relative.rotation[1], relative.rotation[2], relative.rotation[3], relative.scale[0], relative.scale[1], relative.scale[2]);
            bool visible = true, hidden = false;
            if (!VisibilityOf(part.source, visible, hidden)) return false;
            if (part.head_branch) face_visibility_.push_back({Id(part.created),visible,hidden});
            else if (!SetVisible(part.created, visible, hidden)) return false;
            if (!part.head_branch && visible && !hidden) {
                if (groom) reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base() + ue::layout::kApplyGroomHairMode)(part.created);
                else if (ue::IsA(part.created, Class("/Script/Engine.SkinnedMeshComponent")))
                    reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base() + ue::layout::kApplySkinnedHairMode)(part.created);
            }
            Logf("NPC_RETARGET part=%s class=%s parent=%s visible=%d hidden=%d", part.variable.c_str(), Text(ue::ClassOf(part.created)).c_str(), Text(parent).c_str(), visible ? 1 : 0, hidden ? 1 : 0);
            part.finished = true; --remaining; ++progress;
        }
        if (!progress && remaining) return Fail(L"NPC 部件图存在未解决的依赖，未留下半套模型");
    }
    return remaining == 0;
}
bool IsAppearancePart(Object* component) {
    const char* keep[] = {"weapon", "knife", "knifescab", "sword", "blade", "switchkey", "scabbard", "scabard", "shield"};
    if (ue::NameContains(ue::NameOf(component), keep, std::size(keep))) return false;
    return ue::IsA(component, Class("/Script/Engine.MeshComponent")) || ue::IsA(component, Class("/Script/HairStrandsCore.GroomComponent"));
}
bool HideOriginalParts(const Context& context) {
    for (Object* component : Components(context.pawn, Class("/Script/Engine.SceneComponent"))) {
        if (component == context.mesh || component == backup_.object || !IsAppearancePart(component)) continue;
        bool visible = false, hidden = false;
        if (!VisibilityOf(component, visible, hidden)) return false;
        Visibility saved{Id(component),visible,hidden};
        if(visible&&!hidden&&Bool(component,"CastShadow",saved.cast_shadow)&&Bool(component,"bCastHiddenShadow",saved.hidden_shadow))saved.shadows_owned=true;
        hidden_.push_back(saved);
        if(saved.shadows_owned&&(!PutBool(component,"bCastHiddenShadow",false)||!Call(component,"SetCastShadow").Set("NewCastShadow",false).Invoke()))return false;
        if (!SetVisibleOwned(component,false,true,hidden_.back().visible_written,hidden_.back().hidden_written)) return false;
    }
    return true;
}
[[maybe_unused]] bool ValidateAttachments(Object* source_primary, Object* target_probe) {
    const Array children = Property<Array>(source_primary, "AttachChildren");
    if (children.num < 0 || children.num > 2048 || (children.num && !children.data)) return false;
    for (int32_t i = 0; i < children.num; ++i) {
        Object* child = static_cast<Object**>(children.data)[i]; if (!ue::Alive(child) || IsAppearancePart(child)) continue;
        const Name socket = Property<Name>(child, "AttachSocketName");
        if (!socket.None() && !SocketExists(target_probe, socket)) {
            wchar_t detail[500]{};
            swprintf_s(detail, L"当前附件 %S 的挂点 %S 尚未适配；未替换主网格", Text(child).c_str(), Text(socket).c_str());
            failure_text_ = detail; Status(L"%s", detail);
            Logf("NPC_RETARGET missing required attachment socket=%s child=%s", Text(socket).c_str(), Text(child).c_str()); return false;
        }
    }
    return true;
}
bool RestoreSourceSettings();
bool Restore(bool preserve_external = true) {
    stage_ = "restore";
    cleanup_pending_ = HasResources();
    if (Same(skeleton_edit_.skeleton) && !QuiesceSkeleton(skeleton_edit_.skeleton.object)) {
        Logf("NPC_RETARGET shared animation synchronization pending; cleanup deferred");
        return false;
    }
    if (!RestoreGripHook()) return false;
    if (!RestoreFaceHooks()) return false; // Per-hook diagnostics are bounded and identify the actual pending branch.
    bool restored = true;
    if(source_driver_mode_)primary_restored_=true;
    if (!primary_restored_ && Same(primary_) && Same(backup_)) {
        const bool still_ours = !target_mesh_.object || SkelMesh(primary_.object) == target_mesh_.object ||
            (primary_restore_started_ && SkelMesh(primary_.object) == SkelMesh(backup_.object));
        if (still_ours || !preserve_external) {
            Object* mesh = SkelMesh(backup_.object), *anim_class = Property<Object*>(backup_.object, "AnimClass");
            AnimOptions original_options, current_options;
            if (!ReadAnimOptions(backup_.object, original_options) || !ReadAnimOptions(primary_.object, current_options) ||
                (primary_options_written_ && !OwnedAnimOptions(current_options, original_options, primary_written_options_))) {
                Logf("NPC_RETARGET primary PostProcess options changed externally or unreadable; cleanup deferred without overwriting them");
                return false;
            }
            primary_restore_started_ = true;
            restored = mesh && WriteAnimOptions(primary_.object, original_options) && CopyMaterials(backup_.object, primary_.object) &&
                SetPhysics(primary_.object, nullptr) && SetMesh(primary_.object, mesh);
            if (restored) restored = SetPhysics(primary_.object, Property<Object*>(backup_.object, "PhysicsAssetOverride"));
            if (restored && ue::Alive(anim_class) && AnimClass(primary_.object) != anim_class)
                restored = Call(primary_.object, "SetAnimInstanceClass").Set("NewClass", anim_class).Invoke();
            if (restored) restored = ReinitializeAnimation(primary_.object) && SkelMesh(primary_.object) == mesh && AnimClass(primary_.object) == anim_class && AnimInstance(primary_.object) &&
                Property<Object*>(primary_.object, "PhysicsAssetOverride") == Property<Object*>(backup_.object, "PhysicsAssetOverride") &&
                VerifyPostProcess(primary_.object, original_options, "restore-primary");
            if (!restored) {
                Logf("NPC_RETARGET original primary restore incomplete; backup and temporary adaptation retained for retry");
                return false;
            }
            primary_restored_ = true;
        } else {
            // Another system took the primary. Retire only our decorations and
            // shared edits, leaving its current mesh/materials/AnimClass alone.
            Logf("NPC_RETARGET primary asset changed externally; original mesh overwrite skipped");
            primary_restored_ = true;
        }
    } else if (!primary_restored_ && Same(primary_) && backup_.object) {
        Logf("NPC_RETARGET backup identity no longer valid while primary is live; cleanup deferred");
        return false;
    } else if (!Same(primary_)) {
        primary_restored_ = true; // Old avatar was unloaded; never call it.
    }
    active_ = false;
    for (size_t i = added_.size(); i-- > 0;) {
        if (!Same(added_[i]) || Destroy(added_[i].object)) added_.erase(added_.begin() + i);
        else restored = false;
    }
    if(!added_.empty()){Publish();return false;}
    for (size_t i = hidden_.size(); i-- > 0;) {
        Visibility& saved = hidden_[i];
        if (!Same(saved.component)) { hidden_.erase(hidden_.begin() + i); continue; }
        bool visible = false, hidden = false;
        if (!VisibilityOf(saved.component.object, visible, hidden)) { restored = false; continue; }
        if(saved.shadows_owned){bool cast=false,hidden_cast=false;if(!Bool(saved.component.object,"CastShadow",cast)||!Bool(saved.component.object,"bCastHiddenShadow",hidden_cast)){restored=false;continue;}
            if(!hidden_cast&&!PutBool(saved.component.object,"bCastHiddenShadow",saved.hidden_shadow)){restored=false;continue;}
            if(!Call(saved.component.object,"SetCastShadow").Set("NewCastShadow",cast?cast:saved.cast_shadow).Invoke()){restored=false;continue;}}
        if(!RestoreVisibleOwned(saved.component.object,saved.visible,saved.hidden,saved.visible_written,saved.hidden_written)){restored=false;continue;}
        // Each attempted field above retains an independent ownership stage,
        // including a native setter that failed after its first write.
        hidden_.erase(hidden_.begin() + i);
    }
    // Keep native modes/sockets until every one of our registered followers
    // has gone; otherwise they could evaluate against half-restored resources.
    if (!added_.empty()) { Publish(); return false; }
    if(source_driver_mode_&&!RestoreSourceSettings()){Publish();return false;}
    const bool sockets_restored = RestoreSockets(), skeleton_restored = RestoreSkeleton();
    restored = restored && sockets_restored && skeleton_restored;
    if (!restored) { Publish(); return false; }
    if (Same(backup_) && !Destroy(backup_.object)) { Publish(); return false; }
    AcquireSRWLockExclusive(&pose_lock_);if(pose_session_)Logf("NPC_BODY retired pose cache builds=%llu hits=%llu",pose_session_->cache.builds,pose_session_->cache.hits);pose_session_.reset();ReleaseSRWLockExclusive(&pose_lock_);
    display_={};source_asset_={};source_driver_mode_=false;source_settings_owned_=false;source_visible_written_=source_hidden_written_=false;
    backup_ = {}; primary_ = {}; pawn_ = {}; original_class_ = {}; target_mesh_ = {};
    primary_options_written_ = false; primary_written_options_ = {};
    if(InterlockedCompareExchange(&perf_enabled_,0,0)){perf_next_report_=0;PerfReport();SetPerfEnabled(false);}
    cleanup_pending_ = false; primary_restored_ = false; primary_restore_started_ = false; selected_ = -1; Publish(); stage_ = "idle"; return true;
}

bool SourceSettingsRead(Object* source,SourceSettings& settings) {
    Call enabled(source,"IsComponentTickEnabled");
    return VisibilityOf(source,settings.visible,settings.hidden)&&Bool(source,"bEnableUpdateRateOptimizations",settings.uro)&&Bool(source,"CastShadow",settings.cast_shadow)&&Bool(source,"bCastHiddenShadow",settings.hidden_shadow)&&
        Offset(source,"VisibilityBasedAnimTickOption")>=0&&(settings.tick_option=Property<uint8_t>(source,"VisibilityBasedAnimTickOption"),true)&&enabled.Slot("ReturnValue",sizeof(bool))&&enabled.Invoke()&&(settings.tick_enabled=enabled.Get<bool>("ReturnValue"),true);
}
bool SourceBudgetReady(Object* source) {
    Object* budget=Class("/Script/AnimationBudgetAllocator.SkeletalMeshComponentBudgeted");
    if(!budget){for(Object* type=ue::ClassOf(source);type;type=ue::At<Object*>(type,ue::layout::kSuper))if(ue::NameIs(ue::NameOf(type),"SkeletalMeshComponentBudgeted"))return false;return true;}
    if(!ue::IsA(source,budget))return true;
    if(!FaceReadable(source,0x105c))return false;
    if(ue::At<void*>(source,0x1050)||ue::At<int32_t>(source,0x1058)!=-1){Logf("NPC_BODY source budget allocator is active: reversible lease unavailable, apply refused");return false;}
    return true;
}
bool AcquireSourceSettings(Object* source) {
    if(!SourceBudgetReady(source)||!SourceSettingsRead(source,source_settings_))return false;
    source_settings_owned_=true; // Retain the pre-write record even if a native setter fails partway.
    return Put(source,"VisibilityBasedAnimTickOption",uint8_t{0})&&PutBool(source,"bEnableUpdateRateOptimizations",false)&&
        PutBool(source,"bCastHiddenShadow",false)&&Call(source,"SetCastShadow").Set("NewCastShadow",false).Invoke()&&
        Call(source,"SetComponentTickEnabled").Set("bEnabled",true).Invoke()&&SetVisibleOwned(source,false,true,source_visible_written_,source_hidden_written_);
}
bool RestoreSourceSettings() {
    if(!source_settings_owned_)return true;
    if(!Same(primary_)){source_settings_owned_=false;return true;}
    SourceSettings now;if(!SourceSettingsRead(primary_.object,now))return false;Object* source=primary_.object;
    // Each field is restored only while it still contains our written value.
    // A foreign cinematic/equipment write is preserved independently.
    if(now.tick_option==0&&!Put(source,"VisibilityBasedAnimTickOption",source_settings_.tick_option))return false;
    if(!now.uro&&!PutBool(source,"bEnableUpdateRateOptimizations",source_settings_.uro))return false;
    if(now.tick_enabled&&!Call(source,"SetComponentTickEnabled").Set("bEnabled",source_settings_.tick_enabled).Invoke())return false;
    bool shadow_dirty=false;if(!now.hidden_shadow){if(!PutBool(source,"bCastHiddenShadow",source_settings_.hidden_shadow))return false;shadow_dirty=true;}
    if(!now.cast_shadow||shadow_dirty)if(!Call(source,"SetCastShadow").Set("NewCastShadow",now.cast_shadow?now.cast_shadow:source_settings_.cast_shadow).Invoke())return false;
    if(!RestoreVisibleOwned(source,source_settings_.visible,source_settings_.hidden,source_visible_written_,source_hidden_written_))return false;
    source_settings_owned_=false;return true;
}
bool SourceLeaseValid() {
    if(!source_driver_mode_)return true;SourceSettings now;
    return source_settings_owned_&&PoseIdentityValid()&&Same(primary_)&&AnimClass(primary_.object)==original_class_.object&&SourceBudgetReady(primary_.object)&&SourceSettingsRead(primary_.object,now)&&
        !now.visible&&now.hidden&&!now.uro&&now.tick_option==0&&now.tick_enabled&&!now.cast_shadow&&!now.hidden_shadow;
}
bool Apply(const Context& context,int32_t entry) {
    const Profile& profile=kProfiles[entry];stage_="load donor";
    if(!NativeHelpersMatch()||!AnimationApiReady(context.mesh))return Fail(L"原生动画接口 / EXE 签名不匹配");
    if(HasResources()&&!Restore())return Fail(L"上次 NPC 外观未能安全恢复，仍保留恢复记录");
    if(Property<Object*>(SkelMesh(context.mesh),"Skeleton")!=Class(kSourceSkeleton)||!SourceBudgetReady(context.mesh))return Fail(L"当前主角源骨架 / 动画预算状态不支持可恢复的独立显示");
    if(!HasNoLeader(context.mesh)||!FaceQuiesce(context.mesh)||!Idle(context))return Fail(L"原主角动画任务 / 空闲状态已变化，请重试");
    Object* cls=LoadDonor(profile),*cdo=nullptr;if(!cls)return Fail(L"原 NPC 蓝图加载失败");
    ue::ForEachObject([&](Object* object){if(ue::IsDefault(object)&&ue::ClassOf(object)==cls&&ue::Alive(object)){cdo=object;return false;}return true;});
    Object* donor=Property<Object*>(cdo,"Mesh"),*target=SkelMesh(donor);
    if(!target||target!=Class(profile.body)||Property<Object*>(target,"Skeleton")!=Class(profile.skeleton))return Fail(L"NPC 主体网格 / Skeleton 与审计资产不同");
    std::vector<Part> parts;AnimOptions options;
    if(!LoadParts(cls,cdo,profile,parts)||!ValidateSourceFace(profile,parts)||!ReadAnimOptions(donor,options))return false;
    // The only gameplay Mesh/ABP remains the original Hun. All following
    // changes are private components, node caches and reversible own flags.
    source_driver_mode_=true;primary_=Id(context.mesh);source_asset_=Id(SkelMesh(context.mesh));pawn_=Id(context.pawn);original_class_=Id(AnimClass(context.mesh));target_mesh_=Id(target);
    if(!PreparePoseSession(context.mesh,target,profile)||!HideOriginalParts(context)||!AcquireSourceSettings(context.mesh)){Restore();return Fail(L"独立姿势映射 / 原附件隐藏准备失败，已保留恢复记录");}
    stage_="create NPC display";
    Object* display=Add(context.pawn,Class("/Script/Engine.SkeletalMeshComponent"));
    if(display){display_=Id(display);added_.push_back(display_);pose_session_->display=display_;}
    const std::string copy_package="/HuaShira/Characters/Hero/Hero_Hun/Hun_CopyPose";Profile copy_profile=profile;copy_profile.donor=copy_package.c_str();Object* copy_class=LoadDonor(copy_profile);
    if(!display||!copy_class||!SetVisible(display,false,true)||!SetMesh(display,target)||!WriteAnimOptions(display,options)||!CopyMaterials(donor,display)||
        !SetPhysics(display,Property<Object*>(donor,"PhysicsAssetOverride"))||!Call(display,"SetAnimInstanceClass").Set("NewClass",copy_class).Invoke()||
        !Put(display,"VisibilityBasedAnimTickOption",uint8_t{0})||!PutBool(display,"bEnableUpdateRateOptimizations",false)||
        !Call(display,"SetCollisionEnabled").Set("NewType",uint8_t{0}).Invoke()||!Call(display,"SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents",false).Invoke()||
        !Call(display,"AddTickPrerequisiteComponent").Set("PrerequisiteComponent",context.mesh).Invoke()||!AttachFinish(context.pawn,display,context.mesh,Name{},Transform{})){
        Restore();return Fail(L"NPC 独立显示组件初始化失败，已请求完整恢复");}
    CopyRenderFlags(donor,display);
    if(!MatchPlayerLighting(context.mesh,display)||!HasNoLeader(display)||!VerifyPostProcess(display,options,"NPC-display")||!InstallFaceHook(display,profile,true)){
        Restore();return Fail(L"NPC 显示的独立 CopyPose / 照明校验失败，已请求恢复");}
    stage_="assemble NPC parts";Context display_context=context;display_context.mesh=display;face_wait_started_=GetTickCount64();
    if(!Assemble(display_context,donor,profile,parts)||!InstallGripHook(context.mesh,profile)||!SetVisible(display,true,false)){
        Restore();return Fail(L"NPC 部件 / 原生武器适配 / 源姿势持续更新未通过，已请求恢复");}
    if(!SourceLeaseValid()){Restore();return Fail(L"主角源动画 / 隐藏更新状态未通过读回，已请求恢复");}
    active_=true;cleanup_pending_=false;selected_=entry;Publish();stage_="idle";
    Status(L"NPC %s 已装配：保留原主角完整动画，%d 个共享骨与 %d 条原生武器挂点；需实测攻击 / 技能",profile.name,static_cast<int>(pose_session_->plan.mapped),static_cast<int>(grip_hook_->count));
    Logf("NPC_RETARGET applied id=%s source=%s display=%s sourceClassUnchanged=%d parts=%zu routes=%zu sharedAssetWrites=0",profile.id,Text(source_asset_.object).c_str(),Text(target).c_str(),AnimClass(context.mesh)==original_class_.object?1:0,added_.size(),grip_hook_->count);return true;
}
bool Inspect(int32_t entry) {
    const Profile& profile = kProfiles[entry]; stage_ = "inspect donor";
    Object* cls = LoadDonor(profile), *cdo = nullptr;
    if (!cls) return Fail(L"原 NPC 蓝图加载失败");
    ue::ForEachObject([&](Object* object) { if (ue::IsDefault(object) && ue::ClassOf(object) == cls && ue::Alive(object)) { cdo = object; return false; } return true; });
    Object* primary = Property<Object*>(cdo, "Mesh");
    std::vector<Part> parts;
    if (!primary || !LoadParts(cls, cdo, profile, parts)) return Fail(L"原 NPC 模板无法完整读取");
    Logf("NPC_RETARGET_INSPECT id=%s primary=%s body=%s skeleton=%s simulationSettingsSize=%d parts=%zu", profile.id,
        Text(primary).c_str(), Text(SkelMesh(primary)).c_str(), Text(Property<Object*>(SkelMesh(primary), "Skeleton")).c_str(),
        ue::StructSize(Class("/Script/HairStrandsCore.HairSimulationSettings")), parts.size());
    for (const Part& part : parts) {
        bool visible = false, hidden = false; const bool readable = VisibilityOf(part.source, visible, hidden);
        bool hair = false; Bool(part.source, "bInfluencedByMainHairMode", hair);
        Logf("NPC_RETARGET_INSPECT part=%s parent=%s socket=%s class=%s flagsReadable=%d visible=%d hidden=%d mainHairMesh=%d", part.variable.c_str(),
            part.parent.c_str(), Text(part.socket).c_str(), Text(ue::ClassOf(part.source)).c_str(), readable ? 1 : 0, visible ? 1 : 0, hidden ? 1 : 0, hair ? 1 : 0);
    }
    Status(L"已只读检查 %s 的 %d 个部件模板；未替换主角，详见 NPC_RETARGET_INSPECT 日志", profile.name, static_cast<int>(parts.size()));
    stage_ = "idle"; return true;
}
void Tick() {
    if (!initialized_) return;
    if (active_ && !cleanup_pending_ && !RefreshFaceVisibility()) {
        cleanup_pending_=true;
        Status(L"NPC 独立面部节点 / 原生评估状态变化，保留清理记录并于空闲时恢复原模型");
        Logf("NPC_FACE_NEUTRAL node identity/evaluation failed; restore requested, shared CDO untouched");
    }
    int32_t action = -1, entry = -1;
    AcquireSRWLockExclusive(&request_lock); action = pending_action_; entry = pending_entry_; pending_action_ = -1; ReleaseSRWLockExclusive(&request_lock);
    if (action >= 0) { waiting_action_ = action; waiting_entry_ = entry; wait_deadline_ = GetTickCount64() + 60000; }
    if (action < 0 && GetTickCount64() < next_tick_) return;
    next_tick_ = GetTickCount64() + 250;
    PerfReport();PerfScope timing(PerfKind::Maintenance);
    const bool disabled = InterlockedCompareExchange(&disabled_, 0, 0) != 0;
    if (disabled && waiting_action_ != static_cast<int32_t>(Action::Restore)) {
        AcquireSRWLockExclusive(&snapshot_lock); snapshot_.ready = false; ReleaseSRWLockExclusive(&snapshot_lock);
        Publish(); return;
    }
    Context context; const bool context_ok = ContextNow(context);
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_.ready = context_ok && !disabled; ReleaseSRWLockExclusive(&snapshot_lock);
    if (active_ && (!Same(primary_) || !Same(pawn_) || (!source_driver_mode_&&!Same(backup_)) || (source_driver_mode_&&!Same(display_)) || !context_ok || context.pawn != pawn_.object)) {
        const bool restored = Restore();
        Status(restored ? L"玩家或地图已变化，NPC 临时适配已清理" : L"玩家或地图已变化；NPC 清理尚未完整，保留可重试状态并记录日志");
    } else if (active_ && (SkelMesh(primary_.object) != (source_driver_mode_?source_asset_.object:target_mesh_.object)|| (source_driver_mode_&&SkelMesh(display_.object)!=target_mesh_.object))) {
        const bool restored = Restore();
        Status(restored ? L"游戏更换了主网格，已停止 NPC 覆盖并清理临时设置" : L"游戏更换了主网格；已保留外部改动，部分共享设置需查看日志");
    }
    if (active_ && context_ok && !cleanup_pending_) {
        if(source_driver_mode_)RefreshForearmBindings();
        if (!SourceLeaseValid() || !RefreshOwnedLighting() || (pose_session_&&InterlockedCompareExchange(&pose_session_->failed,0,0)) || !grip_hook_ || ue::At<uintptr_t*>(primary_.object,0)!=grip_hook_->table ||
            InterlockedCompareExchange(&grip_hook_->failures,0,0)) {
            cleanup_pending_=true;
            Logf("NPC_RETARGET light channel/current-pose weapon adapter readback failed; retiring at next idle");
            Status(L"NPC 照明通道 / 当前武器姿势校验发生变化，将于空闲时恢复原模型");
        }
        if (Object* conflict = source_driver_mode_?nullptr:ForeignSkeletonUser(context, Property<Object*>(target_mesh_.object, "Skeleton"))) {
            cleanup_pending_ = true;
            Logf("NPC_RETARGET another shared-skeleton user appeared: component=%s; retiring experiment at next idle", Text(conflict).c_str());
            Status(L"场景新增同骨架角色；NPC 实验适配将于空闲时恢复。后续生成同骨架角色前应先恢复主角");
        }
    }
    if (cleanup_pending_ && waiting_action_ < 0) {
        // Rollback may have failed before active_=true. Keep retrying that
        // ledger too, while avoiding a live current-avatar montage reset.
        if (!Same(primary_) || primary_restored_ || !context_ok || context.mesh != primary_.object || Idle(context)) {
            const bool restored = Restore();
            Status(restored ? L"未完成的 NPC 临时适配已清理，可重新选择模型" : L"NPC 清理尚未完整，已保留恢复记录；新外观切换保持阻断");
        }
        Publish();
    }
    if (waiting_action_ < 0) return;
    if (GetTickCount64() > wait_deadline_) { waiting_action_ = -1; Publish(); Status(L"等待空闲或有效玩家超时，未执行 NPC 更换"); return; }
    if (waiting_action_ == static_cast<int32_t>(Action::Restore) && (!Same(primary_) || primary_restored_ || !context_ok || context.mesh != primary_.object)) {
        waiting_action_ = -1; const bool restored = Restore();
        Status(restored ? L"NPC 临时适配已清理" : L"NPC 清理尚未完整，保留恢复记录以便重试"); Publish(); return;
    }
    if (!context_ok || !Idle(context)) { Publish(true); Status(L"等待主角空闲且没有正在播放的 Montage，再执行 NPC 更换 / 恢复"); return; }
    if (waiting_action_ == static_cast<int32_t>(Action::Apply)) {
        if (!release_appearance_) {
            waiting_action_ = -1; Publish(); Fail(L"尚未接入主角外观释放适配器；检查模板可用，应用已阻断"); return;
        }
        if (!release_appearance_()) { Publish(true); Status(L"先完整释放上一种主角外观 / 发型覆盖，再应用 NPC 原模型；恢复未成功时保持等待"); return; }
    }
    action = waiting_action_; entry = waiting_entry_; waiting_action_ = -1;
    if (action == static_cast<int32_t>(Action::Restore)) { const bool restored = Restore(); Status(restored ? L"已恢复原主角模型、材质与临时骨架 / 挂点设置" : L"恢复尚未完整：部分对象、动画任务或共享设置已变化，请查看日志后重试"); }
    else if (entry >= 0 && static_cast<size_t>(entry) < std::size(kProfiles)) {
        failure_text_.clear(); Publish(true);
        const bool ok = action == static_cast<int32_t>(Action::Inspect) ? Inspect(entry) : Apply(context, entry);
        if (!ok) {
            Logf("NPC_RETARGET apply failed; rollback attempted");
            if (!failure_text_.empty()) Status(L"NPC 重定向未完成：%s", failure_text_.c_str());
            else Status(L"NPC 重定向未完成，已尝试恢复原模型；详见 NPC_RETARGET 日志");
        }
        Publish();
    }
}
int FaultFilter(const EXCEPTION_POINTERS* exception) {
    fault_code_ = exception->ExceptionRecord->ExceptionCode; fault_address_ = reinterpret_cast<uintptr_t>(exception->ExceptionRecord->ExceptionAddress); return EXCEPTION_EXECUTE_HANDLER;
}
void Recover() { __try { Restore(); } __except (EXCEPTION_EXECUTE_HANDLER) {} }
void GuardedTick() {
    __try { Tick(); }
    __except (FaultFilter(GetExceptionInformation())) {
        ++faults_; waiting_action_ = -1;
        Logf("NPC_RETARGET fault %lu/3 code=0x%08lx address=%p stage=%s", faults_, fault_code_, reinterpret_cast<void*>(fault_address_), stage_);
        Recover(); if (faults_ >= 3) InterlockedExchange(&disabled_, 1);
        Status(L"NPC 重定向错误 0x%08lx，已停止并尝试恢复；Boss Rush 与原调试继续运行", fault_code_); Publish();
    }
}
}

void Initialize(Logger logger, ReleaseAppearance release_appearance) {
    logger_ = logger; release_appearance_ = release_appearance;InterlockedExchange(&perf_enabled_,0);Snapshot state;
    state.appearance_gate = release_appearance_ != nullptr;
    for (size_t i = 0; i < std::size(kProfiles); ++i) state.entries.push_back({static_cast<int32_t>(i), kProfiles[i].id, kProfiles[i].name, kProfiles[i].detail});
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_ = std::move(state); ReleaseSRWLockExclusive(&snapshot_lock);
    initialized_ = true; Logf("NPC_RETARGET initialized profiles=%zu native AnimationRelative experiment", std::size(kProfiles));
}
bool Request(Action action, int32_t entry) {
    if (action == Action::Apply && !InterlockedCompareExchange(&experimental_, 0, 0)) return false;
    if ((InterlockedCompareExchange(&disabled_, 0, 0) && action != Action::Restore) || ((action == Action::Apply || action == Action::Inspect) && (entry < 0 || static_cast<size_t>(entry) >= std::size(kProfiles)))) return false;
    AcquireSRWLockExclusive(&request_lock); pending_action_ = static_cast<int32_t>(action); pending_entry_ = entry; ReleaseSRWLockExclusive(&request_lock);
    Status(L"NPC 重定向请求已排队；更换前会检查空闲状态、部件与武器挂点"); return true;
}
bool SetExperimental(bool enabled) {
    if (enabled && InterlockedCompareExchange(&disabled_, 0, 0)) return false;
    InterlockedExchange(&experimental_, enabled ? 1 : 0);
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_.experimental = enabled; ReleaseSRWLockExclusive(&snapshot_lock);
    if (!enabled) return Request(Action::Restore);
    Status(L"NPC 实验开关已打开；仍需通过场景、外观互斥、骨架与挂点校验"); return true;
}
Snapshot GetSnapshot() { AcquireSRWLockShared(&snapshot_lock); Snapshot copy = snapshot_; ReleaseSRWLockShared(&snapshot_lock); return copy; }
void OnGameThreadGuarded() { GuardedTick(); }
bool BeforeTravel() {
    AcquireSRWLockExclusive(&request_lock); pending_action_ = -1; ReleaseSRWLockExclusive(&request_lock);
    __try { waiting_action_ = -1; return Restore(); }
    __except (FaultFilter(GetExceptionInformation())) { Logf("NPC_RETARGET pre-travel cleanup fault 0x%08lx", fault_code_); return false; }
}
bool ReleaseForExternal() {
    AcquireSRWLockExclusive(&request_lock); pending_action_ = -1; ReleaseSRWLockExclusive(&request_lock);
    __try {
        waiting_action_ = -1;
        if (!HasResources()) { Publish(); return true; }
        Context context;
        if (!primary_restored_ && Same(primary_) && ContextNow(context) && context.mesh == primary_.object && !Idle(context)) {
            Status(L"先等待当前 NPC 模型空闲，再恢复并切换到主角套装"); Publish(true); return false;
        }
        const bool restored = Restore(); Publish(); return restored;
    } __except (FaultFilter(GetExceptionInformation())) {
        cleanup_pending_ = HasResources(); Publish();
        Logf("NPC_RETARGET appearance-release fault 0x%08lx; preserved cleanup ledger", fault_code_); return false;
    }
}
bool OwnsAppearance() { return active_ || HasResources(); }
namespace {
bool BeforeActorClassSpawnImpl(Object* actor_class) {
    std::vector<Object*> modified;
    const auto add_skeleton = [&](Identity identity) {
        if (Same(identity) && std::find(modified.begin(), modified.end(), identity.object) == modified.end()) modified.push_back(identity.object);
    };
    add_skeleton(skeleton_edit_.skeleton);
    for (const SocketAdded& socket : sockets_) add_skeleton(socket.skeleton);
    if (modified.empty()) return true;
    const auto retire = [&](const char* reason) {
        Logf("NPC_RETARGET pre-spawn release class=%s reason=%s", ue::Alive(actor_class) ? Text(actor_class).c_str() : "invalid", reason);
        const bool released = ReleaseForExternal();
        if (!released) Status(L"生成前需恢复共享骨架：等待当前 NPC 模型空闲并完整清理，尚未生成角色");
        return released;
    };
    if (!ue::Alive(actor_class) || !ue::IsA(actor_class, Class("/Script/CoreUObject.Class"))) return retire("invalid/unresolved actor class");
    std::vector<Object*> lineage, defaults, inspected;
    for (Object* type = actor_class; type; type = ue::At<Object*>(type, ue::layout::kSuper)) {
        if (!ue::Alive(type) || lineage.size() >= 64 || std::find(lineage.begin(), lineage.end(), type) != lineage.end()) return retire("unresolved class ancestry");
        lineage.push_back(type); defaults.push_back(nullptr);
    }
    ue::ForEachObject([&](Object* object) {
        if (!ue::IsDefault(object) || !ue::Alive(object)) return true;
        const auto owner = std::find(lineage.begin(), lineage.end(), ue::ClassOf(object));
        if (owner != lineage.end()) defaults[static_cast<size_t>(owner - lineage.begin())] = object;
        return true;
    });
    if (defaults.empty() || !defaults.front()) return retire("class default object not loaded");
    bool unknown = false, intersects = false;
    Object* skinned_class = Class("/Script/Engine.SkinnedMeshComponent"), *skeletal_class = Class("/Script/Engine.SkeletalMeshComponent");
    if (!skinned_class || !skeletal_class) return retire("skinned component metadata unavailable");
    const auto inspect = [&](Object* component) {
        if (!component) { unknown = true; return; }
        if (!ue::Alive(component)) { unknown = true; return; }
        if (!ue::IsA(component, skinned_class) || std::find(inspected.begin(), inspected.end(), component) != inspected.end()) return;
        inspected.push_back(component);
        Object* asset = ue::IsA(component, skeletal_class) ? SkelMesh(component) : Property<Object*>(component, "SkinnedAsset");
        if (!ue::Alive(asset)) { unknown = true; return; }
        const int32_t skeleton_offset = Offset(asset, "Skeleton");
        Object* skeleton = skeleton_offset >= 0 ? Property<Object*>(asset, "Skeleton") : nullptr;
        if (!ue::Alive(skeleton)) { unknown = true; return; }
        if (std::find(modified.begin(), modified.end(), skeleton) != modified.end()) {
            intersects = true;
            Logf("NPC_RETARGET pre-spawn shared Skeleton intersection component=%s asset=%s skeleton=%s", Text(component).c_str(), Text(asset).c_str(), Text(skeleton).c_str());
        }
    };
    // Only the concrete CDO is an effective actor default. Its component list
    // includes inherited native default subobjects. Do not reinterpret empty
    // native-base CDO meshes as extra components spawned by the concrete class.
    Call get(defaults.front(), "K2_GetComponentsByClass");
    if (!get.Set("ComponentClass", skinned_class).Invoke()) unknown = true;
    else {
        const Array components = get.Get<Array>("ReturnValue");
        if (components.num < 0 || components.num > 2048 || components.max < components.num || (components.num && !components.data)) unknown = true;
        else for (int32_t i = 0; i < components.num; ++i) inspect(static_cast<Object**>(components.data)[i]);
        ue::Free(components.data);
    }
    if (Field(ue::ClassOf(defaults.front()), "Mesh")) inspect(Property<Object*>(defaults.front(), "Mesh"));
    ue::ForEachObject([&](Object* component) {
        if (!ue::IsTemplate(component) || !ue::Alive(component) || !ue::IsA(component, skinned_class)) return true;
        for (size_t i = 0; i < lineage.size(); ++i)
            if (SourceOwner(component, lineage[i], i == 0 ? defaults.front() : nullptr)) { inspect(component); break; }
        return true;
    });
    for (Object* type : lineage) {
        Object* scs = Property<Object*>(type, "SimpleConstructionScript");
        if (!scs) continue;
        if (!ue::Alive(scs)) { unknown = true; continue; }
        const Array nodes = Property<Array>(scs, "AllNodes");
        if (nodes.num < 0 || nodes.num > 2048 || nodes.max < nodes.num || (nodes.num && !nodes.data)) { unknown = true; continue; }
        for (int32_t i = 0; i < nodes.num; ++i) {
            Object* node = static_cast<Object**>(nodes.data)[i];
            if (!ue::Alive(node)) { unknown = true; continue; }
            inspect(Property<Object*>(node, "ComponentTemplate"));
        }
    }
    if (intersects) return retire("authored class uses an owned modified Skeleton");
    if (unknown) return retire("unresolved skinned template asset/Skeleton");
    Logf("NPC_RETARGET pre-spawn class=%s authored skinned templates=%zu disjoint; keeping current NPC model", Text(actor_class).c_str(), inspected.size());
    return true;
}
}
bool BeforeActorClassSpawn(Object* actor_class) {
    __try { return BeforeActorClassSpawnImpl(actor_class); }
    __except (FaultFilter(GetExceptionInformation())) {
        Logf("NPC_RETARGET pre-spawn inspection fault 0x%08lx; requiring conservative appearance release", fault_code_);
        return ReleaseForExternal();
    }
}
bool HandleCommand(const char* verb, const char* arguments) {
    if(!std::strcmp(verb,"npcperf")){std::istringstream input(arguments?arguments:"");std::string value;input>>value;if(value!="on"&&value!="off")return false;return SetPerfEnabled(value=="on");}
    if (std::strcmp(verb, "npcretarget")) return false;
    std::istringstream input(arguments ? arguments : ""); std::string action, id; input >> action >> id;
    if(action=="perf"){if(id!="on"&&id!="off")return false;return SetPerfEnabled(id=="on");}
    if (action == "experimental") {
        if (id != "on" && id != "1" && id != "off" && id != "0") return false;
        return SetExperimental(id == "on" || id == "1");
    }
    if (action == "restore") return Request(Action::Restore);
    if (action == "status") { const Snapshot state = GetSnapshot(); Logf("NPC_RETARGET_STATUS %s", Utf8(state.status.c_str()).c_str()); return true; }
    if (action == "list") { for (size_t i = 0; i < std::size(kProfiles); ++i) Logf("NPC_RETARGET_PROFILE %zu id=%s name=%s", i, kProfiles[i].id, Utf8(kProfiles[i].name).c_str()); return true; }
    if (action == "apply" || action == "inspect") for (size_t i = 0; i < std::size(kProfiles); ++i)
        if (Lower(id) == Lower(kProfiles[i].id) || id == std::to_string(i)) return Request(action == "apply" ? Action::Apply : Action::Inspect, static_cast<int32_t>(i));
    return false;
}
}
