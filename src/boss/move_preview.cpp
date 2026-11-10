#include "move_preview.h"
#include "bone_motion.h"
#include "boss_assist.h"
#include "hub_pause.h"
#include "ue_runtime.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace MovePreview {
namespace {
using ue::Object; using ue::Call; using ue::Name; using ue::Array;
struct Vector { double x, y, z; };
struct Rotator { double pitch, yaw, roll; };
struct LinearColor { float r, g, b, a; };
struct alignas(16) Transform { double rotation[4]{0,0,0,1}, translation[4]{}, scale[4]{1,1,1,0}; };
static_assert(sizeof(Transform) == 96, "PBZ native FTransform layout");
struct Bounds { Vector origin, extent; double radius; };
static_assert(sizeof(Bounds) == 56, "FBoxSphereBounds layout");
struct SoftObject { int32_t index, serial; Name package, asset; ue::String subpath; };
static_assert(sizeof(SoftObject) == 40, "TSoftObjectPtr layout");

constexpr double kStageLiftCm = 80000;   // above the hero: out of the level's way, below its cloud layer
constexpr double kSkyRadiusCm = 4000, kFloorCm = 6000, kBossX = -250, kDummyX = 250;
constexpr ULONGLONG kStageKeepMs = 20000, kReadMs = 120, kRetryMs = 10000;
constexpr DWORD kMaxFaults = 3;
constexpr char kFloorMesh[] = "/Engine/BasicShapes/Cube.Cube";
constexpr char kDummyMesh[] = "/Engine/BasicShapes/Cylinder.Cylinder";
constexpr char kSkyMesh[] = "/Engine/MapTemplates/Sky/SM_SkySphere.SM_SkySphere";
constexpr char kGridMaterial[] = "/Engine/EngineMaterials/WorldGridMaterial.WorldGridMaterial";
constexpr char kShapeMaterial[] = "/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial";
constexpr char kSkyMaterial[] = "/Engine/MapTemplates/Sky/M_Procedural_Sky_MASTER02.M_Procedural_Sky_MASTER02";
constexpr char kFlatMaterial[] = "/Engine/EngineMaterials/EmissiveMeshMaterial.EmissiveMeshMaterial";

Logger logger_ = nullptr;
SRWLOCK lock_ = SRWLOCK_INIT;
std::string want_monster_, want_ability_; bool want_shown_ = false; ULONGLONG hidden_at_ = 0;
int want_speed_ = 100; std::vector<NpcCombo::CurvePoint> want_curve_;  // under lock_: the move's 加速
std::vector<int> want_trim_holds_;
int want_trim_from_ = 0, want_trim_to_ = 0;  // under lock_: its 裁剪 (ms)
Frame frame_;  // under lock_
DWORD faults_ = 0; bool disabled_ = false;

struct Stage {
    Object* world = nullptr; Vector origin{};
    std::vector<Object*> actors; std::vector<Name> names;  // what Teardown removes
    Object* dummy = nullptr; Object* capture = nullptr; Object* component = nullptr; Object* target = nullptr; Object* key = nullptr;
    Vector camera{}; bool built = false;
};
Stage stage_;
std::string monster_, ability_;  // what the preview boss plays now
bool framed_ = false; ULONGLONG next_read_ = 0, retry_at_ = 0; double exposure_ = 1; uint32_t reads_ = 0, read_fails_ = 0;
Object* engine_ = nullptr;
// 实时预览: the game draws the stage itself, every frame, where the page's card shows it (Place): a UMG image (a
// CommonUserWidget whose root is a UImage with the capture's render target as its brush) in the game's viewport, under a
// hole the page cuts in the hub there. The capture is then the engine's final colour (SCS_FinalColorLDR, 2: tone mapped,
// exposed by its own eye adaptation) at the picture's own size; nothing is read back. Only when that widget cannot be
// made are frames read back a few times a second (SCS_SceneColorHDR, 0, exposed and tone mapped here) for the page.
constexpr uint8_t kLiveSource = 2, kReadSource = 0;
int source_override_ = -1;           // `npcretarget preview source <n>` (-1: by the mode)
RECT view_{};                        // under lock_: the card's picture in the game window's client pixels (empty: none)
Object* widget_ = nullptr; Name widget_name_{}; Object* image_ = nullptr;
bool live_ = false, live_failed_ = false; RECT placed_{}; int target_w_ = kWidth, target_h_ = kHeight;
// The hub pauses the game while it is open (HubPause). A preview needs the world running, so HubPause lets it run
// (NeedsWorld) and every pawn but the stage's (the hero, the enemies) and its controller stands still instead:
// CustomTimeDilation 0, set again when the game's own slow motion changes it, given back when the preview goes.
struct Frozen { Object* actor; Name name; float before; };
std::vector<Frozen> frozen_; ULONGLONG freeze_scan_at_ = 0;
constexpr double kStageKeepClearCm = 15000;  // pawns this close to the stage are its own (the boss, its summons)
// Niagara culls effects by distance (and by not being seen) from the player's camera, 800 m below the stage: all of the
// preview boss's effects. Paused while the boss plays (two cvars, back to 1 when it is gone); `preview cull 1` keeps them.
bool keep_culling_ = false, culling_paused_ = false;
bool fx_scan_ = false;            // `preview fx`: one scan of the effect components near the stage, logged
std::vector<Object*> fx_lod_;     // the boss's effect components given the camera's LOD distance
ULONGLONG fx_log_at_ = 0;

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[700]{}; va_list arguments; va_start(arguments, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments); va_end(arguments);
    logger_(text);
}
void SetStatus(const wchar_t* status) {
    AcquireSRWLockExclusive(&lock_);
    if (frame_.status != status) { frame_.status = status; ++frame_.serial; }
    ReleaseSRWLockExclusive(&lock_);
}
// frame_.live: the game shows the stage at the placed rect itself (the page leaves a hole there); no pixels come.
void SetLive(bool live) {
    AcquireSRWLockExclusive(&lock_);
    if (frame_.live != live) { frame_.live = live; if (live) { frame_.pixels.clear(); frame_.status.clear(); } ++frame_.serial; }
    ReleaseSRWLockExclusive(&lock_);
}
std::string Text(Object* object) { char text[200]{}; if (object && ue::Alive(object)) ue::NameText(ue::NameOf(object), text, sizeof(text)); return text[0] ? text : "-"; }
int32_t Offset(Object* object, const char* property) { return object ? ue::Offset(ue::ClassOf(object), property) : -1; }
Object* Property(Object* object, const char* property) {
    const int32_t offset = Offset(object, property);
    Object* value = offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
    return value && ue::Alive(value) ? value : nullptr;
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
template<class T> bool Put(Object* object, const char* property, const T& value) {
    const uintptr_t field = object ? Field(ue::ClassOf(object), property) : 0;
    if (!field || ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropElementSize) != static_cast<int32_t>(sizeof(T))) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    std::memcpy(reinterpret_cast<unsigned char*>(object) + offset, &value, sizeof(value)); return true;
}
// A bool property at `base` (a bitfield one too: FBoolProperty's FieldSize, ByteOffset, ByteMask, FieldMask at 0x70..0x73).
bool PutBoolAt(unsigned char* base, uintptr_t field, bool value) {
    if (!base || !field || ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x70) != 1) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    const uint8_t byte_offset = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x71), byte_mask = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x72),
                  mask = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x73);
    if (byte_offset > 7 || !mask || !byte_mask) return false;
    uint8_t& byte = base[offset + byte_offset];
    byte = static_cast<uint8_t>((byte & ~mask) | (value ? byte_mask : 0)); return true;
}
bool PutBool(Object* object, const char* property, bool value) {
    return object && PutBoolAt(reinterpret_cast<unsigned char*>(object), Field(ue::ClassOf(object), property), value);
}
// A field of the struct property `outer` of `object` (FStructProperty::Struct at 0x70): the struct's address and the field.
unsigned char* InStruct(Object* object, const char* outer, const char* name, uintptr_t& field) {
    field = 0;
    const uintptr_t holder = object ? Field(ue::ClassOf(object), outer) : 0;
    Object* strukt = holder ? ue::At<Object*>(reinterpret_cast<void*>(holder), 0x70) : nullptr;
    field = strukt ? Field(strukt, name) : 0;
    return field ? reinterpret_cast<unsigned char*>(object) + ue::At<int32_t>(reinterpret_cast<void*>(holder), ue::layout::kPropOffset) : nullptr;
}
// One post process override of the capture: bOverride_<name> on, <name> = value (1 or 4 bytes).
template<class T> bool Override(Object* component, const char* name, T value) {
    uintptr_t flag = 0, field = 0;
    const std::string switch_name = std::string("bOverride_") + name;
    unsigned char* base = InStruct(component, "PostProcessSettings", switch_name.c_str(), flag);
    InStruct(component, "PostProcessSettings", name, field);
    if (!base || !field || ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropElementSize) != static_cast<int32_t>(sizeof(T))) return false;
    std::memcpy(base + ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset), &value, sizeof(T));
    return PutBoolAt(base, flag, true);
}
void Console(Object* world, const wchar_t* command) {
    const int32_t n = static_cast<int32_t>(std::wcslen(command)) + 1;
    Call(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "ExecuteConsoleCommand").Set("WorldContextObject", world)
        .Set("Command", ue::String{command, n, n}).Invoke();
}
void PauseCulling(Object* world, bool pause) {
    if (pause == culling_paused_ || !world) return;
    Console(world, pause ? L"fx.Niagara.Scalability.DistanceCulling 0" : L"fx.Niagara.Scalability.DistanceCulling 1");
    Console(world, pause ? L"fx.Niagara.Scalability.VisibilityCulling 0" : L"fx.Niagara.Scalability.VisibilityCulling 1");
    culling_paused_ = pause;
    Logf("MOVE_PREVIEW niagara culling %s", pause ? "paused" : "back on");
}
struct Context { Object* world = nullptr; Object* instance = nullptr; Object* pawn = nullptr; Object* controller = nullptr; };
bool ContextNow(Context& context) {
    if (!engine_ || !ue::Alive(engine_)) engine_ = ue::FindEngine();
    context.world = Property(Property(engine_, "GameViewport"), "World");
    if (!context.world) return false;
    context.instance = Property(context.world, "OwningGameInstance");
    if (!context.instance) return false;
    const int32_t players = Offset(context.instance, "LocalPlayers");
    if (players < 0) return false;
    const Array list = ue::At<Array>(context.instance, players);
    if (!list.data || list.num <= 0 || list.num > 8) return false;
    context.controller = Property(static_cast<Object**>(list.data)[0], "PlayerController");
    context.pawn = Property(context.controller, "Pawn");
    return context.pawn != nullptr;
}
std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }
Object* LoadAsset(const char* full) {
    if (Object* asset = ue::FindObject(full)) return asset;
    const std::string text(full); const size_t dot = text.find('.');
    SoftObject soft{};
    const std::wstring package = Wide(text.substr(0, dot)), asset = Wide(text.substr(dot + 1));
    soft.package = ue::MakeName(package.c_str()); soft.asset = ue::MakeName(asset.c_str());
    Call load(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LoadAsset_Blocking");
    Object* object = load.Set("Asset", soft).Invoke() ? load.Get<Object*>("ReturnValue") : nullptr;
    return object && ue::Alive(object) ? object : nullptr;
}
Object* Class(const char* path) { return ue::FindObject(path); }
Transform At(const Vector& where, double yaw_degrees = 0, const Vector& scale = {1, 1, 1}, double pitch_degrees = 0) {
    Transform t; t.translation[0] = where.x; t.translation[1] = where.y; t.translation[2] = where.z;
    const double cy = std::cos(yaw_degrees * 3.14159265358979 / 360.0), sy = std::sin(yaw_degrees * 3.14159265358979 / 360.0);
    const double cp = std::cos(pitch_degrees * 3.14159265358979 / 360.0), sp = std::sin(pitch_degrees * 3.14159265358979 / 360.0);
    // FQuat from (pitch, yaw, 0): x = -sp*sy... (roll 0)
    t.rotation[0] = sp * sy; t.rotation[1] = -sp * cy; t.rotation[2] = cp * sy; t.rotation[3] = cp * cy;
    t.scale[0] = scale.x; t.scale[1] = scale.y; t.scale[2] = scale.z;
    return t;
}
Object* SpawnActor(Object* context_object, const char* class_path, const Transform& where) {
    Object* statics = ue::Default("/Script/Engine.Default__GameplayStatics"), *cls = Class(class_path);
    if (!statics || !cls) { Logf("MOVE_PREVIEW no class %s", class_path); return nullptr; }
    Call begin(statics, "BeginDeferredActorSpawnFromClass");
    if (!begin.Set("WorldContextObject", context_object).Set("ActorClass", cls).Set("SpawnTransform", where).Set("CollisionHandlingOverride", uint8_t{1}).Invoke()) return nullptr;
    Object* actor = begin.Get<Object*>("ReturnValue");
    if (!actor || !ue::Alive(actor)) return nullptr;
    if (!Call(statics, "FinishSpawningActor").Set("Actor", actor).Set("SpawnTransform", where).Invoke()) { Call(actor, "K2_DestroyActor").Invoke(); return nullptr; }
    stage_.actors.push_back(actor); stage_.names.push_back(ue::NameOf(actor));
    return actor;
}
Object* Material(Object* context_object, const char* path, const char* fallback = nullptr) {
    Object* parent = LoadAsset(path);
    if (!parent && fallback) parent = LoadAsset(fallback);
    if (!parent) { Logf("MOVE_PREVIEW no material %s", path); return nullptr; }
    Call make(ue::Default("/Script/Engine.Default__KismetMaterialLibrary"), "CreateDynamicMaterialInstance");
    Object* mid = make.Set("WorldContextObject", context_object).Set("Parent", parent).Invoke() ? make.Get<Object*>("ReturnValue") : nullptr;
    return mid && ue::Alive(mid) ? mid : parent;
}
void Colour(Object* mid, const wchar_t* parameter, LinearColor value) {
    Call(mid, "SetVectorParameterValue").Set("ParameterName", ue::MakeName(parameter)).Set("Value", value).Invoke();
}
void Scalar(Object* mid, const wchar_t* parameter, float value) {
    Call(mid, "SetScalarParameterValue").Set("ParameterName", ue::MakeName(parameter)).Set("Value", value).Invoke();
}
// A plain actor with one static mesh (seen by scene captures only), placed with `where` (its scale too).
Object* SpawnMesh(Object* context_object, const char* mesh_path, Object* material, const Transform& where, bool collide, bool shadow) {
    Object* mesh = LoadAsset(mesh_path), *component_class = Class("/Script/Engine.StaticMeshComponent");
    if (!mesh || !component_class) { Logf("MOVE_PREVIEW no mesh %s", mesh_path); return nullptr; }
    Object* actor = SpawnActor(context_object, "/Script/Engine.Actor", where);
    if (!actor) return nullptr;
    Call add(actor, "AddComponentByClass");
    if (!add.Set("Class", component_class).Set("bManualAttachment", false).Set("RelativeTransform", Transform{}).Set("bDeferredFinish", true).Invoke()) return nullptr;
    Object* component = add.Get<Object*>("ReturnValue");
    if (!component || !ue::Alive(component)) return nullptr;
    Call(component, "SetMobility").Set("NewMobility", uint8_t{2}).Invoke();
    Call(component, "SetStaticMesh").Set("NewMesh", mesh).Invoke();
    if (material) Call(component, "SetMaterial").Set("ElementIndex", int32_t{0}).Set("Material", material).Invoke();
    PutBool(component, "bVisibleInSceneCaptureOnly", true);
    PutBool(component, "CastShadow", shadow);
    Call(actor, "FinishAddComponent").Set("Component", component).Set("bManualAttachment", false).Set("RelativeTransform", Transform{}).Invoke();
    Call(component, "SetCollisionProfileName").Set("InCollisionProfileName", ue::MakeName(collide ? L"BlockAll" : L"NoCollision")).Set("bUpdateOverlaps", false).Invoke();
    Call(actor, "K2_SetActorTransform").Set("NewTransform", where).Set("bSweep", false).Set("bTeleport", true).Invoke();
    return actor;
}
double MeshRadius(const char* mesh_path) {
    Object* mesh = LoadAsset(mesh_path);
    Call bounds(mesh, "GetBounds");
    if (!mesh || !bounds.Invoke()) return 0;
    const Bounds b = bounds.Get<Bounds>("ReturnValue");
    return std::isfinite(b.radius) && b.radius > 1 ? b.radius : 0;
}
Object* SpawnLight(Object* context_object, const Vector& where, float intensity, LinearColor colour) {
    Object* actor = SpawnActor(context_object, "/Script/Engine.PointLight", At(where));
    Object* light = Property(actor, "PointLightComponent");
    if (!light) light = Property(actor, "LightComponent");
    if (!light) return actor;
    Call(light, "SetMobility").Set("NewMobility", uint8_t{2}).Invoke();
    Call(light, "SetIntensity").Set("NewIntensity", intensity).Invoke();
    Call(light, "SetAttenuationRadius").Set("NewRadius", float(kSkyRadiusCm)).Invoke();
    Call(light, "SetLightColor").Set("NewLightColor", colour).Set("bSRGB", true).Invoke();
    Call(light, "SetCastShadows").Set("bNewValue", false).Invoke();
    // Characters may be on lighting channels of their own: every channel.
    Call(light, "SetLightingChannels").Set("bChannel0", true).Set("bChannel1", true).Set("bChannel2", true).Invoke();
    return actor;
}
void DropWidget(const char* why);
void Teardown(const char* why) {
    BossAssist::StopPreview();
    DropWidget(why);
    size_t removed = 0;
    for (size_t i = 0; i < stage_.actors.size(); ++i)
        if (ue::Alive(stage_.actors[i]) && ue::NameOf(stage_.actors[i]) == stage_.names[i] && Call(stage_.actors[i], "K2_DestroyActor").Invoke()) ++removed;
    if (stage_.built || removed) Logf("MOVE_PREVIEW stage removed (%s) actors=%zu/%zu reads=%u readFails=%u", why, removed, stage_.actors.size(), reads_, read_fails_);
    if (culling_paused_) { Context context; if (ContextNow(context)) PauseCulling(context.world, false); }
    stage_ = Stage{}; monster_.clear(); ability_.clear(); framed_ = false; fx_lod_.clear();
}
bool Build(const Context& context) {
    Teardown("rebuild");
    Vector hero{};
    { Call where(context.pawn, "K2_GetActorLocation"); if (!where.Invoke()) return false; hero = where.Get<Vector>("ReturnValue"); }
    const Vector o{hero.x, hero.y, hero.z + kStageLiftCm};
    stage_.world = context.world; stage_.origin = o;
    Object* ctx = context.world;  // the world owns what is made here (actors, materials, the render target)
    // Floor: a WorldGrid slab whose top is the stage's origin.
    Object* grid = LoadAsset(kGridMaterial);
    Object* floor = SpawnMesh(ctx, kFloorMesh, grid, At({o.x, o.y, o.z - 10}, 0, {kFloorCm / 100, kFloorCm / 100, 0.2}), true, false);
    // Sky: the template sky sphere, grey, around everything (it hides the level from the capture).
    Object* sky_material = Material(ctx, kSkyMaterial, kFlatMaterial);
    if (sky_material) {
        // Grey and dim beside the lit boss and post (the engine's exposure follows the brightest part of the picture).
        Colour(sky_material, L"ZenithColor", {0.07f, 0.075f, 0.08f, 1}); Colour(sky_material, L"HorizonColor", {0.18f, 0.185f, 0.19f, 1});
        Colour(sky_material, L"Color", {0.14f, 0.145f, 0.15f, 1});
        Scalar(sky_material, L"CloudOpacity", 0); Scalar(sky_material, L"Stars", 0); Scalar(sky_material, L"Desaturation", 1);
    }
    const double radius = MeshRadius(kSkyMesh), sky_scale = radius > 0 ? kSkyRadiusCm / radius : 1;
    Object* sky = SpawnMesh(ctx, kSkyMesh, sky_material, At(o, 0, {sky_scale, sky_scale, sky_scale}), false, false);
    // 木桩: a wooden post the boss aims at.
    Object* wood = Material(ctx, kShapeMaterial);
    if (wood) Colour(wood, L"Color", {0.42f, 0.30f, 0.19f, 1});
    stage_.dummy = SpawnMesh(ctx, kDummyMesh, wood, At({o.x + kDummyX, o.y, o.z + 90}, 0, {0.5, 0.5, 1.8}), true, true);
    SpawnLight(ctx, {o.x - 500, o.y - 700, o.z + 700}, 60000, {1.0f, 0.97f, 0.92f, 1});
    SpawnLight(ctx, {o.x + 600, o.y + 500, o.z + 450}, 25000, {0.85f, 0.9f, 1.0f, 1});
    stage_.key = SpawnLight(ctx, {o.x, o.y - 600, o.z + 350}, 30000, {1.0f, 0.98f, 0.95f, 1});  // by the camera (FrameCamera moves it)
    // The capture: into a small float target, every frame while the stage is up (kLiveSource / kReadSource).
    stage_.capture = SpawnActor(ctx, "/Script/Engine.SceneCapture2D", At({o.x, o.y - 900, o.z + 300}, 90, {1, 1, 1}, -10));
    stage_.component = Property(stage_.capture, "CaptureComponent2D");
    Object* rendering = ue::Default("/Script/Engine.Default__KismetRenderingLibrary");
    Call make(rendering, "CreateRenderTarget2D");
    if (make.Set("WorldContextObject", ctx).Set("Width", int32_t{kWidth}).Set("Height", int32_t{kHeight}).Set("Format", uint8_t{6}).Set("ClearColor", LinearColor{0, 0, 0, 1}).Invoke())
        stage_.target = make.Get<Object*>("ReturnValue");
    if (stage_.target && !ue::Alive(stage_.target)) stage_.target = nullptr;
    bool wired = false;
    if (stage_.component && stage_.target) {
        wired = Put(stage_.component, "TextureTarget", stage_.target);
        Put(stage_.component, "CaptureSource", kReadSource);
        Put(stage_.component, "FOVAngle", 50.0f);
        PutBool(stage_.component, "bCaptureEveryFrame", true);
        PutBool(stage_.component, "bCaptureOnMovement", true);
        // Its own view state: eye adaptation and temporal anti-aliasing carry over between frames, as in the game's view.
        PutBool(stage_.component, "bAlwaysPersistRenderingState", true);
        // The level's post process volumes apply to it too: no depth of field (it blurred the whole stage), no motion
        // blur, no vignette.
        const bool sharp = Override(stage_.component, "DepthOfFieldFocalDistance", 0.0f);
        Override(stage_.component, "DepthOfFieldDepthBlurRadius", 0.0f);
        Override(stage_.component, "MotionBlurAmount", 0.0f);
        Override(stage_.component, "VignetteIntensity", 0.0f);
        // Its own exposure: the level's range (a dim night map) burnt the lit stage white. Histogram eye adaptation over
        // a wide EV100 range, quick both ways (a new move: a new picture).
        const bool method = Override(stage_.component, "AutoExposureMethod", uint8_t{0});  // AEM_Histogram
        const bool low = Override(stage_.component, "AutoExposureMinBrightness", -10.0f), high = Override(stage_.component, "AutoExposureMaxBrightness", 20.0f);
        const bool bias = Override(stage_.component, "AutoExposureBias", -1.0f);  // a stop down: the grey stage read near white at 0
        const bool speed = Override(stage_.component, "AutoExposureSpeedUp", 6.0f) && Override(stage_.component, "AutoExposureSpeedDown", 6.0f);
        Logf("MOVE_PREVIEW capture noDepthOfField=%d exposure method=%d range=%d/%d bias=%d speed=%d", sharp ? 1 : 0, method ? 1 : 0, low ? 1 : 0, high ? 1 : 0, bias ? 1 : 0, speed ? 1 : 0);
    }
    stage_.built = floor && sky && stage_.dummy && wired;
    Logf("MOVE_PREVIEW stage at (%.0f,%.0f,%.0f) floor=%d sky=%d (radius %.0f, scale %.2f, material %s) dummy=%d capture=%d target=%d wired=%d", o.x, o.y, o.z, floor ? 1 : 0, sky ? 1 : 0,
         radius, sky_scale, Text(sky_material).c_str(), stage_.dummy ? 1 : 0, stage_.component ? 1 : 0, stage_.target ? 1 : 0, wired ? 1 : 0);
    if (!stage_.built) { Teardown("incomplete"); return false; }
    return true;
}
// The camera at the side of the boss and the post: over the middle of the two, as far as the boss is big and as they are
// apart (the boss moves in its moves); eased towards that every frame, at once the first time. The key light by it.
void FrameCamera(Object* boss, bool first) {
    double half = 90, capsule_radius = 40;
    if (Object* capsule = Property(boss, "CapsuleComponent")) {
        Call h(capsule, "GetScaledCapsuleHalfHeight"), r(capsule, "GetScaledCapsuleRadius");
        if (h.Invoke()) half = h.Get<float>("ReturnValue");
        if (r.Invoke()) capsule_radius = r.Get<float>("ReturnValue");
    }
    const double size = (std::max)(half, capsule_radius * 1.5);
    const Vector& o = stage_.origin;
    Vector b{o.x + kBossX, o.y, o.z + half};
    { Call where(boss, "K2_GetActorLocation"); if (where.Invoke()) { const Vector v = where.Get<Vector>("ReturnValue"); if (std::isfinite(v.x + v.y + v.z)) b = v; } }
    const Vector d{o.x + kDummyX, o.y, o.z};
    const double span = std::fabs(b.x - d.x) + 2.4 * size + 160;  // across the view (FOV 50: 0.93 x the distance)
    const double dist = std::clamp((std::max)(520 + 4.2 * size, span / 0.93 * 1.08), 650.0, 3600.0);
    const double aim_z = (std::max)({110.0, size * 0.9, (b.z - o.z) * 0.7});
    const Vector want{(b.x + d.x) / 2, (b.y + d.y) / 2 - dist, o.z + aim_z + dist * 0.16};
    Vector& camera = stage_.camera;
    if (first) camera = want;
    else { camera.x += (want.x - camera.x) * 0.25; camera.y += (want.y - camera.y) * 0.25; camera.z += (want.z - camera.z) * 0.25; }
    const double pitch = -std::atan2(dist * 0.16, dist) * 180.0 / 3.14159265358979;
    Call(stage_.capture, "K2_SetActorTransform").Set("NewTransform", At(camera, 90, {1, 1, 1}, pitch)).Set("bSweep", false).Set("bTeleport", true).Invoke();
    if (stage_.key && ue::Alive(stage_.key))
        Call(stage_.key, "K2_SetActorLocation").Set("NewLocation", Vector{camera.x + 250, camera.y + dist * 0.35, camera.z + 120}).Set("bSweep", false).Set("bTeleport", true).Invoke();
    if (first) {
        Object* mesh = Property(boss, "Mesh");
        const int32_t channels = Offset(mesh, "LightingChannels");
        Logf("MOVE_PREVIEW framed %s half=%.0f radius=%.0f camera %.0f cm away meshLightingChannels=0x%02x", Text(boss).c_str(), half, capsule_radius, dist,
             channels >= 0 ? ue::At<uint8_t>(mesh, channels) : 0xffu);
    }
}
// The boss's effects (its own components and its weapons'): their LOD distance is the stage camera's, not the player's
// (an emitter may thin out with distance). Every few seconds a line: how many, how many active.
void HoldEffects(Object* boss, ULONGLONG now) {
    Object* niagara = Class("/Script/Niagara.NiagaraComponent"), *cascade = Class("/Script/Engine.ParticleSystemComponent");
    if (!niagara) return;
    std::vector<Object*> owners{boss};
    {
        Call attached(boss, "GetAttachedActors"); attached.Set("bResetArray", true);
        if (attached.Invoke()) {
            const Array list = attached.Get<Array>("OutActors");
            for (int32_t i = 0; list.data && i < list.num && i < 32; ++i)
                if (Object* a = static_cast<Object**>(list.data)[i]; a && ue::Alive(a)) owners.push_back(a);
            ue::Free(list.data);
        }
    }
    const Vector& c = stage_.camera;
    int count = 0, active = 0, fresh = 0, particles = 0;
    for (Object* owner : owners)
        for (Object* cls : {niagara, cascade}) {
            if (!cls) continue;
            Call get(owner, "K2_GetComponentsByClass");
            if (!get.Set("ComponentClass", cls).Invoke()) continue;
            const Array array = get.Get<Array>("ReturnValue");
            for (int32_t i = 0; array.data && i < array.num && i < 256; ++i) {
                Object* fx = static_cast<Object**>(array.data)[i];
                if (!fx || !ue::Alive(fx)) continue;
                if (cls == cascade) { ++particles; continue; }
                ++count;
                Call on(fx, "IsActive"); if (on.Invoke() && on.Get<bool>("ReturnValue")) ++active;
                if (std::find(fx_lod_.begin(), fx_lod_.end(), fx) != fx_lod_.end()) continue;
                Vector at{}; { Call where(fx, "K2_GetComponentLocation"); if (where.Invoke()) at = where.Get<Vector>("ReturnValue"); }
                const double d = std::sqrt((at.x - c.x) * (at.x - c.x) + (at.y - c.y) * (at.y - c.y) + (at.z - c.z) * (at.z - c.z));
                const float distance = std::isfinite(d) && d < 1e5 ? static_cast<float>(d) : 800.0f;
                Call lod(fx, "SetPreviewLODDistance");
                lod.Set("bEnablePreviewLODDistance", true).Set("PreviewLODDistance", distance);
                if (!lod.Set("PreviewMaxDistance", 100000.0f).Invoke()) {
                    Call two(fx, "SetPreviewLODDistance");
                    two.Set("bEnablePreviewLODDistance", true).Set("PreviewLODDistance", distance).Invoke();
                }
                fx_lod_.push_back(fx); ++fresh;
            }
            ue::Free(array.data);
        }
    if (fx_lod_.size() > 512) fx_lod_.erase(fx_lod_.begin(), fx_lod_.begin() + 256);
    if (now >= fx_log_at_) {
        fx_log_at_ = now + 3000;
        Logf("MOVE_PREVIEW effects on %s (+%zu attached): niagara=%d active=%d new=%d cascade=%d culling=%s", Text(boss).c_str(), owners.size() - 1, count, active, fresh,
             particles, culling_paused_ ? "paused" : "on");
    }
}
// preview fx: every effect component within 60 m of the stage (also the ones spawned at a spot, owned by no actor).
void ScanEffects() {
    Object* niagara = Class("/Script/Niagara.NiagaraComponent"), *cascade = Class("/Script/Engine.ParticleSystemComponent");
    const Vector& o = stage_.origin;
    int total = 0, close_by = 0, active = 0; std::string list;
    ue::ForEachObject([&](Object* object) {
        const bool n = niagara && ue::IsA(object, niagara), p = !n && cascade && ue::IsA(object, cascade);
        if ((!n && !p) || ue::IsTemplate(object) || !ue::Alive(object)) return true;
        ++total;
        Call where(object, "K2_GetComponentLocation"); if (!where.Invoke()) return true;
        const Vector v = where.Get<Vector>("ReturnValue");
        const double d = std::sqrt((v.x - o.x) * (v.x - o.x) + (v.y - o.y) * (v.y - o.y) + (v.z - o.z) * (v.z - o.z));
        if (!(d < 6000)) return true;
        ++close_by;
        Call on(object, "IsActive"); const bool a = on.Invoke() && on.Get<bool>("ReturnValue"); if (a) ++active;
        Call owner(object, "GetOwner"); Object* who = owner.Invoke() ? owner.Get<Object*>("ReturnValue") : nullptr;
        if (list.size() < 420) list += Text(Property(object, n ? "Asset" : "Template")) + (a ? "+" : "-") + (p ? "(cascade)" : "") + "@" + Text(who) + " ";
        return true;
    });
    Logf("MOVE_PREVIEW fx scan components=%d nearStage=%d active=%d culling=%s: %s", total, close_by, active, culling_paused_ ? "paused" : "on", list.c_str());
}
bool WidgetAlive() { return widget_ && ue::Alive(widget_) && ue::NameOf(widget_) == widget_name_ && image_ && ue::Alive(image_); }
void DropWidget(const char* why) {
    if (!widget_) return;
    if (WidgetAlive()) Call(widget_, "RemoveFromParent").Invoke();
    Logf("MOVE_PREVIEW live view removed (%s)", why);
    widget_ = nullptr; image_ = nullptr; live_ = false; placed_ = {};
}
void SizeTarget(int width, int height) {
    if (!stage_.target || (std::abs(width - target_w_) <= 4 && std::abs(height - target_h_) <= 4)) return;
    if (Call(ue::Default("/Script/Engine.Default__KismetRenderingLibrary"), "ResizeRenderTarget2D").Set("TextureRenderTarget", stage_.target)
            .Set("Width", int32_t{width}).Set("Height", int32_t{height}).Invoke()) { target_w_ = width; target_h_ = height; }
}
// The live view at `view` (game client pixels). False when it cannot be made: the page shows read-back frames then.
bool ShowLive(const Context& context, const RECT& view) {
    if (live_failed_ || !stage_.target) return false;
    if (!WidgetAlive()) {
        widget_ = nullptr; image_ = nullptr; live_ = false; placed_ = {};
        Object* cls = Class("/Script/CommonUI.CommonUserWidget"), *image_class = Class("/Script/UMG.Image");
        Call create(ue::Default("/Script/UMG.Default__WidgetBlueprintLibrary"), "Create");
        Object* widget = cls && image_class && create.Set("WorldContextObject", context.world).Set("WidgetType", cls).Set("OwningPlayer", context.controller).Invoke()
            ? create.Get<Object*>("ReturnValue") : nullptr;
        if (widget && !ue::Alive(widget)) widget = nullptr;
        Object* tree = Property(widget, "WidgetTree");
        Call spawn(ue::Default("/Script/Engine.Default__GameplayStatics"), "SpawnObject");
        Object* image = tree && spawn.Set("ObjectClass", image_class).Set("Outer", tree).Invoke() ? spawn.Get<Object*>("ReturnValue") : nullptr;
        if (image && !ue::Alive(image)) image = nullptr;
        const bool rooted = image && Put(tree, "RootWidget", image);
        const bool brush = rooted && Call(image, "SetBrushResourceObject").Set("ResourceObject", stage_.target).Invoke();
        if (!brush) {
            live_failed_ = true;
            Logf("MOVE_PREVIEW live view unavailable (widget class=%d image class=%d widget=%d tree=%d image=%d rooted=%d): read-back frames", cls ? 1 : 0, image_class ? 1 : 0,
                 widget ? 1 : 0, tree ? 1 : 0, image ? 1 : 0, rooted ? 1 : 0);
            return false;
        }
        Call(widget, "SetVisibility").Set("InVisibility", uint8_t{3}).Invoke();  // HitTestInvisible
        Call(widget, "AddToViewport").Set("ZOrder", int32_t{2000000000}).Invoke();  // over the game's own HUD layers (the engine adds 10)
        widget_ = widget; widget_name_ = ue::NameOf(widget); image_ = image;
        Logf("MOVE_PREVIEW live view %s (%s, image %s) shows %s", Text(widget).c_str(), Text(cls).c_str(), Text(image).c_str(), Text(stage_.target).c_str());
    }
    if (!EqualRect(&view, &placed_)) {
        float scale = 1;
        { Call get(ue::Default("/Script/UMG.Default__WidgetLayoutLibrary"), "GetViewportScale"); if (get.Set("WorldContextObject", context.world).Invoke()) scale = get.Get<float>("ReturnValue"); }
        if (!(scale > 0.05f && scale < 20.0f)) scale = 1;
        struct Vector2 { double x, y; };
        const int width = view.right - view.left, height = view.bottom - view.top;
        Call(widget_, "SetPositionInViewport").Set("Position", Vector2{view.left / scale, view.top / scale}).Set("bRemoveDPIScale", false).Invoke();
        Call(widget_, "SetDesiredSizeInViewport").Set("Size", Vector2{width / scale, height / scale}).Invoke();
        SizeTarget(std::clamp(width, 64, 1600), std::clamp(height, 36, 900));  // the picture's own pixels: sharp
        if (!placed_.right) Logf("MOVE_PREVIEW live view at (%ld,%ld) %dx%d px, viewport scale %.2f, target %dx%d", view.left, view.top, width, height, scale, target_w_, target_h_);
        placed_ = view;
    }
    Call in(widget_, "IsInViewport");
    live_ = in.Invoke() && in.Get<bool>("ReturnValue");
    return live_;
}
void Unfreeze() {
    if (frozen_.empty()) return;
    size_t back = 0;
    for (const Frozen& f : frozen_) {
        if (!ue::Alive(f.actor) || !(ue::NameOf(f.actor) == f.name)) continue;
        const int32_t at = Offset(f.actor, "CustomTimeDilation");
        if (at >= 0 && ue::At<float>(f.actor, at) == 0.0f) { ue::At<float>(f.actor, at) = f.before; ++back; }
    }
    Logf("MOVE_PREVIEW gameplay given back: %zu of %zu actors", back, frozen_.size());
    frozen_.clear(); freeze_scan_at_ = 0;
}
void Freeze(const Context& context, ULONGLONG now) {
    for (Frozen& f : frozen_) {  // the game's own slow motion may have set one again
        if (!ue::Alive(f.actor) || !(ue::NameOf(f.actor) == f.name)) continue;
        const int32_t at = Offset(f.actor, "CustomTimeDilation");
        if (at >= 0 && ue::At<float>(f.actor, at) != 0.0f) { f.before = ue::At<float>(f.actor, at); ue::At<float>(f.actor, at) = 0.0f; }
    }
    if (now < freeze_scan_at_) return;
    freeze_scan_at_ = now + 400;
    Object* pawn_class = Class("/Script/Engine.Pawn");
    Call all(ue::Default("/Script/Engine.Default__GameplayStatics"), "GetAllActorsOfClass");
    if (!pawn_class || !all.Set("WorldContextObject", context.world).Set("ActorClass", pawn_class).Invoke()) return;
    const Array list = all.Get<Array>("OutActors");
    const size_t before = frozen_.size();
    const auto hold = [&](Object* actor) {
        if (!actor || !ue::Alive(actor)) return;
        for (const Frozen& f : frozen_) if (f.actor == actor) return;
        const int32_t at = Offset(actor, "CustomTimeDilation");
        if (at < 0) return;
        frozen_.push_back({actor, ue::NameOf(actor), ue::At<float>(actor, at)});
        ue::At<float>(actor, at) = 0.0f;
    };
    const Vector& o = stage_.origin;
    for (int32_t i = 0; list.data && i < list.num && i < 4096; ++i) {
        Object* pawn = static_cast<Object**>(list.data)[i];
        if (!pawn || !ue::Alive(pawn)) continue;
        Call where(pawn, "K2_GetActorLocation");
        if (stage_.built && where.Invoke()) {
            const Vector v = where.Get<Vector>("ReturnValue");
            if (std::fabs(v.x - o.x) < kStageKeepClearCm && std::fabs(v.y - o.y) < kStageKeepClearCm && std::fabs(v.z - o.z) < kStageKeepClearCm) continue;  // the stage's
        }
        hold(pawn); hold(Property(pawn, "Controller"));
    }
    ue::Free(list.data);
    if (frozen_.size() != before) Logf("MOVE_PREVIEW gameplay held while the hub is open: %zu actors (pawns and their controllers)", frozen_.size());
}
// HDR scene colour -> exposure (log-average, eased) -> ACES fit -> sRGB -> BGRA.
void Read(Object* context_object) {
    SizeTarget(kWidth, kHeight);
    Call read(ue::Default("/Script/Engine.Default__KismetRenderingLibrary"), "ReadRenderTargetRaw");
    if (!read.Ok() || !read.Set("WorldContextObject", context_object).Set("TextureRenderTarget", stage_.target).Set("bNormalize", false).Invoke()) {
        if (++read_fails_ <= 3) Logf("MOVE_PREVIEW ReadRenderTargetRaw failed (ok=%d)", read.Ok() ? 1 : 0);
        return;
    }
    const Array samples = read.Get<Array>("OutLinearSamples");
    const size_t count = static_cast<size_t>(kWidth) * kHeight;
    if (!samples.data || samples.num != static_cast<int32_t>(count)) {
        if (++read_fails_ <= 3) Logf("MOVE_PREVIEW read %d samples, want %zu", samples.num, count);
        ue::Free(samples.data); return;
    }
    const LinearColor* in = static_cast<const LinearColor*>(samples.data);
    double log_sum = 0;
    for (size_t i = 0; i < count; i += 7) {
        const double l = 0.2126 * in[i].r + 0.7152 * in[i].g + 0.0722 * in[i].b;
        log_sum += std::log(1e-4 + (std::isfinite(l) ? (std::max)(0.0, l) : 0.0));
    }
    const double average = std::exp(log_sum / double((count + 6) / 7));
    const double wanted = std::clamp(0.18 / (std::max)(average, 1e-6), 1e-3, 1e4);
    exposure_ = reads_ ? exposure_ + (wanted - exposure_) * 0.35 : wanted;
    std::vector<uint32_t> out(count);
    const auto tone = [&](float v) {
        double x = (std::isfinite(v) ? (std::max)(0.0f, v) : 0.0) * exposure_;
        x = (x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14);
        x = std::clamp(x, 0.0, 1.0);
        x = x <= 0.0031308 ? 12.92 * x : 1.055 * std::pow(x, 1 / 2.4) - 0.055;
        return static_cast<uint32_t>(std::lround(x * 255));
    };
    for (size_t i = 0; i < count; ++i) out[i] = 0xff000000u | (tone(in[i].r) << 16) | (tone(in[i].g) << 8) | tone(in[i].b);
    ue::Free(samples.data);
    ++reads_;
    AcquireSRWLockExclusive(&lock_);
    frame_.pixels.swap(out); frame_.status.clear(); ++frame_.serial;
    ReleaseSRWLockExclusive(&lock_);
}
void Tick() {
    if (disabled_) return;
    std::string monster, ability; bool shown = false; ULONGLONG hidden_at = 0;
    int speed = 100, trim_from = 0, trim_to = 0;std::vector<int> holds; std::vector<NpcCombo::CurvePoint> curve;
    AcquireSRWLockShared(&lock_); monster = want_monster_; ability = want_ability_; shown = want_shown_; hidden_at = hidden_at_; speed = want_speed_; curve = want_curve_;
    trim_from = want_trim_from_; trim_to = want_trim_to_;holds=want_trim_holds_;
    ReleaseSRWLockShared(&lock_);
    if(!shown)BossAssist::PreviewTrim(trim_from,trim_to,holds,false);
    const ULONGLONG now = GetTickCount64();
    if (!shown) {
        if (!monster_.empty()) { BossAssist::StopPreview(); monster_.clear(); ability_.clear(); framed_ = false; fx_lod_.clear(); }
        DropWidget("hidden"); Unfreeze(); SetLive(false);
        if (culling_paused_) { Context context; if (ContextNow(context)) PauseCulling(context.world, false); }
        if (stage_.built && now > hidden_at + kStageKeepMs) Teardown("hidden");
        return;
    }
    Context context;
    if (!ContextNow(context)) { SetStatus(L"没有可用的场景"); return; }
    if (HubPause::Wanted()) Freeze(context, now); else Unfreeze();
    if (stage_.built && (stage_.world != context.world || !ue::Alive(stage_.capture) || !ue::Alive(stage_.target))) Teardown("world changed");
    if (!stage_.built) {
        if (now < retry_at_) return;
        SetStatus(L"正在搭建预览场景…");
        if (!Build(context)) { retry_at_ = now + kRetryMs; SetStatus(L"预览场景无法创建"); return; }
    }
    PauseCulling(context.world, !keep_culling_);  // before the boss's first cast: no effect of it is culled at birth
    if (monster != monster_ || ability != ability_) {
        // The hub's pause is lifted for the preview later this frame (HubPause after us): the boss is made on a running
        // world (one spawned and cast while paused may never start its move).
        bool paused = false;
        { Call get(ue::Default("/Script/Engine.Default__GameplayStatics"), "IsGamePaused"); if (get.Set("WorldContextObject", context.world).Invoke()) paused = get.Get<bool>("ReturnValue"); }
        if (paused) { SetStatus(L"正在载入招式…"); return; }
        BossAssist::StopPreview(); framed_ = false; monster_ = monster; ability_ = ability; fx_lod_.clear(); fx_log_at_ = now + 1500;
        SetStatus(L"正在载入招式…");
        const double at[3] = {stage_.origin.x + kBossX, stage_.origin.y, stage_.origin.z};
        if (!BossAssist::StartPreview(monster, ability, at, 0, stage_.dummy)) { SetStatus(L"这一招无法预览"); return; }
    }
    BossAssist::PreviewSpeed(speed, curve);  // held on the boss every frame
    DWORD foreground_pid=0;GetWindowThreadProcessId(GetForegroundWindow(),&foreground_pid);
    const bool holding=foreground_pid==GetCurrentProcessId()&&(GetAsyncKeyState(VK_SPACE)&0x8000)!=0;
    BossAssist::PreviewTrim(trim_from, trim_to,holds,holding);
    Object* boss = BossAssist::PreviewBoss();
    if (boss) { FrameCamera(boss, !framed_); framed_ = true; }
    RECT view{}; int source = -1;
    AcquireSRWLockShared(&lock_); view = view_; source = source_override_; ReleaseSRWLockShared(&lock_);
    const bool live = view.right > view.left && view.bottom > view.top && ShowLive(context, view);
    if (!live && widget_) DropWidget("no picture rect");
    SetLive(live);
    if (stage_.component && ue::Alive(stage_.component)) Put(stage_.component, "CaptureSource", static_cast<uint8_t>(source >= 0 ? source : live ? kLiveSource : kReadSource));
    if (now >= next_read_) { next_read_ = now + kReadMs; if (!live) Read(context.world); if (boss) HoldEffects(boss, now); }
    if (fx_scan_) { fx_scan_ = false; ScanEffects(); }
}
}  // namespace

void Initialize(Logger logger) { logger_ = logger; }
void Debug(const std::string& what, int value) {
    AcquireSRWLockExclusive(&lock_);
    if (what == "source" && value >= -1 && value <= 9) source_override_ = value;
    else if (what == "cull") keep_culling_ = value != 0;
    else if (what == "fx") fx_scan_ = true;
    else BossAssist::PreviewOption(what, value);
    ReleaseSRWLockExclusive(&lock_);
    Logf("MOVE_PREVIEW debug %s %d", what.c_str(), value);
}
void Show(const std::string& monster, const std::string& ability, int speed, const std::vector<NpcCombo::CurvePoint>& curve, int trim_from, int trim_to, const BoneMotion::Config& bones,const std::vector<int>& holds) {
    AcquireSRWLockExclusive(&lock_);
    BoneMotionRuntime::Preview(bones);
    want_trim_holds_=NpcCombo::TrimHolds(holds);want_trim_from_ = NpcCombo::ClampTrim(trim_from); want_trim_to_ = NpcCombo::ClampTrim(trim_to);
    want_monster_ = monster; want_ability_ = ability; want_shown_ = !monster.empty() && !ability.empty();
    want_speed_ = NpcCombo::ClampSpeed(speed); want_curve_ = curve.size() >= 2 ? curve : std::vector<NpcCombo::CurvePoint>{};
    ReleaseSRWLockExclusive(&lock_);
}
void Hide() {
    BoneMotionRuntime::Preview({});
    AcquireSRWLockExclusive(&lock_);
    if (want_shown_) hidden_at_ = GetTickCount64();
    want_shown_ = false; view_ = {};
    ReleaseSRWLockExclusive(&lock_);
}
void Place(const RECT& client) { AcquireSRWLockExclusive(&lock_); view_ = client; ReleaseSRWLockExclusive(&lock_); }
bool NeedsWorld() { AcquireSRWLockShared(&lock_); const bool shown = want_shown_; ReleaseSRWLockShared(&lock_); return shown && !disabled_; }
bool HoldsGameplay() { return !frozen_.empty(); }
bool Latest(Frame& frame, uint64_t newer_than) {
    AcquireSRWLockShared(&lock_);
    const bool newer = frame_.serial > newer_than;
    if (newer) frame = frame_;
    ReleaseSRWLockShared(&lock_);
    return newer;
}
void OnGameThreadGuarded() {
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("MOVE_PREVIEW fault 0x%08lx", GetExceptionCode());
        stage_.built = false; widget_ = nullptr; image_ = nullptr; live_ = false;
        __try { Unfreeze(); } __except (EXCEPTION_EXECUTE_HANDLER) { frozen_.clear(); }
        if (++faults_ >= kMaxFaults) { disabled_ = true; Logf("MOVE_PREVIEW disabled after %lu faults", faults_); }
    }
}
}  // namespace MovePreview
