#include "npc_retarget.h"
#include "ue_runtime.h"
#include "npc_grip_math.h"
#include "npc_body_collision.h"
#include "npc_pose_transfer.h"
#include "grab_extras.h"
#include "skill_toast.h"
#include "ui_hotkeys.h"
#include "settings_store.h"
#include "entry_protection.h"
#include "motian_grab.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <map>
#include <memory>
#include <set>
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
// mode: the bone's cooked EBoneTranslationRetargetingMode on that Skeleton; length: its reference translation length;
// animated: that Skeleton's animations carry the bone (the engine fills an untracked one with the mesh rest).
struct BodySkeletonBasis { const char* skeleton;const char* bone;double q0[4];uint8_t mode;double length;uint8_t animated; };
#include "npc_body_basis.generated.h"
struct Profile {
    const char* id; const wchar_t* name; const char* donor; const char* body; const char* skeleton; int32_t bones;
    const uint16_t* shared; size_t shared_count; const char* const* parts; size_t part_count;
    const BoneFallback* fallbacks; size_t fallback_count; const GripBasis* grips; size_t grip_count;
    const char* head; const char* head_postprocess; const uint16_t* facial_bones; size_t facial_count; int32_t head_bones; const wchar_t* detail;
};
#include "npc_retarget_data.generated.h"
#include "npc_weapon_routes.generated.h"
#include "npc_skills.generated.h"
static_assert(std::size(kNpcSkillSets)==std::size(kProfiles),"npc_skills.generated.h rows follow kProfiles");
#include "npc_skill_colliders.generated.h"
static_assert(std::size(kNpcColliderTagSets)==std::size(kNpcSkillSets),"npc_skill_colliders.generated.h rows follow kNpcSkillSets");
#include "npc_weapon_displays.generated.h"
static_assert(std::size(kNpcWeaponDisplays)==std::size(kNpcSkillSets),"npc_weapon_displays.generated.h rows follow kNpcSkillSets");
#include "npc_chains.generated.h"
// A second creature under the display Skeleton root that the hero has no bones for (mount_root, bones in its
// subtree) and the boss's own locomotion loops; ridden: the rider sits on it (a horse).
struct MountLoops { const char* skeleton;const char* mount_root;int32_t bones;uint8_t ridden;const char* idle;const char* walk;const char* run; };
#include "npc_mounts.generated.h"
constexpr uintptr_t kArrayAddRva = 0x0406ce30, kArrayRemoveRva = 0x0406db20;
// Set only after exact-PE proof. Array edits alone do not update Shipping SocketMap.
constexpr uintptr_t kRebuildSocketMapRva = 0x044fc870;
constexpr uintptr_t kSocketTransformRva = 0x03db50e0;
constexpr uintptr_t kSocketInfoRva = 0x03db49c0;
constexpr uintptr_t kGetMeshReferenceRva=0x044f5ae0;
constexpr size_t kSocketTransformSlot = 0x518 / sizeof(uintptr_t);
struct NativeSignature { uintptr_t rva; unsigned char code[16]; };
constexpr NativeSignature kNativeSignatures[] = {
    {kArrayAddRva, {0x48,0x8b,0xc4,0x53,0x56,0x41,0x55,0x48,0x83,0xec,0x50,0x4d,0x8b,0xe8,0x48,0x8b}},
    {kArrayRemoveRva, {0x48,0x85,0xc9,0x0f,0x84,0xae,0x01,0x00,0x00,0x48,0x89,0x6c,0x24,0x18,0x48,0x89}},
    {kRebuildSocketMapRva, {0x40,0x55,0x53,0x56,0x57,0x41,0x56,0x48,0x8d,0x6c,0x24,0xc9,0x48,0x81,0xec,0xe0}},
    {kSocketTransformRva, {0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x8d,0xac,0x24}},
    {kSocketInfoRva, {0x48,0x89,0x5c,0x24,0x18,0x48,0x89,0x54,0x24,0x10,0x55,0x56,0x57,0x41,0x56,0x41}},
    {kGetMeshReferenceRva,{0x40,0x53,0x48,0x83,0xec,0x20,0xba,0x04,0x00,0x00,0x00,0x41,0xb8,0x03,0x00,0x00}},
    {0x1267790, {0x48,0x8b,0x05,0xb1,0x10,0xaa,0x0c,0xc3,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc,0xcc}},
    {0x1f27810, {0x48,0x89,0x5c,0x24,0x10,0x48,0x89,0x74,0x24,0x20,0x57,0x48,0x81,0xec,0x80,0x00}},
    {0x3da9320, {0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xec,0x20,0x48}},
    {0x5cc9a30, {0x48,0x89,0x5c,0x24,0x20,0x57,0x48,0x83,0xec,0x20,0x48,0x83,0xb9,0x50,0x10,0x00}},
};
// Grooms are bound to the head but culled by their own bounds; 4x covers crouch, roll and lunge poses.
constexpr float kHairBoundsScale = 4.0f, kNoPhysicsBoundsScale = 2.0f;
constexpr const char* kSourceSkeleton = "/HuaShira/Characters/Hero/Hero_Hun/Hun_Skin_Skeleton.Hun_Skin_Skeleton";
// Hun_Skin_Skeleton bone translation retarget modes (cooked BoneTree, character_swap/reference_bones.json):
// these 65 are Animation, ball_l / ball_r Skeleton, the other 1067 AnimationScaled.
constexpr const char* kSourceAnimationBones[] = {
    "hand_l","pinky_metacarpal_l","pinky_01_l","pinky_02_l","pinky_03_l","ring_metacarpal_l","ring_01_l","ring_02_l","ring_03_l",
    "middle_metacarpal_l","middle_01_l","middle_02_l","middle_03_l","index_metacarpal_l","index_01_l","index_02_l","index_03_l",
    "thumb_01_l","thumb_02_l","thumb_03_l","wrist_inner_l","wrist_outer_l","weapon_l","weapon_l_Inst","hand_r","pinky_metacarpal_r",
    "pinky_01_r","pinky_02_r","pinky_03_r","ring_metacarpal_r","ring_01_r","ring_02_r","ring_03_r","middle_metacarpal_r","middle_01_r",
    "middle_02_r","middle_03_r","index_metacarpal_r","index_01_r","index_02_r","index_03_r","thumb_01_r","thumb_02_r","thumb_03_r",
    "wrist_inner_r","wrist_outer_r","weapon_r","weapon_r_Inst","ik_foot_root","ik_foot_l","ik_foot_r","ik_hand_root","ik_hand_gun",
    "ik_hand_l","ik_hand_r","interaction","center_of_mass","Pt_Cam","ik_weapon_root","ik_weapon_l","ik_weapon_r","weapon_l_child",
    "weapon_r_child","ik_weapon_l_child","ik_weapon_r_child"};
constexpr const char* kSourceSkeletonBones[] = {"ball_l","ball_r"};
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
    int32_t bone=-1; // source bone of the same name: a route that is a bare bone (weapon_r, WEAPON_R, lowerarm_l) has no socket
};
struct ForearmBinding {
    const ForearmBasis* basis=nullptr;Name bone;bool enabled=false;const char* reason="not-equipped";
    Identity actor,mesh;int32_t actor_index=-1,actor_serial=0,mesh_index=-1,mesh_serial=0;
    int32_t leader=-1,anim_class=-1,anim_instance=-1,hidden=-1;uint8_t hidden_byte=0,hidden_mask=0;
};
// proxy: a hand-chain bone this bone rests on (Hun's ik_weapon_r rests on weapon_r); mounts on it follow that hand.
struct GripAnchor { int32_t target=-1,palm=-1,target_palm=-1,proxy=-1;double basis[4]{0,0,0,1};bool hand=false,forearm=false; };
struct GripHook {
    Identity component; uintptr_t* original = nullptr; uintptr_t* table = nullptr;
    // Routes: the generated weapon mounts, plus any socket another actor (a switched or Shaqi
    // weapon) is found attached to at runtime (RefreshWeaponRoutes). count only grows.
    std::array<GripSocket,48> sockets{}; size_t count=0; volatile LONG queries=0, failures=0;
    std::array<ForearmBinding,2> forearms{};
    std::vector<GripAnchor> anchors;
    // Effect routes (BuildDisplayRoutes): a name the NPC display has and the hero mesh lacks (a boss's
    // BulletOrigin) reads the display's own socket; while an NPC skill runs (skill_route_until_) so do
    // the names both have, so the boss's effects and bullets start from the body that is drawn.
    // Sorted FName keys; the display is destroyed only after this hook is restored.
    Identity display; std::vector<uint64_t> display_only, shared; volatile LONG display_queries=0;
};
#pragma warning(pop)
struct SkeletonEdit { Identity skeleton; uintptr_t data = 0; bool old_flag = false; std::vector<uint8_t> original, written; };
struct FaceHook {
    Identity component, instance; Object* node = nullptr; int32_t instance_index = -1, instance_serial = 0;
    int32_t component_index = -1, component_serial = 0, runtime_bones = 0;
    uintptr_t* original = nullptr; std::unique_ptr<uintptr_t[]> table;
    const Profile* profile = nullptr; uint8_t old_curve = 0, old_attributes = 0;
    void* map_data = nullptr; int32_t map_slots = 0; uint64_t source_mesh = 0, target_mesh = 0;
    std::vector<int32_t> saved; volatile LONG failed = 0; uint32_t epochs = 0;
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
    // Per target bone: the source slot a virtual bone copies in the CopyPose map (its target bone's), else -1 (reference pose).
    std::vector<int32_t> virtual_source;
    Identity puppet; // the mount pose puppet (kMountLoops) once created, else empty
    volatile LONG failed=0;const char* why="not-evaluated";
};
std::unique_ptr<PoseSession> pose_session_;
SRWLOCK pose_lock_=SRWLOCK_INIT;
std::vector<Identity> added_;
std::vector<Visibility> hidden_;
std::vector<Visibility> authored_; // Created parts whose donor template is hidden.
ULONGLONG drift_log_time_=0;unsigned drift_logged_=0;
std::vector<SocketAdded> sockets_;
std::vector<Identity> socket_maps_;
SkeletonEdit skeleton_edit_;
SRWLOCK grip_lock_ = SRWLOCK_INIT;
std::unique_ptr<GripHook> grip_hook_;
// Like the engine Tick hook, retired vtable storage stays valid until process
// exit: a native caller may already have loaded the old table before restore.
std::vector<void*> retired_grip_tables_;
// Taller NPCs: the hero camera's pivot is raised by the NPC-minus-hero head
// height, through a named additive target offset on the ATPC camera that is
// set back to zero on restore. Small differences leave the camera alone.
// The full head difference: the actor itself never moves (the capsule keeps the hero half-height).
// The offset is set again every kCameraHoldMs so a camera mode change in a skill cannot drop it.
Identity camera_;double camera_lift_=0,camera_applied_=0;bool camera_owned_=false;ULONGLONG camera_probe_at_=0,camera_hold_at_=0;uint32_t camera_holds_=0;
constexpr ULONGLONG kCameraHoldMs=2000;
constexpr wchar_t kCameraAdditive[]=L"PBZ_NpcRetargetHeight";
// Giant bosses stand up to ~3 m above the hero's head (力者 +285 cm, 清道夫 +298 cm).
constexpr double kCameraLiftMin=5.0,kCameraLiftLow=-40.0,kCameraLiftHigh=300.0;
// Largest wrist turn MatchArmsToHero gives a hand, relative to its matched forearm.
constexpr double kWristMatchMaxDeg=45.0;
// Weapon size follows the body (NpcPoseTransfer::WeaponScale): 0.81x (WuWuWu) to 4.1x (PowerMan) on
// the 74 profiles; the clamp only guards a broken measurement.
constexpr double kWeaponScaleMin=0.5,kWeaponScaleMax=5.0;
// A hero bone resting this close to a hand-chain bone is an IK copy of it (NpcPoseTransfer::HandProxies).
constexpr double kHandProxyCm=0.5;
// Weapons attached to the pawn as last seen by RefreshWeaponRoutes (component, socket), logged once per change.
std::vector<std::pair<Object*,uint64_t>> weapon_children_;ULONGLONG weapon_log_due_=0;
// Body collision of the skin (NpcBodyCollision): the movement capsule takes the combat blueprint's
// own capsule (kNpcSkillSets[].monster), the other body shapes on the pawn scale with it. Everything
// written is recorded and put back on restore unless the game has since changed it.
constexpr double kCapsuleRatioMin=0.5,kCapsuleRatioMax=4.0;
struct ShapeEdit { Identity component;NpcBodyCollision::Shape original,applied;bool on_socket=false; };
// radius / half: the pawn class default capsule; kept_radius: the skin's (NpcBodyCollision::KeptRadius).
struct BodyCollision {
    bool owned=false;Identity pawn,capsule;double radius=0,half=0,kept_radius=0;
    NpcBodyCollision::CapsulePlan plan;std::vector<ShapeEdit> shapes;uint32_t reapplied=0;
};
BodyCollision body_;double stature_ratio_=0;
// Grabs keep the NPC display at its own size: GrabExtras scales the boss weapon by the skin's size over
// the grab's boss. skin_size_: the skin's rest pelvis height / the hero's.
double skin_size_=1;
// A montage authored for another skeleton on the hero (a boss skill, a borrowed grab) brings that
// skeleton's bone offsets: the display keeps its own rest translations meanwhile (NpcPoseTransfer::
// Evaluate foreign), eased over kForeignBlendMs. foreign_weight_ is read on the animation threads.
// foreign_own_: that montage is the NPC's own skeleton (its skills): the display takes the track back.
std::atomic<ULONGLONG> skill_route_until_{0};constexpr ULONGLONG kSkillRouteTailMs=2000; // effects a skill leaves behind
float audit_speed_=1.0f; // npcretarget audit speed <x> while an audit runs (world time dilation); 1 otherwise
std::atomic<float> skill_damage_{kSkillDamageDefault};constexpr wchar_t kSkillSection[]=L"NpcSkill"; // ScaleSkillDamage
std::atomic<double> foreign_weight_{0.0};std::atomic<bool> foreign_own_{false};double foreign_from_=0,foreign_to_=0;ULONGLONG foreign_start_=0;Object* foreign_montage_=nullptr;
constexpr ULONGLONG kForeignBlendMs=150;
// Mounts (kMountLoops): a pose puppet, an invisible single-node SkeletalMeshComponent on the display mesh,
// plays the boss's own animation: its own montage at the hero's position while one runs, else its idle /
// walk / run loop by the hero's speed, positions advanced here (rate 0, no notifies). NpcPoseTransfer takes
// the mount (and on a ridden one the rider's pelvis and legs) from it always, the whole body during own
// montages. mount_serial_ changes with the animation it plays (read on the animation path).
struct Mount { const MountLoops* row=nullptr;Identity puppet;Object* loops[3]{};double length[3]{};Object* playing=nullptr;int32_t gait=-1,loop=-1;double position=0;
    ULONGLONG last_ms=0;uint32_t switches=0;std::vector<Identity> pins; };
Mount mount_;std::atomic<uint32_t> mount_serial_{0};
// Speed bands (cm/s) for the loops: walk from 30 (idle again under 15), run from 420 (walk again under 360).
constexpr double kMountWalkCm=30,kMountIdleCm=15,kMountRunCm=420,kMountWalkBackCm=360;
// Display post-process nodes aimed by the boss's own state (QuietAimNodes): the instance last quieted.
Object* quiet_instance_=nullptr;
// A skin dropped because the map or the player changed is put back in the next scene: resume_entry_
// once the player has been valid for kResumeSettleMs (then the usual idle wait), at most kResumeTries.
int32_t resume_entry_=-1;ULONGLONG resume_at_=0;uint32_t resume_tries_=0;volatile LONG forget_resume_=0;
constexpr ULONGLONG kResumeSettleMs=2500;constexpr uint32_t kResumeTries=3;
// L3 + R3: the skin's boss skills (kNpcSkillSets[].skills) in random rounds (NpcBodyCollision::SkillRound),
// each granted to the hero's own ASC the way the game's own grab bullet grants one (K2_GiveAbility +
// TryActivateAbility) and removed when it ends. The combat blueprint and every cast skill class hold a
// counted strong reference while the skin is on, so the boss's montages, bullets and effects stay loaded.
// relock_until: R3 is also the game's lock-on toggle, so the lock is put back if lost until then.
struct SkillCast { Identity asc,ability_class;int32_t handle=0;ULONGLONG started=0,relock_until=0;uint32_t relocks=0;std::string name;bool seen_active=false; };
SkillCast skill_;NpcBodyCollision::SkillRound skill_round_;std::vector<Identity> skill_pins_;volatile LONG skill_request_=0;
// TryActivateAbility says no while the hero is still finishing a move (no tag, cooldown or cost involved),
// so a refused skill stays granted and is tried again every kSkillRetryMs for kSkillWaitMs; only then does
// the press move on to the round's next skill (at most kSkillTriesPerPress skills per press).
// exact: a chosen skill (npcretarget cast, the audit); once it gives up waiting no other skill is tried.
struct PendingCast { Identity asc,ability_class;int32_t handle=0;size_t pick=0,tries=0;ULONGLONG since=0,until=0,next_try=0,load_ms=0;std::string name;bool pinned=false,exact=false; };
PendingCast pending_;
constexpr ULONGLONG kSkillTimeoutMs=30000,kSkillWaitMs=2500,kSkillRetryMs=100,kSkillRelockMs=1500;constexpr size_t kMaxSkillPins=96,kSkillTriesPerPress=4; // 64 skills + boss weapons
constexpr uint32_t kSkillRelocksMax=3;
// While a skill runs the boss's own weapons (kNpcSkillSets[].weapons) hang on the NPC display and the
// hero's weapon actors are hidden part by part (HideHeroWeapons); both are undone when the skill ends.
// boss: the kNpcSkillSets[].weapons row the hero weapon stands for as a bullet / effect source.
// boss_weapon_rows_: the shown boss weapon component per weapons row (empty when it failed);
// skill_colliders_: the boss attack colliders copied for the skill (AddSkillColliders);
// seen_effects_: effects MoveSkillEffects has handled (parent: where it put or left it), or found before the skill.
struct HiddenPart { Identity component;bool was_hidden=false; };
struct HiddenWeapon { Identity actor;std::vector<HiddenPart> parts;size_t boss=0; };
struct SeenEffect { Identity effect;Object* parent=nullptr; };
std::vector<Identity> skill_weapons_,boss_weapon_rows_,skill_colliders_,skill_chains_;
struct VoiceSlot { Identity owner;Object** slot=nullptr;Object* written=nullptr;std::string original; };
std::vector<VoiceSlot> voice_slots_;std::vector<SeenEffect> seen_effects_;std::vector<HiddenWeapon> hidden_weapons_;size_t moved_effects_=0,unplaced_effects_=0;
// stand_ins_: the actors standing in for weapon display indices the hero's weapon lacks (AddStandIns), index = theirs.
struct StandIn { Identity actor;int32_t index=-1; };
std::vector<StandIn> stand_ins_;
// Runtime audit (npcretarget audit start [skin] [skill] [end skin]): every skin from the start one is put on and
// each of its skills cast in order on a frozen hostile dummy kAuditDummyCm ahead of the hero's start spot (the
// hero under the boss-entry debug invulnerability, back on that spot before each cast). One NPC_AUDIT_SKILL
// line per skill: cast result, montage, boss weapons shown, colliders copied, new effect components by what
// they hang on, new bullet objects, health drops on the dummy. Damage is the boss's own (x1) meanwhile.
// offset_cm: on a hero part, how far that part's socket is from the same socket on the NPC body / boss weapon
// (-1: no NPC part has the socket, so the effect stays where the hero's hidden part is).
struct AuditFx { Identity effect;const char* anchor="world";std::string asset,socket;bool hidden=false;double offset_cm=-1; };
struct AuditState {
    enum class Phase { Off, Begin, Dummy, DummyWait, Skin, Settle, Run, GrabNext, GrabPrepare, GrabTrigger, GrabRun, Finish } phase=Phase::Off;
    size_t profile=0,skill=0,first_skill=0,end=0,skins=0,skills=0;bool requested=false,placed=false,started=false;
    // grabs mode (npcretarget audit grabs [first variant]): every supported grab variant on the live dummy.
    bool grabs=false,grab_active=false,grab_paired=false,grab_retried=false;size_t grab=0,grabs_done=0;int32_t grab_index=-1;uint32_t grab_attempts=0,grab_activations=0,grab_hits=0;
    MotianGrab::Options grab_options;float grab_health=0;double grab_distance=150;
    ULONGLONG at=0,deadline=0,next_scan=0,begun=0,cast_at=0;double scan_ms=0;
    Identity dummy;Vector home{};double home_yaw=0;std::string stop;
    // A skill whose bullets missed the dummy at kAuditDummyCm is cast once more with it at kAuditCloseCm:
    // a hit there is reach / warping, none at all a missing attack collider.
    bool close_retry=false,hero_weapons_dumped=false;double dummy_cm=0;
    std::vector<size_t> only; // npcretarget audit list: just these skills (0-based, ascending) of the one skin
    std::string result,montage;size_t weapons_shown=0,weapons_expected=0,colliders=0,effects_moved=0,stand_ins=0;
    uint32_t hits=0,dummy_hits=0,targets=0,weapon_swaps=0;float native=0,dummy_damage=0;
    std::vector<Identity> fx_base,bullet_base;std::vector<AuditFx> fx;std::vector<std::pair<Identity,std::string>> bullets;
    std::vector<std::string> weapon_gaps; // OriginType/WeaponIndex of bullets that got no hero weapon actors
};
AuditState audit_;
// 1 start, 2 stop, 3 grabs, 4 export (request_lock). only: audit list's 0-based skills of skin `from`, ascending.
struct AuditRequest { int32_t action=0;size_t from=0,skill=0,end=0;std::vector<size_t> only; };
AuditRequest audit_request_;volatile LONG cast_request_=0; // npcretarget cast <n>: n + 1
bool Auditing() { return audit_.phase!=AuditState::Phase::Off; }

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
    if(result.serial<=0){static constexpr unsigned char weak[]={0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x12,0x48,0x8b,0xd9,0x48,0x85,0xd2,0x74};void* native=reinterpret_cast<void*>(ue::Base()+0x166e900);
        if(FaceReadable(native,sizeof(weak))&&!std::memcmp(native,weak,sizeof(weak))){int32_t value[2]{};reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(native)(value,&object);if(value[0]==result.index)result.serial=value[1];}}
    return result;
}
bool HasResources() {
    return pose_session_ || display_.object || primary_.object || backup_.object || !added_.empty() || !hidden_.empty() || !sockets_.empty() || skeleton_edit_.skeleton.object || !face_hooks_.empty() || grip_hook_ ||
        body_.owned || skill_.handle > 0 || pending_.handle > 0 || !skill_pins_.empty() || !skill_weapons_.empty() || !hidden_weapons_.empty() || !skill_colliders_.empty() || !stand_ins_.empty() || !skill_chains_.empty() || !voice_slots_.empty();
}
std::string Text(Name name) { char text[192]{}; ue::NameText(name, text, sizeof(text)); return text; }
std::string Text(Object* object) { return object ? Text(ue::NameOf(object)) : "-"; }
std::string Lower(std::string value) {
    for (char& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}
void Logf(const char* format, ...) {
    if (!logger_) return;
    char line[1000]{}; va_list arguments; va_start(arguments, format); _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, arguments); va_end(arguments); logger_(line);
}
void Status(const wchar_t* format, ...) {
    wchar_t text[640]{}; va_list arguments; va_start(arguments, format); _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, arguments); va_end(arguments);
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
uint64_t FieldKind(Object* property);
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
        if (method != ue::Base() + 0x3da9320 && method != ue::Base() + 0x5cc9a30) { ok = false; return false; }
        void* graph = reinterpret_cast<void*(__fastcall*)()>(ue::Base() + 0x1267790)();
        if (!graph) { ok = false; return false; }
        reinterpret_cast<void(__fastcall*)(void*, const void*, uint32_t)>(ue::Base() + 0x1f27810)(graph, component + 0xde8, 1);
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
int32_t PlanBone(const std::vector<NpcPoseTransfer::Bone>& bones,const char* name);
bool GeometryForAnchor(const NpcPoseTransfer::Plan& plan,int32_t si,int32_t ti,bool hand,bool forearm,double* basis);
// One retarget pose for every body. Each NPC arm starts from the hero's rest directions (upper
// arm, then forearm, each keeping its parent's turn) and each hand from the hero's hand frame,
// matched by finger geometry. The hero's motion then lands the same way on any skeleton, and
// the weapon adapter's hand turn (target hand * basis * inverse(source hand)) becomes identity
// (up to the wrist limit, kWristMatchMaxDeg).
bool MatchArmsToHero(NpcPoseTransfer::Plan& plan) {
    double degrees[2][4]{};bool matched[2]{};
    for(size_t side=0;side<2;++side){const std::string s=side?"r":"l";
        const int32_t upper=PlanBone(plan.target,("upperarm_"+s).c_str()),lower=PlanBone(plan.target,("lowerarm_"+s).c_str()),hand=PlanBone(plan.target,("hand_"+s).c_str()),source_hand=PlanBone(plan.source,("hand_"+s).c_str());
        if(upper<0||lower<0||hand<0||source_hand<0||plan.target_to_source[static_cast<size_t>(hand)]!=source_hand)continue;
        if(!NpcPoseTransfer::AlignRestDirection(plan,upper,lower,&degrees[side][0])||!NpcPoseTransfer::AlignRestDirection(plan,lower,hand,&degrees[side][1]))continue;
        double basis[4],inverse[4],tmp[4],turn[4],wrist[4];if(!GeometryForAnchor(plan,source_hand,hand,true,false,basis))continue;
        NpcGripMath::Inverse(basis,inverse);NpcGripMath::Mul(plan.source_ref_cs[static_cast<size_t>(source_hand)].rotation,inverse,tmp);
        NpcGripMath::Inverse(plan.target_ref_cs[static_cast<size_t>(hand)].rotation,inverse);NpcGripMath::Mul(tmp,inverse,turn);
        NpcGripMath::Inverse(plan.rest_align[static_cast<size_t>(lower)].data(),inverse);NpcGripMath::Mul(inverse,turn,wrist);degrees[side][2]=degrees[side][3]=NpcPoseTransfer::detail::Degrees(wrist);
        // A hand whose palm sits far off the hero's (Scavenger's left, 63 deg) keeps part of its own
        // rest pose; the weapon adapter's basis still seats the weapon in it.
        if(degrees[side][3]>kWristMatchMaxDeg){const double identity[4]={0,0,0,1};double limited[4];NpcPoseTransfer::detail::Slerp(identity,wrist,kWristMatchMaxDeg/degrees[side][3],limited);
            NpcGripMath::Mul(plan.rest_align[static_cast<size_t>(lower)].data(),limited,turn);degrees[side][2]=NpcPoseTransfer::detail::Degrees(limited);}
        matched[side]=NpcPoseTransfer::SetRestTurn(plan,hand,turn);}
    if(!NpcPoseTransfer::FinishRestTurns(plan)){Logf("NPC_BODY arms rest-pose match refused (compose)");plan.rest_set.clear();return NpcPoseTransfer::FinishRestTurns(plan);}
    Logf("NPC_BODY arms matched to hero rest pose l=%d(upper %.1f fore %.1f wrist %.1f/%.1f deg) r=%d(upper %.1f fore %.1f wrist %.1f/%.1f deg)",
        matched[0],degrees[0][0],degrees[0][1],degrees[0][2],degrees[0][3],matched[1],degrees[1][0],degrees[1][1],degrees[1][2],degrees[1][3]);
    return true;
}
bool PreparePoseSession(Object* source,Object* target_asset,const Profile& profile) {
    auto session=std::make_unique<PoseSession>();session->source=Id(source);session->source_asset=Id(SkelMesh(source));session->target_asset=Id(target_asset);
    std::vector<NpcPoseTransfer::Bone> source_refs,target_refs;size_t source_raw=0,target_raw=0;std::string why;
    if(!ReadMeshReferences(session->source_asset.object,source_refs,source_raw)||!ReadMeshReferences(target_asset,target_refs,target_raw)||target_raw!=static_cast<size_t>(profile.bones))return false;
    std::vector<Transform> source_cs,target_cs;if(!NpcPoseTransfer::ReferenceCS(source_refs,source_cs,why)||!NpcPoseTransfer::ReferenceCS(target_refs,target_cs,why))return false;
    std::map<uint64_t,size_t> source_names;for(size_t i=0;i<source_refs.size();++i)source_names.emplace(source_refs[i].name,i);
    std::map<std::string,const BodySkeletonBasis*> skeleton_bases;
    const char* skeleton_key=std::strrchr(profile.skeleton,'.');skeleton_key=skeleton_key?skeleton_key+1:profile.skeleton;
    for(const BodySkeletonBasis& row:kBodySkeletonBases)if(Lower(row.skeleton)==Lower(skeleton_key))skeleton_bases.emplace(Lower(row.bone),&row);
    std::vector<std::array<double,4>> q0(target_refs.size(),{0,0,0,1});std::vector<const BodySkeletonBasis*> own_rows(target_refs.size(),nullptr);
    for(size_t t=0;t<target_raw;++t){auto found=source_names.find(target_refs[t].name);if(found==source_names.end())continue;Name name{};std::memcpy(&name,&target_refs[t].name,8);auto basis=skeleton_bases.find(Lower(Text(name)));
        if(basis==skeleton_bases.end())return false;std::copy(std::begin(basis->second->q0),std::end(basis->second->q0),q0[t].begin());own_rows[t]=basis->second;}
    if(!NpcPoseTransfer::Build(session->plan,std::move(source_refs),std::move(target_refs),q0,why,target_raw)){Logf("NPC_BODY plan refused reason=%s",why.c_str());return false;}
    if(!MatchArmsToHero(session->plan))return false;
    session->plan.foreign_keep.assign(session->plan.target.size(),0);
    for(const char* bone:{"root","pelvis"}){const int32_t b=PlanBone(session->plan.target,bone);if(b>=0)session->plan.foreign_keep[static_cast<size_t>(b)]=1;}
    {auto& plan=session->plan;plan.source_mode.assign(plan.target.size(),NpcPoseTransfer::kScaled);size_t modes[3]{};
        for(size_t t=0;t<plan.target.size();++t){const int32_t s=plan.target_to_source[t];if(s<0)continue;Name name;std::memcpy(&name,&plan.source[static_cast<size_t>(s)].name,8);const std::string text=Lower(Text(name));
            for(const char* bone:kSourceAnimationBones)if(text==Lower(bone))plan.source_mode[t]=NpcPoseTransfer::kAnimation;
            for(const char* bone:kSourceSkeletonBones)if(text==bone)plan.source_mode[t]=NpcPoseTransfer::kSkeleton;
            ++modes[plan.source_mode[t]];}
        // The boss's own modes and Skeleton lengths give its own montages back as the boss shows them.
        // Untracked bones keep the display length (kScaled, display length: the candidate 55 result).
        plan.own_mode.assign(plan.target.size(),NpcPoseTransfer::kScaled);plan.own_length.resize(plan.target.size());size_t own_modes[3]{},built_off=0,untracked=0;double built_off_cm=0;
        for(size_t t=0;t<plan.target.size();++t){const double display=NpcPoseTransfer::Length3(plan.target[t].ref.translation);plan.own_length[t]=display;
            const BodySkeletonBasis* row=t<own_rows.size()?own_rows[t]:nullptr;if(!row||plan.target_to_source[t]<0||row->mode>2||!std::isfinite(row->length)||row->length<0)continue;
            if(!row->animated){++untracked;continue;}
            plan.own_mode[t]=row->mode==0?NpcPoseTransfer::kAnimation:row->mode==1?NpcPoseTransfer::kSkeleton:NpcPoseTransfer::kScaled;plan.own_length[t]=row->length;++own_modes[plan.own_mode[t]];
            const double off=std::abs(row->length-display);if(off>0.01*std::max(display,1.0)){++built_off;built_off_cm=std::max(built_off_cm,off);}}
        Logf("NPC_BODY own-montage boss Skeleton modes scaled=%zu animation=%zu skeleton=%zu untracked=%zu; %zu bones of the display mesh built off that Skeleton (up to %.1f cm)",own_modes[0],own_modes[1],own_modes[2],untracked,built_off,built_off_cm);
        // Grabs scale by the body's height: the rest pelvis height over the hero's.
        const int32_t sp=PlanBone(plan.source,"pelvis"),tp=PlanBone(plan.target,"pelvis");
        const double hero=sp>=0?plan.source_ref_cs[static_cast<size_t>(sp)].translation[2]:0,npc=tp>=0?plan.target_ref_cs[static_cast<size_t>(tp)].translation[2]:0;
        skin_size_=hero>1&&npc>1&&std::isfinite(npc/hero)?std::clamp(npc/hero,kWeaponScaleMin,kWeaponScaleMax):1.0;
        Logf("NPC_BODY own-montage translation modes scaled=%zu animation=%zu skeleton=%zu; skin size %.3f (pelvis %.1f / hero %.1f cm)",modes[0],modes[1],modes[2],skin_size_,npc,hero);}
    {const auto size_bones=[](const std::vector<NpcPoseTransfer::Bone>& bones){NpcPoseTransfer::SizeBones b;b.head=PlanBone(bones,"head");
            for(size_t s=0;s<2;++s){const std::string x=s?"_r":"_l";b.upper[s]=PlanBone(bones,("upperarm"+x).c_str());b.lower[s]=PlanBone(bones,("lowerarm"+x).c_str());b.hand[s]=PlanBone(bones,("hand"+x).c_str());
                b.middle[s]=PlanBone(bones,("middle_01"+x).c_str());b.index[s]=PlanBone(bones,("index_01"+x).c_str());b.pinky[s]=PlanBone(bones,("pinky_01"+x).c_str());}return b;};
        NpcPoseTransfer::BodyScale size;const bool sized=NpcPoseTransfer::WeaponScale(session->plan,size_bones(session->plan.source),size_bones(session->plan.target),kWeaponScaleMin,kWeaponScaleMax,size);
        stature_ratio_=size.ratio[0];
        Logf("NPC_BODY weapon scale %s applied=%.3f raw=%.3f ratios(stature %.3f reach %.3f fist %.3f) npcCm(%.1f %.1f %.1f) heroCm(%.1f %.1f %.1f) clamp=%.1f..%.1f",sized?"matched":"unavailable",
            session->plan.weapon_scale,size.raw,size.ratio[0],size.ratio[1],size.ratio[2],size.target[0],size.target[1],size.target[2],size.source[0],size.source[1],size.source[2],kWeaponScaleMin,kWeaponScaleMax);}
    // The NPC post-process AnimBP runs after this pose (twist / cloth bones, and on many bosses a Control
    // Rig weapon or hand IK aimed at its ik_* or virtual bones). Keep those on the hands it now has, so
    // that IK has nothing left to pull: ik_* copies follow the FK bone they rest on, and each virtual
    // bone copies its target bone's slot in the CopyPose map (BodyMap) instead of holding the rest pose.
    {auto& plan=session->plan;std::vector<int32_t> hands;for(const char* h:{"hand_l","hand_r"}){const int32_t b=PlanBone(plan.target,h);if(b>=0)hands.push_back(b);}
        std::vector<int32_t> follow=NpcPoseTransfer::HandProxies(plan.target,plan.target_ref_cs,hands,kHandProxyCm);std::string list;size_t used=0;
        for(size_t t=0;t<follow.size();++t){if(follow[t]<0)continue;Name name;std::memcpy(&name,&plan.target[t].name,8);const std::string text=Text(name);
            if(t>=plan.target_raw||Lower(text).rfind("ik_",0)!=0){follow[t]=-1;continue;}++used;if(list.size()<240)list+=(list.empty()?"":",")+text;}
        const bool followed=NpcPoseTransfer::SetFollowers(plan,follow);
        session->virtual_source.assign(plan.target.size(),-1);size_t driven=0;std::string virtual_list;
        Object* skeleton=Class(profile.skeleton);const Array vbs=Property<Array>(skeleton,"VirtualBones");
        // FVirtualBone = {SourceBoneName, TargetBoneName, VirtualBoneName}; a row counts only if its virtual bone exists here under that source.
        if(skeleton&&vbs.num>0&&vbs.num<=256&&vbs.max>=vbs.num&&FaceReadable(vbs.data,static_cast<size_t>(vbs.num)*24))for(int32_t i=0;i<vbs.num;++i){
            uint64_t row[3];std::memcpy(row,static_cast<Object*>(vbs.data)+static_cast<size_t>(i)*24,24);
            for(size_t t=plan.target_raw;t<plan.target.size();++t){if(plan.target[t].name!=row[2])continue;const int32_t p=plan.target[t].parent;if(p<0||plan.target[static_cast<size_t>(p)].name!=row[0])break;
                for(size_t b=0;b<plan.target_raw;++b)if(plan.target[b].name==row[1]){const int32_t s=plan.target_to_source[b];if(s>=0){session->virtual_source[t]=s;++driven;Name vb;std::memcpy(&vb,&row[2],8);
                    if(virtual_list.size()<240)virtual_list+=(virtual_list.empty()?"":",")+Text(vb);}break;}
                break;}}
        Logf("NPC_BODY post-process inputs: ik followers=%zu set=%d (%s); virtual bones=%zu driven=%zu (%s)",used,followed?1:0,list.c_str(),plan.target.size()-plan.target_raw,driven,virtual_list.c_str());}
    Object* identities[]={session->source.object,session->source_asset.object,session->target_asset.object};for(size_t i=0;i<3;++i)reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166e900)(&session->generations[i*2],&identities[i]);
    const auto& built=session->plan;NpcPoseTransfer::HandContact request;
    request.source_hand_l=PlanBone(built.source,"hand_l");request.source_hand_r=PlanBone(built.source,"hand_r");
    request.source_palm_l=PlanBone(built.source,"middle_01_l");request.source_palm_r=PlanBone(built.source,"middle_01_r");
    request.upper=PlanBone(built.target,"upperarm_l");request.lower=PlanBone(built.target,"lowerarm_l");request.hand=PlanBone(built.target,"hand_l");request.hand_r=PlanBone(built.target,"hand_r");
    request.palm_l=PlanBone(built.target,"middle_01_l");request.palm_r=PlanBone(built.target,"middle_01_r");
    // Grip gate: the hero's left fist (weapon_l's rest place in hand_l) must sit on the weapon_r handle.
    request.source_weapon_r=PlanBone(built.source,"weapon_r");const int32_t source_weapon_l=PlanBone(built.source,"weapon_l");
    const bool gate=request.source_weapon_r>=0&&source_weapon_l>=0&&request.source_hand_l>=0&&
        NpcGripMath::Relative(built.source_ref_cs[static_cast<size_t>(source_weapon_l)],built.source_ref_cs[static_cast<size_t>(request.source_hand_l)],request.grip_l);
    // Same hand bases as the weapon adapter (InstallGripHook), so the left hand turns with the weapon.
    const bool bases=gate&&request.source_hand_r>=0&&request.hand>=0&&request.hand_r>=0&&
        GeometryForAnchor(built,request.source_hand_l,request.hand,true,false,request.basis_l)&&GeometryForAnchor(built,request.source_hand_r,request.hand_r,true,false,request.basis_r);
    const bool contact=bases&&NpcPoseTransfer::SetHandContact(session->plan,request);
    // The weapon adapter's right-hand turn at rest, on the NPC's own rest pose and on the matched one (0 when matched).
    double turn_degrees[2]{};
    if(contact)for(size_t k=0;k<2;++k){const auto& cs=k?built.target_rest_cs:built.target_ref_cs;double inverse[4],tmp[4],turn[4];NpcGripMath::Inverse(built.source_ref_cs[static_cast<size_t>(request.source_hand_r)].rotation,inverse);
        NpcGripMath::Mul(cs[static_cast<size_t>(request.hand_r)].rotation,request.basis_r,tmp);NpcGripMath::Mul(tmp,inverse,turn);turn_degrees[k]=NpcPoseTransfer::detail::Degrees(turn);}
    // A mount (kMountLoops) and, on a ridden one, the rider's pelvis and legs (every raw bone outside spine_01)
    // always follow the pose puppet; unmapped ones borrow source slots no target bone maps for the CopyPose.
    mount_.row=nullptr;
    for(const MountLoops& row:kMountLoops){if(std::strcmp(row.skeleton,profile.skeleton))continue;auto& plan=session->plan;
        const int32_t mount=PlanBone(plan.target,row.mount_root),upper=PlanBone(plan.target,"spine_01");
        if(mount<0||(row.ridden&&upper<0)){Logf("NPC_MOUNT %s: no %s / spine_01 on the display mesh, no puppet",profile.id,row.mount_root);break;}
        std::vector<uint8_t> always(plan.target.size(),0);size_t held=0;
        for(size_t t=1;t<plan.target_raw;++t){bool in_mount=false,in_upper=false;
            for(int32_t bone=static_cast<int32_t>(t);bone>=0;bone=plan.target[static_cast<size_t>(bone)].parent){in_mount=in_mount||bone==mount;in_upper=in_upper||bone==upper;}
            always[t]=in_mount||(row.ridden&&!in_upper)?1:0;held+=always[t];}
        size_t assigned=0,wanted=0;if(!NpcPoseTransfer::SetPuppet(plan,always,assigned,wanted)){Logf("NPC_MOUNT %s: puppet plan refused",profile.id);break;}
        mount_.row=&row;
        Logf("NPC_MOUNT %s mount=%s (%d bones) ridden=%d: %zu bones always follow the puppet; borrowed source slots %zu of %zu unmapped",
            profile.id,row.mount_root,row.bones,row.ridden,held,assigned,wanted);break;}
    NpcPoseTransfer::Prepare(session->plan,session->cache);
    const auto& k=session->plan.contact;
    Logf("NPC_BODY two-hand contact %s weaponFrame=1 refTurnDeg native=%.1f matched=%.1f fullCm=%.0f noneCm=%.0f gripGate=%d(weapon_r axis %.0f..%.0f cm, %.0f..%.0f deg)",contact?"enabled":"unavailable",turn_degrees[0],turn_degrees[1],
        k.full_cm,k.none_cm,gate,k.grip_full_cm,k.grip_none_cm,k.axis_full_deg,k.axis_none_deg);
    const int32_t source_head=PlanBone(built.source,"head"),target_head=PlanBone(built.target,"head");
    camera_lift_=source_head>=0&&target_head>=0?built.target_ref_cs[static_cast<size_t>(target_head)].translation[2]-built.source_ref_cs[static_cast<size_t>(source_head)].translation[2]:0;
    Logf("NPC_BODY prepared sourceRaw=%zu sourceRuntime=%zu targetRaw=%zu targetRuntime=%zu mapped=%zu parentMismatchKept=%zu sourceGeneration=%s target=%s sharedAssetWrites=0",
        source_raw,session->plan.source.size(),target_raw,session->plan.target.size(),session->plan.mapped,session->plan.parent_mismatch,Text(session->source_asset.object).c_str(),Text(target_asset).c_str());
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
    // The mount puppet's finished pose (the display ticks after it), while it is the display mesh.
    NpcPoseTransfer::PuppetPose puppet;const NpcPoseTransfer::PuppetPose* posed=nullptr;const Identity& id=pose_session_->puppet;
    if(id.object&&Same(id)&&SkelMesh(id.object)==pose_session_->target_asset.object&&SourceCurrentCS(id.object,puppet.cs,puppet.count)){
        puppet.serial=mount_serial_.load(std::memory_order_relaxed);puppet.now_ms=GetTickCount64();posed=&puppet;}
    if(!NpcPoseTransfer::Evaluate(pose_session_->plan,source,count,pose_session_->cache,why,foreign_weight_.load(std::memory_order_relaxed),foreign_own_.load(std::memory_order_relaxed),posed)){pose_session_->why=why;InterlockedExchange(&pose_session_->failed,1);return false;}
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
    if (method != ue::Base() + 0x3da9320 && method != ue::Base() + 0x5cc9a30) return false;
    void* graph = reinterpret_cast<void*(__fastcall*)()>(ue::Base() + 0x1267790)(); if (!graph) return false;
    reinterpret_cast<void(__fastcall*)(void*, const void*, uint32_t)>(ue::Base() + 0x1f27810)(graph, component + 0xde8, 1);
    reinterpret_cast<void(__fastcall*)(Object*, bool)>(method)(component, true);
    return !ue::At<void*>(component, 0xde8);
}
bool FaceNativeMatch() {
    static constexpr unsigned char pre[] = {0x40,0x53,0x55,0x56,0x48,0x83,0xec,0x40,0x48,0x8b,0xf1,0x48,0x8b,0xea,0x48,0x8b};
    static constexpr unsigned char evaluate[] = {0x4c,0x8b,0xdc,0x56,0x57,0x41,0x54,0x48,0x81,0xec,0xc0,0,0,0,0x48,0x8b};
    static constexpr unsigned char weak[] = {0x40,0x53,0x48,0x83,0xec,0x20,0x48,0x8b,0x12,0x48,0x8b,0xd9,0x48,0x85,0xd2,0x74};
    return ue::Base() && !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x35d7d30),pre,sizeof(pre)) &&
        !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x35bed60),evaluate,sizeof(evaluate)) &&
        !std::memcmp(reinterpret_cast<void*>(ue::Base()+0x166e900),weak,sizeof(weak));
}
bool FaceMap(FaceHook& hook, bool restore) {
    Object* node = hook.node; const Profile& profile = *hook.profile;
    auto refuse=[&](const char* state) { hook.map_state=state; return false; };
    if (restore && !hook.map_written) { hook.map_state="no-owned-map-writes"; return true; }
    if (profile.head_bones < 16 || profile.head_bones > 2048 || !profile.facial_count || hook.saved.size() != static_cast<size_t>(profile.head_bones) || !FaceReadable(node, 344)) return refuse("node-layout");
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
    std::array<bool,2048> facial{}; std::array<bool,2048> seen{}; std::array<Object*,2048> entries{};
    for (size_t i=0;i<profile.facial_count;++i) { if (profile.facial_bones[i]>=profile.head_bones) return refuse("profile-facial-index"); facial[profile.facial_bones[i]]=true; }
    size_t filtered=0;
    for (int32_t i=0;i<slots;++i) if (allocated[i/32] & (1u << (i%32))) {
        Object* entry = static_cast<Object*>(data)+static_cast<size_t>(i)*16;
        const int32_t key=ue::At<int32_t>(entry,0), value=ue::At<int32_t>(entry,4);
        if (key<0 || key>=expected || seen[key]) return refuse("map-duplicate-or-range"); seen[key]=true;
        // Runtime reference skeletons can append virtual bones. Keep them and
        // their source mapping intact; only the audited facial keys change.
        if (key>=profile.head_bones) continue;
        if (!facial[key]) { if (!restore && key<10 && value<0) return refuse("head-neck-source-missing"); continue; }
        entries[key]=entry; ++filtered;
    }
    if (filtered!=profile.facial_count || !std::all_of(seen.begin(),seen.begin()+expected,[](bool value){return value;})) return refuse("map-incomplete");
    // Validate every key/hash/bitmap before committing any facial write. A
    // failed initial installation must have no half-written map to unwind.
    if (!restore && !same_epoch) {
        std::fill(hook.saved.begin(),hook.saved.end(),std::numeric_limits<int32_t>::min()); hook.map_data=data; hook.map_slots=slots;
        hook.source_mesh=source_mesh; hook.target_mesh=target_mesh;
    }
    for (size_t key=0;key<static_cast<size_t>(profile.head_bones);++key) if (Object* entry=entries[key]) {
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
    // virtuals: the rows written here - virtual bones, and unmapped bones on a borrowed source slot (Plan::extra_source).
    std::array<bool,2048> seen{};std::array<Object*,2048> virtuals{};const bool extras=plan.extra_source.size()==count;
    for(int32_t i=0;i<slots;++i)if(allocated[i/32]&(1u<<(i%32))){Object* row=static_cast<Object*>(data)+static_cast<size_t>(i)*16;const int32_t key=ue::At<int32_t>(row,0),value=ue::At<int32_t>(row,4);
        if(key<0||static_cast<size_t>(key)>=count||seen[static_cast<size_t>(key)])return false;seen[static_cast<size_t>(key)]=true;
        if(static_cast<size_t>(key)>=plan.target_raw||(extras&&plan.extra_source[static_cast<size_t>(key)]>=0))virtuals[static_cast<size_t>(key)]=row;
        else if(!restore&&value!=plan.target_to_source[static_cast<size_t>(key)])return false;}
    if(!std::all_of(seen.begin(),seen.begin()+count,[](bool b){return b;}))return false;
    if(!restore&&!same){std::fill(hook.body_saved.begin(),hook.body_saved.end(),std::numeric_limits<int32_t>::min());hook.map_data=data;hook.map_slots=slots;hook.source_mesh=source;hook.target_mesh=target;}
    // A virtual bone copies its target bone's source slot (PreparePoseSession), else -1 keeps its reference pose;
    // a mount bone reads its borrowed slot.
    bool wrote=false,driven=false;for(size_t i=0;i<count;++i)if(virtuals[i]){int32_t& value=ue::At<int32_t>(virtuals[i],4);
        const int32_t want=i<plan.target_raw?plan.extra_source[i]:pose_session_->virtual_source.size()==count?pose_session_->virtual_source[i]:-1;if(want>=0)driven=true;
        if(restore){if(value==want&&hook.body_saved[i]!=std::numeric_limits<int32_t>::min())value=hook.body_saved[i];}
        else{if(value!=want||hook.body_saved[i]==std::numeric_limits<int32_t>::min())hook.body_saved[i]=value;value=want;wrote=true;}}
    hook.map_written=!restore&&wrote;hook.map_state=restore?"body-virtual-map-restored":driven?"body-raw-map-verified-virtual-driven":"body-raw-map-verified-virtual-ref";return true;
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
    if (!hook) { reinterpret_cast<void(__fastcall*)(Object*,Object*)>(ue::Base()+0x35d7d30)(node,instance); return; }
    if (GetCurrentThreadId()!=face_thread_) { InterlockedExchange(&hook->failed,1); return; }
    if(hook->source_input)ue::At<uint64_t>(node,0x10)=hook->source_input;
    // PreUpdate is the engine's GT stage before parallel graph evaluation.
    // Reassert before native caching; then filter every rebuilt map afterward.
    ue::At<uint8_t>(node,0x18) &= static_cast<uint8_t>(~2u);
    ue::At<uint8_t>(node,0x19) = 0;
    reinterpret_cast<void(__fastcall*)(Object*,Object*)>(ue::Base()+0x35d7d30)(node,instance);
    if(hook->source_input&&(ue::At<uint64_t>(node,0x24)!=hook->source_input||ue::At<uint64_t>(node,0x34)!=hook->source_asset_input)){InterlockedExchange(&hook->failed,1);return;}
    const LONGLONG cache_started=hook->body?PerfStart():0;const bool mapped=hook->body?RewriteBodyCache(*hook):FaceMap(*hook,false);if(hook->body)PerfEnd(PerfKind::BodyCache,cache_started);
    if(!mapped){InterlockedExchange(&hook->failed,1);return;}
    if(hook->body){if(++hook->epochs==1)Logf("NPC_BODY CopyPose private owned cache retarget active mapped=%zu targetRaw=%zu targetRuntime=%zu nativeSourceUnchanged=1",pose_session_->plan.mapped,pose_session_->plan.target_raw,pose_session_->plan.target.size());return;}
    if (++hook->epochs==1) Logf("NPC_FACE_NEUTRAL source=%s mappedFacial=0 meshRefFacial=%zu headNeckCopied=10 runtimeBones=%d rawBones=%d curves=0 attributes=0 private-node=1",hook->profile->id,hook->profile->facial_count,hook->runtime_bones,hook->profile->head_bones);
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
    if (original!=reinterpret_cast<uintptr_t*>(ue::Base()+0xae44180) || original[3]!=ue::Base()+0x35bed60 ||
        original[8]!=ue::Base()+0x35d7d30 || original[14]!=ue::Base()+0x35a7fb0 ||
        !FaceReadable(original-1,21*8) || (ue::At<uint8_t>(node,0x1a)&1)) return false;
    auto hook=std::make_unique<FaceHook>(); hook->component=Id(component); hook->instance=Id(instance);hook->body=body;
    int32_t weak[2]{};
    reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166e900)(weak,&instance);
    hook->instance_index=weak[0]; hook->instance_serial=weak[1]; hook->node=node; hook->original=original; hook->profile=&profile;
    if (!FaceRawInstance(*hook)) return false;
    reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166e900)(weak,&component);
    hook->component_index=weak[0]; hook->component_serial=weak[1];
    Call bone_count(component,"GetNumBones");
    if (!bone_count.Slot("ReturnValue",sizeof(int32_t)) || !bone_count.Invoke()) return false;
    hook->runtime_bones=bone_count.Get<int32_t>("ReturnValue");
    if (hook->runtime_bones<(body?profile.bones:profile.head_bones) || hook->runtime_bones>2048) return false;
    if(body){if(!pose_session_||static_cast<size_t>(hook->runtime_bones)!=pose_session_->plan.target.size())return false;hook->body_saved.assign(static_cast<size_t>(hook->runtime_bones),std::numeric_limits<int32_t>::min());}
    if(source_driver_mode_){Object* source=body?primary_.object:display_.object;if(!ue::Alive(source))return false;hook->old_source_input=ue::At<uint64_t>(node,0x10);
        reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166e900)(weak,&source);std::memcpy(&hook->source_input,weak,8);
        Object* asset=SkelMesh(source);reinterpret_cast<void(__fastcall*)(int32_t*,Object* const*)>(ue::Base()+0x166e900)(weak,&asset);std::memcpy(&hook->source_asset_input,weak,8);}
    hook->old_curve=ue::At<uint8_t>(node,0x18)&2; hook->old_attributes=ue::At<uint8_t>(node,0x19);
    if (hook->old_attributes>1) return false;
    hook->saved.assign(static_cast<size_t>(profile.head_bones),std::numeric_limits<int32_t>::min()); hook->table=std::make_unique<uintptr_t[]>(21);
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
    for(size_t side=0;side<2;++side){const auto& bones=side?plan.target:plan.source;const auto& cs=side?plan.target_rest_cs:plan.source_ref_cs;const int32_t anchor=side?ti:si;Transform p[4];
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
int32_t SourceBoneNamed(const NpcPoseTransfer::Plan& plan,Name name){uint64_t key=0;std::memcpy(&key,&name,8);return NpcPoseTransfer::BoneNamed(plan.source,key);}
bool AdjustGripSocket(GripHook& hook,Object* self,Name name,uint32_t space,Transform& out,const Transform& component_space,const Transform& native_mount) {
    if(space>2||!pose_session_)return true;if(!PoseIdentityValid()||!NpcGripMath::Valid(component_space))return false;bool managed=false;int32_t named_bone=-1;
    for(size_t i=0;i<hook.count;++i)if(hook.sockets[i].name==name){managed=true;named_bone=hook.sockets[i].bone;break;}if(!managed)return true;
    // The socket lookup knows only sockets: for a bare bone name (hand weapons hang on WEAPON_R /
    // weapon_l, gloves on lowerarm_l/r) it gives -1, and those weapons used to stay at the hero's.
    Transform local;int32_t bone=-1;OriginalSocketInfo()(self,name,&local,&bone);if(bone<0)bone=named_bone;
    const auto& plan=pose_session_->plan;if(bone<0||static_cast<size_t>(bone)>=plan.source.size())return true;
    int32_t anchor=bone;bool hand_chain=false;const int32_t start=hook.anchors[static_cast<size_t>(bone)].proxy>=0?hook.anchors[static_cast<size_t>(bone)].proxy:bone;
    for(int32_t b=start;b>=0;b=plan.source[static_cast<size_t>(b)].parent)if(hook.anchors[static_cast<size_t>(b)].hand){anchor=b;hand_chain=true;break;}
    if(!hand_chain)while(anchor>=0&&hook.anchors[static_cast<size_t>(anchor)].target<0)anchor=plan.source[static_cast<size_t>(anchor)].parent;
    if(anchor<0)return true;const GripAnchor& row=hook.anchors[static_cast<size_t>(anchor)];
    if(row.forearm&&!RigidForearmEnabled(hook,anchor))return true;
    const Transform* source=nullptr;size_t count=0;if(!SourceCurrentCS(self,source,count))return false;
    bool ok=false;AcquireSRWLockExclusive(&pose_lock_);
    __try {if(UpdatePoseCache(source,count)){
        Transform result;
        ok=NpcGripMath::MapSourceMountPose(source,pose_session_->cache.target_cs.data(),static_cast<size_t>(anchor),static_cast<size_t>(row.target),row.palm,row.target_palm,native_mount,row.basis,component_space,result,
            pose_session_->plan.weapon_scale);
        if(ok){out=result;InterlockedIncrement(&hook.queries);}} }
    __finally {ReleaseSRWLockExclusive(&pose_lock_);}return ok;
}
bool DisplayRouted(const GripHook& hook,Name name) {
    if(!hook.display.object)return false;uint64_t key=0;std::memcpy(&key,&name,8);
    if(std::binary_search(hook.display_only.begin(),hook.display_only.end(),key))return true;
    return GetTickCount64()<skill_route_until_.load(std::memory_order_relaxed)&&std::binary_search(hook.shared.begin(),hook.shared.end(),key);
}
// The display's own socket, in the space asked of the hero mesh it hangs on.
bool DisplaySocketTransform(Object* self,Object* display,Name name,uint32_t space,Transform& out) {
    Transform world,component,local,in_actor;OriginalSocketTransform()(display,&world,name,0);
    if(space==0){out=world;return NpcGripMath::FiniteMount(out);}
    OriginalSocketTransform()(self,&component,Name{},0);if(!NpcGripMath::RelativeNativeSafe(world,component,local))return false;
    if(space==2){out=local;return true;}
    OriginalSocketTransform()(self,&in_actor,Name{},1);return NpcGripMath::ComposeMount(local,in_actor,out);
}
void RouteToDisplay(Object* self,Transform* out,Name name,uint32_t space) {
    AcquireSRWLockShared(&grip_lock_);
    __try {__try {Transform routed;if(grip_hook_&&grip_hook_->component.object==self&&grip_hook_->display.object&&DisplaySocketTransform(self,grip_hook_->display.object,name,space,routed)){*out=routed;InterlockedIncrement(&grip_hook_->display_queries);}}
        __except(EXCEPTION_EXECUTE_HANDLER){}}
    __finally {ReleaseSRWLockShared(&grip_lock_);}
}
Transform* __fastcall GripSocketTransform(Object* self,Transform* out,Name name,uint32_t space) {
    OriginalSocketTransform()(self,out,name,space);if(space>2)return out;
    bool managed=false,display=false;AcquireSRWLockShared(&grip_lock_);
    if(grip_hook_&&grip_hook_->component.object==self){for(size_t i=0;i<grip_hook_->count;++i)if(grip_hook_->sockets[i].name==name){managed=true;break;}if(!managed)display=DisplayRouted(*grip_hook_,name);}
    ReleaseSRWLockShared(&grip_lock_);if(display){RouteToDisplay(self,out,name,space);return out;}if(!managed)return out;
    const LONGLONG started=PerfStart();Transform component_space,native_mount=*out;if(space!=2){OriginalSocketTransform()(self,&native_mount,name,2);OriginalSocketTransform()(self,&component_space,Name{},space);}
    AcquireSRWLockShared(&grip_lock_);
    __try {__try {if(grip_hook_&&grip_hook_->component.object==self&&!AdjustGripSocket(*grip_hook_,self,name,space,*out,component_space,native_mount))InterlockedIncrement(&grip_hook_->failures);}
        __except(EXCEPTION_EXECUTE_HANDLER){if(grip_hook_&&grip_hook_->component.object==self)InterlockedIncrement(&grip_hook_->failures);}}
    __finally {ReleaseSRWLockShared(&grip_lock_);}PerfEnd(PerfKind::Grip,started);return out;
}
bool InstallGripHook(Object* component,const Profile& profile) {
    if(grip_hook_||!pose_session_||!FaceQuiesce(component))return false;
    auto hook=std::make_unique<GripHook>();hook->component=Id(component);const auto& plan=pose_session_->plan;hook->anchors.resize(plan.source.size());
    double torso_root[4],torso_cs[4],inv_root[4],tmp[4];if(!GeometryForAnchor(plan,0,0,false,false,torso_root))return false;NpcGripMath::Inverse(plan.source_ref_cs[0].rotation,inv_root);NpcGripMath::Mul(plan.target_rest_cs[0].rotation,torso_root,tmp);NpcGripMath::Mul(tmp,inv_root,torso_cs);
    for(size_t si=0;si<plan.source.size();++si){GripAnchor& a=hook->anchors[si];a.target=plan.source_to_target[si];if(a.target<0)continue;Name name;std::memcpy(&name,&plan.source[si].name,8);
        a.hand=ue::NameIs(name,"hand_l")||ue::NameIs(name,"hand_r");a.forearm=ue::NameIs(name,"lowerarm_l")||ue::NameIs(name,"lowerarm_r");
        if(a.hand||a.forearm){if(!GeometryForAnchor(plan,static_cast<int32_t>(si),a.target,a.hand,a.forearm,a.basis))return false;}else{double qi[4],qt[4];NpcGripMath::Inverse(plan.target_rest_cs[static_cast<size_t>(a.target)].rotation,qi);NpcGripMath::Mul(qi,torso_cs,qt);NpcGripMath::Mul(qt,plan.source_ref_cs[si].rotation,a.basis);}
        if(a.hand){const char* palm=ue::NameIs(name,"hand_r")?"middle_01_r":"middle_01_l";a.palm=PlanBone(plan.source,palm);a.target_palm=PlanBone(plan.target,palm);if(a.palm<0||a.target_palm<0)return false;}}
    {std::vector<int32_t> hands;for(const char* h:{"hand_l","hand_r"}){const int32_t b=PlanBone(plan.source,h);if(b>=0)hands.push_back(b);}
        const std::vector<int32_t> proxies=NpcPoseTransfer::HandProxies(plan.source,plan.source_ref_cs,hands,kHandProxyCm);std::string list;size_t proxied=0;
        for(size_t si=0;si<proxies.size();++si){if(proxies[si]<0)continue;Name name;std::memcpy(&name,&plan.source[si].name,8);const std::string text=Text(name);
            // Only IK copies, or bones the NPC lacks: a mapped twist bone resting at the wrist keeps its own frame.
            if(Lower(text).rfind("ik_",0)!=0&&plan.source_to_target[si]>=0)continue;
            hook->anchors[si].proxy=proxies[si];++proxied;if(list.size()<240)list+=(list.empty()?"":",")+text;}
        Logf("NPC_GRIP hand proxies=%zu (%s) restToleranceCm=%.1f",proxied,list.c_str(),kHandProxyCm);}
    size_t bare=0;
    for(const char* route:kWeaponAttachmentNames){if(hook->count>=hook->sockets.size())return false;const std::wstring wide(route,route+std::strlen(route));const Name name=ue::MakeName(wide.c_str());
        if(!SocketExists(component,name)){Logf("NPC_GRIP source mesh lacks optional mount=%s; route left native",route);continue;}
        GripSocket& row=hook->sockets[hook->count++];row.name=name;row.bone=SourceBoneNamed(plan,name);if(row.bone>=0)++bare;}
    for(const ForearmBasis& b:kForearmBases)if(Lower(b.profile)==Lower(profile.id)){const size_t side=std::strstr(b.bone,"_r")?0:1;hook->forearms[side].basis=&b;const std::wstring wide(b.bone,b.bone+std::strlen(b.bone));hook->forearms[side].bone=ue::MakeName(wide.c_str());}
    if(!hook->count)return false;uintptr_t* original=ue::At<uintptr_t*>(component,0);size_t count=0;
    if(!FaceReadable(original-1,(kSocketTransformSlot+2)*8)||original[kSocketTransformSlot]!=ue::Base()+kSocketTransformRva)return false;
    while(count<1024&&original[count]>=ue::Base()+0x1000&&original[count]<ue::Base()+0xa600000)++count;
    if(count<=kSocketTransformSlot||!FaceReadable(original-1,(count+1)*8))return false;
    auto* table=static_cast<uintptr_t*>(VirtualAlloc(nullptr,(count+1)*8,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE));if(!table)return false;
    std::memcpy(table,original-1,(count+1)*8);table[1+kSocketTransformSlot]=reinterpret_cast<uintptr_t>(&GripSocketTransform);hook->original=original;hook->table=table+1;retired_grip_tables_.push_back(table);
    AcquireSRWLockExclusive(&grip_lock_);grip_hook_=std::move(hook);const bool installed=InterlockedCompareExchangePointer(reinterpret_cast<void* volatile*>(component),grip_hook_->table,original)==original;if(!installed)grip_hook_.reset();ReleaseSRWLockExclusive(&grip_lock_);
    if(installed){RefreshForearmBindings();Logf("NPC_GRIP source driver current-pose adapter installed routes=%zu bareBoneRoutes=%zu weaponScale=%.3f nativeChildPreserved=1 targetAssetsMutated=0",grip_hook_->count,bare,plan.weapon_scale);}return installed;
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
    Logf("NPC_GRIP restored current-pose adapter queries=%ld failures=%ld routes=%zu displayQueries=%ld vtable=%s",hook.queries,hook.failures,hook.count,hook.display_queries,vtable_result);
    grip_hook_.reset(); ReleaseSRWLockExclusive(&grip_lock_); weapon_children_.clear(); weapon_log_due_=0; return true;
}
// Game thread, after InstallGripHook. Bones and sockets of both meshes (GetAllSocketNames). Names the
// hero lacks always go to the display; shared ones only during NPC skills, never the camera, IK,
// interaction or root names (the game aims and plants feet with those).
std::vector<uint64_t> SocketKeys(Object* mesh) {
    std::vector<uint64_t> keys;Call all(mesh,"GetAllSocketNames");if(!all.Invoke())return keys;
    const Array names=all.Get<Array>("ReturnValue");
    if(names.data&&names.num>0&&names.num<=8192)for(int32_t i=0;i<names.num;++i){uint64_t key=0;std::memcpy(&key,static_cast<Name*>(names.data)+i,8);if(key)keys.push_back(key);}
    ue::Free(names.data);std::sort(keys.begin(),keys.end());keys.erase(std::unique(keys.begin(),keys.end()),keys.end());return keys;
}
void BuildDisplayRoutes(Object* hero,Object* display) {
    const std::vector<uint64_t> hero_keys=SocketKeys(hero),display_keys=SocketKeys(display);
    std::vector<uint64_t> only,shared;std::string sample;
    for(uint64_t key:display_keys){Name name;std::memcpy(&name,&key,8);const std::string text=Lower(Text(name));
        if(!std::binary_search(hero_keys.begin(),hero_keys.end(),key)){only.push_back(key);if(sample.size()<200&&text.find("bone")==std::string::npos)sample+=(sample.empty()?"":",")+Text(name);continue;}
        if(text.rfind("ik_",0)==0||text.find("cam")!=std::string::npos||text=="root"||text=="interaction"||text=="center_of_mass")continue;
        shared.push_back(key);}
    AcquireSRWLockExclusive(&grip_lock_);
    if(grip_hook_){grip_hook_->display=Id(display);grip_hook_->display_only=std::move(only);grip_hook_->shared=std::move(shared);
        Logf("NPC_GRIP display routes heroNames=%zu displayNames=%zu displayOnly=%zu sharedDuringSkills=%zu (e.g. %s)",hero_keys.size(),display_keys.size(),grip_hook_->display_only.size(),grip_hook_->shared.size(),sample.empty()?"-":sample.c_str());}
    ReleaseSRWLockExclusive(&grip_lock_);
}
// Game thread. A switched or Shaqi weapon can hang on a mount the generated list lacks (Hun's
// ik_weapon_r copies, knife sockets): every socket another actor is attached to on the source
// mesh becomes a route. The pawn's own parts (collision boxes, effects) stay native. Weapons
// attached anywhere on the pawn are logged once per change, with their distance to the NPC hands.
void RefreshWeaponRoutes() {
    if(!grip_hook_||!Same(primary_)||!Same(pawn_))return;
    std::vector<std::pair<Object*,uint64_t>> seen;
    for(Object* parent:Components(pawn_.object,Class("/Script/Engine.SceneComponent"))){
        const Array children=Property<Array>(parent,"AttachChildren");if(children.num<0||children.num>512||children.max<children.num||(children.num&&!children.data))continue;
        for(int32_t i=0;i<children.num;++i){Object* child=static_cast<Object**>(children.data)[i];if(!ue::Alive(child))continue;
            Call owner(child,"GetOwner");if(!owner.Invoke())continue;Object* actor=owner.Get<Object*>("ReturnValue");if(!ue::Alive(actor)||actor==pawn_.object)continue;
            const Name socket=Property<Name>(child,"AttachSocketName");uint64_t key=0;std::memcpy(&key,&socket,8);seen.emplace_back(child,key);
            if(parent!=primary_.object||socket.None())continue;
            bool managed=false;AcquireSRWLockShared(&grip_lock_);for(size_t k=0;grip_hook_&&k<grip_hook_->count;++k)if(grip_hook_->sockets[k].name==socket){managed=true;break;}ReleaseSRWLockShared(&grip_lock_);
            if(managed||!SocketExists(primary_.object,socket))continue;
            size_t routes=0;AcquireSRWLockExclusive(&grip_lock_);const bool added=grip_hook_&&grip_hook_->count<grip_hook_->sockets.size();
            if(added){GripSocket& row=grip_hook_->sockets[grip_hook_->count];row.name=socket;row.bone=pose_session_?SourceBoneNamed(pose_session_->plan,socket):-1;++grip_hook_->count;}
            routes=grip_hook_?grip_hook_->count:0;ReleaseSRWLockExclusive(&grip_lock_);
            Logf("NPC_GRIP route discovered socket=%s owner=%s class=%s added=%d routes=%zu",Text(socket).c_str(),Text(actor).c_str(),Text(ue::ClassOf(actor)).c_str(),added?1:0,routes);}}
    std::sort(seen.begin(),seen.end());
    if(seen!=weapon_children_){weapon_children_=std::move(seen);weapon_log_due_=GetTickCount64()+500;return;}
    if(!weapon_log_due_||GetTickCount64()<weapon_log_due_)return;weapon_log_due_=0;
    Vector hand[2]{};bool have[2]{};const wchar_t* hand_names[2]={L"hand_r",L"hand_l"};
    if(Same(display_))for(size_t h=0;h<2;++h){Call at(display_.object,"GetSocketLocation");if(at.Set("InSocketName",ue::MakeName(hand_names[h])).Invoke()){hand[h]=at.Get<Vector>("ReturnValue");have[h]=true;}}
    for(const auto& [child,key]:weapon_children_){if(!ue::Alive(child))continue;Name socket;std::memcpy(&socket,&key,8);
        Call owner(child,"GetOwner"),parent(child,"GetAttachParent"),where(child,"K2_GetComponentLocation"),size(child,"K2_GetComponentScale");
        if(!owner.Invoke()||!parent.Invoke()||!where.Invoke()||!size.Invoke())continue;const Vector at=where.Get<Vector>("ReturnValue"),scale=size.Get<Vector>("ReturnValue");Object* attached=parent.Get<Object*>("ReturnValue");
        double nearest=-1;for(size_t h=0;h<2;++h)if(have[h]){const double d=std::sqrt((at.x-hand[h].x)*(at.x-hand[h].x)+(at.y-hand[h].y)*(at.y-hand[h].y)+(at.z-hand[h].z)*(at.z-hand[h].z));if(nearest<0||d<nearest)nearest=d;}
        bool managed=false;int32_t bare=-1;AcquireSRWLockShared(&grip_lock_);for(size_t k=0;grip_hook_&&k<grip_hook_->count;++k)if(grip_hook_->sockets[k].name==socket){managed=true;bare=grip_hook_->sockets[k].bone;break;}ReleaseSRWLockShared(&grip_lock_);
        const char* role=attached==primary_.object?"hero-mesh":attached==display_.object?"npc-display":"other";
        // Melee hits are sweeps of the weapon actor's own shapes ("Attack Collider", read by the
        // BulletData's SweepTrace); their world scale is the attack reach the NPC weapon really has.
        std::string shapes;for(Object* shape:Components(owner.Get<Object*>("ReturnValue"),Class("/Script/Engine.ShapeComponent"))){
            Call shape_scale(shape,"K2_GetComponentScale");if(!shape_scale.Invoke()||shapes.size()>200)continue;char item[96]{};
            _snprintf_s(item,sizeof(item),_TRUNCATE,"%s%s:%.3f",shapes.empty()?"":",",Text(shape).c_str(),shape_scale.Get<Vector>("ReturnValue").x);shapes+=item;}
        Logf("NPC_GRIP weapon child=%s owner=%s parent=%s(%s) socket=%s managed=%d bone=%d nearestNpcHandCm=%.1f worldScale=%.3f hitShapes=%s",Text(child).c_str(),Text(owner.Get<Object*>("ReturnValue")).c_str(),
            Text(attached).c_str(),role,Text(socket).c_str(),managed&&attached==primary_.object?1:0,bare,nearest,scale.x,shapes.empty()?"-":shapes.c_str());}
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
Object* LoadClassPackage(const std::string& package);
Object* LoadDonor(const Profile& profile) { return LoadClassPackage(profile.donor); }
// A blueprint class by package path ("/PBZCombatAsset/.../GA_X" -> GA_X_C): found if loaded, else loaded blocking.
// A copy the collector has marked for purge is not "loaded" (FindObject skips it): the engine's own
// load brings the class back, and only a live class is returned.
Object* LoadClassPackage(const std::string& package) {
    const std::string leaf = package.substr(package.find_last_of('/') + 1) + "_C";
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
    return ue::Alive(cls) ? cls : nullptr;
}
bool SourceOwner(Object* object, Object* cls, Object* cdo) {
    for (size_t guard = 0; object && guard < 8; ++guard, object = ue::OuterOf(object)) if (object == cls || object == cdo) return true;
    return false;
}
// How far up the donor's blueprint chain the template's owner is (0 = the donor or its CDO), or -1.
// Boss donors inherit body parts from a parent blueprint; a child's own override is closer and wins.
int32_t SourceDepth(Object* object, Object* cls, Object* cdo) {
    for (size_t guard = 0; object && guard < 8; ++guard, object = ue::OuterOf(object)) {
        if (object == cdo) return 0;
        int32_t depth = 0;
        for (Object* type = cls; type && depth < 8; type = ue::At<Object*>(type, ue::layout::kSuper), ++depth) if (object == type) return depth;
    }
    return -1;
}
bool LoadParts(Object* cls, Object* cdo, const Profile& profile, std::vector<Part>& parts) {
    for (size_t i = 0; i < profile.part_count; ++i) {
        Object* found = nullptr; int32_t best = std::numeric_limits<int32_t>::max();
        ue::ForEachObject([&](Object* object) {
            if (!ue::IsTemplate(object) || !ue::Alive(object) || !ue::NameIs(ue::NameOf(object), profile.parts[i])) return true;
            const int32_t depth = SourceDepth(object, cls, cdo);
            if (depth >= 0 && depth < best) { found = object; best = depth; }
            return depth != 0;
        });
        if (found && best > 0) Logf("NPC_RETARGET part=%s inherited from blueprint parent depth=%d", profile.parts[i], best);
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
    if (!profile.head) return true; // Single-mesh NPC: no separate RigLogic head to isolate.
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
// Cutscene donors can ship cloth templates disabled or suspended (the sequence drives them).
// A worn part always simulates: enable, resume, and start from the attached pose.
void EnableCloth(const Part& part) {
    bool template_enabled = true, template_suspended = false;
    const bool read = Bool(part.source, "bEnableSimulation", template_enabled) && Bool(part.source, "bSuspendSimulation", template_suspended);
    const bool set = Call(part.created, "SetEnableSimulation").Set("bEnable", true).Invoke();
    Call suspended(part.created, "IsSimulationSuspended");
    const bool was_suspended = suspended.Invoke() && suspended.Get<bool>("ReturnValue");
    const bool resumed = !was_suspended || Call(part.created, "ResumeSimulation").Invoke();
    const bool reset = Call(part.created, "ForceNextUpdateTeleportAndReset").Invoke();
    Call enabled(part.created, "IsSimulationEnabled");
    const bool simulating = enabled.Invoke() && enabled.Get<bool>("ReturnValue");
    Logf("NPC_CLOTH part=%s template(read=%d enabled=%d suspended=%d) set=%d wasSuspended=%d resumed=%d reset=%d simulating=%d",
        part.variable.c_str(), read ? 1 : 0, template_enabled ? 1 : 0, template_suspended ? 1 : 0, set ? 1 : 0, was_suspended ? 1 : 0, resumed ? 1 : 0, reset ? 1 : 0, simulating ? 1 : 0);
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
    if (groom) {
        Call scale(dest, "SetBoundsScale");
        if (PutBool(dest, "bUseAttachParentBound", false) && scale.Set("NewBoundsScale", kHairBoundsScale).Invoke())
            Logf("NPC_RETARGET hair bounds component=%s scale=%.1f ownBounds=1", Text(dest).c_str(), kHairBoundsScale);
        else Logf("NPC_RETARGET hair bounds unchanged component=%s (setter unavailable)", Text(dest).c_str());
    }
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
            // Only a RigLogic head gets the hidden-until-isolated face path; a plain head mesh is an ordinary part.
            Object* parent = context.mesh; Transform relative = part.relative; part.head_branch=part.variable=="meshhead"&&profile.head;
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
            if (ue::IsA(part.created, Class("/Script/ChaosClothAssetEngine.ChaosClothComponent"))) EnableCloth(part);
            if (part.variable=="meshhead" && profile.head && !InstallFaceHook(part.created,profile)) return Fail(L"自有 Head 的面部 CopyPose 隔离未能通过原生布局 / 私有映射校验");
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
            if (!visible || hidden) authored_.push_back({Id(part.created),visible,hidden});
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
// UI widget components are mesh components too: the lock-on indicator (TargetingComponent +0x1c0, made at
// BeginPlay on the hero and shown with SetVisibility only) must keep bHiddenInGame clear, or no lock is drawn.
bool IsAppearancePart(Object* component) {
    const char* keep[] = {"weapon", "knife", "knifescab", "sword", "blade", "switchkey", "scabbard", "scabard", "shield"};
    if (ue::NameContains(ue::NameOf(component), keep, std::size(keep))) return false;
    if (Object* widget = Class("/Script/UMG.WidgetComponent"); widget && ue::IsA(component, widget)) return false;
    return ue::IsA(component, Class("/Script/Engine.MeshComponent")) || ue::IsA(component, Class("/Script/HairStrandsCore.GroomComponent"));
}
// The hero's small dagger and its sheath on the back (pawn components Knife / KnifeScab on
// Pt_Knife_Back_Socket / Pt_KnifeScabbard_Back_Socket): part of Hun's look, not a weapon actor.
bool IsBackKnife(Object* component) {
    const std::string name = Lower(Text(component));
    return (name == "knife" || name == "knifescab") && ue::IsA(component, Class("/Script/Engine.MeshComponent"));
}
bool HideOriginalParts(const Context& context) {
    for (Object* component : Components(context.pawn, Class("/Script/Engine.SceneComponent"))) {
        if (component == context.mesh || component == backup_.object || (!IsAppearancePart(component) && !IsBackKnife(component))) continue;
        bool visible = false, hidden = false;
        if (!VisibilityOf(component, visible, hidden)) return false;
        Visibility saved{Id(component),visible,hidden};
        if(visible&&!hidden&&Bool(component,"CastShadow",saved.cast_shadow)&&Bool(component,"bCastHiddenShadow",saved.hidden_shadow))saved.shadows_owned=true;
        hidden_.push_back(saved);
        if(saved.shadows_owned&&(!PutBool(component,"bCastHiddenShadow",false)||!Call(component,"SetCastShadow").Set("NewCastShadow",false).Invoke()))return false;
        if (!SetVisibleOwned(component,false,true,hidden_.back().visible_written,hidden_.back().hidden_written)) return false;
        if (IsBackKnife(component)) Logf("NPC_RETARGET back dagger hidden component=%s socket=%s wasVisible=%d", Text(component).c_str(), Text(Property<Name>(component, "AttachSocketName")).c_str(), visible && !hidden ? 1 : 0);
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
            _snwprintf_s(detail, _countof(detail), _TRUNCATE, L"当前附件 %S 的挂点 %S 尚未适配；未替换主网格", Text(child).c_str(), Text(socket).c_str());
            failure_text_ = detail; Status(L"%s", detail);
            Logf("NPC_RETARGET missing required attachment socket=%s child=%s", Text(socket).c_str(), Text(child).c_str()); return false;
        }
    }
    return true;
}
bool SetCameraLift(Object* camera,double lift) {
    const Name name=ue::MakeName(kCameraAdditive);if(name.None())return false;
    return Call(camera,"SetAdditiveParameter_TargetOffset").Set("AdditiveName",name).Set("TargetOffset",Vector{0,0,lift}).Set("BlendTime",0.4f).Invoke();
}
void ApplyCameraLift(Object* pawn) {
    camera_owned_=false;camera_={};camera_probe_at_=0;
    if(std::abs(camera_lift_)<kCameraLiftMin){Logf("NPC_CAMERA headDeltaCm=%.1f under %.0f; camera unchanged",camera_lift_,kCameraLiftMin);return;}
    const double lift=(std::max)(kCameraLiftLow,(std::min)(camera_lift_,kCameraLiftHigh));
    Object* atpc=Class("/Script/AdvancedThirdPersonCamera.ATPCCameraComponent"),*camera=nullptr;
    if(atpc){Call get(pawn,"GetComponentByClass");if(get.Set("ComponentClass",atpc).Invoke())camera=get.Get<Object*>("ReturnValue");}
    if(!ue::Alive(camera)){Object* nb=Property<Object*>(pawn,"NBCamera");
        Logf("NPC_CAMERA no ATPC camera on the pawn (NBCamera=%s class=%s); height adaptation skipped",Text(nb).c_str(),Text(ue::Alive(nb)?ue::ClassOf(nb):nullptr).c_str());return;}
    if(!SetCameraLift(camera,lift)){Logf("NPC_CAMERA SetAdditiveParameter_TargetOffset refused on %s",Text(camera).c_str());return;}
    camera_=Id(camera);camera_owned_=true;camera_applied_=lift;camera_holds_=0;camera_probe_at_=GetTickCount64()+1500;camera_hold_at_=GetTickCount64()+kCameraHoldMs;
    Logf("NPC_CAMERA raised camera=%s class=%s headDeltaCm=%.1f appliedCm=%.1f additive=%ls",Text(camera).c_str(),Text(ue::ClassOf(camera)).c_str(),camera_lift_,lift,kCameraAdditive);
}
// Same named additive, same value: a no-op while it holds, and back in place if a camera mode reset it.
void HoldCameraLift() {
    if(!camera_owned_||!Same(camera_)||GetTickCount64()<camera_hold_at_)return;camera_hold_at_=GetTickCount64()+kCameraHoldMs;
    if(SetCameraLift(camera_.object,camera_applied_))++camera_holds_;
}
void ResetCameraLift() {
    if(!camera_owned_)return;camera_owned_=false;camera_probe_at_=camera_hold_at_=0;
    Logf("NPC_CAMERA held appliedCm=%.1f reasserts=%u",camera_applied_,camera_holds_);
    if(!Same(camera_)){Logf("NPC_CAMERA camera component gone; nothing to reset");camera_={};return;}
    const bool reset=SetCameraLift(camera_.object,0);Logf("NPC_CAMERA reset camera=%s ok=%d",Text(camera_.object).c_str(),reset?1:0);camera_={};
}
// One readback after the blend, so a log shows whether the camera kept the offset.
void ProbeCameraLift() {
    if(!camera_owned_||!camera_probe_at_||GetTickCount64()<camera_probe_at_||!Same(camera_))return;camera_probe_at_=0;
    Call location(camera_.object,"GetCameraLocationObject");Object* object=location.Invoke()?location.Get<Object*>("ReturnValue"):nullptr;
    Vector offset{};bool read=false;if(ue::Alive(object)){Call get(object,"GetTargetOffset");if(get.Invoke()){offset=get.Get<Vector>("ReturnValue");read=true;}}
    Logf("NPC_CAMERA readback locationObject=%s read=%d targetOffset=(%.1f,%.1f,%.1f)",Text(object).c_str(),read?1:0,offset.x,offset.y,offset.z);
}
// The grab module's counted strong-reference pair (exact bytes checked before every call).
bool ClassRefsReady() { return ue::ReferenceCodeMatches(); }
// Only package-root UClasses: never an actor, component or anything whose outer holds a world.
bool PinClass(Object* cls) {
    if(!ClassRefsReady()||!ue::Alive(cls))return false;
    for(const Identity& pin:skill_pins_)if(pin.object==cls&&Same(pin))return true;
    Object* outer=ue::OuterOf(cls);
    if(skill_pins_.size()>=kMaxSkillPins||!ue::IsA(cls,Class("/Script/CoreUObject.Class"))||!ue::Alive(outer)||!ue::IsA(outer,Class("/Script/CoreUObject.Package"))||ue::OuterOf(outer))return false;
    const Identity id=Id(cls);if(!Same(id))return false;
    reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x1573ff0)(cls);skill_pins_.push_back(id);return true;
}
void ReleasePins() {
    if(skill_pins_.empty())return;
    if(!ClassRefsReady()){Logf("NPC_SKILL class references kept: native helper bytes changed");return;}
    size_t released=0;for(const Identity& pin:skill_pins_)if(RawIdentity(pin,pin.index,pin.serial)){reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x158fa30)(pin.object);++released;}
    Logf("NPC_SKILL released class references=%zu of %zu",released,skill_pins_.size());skill_pins_.clear();
}
Object* PawnAsc(Object* pawn) {
    Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"),"GetAbilitySystemComponent");
    return get.Set("Actor",pawn).Invoke()?get.Get<Object*>("ReturnValue"):nullptr;
}
// The spec's ability: its live instance, or the class default while none exists; nullptr once the spec is gone.
Object* SpecAbility(Object* asc,int32_t handle,bool& instance) {
    instance=false;Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"),"GetGameplayAbilityFromSpecHandle");
    if(!get.Set("AbilitySystem",asc).Set("AbilitySpecHandle",handle).Invoke())return nullptr;
    instance=get.Get<bool>("bIsInstance");return get.Get<Object*>("ReturnValue");
}
// The abilities the ASC is running now (ActivatableAbilities rows with an ActiveCount), for a refusal log.
std::string ActiveSpecs(Object* asc) {
    Object* type=Class("/Script/GameplayAbilities.GameplayAbilitySpec"),*container=Class("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t specs=Offset(asc,"ActivatableAbilities"),items=container?ue::Offset(container,"Items"):-1,size=type?ue::StructSize(type):0,
        ability_at=type?ue::Offset(type,"Ability"):-1,count_at=type?ue::Offset(type,"ActiveCount"):-1;
    if(specs<0||items<0||size<=0||ability_at<0||count_at<0)return "?";
    const Array array=ue::At<Array>(asc,specs+items);
    if(array.num<0||array.num>4096||array.max<array.num||(array.num&&!FaceReadable(array.data,static_cast<size_t>(array.num)*static_cast<size_t>(size))))return "?";
    std::string text;
    for(int32_t i=0;i<array.num;++i){Object* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*static_cast<size_t>(size);
        if(ue::At<uint8_t>(row,count_at))text+=(text.empty()?"":",")+Text(ue::At<Object*>(row,ability_at));}
    return text.empty()?"none":text;
}
// Why TryActivateAbility said no: the ability's required tags the hero lacks, blocked tags it has, and its cooldown / cost.
void LogActivationRefusal(Object* asc,Object* ability,const std::string& name) {
    static constexpr std::pair<const char*,bool> kChecks[]={{"ActivationRequiredTags",true},{"ActivationBlockedTags",false},{"SourceRequiredTags",true},{"SourceBlockedTags",false}};
    std::string text;
    for(const auto& [property,want]:kChecks){
        const Array tags=Property<Array>(ability,property);if(tags.num<=0||tags.num>64||!tags.data)continue;
        text+=std::string(text.empty()?"":" ")+property+"=[";
        for(int32_t i=0;i<tags.num;++i){const Name tag=static_cast<const Name*>(tags.data)[i];Call has(asc,"HasMatchingGameplayTag","/Script/GameplayTags.GameplayTagAssetInterface");
            const char* mark=has.Set("TagToCheck",tag).Invoke()?(has.Get<bool>("ReturnValue")==want?"":"!"):"?";text+=(i?",":"")+std::string(mark)+Text(tag);}
        text+="]";
    }
    Object* montage=nullptr;if(Object* anim=Same(primary_)?AnimInstance(primary_.object):nullptr){Call current(anim,"GetCurrentActiveMontage");if(current.Invoke())montage=current.Get<Object*>("ReturnValue");}
    Logf("NPC_SKILL refused ability=%s %s cooldown=%s cost=%s running=[%s] heroMontage=%s ('!' = the condition the hero fails)",name.c_str(),text.empty()?"tags=none":text.c_str(),
        Text(Property<Object*>(ability,"CooldownGameplayEffectClass")).c_str(),Text(Property<Object*>(ability,"CostGameplayEffectClass")).c_str(),ActiveSpecs(asc).c_str(),Text(montage).c_str());
}
// ---- The boss's weapons while one of its skills runs --------------------------------------------
bool IsWeaponActor(Object* actor) { return ue::Alive(actor)&&Offset(actor,"WeaponMesh")>=0; }
// Weapon actors attached anywhere under the hero (hands, back, scabbards), found as the grab add-on does.
std::vector<Object*> HeroWeaponActors(Object* pawn) {
    std::vector<Object*> result;Call attached(pawn,"GetAttachedActors");
    if(!attached.Set("bResetArray",true).Set("bRecursivelyIncludeAttachedActors",true).Invoke())return result;
    const Array list=attached.Get<Array>("OutActors");
    if(list.data&&list.num>0&&list.num<=256)for(int32_t i=0;i<list.num;++i){Object* actor=static_cast<Object**>(list.data)[i];if(IsWeaponActor(actor))result.push_back(actor);}
    ue::Free(list.data);return result;
}
bool IsEffect(Object* component) {
    Object* niagara=Class("/Script/Niagara.NiagaraComponent"),*cascade=Class("/Script/Engine.ParticleSystemComponent");
    return ue::Alive(component)&&((niagara&&ue::IsA(component,niagara))||(cascade&&ue::IsA(component,cascade)));
}
// Every hero weapon actor is hidden for the skill one primitive component at a time, each one's own hidden
// state kept so the end shows only those that were visible. Not the actor: the boss skill's weapon effects
// spawn on the hero's weapon actors (MoveSkillEffects), and a component renders only while its owner actor is
// not hidden. A later pass (a weapon the skill attached since) hides new meshes only: an effect that appeared
// since is the skill's own. Shapes stay as they are (hidden in game anyway; the copied boss colliders are shapes).
// EnforceHiddenFlags hides a part again every frame if a skill shows one.
void HideHeroWeapons() {
    if(!Same(pawn_))return;
    Object* primitive=Class("/Script/Engine.PrimitiveComponent"),*shape=Class("/Script/Engine.ShapeComponent");if(!primitive)return;
    for(Object* actor:HeroWeaponActors(pawn_.object)){
        auto known=std::find_if(hidden_weapons_.begin(),hidden_weapons_.end(),[&](const HiddenWeapon& w){return w.actor.object==actor&&Same(w.actor);});
        const bool fresh=known==hidden_weapons_.end();
        if(fresh){hidden_weapons_.push_back({Id(actor),{},0});known=std::prev(hidden_weapons_.end());}
        for(Object* part:Components(actor,primitive)){
            if((shape&&ue::IsA(part,shape))||(!fresh&&IsEffect(part)))continue;
            if(std::any_of(known->parts.begin(),known->parts.end(),[&](const HiddenPart& p){return p.component.object==part;}))continue;
            bool visible=false,hidden=false;if(!VisibilityOf(part,visible,hidden))continue;
            known->parts.push_back({Id(part),hidden});
            if(!hidden)Call(part,"SetHiddenInGame").Set("NewHidden",true).Set("bPropagateToChildren",false).Invoke();
        }
    }
}
void RestoreHeroWeapons() {
    size_t shown=0;
    for(const HiddenWeapon& w:hidden_weapons_)for(const HiddenPart& p:w.parts)
        if(!p.was_hidden&&Same(p.component)&&Call(p.component.object,"SetHiddenInGame").Set("NewHidden",false).Set("bPropagateToChildren",false).Invoke())++shown;
    if(!hidden_weapons_.empty())Logf("NPC_SKILL hero weapons shown again parts=%zu actors=%zu",shown,hidden_weapons_.size());
    hidden_weapons_.clear();
}
// FRotator (degrees) -> quaternion xyzw, exactly as UE's FRotator::Quaternion.
void RotatorQuat(double pitch,double yaw,double roll,double* q) {
    const double k=3.14159265358979323846/360.0,sp=std::sin(pitch*k),cp=std::cos(pitch*k),sy=std::sin(yaw*k),cy=std::cos(yaw*k),sr=std::sin(roll*k),cr=std::cos(roll*k);
    q[0]=cr*sp*sy-sr*cp*cy;q[1]=-cr*sp*cy-sr*cp*sy;q[2]=cr*cp*sy-sr*sp*cy;q[3]=cr*cp*cy+sr*sp*sy;
}
// Weapon configs name the bone (weapon_r) or a socket (weapon_r_Socket, weapon_rSocket); take what the display mesh has.
Name DisplaySocket(Object* mesh,const char* text) {
    const std::string base=text?text:"";std::vector<std::string> tries{base};
    const std::string lower=Lower(base);const size_t cut=lower.rfind("socket");
    if(cut!=std::string::npos&&cut+6==lower.size()){std::string stem=base.substr(0,cut);if(!stem.empty()&&stem.back()=='_')stem.pop_back();tries.push_back(stem);}
    else{tries.push_back(base+"_Socket");tries.push_back(base+"_socket");}
    for(const std::string& name:tries){const std::wstring wide(name.begin(),name.end());const Name candidate=ue::MakeName(wide.c_str());Call exists(mesh,"DoesSocketExist");
        if(!candidate.None()&&exists.Set("InSocketName",candidate).Invoke()&&exists.Get<bool>("ReturnValue"))return candidate;}
    return {};
}
// One boss weapon: its weapon actor's "Weapon Mesh" template (mesh, materials, relative placement) as a
// display-only component on the NPC display mesh, the boss's own skeleton, at the config's socket.
bool ShowSkillWeapon(const Context& context,Object* display,const NpcSkillWeapon& row) {
    const std::string path=row.actor_class;const size_t dot=path.find('.');if(dot==std::string::npos)return false;
    if(!Class(path.c_str()))LoadClassPackage(path.substr(0,dot));
    Object* source=Property<Object*>(Class((path.substr(0,dot+1)+"Default__"+path.substr(dot+1)).c_str()),"WeaponMesh");
    Object* skeletal=Class("/Script/Engine.SkeletalMeshComponent"),*static_type=Class("/Script/Engine.StaticMeshComponent");
    const bool is_skeletal=ue::Alive(source)&&skeletal&&ue::IsA(source,skeletal),is_static=ue::Alive(source)&&!is_skeletal&&static_type&&ue::IsA(source,static_type);
    Object* asset=is_skeletal?SkelMesh(source):is_static?Property<Object*>(source,"StaticMesh"):nullptr;
    const Name socket=DisplaySocket(display,row.socket);
    if(!ue::Alive(asset)||socket.None()){Logf("NPC_SKILL boss weapon %s: template=%d mesh=%d socket %s on %s=%d; not shown",path.c_str(),ue::Alive(source)?1:0,ue::Alive(asset)?1:0,row.socket,Text(display).c_str(),socket.None()?0:1);return false;}
    struct Rotator { double pitch=0,yaw=0,roll=0; };
    const Vector at=Property<Vector>(source,"RelativeLocation"),size=Property<Vector>(source,"RelativeScale3D");const Rotator turn=Property<Rotator>(source,"RelativeRotation");
    Transform relative;RotatorQuat(turn.pitch,turn.yaw,turn.roll,relative.rotation);
    relative.translation[0]=at.x;relative.translation[1]=at.y;relative.translation[2]=at.z;relative.scale[0]=size.x;relative.scale[1]=size.y;relative.scale[2]=size.z;
    Object* component=Add(context.pawn,is_skeletal?skeletal:static_type,relative);if(!component)return false;
    skill_weapons_.push_back(Id(component)); // tracked first: RemoveSkillWeapons destroys it whatever fails below
    const bool mesh_set=is_skeletal?SetMesh(component,asset):Call(component,"SetStaticMesh").Set("NewMesh",asset).Invoke();
    Call(component,"SetCollisionEnabled").Set("NewType",uint8_t{0}).Invoke();
    const bool attached=AttachFinish(context.pawn,component,display,socket,relative);
    const bool dressed=attached&&CopyMaterials(source,component)&&MatchPlayerLighting(context.mesh,component);
    Logf("NPC_SKILL boss weapon %s %s socket=%s on %s mesh=%s meshSet=%d attached=%d materials+lighting=%d",attached&&mesh_set?"shown":"failed",path.c_str(),Text(socket).c_str(),
        Text(display).c_str(),Text(asset).c_str(),mesh_set?1:0,attached?1:0,dressed?1:0);
    return attached&&mesh_set;
}
// ---- Boss attack colliders ------------------------------------------------------------------------
// A sweep bullet (PBZ_SweepTraceBullet) takes its source components from its data's holder: the avatar's socket
// component, or each weapon actor WeaponUtils.GetWeaponActorsByDisplayIndex(avatar, slot, index) returns. On the
// actor owning each source it then sweeps every primitive component tagged AttachColliderData.ComponentTag
// (PBZ.exe 0x7d72110 -> AttachColliderHelper::GetAttachedCollidersAndShapes 0x5be6aa0 -> AActor::GetComponentsByTag).
// The hero's weapons carry only "Attack Collider" and the hero's pawn none, so the boss tags (Attack Collider
// Strong / Stab, LeftLeg, Big Arm, Snake Spine L ...) found nothing and those hits dealt no damage. For the skill,
// every Box / Sphere / Capsule template in the construction scripts of the boss weapon and of the boss tagged one
// of the skin's kNpcColliderTagSets is copied (shape, tags, collision profile): weapon ones owned by the hero
// weapon actors standing for that weapon, at the display's weapon socket (the boss weapon's root) or on the shown
// boss weapon mesh; body ones owned by the pawn, on the display's bone. RemoveSkillWeapons destroys them.
#pragma warning(push)
#pragma warning(disable: 4324) // Transform is 16-byte aligned.
struct ShapeTemplate { Object* component=nullptr;Transform relative;Name parent,socket;bool on_mesh=false; };
#pragma warning(pop)
Transform TemplateRelative(Object* component) {
    struct Rotator { double pitch=0,yaw=0,roll=0; };
    const Vector at=Property<Vector>(component,"RelativeLocation"),size=Property<Vector>(component,"RelativeScale3D");const Rotator turn=Property<Rotator>(component,"RelativeRotation");
    Transform relative;RotatorQuat(turn.pitch,turn.yaw,turn.roll,relative.rotation);
    relative.translation[0]=at.x;relative.translation[1]=at.y;relative.translation[2]=at.z;relative.scale[0]=size.x;relative.scale[1]=size.y;relative.scale[2]=size.z;
    return relative;
}
bool TagWanted(Object* component,const NpcColliderTags& want,std::string& matched) {
    const Array tags=Property<Array>(component,"ComponentTags");
    if(tags.num<=0||tags.num>32||!tags.data)return false;
    for(int32_t i=0;i<tags.num;++i)for(size_t k=0;k<want.count;++k)if(ue::NameIs(static_cast<Name*>(tags.data)[i],want.tags[k])){matched=want.tags[k];return true;}
    return false;
}
// The tagged shape templates of a blueprint class's construction scripts (its own and its blueprint parents'),
// each placed relative to its nearest parent outside the scripts (parents inside them composed). on_mesh: that
// parent is mesh_name (the weapon's mesh / the character mesh) and socket is where it hangs on it.
std::vector<ShapeTemplate> ShapeTemplates(Object* cls,const NpcColliderTags& want,Name mesh_name,std::string& seen) {
    std::vector<ShapeTemplate> out;Object* shape=Class("/Script/Engine.ShapeComponent"),*generated=Class("/Script/Engine.BlueprintGeneratedClass");
    if(!shape||!generated)return out;
    struct Node { Object* component;Name variable,parent,socket; };std::vector<Node> nodes;
    for(Object* c=cls;c&&ue::IsA(c,generated)&&nodes.size()<256;c=ue::At<Object*>(c,ue::layout::kSuper)){
        Object* script=Property<Object*>(c,"SimpleConstructionScript");if(!ue::Alive(script))continue;
        const Array all=Property<Array>(script,"AllNodes");if(all.num<=0||all.num>256||!all.data)continue;
        for(int32_t i=0;i<all.num;++i){Object* n=static_cast<Object**>(all.data)[i];if(!ue::Alive(n))continue;
            nodes.push_back({Property<Object*>(n,"ComponentTemplate"),Property<Name>(n,"InternalVariableName"),Property<Name>(n,"ParentComponentOrVariableName"),Property<Name>(n,"AttachToName")});}
    }
    for(const Node& n:nodes){
        std::string tag;if(!ue::Alive(n.component)||!ue::IsA(n.component,shape)||!TagWanted(n.component,want,tag))continue;
        ShapeTemplate s;s.component=n.component;s.relative=TemplateRelative(n.component);s.socket=n.socket;Name parent=n.parent;
        for(size_t depth=0;depth<8;++depth){
            if(!mesh_name.None()&&parent==mesh_name){s.on_mesh=true;break;}
            const auto up=std::find_if(nodes.begin(),nodes.end(),[&](const Node& m){return !parent.None()&&m.variable==parent;});
            if(up==nodes.end()||!ue::Alive(up->component))break;
            Transform composed;if(!Compose(s.relative,TemplateRelative(up->component),composed))break;
            s.relative=composed;s.socket=up->socket;parent=up->parent;
        }
        s.parent=parent;out.push_back(s);if(seen.find(tag)==std::string::npos)seen+=(seen.empty()?"":",")+tag;
    }
    return out;
}
// The template's tags, written into an array the game allocated (the display's GetAllSocketNames, cut down) so
// the component frees it as its own.
bool CopyTags(Object* from,Object* to) {
    const Array tags=Property<Array>(from,"ComponentTags");const int32_t at=Offset(to,"ComponentTags");
    if(at<0||tags.num<=0||tags.num>32||!tags.data||!Same(display_))return false;
    Call names(display_.object,"GetAllSocketNames");if(!names.Invoke())return false;
    Array storage=names.Get<Array>("ReturnValue");
    if(storage.num<tags.num||!storage.data){ue::Free(storage.data);return false;}
    std::memcpy(storage.data,tags.data,sizeof(Name)*static_cast<size_t>(tags.num));storage.num=tags.num;
    Array& dest=ue::At<Array>(to,at);if(dest.data)ue::Free(dest.data);dest=storage;return true;
}
// One shape template copied onto owner (shape, tags, collision profile; never physics), on parent at socket.
Object* CopyShape(Object* owner,const ShapeTemplate& s,Object* parent,Name socket) {
    Object* source=s.component,*box=Class("/Script/Engine.BoxComponent"),*sphere=Class("/Script/Engine.SphereComponent"),*capsule=Class("/Script/Engine.CapsuleComponent");
    Object* component=Add(owner,ue::ClassOf(source),s.relative);if(!component)return nullptr;
    skill_colliders_.push_back(Id(component)); // tracked first: RemoveSkillWeapons destroys it whatever fails below
    if(box&&ue::IsA(source,box))Call(component,"SetBoxExtent").Set("InBoxExtent",Property<Vector>(source,"BoxExtent")).Set("bUpdateOverlaps",false).Invoke();
    else if(sphere&&ue::IsA(source,sphere))Call(component,"SetSphereRadius").Set("InSphereRadius",Property<float>(source,"SphereRadius")).Set("bUpdateOverlaps",false).Invoke();
    else if(capsule&&ue::IsA(source,capsule))Call(component,"SetCapsuleSize").Set("InRadius",Property<float>(source,"CapsuleRadius")).Set("InHalfHeight",Property<float>(source,"CapsuleHalfHeight")).Set("bUpdateOverlaps",false).Invoke();
    Call profile(source,"GetCollisionProfileName");
    if(profile.Invoke())Call(component,"SetCollisionProfileName").Set("InCollisionProfileName",profile.Get<Name>("ReturnValue")).Set("bUpdateOverlaps",false).Invoke();
    Call enabled(source,"GetCollisionEnabled");const uint8_t mode=enabled.Invoke()?enabled.Get<uint8_t>("ReturnValue"):0;
    Call(component,"SetCollisionEnabled").Set("NewType",static_cast<uint8_t>(mode==3?1:mode==2?0:mode)).Invoke();
    return CopyTags(source,component)&&AttachFinish(owner,component,parent,socket,s.relative)?component:nullptr;
}
// WeaponUtils.GetWeaponActorsByDisplayIndex(Avatar, SlotTag, DisplayIndex, WithPending, OutActors): the hero weapon
// actors a weapon-held bullet or effect of that display index uses (WithPending false, as the sweep bullet passes it).
// Before c75 this wanted exactly four parameters, so it always came back empty: the colliders went onto every hidden
// hero weapon and weaponGaps listed index 0 too.
std::vector<Object*> SlotWeaponActors(Object* pawn,int32_t display) {
    std::vector<Object*> result;Object* utils=ue::Default("/Script/SGameCombatSystem.Default__WeaponUtils");
    Object* function=utils?ue::FindFunction(ue::ClassOf(utils),"GetWeaponActorsByDisplayIndex"):nullptr;if(!function)return result;
    std::map<std::string,int32_t> params;
    for(uintptr_t f=ue::At<uintptr_t>(function,ue::layout::kChildProperties);f&&params.size()<8;f=ue::At<uintptr_t>(reinterpret_cast<void*>(f),ue::layout::kPropNext))
        params[Text(ue::At<Name>(reinterpret_cast<void*>(f),ue::layout::kPropName))]=ue::At<int32_t>(reinterpret_cast<void*>(f),ue::layout::kPropElementSize);
    const auto has=[&](const char* name,int32_t size){const auto p=params.find(name);return p!=params.end()&&p->second==size;};
    if(!has("Avatar",8)||!has("SlotTag",8)||!has("DisplayIndex",4)||!has("OutActors",16)||(params.size()!=4&&!(params.size()==5&&has("WithPending",1))))return result;
    const Name main=ue::MakeName(L"Weapon.Slot.Main");if(main.None())return result;
    Call get(utils,function);get.Set("Avatar",pawn).Set("SlotTag",main).Set("DisplayIndex",display);
    if(params.size()==5)get.Set("WithPending",false);
    if(!get.Invoke())return result;
    const Array list=get.Get<Array>("OutActors");
    if(list.num>0&&list.num<=16&&list.data)for(int32_t i=0;i<list.num;++i){Object* actor=static_cast<Object**>(list.data)[i];if(ue::Alive(actor))result.push_back(actor);}
    ue::Free(list.data);return result;
}
// ---- Weapon display-index stand-ins -----------------------------------------------------------------
// GetWeaponActorsByDisplayIndex(avatar, slot, k) is WeaponActors[k] of the avatar's current weapon for the slot (all
// of them for k < 0): CharacterWeaponsComponent.RuntimeWeaponData[slot].Weapons[current].WeaponActors (PBZ.exe
// 0x5e96020 -> 0x5e76f20 -> RuntimeWeaponSingleData +0x10). Bosses with two or more weapons address the others as
// display index 1..3 (沐小葵 double swords 2, 钩爪客 1..3, 乱世 / 铁鬼 1..3, 鬼差 1 ...: 302 skills of 25 skins); the
// hero's weapon has index 0 only, so those weapon-held sweeps found no source and dealt no damage. While a skill
// runs, every index its skin's skills address (kNpcWeaponDisplays, at least 0) that the entry the lookup reads
// lacks gets a plain actor (scene root at the boss weapon's socket on the display) appended to that WeaponActors array
// (reflection + GenericArray_Add, no native hook); AddSkillColliders copies the boss weapon's attack colliders
// onto it, and the skill's end removes and destroys it. It is no weapon actor (no SocketComponentHolder): weapon
// effects of index >= 1 still have no socket to spawn on, and an all-weapons (-1) effect still spawns once.
// The hero's Main weapon slot (RuntimeWeaponData: TMap<GameplayTag, RuntimeWeaponSlotData{Weapons:
// TArray<RuntimeWeaponSingleData>}>): its weapon entries, and per entry the WeaponActors array with that array's
// FArrayProperty for GenericArray_Add / Remove. Layout from reflection; no entries when anything does not match.
struct MainSlot {
    Object* weapons=nullptr;const Array* entries=nullptr;int32_t entry_size=0,actors_at=0;uintptr_t field=0;
    int32_t Count() const { return entries?entries->num:0; }
    Array* Actors(int32_t e) const {
        if(!entries||e<0||e>=entries->num)return nullptr;
        Array* members=reinterpret_cast<Array*>(static_cast<unsigned char*>(entries->data)+size_t(e)*size_t(entry_size)+actors_at);
        return members->num>=0&&members->num<=members->max&&members->max<=64&&(!members->num||FaceReadable(members->data,size_t(members->num)*sizeof(Object*)))?members:nullptr;
    }
    int32_t Holding(Object* member) const {
        for(int32_t e=0;e<Count();++e)if(const Array* a=Actors(e))for(int32_t m=0;m<a->num;++m)if(static_cast<Object**>(a->data)[m]==member)return e;
        return -1;
    }
};
MainSlot FindMainSlot(Object* pawn) {
    MainSlot out;static const Name main=ue::MakeName(L"Weapon.Slot.Main");if(main.None()||!ue::Alive(pawn))return out;
    Call get(pawn,"GetCharacterWeaponsComponent","/Script/SGameCombatSystem.CharacterWeaponsHolder");
    Object* weapons=get.Invoke()?get.Get<Object*>("ReturnValue"):nullptr;if(!ue::Alive(weapons))return out;out.weapons=weapons;
    const auto prop=[](uintptr_t field){return reinterpret_cast<Object*>(field);};
    const uintptr_t map=Field(ue::ClassOf(weapons),"RuntimeWeaponData");if(!map||FieldKind(prop(map))!=0x400000000000ull)return out;
    Object* key=ue::At<Object*>(prop(map),0x70),*value=ue::At<Object*>(prop(map),0x78);
    if(!key||!value||FieldKind(key)!=0x100000||FieldKind(value)!=0x100000||Text(ue::At<Object*>(key,0x70))!="GameplayTag")return out;
    const uintptr_t list=Field(ue::At<Object*>(value,0x70),"Weapons");if(!list||FieldKind(prop(list))!=0x200000)return out;
    Object* inner=ue::At<Object*>(prop(list),0x78);if(!inner||FieldKind(inner)!=0x100000)return out;
    const uintptr_t actors=Field(ue::At<Object*>(inner,0x70),"WeaponActors");if(!actors||FieldKind(prop(actors))!=0x200000)return out;
    Object* element=ue::At<Object*>(prop(actors),0x78);if(!element||ue::At<int32_t>(element,ue::layout::kPropElementSize)!=8)return out;
    const int32_t entry_size=ue::At<int32_t>(inner,ue::layout::kPropElementSize),value_offset=ue::At<int32_t>(prop(map),0x80),stride=ue::At<int32_t>(prop(map),0x8c);
    const int32_t list_at=ue::At<int32_t>(prop(list),ue::layout::kPropOffset),actors_at=ue::At<int32_t>(prop(actors),ue::layout::kPropOffset);
    const unsigned char* at=reinterpret_cast<const unsigned char*>(weapons)+ue::At<int32_t>(prop(map),ue::layout::kPropOffset);
    const int32_t count=*reinterpret_cast<const int32_t*>(at+8),bits=*reinterpret_cast<const int32_t*>(at+40);
    unsigned char* data=*reinterpret_cast<unsigned char* const*>(at);const uint32_t* allocation=*reinterpret_cast<uint32_t* const*>(at+32);
    if(!allocation)allocation=reinterpret_cast<const uint32_t*>(at+16);
    if(entry_size<=0||entry_size>4096||stride<16||stride>4096||value_offset<8||value_offset>=stride||count<=0||count>64||bits<count||
        !FaceReadable(data,size_t(count)*size_t(stride))||!FaceReadable(allocation,(size_t(count)+31)/32*4))return out;
    for(int32_t i=0;i<count;++i){
        unsigned char* row=data+size_t(i)*size_t(stride);
        if(!(allocation[i/32]&(1u<<(i%32)))||!(*reinterpret_cast<const Name*>(row)==main))continue;
        const Array* entries=reinterpret_cast<const Array*>(row+value_offset+list_at);
        if(entries->num<=0||entries->num>64||!FaceReadable(entries->data,size_t(entries->num)*size_t(entry_size)))return out;
        out.entries=entries;out.entry_size=entry_size;out.actors_at=actors_at;out.field=actors;return out;
    }
    return out;
}
bool CurrentMainWeapon(Object* weapons,int32_t& index);
// A plain actor (scene root) owned by the hero, its root snapped to parent's socket.
Object* SpawnStandIn(Object* pawn,Object* parent,Name socket) {
    Object* statics=ue::Default("/Script/Engine.Default__GameplayStatics"),*actor_class=Class("/Script/Engine.Actor"),*scene=Class("/Script/Engine.SceneComponent");
    if(!statics||!actor_class||!scene||!ue::Alive(parent))return nullptr;
    Transform at;{Call get(pawn,"GetTransform");if(get.Invoke())at=get.Get<Transform>("ReturnValue");}
    Call begin(statics,"BeginDeferredActorSpawnFromClass");
    if(!begin.Set("WorldContextObject",pawn).Set("ActorClass",actor_class).Set("SpawnTransform",at).Set("CollisionHandlingOverride",uint8_t{1}).Set("Owner",pawn).Invoke())return nullptr;
    Object* actor=begin.Get<Object*>("ReturnValue");if(!ue::Alive(actor))return nullptr;
    const auto drop=[&](const char* why){Logf("NPC_SKILL stand-in %s: %s; destroyed",Text(actor).c_str(),why);Call(actor,"K2_DestroyActor").Invoke();return nullptr;};
    if(!Call(statics,"FinishSpawningActor").Set("Actor",actor).Set("SpawnTransform",at).Invoke())return drop("FinishSpawningActor refused");
    Call add(actor,"AddComponentByClass");
    if(!add.Set("Class",scene).Set("bManualAttachment",false).Set("RelativeTransform",Transform{}).Set("bDeferredFinish",false).Invoke())return drop("AddComponentByClass refused");
    Object* root=Property<Object*>(actor,"RootComponent");if(!ue::Alive(root)||root!=add.Get<Object*>("ReturnValue"))return drop("no scene root");
    Call attach(root,"K2_AttachToComponent"); // EAttachmentRule::SnapToTarget = 2
    if(!attach.Set("Parent",parent).Set("SocketName",socket).Set("LocationRule",uint8_t{2}).Set("RotationRule",uint8_t{2}).Set("ScaleRule",uint8_t{2}).Set("bWeldSimulatedBodies",false).Invoke()||
        !attach.Get<bool>("ReturnValue"))return drop("attach refused");
    return actor;
}
// The entry the native lookup reads: the one holding what GetWeaponActorsByDisplayIndex(-1) returns, else the
// slot's current one (GetCurrentWeapon). Every index below max(kNpcWeaponDisplays, 1) it lacks gets a stand-in,
// index 0 too when that entry has no actor at all.
void AddStandIns(const Context& context) {
    if(!stand_ins_.empty()||selected_<0||static_cast<size_t>(selected_)>=std::size(kNpcWeaponDisplays)||!Same(display_))return;
    const int32_t want=(std::max)(int32_t{kNpcWeaponDisplays[selected_]},1);
    const MainSlot slot=FindMainSlot(context.pawn);const std::vector<Object*> all=SlotWeaponActors(context.pawn,-1);
    int32_t current=-1;const bool known=slot.weapons&&CurrentMainWeapon(slot.weapons,current);
    const int32_t used=!all.empty()?slot.Holding(all.front()):known&&current<slot.Count()?current:-1;
    std::string layout;
    for(int32_t e=0;e<slot.Count()&&layout.size()<400;++e){const Array* a=slot.Actors(e);char row[160]{};
        _snprintf_s(row,sizeof(row),_TRUNCATE,"%s%d:%d:%s",layout.empty()?"":" ",e,a?a->num:-1,a&&a->num?Text(static_cast<Object**>(a->data)[0]).c_str():"-");layout+=row;}
    Array* target=slot.Actors(used);
    Logf("NPC_SKILL hero Main slot entries=%d current=%d%s lookupAll=%zu used=%d [%s]",slot.Count(),current,known?"":"(unknown)",all.size(),used,layout.empty()?"-":layout.c_str());
    if(!target||target->num>=want||!NativeHelpersMatch()){if(!target)Logf("NPC_SKILL stand-ins: none (no entry the lookup reads; array helpers=%d)",NativeHelpersMatch()?1:0);return;}
    const NpcSkillSet& set=kNpcSkillSets[selected_];const int32_t had=target->num;
    for(int32_t k=had;k<want;++k){
        // At the boss weapon of that index, else at the first one's socket (an index of a weapon in another config).
        const size_t row=static_cast<size_t>(k)<set.weapon_count?static_cast<size_t>(k):0;
        const Name socket=set.weapon_count?DisplaySocket(display_.object,set.weapons[row].socket):Name{};
        Object* actor=SpawnStandIn(context.pawn,display_.object,socket);if(!actor)break;
        stand_ins_.push_back({Id(actor),k}); // tracked first: RemoveStandIns destroys it whatever fails below
        const int32_t at=reinterpret_cast<int32_t(__fastcall*)(void*,const void*,const void*)>(ue::Base()+kArrayAddRva)(target,reinterpret_cast<void*>(slot.field),&actor);
        if(at!=k||target->num!=k+1||static_cast<Object**>(target->data)[k]!=actor){Logf("NPC_SKILL stand-in %d: array add returned %d (num %d); stopped",k,at,target->num);break;}
    }
    int32_t resolved=0;for(int32_t k=0;k<want;++k)if(!SlotWeaponActors(context.pawn,k).empty())++resolved; // the native lookup now finds them
    Logf("NPC_SKILL stand-ins for display %d..%d: %zu added in entry %d on %s; lookup resolves %d/%d",had,want-1,stand_ins_.size(),used,Text(display_.object).c_str(),resolved,want);
}
// Out of the hero's WeaponActors (last added first, so the indices before stay) and destroyed.
void RemoveStandIns() {
    if(stand_ins_.empty())return;
    size_t removed=0,destroyed=0;
    for(auto s=stand_ins_.rbegin();s!=stand_ins_.rend();++s){
        if(!Same(s->actor))continue;
        const MainSlot slot=Same(pawn_)&&NativeHelpersMatch()?FindMainSlot(pawn_.object):MainSlot{};
        if(Array* list=slot.Actors(slot.Holding(s->actor.object)))for(int32_t i=list->num-1;i>=0;--i)if(static_cast<Object**>(list->data)[i]==s->actor.object){
            reinterpret_cast<void(__fastcall*)(void*,const void*,int32_t)>(ue::Base()+kArrayRemoveRva)(list,reinterpret_cast<void*>(slot.field),i);++removed;break;}
        if(Call(s->actor.object,"K2_DestroyActor").Invoke())++destroyed;
    }
    Logf("NPC_SKILL stand-ins removed=%zu destroyed=%zu of %zu",removed,destroyed,stand_ins_.size());stand_ins_.clear();
}
void AddSkillColliders(const Context& context) {
    if(selected_<0||static_cast<size_t>(selected_)>=std::size(kNpcColliderTagSets)||!Same(display_))return;
    const NpcColliderTags& want=kNpcColliderTagSets[selected_];const NpcSkillSet& set=kNpcSkillSets[selected_];
    if(!want.count)return;
    size_t weapon=0,body=0,owners=0;std::string tags;
    std::vector<std::vector<ShapeTemplate>> shapes(set.weapon_count);
    // Boss weapon k's shapes onto owner: on the shown boss weapon mesh, else at the display's weapon socket.
    const auto copy_weapon=[&](size_t k,Object* owner){
        const Name root=DisplaySocket(display_.object,set.weapons[k].socket);
        Object* shown=k<boss_weapon_rows_.size()&&Same(boss_weapon_rows_[k])?boss_weapon_rows_[k].object:nullptr;
        for(const ShapeTemplate& s:shapes[k]){
            Object* parent=s.on_mesh?shown:display_.object;const Name socket=s.on_mesh?s.socket:root;
            if(parent&&(s.on_mesh||!root.None())&&CopyShape(owner,s,parent,socket))++weapon;}
    };
    for(size_t k=0;k<set.weapon_count;++k){
        const std::string path=set.weapons[k].actor_class;const size_t dot=path.find('.');Object* cls=dot==std::string::npos?nullptr:Class(path.c_str());if(!cls)continue;
        Object* mesh=Property<Object*>(Class((path.substr(0,dot+1)+"Default__"+path.substr(dot+1)).c_str()),"WeaponMesh");
        shapes[k]=ShapeTemplates(cls,want,ue::Alive(mesh)?ue::NameOf(mesh):Name{},tags);if(shapes[k].empty())continue;
        // The hero weapons (or stand-in) of the same display index stand for this boss weapon (its effects go to it
        // too); with none there, the first ones (or every hero weapon) also take its colliders.
        std::vector<Object*> actors=SlotWeaponActors(context.pawn,static_cast<int32_t>(k));const bool exact=!actors.empty();
        if(actors.empty()&&k)actors=SlotWeaponActors(context.pawn,0);
        if(actors.empty())for(const HiddenWeapon& w:hidden_weapons_)if(Same(w.actor))actors.push_back(w.actor.object);
        for(Object* actor:actors){
            if(exact)for(HiddenWeapon& w:hidden_weapons_)if(w.actor.object==actor)w.boss=k;
            ++owners;copy_weapon(k,actor);
        }
    }
    // A stand-in past the boss's listed weapons is a weapon of another config (沐小葵's second sword, 赤练's second
    // snake ...): it takes every boss weapon's colliders, or with no boss weapon (an unarmed boss) the hero weapon's own.
    for(const StandIn& s:stand_ins_){
        if(!Same(s.actor)||static_cast<size_t>(s.index)<set.weapon_count)continue;
        ++owners;const size_t before=weapon;
        for(size_t k=0;k<set.weapon_count;++k)copy_weapon(k,s.actor.object);
        // The hero's own Main weapon: the one shown before the skill (its parts were visible when hidden).
        static const Name main=ue::MakeName(L"Weapon.Slot.Main");Object* shape=Class("/Script/Engine.ShapeComponent");
        if(weapon==before&&shape)for(const HiddenWeapon& w:hidden_weapons_){
            if(!Same(w.actor)||!(Property<Name>(w.actor.object,"Slot")==main)||std::none_of(w.parts.begin(),w.parts.end(),[](const HiddenPart& p){return !p.was_hidden;}))continue;
            for(Object* c:Components(w.actor.object,shape)){
                std::string tag;if(!TagWanted(c,want,tag)||std::any_of(skill_colliders_.begin(),skill_colliders_.end(),[&](const Identity& x){return x.object==c;}))continue;
                ShapeTemplate copy;copy.component=c;if(CopyShape(s.actor.object,copy,c,Name{}))++weapon;
                if(tags.find(tag)==std::string::npos)tags+=(tags.empty()?"":",")+tag;
            }
        }
    }
    Object* monster=set.monster&&*set.monster?LoadClassPackage(set.monster):nullptr,*root=Property<Object*>(context.pawn,"RootComponent");
    if(monster)for(const ShapeTemplate& s:ShapeTemplates(monster,want,ue::MakeName(L"CharacterMesh0"),tags)){
        Object* parent=s.on_mesh?display_.object:root;const Name socket=s.on_mesh?s.socket:Name{};
        if(s.on_mesh&&!socket.None()){Call exists(display_.object,"DoesSocketExist");if(!exists.Set("InSocketName",socket).Invoke()||!exists.Get<bool>("ReturnValue"))continue;}
        if(ue::Alive(parent)&&CopyShape(context.pawn,s,parent,socket))++body;
    }
    Logf("NPC_SKILL boss colliders weapon=%zu (hero weapon actors %zu) body=%zu tags=%s tracked=%zu",weapon,owners,body,tags.empty()?"-":tags.c_str(),skill_colliders_.size());
}
// ---- Boss skill effects ---------------------------------------------------------------------------
// The weapon effect actions (GAAction_WeaponTrail / WeaponParticle / WeaponParticleFrame, GAA_PBZWeaponTrail ...)
// spawn on WeaponUtils.GetWeaponActorsByDisplayIndex(avatar, slot, index)'s SocketComponentHolder component at the
// action's socket: on a skin that is the hero's weapon mesh, so they stayed with the hero's hidden weapon (and,
// while the whole actor was hidden, never rendered). Avatar-held effects and the montage's notify effects attach
// to the hero's own mesh at the boss skeleton's sockets. Every frame of a skill a Niagara / Cascade component that
// appears under a hero weapon moves onto the boss weapon it stands for (same socket, same relative transform), and
// one under the hero's mesh moves onto the display at its socket unless only the hero's skeleton has that socket.
// A boss skill's GAA_SwitchWeapon (44 skills: 沐小葵 P2's double swords, ...) sets the avatar's current weapon to
// the boss's weapon config index (CharacterWeaponsComponent.SetCurrentWeapon + SetArmed) and back at its end.
// On the hero it switched the hero's own weapon mid-skill, and the sweeps then looked for their colliders on
// another (sheathed) weapon. While a skill runs the hero's current Main weapon stays the one it began with.
struct HeroWeaponHold { Identity weapons;int32_t index=-1;uint32_t restored=0; };
HeroWeaponHold weapon_hold_;
bool CurrentMainWeapon(Object* weapons,int32_t& index) {
    static const Name main=ue::MakeName(L"Weapon.Slot.Main");if(main.None()||!ue::Alive(weapons))return false;
    Call get(weapons,"GetCurrentWeapon");if(!get.Set("SlotTag",main).Invoke())return false;
    index=get.Get<int32_t>("CurrentIndex");return index>=0&&index<64;
}
void HoldHeroWeapon(bool begin) {
    if(begin){weapon_hold_={};Call get(pawn_.object,"GetCharacterWeaponsComponent","/Script/SGameCombatSystem.CharacterWeaponsHolder");
        Object* weapons=Same(pawn_)&&get.Invoke()?get.Get<Object*>("ReturnValue"):nullptr;int32_t index=-1;
        if(CurrentMainWeapon(weapons,index)){weapon_hold_.weapons=Id(weapons);weapon_hold_.index=index;}return;}
    int32_t now=-1;if(weapon_hold_.index<0||!Same(weapon_hold_.weapons)||!CurrentMainWeapon(weapon_hold_.weapons.object,now)||now==weapon_hold_.index)return;
    static const Name main=ue::MakeName(L"Weapon.Slot.Main");Call set(weapon_hold_.weapons.object,"SetCurrentWeapon");
    const bool ok=set.Set("SlotTag",main).Set("WeaponIndex",weapon_hold_.index).Invoke()&&set.Get<bool>("ReturnValue");
    if(++weapon_hold_.restored<=3)Logf("NPC_SKILL hero weapon switched by %s: Main %d -> %d, put back=%d",skill_.name.empty()?"the skill tail":skill_.name.c_str(),weapon_hold_.index,now,ok?1:0);
}
std::vector<Object*> Children(Object* component) {
    std::vector<Object*> out;Call get(component,"GetChildrenComponents");if(!get.Set("bIncludeAllDescendants",false).Invoke())return out;
    const Array list=get.Get<Array>("Children");
    if(list.num>0&&list.num<=512&&list.data)for(int32_t i=0;i<list.num;++i){Object* c=static_cast<Object**>(list.data)[i];if(ue::Alive(c))out.push_back(c);}
    ue::Free(list.data);return out;
}
// Handled already and still where it was put or left: a pooled effect the game re-attached under a hero part is new.
bool Seen(Object* effect,Object* holder) {
    return std::any_of(seen_effects_.begin(),seen_effects_.end(),[&](const SeenEffect& s){return s.effect.object==effect&&s.parent==holder&&Same(s.effect);});
}
bool HasSocket(Object* component,Name socket) {
    if(socket.None()||!ue::Alive(component))return false;
    Call exists(component,"DoesSocketExist");return exists.Set("InSocketName",socket).Invoke()&&exists.Get<bool>("ReturnValue");
}
// A boss socket the NPC body lacks can sit on a boss weapon or a part attached to the body (MoYuan's
// TrailStart2 trail starts): that component, else none. Only a component that really has the socket is
// returned; a weapon whose child carries it yields the child, so the socket name resolves where it is used.
Object* BossSocketHolder(Name socket) {
    for(const Identity& w:skill_weapons_)if(Same(w)){
        if(HasSocket(w.object,socket))return w.object;
        for(Object* c:Children(w.object))if(ue::Alive(c)&&!IsEffect(c)&&HasSocket(c,socket))return c;
    }
    if(Same(display_))for(Object* c:Children(display_.object))if(!IsEffect(c)&&HasSocket(c,socket))return c;
    return nullptr;
}
// Where an effect that hangs on a socket its intended parent lacks should go. A weapon effect (spawned on a
// hero weapon) looks for the socket on the boss weapons first, a body effect (spawned on the hero mesh) on the
// NPC body first; the other side is the fallback. nullptr means nothing has the socket.
Object* SocketHost(Name socket,bool prefer_weapon) {
    if(socket.None())return nullptr;
    const auto body=[&]()->Object*{return Same(display_)&&HasSocket(display_.object,socket)?display_.object:nullptr;};
    if(prefer_weapon){if(Object* w=BossSocketHolder(socket))return w;return body();}
    if(Object* b=body())return b;return BossSocketHolder(socket);
}
void MoveEffect(Object* effect,Object* holder,Object* parent,bool keep_hero_socket) {
    if(seen_effects_.size()>=512)seen_effects_.erase(seen_effects_.begin(),seen_effects_.begin()+256);
    Call get(effect,"GetAttachSocketName");const Name socket=get.Invoke()?get.Get<Name>("ReturnValue"):Name{};
    // A named socket the intended parent lacks: the boss's sockets live on its skeleton and weapons, not on
    // the hero's, so look for the component that really owns it first (weapon effect: boss weapons; body
    // effect: the NPC body). Attach rules are EAttachmentRule {KeepRelative, KeepWorld, SnapToTarget}.
    bool has=HasSocket(parent,socket);const uint8_t rule=0; // relative on purpose: a socket-less effect keeps its offset
    if(!socket.None()&&!has){
        // The boss's sockets live on its skeleton and weapons, not on the hero's: look for the component that
        // really owns the name (a weapon effect: the boss weapons first; a body effect: the NPC body).
        if(Object* other=SocketHost(socket,!keep_hero_socket)){
            parent=other;has=HasSocket(parent,socket);
        }else if(++unplaced_effects_<=12)Logf("NPC_SKILL effect %s: no %s socket on the NPC body or a boss weapon; at the parent root",
            Text(effect).c_str(),Text(socket).c_str());
    }
    // Recorded against the component it was found under, not where it was put: the next frame's Seen() check
    // walks the same containers, and an effect moved elsewhere must not be treated as a new one and moved again.
    seen_effects_.push_back({Id(effect),holder});
    Call attach(effect,"K2_AttachToComponent");
    const bool ok=attach.Set("Parent",parent).Set("SocketName",has?socket:Name{}).Set("LocationRule",rule).Set("RotationRule",rule).Set("ScaleRule",rule)
        .Set("bWeldSimulatedBodies",false).Invoke()&&attach.Get<bool>("ReturnValue");
    if(++moved_effects_<=12)Logf("NPC_SKILL effect %s %s socket=%s -> %s%s",ok?"moved":"not moved",Text(effect).c_str(),Text(socket).c_str(),Text(parent).c_str(),
        has||socket.None()?"":" (no such socket: world placement kept)");
}
void MoveSkillBullet(Object* root);
Object* Owner(Object* component);
void MoveSkillEffects() {
    if(!active_||skill_.handle<=0||!Same(display_))return;
    HoldHeroWeapon(false);
    for(const HiddenWeapon& w:hidden_weapons_){
        if(!Same(w.actor))continue;
        Object* target=w.boss<boss_weapon_rows_.size()&&Same(boss_weapon_rows_[w.boss])?boss_weapon_rows_[w.boss].object:nullptr;
        if(!target)for(const Identity& row:boss_weapon_rows_)if(Same(row)){target=row.object;break;}
        if(!target)continue; // an unarmed boss: the effect stays at the hero's hand, now visible
        std::vector<Object*> holders;if(Object* root=Property<Object*>(w.actor.object,"RootComponent");ue::Alive(root))holders.push_back(root);
        for(const HiddenPart& p:w.parts)if(Same(p.component)&&!IsEffect(p.component.object))holders.push_back(p.component.object);
        for(Object* holder:holders)for(Object* c:Children(holder)){
            if(!IsEffect(c)||Seen(c,holder)||std::any_of(w.parts.begin(),w.parts.end(),[&](const HiddenPart& p){return p.component.object==c;}))continue;
            MoveEffect(c,holder,target,false);}
    }
    if(source_driver_mode_&&Same(primary_))for(Object* c:Children(primary_.object))if(IsEffect(c)&&!Seen(c,primary_.object))MoveEffect(c,primary_.object,display_.object,true);
    if(source_driver_mode_&&Same(primary_))for(Object* c:Children(primary_.object))if(!IsEffect(c)&&!Seen(c,primary_.object))MoveSkillBullet(c);
}
// A bullet of the hero's that hangs on a named socket of the hero's mesh (花千笑's wine jar, BD_JiuTan on weapon_l):
// the grip hook maps the hand names as for a hero weapon (the hero's hand frame, times the weapon scale), so the
// jar sat off the hand and 1.29x too big. Its root goes onto the NPC body's own socket: offset as authored for the
// boss, scale the body's. Anything else under the mesh (hero weapons, parts) is left alone.
void MoveSkillBullet(Object* root) {
    Object* bullet=Owner(root);
    if(!ue::Alive(bullet)||Property<Object*>(bullet,"RootComponent")!=root||Property<Object*>(bullet,"BulletOwnerAvatar")!=pawn_.object)return;
    seen_effects_.push_back({Id(root),primary_.object}); // looked at once, moved or not
    Call get(root,"GetAttachSocketName");const Name socket=get.Invoke()?get.Get<Name>("ReturnValue"):Name{};
    Object* host=socket.None()?nullptr:SocketHost(socket,false);Transform before;const bool known=TransformOf(root,before);
    Call attach(root,"K2_AttachToComponent");
    const bool ok=host&&attach.Set("Parent",host).Set("SocketName",socket).Set("LocationRule",uint8_t{0}).Set("RotationRule",uint8_t{0}).Set("ScaleRule",uint8_t{2})
        .Set("bWeldSimulatedBodies",false).Invoke()&&attach.Get<bool>("ReturnValue");
    if(++moved_effects_<=12)Logf("NPC_SKILL bullet %s %s socket=%s -> %s relative=(%.1f,%.1f,%.1f) scale=%.3f",ok?"moved":"left",Text(bullet).c_str(),Text(socket).c_str(),
        host?Text(host).c_str():"- (no such socket on the NPC body or a boss weapon)",known?before.translation[0]:0.0,known?before.translation[1]:0.0,known?before.translation[2]:0.0,known?before.scale[0]:0.0);
}
// ---- Boss chains ----------------------------------------------------------------------------------
// 火流星 (and P2) and 勾魂 P2 draw the chain from hand to blade with BP_ChainComponent: every tick it puts the ends of
// a spline at HandBoneName / WeaponBoneName of its Mesh and a Niagara chain follows the spline (the simulated middle
// chain through a Niagara particle callback). They are components of the boss actor wired up by its construction
// script (npc_chains.generated.h), so a skin showed the blades and no chain. While a skill runs each auto-activated
// chain is rebuilt on the hero: spline and Niagara components from the boss's templates (points, asset, placement)
// and a chain component with the template's settings and Mesh = the NPC display. Gone again at the skill's end.
struct ScsNode { Object* component=nullptr;Name parent,socket; };
ScsNode FindScsNode(Object* cls,const char* variable) {
    Object* generated=Class("/Script/Engine.BlueprintGeneratedClass");if(!variable||!*variable)return {};
    for(Object* c=cls;generated&&c&&ue::IsA(c,generated);c=ue::At<Object*>(c,ue::layout::kSuper)){
        Object* script=Property<Object*>(c,"SimpleConstructionScript");if(!ue::Alive(script))continue;
        const Array all=Property<Array>(script,"AllNodes");if(all.num<=0||all.num>256||!all.data)continue;
        for(int32_t i=0;i<all.num;++i){Object* n=static_cast<Object**>(all.data)[i];
            if(ue::Alive(n)&&ue::NameIs(Property<Name>(n,"InternalVariableName"),variable))
                return {Property<Object*>(n,"ComponentTemplate"),Property<Name>(n,"ParentComponentOrVariableName"),Property<Name>(n,"AttachToName")};}
    }
    return {};
}
// A component of the boss's character mesh hangs on the display (at its socket when the display has it), anything
// else on the pawn's root: both capsules stand where the character stands.
Object* ChainComponent(Object* pawn,const ScsNode& node,const char* class_path) {
    Object* cls=Class(class_path);if(!cls||!ue::Alive(node.component)||!ue::IsA(node.component,cls))return nullptr;
    const bool on_mesh=ue::NameIs(node.parent,"CharacterMesh0")||ue::NameIs(node.parent,"Mesh");
    Object* parent=on_mesh?display_.object:Property<Object*>(pawn,"RootComponent");const Name socket=on_mesh&&HasSocket(parent,node.socket)?node.socket:Name{};
    const Transform relative=TemplateRelative(node.component);Object* component=ue::Alive(parent)?Add(pawn,cls,relative):nullptr;if(!component)return nullptr;
    skill_chains_.push_back(Id(component)); // tracked first: RemoveSkillWeapons destroys it whatever fails below
    if(Object* asset=Property<Object*>(node.component,"Asset");ue::Alive(asset))Call(component,"SetAsset").Set("InAsset",asset).Invoke();
    if(!AttachFinish(pawn,component,parent,socket,relative))return nullptr;
    // The spline's points as the boss has them (local space); the chain moves its first and last one every tick.
    Call count(node.component,"GetNumberOfSplinePoints");const int32_t points=count.Invoke()?count.Get<int32_t>("ReturnValue"):0;
    if(points>=2&&points<=64&&Call(component,"ClearSplinePoints").Set("bUpdateSpline",false).Invoke())
        for(int32_t i=0;i<points;++i){Call at(node.component,"GetLocationAtSplinePoint");if(!at.Set("PointIndex",i).Set("CoordinateSpace",uint8_t{0}).Invoke())break;
            Call(component,"AddSplinePoint").Set("Position",at.Get<Vector>("ReturnValue")).Set("CoordinateSpace",uint8_t{0}).Set("bUpdateSpline",i+1==points).Invoke();}
    return component;
}
// The template's own blueprint variables that hold plain values (names, flags, numbers, vectors, arrays of them):
// its object references are the boss's components and its Initialized / Activated state starts fresh.
size_t CopyChainSettings(Object* from,Object* to) {
    Object* cls=ue::ClassOf(from);if(!cls||cls!=ue::ClassOf(to))return 0;size_t copied=0;
    const auto plain=[](Object* p){const uint64_t kind=FieldKind(p);
        if(kind==0x2000||kind==0x20000||kind==0x100||kind==0x100000000ull||kind==0x80||kind==0x40||kind==0x1000000000000ull)return true;
        if(kind!=0x100000)return false;const std::string s=Text(ue::At<Object*>(p,0x70));return s=="Vector"||s=="Rotator"||s=="Transform"||s=="Vector2D"||s=="LinearColor"||s=="Quat";};
    for(Object* p=ue::At<Object*>(cls,ue::layout::kChildProperties);p;p=ue::At<Object*>(p,ue::layout::kPropNext)){
        const Name name=ue::At<Name>(p,ue::layout::kPropName);if(ue::NameIs(name,"Initialized")||ue::NameIs(name,"Activated"))continue;
        const int32_t at=ue::At<int32_t>(p,ue::layout::kPropOffset),size=ue::At<int32_t>(p,ue::layout::kPropElementSize);if(at<=0||size<=0||size>256)continue;
        if(plain(p)){std::memcpy(reinterpret_cast<unsigned char*>(to)+at,reinterpret_cast<const unsigned char*>(from)+at,static_cast<size_t>(size));++copied;continue;}
        Object* inner=FieldKind(p)==0x200000?ue::At<Object*>(p,0x78):nullptr;if(!inner||!plain(inner)||!NativeHelpersMatch())continue;
        const Array& source=ue::At<Array>(from,at);Array& dest=ue::At<Array>(to,at);const int32_t item=ue::At<int32_t>(inner,ue::layout::kPropElementSize);
        if(dest.num||item<=0||source.num<0||source.num>256||(source.num&&!source.data))continue;
        for(int32_t i=0;i<source.num;++i)
            reinterpret_cast<int32_t(__fastcall*)(void*,const void*,const void*)>(ue::Base()+kArrayAddRva)(&dest,p,static_cast<const unsigned char*>(source.data)+static_cast<size_t>(i)*static_cast<size_t>(item));
        ++copied;
    }
    return copied;
}
void AddSkillChains(const Context& context) {
    if(selected_<0||static_cast<size_t>(selected_)>=std::size(kNpcSkillSets)||!Same(display_))return;
    const NpcSkillSet& set=kNpcSkillSets[selected_];if(!set.monster||!*set.monster)return;
    Object* monster=nullptr;size_t built=0,wanted=0;std::string names;
    for(const NpcChain& row:kNpcChains){
        if(std::strcmp(row.monster,set.monster)||!row.auto_activate)continue;++wanted;
        if(!monster&&!(monster=LoadClassPackage(set.monster)))break;
        const ScsNode chain=FindScsNode(monster,row.chain);
        Object* spline=ChainComponent(context.pawn,FindScsNode(monster,row.spline),"/Script/Engine.SplineComponent");
        Object* niagara=ChainComponent(context.pawn,FindScsNode(monster,row.niagara),"/Script/Niagara.NiagaraComponent");
        Object* simulation=*row.simulation?ChainComponent(context.pawn,FindScsNode(monster,row.simulation),"/Script/Niagara.NiagaraComponent"):nullptr;
        Object* component=ue::Alive(chain.component)&&spline&&niagara&&(!*row.simulation||simulation)?Add(context.pawn,ue::ClassOf(chain.component)):nullptr;
        if(!component){Logf("NPC_SKILL chain %s: template=%d spline=%d niagara=%d simulation=%d; not built",row.chain,ue::Alive(chain.component)?1:0,spline?1:0,niagara?1:0,simulation?1:0);continue;}
        skill_chains_.push_back(Id(component));
        // ConstructFunc before the component registers, as the boss's construction script runs before BeginPlay:
        // BeginPlay then finds it initialized and activates it.
        const size_t settings=CopyChainSettings(chain.component,component);Call construct(component,"ConstructFunc");
        const bool ok=construct.Set("Mesh",display_.object).Set("Spline",spline).Set("Niagara",niagara).Set("NiagaraSimulation",simulation).Set("AutoActivate",true).Invoke()&&
            Call(context.pawn,"FinishAddComponent").Set("Component",component).Set("bManualAttachment",true).Set("RelativeTransform",Transform{}).Invoke();
        if(ok)++built;
        char row_text[160]{};_snprintf_s(row_text,sizeof(row_text),_TRUNCATE,"%s%s:%zu%s",names.empty()?"":" ",row.chain,settings,ok?"":"(failed)");names+=row_text;
    }
    if(wanted)Logf("NPC_SKILL boss chains %zu/%zu on %s [settings copied: %s]",built,wanted,Text(display_.object).c_str(),names.empty()?"-":names.c_str());
}
void ShowSkillWeapons(const Context& context) {
    if(!skill_weapons_.empty()||!skill_colliders_.empty()||!stand_ins_.empty()||!skill_chains_.empty()||selected_<0||static_cast<size_t>(selected_)>=std::size(kNpcSkillSets)||!Same(display_))return;
    const NpcSkillSet& set=kNpcSkillSets[selected_];
    HoldHeroWeapon(true);
    HideHeroWeapons(); // also for an unarmed boss: its moves have no sword in the hand
    seen_effects_.clear();moved_effects_=0;unplaced_effects_=0; // effects already on the hero's mesh are not the skill's
    if(source_driver_mode_&&Same(primary_))for(Object* c:Children(primary_.object))if(IsEffect(c))seen_effects_.push_back({Id(c),primary_.object});
    boss_weapon_rows_.assign(set.weapon_count,Identity{});
    size_t shown=0;for(size_t i=0;i<set.weapon_count;++i)if(ShowSkillWeapon(context,display_.object,set.weapons[i])){++shown;boss_weapon_rows_[i]=skill_weapons_.back();}
    Logf("NPC_SKILL boss weapons shown=%zu/%zu heroWeaponsHidden=%zu",shown,set.weapon_count,hidden_weapons_.size());
    AddStandIns(context); // before the colliders: they go onto the stand-ins too
    AddSkillColliders(context);
    AddSkillChains(context);
    audit_.weapons_shown=shown;audit_.weapons_expected=set.weapon_count;audit_.colliders=skill_colliders_.size();audit_.stand_ins=stand_ins_.size();
}
void RemoveSkillWeapons(const char* why) {
    if(skill_route_until_.load(std::memory_order_relaxed)==~0ull)skill_route_until_.store(GetTickCount64()+kSkillRouteTailMs,std::memory_order_relaxed);
    audit_.effects_moved=(std::max)(audit_.effects_moved,moved_effects_);
    HoldHeroWeapon(false);audit_.weapon_swaps=(std::max)(audit_.weapon_swaps,weapon_hold_.restored);
    seen_effects_.clear();boss_weapon_rows_.clear();
    RemoveStandIns(); // first: no lookup may find a stand-in being torn down; their collider copies go with them
    if(!skill_chains_.empty()){ // chain components were added last of each row: gone before the Niagara / spline they drive
        size_t removed=0;for(auto it=skill_chains_.rbegin();it!=skill_chains_.rend();++it)if(!Same(*it)||Destroy(it->object))++removed;
        Logf("NPC_SKILL boss chain components removed=%zu/%zu",removed,skill_chains_.size());skill_chains_.clear();}
    if(skill_weapons_.empty()&&hidden_weapons_.empty()&&skill_colliders_.empty())return;
    const size_t colliders=skill_colliders_.size();
    skill_colliders_.erase(std::remove_if(skill_colliders_.begin(),skill_colliders_.end(),[](const Identity& c){return !Same(c)||Destroy(c.object);}),skill_colliders_.end());
    if(colliders)Logf("NPC_SKILL boss colliders removed=%zu/%zu effectsMoved=%zu",colliders-skill_colliders_.size(),colliders,moved_effects_);
    const size_t count=skill_weapons_.size();
    skill_weapons_.erase(std::remove_if(skill_weapons_.begin(),skill_weapons_.end(),[](const Identity& w){return !Same(w)||Destroy(w.object);}),skill_weapons_.end());
    Logf("NPC_SKILL boss weapons removed=%zu/%zu reason=%s",count-skill_weapons_.size(),count,why);RestoreHeroWeapons();
}
// The skill's spec in the ASC's ActivatableAbilities (layout from reflection): present, and its
// running activation count. Works for every instancing policy, unlike reading an instance.
bool SpecState(Object* asc,int32_t handle,bool& present,uint8_t& active) {
    present=false;active=0;
    Object* type=Class("/Script/GameplayAbilities.GameplayAbilitySpec"),*container=Class("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t specs=Offset(asc,"ActivatableAbilities"),items=container?ue::Offset(container,"Items"):-1,size=type?ue::StructSize(type):0,
        handle_at=type?ue::Offset(type,"Handle"):-1,count_at=type?ue::Offset(type,"ActiveCount"):-1;
    if(specs<0||items<0||size<=0||handle_at<0||count_at<0)return false;
    const Array array=ue::At<Array>(asc,specs+items);
    if(array.num<0||array.num>4096||array.max<array.num||(array.num&&!FaceReadable(array.data,static_cast<size_t>(array.num)*static_cast<size_t>(size))))return false;
    for(int32_t i=0;i<array.num;++i){Object* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*static_cast<size_t>(size);
        if(ue::At<int32_t>(row,handle_at)!=handle)continue;present=true;active=ue::At<uint8_t>(row,count_at);return true;}
    return true;
}
// What the notice calls a skill: the curated name (kNpcSkillNames), else the kind of move and its number
// from the ability's own name, GA_<boss>[_P2]_<kind>_<number...> ("P2 技能 21").
std::wstring SkillLabel(const std::string& ability) {
    for(const NpcSkillName& row:kNpcSkillNames)if(ability==row.ability)return row.name;
    static constexpr std::pair<const char*,const wchar_t*> kKinds[]={{"SpecialAttack",L"特殊攻击"},{"Special",L"特殊攻击"},{"CriticalArt",L"绝技"},{"Ultimate",L"奥义"},
        {"HeavyAttack",L"重击"},{"LightAttack",L"轻攻击"},{"MiddleAttack",L"中攻击"},{"DodgeAttack",L"闪避攻击"},{"CA",L"绝技"},{"Ca",L"绝技"},{"RangeAttack",L"远程攻击"},{"AutoCombo",L"连招"},{"Combo",L"连招"},{"SkillAttack",L"技能"},{"Skill",L"技能"},{"Attack",L"攻击"},{"Atk",L"攻击"},{"ZuiAtk",L"攻击"}};
    const auto wide=[](const std::string& text){return std::wstring(text.begin(),text.end());};
    std::vector<std::string> tokens;std::stringstream in(ability);for(std::string token;std::getline(in,token,'_');)if(!token.empty())tokens.push_back(token);
    if(!tokens.empty()&&tokens[0]=="GA")tokens.erase(tokens.begin());
    std::wstring label,phase;size_t kind=tokens.size();
    for(size_t i=0;i<tokens.size()&&kind==tokens.size();++i)for(const auto& [word,name]:kKinds)if(tokens[i]==word){kind=i;label=name;break;}
    if(kind==tokens.size()){for(size_t i=1;i<tokens.size();++i)label+=(label.empty()?L"":L" ")+wide(tokens[i]);return label.empty()?wide(ability):label;}
    for(size_t i=0;i<kind;++i){const std::string& t=tokens[i];if((t.size()==2&&t[0]=='P'&&t[1]>='0'&&t[1]<='9')||t.rfind("Phase",0)==0)phase=wide(t);}
    for(size_t i=kind+1;i<tokens.size();++i)if(tokens[i]!="NoCD"&&tokens[i]!="New")label+=L" "+wide(tokens[i]);
    return phase.empty()?label:phase+L" "+label;
}
// ---- Lock-on ------------------------------------------------------------------------------------
// R3 is also the game's lock-on toggle, so the L3 + R3 chord drops (or flips) the lock. A skill locks
// the nearest enemy the way the game's own action abilities do (BP_ActionAbilityBase): the
// CombatSoftLock targeting handler picks it (FindTargetByHandler) and the target struct goes to
// SetLockTarget as found; FindTarget is the fallback.
struct LockTarget { unsigned char bytes[16]{}; };
Object* TargetingComponent(Object* pawn) {
    if(!ue::Alive(pawn))return nullptr;
    Call get(pawn,"GetTargetingComponent","/Script/SGameCombatSystem.TargetingComponentHolder");
    Object* component=get.Invoke()?get.Get<Object*>("ReturnValue"):nullptr;return ue::Alive(component)?component:nullptr;
}
bool HasLock(Object* targeting) { Call has(targeting,"HasLockTarget");return has.Invoke()&&has.Get<bool>("ReturnValue"); }
// "kept" (already locked), "soft-lock" / "find" (locked now), else why not.
const char* LockNearestEnemy(Object* pawn) {
    Object* targeting=TargetingComponent(pawn);if(!targeting)return "no-targeting-component";
    if(HasLock(targeting))return "kept";
    static const Name soft=ue::MakeName(L"Targeting.Handler.CombatSoftLock");
    LockTarget target;const char* how=nullptr;
    if(!soft.None()){Call find(targeting,"FindTargetByHandler");
        if(find.Set("HandlerTag",soft).Set("UseLockTarget",false).Invoke()&&find.Get<bool>("ReturnValue")){target=find.Get<LockTarget>("Target");how="soft-lock";}}
    if(!how){Call find(targeting,"FindTarget");if(find.Invoke()&&find.Get<bool>("ReturnValue")){target=find.Get<LockTarget>("Target");how="find";}}
    if(!how)return "no-enemy-found";
    Call set(targeting,"SetLockTarget");
    if(!set.Set("Target",target).Invoke())return "set-call-failed";
    return set.Get<bool>("ReturnValue")?how:"set-refused";
}
void HoldSkillLock() {
    if(skill_.handle<=0||GetTickCount64()>=skill_.relock_until||!Same(pawn_))return;
    Object* targeting=TargetingComponent(pawn_.object);if(!targeting||HasLock(targeting))return;
    const char* how=LockNearestEnemy(pawn_.object);
    Logf("NPC_SKILL lock lost %llums into %s; relock=%s",GetTickCount64()-skill_.started,skill_.name.c_str(),how);
    if(++skill_.relocks>=kSkillRelocksMax)skill_.relock_until=0;
}
// ---- Casting ------------------------------------------------------------------------------------
// The round's skill, granted and kept in pending_ until it starts. false: it cannot be cast at all.
bool GrantSkill(Object* asc,const NpcSkillSet& set,size_t pick,size_t tries) {
    const std::string package=set.skills[pick],name=package.substr(package.find_last_of('/')+1);
    const ULONGLONG t0=GetTickCount64();Object* cls=LoadClassPackage(package);const ULONGLONG load_ms=GetTickCount64()-t0;
    if(!cls){Logf("NPC_SKILL load failed ability=%s",package.c_str());return false;}
    const bool pinned=PinClass(cls);
    Call give(asc,"K2_GiveAbility");
    if(!give.Set("AbilityClass",cls).Set("Level",int32_t{0}).Set("InputID",int32_t{-1}).Invoke()||give.Get<int32_t>("ReturnValue")<=0){
        Logf("NPC_SKILL grant failed ability=%s asc=%s",name.c_str(),Text(asc).c_str());return false;}
    pending_={};pending_.asc=Id(asc);pending_.ability_class=Id(cls);pending_.handle=give.Get<int32_t>("ReturnValue");pending_.pick=pick;pending_.tries=tries;
    pending_.since=GetTickCount64();pending_.until=pending_.since+kSkillWaitMs;pending_.next_try=pending_.since+kSkillRetryMs;pending_.load_ms=load_ms;pending_.name=name;pending_.pinned=pinned;
    return true;
}
void DropPendingSkill(const char* why) {
    if(pending_.handle<=0){pending_={};return;}
    bool cleared=false;
    if(Same(pending_.asc)){bool instance=false;if(Object* ability=SpecAbility(pending_.asc.object,pending_.handle,instance))LogActivationRefusal(pending_.asc.object,ability,pending_.name);
        cleared=Call(pending_.asc.object,"ClearAbility").Set("Handle",pending_.handle).Invoke();}
    Logf("NPC_SKILL not activated ability=%s handle=%d waitedMs=%llu cleared=%d reason=%s; skipped for the rest of this round",pending_.name.c_str(),pending_.handle,
        GetTickCount64()-pending_.since,cleared?1:0,why);
    if(Auditing()&&audit_.result.empty())audit_.result=std::string("refused-")+why;
    pending_={};
}
// One activation attempt of pending_, locked on the nearest enemy first. true: it started and is skill_ now.
bool TryPendingSkill(const Context& context) {
    const char* lock=LockNearestEnemy(context.pawn);
    Object* asc=pending_.asc.object;Call activate(asc,"TryActivateAbility");
    if(!activate.Set("AbilityToActivate",pending_.handle).Set("bAllowRemoteActivation",true).Invoke()||!activate.Get<bool>("ReturnValue"))return false;
    // The spec leaves the ASC when the ability ends (GAS RemoveAfterActivation); WatchSkill only observes.
    const bool remove_on_end=Call(asc,"SetRemoveAbilityOnEnd").Set("AbilitySpecHandle",pending_.handle).Invoke();
    const PendingCast cast=pending_;pending_={};
    skill_={};skill_.asc=cast.asc;skill_.ability_class=cast.ability_class;skill_.handle=cast.handle;skill_.started=GetTickCount64();skill_.name=cast.name;
    skill_.relock_until=skill_.started+kSkillRelockMs;
    bool present=false;uint8_t active=0;if(SpecState(asc,skill_.handle,present,active)&&active)skill_.seen_active=true;
    ShowSkillWeapons(context);skill_route_until_.store(~0ull,std::memory_order_relaxed);
    const NpcSkillSet& set=kNpcSkillSets[selected_];const Profile& profile=kProfiles[selected_];
    const size_t order=set.skill_count-skill_round_.Left(set.skill_count);const std::wstring label=SkillLabel(cast.name);
    Object* montage=nullptr;if(Object* anim=AnimInstance(context.mesh)){Call current(anim,"GetCurrentActiveMontage");if(current.Invoke())montage=current.Get<Object*>("ReturnValue");}
    Logf("NPC_SKILL cast profile=%s monster=%s ability=%s skill=%zu round=%zu/%zu handle=%d loadMs=%llu waitedMs=%llu pinned=%d removeOnEnd=%d lock=%s heroMontage=%s triesThisPress=%zu",
        set.profile,set.monster,cast.name.c_str(),cast.pick+1,order,set.skill_count,cast.handle,cast.load_ms,skill_.started-cast.since,cast.pinned?1:0,remove_on_end?1:0,lock,Text(montage).c_str(),cast.tries);
    Status(L"施放 %s 技能（本轮 %zu/%zu）：%s",profile.name,order,set.skill_count,label.c_str());
    wchar_t detail[160]{};_snwprintf_s(detail,_countof(detail),_TRUNCATE,L"%s · 本轮 %zu/%zu",profile.name,order,set.skill_count);SkillToast::Show(label,detail);
    return true;
}
// The round's next skills until one starts or waits for the hero (at most kSkillTriesPerPress per press).
void CastNext(const Context& context,Object* asc,size_t tries) {
    const NpcSkillSet& set=kNpcSkillSets[selected_];const Profile& profile=kProfiles[selected_];
    while(tries<kSkillTriesPerPress&&tries<set.skill_count){
        const size_t pick=skill_round_.Next(set.skill_count);++tries;
        if(!GrantSkill(asc,set,pick,tries))continue;
        if(TryPendingSkill(context))return;
        Logf("NPC_SKILL waiting ability=%s handle=%d: not startable yet, retried every %llums for %llums",pending_.name.c_str(),pending_.handle,kSkillRetryMs,kSkillWaitMs);
        Status(L"技能 %s 等主角收招后施放",SkillLabel(pending_.name).c_str());return;
    }
    Status(L"%s 暂时无法施放技能，稍后再按",profile.name);Logf("NPC_SKILL none started profile=%s tries=%zu",set.profile,tries);
}
void WatchSkill();
// One press: the round's next skill (NpcBodyCollision::SkillRound), on the nearest enemy.
void CastSkill() {
    if(!active_||cleanup_pending_||!source_driver_mode_||selected_<0||static_cast<size_t>(selected_)>=std::size(kNpcSkillSets))return;
    const NpcSkillSet& set=kNpcSkillSets[selected_];const Profile& profile=kProfiles[selected_];
    if(!set.skill_count){Status(L"%s 没有可施放的战斗技能：游戏里没有它的战斗蓝图",profile.name);Logf("NPC_SKILL none profile=%s (no combat blueprint)",set.profile);return;}
    WatchSkill(); // the last skill may have ended since the 250 ms maintenance pass
    if(skill_.handle>0){Status(L"技能 %s 仍在施放，结束后再按 %s",SkillLabel(skill_.name).c_str(),UiHotkeys::Hint(UiHotkeys::Action::NpcSkill).c_str());Logf("NPC_SKILL busy ability=%s elapsedMs=%llu",skill_.name.c_str(),GetTickCount64()-skill_.started);return;}
    if(pending_.handle>0){Status(L"技能 %s 等主角收招后施放",SkillLabel(pending_.name).c_str());return;}
    Context context;Object* asc=nullptr;
    if(!ContextNow(context)||context.pawn!=pawn_.object||!ue::Alive(asc=PawnAsc(context.pawn))){Logf("NPC_SKILL refused: player / ability system not ready");return;}
    CastNext(context,asc,0);
}
// Every frame while a refused skill waits: try again, and after kSkillWaitMs move on to the round's next.
void RetryPendingSkill() {
    if(pending_.handle<=0)return;
    const ULONGLONG now=GetTickCount64();if(now<pending_.next_try)return;pending_.next_try=now+kSkillRetryMs;
    Context context;
    if(!active_||cleanup_pending_||selected_<0||static_cast<size_t>(selected_)>=std::size(kNpcSkillSets)||!Same(pending_.asc)||!ContextNow(context)||context.pawn!=pawn_.object){
        DropPendingSkill("skin-or-player-changed");return;}
    if(TryPendingSkill(context)||now<pending_.until)return;
    Object* asc=pending_.asc.object;const size_t tries=pending_.tries;const bool exact=pending_.exact;DropPendingSkill("wait-expired");
    if(!exact)CastNext(context,asc,tries);
}
void ClearSkill(const char* why) {
    DropPendingSkill(why);
    if(skill_.handle<=0){RemoveSkillWeapons(why);return;}
    bool present=false;uint8_t active=0;
    if(Same(skill_.asc)&&SpecState(skill_.asc.object,skill_.handle,present,active)&&present){
        if(active)Call(skill_.asc.object,"CancelAbilityHandle").Set("AbilityHandle",skill_.handle).Invoke();
        if(SpecState(skill_.asc.object,skill_.handle,present,active)&&present)Call(skill_.asc.object,"ClearAbility").Set("Handle",skill_.handle).Invoke();
        SpecState(skill_.asc.object,skill_.handle,present,active);}
    Logf("NPC_SKILL stopped reason=%s ability=%s elapsedMs=%llu specLeft=%d",why,skill_.name.c_str(),GetTickCount64()-skill_.started,present?1:0);skill_={};
    RemoveSkillWeapons(why);
}
void WatchSkill() {
    if(skill_.handle<=0)return;
    if(!Same(skill_.asc)){Logf("NPC_SKILL ended ability=%s: ability system gone",skill_.name.c_str());skill_={};RemoveSkillWeapons("ability-system-gone");return;}
    const ULONGLONG ms=GetTickCount64()-skill_.started;bool present=false;uint8_t active=0;
    if(!SpecState(skill_.asc.object,skill_.handle,present,active)){if(ms>kSkillTimeoutMs)ClearSkill("timeout-spec-unreadable");return;}
    if(!present){Logf("NPC_SKILL ended ability=%s ms=%llu sawActive=%d",skill_.name.c_str(),ms,skill_.seen_active?1:0);skill_={};RemoveSkillWeapons("ended");return;}
    if(active)skill_.seen_active=true;
    // The spec is left behind when SetRemoveAbilityOnEnd is unavailable: cleared as soon as the skill has
    // run and stopped (or never ran within 500 ms), so the next press is free right away.
    if((!active&&(skill_.seen_active||ms>500))||ms>kSkillTimeoutMs)ClearSkill(active?"timeout":"ended-spec-left");
}
// ---- Skill damage -------------------------------------------------------------------------------
// The boss skills deal the boss's own damage (tuned against the hero). ScaleSkillDamage watches the health of
// the hostile pawns around the hero while one runs and until kSkillRouteTailMs after it (projectiles, lingering
// hits), unless the hero starts a montage the skill never played; each drop d is topped up to skill_damage_ x d
// by taking d * (x - 1) more, at most what is left, through GrabExtras::ChangeHealth (the game's attribute call).
// Health is read in place: PBZCharacter.AttributeSetBase -> BasicAttributeSet.Health.CurrentValue (as AirSkills).
struct DamageTarget { Identity pawn,set;int32_t offset=-1;float last=0; };
struct DamageWindow {
    bool open=false;ULONGLONG skill_started=0,until=0,next_scan=0;std::string skill;float scale=1;
    std::vector<DamageTarget> targets;std::vector<Object*> montages;float native=0,extra=0;uint32_t hits=0,refused=0;
};
DamageWindow damage_;
constexpr ULONGLONG kSkillDamageScanMs=500;constexpr double kSkillDamageRangeCm=6000;constexpr size_t kSkillDamageTargetsMax=64,kSkillDamageHitLogs=8;
bool HealthNow(const DamageTarget& target,float& value) {
    if(target.offset<0||!Same(target.pawn)||!Same(target.set)||!FaceReadable(target.set.object+target.offset,sizeof(float)))return false;
    value=ue::At<float>(target.set.object,target.offset);return std::isfinite(value)&&value>=0&&value<1e9f;
}
// A living AI pawn the hero's team is hostile to (SGameAIController team matrix), with a readable health.
bool DamageCandidate(Object* pawn,Object* hero,DamageTarget& target) {
    if(!ue::Alive(pawn)||pawn==hero||ue::IsTemplate(pawn))return false;
    Object* controller=Property<Object*>(pawn,"Controller"),*ai=Class("/Script/SGameAI.SGameAIController");
    if(!ue::Alive(controller)||!ai||!ue::IsA(controller,ai))return false;
    Object* teams=ue::Default("/Script/SGameAI.Default__SGameAIController");Call own(teams,"GetTeam"),other(teams,"GetTeam");
    if(!own.Set("Actor",hero).Invoke()||!other.Set("Actor",pawn).Invoke())return false;
    Call attitude(teams,"GetDefaultTeamAttitude");
    if(!attitude.Set("Team1",other.Get<uint8_t>("ReturnValue")).Set("Team2",own.Get<uint8_t>("ReturnValue")).Invoke()||attitude.Get<uint8_t>("ReturnValue")!=2)return false; // Hostile
    Call dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");
    if(!dead.Set("Pawn",pawn).Invoke()||dead.Get<bool>("ReturnValue"))return false;
    Object* set=Property<Object*>(pawn,"AttributeSetBase"),*basic=Class("/Script/CharacterBasics.BasicAttributeSet"),*data=Class("/Script/GameplayAbilities.GameplayAttributeData");
    const int32_t health=Offset(set,"Health");
    if(!ue::Alive(set)||!basic||!ue::IsA(set,basic)||health<0||!data||ue::StructSize(data)!=16||ue::Offset(data,"CurrentValue")!=12)return false;
    target={};target.pawn=Id(pawn);target.set=Id(set);target.offset=health+12;
    return HealthNow(target,target.last)&&target.last>0;
}
void ScanDamageTargets(Object* hero) {
    Vector at{};{Call where(hero,"K2_GetActorLocation");if(!where.Invoke())return;at=where.Get<Vector>("ReturnValue");}
    Call actors(ue::Default("/Script/Engine.Default__GameplayStatics"),"GetAllActorsOfClass");
    if(!actors.Set("WorldContextObject",hero).Set("ActorClass",Class("/Script/Engine.Pawn")).Invoke())return;
    const Array list=actors.Get<Array>("OutActors");
    if(list.data&&list.num>0&&list.num<=2048&&FaceReadable(list.data,static_cast<size_t>(list.num)*sizeof(Object*)))
        for(int32_t i=0;i<list.num&&damage_.targets.size()<kSkillDamageTargetsMax;++i){
            Object* pawn=static_cast<Object**>(list.data)[i];
            if(!ue::Alive(pawn)||pawn==hero||std::any_of(damage_.targets.begin(),damage_.targets.end(),[&](const DamageTarget& t){return t.pawn.object==pawn;}))continue;
            Call where(pawn,"K2_GetActorLocation");if(!where.Invoke())continue;
            const Vector p=where.Get<Vector>("ReturnValue");const double dx=p.x-at.x,dy=p.y-at.y,dz=p.z-at.z;
            if(!(dx*dx+dy*dy+dz*dz<=kSkillDamageRangeCm*kSkillDamageRangeCm))continue;
            DamageTarget target;if(DamageCandidate(pawn,hero,target))damage_.targets.push_back(target);
        }
    ue::Free(list.data);
}
void CloseDamageWindow(const char* why) {
    if(!damage_.open)return;
    Logf("NPC_SKILL damage ability=%s x%.2f targets=%zu hits=%u native=%.0f extra=%.0f refused=%u closed=%s",damage_.skill.c_str(),damage_.scale,
        damage_.targets.size(),damage_.hits,damage_.native,damage_.extra,damage_.refused,why);
    if(Auditing()){audit_.hits+=damage_.hits;audit_.native+=damage_.native;audit_.targets=(std::max)(audit_.targets,static_cast<uint32_t>(damage_.targets.size()));}
    damage_={};
}
void ScaleSkillDamage() {
    const ULONGLONG now=GetTickCount64();const bool running=active_&&skill_.handle>0;
    if(running&&damage_.open&&damage_.skill_started!=skill_.started)CloseDamageWindow("next-skill");
    if(running&&!damage_.open){damage_.open=true;damage_.skill_started=skill_.started;damage_.skill=skill_.name;}
    if(!damage_.open)return;
    const bool foreign=foreign_montage_&&std::find(damage_.montages.begin(),damage_.montages.end(),foreign_montage_)==damage_.montages.end();
    if(running){damage_.until=now+static_cast<ULONGLONG>(double(kSkillRouteTailMs)/(std::max)(1.0f,audit_speed_));if(foreign)damage_.montages.push_back(foreign_montage_);}
    else if(!active_||now>=damage_.until||foreign||!Same(pawn_)){CloseDamageWindow(!active_?"skin-off":foreign?"hero-montage":now>=damage_.until?"tail-over":"hero-gone");return;}
    damage_.scale=Auditing()?1.0f:std::clamp(skill_damage_.load(std::memory_order_relaxed),kSkillDamageMin,kSkillDamageMax);
    if(now>=damage_.next_scan&&Same(pawn_)){damage_.next_scan=now+kSkillDamageScanMs;ScanDamageTargets(pawn_.object);}
    const bool apply=damage_.scale>1.001f&&!GrabExtras::Active(); // a grab measures and scales its own damage
    for(size_t i=0;i<damage_.targets.size();){
        DamageTarget& target=damage_.targets[i];float health=0;
        if(!HealthNow(target,health)){damage_.targets.erase(damage_.targets.begin()+static_cast<std::ptrdiff_t>(i));continue;}
        const float drop=target.last-health;
        if(drop>0.01f){
            damage_.native+=drop;float extra=0;
            if(Auditing()&&target.pawn.object==audit_.dummy.object){++audit_.dummy_hits;audit_.dummy_damage+=drop;}
            if(apply&&health>0){
                const float want=std::min(drop*(damage_.scale-1),health);float after=health;
                if(GrabExtras::ChangeHealth(target.pawn.object,-want)&&HealthNow(target,after)){extra=health-after;health=after;}
                if(extra<=0.01f)++damage_.refused;}
            damage_.extra+=std::max(extra,0.0f);
            if(++damage_.hits<=kSkillDamageHitLogs)
                Logf("NPC_SKILL damage hit ability=%s target=%s native=%.1f extra=%.1f left=%.0f x%.2f%s",damage_.skill.c_str(),Text(target.pawn.object).c_str(),
                    drop,extra,health,damage_.scale,apply?"":GrabExtras::Active()?" (grab owns it)":" (x1)");}
        target.last=health;++i;
    }
}
// ---- Boss-only skill calls ----------------------------------------------------------------------
// A boss skill that casts its avatar to the boss blueprint and calls into it does nothing of that on the
// hero: the cast fails. Of 真龙之躯's (ZuoShang P2) skills only GA_ZuoShang_P2_Skill_23 does so
// (analysis/pbz_bossrush_20261030/npc_skill_fx_20261103: scan_p2.py, decode.py, timeline.py). Its Main
// timeline sends GameplayEvent.ZuoShang.DragonPillar.Prepare at 0.681 s and ...Throw at 2.015 s, and the
// ability answers with BP_Monster_ZuoShang_P2.PrepareDragonPillar(self) / ThrowDragonPillar(player, 1 s):
//   Prepare: at = actor transform * DragonPillarSpawnOffset; yaw = look-at yaw from there to the target;
//            bullet = DragonPillarData.CreateBullet(Transform(at, (DragonPillarInitialTilt, yaw, 0)), self);
//            bullet.Initialize(null, self, ability, DragonPillarData, false); bullet.BeginPrepareMotion()
//   Throw:   bullet.LaunchTowardPoint(target location, 1 s)
//   Cancel:  bullet.StopPillarFlight(); bullet.Deactivate(0.1)
// DriveDragonPillar replays them for the hero at the same skill montage positions, with the boss's values read
// from its class default object and the nearest hostile pawn as the target (the boss aims at the player).
// The bullet's native parameters are filled by position (bytecode carries no names), sizes checked.
// Initialize and Deactivate are BulletInterface functions (the bytecode calls them through an interface
// context, 0x51): they are not on the bullet class, so they are looked up on the interface.
Object* DefaultObject(Object* cls);
Object* LoadAssetPath(const char* path);
struct BossCall { const char* ability; const char* montage; float prepare_at, throw_at, flight_seconds; };
constexpr BossCall kDragonPillar{"GA_ZuoShang_P2_Skill_23","ZuoShang_LongTi_Skill_23_Montage",0.681f,2.015f,1.0f};
struct PillarState { int32_t handle=0;Identity bullet;bool prepared=false,thrown=false; };
PillarState pillar_;
struct alignas(16) Transform96 { unsigned char bytes[96]{}; };
struct RotatorD { double pitch=0,yaw=0,roll=0; };
struct Param { std::string name;int32_t offset=-1,size=0; };
constexpr char kBulletInterface[]="/Script/SGameCombatSystem.BulletInterface";
// A UFunction's parameters in declaration order (its ChildProperties), the return value among them; a
// function the class lacks is looked up on interface_path.
std::vector<Param> Params(Object* object,const char* function,const char* interface_path=nullptr) {
    std::vector<Param> out;Object* fn=ue::Alive(object)?ue::FindFunction(ue::ClassOf(object),function):nullptr;
    if(!fn&&interface_path)if(Object* face=Class(interface_path))fn=ue::FindFunction(face,function);if(!fn)return out;
    uintptr_t field=ue::At<uintptr_t>(fn,ue::layout::kChildProperties);
    for(size_t guard=0;field&&guard<32&&FaceReadable(reinterpret_cast<void*>(field),0x48);++guard,field=ue::At<uintptr_t>(reinterpret_cast<void*>(field),ue::layout::kPropNext))
        out.push_back({Text(ue::At<Name>(reinterpret_cast<void*>(field),ue::layout::kPropName)),ue::At<int32_t>(reinterpret_cast<void*>(field),ue::layout::kPropOffset),
            ue::At<int32_t>(reinterpret_cast<void*>(field),ue::layout::kPropElementSize)});
    return out;
}
std::string ParamText(const std::vector<Param>& params) {
    std::string text;for(const Param& p:params)text+=(text.empty()?"":",")+p.name+":"+std::to_string(p.size);return text.empty()?"-":text;
}
// A float or double property (Blueprint reals are doubles).
bool Real(Object* object,const char* property,double& value) {
    const uintptr_t field=object?Field(ue::ClassOf(object),property):0;if(!field)return false;
    const int32_t size=ue::At<int32_t>(reinterpret_cast<void*>(field),ue::layout::kPropElementSize),offset=ue::At<int32_t>(reinterpret_cast<void*>(field),ue::layout::kPropOffset);
    if(size==8)value=ue::At<double>(object,offset);else if(size==4)value=ue::At<float>(object,offset);else return false;return std::isfinite(value);
}
bool NearestEnemy(Object* hero,Vector& at) {
    Vector self{};{Call where(hero,"K2_GetActorLocation");if(!where.Invoke())return false;self=where.Get<Vector>("ReturnValue");}
    Call actors(ue::Default("/Script/Engine.Default__GameplayStatics"),"GetAllActorsOfClass");
    if(!actors.Set("WorldContextObject",hero).Set("ActorClass",Class("/Script/Engine.Pawn")).Invoke())return false;
    const Array list=actors.Get<Array>("OutActors");double best=kSkillDamageRangeCm*kSkillDamageRangeCm;bool found=false;
    if(list.data&&list.num>0&&list.num<=2048&&FaceReadable(list.data,static_cast<size_t>(list.num)*sizeof(Object*)))
        for(int32_t i=0;i<list.num;++i){Object* pawn=static_cast<Object**>(list.data)[i];if(!ue::Alive(pawn)||pawn==hero)continue;
            Call where(pawn,"K2_GetActorLocation");if(!where.Invoke())continue;const Vector p=where.Get<Vector>("ReturnValue");
            const double d=(p.x-self.x)*(p.x-self.x)+(p.y-self.y)*(p.y-self.y)+(p.z-self.z)*(p.z-self.z);DamageTarget probe;
            if(d<best&&DamageCandidate(pawn,hero,probe)){best=d;at=p;found=true;}}
    ue::Free(list.data);return found;
}
// Where the pillar flies: the nearest hostile pawn, else 15 m ahead of the hero.
Vector PillarTarget(Object* hero) {
    Vector at{};if(NearestEnemy(hero,at))return at;
    Vector self{},ahead{};{Call where(hero,"K2_GetActorLocation");if(where.Invoke())self=where.Get<Vector>("ReturnValue");}
    {Call forward(hero,"GetActorForwardVector");if(forward.Invoke())ahead=forward.Get<Vector>("ReturnValue");}
    return Vector{self.x+ahead.x*1500,self.y+ahead.y*1500,self.z+ahead.z*1500};
}
void PreparePillar(Object* hero,Object* asc) {
    const NpcSkillSet& set=kNpcSkillSets[selected_];const std::string monster=set.monster?set.monster:"";
    Object* cls=monster.empty()?nullptr:LoadClassPackage(monster);const std::string leaf=monster.substr(monster.find_last_of('/')+1);
    Object* cdo=cls?Class((monster+".Default__"+leaf+"_C").c_str()):nullptr;if(cls&&!cdo)cdo=DefaultObject(cls);
    Object* data=cdo?Property<Object*>(cdo,"DragonPillarData"):nullptr;double tilt=0;const Vector offset=cdo?Property<Vector>(cdo,"DragonPillarSpawnOffset"):Vector{};
    if(!ue::Alive(data)||!Real(cdo,"DragonPillarInitialTilt",tilt)){Logf("NPC_SKILL pillar skipped: %s has no DragonPillarData / DragonPillarInitialTilt (cdo=%s)",leaf.c_str(),Text(cdo).c_str());return;}
    const Vector target=PillarTarget(hero);Transform96 actor{};{Call get(hero,"GetTransform");if(!get.Invoke())return;actor=get.Get<Transform96>("ReturnValue");}
    Call place(Math(),"TransformLocation");if(!place.Set("T",actor).Set("Location",offset).Invoke())return;const Vector at=place.Get<Vector>("ReturnValue");
    Call look(Math(),"FindLookAtRotation");if(!look.Set("Start",at).Set("Target",target).Invoke())return;
    RotatorD turn;turn.pitch=tilt;turn.yaw=look.Get<RotatorD>("ReturnValue").yaw;
    Call make(Math(),"MakeTransform");if(!make.Set("Location",at).Set("Rotation",turn).Set("Scale",Vector{1,1,1}).Invoke())return;const Transform96 spawn=make.Get<Transform96>("ReturnValue");
    const std::vector<Param> create=Params(data,"CreateBullet");
    if(create.size()<3||create[0].size!=96||create[1].size!=8){Logf("NPC_SKILL pillar skipped: CreateBullet params=%s",ParamText(create).c_str());return;}
    Call spawn_call(data,"CreateBullet");if(!spawn_call.Set(create[0].name.c_str(),spawn).Set(create[1].name.c_str(),hero).Invoke())return;
    Object* bullet=spawn_call.Get<Object*>("ReturnValue");if(!ue::Alive(bullet)){Logf("NPC_SKILL pillar CreateBullet returned no bullet (data=%s)",Text(data).c_str());return;}
    bool instance=false;Object* ability=SpecAbility(asc,skill_.handle,instance);
    const std::vector<Param> init=Params(bullet,"Initialize",kBulletInterface);bool initialized=false;
    if(init.size()>=5&&init[0].size==16&&init[1].size==8&&init[2].size==8&&init[3].size==8&&init[4].size==1){
        Call start(bullet,"Initialize",kBulletInterface);initialized=start.Set(init[1].name.c_str(),hero).Set(init[2].name.c_str(),ability).Set(init[3].name.c_str(),data).Set(init[4].name.c_str(),false).Invoke();}
    if(!initialized){Logf("NPC_SKILL pillar Initialize params=%s refused; bullet destroyed",ParamText(init).c_str());Call(bullet,"K2_DestroyActor").Invoke();return;}
    const bool motion=Call(bullet,"BeginPrepareMotion").Invoke();pillar_.bullet=Id(bullet);
    Logf("NPC_SKILL pillar prepared bullet=%s data=%s at=(%.0f,%.0f,%.0f) offset=(%.0f,%.0f,%.0f) tilt=%.1f yaw=%.1f target=(%.0f,%.0f,%.0f) ability=%s instance=%d motion=%d createParams=%s initParams=%s",
        Text(bullet).c_str(),Text(data).c_str(),at.x,at.y,at.z,offset.x,offset.y,offset.z,tilt,turn.yaw,target.x,target.y,target.z,Text(ability).c_str(),instance?1:0,motion?1:0,ParamText(create).c_str(),ParamText(init).c_str());
}
void ThrowPillar(Object* hero) {
    if(!Same(pillar_.bullet))return;Object* bullet=pillar_.bullet.object;const Vector target=PillarTarget(hero);
    const std::vector<Param> launch=Params(bullet,"LaunchTowardPoint");bool ok=false;
    if(launch.size()>=2&&launch[0].size==24&&(launch[1].size==4||launch[1].size==8)){Call fly(bullet,"LaunchTowardPoint");fly.Set(launch[0].name.c_str(),target);
        if(launch[1].size==8)fly.Set(launch[1].name.c_str(),static_cast<double>(kDragonPillar.flight_seconds));else fly.Set(launch[1].name.c_str(),kDragonPillar.flight_seconds);ok=fly.Invoke();}
    Logf("NPC_SKILL pillar thrown=%d target=(%.0f,%.0f,%.0f) params=%s",ok?1:0,target.x,target.y,target.z,ParamText(launch).c_str());
}
void CancelPillar(const char* why) {
    if(Same(pillar_.bullet)){Object* bullet=pillar_.bullet.object;Call(bullet,"StopPillarFlight").Invoke();const std::vector<Param> off=Params(bullet,"Deactivate",kBulletInterface);Call stop(bullet,"Deactivate",kBulletInterface);
        if(!off.empty()&&off[0].size==8)stop.Set(off[0].name.c_str(),0.1);else if(!off.empty()&&off[0].size==4)stop.Set(off[0].name.c_str(),0.1f);
        Logf("NPC_SKILL pillar cancelled reason=%s deactivated=%d",why,stop.Invoke()?1:0);}
    pillar_={};
}
void DriveDragonPillar() {
    const bool mine=active_&&skill_.handle>0&&skill_.name==kDragonPillar.ability&&Same(pawn_)&&Same(skill_.asc);
    if(!mine){if(pillar_.handle){if(pillar_.prepared&&!pillar_.thrown)CancelPillar(active_?"skill-ended":"skin-off");else pillar_={};}return;}
    if(pillar_.handle!=skill_.handle){if(pillar_.prepared&&!pillar_.thrown)CancelPillar("next-skill");pillar_={};pillar_.handle=skill_.handle;}
    if(pillar_.thrown||!foreign_montage_||!ue::Alive(foreign_montage_)||Text(foreign_montage_)!=kDragonPillar.montage)return;
    Object* anim=AnimInstance(primary_.object);if(!anim)return;Call at(anim,"Montage_GetPosition");if(!at.Set("Montage",foreign_montage_).Invoke())return;
    const float position=at.Get<float>("ReturnValue");
    if(!pillar_.prepared&&position>=kDragonPillar.prepare_at){pillar_.prepared=true;PreparePillar(pawn_.object,skill_.asc.object);}
    if(pillar_.prepared&&!pillar_.thrown&&position>=kDragonPillar.throw_at){pillar_.thrown=true;ThrowPillar(pawn_.object);}
}
// ---- Boss-only calls found by the full scan --------------------------------------------------------
// scan_all.py (same folder) decodes the blueprint closure of every skin skill (1713) and lists the code that only
// works on the boss: casts to the boss blueprint and calls into its post-process AnimBP. Besides the dragon pillar
// these are replayed:
//   weapon trace: 55 skills of 左殇, 白赤练, 火流星, 断肠, 烟千岁, 梦华圣母, 虚空      RedirectWeaponTrace
//   魔渊 skill 06: the pillar on the body                                            DriveSkillPillar
//   魔渊 skill 08: the pillar group behind the caster                                PlacePillarGroup
//   雷当 special attack 03, 左殇 skill 20: the counter flash                          DriveCounterFlash
// Not replayed: arena actors (火流星 P2 safe zone, 白赤练 P2 wires), 火流星's chains (components of the boss actor),
// 左殇 CA_02 / CA_03 QTE mesh scale, the boss's head / body look-at toggles, 火流星 P2 BEd_01 (the victim's side).
Object* Niagara() { return ue::Default("/Script/Niagara.Default__NiagaraFunctionLibrary"); }
// A Niagara system on a display socket: not pooled (it may be destroyed here) and never pre-culled.
Object* SpawnEffectAttached(const char* path,Object* parent,Name socket,const Vector& at,const RotatorD& turn,uint8_t location_type) {
    Object* system=LoadAssetPath(path);if(!ue::Alive(system)||!ue::Alive(parent))return nullptr;Call spawn(Niagara(),"SpawnSystemAttached");
    if(!spawn.Set("SystemTemplate",system).Set("AttachToComponent",parent).Set("AttachPointName",socket).Set("Location",at).Set("Rotation",turn).Set("LocationType",location_type)
        .Set("bAutoDestroy",true).Set("bAutoActivate",true).Set("PoolingMethod",uint8_t{0}).Set("bPreCullCheck",false).Invoke())return nullptr;
    Object* component=spawn.Get<Object*>("ReturnValue");return ue::Alive(component)?component:nullptr;
}
bool IsBlueprint(Object* object,const char* class_name) {
    for(Object* cls=ue::Alive(object)?ue::ClassOf(object):nullptr;cls;cls=ue::At<Object*>(cls,ue::layout::kSuper))if(ue::NameIs(ue::NameOf(cls),class_name))return true;
    return false;
}
// Weapon trace: GA_WeaponTraceAttack_Base keeps PP = the avatar's socket mesh's post-process instance (set on
// activation), sends BPI_EnemyWeaponTrace.SetTraceTarget(alpha R / L / custom from its curves, the target's
// location) to PP every tick and zeros it at the end. The boss's post-process AnimBP implements the interface (its
// weapon bones bend toward the target); the hero's does not, so on a skin nothing moved. Every running instance
// (the skill itself or a move its combo tree starts) gets the display's post-process instance as PP, and the
// ability's own tick drives the boss's weapon. A skin taken off mid-skill hands the old PP back.
struct TracedAbility { Identity ability,original;Object* pp=nullptr; };
std::vector<TracedAbility> traced_;
void RedirectWeaponTrace() {
    if(!active_||skill_.handle<=0||!Same(skill_.asc)||!Same(display_)){traced_.clear();return;}
    Object* pp=Property<Object*>(display_.object,"PostProcessAnimInstance");if(!ue::Alive(pp))return;
    Object* asc=skill_.asc.object,*type=Class("/Script/GameplayAbilities.GameplayAbilitySpec"),*container=Class("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t specs=Offset(asc,"ActivatableAbilities"),items=container?ue::Offset(container,"Items"):-1,size=type?ue::StructSize(type):0,
        handle_at=type?ue::Offset(type,"Handle"):-1,count_at=type?ue::Offset(type,"ActiveCount"):-1;
    if(specs<0||items<0||size<=0||handle_at<0||count_at<0)return;
    const Array array=ue::At<Array>(asc,specs+items);
    if(array.num<0||array.num>4096||array.max<array.num||(array.num&&!FaceReadable(array.data,static_cast<size_t>(array.num)*static_cast<size_t>(size))))return;
    std::vector<int32_t> running; // handles first: the calls below must not walk the array they could change
    for(int32_t i=0;i<array.num;++i){Object* row=static_cast<Object*>(array.data)+static_cast<size_t>(i)*static_cast<size_t>(size);if(ue::At<uint8_t>(row,count_at))running.push_back(ue::At<int32_t>(row,handle_at));}
    for(const int32_t handle:running){
        bool instance=false;Object* ability=SpecAbility(asc,handle,instance);
        if(!instance||!IsBlueprint(ability,"GA_WeaponTraceAttack_Base_C"))continue;
        Object* was=Property<Object*>(ability,"PP");if(was==pp||!Put(ability,"PP",pp))continue;
        const auto known=std::find_if(traced_.begin(),traced_.end(),[&](const TracedAbility& t){return t.ability.object==ability&&Same(t.ability);});
        if(known==traced_.end())traced_.push_back({Id(ability),Id(was),pp});else{known->original=Id(was);known->pp=pp;}
        Logf("NPC_SKILL weapon trace %s: PP %s -> %s",Text(ability).c_str(),Text(was).c_str(),Text(pp).c_str());
    }
}
// 魔渊 skill 06 (GA_MoTian_Skill_06): its timeline holds CharacterState.AbilityState.MoYuan.SpawnPillar on the caster
// from 0.12 s for 4.47 s; the ability waits for the tag and calls BP_Monster_MoTian.SpawnPillar / DespawnPillar:
//   Spawn:   Pillar = SpawnSystemAttached(NS_MoYuan_Pillar_001_Main, Mesh, "interaction", (0,0,0), (0,90,0), SnapToTarget,
//            auto destroy, AutoRelease pool, pre-cull); Pillar.SetWorldScale3D(1.6, 1.4, 1.6)
//   Despawn: Pillar.Deactivate(); 0.5 s later Pillar.K2_DestroyComponent()
// The hero's ability system holds the same tag; DriveSkillPillar follows it on the display mesh.
struct SkillPillar { const char* ability; const wchar_t* tag; const char* system; const char* socket; RotatorD turn; Vector scale; ULONGLONG linger_ms; };
const SkillPillar kMoYuanPillar{"GA_MoTian_Skill_06",L"CharacterState.AbilityState.MoYuan.SpawnPillar",
    "/PBZVFXPack/VFX/BattleFX/MoYuan/NS_MoYuan_Pillar_001_Main.NS_MoYuan_Pillar_001_Main","interaction",{0,90,0},{1.6,1.4,1.6},500};
struct Lingering { Identity component;ULONGLONG destroy_at=0; };
struct SkillPillarState { ULONGLONG skill_started=0;Identity component;bool spawned=false,despawned=false; };
SkillPillarState skill_pillar_;std::vector<Lingering> lingering_;
void DespawnSkillPillar(const char* why) {
    if(skill_pillar_.spawned&&!skill_pillar_.despawned&&Same(skill_pillar_.component)){Object* component=skill_pillar_.component.object;
        const bool off=Call(component,"Deactivate").Invoke();lingering_.push_back({skill_pillar_.component,GetTickCount64()+kMoYuanPillar.linger_ms});
        Logf("NPC_SKILL %s pillar despawned reason=%s deactivated=%d",kMoYuanPillar.ability,why,off?1:0);}
    skill_pillar_.despawned=true;
}
void DriveSkillPillar() {
    const ULONGLONG now=GetTickCount64();
    lingering_.erase(std::remove_if(lingering_.begin(),lingering_.end(),[&](const Lingering& l){if(active_&&now<l.destroy_at)return false;if(Same(l.component))Destroy(l.component.object);return true;}),lingering_.end());
    const SkillPillar& e=kMoYuanPillar;
    const bool mine=active_&&skill_.handle>0&&skill_.name==e.ability&&Same(pawn_)&&Same(skill_.asc)&&Same(display_);
    if(skill_pillar_.skill_started&&(!mine||skill_pillar_.skill_started!=skill_.started)){DespawnSkillPillar(!active_?"skin-off":!mine?"skill-ended":"next-skill");skill_pillar_={};}
    if(!mine)return;
    skill_pillar_.skill_started=skill_.started;if(skill_pillar_.despawned)return;
    static const Name tag=ue::MakeName(e.tag);Call has(skill_.asc.object,"HasMatchingGameplayTag","/Script/GameplayTags.GameplayTagAssetInterface");
    const bool held=!tag.None()&&has.Set("TagToCheck",tag).Invoke()&&has.Get<bool>("ReturnValue");
    if(held&&!skill_pillar_.spawned){
        skill_pillar_.spawned=true;Object* display=display_.object;const Name socket=DisplaySocket(display,e.socket);
        Object* component=SpawnEffectAttached(e.system,display,socket,Vector{},e.turn,2);
        const bool scaled=component&&Call(component,"SetWorldScale3D").Set("NewScale",e.scale).Invoke();if(component)skill_pillar_.component=Id(component);
        Logf("NPC_SKILL %s pillar %s socket=%s on %s scaled=%d",e.ability,component?"spawned":"failed",socket.None()?"none (mesh origin)":Text(socket).c_str(),Text(display).c_str(),scaled?1:0);}
    else if(!held&&skill_pillar_.spawned)DespawnSkillPillar("tag-removed");
}
// 魔渊 skill 08 (GA_MoTian_Skill_08): on CharacterState.Combat.ComboPoint.ComboPoint_1 it spawns BP_MoYuan_CA_PillarGroup_001
// (pillars rising behind the boss, child actors) with the avatar's rotation at the avatar's location - 1000 x
// forward(look-at from it to the player), at the height of the boss's Mesh. On the hero that Mesh comes from the failed
// boss cast (0) and the player is the hero itself (no direction, forward = world +X): the group appears 10 m along
// world -X at height 0. PlacePillarGroup moves such a group, the frame it appears, to the boss's spot: 10 m behind the
// hero away from the nearest enemy (behind its facing when there is none), at the height of the hero's mesh.
struct PillarGroups { ULONGLONG skill_started=0;Identity cls;std::vector<Identity> placed; };
PillarGroups groups_;
void PlacePillarGroup() {
    if(!(active_&&skill_.handle>0&&skill_.name=="GA_MoTian_Skill_08"&&Same(pawn_)&&Same(primary_))){if(groups_.skill_started)groups_={};return;}
    if(groups_.skill_started!=skill_.started){groups_={};groups_.skill_started=skill_.started;
        Object* cls=LoadClassPackage("/PBZVFXPack/VFX/BattleFX/MoYuan/BP_MoYuan_CA_PillarGroup_001");if(cls)groups_.cls=Id(cls);
        else Logf("NPC_SKILL GA_MoTian_Skill_08 pillar group class not loaded; group left where the skill puts it");}
    if(!Same(groups_.cls))return;
    Object* hero=pawn_.object;Vector self{};{Call where(hero,"K2_GetActorLocation");if(!where.Invoke())return;self=where.Get<Vector>("ReturnValue");}
    Call actors(ue::Default("/Script/Engine.Default__GameplayStatics"),"GetAllActorsOfClass");
    if(!actors.Set("WorldContextObject",hero).Set("ActorClass",groups_.cls.object).Invoke())return;
    const Array list=actors.Get<Array>("OutActors");std::vector<Object*> found;
    if(list.data&&list.num>0&&list.num<=256&&FaceReadable(list.data,static_cast<size_t>(list.num)*sizeof(Object*)))
        for(int32_t i=0;i<list.num;++i)if(Object* group=static_cast<Object**>(list.data)[i];ue::Alive(group))found.push_back(group);
    ue::Free(list.data);
    for(Object* group:found){
        if(std::any_of(groups_.placed.begin(),groups_.placed.end(),[&](const Identity& p){return p.object==group&&Same(p);}))continue;
        Vector at{};{Call where(group,"K2_GetActorLocation");if(!where.Invoke())continue;at=where.Get<Vector>("ReturnValue");}
        if(std::abs(at.z)>1||std::hypot(at.x-(self.x-1000),at.y-self.y)>400)continue; // not the hero's misplaced one (a boss's own is elsewhere)
        groups_.placed.push_back(Id(group));
        Vector toward{},enemy{};const bool aimed=NearestEnemy(hero,enemy);
        if(aimed){Call look(Math(),"FindLookAtRotation");Call forward(Math(),"GetForwardVector");
            if(look.Set("Start",self).Set("Target",enemy).Invoke()&&forward.Set("InRot",look.Get<RotatorD>("ReturnValue")).Invoke())toward=forward.Get<Vector>("ReturnValue");}
        else{Call forward(hero,"GetActorForwardVector");if(forward.Invoke())toward=forward.Get<Vector>("ReturnValue");}
        double z=self.z;{Call mesh(primary_.object,"K2_GetComponentLocation");if(mesh.Invoke())z=mesh.Get<Vector>("ReturnValue").z;}
        const Vector spot{self.x-toward.x*1000,self.y-toward.y*1000,z};
        Call move(group,"K2_SetActorLocation");const bool moved=move.Set("NewLocation",spot).Set("bSweep",false).Set("bTeleport",true).Invoke()&&move.Get<bool>("ReturnValue");
        Logf("NPC_SKILL GA_MoTian_Skill_08 pillar group %s moved=%d from (%.0f,%.0f,%.0f) to (%.0f,%.0f,%.0f) %s",Text(group).c_str(),moved?1:0,at.x,at.y,at.z,spot.x,spot.y,spot.z,
            aimed?"behind the hero, away from the nearest enemy":"behind the hero (no enemy)");
    }
}
// 雷当 special attack 03 and 左殇 skill 20 open a counter window (GAA_DeflectFrame); a successful counter sends
// GameplayEvent.Ability.LeiDang.Deflect to the caster and the ability answers with a flash (NS_GP_Triggered_001) and a
// short slow motion:
//   雷当: SpawnSystemAtLocation(at the boss Mesh's Weapon_r socket, scale 1.5)
//   左殇: SpawnSystemAttached(Mesh, "root", (0, 50, 120), KeepRelativeOffset); SetRelativeScale3D(1.5)
// On the hero the Mesh is the failed boss cast: the flash lands at the world origin or is not spawned at all. The
// ability keeps the event's payload in its persistent ubergraph frame (K2Node_CustomEvent_Payload): DriveCounterFlash
// clears its EventTag when the skill starts and shows the flash on the display each time the tag is set again.
struct CounterFlash { const char* ability; bool attached; const char* socket; Vector at; };
constexpr CounterFlash kCounterFlashes[]={{"GA_LeiDang_SpecialAttack_03",false,"Weapon_r",{}},{"GA_ZuoShang_Skill_20",true,"root",{0,50,120}}};
constexpr char kCounterFlashSystem[]="/PBZVFXPack/VFX/BattleFX/Common/NS_GP_Triggered_001.NS_GP_Triggered_001";
constexpr ULONGLONG kCounterFrameWaitMs=500;
struct CounterState { ULONGLONG skill_started=0;Identity ability;Name* tag=nullptr;bool logged=false;uint32_t flashes=0; };
CounterState counter_;
// The EventTag of the event payload in the ability's ubergraph frame; nullptr when the layout is not the expected one.
Name* PayloadTag(Object* ability,const std::string& name) {
    Object* cls=ue::ClassOf(ability),*data=Class("/Script/GameplayAbilities.GameplayEventData");const uintptr_t frame_field=Field(cls,"UberGraphFrame");
    Object* graph=ue::FindFunction(cls,("ExecuteUbergraph_"+name).c_str());const uintptr_t payload=graph?Field(graph,"K2Node_CustomEvent_Payload"):0;
    if(!frame_field||!payload||!data||ue::Offset(data,"EventTag")!=0||ue::At<int32_t>(reinterpret_cast<void*>(payload),ue::layout::kPropElementSize)!=ue::StructSize(data))return nullptr;
    unsigned char* frame=ue::At<unsigned char*>(ability,ue::At<int32_t>(reinterpret_cast<void*>(frame_field),ue::layout::kPropOffset));
    const int32_t at=ue::At<int32_t>(reinterpret_cast<void*>(payload),ue::layout::kPropOffset);
    return frame&&at>=0&&FaceReadable(frame+at,sizeof(Name))?reinterpret_cast<Name*>(frame+at):nullptr;
}
void ShowCounterFlash(const CounterFlash& row) {
    Object* display=display_.object;const Name socket=DisplaySocket(display,row.socket);Object* flash=nullptr;
    if(row.attached){flash=SpawnEffectAttached(kCounterFlashSystem,display,socket,row.at,RotatorD{},0);if(flash)Call(flash,"SetRelativeScale3D").Set("NewScale",Vector{1.5,1.5,1.5}).Invoke();}
    else{Call where(display,socket.None()?"K2_GetComponentLocation":"GetSocketLocation");if(!socket.None())where.Set("InSocketName",socket);
        Object* system=LoadAssetPath(kCounterFlashSystem);Call spawn(Niagara(),"SpawnSystemAtLocation");
        if(where.Invoke()&&ue::Alive(system)&&spawn.Set("WorldContextObject",display).Set("SystemTemplate",system).Set("Location",where.Get<Vector>("ReturnValue")).Set("Rotation",RotatorD{})
            .Set("Scale",Vector{1.5,1.5,1.5}).Set("bAutoDestroy",true).Set("bAutoActivate",true).Set("PoolingMethod",uint8_t{0}).Set("bPreCullCheck",false).Invoke())flash=spawn.Get<Object*>("ReturnValue");}
    Logf("NPC_SKILL %s counter flash %s socket=%s on %s (flash %u this skill)",row.ability,ue::Alive(flash)?"shown":"failed",socket.None()?"none":Text(socket).c_str(),Text(display).c_str(),++counter_.flashes);
}
void DriveCounterFlash() {
    const CounterFlash* row=nullptr;
    if(active_&&skill_.handle>0&&Same(skill_.asc)&&Same(display_))for(const CounterFlash& c:kCounterFlashes)if(skill_.name==c.ability)row=&c;
    if(!row){counter_={};return;}
    if(counter_.skill_started!=skill_.started){counter_={};counter_.skill_started=skill_.started;}
    if(!counter_.tag){
        bool instance=false;Object* ability=SpecAbility(skill_.asc.object,skill_.handle,instance);Name* tag=instance&&ue::Alive(ability)?PayloadTag(ability,skill_.name):nullptr;
        if(tag){*tag=Name{};counter_.tag=tag;counter_.ability=Id(ability);Logf("NPC_SKILL %s counter flash: event payload watched on %s",row->ability,Text(ability).c_str());}
        else if(!counter_.logged&&GetTickCount64()-skill_.started>kCounterFrameWaitMs){counter_.logged=true;Logf("NPC_SKILL %s counter flash: no event payload in the ability's ubergraph frame; flash not shown",row->ability);}
        return;}
    if(!Same(counter_.ability)){counter_.tag=nullptr;return;}
    if(counter_.tag->None())return;
    *counter_.tag=Name{}; // the next counter sets it again
    ShowCounterFlash(*row);
}
void DriveBossCalls() { DriveDragonPillar();RedirectWeaponTrace();DriveSkillPillar();PlacePillarGroup();DriveCounterFlash();MoveSkillEffects(); }
// The skin is coming off: what the replays hold goes now, not a frame later.
void EndBossCalls(const char* why) {
    if(pillar_.prepared&&!pillar_.thrown)CancelPillar(why);pillar_={};
    DespawnSkillPillar(why);skill_pillar_={};
    for(const Lingering& l:lingering_)if(Same(l.component))Destroy(l.component.object);lingering_.clear();
    size_t restored=0;
    for(const TracedAbility& t:traced_)if(Same(t.ability)&&Property<Object*>(t.ability.object,"PP")==t.pp&&Put(t.ability.object,"PP",Same(t.original)?t.original.object:nullptr))++restored;
    if(!traced_.empty())Logf("NPC_SKILL weapon trace PP handed back=%zu of %zu reason=%s",restored,traced_.size(),why);
    traced_.clear();groups_={};counter_={};
}
// ---- Foreign montages ---------------------------------------------------------------------------
void HoldForeignMontage() {
    if(!active_||!source_driver_mode_||!Same(primary_))return;
    Object* montage=nullptr;if(Object* anim=AnimInstance(primary_.object)){Call current(anim,"GetCurrentActiveMontage");if(current.Invoke())montage=current.Get<Object*>("ReturnValue");}
    if(!ue::Alive(montage))montage=nullptr;
    const ULONGLONG now=GetTickCount64();
    if(montage!=foreign_montage_){
        foreign_montage_=montage;
        Object* skeleton=montage?Property<Object*>(montage,"Skeleton"):nullptr,*own=Property<Object*>(SkelMesh(primary_.object),"Skeleton");
        Object* npc=Same(display_)?Property<Object*>(SkelMesh(display_.object),"Skeleton"):nullptr;
        const double want=skeleton&&own&&skeleton!=own?1:0;
        // Fading back to the hero keeps the last montage's kind, so the display does not jump.
        const bool kind=want>0?skeleton==npc:foreign_own_.load(std::memory_order_relaxed);
        if(want!=foreign_to_||kind!=foreign_own_.load(std::memory_order_relaxed)){
            if(want!=foreign_to_){foreign_from_=foreign_weight_.load(std::memory_order_relaxed);foreign_to_=want;foreign_start_=now;}
            foreign_own_.store(kind,std::memory_order_relaxed);
            Logf("NPC_BODY montage %s on %s: display translations %s",Text(montage).c_str(),Text(skeleton).c_str(),
                want<=0?"follow the hero again":kind?"take the NPC's own track back (own skeleton)":"held at the NPC's own rest (foreign skeleton)");}}
    const double weight=foreign_weight_.load(std::memory_order_relaxed);if(weight==foreign_to_)return;
    const double t=std::clamp(static_cast<double>(now-foreign_start_)/static_cast<double>(kForeignBlendMs),0.0,1.0);
    foreign_weight_.store(t>=1?foreign_to_:foreign_from_+(foreign_to_-foreign_from_)*t,std::memory_order_relaxed);
}
// ---- Post-process nodes aimed by the boss's own state ----------------------------------------------
// The display runs the boss's post-process AnimBP, whose event graph feeds some nodes from the boss's AI and
// weapon; under the hero those inputs stay unset (长鸿: a LookAt turning the head to a default world spot, a
// TwoBoneIK pulling both hands to HandIKLocation on a weapon the hero does not hold, off the weapon the skin
// shows). A LookAt with no target bone or socket (aimed at a world location) and a TwoBoneIK whose effector is
// a world or component location read their alpha from a curve the skin never has: 0. Bone- and socket-aimed
// ones are rig constraints and stay. Once per post-process instance (it is rebuilt with the anim instance).
void QuietAimNodes(Object* display) {
    Object* instance=display&&Same(display_)&&display_.object==display?Property<Object*>(display,"PostProcessAnimInstance"):nullptr;
    if(instance==quiet_instance_)return;quiet_instance_=instance;if(!ue::Alive(instance))return;
    Object* look=Class("/Script/AnimGraphRuntime.AnimNode_LookAt"),*ik=Class("/Script/AnimGraphRuntime.AnimNode_TwoBoneIK"),*target=Class("/Script/Engine.BoneSocketTarget");
    const int32_t type=look?ue::Offset(look,"AlphaInputType"):-1,alpha=look?ue::Offset(look,"Alpha"):-1,enabled=look?ue::Offset(look,"bAlphaBoolEnabled"):-1,curve=look?ue::Offset(look,"AlphaCurveName"):-1,
        look_target=look?ue::Offset(look,"LookAtTarget"):-1,look_bone=look?ue::Offset(look,"BoneToModify"):-1,space=ik?ue::Offset(ik,"EffectorLocationSpace"):-1,ik_bone=ik?ue::Offset(ik,"IKBone"):-1,
        bone=target?ue::Offset(target,"BoneReference"):-1,socket=target?ue::Offset(target,"SocketReference"):-1,look_size=ue::StructSize(look),ik_size=ue::StructSize(ik);
    // The alpha fields are FAnimNode_SkeletalControlBase's, at the same offsets in both nodes.
    if(type<0||alpha<0||enabled<0||curve<0||look_target<0||look_bone<0||space<0||ik_bone<0||bone<0||socket<0||look_size<=0||ik_size<=0||
        ue::Offset(ik,"AlphaInputType")!=type||ue::Offset(ik,"Alpha")!=alpha||ue::Offset(ik,"bAlphaBoolEnabled")!=enabled||ue::Offset(ik,"AlphaCurveName")!=curve){
        Logf("NPC_BODY post-process aim nodes left alone: node layout unreadable");return;}
    const Name quiet=ue::MakeName(L"PBZ_NpcSkinNoAimTarget");size_t off=0,kept=0;std::string list;
    for(Object* cls=ue::ClassOf(instance);cls;cls=ue::At<Object*>(cls,ue::layout::kSuper)){uintptr_t field=ue::At<uintptr_t>(cls,ue::layout::kChildProperties);
        for(size_t guard=0;field&&guard<4096;++guard,field=ue::At<uintptr_t>(reinterpret_cast<void*>(field),ue::layout::kPropNext)){void* f=reinterpret_cast<void*>(field);
            // A struct property of the node's size whose struct is the node (FStructProperty::Struct at 0x70).
            const int32_t size=ue::At<int32_t>(f,ue::layout::kPropElementSize);if(size!=look_size&&size!=ik_size)continue;
            Object* strukt=ue::At<Object*>(f,0x70);if(!((strukt==look&&size==look_size)||(strukt==ik&&size==ik_size)))continue;
            Object* node=instance+ue::At<int32_t>(f,ue::layout::kPropOffset);bool aimed=false;Name modified;std::string why;
            if(strukt==look){aimed=ue::At<Name>(node,look_target+bone).None()&&ue::At<Name>(node,look_target+socket).None();modified=ue::At<Name>(node,look_bone);why=aimed?"world location":"bone/socket target";}
            else{const uint8_t s=ue::At<uint8_t>(node,space);aimed=s<=1;modified=ue::At<Name>(node,ik_bone);why=s==0?"world effector":s==1?"component effector":"bone-space effector";}
            if(list.size()<400)list+=(list.empty()?"":", ")+Text(ue::At<Name>(f,ue::layout::kPropName))+"("+Text(modified)+" "+why+(aimed?" -> off":" kept")+")";
            if(!aimed){++kept;continue;}
            ue::At<uint8_t>(node,type)=2;ue::At<Name>(node,curve)=quiet;ue::At<float>(node,alpha)=0;ue::At<bool>(node,enabled)=false;++off;}}
    Logf("NPC_BODY post-process aim nodes off=%zu kept=%zu on %s: %s",off,kept,Text(ue::ClassOf(instance)).c_str(),list.empty()?"none":list.c_str());
}
// ---- Mount pose puppet ----------------------------------------------------------------------------
// Package-root assets only (an AnimSequence, never anything that holds a world), held by the same counted
// strong reference as the skill classes until the skin comes off.
bool PinAsset(Object* asset,std::vector<Identity>& pins) {
    if(!ClassRefsReady()||!ue::Alive(asset))return false;Object* outer=ue::OuterOf(asset);
    if(!ue::Alive(outer)||!ue::IsA(outer,Class("/Script/CoreUObject.Package"))||ue::OuterOf(outer))return false;
    const Identity id=Id(asset);if(!Same(id))return false;
    reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x1573ff0)(asset);pins.push_back(id);return true;
}
// An asset by object path ("/Pkg/Name.Name"): found if loaded, else loaded blocking.
Object* LoadAssetPath(const char* path) {
    if(Object* object=ue::FindObject(path))if(ue::Alive(object))return object;
    const std::string text(path);const size_t dot=text.find_last_of('.');if(dot==std::string::npos)return nullptr;
    const std::string package=text.substr(0,dot),leaf=text.substr(dot+1);const std::wstring p(package.begin(),package.end()),a(leaf.begin(),leaf.end());
    SoftPath soft;soft.package=ue::MakeName(p.c_str());soft.asset=ue::MakeName(a.c_str());
    Call load(Kismet(),"LoadAsset_Blocking");if(!load.Set("Asset",soft).Invoke())return nullptr;
    Object* object=load.Get<Object*>("ReturnValue");return ue::Alive(object)?object:nullptr;
}
// The puppet: the display mesh on an invisible component that keeps evaluating, without post-process,
// cloth or collision; the display ticks after it so its finished pose is this frame's. Not fatal: without
// it the mount bones keep their rest pose, as before.
void CreateMountPuppet(const Context& context,Object* display,Object* mesh) {
    Mount& m=mount_;if(!m.row||!pose_session_)return;const MountLoops& row=*m.row;const char* paths[3]={row.idle,row.walk,row.run};size_t loaded=0;
    for(size_t i=0;i<3;++i){Object* anim=paths[i]?LoadAssetPath(paths[i]):nullptr;if(!anim||!PinAsset(anim,m.pins))continue;m.loops[i]=anim;
        Call length(anim,"GetPlayLength");m.length[i]=length.Invoke()?length.Get<float>("ReturnValue"):0;if(!(m.length[i]>0))m.length[i]=Property<float>(anim,"SequenceLength");++loaded;}
    Object* puppet=Add(context.pawn,Class("/Script/Engine.SkeletalMeshComponent"));if(puppet)added_.push_back(Id(puppet));
    const bool made=puppet&&SetVisible(puppet,false,true)&&SetMesh(puppet,mesh)&&Put(puppet,"VisibilityBasedAnimTickOption",uint8_t{0})&&
        PutBool(puppet,"bEnableUpdateRateOptimizations",false)&&PutBool(puppet,"bDisablePostProcessBlueprint",true)&&
        Call(puppet,"SetCollisionEnabled").Set("NewType",uint8_t{0}).Invoke()&&Call(puppet,"SetGenerateOverlapEvents").Set("bInGenerateOverlapEvents",false).Invoke()&&
        AttachFinish(context.pawn,puppet,context.mesh,Name{},Transform{})&&Call(puppet,"SetAnimationMode").Set("InAnimationMode",uint8_t{1}).Invoke()&& // AnimationSingleNode
        Call(display,"AddTickPrerequisiteComponent").Set("PrerequisiteComponent",puppet).Invoke();
    if(made){PutBool(puppet,"bDisableClothSimulation",true);Call(puppet,"SetForcedLOD").Set("InNewForcedLOD",int32_t{1}).Invoke();m.puppet=Id(puppet);
        AcquireSRWLockExclusive(&pose_lock_);pose_session_->puppet=m.puppet;ReleaseSRWLockExclusive(&pose_lock_);}
    Logf("NPC_MOUNT puppet %s on %s: loops idle=%s (%.2f s) walk=%s (%.2f s) run=%s (%.2f s), %zu loaded and held",made?"created":"refused",Text(mesh).c_str(),
        Text(m.loops[0]).c_str(),m.length[0],Text(m.loops[1]).c_str(),m.length[1],Text(m.loops[2]).c_str(),m.length[2],loaded);
}
// Every frame: the boss's own montage at the hero's position while one runs, else the loop for the hero's
// speed (a missing walk is the run, else idle; a missing run the walk), advanced here at rate 1.
void DriveMount() {
    Mount& m=mount_;if(!m.row||!m.puppet.object||!Same(m.puppet)||!Same(primary_)||!Same(pawn_))return;
    const ULONGLONG now=GetTickCount64();const double dt=m.last_ms?(std::min)(0.25,static_cast<double>(now-m.last_ms)/1000.0):0;m.last_ms=now;
    Object* want=nullptr;float position=0;bool looping=false;int32_t loop=-1;
    if(foreign_montage_&&foreign_to_>0&&foreign_own_.load(std::memory_order_relaxed)&&ue::Alive(foreign_montage_))
        if(Object* anim=AnimInstance(primary_.object)){Call at(anim,"Montage_GetPosition");if(at.Set("Montage",foreign_montage_).Invoke()){want=foreign_montage_;position=at.Get<float>("ReturnValue");}}
    if(!want){
        Vector v;Call velocity(pawn_.object,"GetVelocity");if(velocity.Invoke())v=velocity.Get<Vector>("ReturnValue");
        const double speed=std::sqrt(v.x*v.x+v.y*v.y);int32_t gait=m.gait<0?0:m.gait;
        if(gait==0&&speed>kMountWalkCm)gait=1;if(gait==1&&speed>kMountRunCm)gait=2;if(gait==2&&speed<kMountWalkBackCm)gait=1;if(gait==1&&speed<kMountIdleCm)gait=0;m.gait=gait;
        static constexpr int32_t kOrder[3][3]={{0,1,2},{1,2,0},{2,1,0}};
        for(const int32_t k:kOrder[gait])if(m.loops[k]){loop=k;want=m.loops[k];looping=true;break;}}
    if(!want)return;
    if(want!=m.playing){
        if(!Call(m.puppet.object,"PlayAnimation").Set("NewAnimToPlay",want).Set("bLooping",looping).Invoke())return;
        Call(m.puppet.object,"SetPlayRate").Set("Rate",0.f).Invoke();
        // Walk <-> run keeps the stride phase; anything else starts the loop over.
        m.position=looping&&m.loop>=0&&m.length[m.loop]>0&&m.playing==m.loops[m.loop]?std::fmod(m.position/m.length[m.loop],1.0)*m.length[loop]:0;
        m.playing=want;mount_serial_.fetch_add(1,std::memory_order_relaxed);
        if(++m.switches<=24)Logf("NPC_MOUNT puppet plays %s%s",Text(want).c_str(),looping?" (loop by speed)":" (the boss's own montage at the hero's position)");}
    m.loop=loop;
    if(looping){const double length=m.length[loop];m.position=length>0?std::fmod(m.position+dt,length):0;position=static_cast<float>(m.position);}
    Call(m.puppet.object,"SetPosition").Set("InPos",position).Set("bFireNotifies",false).Invoke();
}
// The puppet component itself goes with added_.
void RetireMount(const char* why) {
    if(!mount_.row&&!mount_.puppet.object&&mount_.pins.empty()){quiet_instance_=nullptr;return;}
    AcquireSRWLockExclusive(&pose_lock_);if(pose_session_)pose_session_->puppet={};ReleaseSRWLockExclusive(&pose_lock_);
    size_t released=0;
    if(!ClassRefsReady())Logf("NPC_MOUNT loop references kept: native helper bytes changed");
    else for(const Identity& pin:mount_.pins)if(RawIdentity(pin,pin.index,pin.serial)){reinterpret_cast<void(__fastcall*)(Object*)>(ue::Base()+0x158fa30)(pin.object);++released;}
    Logf("NPC_MOUNT retired (%s): puppet switches=%u, loop references released=%zu of %zu",why,mount_.switches,released,mount_.pins.size());
    mount_={};quiet_instance_=nullptr;
}
Object* DefaultObject(Object* cls) {
    Object* cdo=nullptr;
    if(cls)ue::ForEachObject([&](Object* object){if(ue::IsDefault(object)&&ue::ClassOf(object)==cls&&ue::Alive(object)){cdo=object;return false;}return true;});
    return cdo;
}
// Loads the skin's combat blueprint once: reads the capsule its class default carries and pins the
// boss's weapon classes and the skill classes it pulled in (its behavior trees import them), so casts
// need no load. The blueprint itself is not pinned: its combat mesh and AnimBP may unload.
void LoadCombatBlueprint(int32_t entry,double& radius,double& half) {
    radius=half=0;const NpcSkillSet& set=kNpcSkillSets[entry];if(!set.monster)return;
    const ULONGLONG t0=GetTickCount64();Object* cls=LoadClassPackage(set.monster);
    Object* capsule=Property<Object*>(DefaultObject(cls),"CapsuleComponent");
    if(ue::Alive(capsule)){radius=Property<float>(capsule,"CapsuleRadius");half=Property<float>(capsule,"CapsuleHalfHeight");}
    size_t weapons=0;
    for(size_t i=0;i<set.weapon_count;++i){const std::string path=set.weapons[i].actor_class;Object* weapon=Class(path.c_str());
        if(!weapon&&path.find('.')!=std::string::npos)weapon=LoadClassPackage(path.substr(0,path.find('.')));weapons+=PinClass(weapon)?1:0;}
    // One pass over the object array: every skill class already loaded, matched by name and package.
    std::map<std::string,size_t> wanted;for(size_t i=0;i<set.skill_count;++i){const std::string package=set.skills[i];wanted[package.substr(package.find_last_of('/')+1)+"_C"]=i;}
    std::vector<Object*> found;Object* class_type=Class("/Script/CoreUObject.Class");
    if(class_type)ue::ForEachObject([&](Object* object){
        if(!ue::IsA(object,class_type))return true;const auto hit=wanted.find(Text(object));
        if(hit!=wanted.end()&&ue::Alive(object)&&Text(ue::OuterOf(object))==set.skills[hit->second])found.push_back(object);return true;});
    size_t pinned=0;for(Object* skill:found)pinned+=PinClass(skill)?1:0;
    Logf("NPC_BODY combat blueprint=%s loaded=%d ms=%llu capsule radius=%.1f half=%.1f bossWeaponsPinned=%zu/%zu skillClassesLoaded=%zu/%zu pinned=%zu",
        set.monster,cls?1:0,GetTickCount64()-t0,radius,half,weapons,set.weapon_count,found.size(),set.skill_count,pinned);
}
bool CapsuleSize(Object* capsule,double radius,double half) {
    return Call(capsule,"SetCapsuleSize").Set("InRadius",static_cast<float>(radius)).Set("InHalfHeight",static_cast<float>(half)).Set("bUpdateOverlaps",true).Invoke();
}
bool RelativeLocation(Object* component,const Vector& at) {
    return Call(component,"K2_SetRelativeLocation").Set("NewLocation",at).Set("bSweep",false).Set("bTeleport",true).Invoke();
}
bool Near(double a,double b){return std::fabs(a-b)<0.01;}
bool NearShape(const NpcBodyCollision::Shape& a,const NpcBodyCollision::Shape& b) {
    for(size_t i=0;i<3;++i)if(!Near(a.location[i],b.location[i])||!Near(a.scale[i],b.scale[i]))return false;
    return true;
}
NpcBodyCollision::Shape ReadShape(Object* component) {
    NpcBodyCollision::Shape shape;const Vector at=Property<Vector>(component,"RelativeLocation"),scale=Property<Vector>(component,"RelativeScale3D");
    shape.location[0]=at.x;shape.location[1]=at.y;shape.location[2]=at.z;shape.scale[0]=scale.x;shape.scale[1]=scale.y;shape.scale[2]=scale.z;return shape;
}
bool WriteShape(Object* component,const NpcBodyCollision::Shape& shape) {
    return RelativeLocation(component,Vector{shape.location[0],shape.location[1],shape.location[2]})&&
        Call(component,"SetRelativeScale3D").Set("NewScale3D",Vector{shape.scale[0],shape.scale[1],shape.scale[2]}).Invoke();
}
// The skin's body: the movement capsule takes the skin's radius (its combat blueprint's, at most the hero
// half-height) and keeps the hero class-default half-height, so the actor, the mesh offset and the camera
// never move; the other pawn shapes (dodge / shield / dash-damage / touch volumes) scale about the feet.
// The hero size comes from the pawn's class default, never from a capsule a game state left resized.
void ApplyBodyCollision(const Context& context,int32_t entry) {
    Object* capsule=Property<Object*>(context.pawn,"CapsuleComponent");
    if(!ue::Alive(capsule)){Logf("NPC_BODY collision unchanged: the pawn has no CapsuleComponent");return;}
    const double live_radius=Property<float>(capsule,"CapsuleRadius"),live_half=Property<float>(capsule,"CapsuleHalfHeight");
    Object* default_capsule=Property<Object*>(DefaultObject(ue::ClassOf(context.pawn)),"CapsuleComponent");
    double hero_radius=ue::Alive(default_capsule)?Property<float>(default_capsule,"CapsuleRadius"):0,hero_half=ue::Alive(default_capsule)?Property<float>(default_capsule,"CapsuleHalfHeight"):0;
    const char* hero_from="class-default";if(!(hero_radius>1&&hero_half>=hero_radius)){hero_radius=live_radius;hero_half=live_half;hero_from="live";}
    double npc_radius=0,npc_half=0;LoadCombatBlueprint(entry,npc_radius,npc_half);
    const NpcBodyCollision::CapsulePlan plan=NpcBodyCollision::PlanCapsule(hero_radius,hero_half,npc_radius,npc_half,stature_ratio_,kCapsuleRatioMin,kCapsuleRatioMax);
    if(!plan.valid){Logf("NPC_BODY collision unchanged: hero capsule radius=%.1f half=%.1f (%s), blueprint %.1f/%.1f, stature ratio %.3f",hero_radius,hero_half,hero_from,npc_radius,npc_half,stature_ratio_);return;}
    body_={};body_.owned=true;body_.pawn=Id(context.pawn);body_.capsule=Id(capsule);body_.radius=hero_radius;body_.half=hero_half;body_.plan=plan;
    body_.kept_radius=NpcBodyCollision::KeptRadius(plan,hero_half);
    // Only at the class default: any other size is a game state of its own (crouch, a skill), and
    // HoldBodyCollision applies the radius once the game has put the default back.
    const bool at_default=Near(live_radius,hero_radius)&&Near(live_half,hero_half);
    bool ok=!at_default||CapsuleSize(capsule,body_.kept_radius,hero_half);
    std::string list;
    for(Object* shape:Components(context.pawn,Class("/Script/Engine.ShapeComponent"))){
        if(shape==capsule)continue;ShapeEdit edit;edit.component=Id(shape);edit.original=ReadShape(shape);edit.on_socket=!Property<Name>(shape,"AttachSocketName").None();
        Call parent(shape,"GetAttachParent");const double pivot=parent.Invoke()&&parent.Get<Object*>("ReturnValue")==capsule?-hero_half:0;
        edit.applied=NpcBodyCollision::ScaleShape(edit.original,plan.ratio,edit.on_socket,pivot);body_.shapes.push_back(edit);
        const bool written=WriteShape(shape,edit.applied);ok=written&&ok;if(list.size()<200)list+=(list.empty()?"":",")+Text(shape)+(written?"":"(refused)");}
    Logf("NPC_BODY collision capsule radius %.1f->%.1f half %.1f kept (hero %s; live %.1f/%.1f%s) blueprint %.1f/%.1f source=%s ratio=%.3f shapes=%zu(%s) actor/mesh/camera unmoved ok=%d",
        hero_radius,body_.kept_radius,hero_half,hero_from,live_radius,live_half,at_default?"":", applied when the game returns to the default",npc_radius,npc_half,plan.source,plan.ratio,
        body_.shapes.size(),list.empty()?"-":list.c_str(),ok?1:0);
}
void RestoreBodyCollision() {
    if(!body_.owned)return;std::string kept;
    if(Same(body_.capsule)){Object* capsule=body_.capsule.object;const double r=Property<float>(capsule,"CapsuleRadius"),h=Property<float>(capsule,"CapsuleHalfHeight");
        // Our radius goes back to the default; the half-height stays whatever the game has now.
        if(Near(r,body_.kept_radius)&&!Near(r,body_.radius))CapsuleSize(capsule,body_.radius,h);
        else if(!Near(r,body_.radius)){char text[64]{};_snprintf_s(text, sizeof(text), _TRUNCATE," capsule(%.1f/%.1f)",r,h);kept+=text;}}
    size_t restored=0;
    for(const ShapeEdit& edit:body_.shapes)if(Same(edit.component)){
        if(NearShape(ReadShape(edit.component.object),edit.applied)){WriteShape(edit.component.object,edit.original);++restored;}else kept+=" "+Text(edit.component.object);}
    Logf("NPC_BODY collision restored capsule radius=%.1f shapes=%zu/%zu radiusReapplied=%u changedByGameKept=%s",body_.radius,restored,body_.shapes.size(),body_.reapplied,kept.empty()?"none":kept.c_str());
    body_={};
}
// ---- Hurt voice -------------------------------------------------------------------------------------
// The hero's hurt voice (SFX_Combat_VO_Hun_Hurt / _Hurt_Heavy_Mix, the young form's HunYoung ones) is an
// AkAudioEvent in its hit-reaction data: ActorConfig_Hun -> Hit_Hun_Combined -> HitAnim_Hun_Double / Tang / Long
// (PDA_HitAnim_PlayerNew and their instanced strength sets). A boss has the same kind of data (ActorC_* -> Hit_*,
// PDA_HitAnim_MobNew) with its own hurt events. While a skin is on, every hurt slot of the hero's hit data holds
// the boss's event (its heavy one in a heavy slot when it has one). Only those player-only hit assets are written;
// the slots go back at restore by path, since nothing else holds the hero's events while they are out.
std::string ObjectPath(Object* object);
std::string PackageLeaf(Object* object) {
    Object* package=object;for(size_t i=0;package&&ue::OuterOf(package)&&i<16;++i)package=ue::OuterOf(package);
    const std::string path=package?Text(package):std::string{};return path.substr(path.find_last_of('/')+1);
}
void ObjectSlots(Object* type,unsigned char* base,int depth,std::vector<Object**>& out);
// The object reference slots of one value: the reference itself, a struct's, an array's or a map's elements.
void SlotsOf(Object* property,unsigned char* at,int depth,std::vector<Object**>& out) {
    const uint64_t kind=FieldKind(property);
    if(kind==0x10000){out.push_back(reinterpret_cast<Object**>(at));return;}
    if(depth>=6||out.size()>=4096)return;
    if(kind==0x100000){Object* type=ue::At<Object*>(property,0x70);if(ue::Alive(type))ObjectSlots(type,at,depth+1,out);return;}
    if(kind==0x200000){Object* inner=ue::At<Object*>(property,0x78);const ue::Array list=*reinterpret_cast<const ue::Array*>(at);
        const int32_t stride=inner?ue::At<int32_t>(inner,ue::layout::kPropElementSize):0;
        if(!inner||list.num<=0||list.num>4096||stride<=0||stride>65536||!FaceReadable(list.data,size_t(list.num)*size_t(stride)))return;
        for(int32_t i=0;i<list.num;++i)SlotsOf(inner,static_cast<unsigned char*>(list.data)+size_t(i)*size_t(stride),depth+1,out);return;}
    if(kind==0x400000000000ull){Object* key=ue::At<Object*>(property,0x70),*value=ue::At<Object*>(property,0x78);if(!key||!value)return;
        const int32_t value_offset=ue::At<int32_t>(property,0x80),stride=ue::At<int32_t>(property,0x8c);
        const int32_t count=*reinterpret_cast<const int32_t*>(at+8),bits=*reinterpret_cast<const int32_t*>(at+40);
        unsigned char* data=*reinterpret_cast<unsigned char* const*>(at);const uint32_t* allocation=*reinterpret_cast<uint32_t* const*>(at+32);
        if(!allocation)allocation=reinterpret_cast<const uint32_t*>(at+16);
        if(count<=0||stride<8||stride>4096||value_offset<0||value_offset>=stride||count>4096||bits!=count||
            !FaceReadable(data,size_t(count)*size_t(stride))||!FaceReadable(allocation,(size_t(bits)+31)/32*4))return;
        for(int32_t i=0;i<count;++i)if(allocation[i/32]&(1u<<(i%32))){unsigned char* row=data+size_t(i)*size_t(stride);
            SlotsOf(key,row,depth+1,out);SlotsOf(value,row+value_offset,depth+1,out);}}
}
void ObjectSlots(Object* type,unsigned char* base,int depth,std::vector<Object**>& out) {
    for(Object* s=type;ue::Alive(s)&&out.size()<4096;s=ue::At<Object*>(s,ue::layout::kSuper))
        for(Object* p=ue::At<Object*>(s,ue::layout::kChildProperties);p&&out.size()<4096;p=ue::At<Object*>(p,ue::layout::kPropNext))
            SlotsOf(p,base+ue::At<int32_t>(p,ue::layout::kPropOffset),depth,out);
}
// The hurt event slots of the hit data an actor config leads to: objects of packages named Hit* (the hit assets
// and their instanced subobjects), at most 256 of them; owners[i] is the object slot i lies in.
std::vector<Object**> HurtSlots(Object* config,std::vector<Object*>& owners) {
    std::vector<Object*> queue{config};std::set<Object*> seen{config};std::vector<Object**> found;
    for(size_t i=0;i<queue.size()&&i<256;++i){
        Object* owner=queue[i];std::vector<Object**> slots;ObjectSlots(ue::ClassOf(owner),reinterpret_cast<unsigned char*>(owner),0,slots);
        for(Object** slot:slots){Object* value=*slot;if(!value||!ue::Alive(value))continue;
            if(Text(ue::ClassOf(value))=="AkAudioEvent"){if(i&&Text(value).find("_Hurt")!=std::string::npos){found.push_back(slot);owners.push_back(owner);}continue;}
            if(seen.insert(value).second&&PackageLeaf(value).rfind("Hit",0)==0)queue.push_back(value);}
    }
    return found;
}
void RestoreHurtVoice() {
    if(voice_slots_.empty())return;size_t restored=0;
    for(auto it=voice_slots_.rbegin();it!=voice_slots_.rend();++it){
        if(!Same(it->owner)||*it->slot!=it->written)continue; // reloaded or changed since: not ours any more
        if(Object* original=LoadAssetPath(it->original.c_str())){*it->slot=original;++restored;}}
    Logf("NPC_VOICE hurt voice restored %zu/%zu",restored,voice_slots_.size());voice_slots_.clear();
}
void ApplyHurtVoice(const Context& context,int32_t entry) {
    RestoreHurtVoice();
    Object* hero_config=Property<Object*>(context.pawn,"Actor Config");
    Object* monster=entry>=0&&static_cast<size_t>(entry)<std::size(kNpcSkillSets)&&*kNpcSkillSets[entry].monster?LoadClassPackage(kNpcSkillSets[entry].monster):nullptr;
    Object* boss_config=monster?Property<Object*>(DefaultObject(monster),"Actor Config"):nullptr;
    if(!ue::Alive(hero_config)||!ue::Alive(boss_config)){Logf("NPC_VOICE hurt voice unchanged: actor config hero=%d boss=%d",ue::Alive(hero_config)?1:0,ue::Alive(boss_config)?1:0);return;}
    std::vector<Object*> hero_owners,boss_owners;const std::vector<Object**> hero=HurtSlots(hero_config,hero_owners),boss=HurtSlots(boss_config,boss_owners);
    Object* light=nullptr,*heavy=nullptr;
    for(Object** slot:boss){Object* e=*slot;if(Text(e).find("Heavy")!=std::string::npos){if(!heavy)heavy=e;}else if(!light)light=e;}
    if(!light)light=heavy;if(!heavy)heavy=light;
    if(!light){Logf("NPC_VOICE hurt voice unchanged: %s's hit data (%s) has no hurt event; hero slots %zu",Text(monster).c_str(),Text(boss_config).c_str(),hero.size());return;}
    std::string names;
    for(size_t i=0;i<hero.size();++i){Object* e=*hero[i];if(!ue::Alive(e))continue;Object* use=Text(e).find("Heavy")!=std::string::npos?heavy:light;if(use==e)continue;
        voice_slots_.push_back({Id(hero_owners[i]),hero[i],use,ObjectPath(e)});*hero[i]=use;
        if(names.size()<300)names+=(names.empty()?"":",")+Text(e)+"@"+Text(hero_owners[i]);}
    Logf("NPC_VOICE hurt voice of %s: %zu hero slots -> %s / %s (boss slots %zu) [%s]",Text(monster).c_str(),voice_slots_.size(),Text(light).c_str(),Text(heavy).c_str(),boss.size(),names.empty()?"-":names.c_str());
}
// The game resets the capsule to the class default after skills and crouch-like states; the skin's
// radius goes back on as soon as it does. Other sizes are the game's own state and are left alone.
ULONGLONG body_state_logged_=0;
void HoldBodyCollision() {
    if(!body_.owned||!Same(body_.capsule))return;Object* capsule=body_.capsule.object;
    const double r=Property<float>(capsule,"CapsuleRadius"),h=Property<float>(capsule,"CapsuleHalfHeight");
    if(Near(r,body_.kept_radius)&&Near(h,body_.half))return;
    if(Near(r,body_.radius)&&Near(h,body_.half)){if(CapsuleSize(capsule,body_.kept_radius,body_.half))++body_.reapplied;return;}
    if(GetTickCount64()<body_state_logged_)return;body_state_logged_=GetTickCount64()+10000;
    Logf("NPC_BODY capsule radius=%.1f half=%.1f is a game state of its own (skin %.1f/%.1f); left until it returns to the default",r,h,body_.kept_radius,body_.half);
}
bool RestoreSourceSettings();
bool Restore(bool preserve_external = true) {
    stage_ = "restore";
    ResetCameraLift();
    ClearSkill("restore");EndBossCalls("restore");RestoreBodyCollision();RestoreHurtVoice();ReleasePins();RetireMount("restore");
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
    AcquireSRWLockExclusive(&pose_lock_);if(pose_session_)Logf("NPC_BODY retired pose cache builds=%llu hits=%llu handContacts=%llu gripRejected=%llu handContactMaxErrorCm=%.2f",pose_session_->cache.builds,pose_session_->cache.hits,pose_session_->cache.contacts,pose_session_->cache.grip_rejected,pose_session_->cache.contact_error_max);pose_session_.reset();ReleaseSRWLockExclusive(&pose_lock_);
    display_={};source_asset_={};authored_.clear();skin_size_=1;skill_route_until_.store(0,std::memory_order_relaxed);
    foreign_weight_.store(0.0,std::memory_order_relaxed);foreign_own_.store(false,std::memory_order_relaxed);foreign_from_=foreign_to_=0;foreign_montage_=nullptr;source_driver_mode_=false;source_settings_owned_=false;source_visible_written_=source_hidden_written_=false;
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
// Identity of the hidden animation source only. Its render/tick flags are
// repaired instead: the game's skill actions (GAA_SetCharMeshVisibility*) set
// visibility on every mesh under the actor root, including this source.
bool SourceLeaseValid(const char*& why) {
    if(!source_driver_mode_)return true;
    if(!source_settings_owned_){why="source-settings-not-owned";return false;}
    if(!PoseIdentityValid()||!Same(primary_)){why="source-identity";return false;}
    if(AnimClass(primary_.object)!=original_class_.object){why="source-anim-class";return false;}
    if(!SourceBudgetReady(primary_.object)){why="source-budget-allocator";return false;}
    return true;
}
bool SourceSettingsHeld() {
    SourceSettings now;
    return SourceSettingsRead(primary_.object,now)&&!now.visible&&now.hidden&&!now.uro&&now.tick_option==0&&now.tick_enabled&&!now.cast_shadow&&!now.hidden_shadow;
}
void LogDrift(const char* who,unsigned drift) {
    // Skills toggle these many times a minute; one line per kind every 10 s.
    const ULONGLONG now=GetTickCount64();if((drift_logged_&drift)==drift&&now-drift_log_time_<10000)return;
    if(now-drift_log_time_>=10000)drift_logged_=0;drift_logged_|=drift;drift_log_time_=now;
    Logf("NPC_RETARGET game write repaired on %s flags=0x%x (1 hidden,2 hiddenShadow,4 URO,8 tickOption,16 tickEnabled,32 shown)",who,drift);
}
// bVisible and CastShadow never render a component that stays hidden in game
// without bCastHiddenShadow, so the game's visibility intent is left alone.
bool RepairSourceSettings() {
    SourceSettings now;Object* source=primary_.object;if(!SourceSettingsRead(source,now))return false;unsigned drift=0;
    if(!now.hidden){drift|=1;source_hidden_written_=true;if(!Call(source,"SetHiddenInGame").Set("NewHidden",true).Set("bPropagateToChildren",false).Invoke())return false;}
    if(now.hidden_shadow){drift|=2;if(!PutBool(source,"bCastHiddenShadow",false)||!Call(source,"SetCastShadow").Set("NewCastShadow",now.cast_shadow).Invoke())return false;}
    if(now.uro){drift|=4;if(!PutBool(source,"bEnableUpdateRateOptimizations",false))return false;}
    if(now.tick_option!=0){drift|=8;if(!Put(source,"VisibilityBasedAnimTickOption",uint8_t{0}))return false;}
    if(!now.tick_enabled){drift|=16;if(!Call(source,"SetComponentTickEnabled").Set("bEnabled",true).Invoke())return false;}
    if(drift)LogDrift("source",drift);return true;
}
bool RepairOriginalShadows() {
    for(Visibility& saved:hidden_){if(!saved.shadows_owned||!Same(saved.component))continue;bool hidden_cast=false;
        if(!Bool(saved.component.object,"bCastHiddenShadow",hidden_cast))return false;if(!hidden_cast)continue;
        if(!PutBool(saved.component.object,"bCastHiddenShadow",false)||!Call(saved.component.object,"SetCastShadow").Set("NewCastShadow",false).Invoke())return false;LogDrift("original part",2);}
    return true;
}
struct BitRef { int32_t offset=-1; uint8_t mask=0; };
BitRef ResolveBit(const char* type,const char* property) {
    BitRef bit;const uintptr_t field=Field(Class(type),property);
    if(!field||ue::At<uint8_t>(reinterpret_cast<void*>(field),0x70)!=1)return bit;const uint8_t byte=ue::At<uint8_t>(reinterpret_cast<void*>(field),0x71);
    if(byte>7)return bit;bit.offset=ue::At<int32_t>(reinterpret_cast<void*>(field),ue::layout::kPropOffset)+byte;bit.mask=ue::At<uint8_t>(reinterpret_cast<void*>(field),0x73);return bit;
}
// Every frame while active: a skill that un-hides a mesh must not show the
// original hero parts or donor-hidden NPC variants for even one frame.
void EnforceHiddenFlags() {
    static const BitRef hidden_bit=ResolveBit("/Script/Engine.SceneComponent","bHiddenInGame"),visible_bit=ResolveBit("/Script/Engine.SceneComponent","bVisible");
    if(hidden_bit.offset<0||visible_bit.offset<0||!hidden_bit.mask||!visible_bit.mask)return;
    const auto hidden=[&](Object* o){return (ue::At<uint8_t>(o,hidden_bit.offset)&hidden_bit.mask)!=0;};
    const auto visible=[&](Object* o){return (ue::At<uint8_t>(o,visible_bit.offset)&visible_bit.mask)!=0;};
    if(source_settings_owned_&&Same(primary_)&&!hidden(primary_.object)){source_hidden_written_=true;
        if(Call(primary_.object,"SetHiddenInGame").Set("NewHidden",true).Set("bPropagateToChildren",false).Invoke())LogDrift("source",1);}
    for(Visibility& saved:hidden_)if(Same(saved.component)&&!hidden(saved.component.object)){saved.hidden_written=true;
        if(Call(saved.component.object,"SetHiddenInGame").Set("NewHidden",true).Set("bPropagateToChildren",false).Invoke())LogDrift("original part",1);}
    if(face_visibility_.empty())for(const Visibility& part:authored_)if(Same(part.component)&&visible(part.component.object)&&!hidden(part.component.object)&&
        SetVisible(part.component.object,part.visible,part.hidden))LogDrift("donor-hidden NPC part",32);
    // During a boss skill: a skill action (ForceWeaponVisibility) that shows a hero weapon part is undone the same frame.
    for(const HiddenWeapon& weapon:hidden_weapons_)for(const HiddenPart& part:weapon.parts)
        if(Same(part.component)&&!hidden(part.component.object)&&
            Call(part.component.object,"SetHiddenInGame").Set("NewHidden",true).Set("bPropagateToChildren",false).Invoke())LogDrift("hero weapon during a boss skill",1);
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
    // Cutscene actors switch the body's post-process graph off. In play it drives the twist and
    // corrective bones and the sleeve / skirt physics bones that the shared skeleton never reaches;
    // without it those bones hold the rest pose (stiff sleeves, wrists that do not twist).
    Object* body_post=Property<Object*>(target,"PostProcessAnimBlueprint");
    const bool post_enabled=options.disabled&&!options.override_class&&body_post;
    if(post_enabled)options.disabled=false;
    Logf("NPC_RETARGET body post-process class=%s donorDisabled=%d enabledForPlay=%d",Text(body_post).c_str(),options.disabled||post_enabled?1:0,post_enabled?1:0);
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
    if(!Property<Object*>(donor,"PhysicsAssetOverride")&&!Property<Object*>(target,"PhysicsAsset")){
        // No physics bodies: the engine keeps the mesh's reference-pose bounds,
        // so widen them rather than let animated limbs leave the culling volume.
        Call scale(display,"SetBoundsScale");const bool widened=scale.Set("NewBoundsScale",kNoPhysicsBoundsScale).Invoke();
        Logf("NPC_RETARGET display has no physics asset; boundsScale=%.1f applied=%d",kNoPhysicsBoundsScale,widened?1:0);}
    if(!MatchPlayerLighting(context.mesh,display)||!HasNoLeader(display)||!VerifyPostProcess(display,options,"NPC-display")||!InstallFaceHook(display,profile,true)){
        Restore();return Fail(L"NPC 显示的独立 CopyPose / 照明校验失败，已请求恢复");}
    QuietAimNodes(display);if(mount_.row)CreateMountPuppet(context,display,target);
    stage_="assemble NPC parts";Context display_context=context;display_context.mesh=display;face_wait_started_=GetTickCount64();
    if(!Assemble(display_context,donor,profile,parts)||!InstallGripHook(context.mesh,profile)||(BuildDisplayRoutes(context.mesh,display),!SetVisible(display,true,false))){
        Restore();return Fail(L"NPC 部件 / 原生武器适配 / 源姿势持续更新未通过，已请求恢复");}
    const char* lease_why=nullptr;
    if(!SourceLeaseValid(lease_why)||!SourceSettingsHeld()){Restore();return Fail(L"主角源动画 / 隐藏更新状态未通过读回，已请求恢复");}
    LARGE_INTEGER seed{};QueryPerformanceCounter(&seed);skill_round_.Reset(static_cast<uint64_t>(seed.QuadPart)^(GetTickCount64()<<32));pending_={};
    foreign_weight_.store(0.0,std::memory_order_relaxed);foreign_own_.store(false,std::memory_order_relaxed);foreign_from_=foreign_to_=0;foreign_montage_=nullptr;
    if(resume_entry_>=0)Logf("NPC_RETARGET resumed id=%s after a scene / player change (try %u)",profile.id,resume_tries_);
    resume_entry_=-1;resume_at_=0;resume_tries_=0;
    active_=true;cleanup_pending_=false;selected_=entry;Publish();stage_="idle";ApplyBodyCollision(context,entry);ApplyHurtVoice(context,entry);ApplyCameraLift(context.pawn);
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
// A skin the map change (or a new player pawn) took off: remembered for the next scene. A restore the
// player asks for, another skin, the experiment switch or a hero outfit forgets it.
void KeepForNextScene(const char* why) {
    if (!active_ || selected_ < 0) return;
    if (resume_entry_ != selected_) { resume_tries_ = 0; Logf("NPC_RETARGET %s: id=%s is put back once the next scene's player is ready", why, kProfiles[selected_].id); }
    resume_entry_ = selected_; resume_at_ = 0;
}
void ForgetResume(const char* why) {
    if (resume_entry_ < 0) return;
    Logf("NPC_RETARGET resume of id=%s dropped: %s", kProfiles[resume_entry_].id, why);
    resume_entry_ = -1; resume_at_ = 0; resume_tries_ = 0;
}
void ResumeAfterScene(bool context_ok, bool disabled) {
    if (active_ || cleanup_pending_ || HasResources() || disabled || !InterlockedCompareExchange(&experimental_, 0, 0)) { resume_at_ = 0; return; }
    const ULONGLONG now = GetTickCount64();
    if (!context_ok) { resume_at_ = 0; return; }
    if (!resume_at_) { resume_at_ = now + kResumeSettleMs; return; }
    if (now < resume_at_) return;
    if (resume_tries_ >= kResumeTries) { Status(L"新场景中重新套用 NPC %s 失败 %u 次，请在菜单里手动选择", kProfiles[resume_entry_].name, resume_tries_); ForgetResume("tries-exhausted"); return; }
    ++resume_tries_; resume_at_ = 0; // a failed try waits kResumeSettleMs again once its idle wait is over
    waiting_action_ = static_cast<int32_t>(Action::Apply); waiting_entry_ = resume_entry_; wait_deadline_ = now + 60000;
    Status(L"新场景：等主角空闲后重新套用 NPC %s", kProfiles[resume_entry_].name);
    Logf("NPC_RETARGET resume queued id=%s try=%u/%u", kProfiles[resume_entry_].id, resume_tries_, kResumeTries);
}
// ---- Exact casts and the runtime audit -----------------------------------------------------------
// npcretarget cast <n> / the audit: skill n of the skin, on the nearest enemy; a refused one is tried again
// for kSkillWaitMs, never another one.
bool CastExact(size_t pick) {
    if(!active_||cleanup_pending_||!source_driver_mode_||selected_<0||static_cast<size_t>(selected_)>=std::size(kNpcSkillSets))return false;
    const NpcSkillSet& set=kNpcSkillSets[selected_];if(pick>=set.skill_count)return false;
    WatchSkill();if(skill_.handle>0||pending_.handle>0)return false;
    Context context;Object* asc=nullptr;
    if(!ContextNow(context)||context.pawn!=pawn_.object||!ue::Alive(asc=PawnAsc(context.pawn))||!GrantSkill(asc,set,pick,1))return false;
    pending_.exact=true;TryPendingSkill(context);return true;
}
constexpr char kAuditDummy[]="/PBZCombatAsset/ActorData/Mobs/003_MaleThug/BP_Monster_MaleThug_Sword_Blue";
constexpr double kAuditDummyCm=350,kAuditCloseCm=150,kAuditFxOffsetCm=30;constexpr ULONGLONG kAuditSkillMs=20000,kAuditSkinMs=75000,kAuditScanMs=250;constexpr size_t kAuditFxMax=256;
// npcretarget audit speed <x>: run the audit's cast at x times normal flow speed. Almost all of a skill's wall
// time is its montage playing out (median 4.7 s of which the effect scan is 7 ms), so scaling the world's time
// dilation is the only honest speed-up: every animation, bullet and effect still happens, just sooner. Scans and
// the settle waits scale with it too, so nothing is sampled less often in game time. 1 = off (the default).
Object* audit_speed_world_=nullptr;float audit_speed_saved_=1.0f;
struct AuditRotator { double pitch=0,yaw=0,roll=0; };
Object* Owner(Object* component) { Call get(component,"GetOwner");return get.Invoke()?get.Get<Object*>("ReturnValue"):nullptr; }
bool AuditDead(Object* pawn) {
    Call dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"),"IsDead");
    return !ue::Alive(pawn)||!dead.Set("Pawn",pawn).Invoke()||dead.Get<bool>("ReturnValue");
}
// The audit scan's kind of a class, by the class object's index: 1 an effect component (FXSystemComponent),
// 2 a bullet (a native class named *Bullet, PBZ_SweepTraceBullet ..., not an ability action or AI task), 0 else,
// -1 not looked at yet. Reset for every skin (a freed class's index may be reused).
std::vector<int8_t> audit_kinds_;Object* audit_fx_class_=nullptr;std::vector<Object*> audit_bullet_bases_;
void AuditInitKinds() {
    audit_kinds_.assign(static_cast<size_t>(ue::detail::ObjectCount())+65536,int8_t{-1});audit_fx_class_=Class("/Script/Engine.FXSystemComponent");
    if(!audit_bullet_bases_.empty())return;
    Object* meta=Class("/Script/CoreUObject.Class");
    ue::ForEachObject([&](Object* o){
        if(ue::ClassOf(o)!=meta||!ue::Alive(o))return true;
        const std::string n=Text(o);
        if(n.size()>6&&!n.compare(n.size()-6,6,"Bullet")&&n.rfind("GAA",0)&&n.rfind("LGA",0)&&n.rfind("BT",0))audit_bullet_bases_.push_back(o);
        return audit_bullet_bases_.size()<64;});
    std::string names;for(Object* b:audit_bullet_bases_)if(names.size()<300)names+=(names.empty()?"":",")+Text(b);
    Logf("NPC_AUDIT bullet bases %zu: %s",audit_bullet_bases_.size(),names.empty()?"-":names.c_str());
}
int AuditKind(Object* cls) {
    if(!cls)return 0;const int32_t index=ue::At<int32_t>(cls,ue::layout::kObjIndex);if(index<0)return 0;
    if(static_cast<size_t>(index)>=audit_kinds_.size())audit_kinds_.resize(static_cast<size_t>(index)+65536,int8_t{-1});
    int8_t& kind=audit_kinds_[static_cast<size_t>(index)];if(kind>=0)return kind;
    kind=0;
    for(Object* c=cls;c&&!kind;c=ue::At<Object*>(c,ue::layout::kSuper)){
        if(c==audit_fx_class_)kind=1;
        else if(std::find(audit_bullet_bases_.begin(),audit_bullet_bases_.end(),c)!=audit_bullet_bases_.end())kind=2;
    }
    return kind;
}
// Reflected fields of one object as "name=value" text (structs two levels deep), for runtime digs into native
// data the cooked packages store unversioned. Property ids are the FFieldClass cast ids (Byte 0x40, Int 0x80 ...).
void AppendFields(std::string& out,Object* type,const unsigned char* base,int depth,const std::string& prefix) {
    for(Object* s=type;ue::Alive(s)&&out.size()<4000;s=ue::At<Object*>(s,ue::layout::kSuper))
        for(Object* p=ue::At<Object*>(s,ue::layout::kChildProperties);p&&out.size()<4000;p=ue::At<Object*>(p,ue::layout::kPropNext)){
            const uint64_t kind=ue::At<uint64_t>(ue::At<Object*>(p,8),0x18);const int32_t off=ue::At<int32_t>(p,ue::layout::kPropOffset);
            const unsigned char* at=base+off;std::string name=prefix+Text(ue::At<Name>(p,ue::layout::kPropName)),value;char number[64]{};
            switch(kind){
            case 0x20000:{const uint8_t byte=ue::At<uint8_t>(p,0x71),mask=ue::At<uint8_t>(p,0x73);value=(at[byte]&mask)?"1":"0";break;}
            case 0x40:case 0x1000000000000ull:_snprintf_s(number,sizeof(number),_TRUNCATE,"%u",unsigned(at[0]));value=number;break;
            case 0x80:_snprintf_s(number,sizeof(number),_TRUNCATE,"%d",*reinterpret_cast<const int32_t*>(at));value=number;break;
            case 0x100:_snprintf_s(number,sizeof(number),_TRUNCATE,"%g",double(*reinterpret_cast<const float*>(at)));value=number;break;
            case 0x100000000ull:_snprintf_s(number,sizeof(number),_TRUNCATE,"%g",*reinterpret_cast<const double*>(at));value=number;break;
            case 0x400000:_snprintf_s(number,sizeof(number),_TRUNCATE,"%lld",*reinterpret_cast<const long long*>(at));value=number;break;
            case 0x2000:value=Text(*reinterpret_cast<const Name*>(at));break;
            case 0x10000:case 0x400:case 0x1000:{Object* o=*reinterpret_cast<Object* const*>(at);value=ue::Alive(o)?Text(o):o?"dead":"null";break;}
            case 0x200000:case 0x800000000000ull:case 0x400000000000ull:_snprintf_s(number,sizeof(number),_TRUNCATE,"[%d]",*reinterpret_cast<const int32_t*>(at+8));value=number;break;
            case 0x100000:{Object* st=ue::At<Object*>(p,0x70);
                if(depth<2&&ue::Alive(st)){out+=" "+name+"{";AppendFields(out,st,at,depth+1,"");out+=" }";continue;}
                value="struct";break;}
            default:continue; // text, soft and weak pointers, delegates
            }
            out+=" "+name+"="+value;
        }
}
void DumpObject(const char* label,Object* object) {
    if(!ue::Alive(object))return;
    std::string out;AppendFields(out,ue::ClassOf(object),reinterpret_cast<const unsigned char*>(object),0,"");
    for(size_t at=0,part=0;at<out.size()&&part<6;at+=850,++part)
        Logf("NPC_AUDIT_DUMP %s %s(%s) part=%zu:%s",label,Text(object).c_str(),Text(ue::ClassOf(object)).c_str(),part,out.substr(at,850).c_str());
}
// A weapon and the data / config objects it points at: where its default particles and trails are set.
void DumpWeapon(const char* label,Object* weapon) {
    if(!ue::Alive(weapon))return;
    DumpObject(label,weapon);
    for(Object* s=ue::ClassOf(weapon);ue::Alive(s);s=ue::At<Object*>(s,ue::layout::kSuper))
        for(Object* p=ue::At<Object*>(s,ue::layout::kChildProperties);p;p=ue::At<Object*>(p,ue::layout::kPropNext)){
            if(ue::At<uint64_t>(ue::At<Object*>(p,8),0x18)!=0x10000)continue;
            const std::string name=Text(ue::At<Name>(p,ue::layout::kPropName));
            if(name.find("Data")==std::string::npos&&name.find("Config")==std::string::npos)continue;
            Object* linked=ue::At<Object*>(weapon,ue::At<int32_t>(p,ue::layout::kPropOffset));
            if(ue::Alive(linked))DumpObject((std::string(label)+"."+name).c_str(),linked);
        }
}
// The hero part an effect hangs on against the NPC part that has the same socket (body, then boss weapons /
// attached parts): their socket distance. Hero sockets the grip routes drive sit on the NPC's within ~2 cm.
double AuditFxOffset(Object* effect) {
    Object* parent=Property<Object*>(effect,"AttachParent");if(!ue::Alive(parent)||!Same(display_))return -1;
    const Name socket=Property<Name>(effect,"AttachSocketName");
    Object* npc=socket.None()||HasSocket(display_.object,socket)?display_.object:BossSocketHolder(socket);
    if(!npc)return -1;
    const auto where=[&](Object* c,Vector& v){Call at(c,socket.None()?"K2_GetComponentLocation":"GetSocketLocation");if(!socket.None())at.Set("InSocketName",socket);
        if(!at.Invoke())return false;v=at.Get<Vector>("ReturnValue");return true;};
    Vector a,b;if(!where(parent,a)||!where(npc,b))return -1;
    return std::sqrt((a.x-b.x)*(a.x-b.x)+(a.y-b.y)*(a.y-b.y)+(a.z-b.z)*(a.z-b.z));
}
const char* AuditAnchor(Object* effect) {
    Object* parent=Property<Object*>(effect,"AttachParent");if(!ue::Alive(parent))return Owner(effect)==pawn_.object?"pawn":"world";
    for(size_t depth=0;ue::Alive(parent)&&depth<16;++depth,parent=Property<Object*>(parent,"AttachParent")){
        if(Same(display_)&&parent==display_.object)return "display";
        for(const Identity& w:skill_weapons_)if(Same(w)&&w.object==parent)return "bossWeapon";
        if(Same(primary_)&&parent==primary_.object)return "heroMesh";
        Object* owner=Owner(parent);
        for(const HiddenWeapon& w:hidden_weapons_)if(Same(w.actor)&&owner==w.actor.object)return "heroWeapon";
        if(Same(audit_.dummy)&&owner==audit_.dummy.object)return "dummy";
    }
    return Owner(effect)==pawn_.object?"pawn":"other";
}
// Effect components active (and bullet objects alive) now: baseline before a cast, else the new ones recorded.
void AuditScan(bool baseline) {
    static const BitRef active=ResolveBit("/Script/Engine.ActorComponent","bIsActive"),hidden=ResolveBit("/Script/Engine.SceneComponent","bHiddenInGame"),
        visible=ResolveBit("/Script/Engine.SceneComponent","bVisible");
    if(active.offset<0||!Same(pawn_))return;
    LARGE_INTEGER t0{},t1{},f{};QueryPerformanceCounter(&t0);
    std::vector<Object*> effects,bullets;
    ue::ForEachObject([&](Object* o){
        const int kind=AuditKind(ue::ClassOf(o));if(!kind||ue::IsTemplate(o))return true;
        if(kind==1){if(ue::At<uint8_t>(o,active.offset)&active.mask)effects.push_back(o);}else bullets.push_back(o);
        return effects.size()<4096&&bullets.size()<4096;});
    const auto known=[](const std::vector<Identity>& list,Object* o){return std::any_of(list.begin(),list.end(),[&](const Identity& i){return i.object==o&&Same(i);});};
    if(baseline){audit_.fx_base.clear();audit_.bullet_base.clear();for(Object* e:effects)audit_.fx_base.push_back(Id(e));for(Object* b:bullets)audit_.bullet_base.push_back(Id(b));}
    else{
        Object* niagara=Class("/Script/Niagara.NiagaraComponent");
        for(Object* e:effects){
            if(audit_.fx.size()>=kAuditFxMax||!ue::Alive(e)||known(audit_.fx_base,e)||std::any_of(audit_.fx.begin(),audit_.fx.end(),[&](const AuditFx& x){return x.effect.object==e&&Same(x.effect);}))continue;
            {Call where(e,"K2_GetComponentLocation");if(!where.Invoke())continue;const Vector v=where.Get<Vector>("ReturnValue");
                const double dx=v.x-audit_.home.x,dy=v.y-audit_.home.y,dz=v.z-audit_.home.z;if(dx*dx+dy*dy+dz*dz>4000.0*4000.0)continue;} // ambient effects elsewhere
            AuditFx x;x.effect=Id(e);x.anchor=AuditAnchor(e);x.asset=Text(Property<Object*>(e,niagara&&ue::IsA(e,niagara)?"Asset":"Template"));x.socket=Text(Property<Name>(e,"AttachSocketName"));
            x.hidden=(hidden.offset>=0&&(ue::At<uint8_t>(e,hidden.offset)&hidden.mask))||(visible.offset>=0&&!(ue::At<uint8_t>(e,visible.offset)&visible.mask));
            if(!std::strcmp(x.anchor,"heroMesh")||!std::strcmp(x.anchor,"heroWeapon"))x.offset_cm=AuditFxOffset(e);
            audit_.fx.push_back(x);
        }
        for(Object* b:bullets)if(audit_.bullets.size()<kAuditFxMax&&ue::Alive(b)&&!known(audit_.bullet_base,b)&&
            std::none_of(audit_.bullets.begin(),audit_.bullets.end(),[&](const auto& x){return x.first.object==b&&Same(x.first);})){
            audit_.bullets.push_back({Id(b),Text(ue::ClassOf(b))});
            // A sweep bullet whose data names a display index the hero has no weapon for gets no scan source
            // at all: it deals no damage and its weapon effects have no holder. Record that index per skill
            // (OriginType, WeaponIndex) so a report can tell it from a reach or collider problem. Only the
            // first few bullets: the check costs a native call per bullet.
            if(audit_.bullets.size()<=4)for(const char* field:{"BulletData","Data","BulletDataAsset"})if(Object* data=Property<Object*>(b,field);ue::Alive(data)){
                const int32_t origin=Offset(data,"OriginType"),index=Offset(data,"WeaponIndex");
                if(origin>=0&&index>=0){const uint8_t kind=ue::At<uint8_t>(data,origin);const int32_t weapon=ue::At<int32_t>(data,index);
                    if((kind==1||weapon>0)&&SlotWeaponActors(pawn_.object,weapon).empty()&&audit_.weapon_gaps.size()<8){
                        char text[64]{};_snprintf_s(text,sizeof(text),_TRUNCATE,"%d/%d",static_cast<int>(kind),weapon);
                        if(std::find(audit_.weapon_gaps.begin(),audit_.weapon_gaps.end(),text)==audit_.weapon_gaps.end())audit_.weapon_gaps.push_back(text);}}
                break;}
            // The close retry of a skill that missed: what its first bullets read their colliders from.
            if(audit_.close_retry&&audit_.bullets.size()<=2){DumpObject("bullet",b);
                for(const char* field:{"BulletData","Data","BulletDataAsset"})if(Object* data=Property<Object*>(b,field);ue::Alive(data)){DumpObject("bulletData",data);break;}}}
    }
    QueryPerformanceCounter(&t1);QueryPerformanceFrequency(&f);audit_.scan_ms=(std::max)(audit_.scan_ms,1000.0*double(t1.QuadPart-t0.QuadPart)/double(f.QuadPart));
}
bool AuditTeleport(Object* actor,const Vector& at,double yaw) {
    Call tp(actor,"K2_TeleportTo");
    if(tp.Set("DestLocation",at).Set("DestRotation",AuditRotator{0,yaw,0}).Invoke()&&tp.Get<bool>("ReturnValue"))return true;
    Call set(actor,"K2_SetActorLocationAndRotation");
    return set.Set("NewLocation",at).Set("NewRotation",AuditRotator{0,yaw,0}).Set("bSweep",false).Set("bTeleport",true).Invoke();
}
Vector AuditAhead(double distance=kAuditDummyCm) {
    const double r=audit_.home_yaw*3.14159265358979323846/180.0;
    return Vector{audit_.home.x+std::cos(r)*distance,audit_.home.y+std::sin(r)*distance,audit_.home.z+5};
}
// The hero back on its start spot facing the dummy; the dummy frozen there (it and its controller at time
// dilation 0, so it never attacks or walks off), at full health. A grab's victim must play its own part: live.
void AuditPlace(const Context& context,double distance=kAuditDummyCm,bool live=false) {
    AuditTeleport(context.pawn,audit_.home,audit_.home_yaw);
    if(Object* controller=Property<Object*>(context.pawn,"Controller"))Call(controller,"SetControlRotation").Set("NewRotation",AuditRotator{-10,audit_.home_yaw,0}).Invoke();
    EntryProtection::Protect(context.pawn);
    if(!Same(audit_.dummy))return;
    Object* dummy=audit_.dummy.object;AuditTeleport(dummy,AuditAhead(distance),audit_.home_yaw+180);
    for(Object* actor:{dummy,Property<Object*>(dummy,"Controller")}){const int32_t at=Offset(actor,"CustomTimeDilation");if(ue::Alive(actor)&&at>=0)ue::At<float>(actor,at)=live?1.0f:0.0f;}
    GrabExtras::ChangeHealth(dummy,1.0e7f);
    if(Object* targeting=TargetingComponent(context.pawn))Call(targeting,"StopLockTarget").Invoke(); // the next cast locks the nearest: the dummy
}
bool AuditSpawnDummy(const Context& context) {
    Object* cls=LoadClassPackage(kAuditDummy),*library_class=LoadClassPackage("/SGameAI/FunctionLibrary/BP_SGameAIFunctionLibrary");
    Object* library=library_class?DefaultObject(library_class):nullptr;
    if(!cls||!library){Logf("NPC_AUDIT dummy class=%d library=%d; no dummy",cls?1:0,library?1:0);return false;}
    if(!NpcRetarget::BeforeActorClassSpawn(cls))return false;
    Transform at;RotatorQuat(0,audit_.home_yaw+180,0,at.rotation);const Vector p=AuditAhead();at.translation[0]=p.x;at.translation[1]=p.y;at.translation[2]=p.z+60;
    Call spawn(library,"SpawnAI");
    if(!spawn.Set("Class",cls).Set("SpawnTransform",at).Set("__WorldContext",context.world).Invoke())return false;
    Object* actor=spawn.Get<Object*>("ReturnValue");audit_.dummy=Id(actor);
    Logf("NPC_AUDIT dummy spawned actor=%s class=%s",Text(actor).c_str(),Text(cls).c_str());
    return Same(audit_.dummy);
}
void AuditResetSkill() {
    audit_.result.clear();audit_.montage.clear();audit_.weapons_shown=audit_.weapons_expected=audit_.colliders=audit_.effects_moved=audit_.stand_ins=0;
    audit_.hits=audit_.dummy_hits=audit_.targets=audit_.weapon_swaps=0;audit_.native=audit_.dummy_damage=0;audit_.started=false;audit_.cast_at=0;audit_.scan_ms=0;
    audit_.fx.clear();audit_.bullets.clear();audit_.weapon_gaps.clear();
}
void AuditReport() {
    const NpcSkillSet& set=kNpcSkillSets[audit_.profile];
    std::map<std::string,size_t> anchors;size_t hidden=0,misplaced=0;std::string assets,bullet_types,hero;
    for(const AuditFx& x:audit_.fx){++anchors[x.anchor];hidden+=x.hidden?1:0;
        const bool on_hero=!std::strcmp(x.anchor,"heroMesh")||!std::strcmp(x.anchor,"heroWeapon"),off=on_hero&&(x.offset_cm<0||x.offset_cm>kAuditFxOffsetCm);
        misplaced+=off?1:0;
        if(off&&hero.size()<300){char cm[24]{};if(x.offset_cm<0)strcpy_s(cm,"~none");else _snprintf_s(cm,sizeof(cm),_TRUNCATE,"~%.0fcm",x.offset_cm);
            hero+=(hero.empty()?"":"|")+x.asset+"@"+x.anchor+":"+x.socket+cm;}
        if(assets.size()<400&&assets.find(x.asset)==std::string::npos)assets+=(assets.empty()?"":"|")+x.asset;}
    for(const auto& b:audit_.bullets)if(bullet_types.size()<300&&bullet_types.find(b.second)==std::string::npos)bullet_types+=(bullet_types.empty()?"":"|")+b.second;
    const auto n=[&](const char* a){const auto f=anchors.find(a);return f==anchors.end()?size_t{0}:f->second;};
    const std::string ability=audit_.skill<set.skill_count?set.skills[audit_.skill]:"-";
    // Two lines, each well under the logger's 1 KB: the names first, then the counts the runner keys on.
    Logf("NPC_AUDIT_SKILL_FX profile=%zu skill=%zu/%zu heroFx=%s assets=%s bulletClasses=%s",audit_.profile,audit_.skill+1,set.skill_count,hero.empty()?"-":hero.c_str(),
        assets.empty()?"-":assets.c_str(),bullet_types.empty()?"-":bullet_types.c_str());
    std::string gaps;for(const std::string& g:audit_.weapon_gaps)if(gaps.size()<80)gaps+=(gaps.empty()?"":",")+g;
    Logf("NPC_AUDIT_SKILL profile=%zu id=%s skill=%zu/%zu ability=%s result=%s ms=%llu montage=%s weapons=%zu/%zu colliders=%zu fx=%zu fxDisplay=%zu fxBossWeapon=%zu "
         "fxHeroMesh=%zu fxHeroWeapon=%zu fxMisplaced=%zu fxPawn=%zu fxWorld=%zu fxOther=%zu fxDummy=%zu fxHidden=%zu moved=%zu bullets=%zu hits=%u dummyHits=%u dummyDamage=%.0f targets=%u "
         "weaponSwaps=%u dummyCm=%.0f closeRetry=%d scanMs=%.1f standIns=%zu weaponGaps=%s",audit_.profile,set.profile,audit_.skill+1,set.skill_count,ability.substr(ability.find_last_of('/')+1).c_str(),
        audit_.result.empty()?"ok":audit_.result.c_str(),audit_.started?GetTickCount64()-audit_.cast_at:0ull,audit_.montage.empty()?"-":audit_.montage.c_str(),
        audit_.weapons_shown,audit_.weapons_expected,audit_.colliders,audit_.fx.size(),n("display"),n("bossWeapon"),n("heroMesh"),n("heroWeapon"),misplaced,n("pawn"),n("world"),n("other"),n("dummy"),
        hidden,audit_.effects_moved,audit_.bullets.size(),audit_.hits,audit_.dummy_hits,audit_.dummy_damage,audit_.targets,audit_.weapon_swaps,audit_.dummy_cm,audit_.close_retry?1:0,audit_.scan_ms,audit_.stand_ins,
        gaps.empty()?"-":gaps.c_str());
    ++audit_.skills;
}
void AuditNextSkin() { ++audit_.profile;audit_.skill=0;audit_.close_retry=false;audit_.requested=false;audit_.placed=false;audit_.phase=AuditState::Phase::Skin; }
// World time dilation for the audit's run speed. WorldSettings holds WorldToMeters then TimeDilation as floats
// (1.0 by default); read/write them by the reflected field rather than a raw offset. The pre-audit value is put
// back when the audit ends or is stopped, so the player's game runs at normal speed afterwards.
bool SetAuditTimeDilation(Object* world,float value) {
    if(!ue::Alive(world))return false;
    Object* level=Property<Object*>(world,"PersistentLevel"),*cls=Class("/Script/Engine.WorldSettings");
    if(!ue::Alive(level)||!cls)return false;
    Object* settings=nullptr;
    ue::ForEachObject([&](Object* o){
        if(ue::Alive(o)&&!ue::IsTemplate(o)&&ue::OuterOf(o)==level&&ue::IsA(o,cls)){settings=o;return false;}return true;});
    const int32_t at=settings?Offset(settings,"TimeDilation"):-1;
    if(at<0||!FaceReadable(settings+at,4))return false;
    if(audit_speed_world_!=world){audit_speed_saved_=ue::At<float>(settings,at);audit_speed_world_=world;}
    ue::At<float>(settings,at)=value;return true;
}
void RestoreAuditTimeDilation() {
    if(!audit_speed_world_)return;
    SetAuditTimeDilation(audit_speed_world_,audit_speed_saved_>0.01f?audit_speed_saved_:1.0f);
    Logf("NPC_AUDIT time dilation restored to %.2f",audit_speed_saved_);
    audit_speed_world_=nullptr;audit_speed_saved_=1.0f;
}
// The next skill to cast: the following one, or under audit list the next listed one (past the last: next skin).
void AuditAdvance() {
    audit_.close_retry=false;
    if(audit_.only.empty()){++audit_.skill;return;}
    const auto next=std::upper_bound(audit_.only.begin(),audit_.only.end(),audit_.skill);
    audit_.skill=next==audit_.only.end()?size_t{1}<<20:*next;
}
void AuditStop(const char* why) { if(audit_.stop.empty())audit_.stop=why;audit_.phase=AuditState::Phase::Finish; }
// ---- npcretarget audit export: the skills' data for the offline lint ------------------------------
// Loads every skill class of the chosen skins (no cast, nothing pinned) and writes, one JSON object per line, the
// reflected fields of its class default and of the objects under it (timelines and their actions), of the bullet
// data / blueprint classes those name (three hops), of the boss weapon and body blueprints with their component
// templates, and of the hero's current weapon actors, to PBZAuditExport.jsonl beside the log. Cooked packages
// store properties unversioned, so the game's own reflection is the schema. Zero / None / empty fields are left
// out. Runs a slice of the work per frame on the game thread; nothing is held across frames but path strings.
struct ExportState {
    bool on=false,skin_done=false;size_t skin=0,end=0,skill=0,objects=0,classes=0,missing=0,bytes=0;
    HANDLE file=INVALID_HANDLE_VALUE;ULONGLONG t0=0,skin_t0=0;std::set<std::string> written;
};
ExportState export_;
constexpr ULONGLONG kExportSliceMs=60;constexpr size_t kExportObjectBytes=60000;
std::string ObjectPath(Object* object) {
    std::vector<Object*> chain;for(Object* o=object;o&&chain.size()<16;o=ue::OuterOf(o))chain.push_back(o);
    std::string out;
    for(size_t i=chain.size();i-->0;){if(i+1<chain.size())out+=i+3==chain.size()?':':'.';out+=Text(chain[i]);}
    return out;
}
void JsonText(std::string& out,const std::string& text) {
    out+='"';
    for(const char c:text){
        if(c=='"'||c=='\\'){out+='\\';out+=c;}
        else if(static_cast<unsigned char>(c)<0x20){char e[8]{};_snprintf_s(e,sizeof(e),_TRUNCATE,"\\u%04x",unsigned(c));out+=e;}
        else out+=c;
    }
    out+='"';
}
uint64_t FieldKind(Object* property) { return ue::At<uint64_t>(ue::At<Object*>(property,8),0x18); }
bool JsonValue(std::string& out,Object* property,const unsigned char* at,int depth,std::vector<Object*>& refs);
void JsonStruct(std::string& out,Object* type,const unsigned char* base,int depth,std::vector<Object*>& refs) {
    out+='{';bool first=true;
    for(Object* s=type;ue::Alive(s)&&out.size()<kExportObjectBytes;s=ue::At<Object*>(s,ue::layout::kSuper))
        for(Object* p=ue::At<Object*>(s,ue::layout::kChildProperties);p&&out.size()<kExportObjectBytes;p=ue::At<Object*>(p,ue::layout::kPropNext)){
            const size_t mark=out.size();if(!first)out+=',';
            JsonText(out,Text(ue::At<Name>(p,ue::layout::kPropName)));out+=':';
            if(JsonValue(out,p,base+ue::At<int32_t>(p,ue::layout::kPropOffset),depth,refs))first=false;else out.resize(mark);
        }
    out+='}';
}
// One property value; false (nothing written) for zero, None, null, empty and the kinds left out (text, weak
// pointers, delegates, sets). Arrays write null for such an element so positions stay.
bool JsonValue(std::string& out,Object* property,const unsigned char* at,int depth,std::vector<Object*>& refs) {
    const uint64_t kind=FieldKind(property);const int32_t size=ue::At<int32_t>(property,ue::layout::kPropElementSize);char number[64]{};
    auto integer=[&](long long value){if(!value)return false;_snprintf_s(number,sizeof(number),_TRUNCATE,"%lld",value);out+=number;return true;};
    auto real=[&](double value){if(value==0.0||!std::isfinite(value))return false;_snprintf_s(number,sizeof(number),_TRUNCATE,"%.9g",value);out+=number;return true;};
    switch(kind){
    case 0x20000:{const uint8_t byte=ue::At<uint8_t>(property,0x71),mask=ue::At<uint8_t>(property,0x73);if(!(at[byte]&mask))return false;out+="true";return true;}
    case 0x40:return integer(at[0]);
    case 0x2:return integer(*reinterpret_cast<const int8_t*>(at));
    case 0x80000000:return integer(*reinterpret_cast<const int16_t*>(at));
    case 0x40000:return integer(*reinterpret_cast<const uint16_t*>(at));
    case 0x80:return integer(*reinterpret_cast<const int32_t*>(at));
    case 0x800:return integer(*reinterpret_cast<const uint32_t*>(at));
    case 0x400000:return integer(*reinterpret_cast<const long long*>(at));
    case 0x200:return integer(static_cast<long long>(*reinterpret_cast<const unsigned long long*>(at)));
    case 0x1000000000000ull:{long long value=0;if(size==1)value=at[0];else if(size==2)value=*reinterpret_cast<const int16_t*>(at);
        else if(size==4)value=*reinterpret_cast<const int32_t*>(at);else if(size==8)value=*reinterpret_cast<const long long*>(at);return integer(value);}
    case 0x100:return real(*reinterpret_cast<const float*>(at));
    case 0x100000000ull:return real(*reinterpret_cast<const double*>(at));
    case 0x2000:{const Name name=*reinterpret_cast<const Name*>(at);if(name.None())return false;JsonText(out,Text(name));return true;}
    case 0x4000:{const ue::String text=*reinterpret_cast<const ue::String*>(at);
        if(!text.data||text.num<=1||text.num>4096||!FaceReadable(text.data,size_t(text.num)*2))return false;JsonText(out,Utf8(text.data));return true;}
    case 0x10000:case 0x400:case 0x1000:{Object* o=*reinterpret_cast<Object* const*>(at);if(!o)return false;
        if(!ue::Alive(o)){out+="\"<dead>\"";return true;}JsonText(out,ObjectPath(o));refs.push_back(o);return true;}
    case 0x20000000:case 0x200000000ull:{
        // FSoftObjectPtr: weak pointer, then FSoftObjectPath (package + asset names from UE 5.1 on, one path before).
        const Name a=*reinterpret_cast<const Name*>(at+8),b=*reinterpret_cast<const Name*>(at+16);if(a.None())return false;
        std::string path=Text(a);if(!path.empty()&&path[0]=='/'&&!b.None())path+="."+Text(b);JsonText(out,path);return true;}
    case 0x100000:{Object* type=ue::At<Object*>(property,0x70);const std::string type_name=Text(type);
        if(type_name=="GameplayTag"){const Name tag=*reinterpret_cast<const Name*>(at);if(tag.None())return false;JsonText(out,Text(tag));return true;}
        if(type_name=="GameplayTagContainer"){const ue::Array tags=*reinterpret_cast<const ue::Array*>(at);
            if(tags.num<=0||tags.num>512||!FaceReadable(tags.data,size_t(tags.num)*sizeof(Name)))return false;
            out+='[';for(int32_t i=0;i<tags.num;++i){if(i)out+=',';JsonText(out,Text(static_cast<const Name*>(tags.data)[i]));}out+=']';return true;}
        if(depth>=6||!ue::Alive(type))return false;
        std::string inner;JsonStruct(inner,type,at,depth+1,refs);if(inner=="{}")return false;out+=inner;return true;}
    case 0x200000:{Object* inner=ue::At<Object*>(property,0x78);const ue::Array list=*reinterpret_cast<const ue::Array*>(at);
        const int32_t stride=inner?ue::At<int32_t>(inner,ue::layout::kPropElementSize):0;
        if(list.num<=0||depth>=6)return false;
        if(!inner||stride<=0||stride>65536||list.num>65536||!FaceReadable(list.data,size_t(list.num)*size_t(stride))){out+="\"<bad array>\"";return true;}
        out+='[';
        for(int32_t i=0;i<list.num&&i<512&&out.size()<kExportObjectBytes;++i){if(i)out+=',';
            if(!JsonValue(out,inner,static_cast<const unsigned char*>(list.data)+size_t(i)*size_t(stride),depth+1,refs))out+="null";}
        out+=']';return true;}
    case 0x400000000000ull:{ // TMap as [[key, value], ...] (FMapProperty: key 0x70, value 0x78, value offset 0x80, row stride 0x8c)
        Object* key=ue::At<Object*>(property,0x70),*value=ue::At<Object*>(property,0x78);if(!key||!value||depth>=6)return false;
        const int32_t value_offset=ue::At<int32_t>(property,0x80),stride=ue::At<int32_t>(property,0x8c);
        const int32_t count=*reinterpret_cast<const int32_t*>(at+8),bits=*reinterpret_cast<const int32_t*>(at+40);
        if(count<=0)return false;
        const unsigned char* data=*reinterpret_cast<unsigned char* const*>(at);const uint32_t* allocation=*reinterpret_cast<uint32_t* const*>(at+32);
        if(!allocation)allocation=reinterpret_cast<const uint32_t*>(at+16);
        if(stride<8||stride>4096||value_offset<0||value_offset>=stride||count>4096||bits!=count||
            !FaceReadable(data,size_t(count)*size_t(stride))||!FaceReadable(allocation,(size_t(bits)+31)/32*4)){out+="\"<bad map>\"";return true;}
        out+='[';bool first=true;
        for(int32_t i=0;i<count&&out.size()<kExportObjectBytes;++i){if(!(allocation[i/32]&(1u<<(i%32))))continue;
            if(!first)out+=',';first=false;out+='[';const unsigned char* row=data+size_t(i)*size_t(stride);
            if(!JsonValue(out,key,row,depth+1,refs))out+="null";out+=',';if(!JsonValue(out,value,row+value_offset,depth+1,refs))out+="null";out+=']';}
        out+=']';return true;}
    default:return false;
    }
}
bool EndsWith(const std::string& text,const char* tail) { const size_t n=std::strlen(tail);return text.size()>=n&&!text.compare(text.size()-n,n,tail); }
bool ExportSkipClass(const std::string& name) {
    if(EndsWith(name,"Class")||EndsWith(name,"Function"))return true;
    for(const char* type:{"Package","ScriptStruct","Enum","UserDefinedEnum","UserDefinedStruct"})if(name==type)return true;
    return false;
}
// Of a boss body blueprint only what bullets and weapons attach to: shapes, socket holders, SCS nodes.
bool ExportMonsterPart(Object* object) {
    if(ue::IsDefault(object))return true;
    const std::string cls=Text(ue::ClassOf(object));
    for(const char* part:{"Box","Sphere","Capsule","Holder","Socket","SCS_Node","Collider","Weapon","Attack"})if(cls.find(part)!=std::string::npos)return true;
    return false;
}
// Assets worth following from a reference: data, bullet and config objects, not meshes / animations / effects / sounds.
bool ExportFollowAsset(Object* object) {
    const std::string cls=Text(ue::ClassOf(object));
    for(const char* heavy:{"Niagara","Anim","SkeletalMesh","StaticMesh","Material","Texture","Sound","Ak","Particle","PhysicsAsset","Skeleton",
        "BlendSpace","LevelSequence","World","Level","Font","Widget","Pose","IKRig","Mirror","ControlRig","Chooser"})
        if(cls.rfind(heavy,0)==0)return false;
    return true;
}
bool ExportFollowClass(Object* cls) {
    const std::string path=ObjectPath(cls),name=Text(cls);
    if(path.rfind("/Script/",0)==0||name.size()<3||name.compare(name.size()-2,2,"_C"))return false;
    for(const char* skip:{"ABP_","WBP_","W_","BP_Monster","BP_Hero","BP_Player","SKEL_","BP_Level","BP_Map"})if(name.rfind(skip,0)==0)return false;
    return true;
}
void ExportWrite(const std::string& text) {
    if(export_.file==INVALID_HANDLE_VALUE||text.empty())return;
    DWORD written=0;WriteFile(export_.file,text.data(),static_cast<DWORD>(text.size()),&written,nullptr);export_.bytes+=written;
}
// One pass over the object array: every object at or under a root (a class and its default, an asset, an actor),
// one line each. Returns the classes and assets the written fields name, for the next hop.
void ExportPass(const std::vector<std::pair<Object*,std::string>>& roots,size_t skin,std::string& lines,std::vector<Object*>& refs) {
    std::map<Object*,size_t> index;for(size_t i=0;i<roots.size();++i)if(roots[i].first)index[roots[i].first]=i;
    if(index.empty())return;
    std::vector<std::pair<Object*,size_t>> hits;
    ue::ForEachObject([&](Object* object){
        size_t guard=0;for(Object* o=object;o&&guard<10;o=ue::OuterOf(o),++guard){const auto hit=index.find(o);if(hit!=index.end()){hits.push_back({object,hit->second});return true;}}
        if(ue::IsDefault(object))if(const auto hit=index.find(ue::ClassOf(object));hit!=index.end())hits.push_back({object,hit->second}); // a class's default
        return true;});
    for(const auto& [object,root]:hits){
        const bool monster=roots[root].second=="monster";
        if(!ue::Alive(object)||ExportSkipClass(Text(ue::ClassOf(object)))||(monster&&!ExportMonsterPart(object)))continue;
        const std::string path=ObjectPath(object);if(!export_.written.insert(path).second)continue;
        std::string line="{\"k\":";JsonText(line,roots[root].second);
        char head[48]{};_snprintf_s(head,sizeof(head),_TRUNCATE,",\"skin\":%zu,\"root\":",skin);line+=head;JsonText(line,ObjectPath(roots[root].first));
        line+=",\"path\":";JsonText(line,path);line+=",\"class\":";JsonText(line,ObjectPath(ue::ClassOf(object)));
        if(ue::IsDefault(object)){line+=",\"supers\":[";size_t n=0;
            for(Object* s=ue::At<Object*>(ue::ClassOf(object),ue::layout::kSuper);ue::Alive(s)&&n<16;s=ue::At<Object*>(s,ue::layout::kSuper),++n){if(n)line+=',';JsonText(line,ObjectPath(s));}
            line+=']';}
        std::vector<Object*> monster_refs; // a body blueprint's references (anim, AI, ability sets) are not followed
        line+=",\"f\":";JsonStruct(line,ue::ClassOf(object),object,0,monster?monster_refs:refs);line+="}\n";
        lines+=line;++export_.objects;
    }
}
// A root and up to two more hops of what its fields name (bullet data, bullet / effect / weapon classes).
void ExportTree(std::vector<std::pair<Object*,std::string>> roots,size_t skin,std::string& lines) {
    for(int hop=0;hop<3&&!roots.empty();++hop){
        std::vector<Object*> refs;ExportPass(roots,skin,lines,refs);roots.clear();
        std::set<Object*> seen;
        for(Object* ref:refs){
            if(!ue::Alive(ref)||!seen.insert(ref).second)continue;
            Object* class_type=Class("/Script/CoreUObject.Class");
            if(class_type&&ue::IsA(ref,class_type)){
                if(!ExportFollowClass(ref)||export_.written.count(ObjectPath(ref)))continue;
                export_.written.insert(ObjectPath(ref));++export_.classes;roots.push_back({ref,"class"});continue;}
            Object* outer=ue::OuterOf(ref);if(!outer||ue::OuterOf(outer))continue; // top-level assets only
            if(Text(ue::ClassOf(outer))!="Package"||!ExportFollowAsset(ref)||export_.written.count(ObjectPath(ref)))continue;
            if(ObjectPath(ref).rfind("/Script/",0)==0)continue;
            roots.push_back({ref,"asset"});
        }
    }
}
void ExportClassPackage(const std::string& package,const char* kind,size_t skin,std::string& lines) {
    const std::string key=std::string(kind)+":"+package;if(!export_.written.insert(key).second)return;
    Object* cls=LoadClassPackage(package);
    if(!cls){++export_.missing;std::string line="{\"k\":\"missing\",\"kind\":";JsonText(line,kind);line+=",\"path\":";JsonText(line,package);line+="}\n";lines+=line;return;}
    ++export_.classes;export_.written.insert(ObjectPath(cls));
    ExportTree({{cls,kind}},skin,lines);
}
void ExportFinish(const char* why) {
    if(export_.file!=INVALID_HANDLE_VALUE){std::string line="{\"k\":\"end\",\"why\":";JsonText(line,why);line+="}\n";ExportWrite(line);CloseHandle(export_.file);}
    Logf("NPC_EXPORT done why=%s skins=%zu objects=%zu classes=%zu missing=%zu bytes=%zu ms=%llu",why,export_.skin,export_.objects,export_.classes,export_.missing,
        export_.bytes,GetTickCount64()-export_.t0);
    export_=ExportState{};
}
void ExportBegin(size_t from,size_t end) {
    wchar_t module_path[MAX_PATH]{};HMODULE self=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(&ExportBegin),&self);
    GetModuleFileNameW(self,module_path,MAX_PATH);std::wstring path=module_path;path=path.substr(0,path.find_last_of(L"\\/")+1)+L"PBZAuditExport.jsonl";
    export_=ExportState{};export_.file=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(export_.file==INVALID_HANDLE_VALUE){Logf("NPC_EXPORT cannot create %s (error %lu)",Utf8(path.c_str()).c_str(),GetLastError());return;}
    export_.on=true;export_.skin=from;export_.end=(std::min)(end,std::size(kNpcSkillSets));export_.t0=export_.skin_t0=GetTickCount64();
    std::string lines;char head[96]{};_snprintf_s(head,sizeof(head),_TRUNCATE,"{\"k\":\"begin\",\"from\":%zu,\"end\":%zu,\"skins\":%zu}\n",from,export_.end,std::size(kNpcSkillSets));lines+=head;
    // The hero as it stands: its weapon actors with their components (what the bullets of a cast find on the hero).
    Context context;
    if(ContextNow(context)){
        std::vector<std::pair<Object*,std::string>> roots;for(Object* weapon:HeroWeaponActors(context.pawn))roots.push_back({weapon,"heroWeapon"});
        std::vector<Object*> refs;ExportPass(roots,size_t(-1),lines,refs);
        std::string line="{\"k\":\"hero\",\"pawn\":";JsonText(line,ObjectPath(ue::ClassOf(context.pawn)));line+=",\"f\":";
        std::vector<Object*> ignored;JsonStruct(line,ue::ClassOf(context.pawn),context.pawn,4,ignored);line+="}\n";lines+=line;
    }
    ExportWrite(lines);Logf("NPC_EXPORT begin skins %zu..%zu -> %s",from,export_.end,Utf8(path.c_str()).c_str());
}
void ExportTick() {
    const ULONGLONG start=GetTickCount64();std::string lines;
    while(export_.on&&GetTickCount64()-start<kExportSliceMs){
        if(export_.skin>=export_.end){ExportWrite(lines);lines.clear();ExportFinish("complete");return;}
        const NpcSkillSet& set=kNpcSkillSets[export_.skin];
        if(!export_.skin_done){export_.skin_done=true;
            std::string line;char head[64]{};_snprintf_s(head,sizeof(head),_TRUNCATE,"{\"k\":\"skin\",\"skin\":%zu,\"profile\":",export_.skin);line+=head;
            JsonText(line,set.profile?set.profile:"");line+=",\"monster\":";JsonText(line,set.monster?set.monster:"");line+=",\"skills\":[";
            for(size_t i=0;i<set.skill_count;++i){if(i)line+=',';JsonText(line,set.skills[i]);}
            line+="],\"weapons\":[";for(size_t i=0;i<set.weapon_count;++i){if(i)line+=',';line+="[";JsonText(line,set.weapons[i].actor_class);line+=',';JsonText(line,set.weapons[i].socket);line+="]";}
            line+="],\"colliderTags\":[";const NpcColliderTags& tags=kNpcColliderTagSets[export_.skin];
            for(size_t i=0;i<tags.count;++i){if(i)line+=',';JsonText(line,tags.tags[i]);}line+="]}\n";lines+=line;
            for(size_t i=0;i<set.weapon_count;++i){const std::string path=set.weapons[i].actor_class;const size_t dot=path.find('.');
                if(dot!=std::string::npos)ExportClassPackage(path.substr(0,dot),"weapon",export_.skin,lines);}
            if(set.monster)ExportClassPackage(set.monster,"monster",export_.skin,lines);
            continue;}
        if(export_.skill<set.skill_count){ExportClassPackage(set.skills[export_.skill++],"skill",export_.skin,lines);continue;}
        Logf("NPC_EXPORT skin %zu %s skills=%zu objects=%zu bytes=%zu ms=%llu",export_.skin,set.profile?set.profile:"",set.skill_count,export_.objects,export_.bytes,
            GetTickCount64()-export_.skin_t0);
        ++export_.skin;export_.skill=0;export_.skin_done=false;export_.skin_t0=GetTickCount64();
    }
    ExportWrite(lines);
}
void AuditTick() {
    using P=AuditState::Phase;
    {AcquireSRWLockExclusive(&request_lock);const AuditRequest r=audit_request_;audit_request_={};ReleaseSRWLockExclusive(&request_lock);
        if(r.action==1&&!Auditing()){audit_=AuditState{};audit_.profile=r.from;audit_.first_skill=audit_.skill=r.skill;audit_.only=r.only;
            audit_.end=(std::min)(r.end,std::size(kNpcSkillSets));audit_.phase=P::Begin;}
        else if(r.action==3&&!Auditing()){audit_=AuditState{};audit_.grabs=true;audit_.grab=r.from;audit_.phase=P::Begin;}
        else if(r.action==2&&Auditing())AuditStop("stop-command");
        else if(r.action==2&&export_.on)ExportFinish("stop-command");
        else if(r.action==4&&!Auditing()&&!export_.on)ExportBegin(r.from,r.end);}
    if(export_.on)ExportTick();
    if(audit_.phase==P::Off)return;
    const ULONGLONG now=GetTickCount64();Context context;const bool ok=ContextNow(context);
    if(InterlockedCompareExchange(&disabled_,0,0)&&audit_.phase!=P::Finish)AuditStop("npcretarget-disabled");
    switch(audit_.phase){
    case P::Begin:{
        if(!ok)return;
        {Call where(context.pawn,"K2_GetActorLocation"),turn(context.pawn,"K2_GetActorRotation");if(!where.Invoke()||!turn.Invoke())return;
            audit_.home=where.Get<Vector>("ReturnValue");audit_.home_yaw=turn.Get<AuditRotator>("ReturnValue").yaw;}
        InterlockedExchange(&experimental_,1);EntryProtection::Protect(context.pawn);audit_.begun=now;audit_.deadline=now+30000;audit_.phase=P::Dummy;
        Logf("NPC_AUDIT begin skins=%zu..%zu firstSkill=%zu home=(%.0f,%.0f,%.0f) yaw=%.1f",audit_.profile,audit_.end,audit_.skill+1,audit_.home.x,audit_.home.y,audit_.home.z,audit_.home_yaw);
        return;}
    case P::Dummy:
        if(Same(audit_.dummy)&&!AuditDead(audit_.dummy.object)){audit_.phase=P::Skin;return;}
        if(!ok||skill_.handle>0||pending_.handle>0)return;
        if(now>audit_.deadline){AuditStop("dummy-spawn-failed");return;}
        if(AuditSpawnDummy(context)){audit_.at=now+2000;audit_.phase=P::DummyWait;}
        return;
    case P::DummyWait:
        if(now<audit_.at)return;
        {const bool raised=Same(audit_.dummy)&&GrabExtras::ChangeMaxHealth(audit_.dummy.object,1.0e6f);
            if(ok)AuditPlace(context);
            DamageTarget target;Logf("NPC_AUDIT dummy maxHealthRaised=%d health=%.0f",raised?1:0,ok&&Same(audit_.dummy)&&DamageCandidate(audit_.dummy.object,context.pawn,target)?target.last:-1.0f);}
        Logf("NPC_AUDIT dummy ready alive=%d controller=%s",Same(audit_.dummy)&&!AuditDead(audit_.dummy.object)?1:0,Same(audit_.dummy)?Text(Property<Object*>(audit_.dummy.object,"Controller")).c_str():"-");
        audit_.phase=audit_.grabs?P::GrabNext:P::Skin;return;
    case P::GrabNext:{
        if(active_){if(waiting_action_<0){waiting_action_=static_cast<int32_t>(Action::Restore);waiting_entry_=-1;wait_deadline_=now+60000;}return;} // grabs on the hero's own body
        const MotianGrab::Snapshot s=MotianGrab::GetSnapshot();
        if(audit_.grab_index<0)audit_.grab_options=s.options;
        if(audit_.grab>=s.variants.size()){MotianGrab::RequestOptions(audit_.grab_options);audit_.phase=P::Finish;return;}
        const MotianGrab::Variant& v=s.variants[audit_.grab];
        if(!v.supported){Logf("NPC_AUDIT_GRAB variant=%d id=%s boss=%s result=unsupported why=%s",v.index,Utf8(v.id.c_str()).c_str(),Utf8(v.boss_id.c_str()).c_str(),Utf8(v.reason.c_str()).c_str());
            ++audit_.grab;return;}
        // Long pulls (HookMan / Scavenger P2, ~11 m) only take a target near their own authored distance and range.
        MotianGrab::Options o=s.options;o.enabled=true;o.variant=v.index;o.position_correction=true;o.range=(std::min)((std::max)(o.range,v.recommended_range),v.max_range);
        MotianGrab::RequestOptions(o);audit_.grab_distance=v.authored_distance>300?v.authored_distance:150.0;
        audit_.grab_index=v.index;audit_.grab_retried=false;audit_.at=(std::max)(audit_.at,now);audit_.deadline=audit_.at+30000;audit_.phase=P::GrabPrepare;return;}
    case P::GrabPrepare:{
        const MotianGrab::Snapshot s=MotianGrab::GetSnapshot();
        if(now<audit_.at)return; // the dummy gets up from the last grab
        if(s.options.variant==audit_.grab_index&&s.options.enabled&&s.prepared&&!s.preparing&&!s.active&&!s.cleanup_pending&&ok){
            if(!Same(audit_.dummy)||AuditDead(audit_.dummy.object)){audit_.dummy={};audit_.deadline=now+30000;audit_.phase=P::Dummy;return;}
            AuditPlace(context,audit_.grab_distance,true);audit_.at=now+900;audit_.phase=P::GrabTrigger;return;}
        if(now>audit_.deadline){Logf("NPC_AUDIT_GRAB variant=%d result=not-prepared ms=%llu status=%s",audit_.grab_index,now-audit_.at,Utf8(s.preparation_status.c_str()).c_str());
            ++audit_.grab;audit_.phase=P::GrabNext;}
        return;}
    case P::GrabTrigger:{
        if(now<audit_.at||!ok)return;
        const MotianGrab::Snapshot s=MotianGrab::GetSnapshot();
        audit_.grab_attempts=s.attempts;audit_.grab_activations=s.activations;audit_.grab_hits=s.paired_hits;audit_.grab_active=audit_.grab_paired=false;
        DamageTarget target;audit_.grab_health=DamageCandidate(audit_.dummy.object,context.pawn,target)?target.last:-1;
        Logf("NPC_AUDIT_GRAB_START pos=%zu variant=%d dummyHealth=%.0f",audit_.grab,audit_.grab_index,audit_.grab_health);
        MotianGrab::RequestTrigger(audit_.grab_index);audit_.at=now;audit_.deadline=now+25000;audit_.phase=P::GrabRun;return;}
    case P::GrabRun:{
        const MotianGrab::Snapshot s=MotianGrab::GetSnapshot();
        audit_.grab_active|=s.active;audit_.grab_paired|=s.paired;
        const bool tried=s.attempts>audit_.grab_attempts,timeout=now>audit_.deadline;
        if(!timeout&&(s.active||s.cleanup_pending||(!tried&&now<audit_.at+3000)||now<audit_.at+1500))return;
        if(timeout&&(s.active||s.cleanup_pending))MotianGrab::RequestCancel();
        float health=-1;{DamageTarget target;if(ok&&Same(audit_.dummy)&&DamageCandidate(audit_.dummy.object,context.pawn,target))health=target.last;}
        const MotianGrab::Variant* v=audit_.grab<s.variants.size()?&s.variants[audit_.grab]:nullptr;
        // Refused on its target or footing: once more after the dummy has stood up a while longer.
        if(!timeout&&tried&&!audit_.grab_active&&!audit_.grab_retried){audit_.grab_retried=true;
            Logf("NPC_AUDIT_GRAB_RETRY variant=%d status=%s",audit_.grab_index,Utf8(s.status.c_str()).c_str());
            if(ok)AuditPlace(context,audit_.grab_distance,true);audit_.at=now+4000;audit_.deadline=now+34000;audit_.phase=P::GrabPrepare;return;}
        Logf("NPC_AUDIT_GRAB variant=%d id=%s boss=%s result=%s ms=%llu attempts=+%u activations=+%u pairedHits=+%u dummyDamage=%.0f status=%s alignment=%s",audit_.grab_index,
            v?Utf8(v->id.c_str()).c_str():"-",v?Utf8(v->boss_id.c_str()).c_str():"-",timeout?"timeout":audit_.grab_paired?"paired":audit_.grab_active?"started":tried?"refused":"no-attempt",now-audit_.at,
            s.attempts-audit_.grab_attempts,s.activations-audit_.grab_activations,s.paired_hits-audit_.grab_hits,audit_.grab_health>=0&&health>=0?audit_.grab_health-health:-1.0f,
            Utf8(s.status.c_str()).c_str(),Utf8(s.alignment_status.c_str()).c_str());
        ++audit_.grab;++audit_.grabs_done;audit_.phase=P::GrabNext;
        if(ok)AuditPlace(context,150,true); // live, so it gets up before the next grab (GrabPrepare waits)
        audit_.at=now+4000;return;}
    case P::Skin:{
        if(audit_.profile>=audit_.end){audit_.phase=P::Finish;return;}
        const NpcSkillSet& set=kNpcSkillSets[audit_.profile];
        if(!set.skill_count){Logf("NPC_AUDIT_SKIN profile=%zu id=%s result=no-skills",audit_.profile,set.profile);AuditNextSkin();return;}
        if(active_&&selected_==static_cast<int32_t>(audit_.profile)&&!cleanup_pending_){
            if(audit_.requested)Logf("NPC_AUDIT_SKIN profile=%zu id=%s result=applied ms=%llu skills=%zu weapons=%zu",audit_.profile,set.profile,now-audit_.at,set.skill_count,set.weapon_count);
            audit_.requested=false;audit_.placed=false;++audit_.skins;audit_.phase=P::Settle;AuditInitKinds();
            for(size_t i=0;i<set.weapon_count;++i){const std::string path=set.weapons[i].actor_class;const size_t dot=path.find('.');
                if(dot==std::string::npos)continue;if(!Class(path.c_str()))LoadClassPackage(path.substr(0,dot));
                DumpWeapon("bossWeaponDefault",Class((path.substr(0,dot+1)+"Default__"+path.substr(dot+1)).c_str()));}
            return;}
        if(!audit_.requested){
            if(!ok||skill_.handle>0||pending_.handle>0||damage_.open||waiting_action_>=0)return;
            AuditPlace(context);
            waiting_action_=static_cast<int32_t>(Action::Apply);waiting_entry_=static_cast<int32_t>(audit_.profile);wait_deadline_=now+60000;
            audit_.requested=true;audit_.at=now;audit_.deadline=now+kAuditSkinMs;
            Logf("NPC_AUDIT_SKIN profile=%zu id=%s result=requested",audit_.profile,set.profile);return;}
        if((waiting_action_<0&&now>audit_.at+1500)||now>audit_.deadline){
            if(now>audit_.deadline)waiting_action_=-1;
            Logf("NPC_AUDIT_SKIN profile=%zu id=%s result=failed ms=%llu why=%s",audit_.profile,set.profile,now-audit_.at,failure_text_.empty()?"-":Utf8(failure_text_.c_str()).c_str());
            AuditNextSkin();}
        return;}
    case P::Settle:{
        if(skill_.handle>0||pending_.handle>0||damage_.open)return;
        const NpcSkillSet& set=kNpcSkillSets[audit_.profile];
        if(audit_.skill>=set.skill_count){AuditNextSkin();return;}
        if(!(active_&&selected_==static_cast<int32_t>(audit_.profile))||cleanup_pending_){audit_.requested=false;audit_.phase=P::Skin;return;}
        if(!Same(audit_.dummy)||AuditDead(audit_.dummy.object)){Logf("NPC_AUDIT dummy lost; a new one");audit_.dummy={};audit_.deadline=now+30000;audit_.phase=P::Dummy;return;}
        if(!ok)return;
        if(audit_speed_>1.01f&&audit_.phase==P::Settle&&!audit_.placed)
            SetAuditTimeDilation(context.world,audit_speed_); // every cast and its waits then run at that speed
        if(!audit_.placed){audit_.dummy_cm=audit_.close_retry?kAuditCloseCm:kAuditDummyCm;AuditPlace(context,audit_.dummy_cm);audit_.placed=true;
            audit_.at=now+static_cast<ULONGLONG>(800.0/(std::max)(1.0f,audit_speed_));return;}
        if(now<audit_.at||(!Idle(context)&&now<audit_.at+5000))return;
        AuditResetSkill();AuditScan(true);audit_.placed=false;
        Logf("NPC_AUDIT_SKILL_CAST profile=%zu id=%s skill=%zu/%zu",audit_.profile,set.profile,audit_.skill+1,set.skill_count);
        if(!CastExact(audit_.skill)){audit_.result="grant-failed";AuditReport();AuditAdvance();return;}
        // Scan and settle timers are game-time budgets: at 2x the skill ends in about half the wall time, so the
        // scan interval and the deadline shrink with it and keep the same game-time sampling density.
        const ULONGLONG scale=static_cast<ULONGLONG>((std::max)(1.0f,audit_speed_));
        audit_.deadline=now+kAuditSkillMs/scale;audit_.next_scan=now+100;audit_.phase=P::Run;return;}
    case P::Run:{
        const NpcSkillSet& set=kNpcSkillSets[audit_.profile];
        if(!audit_.started&&skill_.handle>0&&!audit_.hero_weapons_dumped){audit_.hero_weapons_dumped=true;
            for(size_t i=0;i<hidden_weapons_.size()&&i<3;++i)if(Same(hidden_weapons_[i].actor))DumpWeapon("heroWeapon",hidden_weapons_[i].actor.object);}
        if(!audit_.started&&skill_.handle>0){audit_.started=true;audit_.cast_at=skill_.started;
            Logf("NPC_AUDIT_SKILL_START profile=%zu id=%s skill=%zu/%zu ability=%s",audit_.profile,set.profile,audit_.skill+1,set.skill_count,skill_.name.c_str());}
        if(audit_.started&&audit_.montage.empty()&&skill_.handle>0&&now>audit_.cast_at+200&&ok)
            if(Object* anim=AnimInstance(context.mesh)){Call current(anim,"GetCurrentActiveMontage");if(current.Invoke())if(Object* m=current.Get<Object*>("ReturnValue"))audit_.montage=Text(m);}
        if(now>=audit_.next_scan){audit_.next_scan=now+(std::max)(ULONGLONG{60},static_cast<ULONGLONG>(double(kAuditScanMs)/(std::max)(1.0f,audit_speed_)));AuditScan(false);}
        if(skill_.handle>0&&now>audit_.deadline){audit_.result="timeout";ClearSkill("audit-timeout");}
        if(skill_.handle>0||pending_.handle>0||damage_.open)return;
        AuditScan(false);AuditReport();
        // Same skill, dummy closer; not in a sped-up pass, whose flagged skills the runner casts again at 1x.
        if(audit_speed_<=1.01f&&!audit_.close_retry&&audit_.result.empty()&&!audit_.bullets.empty()&&!audit_.dummy_hits){audit_.close_retry=true;audit_.phase=P::Settle;return;}
        AuditAdvance();audit_.phase=P::Settle;return;}
    case P::Finish:{
        ClearSkill("audit-end");
        RestoreAuditTimeDilation();audit_speed_=1.0f; // before anything else: the rest of the game runs at normal speed again
        if(Same(audit_.dummy)){Object* controller=Property<Object*>(audit_.dummy.object,"Controller");
            Call(audit_.dummy.object,"K2_DestroyActor").Invoke();if(ue::Alive(controller))Call(controller,"K2_DestroyActor").Invoke();}
        if(active_){waiting_action_=static_cast<int32_t>(Action::Restore);waiting_entry_=-1;wait_deadline_=now+60000;}
        if(ok)AuditTeleport(context.pawn,audit_.home,audit_.home_yaw);
        EntryProtection::ReleaseProtection();
        if(audit_.grabs&&audit_.grab_index>=0)MotianGrab::RequestOptions(audit_.grab_options);
        Logf("NPC_AUDIT done reason=%s skins=%zu skills=%zu nextSkin=%zu grabs=%zu nextGrab=%zu minutes=%.1f",audit_.stop.empty()?"complete":audit_.stop.c_str(),audit_.skins,audit_.skills,audit_.profile,
            audit_.grabs_done,audit_.grab,double(now-audit_.begun)/60000.0);
        audit_.phase=P::Off;audit_.dummy={};return;}
    default:return;
    }
}
void Tick() {
    if (!initialized_) return;
    if (active_ && !cleanup_pending_ && !RefreshFaceVisibility()) {
        cleanup_pending_=true;
        Status(L"NPC 独立面部节点 / 原生评估状态变化，保留清理记录并于空闲时恢复原模型");
        Logf("NPC_FACE_NEUTRAL node identity/evaluation failed; restore requested, shared CDO untouched");
    }
    if (active_ && !cleanup_pending_ && source_driver_mode_) EnforceHiddenFlags();
    if (active_) ProbeCameraLift();
    if (InterlockedExchange(&skill_request_, 0) && active_) CastSkill();
    if (const LONG pick = InterlockedExchange(&cast_request_, 0); pick > 0 && active_ && !CastExact(static_cast<size_t>(pick - 1))) Logf("NPC_SKILL cast %ld refused: no skin / busy / no such skill", pick);
    if (active_) { RetryPendingSkill(); HoldSkillLock(); HoldForeignMontage(); DriveMount(); } // every frame
    ScaleSkillDamage(); DriveBossCalls(); // every frame; both close what they hold once the skin is off
    AuditTick();
    int32_t action = -1, entry = -1;
    AcquireSRWLockExclusive(&request_lock); action = pending_action_; entry = pending_entry_; pending_action_ = -1; ReleaseSRWLockExclusive(&request_lock);
    if (InterlockedExchange(&forget_resume_, 0)) ForgetResume(action == static_cast<int32_t>(Action::Restore) ? "restore requested" : "another request");
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
        KeepForNextScene("player-or-map-changed");
        const bool restored = Restore();
        Status(restored ? L"玩家或地图已变化，NPC 模型将在新场景就绪后重新套用" : L"玩家或地图已变化；NPC 清理尚未完整，保留可重试状态并记录日志");
    } else if (active_ && (SkelMesh(primary_.object) != (source_driver_mode_?source_asset_.object:target_mesh_.object)|| (source_driver_mode_&&SkelMesh(display_.object)!=target_mesh_.object))) {
        const bool restored = Restore();
        Status(restored ? L"游戏更换了主网格，已停止 NPC 覆盖并清理临时设置" : L"游戏更换了主网格；已保留外部改动，部分共享设置需查看日志");
    }
    if (active_ && context_ok && !cleanup_pending_) {
        if(source_driver_mode_){RefreshForearmBindings();RefreshWeaponRoutes();QuietAimNodes(display_.object);}
        WatchSkill();HoldBodyCollision();HoldCameraLift();
        if(skill_.handle>0&&!hidden_weapons_.empty())HideHeroWeapons(); // a weapon the skill attached since
        const char* why=nullptr;
        if(!SourceLeaseValid(why)){}
        else if(source_driver_mode_&&!RepairSourceSettings())why="source-flag-repair";
        else if(!RepairOriginalShadows())why="original-part-shadow-repair";
        else if(!RefreshOwnedLighting())why="lighting-channels";
        else if(pose_session_&&InterlockedCompareExchange(&pose_session_->failed,0,0))why=pose_session_->why;
        else if(!grip_hook_)why="weapon-adapter-missing";
        else if(ue::At<uintptr_t*>(primary_.object,0)!=grip_hook_->table)why="weapon-adapter-vtable-replaced";
        else if(InterlockedCompareExchange(&grip_hook_->failures,0,0))why="weapon-adapter-query-failed";
        if (why) {
            cleanup_pending_=true;
            Logf("NPC_RETARGET retiring at next idle reason=%s",why);
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
    if (waiting_action_ < 0 && resume_entry_ >= 0) ResumeAfterScene(context_ok, disabled);
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
    const float damage = SettingsStore::GetFloat(kSkillSection, L"DamageMultiplier", kSkillDamageDefault);
    skill_damage_.store(std::isfinite(damage) ? std::clamp(damage, kSkillDamageMin, kSkillDamageMax) : kSkillDamageDefault, std::memory_order_relaxed);
    initialized_ = true; Logf("NPC_RETARGET initialized profiles=%zu native AnimationRelative experiment skillDamage=x%.2f", std::size(kProfiles), skill_damage_.load(std::memory_order_relaxed));
}
bool Request(Action action, int32_t entry) {
    if (action == Action::Apply && !InterlockedCompareExchange(&experimental_, 0, 0)) return false;
    if ((InterlockedCompareExchange(&disabled_, 0, 0) && action != Action::Restore) || ((action == Action::Apply || action == Action::Inspect) && (entry < 0 || static_cast<size_t>(entry) >= std::size(kProfiles)))) return false;
    AcquireSRWLockExclusive(&request_lock); pending_action_ = static_cast<int32_t>(action); pending_entry_ = entry; ReleaseSRWLockExclusive(&request_lock);
    if (action != Action::Inspect) InterlockedExchange(&forget_resume_, 1); // the game thread drops the remembered skin
    Status(L"NPC 重定向请求已排队；更换前会检查空闲状态、部件与武器挂点"); return true;
}
bool SetExperimental(bool enabled) {
    if (enabled && InterlockedCompareExchange(&disabled_, 0, 0)) return false;
    InterlockedExchange(&experimental_, enabled ? 1 : 0);
    AcquireSRWLockExclusive(&snapshot_lock); snapshot_.experimental = enabled; ReleaseSRWLockExclusive(&snapshot_lock);
    if (!enabled) return Request(Action::Restore);
    Status(L"NPC 实验开关已打开；仍需通过场景、外观互斥、骨架与挂点校验"); return true;
}
float SkillDamageMultiplier() { return skill_damage_.load(std::memory_order_relaxed); }
bool SetSkillDamageMultiplier(float multiplier) {
    if (!std::isfinite(multiplier)) return false;
    const float value = std::clamp(multiplier, kSkillDamageMin, kSkillDamageMax);
    if (value == skill_damage_.exchange(value, std::memory_order_relaxed)) return true;
    SettingsStore::PutFloat(kSkillSection, L"DamageMultiplier", value); Logf("NPC_SKILL damage multiplier x%.2f", value); return true;
}
bool RequestSkill() {
    AcquireSRWLockShared(&snapshot_lock); const bool active = snapshot_.active; ReleaseSRWLockShared(&snapshot_lock);
    if (!active || InterlockedCompareExchange(&disabled_, 0, 0)) return false;
    InterlockedExchange(&skill_request_, 1); return true;
}
unsigned char* GrabDisplay(double& size) {
    const bool on = active_ && source_driver_mode_ && Same(display_);
    size = on ? skin_size_ : 1.0; return on ? display_.object : nullptr;
}
Snapshot GetSnapshot() { AcquireSRWLockShared(&snapshot_lock); Snapshot copy = snapshot_; ReleaseSRWLockShared(&snapshot_lock); return copy; }
void OnGameThreadGuarded() { GuardedTick(); }
bool BeforeTravel() {
    AcquireSRWLockExclusive(&request_lock); pending_action_ = -1; ReleaseSRWLockExclusive(&request_lock);
    __try { KeepForNextScene("travel"); waiting_action_ = -1; return Restore(); }
    __except (FaultFilter(GetExceptionInformation())) { Logf("NPC_RETARGET pre-travel cleanup fault 0x%08lx", fault_code_); return false; }
}
bool ReleaseForExternal() {
    AcquireSRWLockExclusive(&request_lock); pending_action_ = -1; ReleaseSRWLockExclusive(&request_lock);
    __try {
        waiting_action_ = -1;
        if (!HasResources()) { Publish(); return true; }
        ForgetResume("hero outfit applied");
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
    if (action == "skill") return RequestSkill();
    if (action == "cast") { char* end = nullptr; const long pick = std::strtol(id.c_str(), &end, 10); if (id.empty() || !end || *end || pick < 1 || pick > 256) return false; InterlockedExchange(&cast_request_, pick); return true; }
    if (action == "audit") {
        // npcretarget audit start [first skin index] [first skill, 1-based] [end skin index, exclusive] | list | export | stop
        AuditRequest request; size_t from = 0, skill = 1, end = std::size(kNpcSkillSets);
        if (id == "start") { input >> from; if (input.fail()) { input.clear(); from = 0; } else { input >> skill; if (input.fail()) { input.clear(); skill = 1; } else { input >> end; if (input.fail()) end = std::size(kNpcSkillSets); } }
            if (from >= std::size(kNpcSkillSets) || !skill) return false; request = {1, from, skill - 1, end}; }
        else if (id == "list") {
            // npcretarget audit list <skin index> <skill,skill,...> (1-based): only those skills of that skin.
            std::string skills; input >> from >> skills;
            if (input.fail() || from >= std::size(kNpcSkillSets)) return false;
            std::vector<size_t> only; std::stringstream parts(skills);
            for (std::string part; std::getline(parts, part, ',');) { const long n = std::strtol(part.c_str(), nullptr, 10); if (n >= 1 && n <= 512) only.push_back(static_cast<size_t>(n - 1)); }
            if (only.empty()) return false;
            std::sort(only.begin(), only.end()); only.erase(std::unique(only.begin(), only.end()), only.end());
            request = {1, from, only.front(), from + 1, only}; }
        else if (id == "export") {
            // npcretarget audit export [first skin index] [end skin index, exclusive]: no casts, data only (PBZAuditExport.jsonl).
            input >> from; if (input.fail()) { input.clear(); from = 0; } else { input >> end; if (input.fail()) end = std::size(kNpcSkillSets); }
            if (from >= std::size(kNpcSkillSets)) return false; request = {4, from, 0, end}; }
        else if (id == "stop") request.action = 2;
        else if (id == "grabs") { size_t first = 0; input >> first; if (input.fail()) first = 0; request = {3, first, 0, 0}; }
        else if (id == "speed") {
            // npcretarget audit speed <x>: 1 = normal, up to 10x. Lives until the audit ends (the original
            // world time dilation is restored there), and applies from the next skill on.
            float speed = 0.0f; input >> speed;
            if (input.fail() || !(speed >= 1.0f) || speed > 10.0f) { Logf("NPC_AUDIT speed needs 1..10"); return true; }
            audit_speed_ = speed;
            Logf("NPC_AUDIT speed %.2fx (from the next skill; restored when the audit ends)", speed);
            return true; }
        else return false;
        AcquireSRWLockExclusive(&request_lock); audit_request_ = request; ReleaseSRWLockExclusive(&request_lock); return true;
    }
    if (action == "damage") { char* end = nullptr; const float value = std::strtof(id.c_str(), &end); return !id.empty() && end && !*end && SetSkillDamageMultiplier(value); }
    if (action == "status") { const Snapshot state = GetSnapshot(); Logf("NPC_RETARGET_STATUS %s", Utf8(state.status.c_str()).c_str()); return true; }
    if (action == "list") { for (size_t i = 0; i < std::size(kProfiles); ++i) Logf("NPC_RETARGET_PROFILE %zu id=%s name=%s", i, kProfiles[i].id, Utf8(kProfiles[i].name).c_str()); return true; }
    if (action == "apply" || action == "inspect") for (size_t i = 0; i < std::size(kProfiles); ++i)
        if (Lower(id) == Lower(kProfiles[i].id) || id == std::to_string(i)) return Request(action == "apply" ? Action::Apply : Action::Inspect, static_cast<int32_t>(i));
    return false;
}
}
