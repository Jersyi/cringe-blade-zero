#include "net_puppet.h"
#include "net_steam.h"
#include "motian_grab.h"
#include "npc_retarget.h"
#include "ue_runtime.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

namespace NetPuppet {
namespace {
using ue::Object; using ue::Call; using ue::Name; using ue::Array;
struct Vector { double x, y, z; };
struct Rotator { double pitch, yaw, roll; };
struct alignas(16) Transform { double rotation[4]{0, 0, 0, 1}, translation[4]{}, scale[4]{1, 1, 1, 0}; };
static_assert(sizeof(Transform) == 96, "PBZ native FTransform layout");
struct SoftObject { int32_t index, serial; Name package, asset; ue::String subpath; };
static_assert(sizeof(SoftObject) == 40, "TSoftObjectPtr layout");

constexpr double kPi = 3.14159265358979, kBesideCm = 300, kSnapCm = 600;
constexpr ULONGLONG kStateMs = 33, kPoseMs = 33, kScanMs = 500, kPartsResendMs = 4000, kCameraGuardMs = 10000, kPublishMs = 100, kRateMs = 2000;
constexpr ULONGLONG kReportsMs[] = {0, 1000, 5000};
constexpr size_t kMaxParts = 96, kMaxBones = 1500;
constexpr uint16_t kNone = 0xFFFF;
constexpr uint8_t kSpaceWorld = 0, kSpaceParentBone = 3;  // ERelativeTransformSpace
constexpr uint8_t kBoneComponentSpace = 1;                // EBoneSpaces::ComponentSpace
// GroomComponent.SimulationSettings: copied as GroomForce copies it (0x90 bytes, the LocalBone FString cleared).
constexpr size_t kGroomSettingsBytes = 0x90, kLocalBoneString = 0x08 + 0x10;
// Pose entry flags: shown, bones follow, its scale is not 1, the source is gone (nothing follows).
constexpr uint8_t kShown = 1, kBones = 2, kScaled = 4, kGone = 0x80;
// Bone flags: its translation follows as floats; its scale follows (else 1); it is in the mesh's reference pose (nothing
// follows; the high 4 bits: how many more bones after it are, up to 15); its translation follows as 16-bit offsets from
// the reference one in 0.01 cm. Without a translation flag the translation is the reference one.
constexpr uint8_t kBoneMoved = 1, kBoneScaled = 2, kBoneRef = 4, kBoneNear = 8;
// Poses go out 30 a second, less often while they are big: at most kPoseBudget bytes a second (Steam sends 256 KB a
// second by default; a grab with its boss body and victim is about 7 KB a pose).
constexpr double kPoseBudget = 160.0 * 1024;

Logger logger_ = nullptr;
SRWLOCK lock_ = SRWLOCK_INIT;
std::vector<Request> requests_;
Snapshot snapshot_;

Object* engine_ = nullptr;
Object* puppet_ = nullptr; Name puppet_name_{};
Object* hero_ = nullptr; Name hero_name_{};
bool mirror_ = true, auto_spawn_ = false, peer_was_ready_ = false;
Vector offset_{};
ULONGLONG spawned_at_ = 0, next_state_ = 0, last_tick_ = 0, next_camera_ = 0, next_publish_ = 0;
size_t reports_ = 0;
std::string cls_name_, controller_name_, local_map_;
int camera_fixes_ = 0, possess_fixes_ = 0, source_ = 0;
std::wstring note_[2];
DWORD faults_ = 0; bool disabled_ = false;
// The step Tick is in (logged with a fault) and the part being made.
const char* stage_ = "idle";
char stage_part_[200] = "-";

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[900]{}; va_list arguments; va_start(arguments, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments); va_end(arguments);
    logger_(text);
}
void Note(const wchar_t* zh, const wchar_t* en) { note_[0] = zh; note_[1] = en; next_publish_ = 0; }
std::string Text(Object* object) { char text[200]{}; if (object && ue::Alive(object)) ue::NameText(ue::NameOf(object), text, sizeof(text)); return text[0] ? text : "-"; }
int32_t Offset(Object* object, const char* property) { return object ? ue::Offset(ue::ClassOf(object), property) : -1; }
Object* Property(Object* object, const char* property) {
    const int32_t offset = Offset(object, property);
    Object* value = offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
    return value && ue::Alive(value) ? value : nullptr;
}
// A TWeakObjectPtr property (ObjectIndex, SerialNumber): the live object at its index.
Object* Weak(Object* object, const char* property) {
    const int32_t offset = Offset(object, property);
    if (offset < 0) return nullptr;
    const int32_t index = ue::At<int32_t>(object, offset);
    Object* value = index > 0 ? ue::detail::ObjectAt(index) : nullptr;
    return value && ue::Alive(value) ? value : nullptr;
}
// A bitfield bool property (FBoolProperty: field size at 0x70, byte offset 0x71, mask 0x73), as NpcRetarget reads them.
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
bool Bool(Object* object, const char* property, bool& value) {
    const uintptr_t field = object ? Field(ue::ClassOf(object), property) : 0;
    if (!field || ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x70) != 1) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    const uint8_t byte_offset = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x71), mask = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x73);
    if (byte_offset > 7 || !mask) return false;
    value = (ue::At<uint8_t>(object, offset + byte_offset) & mask) != 0; return true;
}
bool Same(Object* object, Name name) { return object && ue::Alive(object) && ue::NameOf(object) == name; }
bool PuppetAlive() { return Same(puppet_, puppet_name_); }
struct Context { Object* world = nullptr; Object* controller = nullptr; Object* pawn = nullptr; };
bool ContextNow(Context& context) {
    if (!engine_ || !ue::Alive(engine_)) engine_ = ue::FindEngine();
    context.world = Property(Property(engine_, "GameViewport"), "World");
    Object* instance = Property(context.world, "OwningGameInstance");
    const int32_t players = Offset(instance, "LocalPlayers");
    if (players < 0) return false;
    const Array list = ue::At<Array>(instance, players);
    if (!list.data || list.num <= 0 || list.num > 8) return false;
    context.controller = Property(static_cast<Object**>(list.data)[0], "PlayerController");
    context.pawn = Property(context.controller, "Pawn");
    return context.pawn != nullptr;
}
bool Location(Object* actor, Vector& out) {
    Call where(actor, "K2_GetActorLocation"); if (!where.Invoke()) return false;
    out = where.Get<Vector>("ReturnValue"); return std::isfinite(out.x + out.y + out.z);
}
double Yaw(Object* actor) { Call get(actor, "K2_GetActorRotation"); return get.Invoke() ? get.Get<Rotator>("ReturnValue").yaw : 0; }
Vector Velocity(Object* actor) { Call get(actor, "GetVelocity"); return get.Invoke() ? get.Get<Vector>("ReturnValue") : Vector{}; }
float Dilation(Object* actor) { const int32_t at = Offset(actor, "CustomTimeDilation"); return at >= 0 ? ue::At<float>(actor, at) : 1.0f; }
Object* Asc(Object* actor) {
    Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetAbilitySystemComponent");
    Object* asc = get.Set("Actor", actor).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    return asc && ue::Alive(asc) ? asc : nullptr;
}
std::string ClassName(Object* object) { return object && ue::Alive(object) ? Text(ue::ClassOf(object)) : "-"; }
// "/Game/Path/AM_X.AM_X" for an asset in a package (as FindObject takes it).
std::string PathOf(Object* object) {
    if (!object || !ue::Alive(object)) return std::string();
    Object* outer = ue::OuterOf(object);
    if (!outer) return Text(object);
    return Text(outer) + "." + Text(object);
}
std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }
Object* FindAsset(const std::string& path) {
    if (path.empty()) return nullptr;
    if (Object* asset = ue::FindObject(path.c_str())) return asset;
    const size_t dot = path.find('.');
    if (dot == std::string::npos || path.compare(0, 8, "/Script/") == 0 || path.compare(0, 16, "/Engine/Transient") == 0) return nullptr;
    SoftObject soft{};
    const std::wstring package = Wide(path.substr(0, dot)), asset = Wide(path.substr(dot + 1));
    soft.package = ue::MakeName(package.c_str()); soft.asset = ue::MakeName(asset.c_str());
    Call load(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LoadAsset_Blocking");
    Object* object = load.Set("Asset", soft).Invoke() ? load.Get<Object*>("ReturnValue") : nullptr;
    return object && ue::Alive(object) ? object : nullptr;
}
std::vector<Object*> Components(Object* actor, Object* cls) {
    std::vector<Object*> result;
    if (!actor || !cls) return result;
    Call get(actor, "K2_GetComponentsByClass");
    if (!get.Set("ComponentClass", cls).Invoke()) return result;
    const Array array = get.Get<Array>("ReturnValue");
    if (array.num >= 0 && array.num < 2048 && (array.num == 0 || array.data))
        for (int32_t i = 0; i < array.num; ++i) if (ue::Alive(static_cast<Object**>(array.data)[i])) result.push_back(static_cast<Object**>(array.data)[i]);
    ue::Free(array.data);
    return result;
}
std::vector<Object*> AttachedActors(Object* actor) {
    std::vector<Object*> result;
    Call attached(actor, "GetAttachedActors");
    if (!attached.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Invoke()) return result;
    const Array array = attached.Get<Array>("OutActors");
    if (array.num >= 0 && array.num < 512 && (array.num == 0 || array.data))
        for (int32_t i = 0; i < array.num; ++i) if (ue::Alive(static_cast<Object**>(array.data)[i])) result.push_back(static_cast<Object**>(array.data)[i]);
    ue::Free(array.data);
    return result;
}
bool WorldOf(Object* component, Transform& out) {
    Call get(component, "K2_GetComponentToWorld");
    if (!get.Invoke()) return false;
    out = get.Get<Transform>("ReturnValue"); return true;
}
Object* Cls(const char* path) { return ue::FindObject(path); }
// A memory range that can be read (SEH: a bad pointer read is caught).
bool Readable(const void* at, size_t bytes) {
    if (!at || !bytes) return false;
    __try { volatile unsigned char sink = 0; const unsigned char* p = static_cast<const unsigned char*>(at); for (size_t i = 0; i < bytes; i += 4096) sink ^= p[i]; sink ^= p[bytes - 1]; (void)sink; return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// ---- transforms ----------------------------------------------------------------------------------------------------
struct Quat { double x = 0, y = 0, z = 0, w = 1; };
struct Vec3 { double x = 0, y = 0, z = 0; };
Quat QuatOf(const Transform& t) { return {t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]}; }
Vec3 MoveOf(const Transform& t) { return {t.translation[0], t.translation[1], t.translation[2]}; }
Vec3 ScaleOf(const Transform& t) { return {t.scale[0], t.scale[1], t.scale[2]}; }
Transform Make(const Quat& q, const Vec3& t, const Vec3& s) {
    Transform r; r.rotation[0] = q.x; r.rotation[1] = q.y; r.rotation[2] = q.z; r.rotation[3] = q.w;
    r.translation[0] = t.x; r.translation[1] = t.y; r.translation[2] = t.z; r.scale[0] = s.x; r.scale[1] = s.y; r.scale[2] = s.z; return r;
}
Quat Mul(const Quat& a, const Quat& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
Quat Conj(const Quat& q) { return {-q.x, -q.y, -q.z, q.w}; }
Quat Unit(Quat q) { const double n = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w); if (n < 1e-12) return {}; q.x /= n; q.y /= n; q.z /= n; q.w /= n; return q; }
Vec3 Rotate(const Quat& q, const Vec3& v) {
    const Vec3 t{2 * (q.y * v.z - q.z * v.y), 2 * (q.z * v.x - q.x * v.z), 2 * (q.x * v.y - q.y * v.x)};
    return {v.x + q.w * t.x + (q.y * t.z - q.z * t.y), v.y + q.w * t.y + (q.z * t.x - q.x * t.z), v.z + q.w * t.z + (q.x * t.y - q.y * t.x)};
}
double Safe(double v) { return std::fabs(v) < 1e-8 ? 1.0 : v; }
// `a` in `b`'s frame (FTransform::GetRelativeTransform: a * b^-1).
Transform RelativeTo(const Transform& a, const Transform& b) {
    const Quat inverse = Conj(Unit(QuatOf(b)));
    const Vec3 sb = ScaleOf(b), d{a.translation[0] - b.translation[0], a.translation[1] - b.translation[1], a.translation[2] - b.translation[2]};
    const Vec3 r = Rotate(inverse, d), sa = ScaleOf(a);
    return Make(Unit(Mul(inverse, QuatOf(a))), {r.x / Safe(sb.x), r.y / Safe(sb.y), r.z / Safe(sb.z)}, {sa.x / Safe(sb.x), sa.y / Safe(sb.y), sa.z / Safe(sb.z)});
}
// `local` under `parent` (FTransform local * parent).
Transform Compose(const Transform& local, const Transform& parent) {
    const Quat qp = QuatOf(parent); const Vec3 sp = ScaleOf(parent), tl = MoveOf(local), sl = ScaleOf(local);
    const Vec3 r = Rotate(qp, {tl.x * sp.x, tl.y * sp.y, tl.z * sp.z});
    return Make(Unit(Mul(qp, QuatOf(local))), {r.x + parent.translation[0], r.y + parent.translation[1], r.z + parent.translation[2]}, {sl.x * sp.x, sl.y * sp.y, sl.z * sp.z});
}
Quat Nlerp(const Quat& a, Quat b, double k) {
    if (a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w < 0) { b.x = -b.x; b.y = -b.y; b.z = -b.z; b.w = -b.w; }
    return Unit({a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k, a.w + (b.w - a.w) * k});
}
Vec3 Lerp(const Vec3& a, const Vec3& b, double k) { return {a.x + (b.x - a.x) * k, a.y + (b.y - a.y) * k, a.z + (b.z - a.z) * k}; }
Transform Blend(const Transform& a, const Transform& b, double k) { return Make(Nlerp(QuatOf(a), QuatOf(b), k), Lerp(MoveOf(a), MoveOf(b), k), Lerp(ScaleOf(a), ScaleOf(b), k)); }
bool Close(const Transform& a, const Transform& b) {
    for (int i = 0; i < 3; ++i) if (std::fabs(a.translation[i] - b.translation[i]) > 0.02 || std::fabs(a.scale[i] - b.scale[i]) > 1e-4) return false;
    const double dot = std::fabs(a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] + a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3]);
    return dot > 1 - 1e-8;
}
bool Unscaled(const Vec3& s) { return std::fabs(s.x - 1) < 1e-4 && std::fabs(s.y - 1) < 1e-4 && std::fabs(s.z - 1) < 1e-4; }

// ---- the wire ------------------------------------------------------------------------------------------------------
struct Writer {
    std::vector<unsigned char> b;
    template<class T> void Put(const T& v) { const unsigned char* p = reinterpret_cast<const unsigned char*>(&v); b.insert(b.end(), p, p + sizeof(T)); }
    void Str(const std::string& s) { const uint16_t n = static_cast<uint16_t>((std::min)(s.size(), static_cast<size_t>(1000))); Put(n); b.insert(b.end(), s.begin(), s.begin() + n); }
    // A unit quaternion in 6 bytes: the index of its largest component (made positive) and the other three in 15 bits.
    void Q(Quat q) {
        q = Unit(q); const double c[4] = {q.x, q.y, q.z, q.w};
        int big = 0; for (int i = 1; i < 4; ++i) if (std::fabs(c[i]) > std::fabs(c[big])) big = i;
        const double sign = c[big] < 0 ? -1 : 1;
        uint64_t bits = static_cast<uint64_t>(big); int k = 0;
        for (int i = 0; i < 4; ++i) {
            if (i == big) continue;
            const double v = std::clamp(c[i] * sign * 1.4142135623730951, -1.0, 1.0);
            bits |= static_cast<uint64_t>(std::lround((v + 1) * 0.5 * 32767)) << (2 + 15 * k++);
        }
        for (int i = 0; i < 6; ++i) b.push_back(static_cast<unsigned char>(bits >> (8 * i)));
    }
    void Move(const Vec3& t) { Put(static_cast<float>(t.x)); Put(static_cast<float>(t.y)); Put(static_cast<float>(t.z)); }
};
struct Reader {
    const unsigned char* p = nullptr; size_t n = 0, at = 0; bool bad = false;
    template<class T> T Get() { T v{}; if (bad || at + sizeof(T) > n) { bad = true; return v; } std::memcpy(&v, p + at, sizeof(T)); at += sizeof(T); return v; }
    std::string Str() { const uint16_t len = Get<uint16_t>(); if (bad || at + len > n) { bad = true; return {}; } std::string s(reinterpret_cast<const char*>(p + at), len); at += len; return s; }
    Quat Q() {
        if (bad || at + 6 > n) { bad = true; return {}; }
        uint64_t bits = 0; for (int i = 0; i < 6; ++i) bits |= static_cast<uint64_t>(p[at + i]) << (8 * i);
        at += 6;
        const int big = static_cast<int>(bits & 3); double c[4]{}; double sum = 0; int k = 0;
        for (int i = 0; i < 4; ++i) {
            if (i == big) continue;
            const double v = static_cast<double>((bits >> (2 + 15 * k++)) & 0x7fff) / 32767.0 * 2 - 1;
            c[i] = v / 1.4142135623730951; sum += c[i] * c[i];
        }
        c[big] = std::sqrt((std::max)(0.0, 1 - sum));
        return Unit({c[0], c[1], c[2], c[3]});
    }
    Vec3 Move() { const float x = Get<float>(), y = Get<float>(), z = Get<float>(); return {x, y, z}; }
};

// One part of what the hero shows: a mesh (0 static, 1 skinned, 2 Groom, 3 Chaos Cloth: the hero's hair cards, upper
// cloth, skirt and cape) with its asset, materials and where it hangs: on part `parent` at `socket` (kNone: the root).
// A skinned part either poses itself (`posed`: its bones are sent) or takes the pose of part `leader` (leader pose).
struct PartDesc {
    uint16_t id = 0, parent = kNone, leader = kNone, bones = 0; uint8_t kind = 0, posed = 0, main_hair = 0, local_sim = 0;
    std::string socket, asset, binding, physics; std::vector<std::string> materials; std::vector<unsigned char> settings;
};
void WriteDesc(Writer& w, const PartDesc& d) {
    w.Put(d.id); w.Put(d.kind); w.Put(d.posed); w.Put(d.parent); w.Put(d.leader); w.Put(d.bones);
    w.Str(d.socket); w.Str(d.asset);
    w.Put(static_cast<uint8_t>((std::min)(d.materials.size(), static_cast<size_t>(64)))); for (size_t i = 0; i < d.materials.size() && i < 64; ++i) w.Str(d.materials[i]);
    if (d.kind == 2) {
        w.Str(d.binding); w.Str(d.physics); w.Put(d.main_hair); w.Put(d.local_sim);
        w.Put(static_cast<uint16_t>(d.settings.size())); w.b.insert(w.b.end(), d.settings.begin(), d.settings.end());
    }
}
PartDesc ReadDesc(Reader& r) {
    PartDesc d; d.id = r.Get<uint16_t>(); d.kind = r.Get<uint8_t>(); d.posed = r.Get<uint8_t>(); d.parent = r.Get<uint16_t>(); d.leader = r.Get<uint16_t>(); d.bones = r.Get<uint16_t>();
    d.socket = r.Str(); d.asset = r.Str();
    const uint8_t materials = r.Get<uint8_t>(); for (uint8_t i = 0; i < materials && !r.bad; ++i) d.materials.push_back(r.Str());
    if (d.kind == 2) {
        d.binding = r.Str(); d.physics = r.Str(); d.main_hair = r.Get<uint8_t>(); d.local_sim = r.Get<uint8_t>();
        const uint16_t bytes = r.Get<uint16_t>();
        if (!r.bad && r.at + bytes <= r.n) { d.settings.assign(r.p + r.at, r.p + r.at + bytes); r.at += bytes; } else r.bad = true;
    }
    if (d.kind > 3) r.bad = true;
    return d;
}
// The same component on the puppet can show it (only materials may differ).
bool SameShape(const PartDesc& a, const PartDesc& b) {
    return a.kind == b.kind && a.posed == b.posed && a.parent == b.parent && a.leader == b.leader && a.socket == b.socket && a.asset == b.asset &&
           a.binding == b.binding && a.physics == b.physics;
}

// What a part's component shows: its own flags and its actor's.
bool SourceShown(Object* component) {
    bool visible = false, hidden = false, actor_hidden = false;
    if (!Bool(component, "bVisible", visible) || !visible) return false;
    if (Bool(component, "bHiddenInGame", hidden) && hidden) return false;
    Call owner(component, "GetOwner");
    Object* actor = owner.Invoke() ? owner.Get<Object*>("ReturnValue") : nullptr;
    return !(actor && Bool(actor, "bHidden", actor_hidden) && actor_hidden);
}
Object* MeshAsset(Object* component, int kind) {
    if (kind == 0) return Property(component, "StaticMesh");
    if (kind == 1) { Object* asset = Property(component, "SkinnedAsset"); return asset ? asset : Property(component, "SkeletalMesh"); }
    if (kind == 3) {
        if (Object* asset = Property(component, "SkinnedAsset")) return asset;
        Call get(component, "GetClothAsset"); Object* asset = get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
        return asset && ue::Alive(asset) ? asset : nullptr;
    }
    return Property(component, "GroomAsset");
}
// A material's asset path (a dynamic instance made at run time: its parent's; "" none).
std::string MaterialPath(Object* material) {
    Object* dynamic = Cls("/Script/Engine.MaterialInstanceDynamic");
    for (int i = 0; i < 4 && material && dynamic && ue::IsA(material, dynamic); ++i) material = Property(material, "Parent");
    const std::string path = PathOf(material);
    return path.compare(0, 16, "/Engine/Transient") == 0 ? std::string() : path;
}
std::vector<std::string> MaterialsOf(Object* component) {
    std::vector<std::string> out;
    Call count(component, "GetNumMaterials");
    const int32_t n = count.Invoke() ? count.Get<int32_t>("ReturnValue") : 0;
    for (int32_t i = 0; i < n && i < 64; ++i) {
        Call get(component, "GetMaterial");
        out.push_back(MaterialPath(get.Set("ElementIndex", i).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr));
    }
    return out;
}
int32_t NumBones(Object* component) { Call get(component, "GetNumBones"); return get.Invoke() ? get.Get<int32_t>("ReturnValue") : 0; }
Name BoneName(Object* component, int32_t index) { Call get(component, "GetBoneName"); return get.Set("BoneIndex", index).Invoke() ? get.Get<Name>("ReturnValue") : Name{}; }
// The mesh's reference pose of each bone, local (both sides have the asset: a bone in its reference pose is sent as one
// flag, a translation near the reference one as 16-bit offsets). false: GetRefPoseTransform is not there (all is sent).
struct RefBone { Quat q; Vec3 t; Vec3 s{1, 1, 1}; };
bool RefPose(Object* component, int32_t bones, std::vector<RefBone>& out) {
    out.clear();
    Object* fn = ue::FindFunction(ue::ClassOf(component), "GetRefPoseTransform");
    if (!fn) return false;
    for (int32_t i = 0; i < bones; ++i) {
        Call get(component, fn); if (!get.Set("BoneIndex", i).Invoke()) { out.clear(); return false; }
        const Transform t = get.Get<Transform>("ReturnValue"); out.push_back({Unit(QuatOf(t)), MoveOf(t), ScaleOf(t)});
    }
    return true;
}
std::vector<Vec3> RefMoves(const std::vector<RefBone>& ref) { std::vector<Vec3> out; for (const RefBone& r : ref) out.push_back(r.t); return out; }

// ---- the hero's side: its parts and pose ---------------------------------------------------------------------------
// Every kScanMs the meshes the hero shows are listed: on it, on actors hanging on it (weapons, a look's parts), and on
// the actors shown with it that do not hang on it (NpcRetarget::ShownWithHero: a grab's boss body left on the spot and
// the target it holds; MotianGrab::HeldTarget: the running grab's target). Each keeps its id while it lives. A part
// hangs on its nearest ancestor that is a part (at the socket the attachment takes there), else on the root (the
// hero's root component). The list goes to the peer when it changes and every kPartsResendMs; the pose every kPoseMs:
// the hero's root, each part's place on what it hangs on and whether it shows, and the bones (local, parent-bone
// space) of the self-posing skinned parts something shown needs.
struct Source {
    Object* comp = nullptr; Name name{}; Object* asset = nullptr; PartDesc desc; Object* parent = nullptr, *leader = nullptr; Name socket{};
    std::vector<Name> bones; std::vector<RefBone> ref; bool ref_ok = false; Object* bones_of = nullptr; Object* socket_fn = nullptr; bool shown = false, needed = false;
    std::vector<int> parents; Object* bone_fn = nullptr; std::vector<Transform> worlds;  // parent bone indices; GetBoneTransform where there is one; scratch
};
std::vector<Source> sources_;
uint16_t next_id_ = 1;
uint32_t parts_serial_ = 0, pose_seq_ = 0, frames_sent_ = 0;
std::vector<unsigned char> parts_body_, parts_blob_;
ULONGLONG next_scan_ = 0, next_parts_send_ = 0, next_pose_ = 0, rate_at_ = 0;
uint64_t bytes_window_ = 0; double send_kbps_ = 0; size_t last_pose_bytes_ = 0;
size_t bones_sent_ = 0, send_bones_ = 0, bones_ref_ = 0, send_ref_ = 0;  // bones in the pose being built, in the last one (all, in the reference pose)
// Time spent building and showing poses (NET_PUPPET timing).
double build_ms_ = 0, show_ms_ = 0; uint32_t builds_ = 0, shows_ = 0; ULONGLONG next_timing_ = 0; int timings_ = 0;

int IndexOf(const std::vector<Source>& list, Object* comp) {
    for (size_t i = 0; i < list.size(); ++i) if (list[i].comp == comp) return static_cast<int>(i);
    return -1;
}
void ScanSources(Object* hero, ULONGLONG now) {
    if (now < next_scan_) return;
    next_scan_ = now + kScanMs;
    Object* static_class = Cls("/Script/Engine.StaticMeshComponent"), *skinned_class = Cls("/Script/Engine.SkinnedMeshComponent"),
          *groom_class = Cls("/Script/HairStrandsCore.GroomComponent"), *cloth_class = Cls("/Script/ChaosClothAssetEngine.ChaosClothComponent");
    std::vector<Object*> actors = AttachedActors(hero);
    actors.insert(actors.begin(), hero);
    std::vector<Object*> extra; NpcRetarget::ShownWithHero(extra);
    if (Object* held = MotianGrab::HeldTarget()) extra.push_back(held);
    for (Object* a : extra) {
        if (!ue::Alive(a) || std::find(actors.begin(), actors.end(), a) != actors.end()) continue;
        actors.push_back(a);
        for (Object* b : AttachedActors(a)) if (std::find(actors.begin(), actors.end(), b) == actors.end()) actors.push_back(b);
    }
    std::vector<Source> next;
    const auto add = [&](Object* c, int kind) {
        if (next.size() >= kMaxParts || IndexOf(next, c) >= 0) return;
        Object* asset = MeshAsset(c, kind); if (!asset) return;
        Source s; s.comp = c; s.name = ue::NameOf(c); s.asset = asset; s.desc.kind = static_cast<uint8_t>(kind);
        for (const Source& old : sources_) if (old.comp == c && old.name == s.name) {
            s.desc.id = old.desc.id; s.bones = old.bones; s.ref = old.ref; s.ref_ok = old.ref_ok; s.bones_of = old.bones_of; s.socket_fn = old.socket_fn; s.parents = old.parents; s.bone_fn = old.bone_fn; break; }
        if (!s.desc.id) { s.desc.id = next_id_++; if (next_id_ == kNone) next_id_ = 1; }
        next.push_back(std::move(s));
    };
    for (Object* actor : actors) {
        if (actor == puppet_) continue;
        for (Object* c : Components(actor, static_class)) add(c, 0);
        for (Object* c : Components(actor, skinned_class)) add(c, cloth_class && ue::IsA(c, cloth_class) ? 3 : 1);
        for (Object* c : Components(actor, groom_class)) add(c, 2);
    }
    // What each hangs on, and the pose it takes.
    for (Source& s : next) {
        Object* link = s.comp;
        for (int i = 0; i < 16 && link; ++i) {
            Object* up = Property(link, "AttachParent");
            if (!up) break;
            if (IndexOf(next, up) >= 0) { s.parent = up; const int32_t at = Offset(link, "AttachSocketName"); s.socket = at >= 0 ? ue::At<Name>(link, at) : Name{}; break; }
            link = up;
        }
        if (s.desc.kind == 1 || s.desc.kind == 3) {
            Object* lead = Weak(s.comp, "LeaderPoseComponent"); if (!lead) lead = Weak(s.comp, "MasterPoseComponent");
            if (lead && IndexOf(next, lead) >= 0 && lead != s.comp) s.leader = lead;
            else if (s.desc.kind == 3 && s.parent && skinned_class && ue::IsA(s.parent, skinned_class)) s.leader = s.parent;
        }
    }
    // A leader that follows another is followed down to the one that poses itself.
    for (Source& s : next)
        for (int i = 0; i < 8 && s.leader; ++i) { const int at = IndexOf(next, s.leader); if (at < 0 || !next[static_cast<size_t>(at)].leader) break; s.leader = next[static_cast<size_t>(at)].leader; }
    // Cloth needs its leader; a Groom a skinned part to hang on.
    next.erase(std::remove_if(next.begin(), next.end(), [&](const Source& s) {
        if (s.desc.kind == 3) return !s.leader;
        if (s.desc.kind == 2) { const int at = s.parent ? IndexOf(next, s.parent) : -1; return at < 0 || (next[static_cast<size_t>(at)].desc.kind != 1 && next[static_cast<size_t>(at)].desc.kind != 3); }
        return false;
    }), next.end());
    for (Source& s : next) {
        if (s.parent && IndexOf(next, s.parent) < 0) { s.parent = nullptr; s.socket = Name{}; }
        if (s.leader && IndexOf(next, s.leader) < 0) s.leader = nullptr;
        if (s.desc.kind == 3 && !s.leader) s.desc.kind = 0xff;
    }
    next.erase(std::remove_if(next.begin(), next.end(), [](const Source& s) { return s.desc.kind == 0xff; }), next.end());
    // Parents and leaders first.
    std::vector<Source> ordered; std::vector<bool> placed(next.size(), false);
    for (size_t pass = 0; pass <= next.size() && ordered.size() < next.size(); ++pass)
        for (size_t i = 0; i < next.size(); ++i) {
            if (placed[i]) continue;
            const int p = next[i].parent ? IndexOf(next, next[i].parent) : -1, l = next[i].leader ? IndexOf(next, next[i].leader) : -1;
            if ((p >= 0 && !placed[static_cast<size_t>(p)]) || (l >= 0 && !placed[static_cast<size_t>(l)])) continue;
            placed[i] = true; ordered.push_back(next[i]);
        }
    for (Source& s : ordered) {
        PartDesc& d = s.desc;
        const int p = s.parent ? IndexOf(ordered, s.parent) : -1, l = s.leader ? IndexOf(ordered, s.leader) : -1;
        d.parent = p >= 0 ? ordered[static_cast<size_t>(p)].desc.id : kNone; d.leader = l >= 0 ? ordered[static_cast<size_t>(l)].desc.id : kNone;
        if (p < 0) s.parent = nullptr;
        if (l < 0) s.leader = nullptr;
        d.socket.clear();  // a part hangs on its parent part's component frame: a socket can be overridden by the game (grabs) on the hero only
        d.asset = PathOf(s.asset);
        d.posed = d.kind == 1 && !s.leader ? 1 : 0;
        d.bones = d.posed ? static_cast<uint16_t>(std::clamp(NumBones(s.comp), 0, static_cast<int32_t>(kMaxBones))) : 0;
        if (d.kind != 2) d.materials = MaterialsOf(s.comp);
        if (d.kind == 2) {
            d.binding = PathOf(Property(s.comp, "BindingAsset")); d.physics = PathOf(Property(s.comp, "PhysicsAsset"));
            const int32_t main = Offset(s.comp, "bIsMainHair"), local = Offset(s.comp, "bForceLocalSpaceSimulation"), settings = Offset(s.comp, "SimulationSettings");
            d.main_hair = main >= 0 ? ue::At<uint8_t>(s.comp, main) : 0; d.local_sim = local >= 0 ? ue::At<uint8_t>(s.comp, local) : 0;
            if (settings >= 0) {
                d.settings.assign(static_cast<unsigned char*>(s.comp) + settings, static_cast<unsigned char*>(s.comp) + settings + kGroomSettingsBytes);
                std::memset(d.settings.data() + kLocalBoneString, 0, sizeof(ue::String));  // never a pointer of ours on the wire
            }
        }
    }
    sources_ = std::move(ordered);
    Writer body; body.Put(static_cast<uint16_t>(sources_.size())); for (const Source& s : sources_) WriteDesc(body, s.desc);
    if (body.b != parts_body_) {
        parts_body_ = body.b; ++parts_serial_;
        Writer blob; blob.Put(parts_serial_); blob.b.insert(blob.b.end(), parts_body_.begin(), parts_body_.end()); parts_blob_ = std::move(blob.b);
        next_parts_send_ = 0;
        size_t posed = 0, bones = 0; for (const Source& s : sources_) if (s.desc.posed) { ++posed; bones += s.desc.bones; }
        Logf("NET_PUPPET hero parts #%u: %zu from %zu actors (%zu pose themselves, %zu bones), %zu bytes", parts_serial_, sources_.size(), actors.size(), posed, bones, parts_blob_.size());
    }
}
// FRotator::Quaternion.
Quat QuatOfRotator(const Rotator& r) {
    const double k = kPi / 360.0, p = std::fmod(r.pitch, 360.0) * k, y = std::fmod(r.yaw, 360.0) * k, l = std::fmod(r.roll, 360.0) * k;
    const double sp = std::sin(p), cp = std::cos(p), sy = std::sin(y), cy = std::cos(y), sr = std::sin(l), cr = std::cos(l);
    return Unit({cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy, cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy});
}
// A bone's own world transform, by bone name only (never a socket of that name): TransformFromBoneSpace of the bone's
// origin (GetBoneQuaternion / GetBoneLocation are no UFUNCTIONs in PBZ), with the component's world scale (the bones
// themselves taken as unscaled).
bool BoneOwnWorld(Object* comp, Name bone, const Vec3& scale, Transform& out) {
    Call get(comp, "TransformFromBoneSpace");
    if (!get.Set("BoneName", bone).Set("InPosition", Vector{}).Set("InRotation", Rotator{}).Invoke()) return false;
    const Vector at = get.Get<Vector>("OutPosition");
    out = Make(QuatOfRotator(get.Get<Rotator>("OutRotation")), {at.x, at.y, at.z}, scale);
    return true;
}
// The bones of a self-posing part, local (each in its parent bone's space; the root in the component's).
// GetBoneTransform(InBoneName, TransformSpace) where the engine has it as a UFUNCTION: by bone name only.
Object* BoneTransformFn(Object* comp) {
    Object* fn = ue::FindFunction(ue::ClassOf(comp), "GetBoneTransform");
    if (!fn) return nullptr;
    ue::Field f[8]{}; const size_t n = ue::Fields(fn, f, 8); bool name = false, space = false, value = false;
    for (size_t i = 0; i < n; ++i) {
        name |= !std::strcmp(f[i].name, "InBoneName") && f[i].size == 8; space |= !std::strcmp(f[i].name, "TransformSpace") && f[i].size == 1;
        value |= !std::strcmp(f[i].name, "ReturnValue") && f[i].size == static_cast<int32_t>(sizeof(Transform));
    }
    return name && space && value ? fn : nullptr;
}
// A bone's world transform by its name only. GetSocketTransform is no use for bones: a socket of that name, or a socket
// override the game adds (during a grab weapon_l / WEAPON_R led to other sockets), comes first.
Transform BoneOnlyWorld(Object* comp, Object* bone_fn, Name bone, const Vec3& scale) {
    Transform t{};
    if (bone_fn) { Call get(comp, bone_fn); if (get.Set("InBoneName", bone).Set("TransformSpace", kSpaceWorld).Invoke()) t = get.Get<Transform>("ReturnValue"); }
    else BoneOwnWorld(comp, bone, scale, t);
    return t;
}
// The bones of a self-posing part, local (each in its parent bone's space; the root in the component's), made from the
// bones' final world transforms read by bone name only (GetBoneTransform, else TransformFromBoneSpace without scale).
// Never the engine's own parent-bone-space answer: the game moves some bones after the animation, in component space
// (during a grab weapon_l / WEAPON_R are snapped to a socket, scale 1.04), and that answer leaves this out.
void WriteBones(Writer& w, Source& s) {
    if (s.bones_of != s.asset) {
        s.bones.clear(); const int32_t n = std::clamp(NumBones(s.comp), 0, static_cast<int32_t>(kMaxBones));
        for (int32_t i = 0; i < n; ++i) s.bones.push_back(BoneName(s.comp, i));
        s.ref_ok = RefPose(s.comp, n, s.ref); s.bones_of = s.asset;
        s.parents.assign(s.bones.size(), -1);
        for (size_t i = 0; i < s.bones.size(); ++i) {
            Call parent(s.comp, "GetParentBone"); const Name up = parent.Set("BoneName", s.bones[i]).Invoke() ? parent.Get<Name>("ReturnValue") : Name{};
            if (!up.None()) for (size_t k = 0; k < i; ++k) if (s.bones[k] == up) { s.parents[i] = static_cast<int>(k); break; }
        }
        s.bone_fn = BoneTransformFn(s.comp);
        Logf("NET_PUPPET part %u: %zu bones read with %s", s.desc.id, s.bones.size(), s.bone_fn ? "GetBoneTransform" : "TransformFromBoneSpace");
    }
    Transform comp_world{}; WorldOf(s.comp, comp_world);
    s.worlds.resize(s.bones.size());
    for (size_t i = 0; i < s.bones.size(); ++i) s.worlds[i] = BoneOnlyWorld(s.comp, s.bone_fn, s.bones[i], ScaleOf(comp_world));
    w.Put(static_cast<uint16_t>(s.bones.size()));
    size_t run = 0;  // bones in the reference pose not written yet (one byte for up to 16)
    const auto flush = [&] { while (run) { const size_t n = (std::min)(run, static_cast<size_t>(16)); w.Put(static_cast<uint8_t>(kBoneRef | ((n - 1) << 4))); run -= n; } };
    for (size_t i = 0; i < s.bones.size(); ++i) {
        const int up = s.parents[i];
        Transform local = RelativeTo(s.worlds[i], up >= 0 ? s.worlds[static_cast<size_t>(up)] : comp_world);
        if (!s.bone_fn) local.scale[0] = local.scale[1] = local.scale[2] = 1;
        const Vec3 t = MoveOf(local), sc = ScaleOf(local);
        const bool has_ref = s.ref_ok && i < s.ref.size();
        const RefBone ref = has_ref ? s.ref[i] : RefBone{};
        const Vec3 d{t.x - ref.t.x, t.y - ref.t.y, t.z - ref.t.z};
        const Quat q = Unit(QuatOf(local));
        const double dot = std::fabs(q.x * ref.q.x + q.y * ref.q.y + q.z * ref.q.z + q.w * ref.q.w);
        if (has_ref && dot > 1 - 2e-8 && std::fabs(d.x) < 0.01 && std::fabs(d.y) < 0.01 && std::fabs(d.z) < 0.01 &&
            std::fabs(sc.x - ref.s.x) < 1e-4 && std::fabs(sc.y - ref.s.y) < 1e-4 && std::fabs(sc.z - ref.s.z) < 1e-4) { ++run; ++bones_ref_; continue; }
        flush();
        const bool kept = has_ref && std::fabs(d.x) <= 0.05 && std::fabs(d.y) <= 0.05 && std::fabs(d.z) <= 0.05;
        const bool offset_ok = has_ref && !kept && std::fabs(d.x) < 327 && std::fabs(d.y) < 327 && std::fabs(d.z) < 327;
        const bool scaled = !Unscaled(sc);
        w.Put(static_cast<uint8_t>((kept ? 0 : offset_ok ? kBoneNear : kBoneMoved) | (scaled ? kBoneScaled : 0)));
        w.Q(q);
        if (offset_ok) { w.Put(static_cast<int16_t>(std::lround(d.x * 100))); w.Put(static_cast<int16_t>(std::lround(d.y * 100))); w.Put(static_cast<int16_t>(std::lround(d.z * 100))); }
        else if (!kept) w.Move(t);
        if (scaled) w.Move(sc);
    }
    flush();
    bones_sent_ += s.bones.size();
}
ULONGLONG check_until_ = 0, next_check_ = 0;  // net check (below)
void Capture(uint32_t seq);
bool BuildPose(Object* hero, std::vector<unsigned char>& out) {
    Transform root{}; Object* root_comp = Property(hero, "RootComponent");
    if (!root_comp || !WorldOf(root_comp, root)) return false;
    for (Source& s : sources_) { s.shown = Same(s.comp, s.name) && SourceShown(s.comp); s.needed = false; }
    // What a shown part needs: its leader's bones and everything it hangs on.
    for (const Source& s : sources_) {
        if (!s.shown) continue;
        for (Object* up : {s.leader, s.parent}) {
            for (int guard = 0; up && guard < 16; ++guard) {
                const int at = IndexOf(sources_, up); if (at < 0) break;
                Source& u = sources_[static_cast<size_t>(at)]; u.needed = true; up = u.parent;
            }
        }
    }
    bones_sent_ = 0; bones_ref_ = 0;
    Writer w; w.Put(parts_serial_); w.Put(++pose_seq_);
    w.Put(root.translation[0]); w.Put(root.translation[1]); w.Put(root.translation[2]); w.Q(QuatOf(root));
    w.Put(static_cast<uint16_t>(sources_.size()));
    for (Source& s : sources_) {
        w.Put(s.desc.id);
        if (!Same(s.comp, s.name)) { w.Put(kGone); continue; }
        Transform world{}; WorldOf(s.comp, world);
        Transform parent_world = root; if (s.parent) WorldOf(s.parent, parent_world);
        const Transform rel = RelativeTo(world, parent_world);
        const bool bones = s.desc.posed && (s.shown || s.needed), scaled = !Unscaled(ScaleOf(rel));
        w.Put(static_cast<uint8_t>((s.shown ? kShown : 0) | (bones ? kBones : 0) | (scaled ? kScaled : 0)));
        w.Move(MoveOf(rel)); w.Q(QuatOf(rel)); if (scaled) w.Move(ScaleOf(rel));
        if (bones) WriteBones(w, s);
    }
    Capture(pose_seq_);
    out = std::move(w.b); send_bones_ = bones_sent_; send_ref_ = bones_ref_;
    return true;
}

// ---- the puppet's side: the parts shown and posed ------------------------------------------------------------------
// Each part a component on the puppet: a static mesh, a Groom, Chaos Cloth (simulating its own cloth), a skinned mesh
// in its leader's pose (leader pose), or a PoseableMeshComponent for a self-posing part, whose local bone transforms
// (UPoseableMeshComponent::BoneSpaceTransforms, found by its size between the SkinnedMeshComponent and the
// PoseableMeshComponent class sizes) are written every frame and refreshed through SetBoneTransformByName on its root
// bone (without that array: every bone through it, in component space). Poses are shown one send apart: blended from
// the one before to the newest by the time since it came (so a 30 a second pose moves smoothly), the root, each part's
// place and every bone likewise.
struct Bone { Quat q; Vec3 t; Vec3 s{1, 1, 1}; };
#pragma warning(push)
#pragma warning(disable: 4324)  // its transforms are 16-byte aligned
struct Part {
    PartDesc desc; Object* comp = nullptr; Name name{}; int32_t pose_at = -1; std::vector<Name> bones; std::vector<int> parents; std::vector<RefBone> ref; bool ref_ok = false;
    std::vector<Bone> prev, curr; bool have_bones = false; Transform rel_prev{}, rel_curr{}, rel_set{}; bool has_rel = false, rel_applied = false;
    int shown = -1, want_shown = 0; std::vector<std::string> materials_set;
};
#pragma warning(pop)
std::vector<Part> parts_;
uint32_t parts_in_serial_ = 0; bool parts_in_valid_ = false;
uint32_t pose_in_seq_ = 0; bool pose_in_valid_ = false; ULONGLONG pose_in_at_ = 0, pose_in_prev_at_ = 0;
Transform root_prev_{}, root_curr_{}; bool root_valid_ = false, root_placed_ = false;
int part_failures_ = 0, parts_shown_ = 0, bone_writes_logged_ = 0;
uint32_t poses_applied_ = 0, assets_missing_ = 0, parts_updates_ = 0;
// The loop (mirror on, no peer): the hero's own parts and pose go straight to its puppet, through the same encoding.
std::vector<unsigned char> loop_parts_, loop_pose_; bool loop_parts_new_ = false, loop_pose_new_ = false;

Part* FindPart(uint16_t id) { for (Part& p : parts_) if (p.desc.id == id) return &p; return nullptr; }
void DropPart(Part& p) {
    if (Same(p.comp, p.name)) Call(p.comp, "K2_DestroyComponent").Set("Object", p.comp).Invoke();
    p.comp = nullptr;
}
void ClearParts(const char* why) {
    if (!parts_.empty()) Logf("NET_PUPPET parts removed (%zu, %s)", parts_.size(), why);
    for (Part& p : parts_) DropPart(p);
    parts_.clear(); parts_in_valid_ = false; pose_in_valid_ = false; root_valid_ = false; root_placed_ = false; parts_shown_ = 0;
}
// UPoseableMeshComponent::BoneSpaceTransforms: the array of `bones` transforms in the class's own part (it starts a
// little before SkinnedMeshComponent's PropertiesSize: 0x908 with 2320 = 0x910 in PBZ), holding the reference pose
// when the mesh was just set: unit rotations and, where the reference pose is known, its translations (the component
// space arrays before it hold the same root but other translations).
int32_t FindBoneSpace(Object* comp, int32_t bones, const std::vector<Vec3>& ref) {
    Object* skinned = Cls("/Script/Engine.SkinnedMeshComponent"), *poseable = Cls("/Script/Engine.PoseableMeshComponent");
    const int32_t named = Offset(comp, "BoneSpaceTransforms");
    if (named >= 0 && ue::At<Array>(comp, named).num == bones) return named;
    const int32_t from = ue::StructSize(skinned), to = ue::StructSize(poseable);
    if (!from || to <= from || bones <= 0) return -1;
    for (int32_t at = (std::max)(from - 32, 0) & ~7; at + static_cast<int32_t>(sizeof(Array)) <= to; at += 8) {
        if (!Readable(comp + at, sizeof(Array))) continue;
        const Array a = ue::At<Array>(comp, at);
        if (a.num != bones || a.max < bones || !a.data || !Readable(a.data, static_cast<size_t>(bones) * sizeof(Transform))) continue;
        const Transform* t = static_cast<const Transform*>(a.data); bool ok = true;
        for (int32_t i = 0; i < (std::min)(bones, 8) && ok; ++i) {
            const double n = t[i].rotation[0] * t[i].rotation[0] + t[i].rotation[1] * t[i].rotation[1] + t[i].rotation[2] * t[i].rotation[2] + t[i].rotation[3] * t[i].rotation[3];
            ok = std::fabs(n - 1) < 1e-3 && t[i].scale[0] > 1e-3 && t[i].scale[0] < 1e3;
        }
        int same = 0, checked = 0;
        for (int32_t i = 1; ok && i < bones && static_cast<size_t>(i) < ref.size() && checked < 32; ++i, ++checked)
            same += std::fabs(t[i].translation[0] - ref[static_cast<size_t>(i)].x) < 0.01 && std::fabs(t[i].translation[1] - ref[static_cast<size_t>(i)].y) < 0.01 &&
                    std::fabs(t[i].translation[2] - ref[static_cast<size_t>(i)].z) < 0.01;
        if (ok && same * 10 >= checked * 9) return at;
    }
    return -1;
}
// Why it was not found (once per run): the class sizes and every array of that many items in the component.
bool bone_space_described_ = false;
void DescribeBoneSpace(Object* comp, int32_t bones) {
    if (bone_space_described_) return;
    bone_space_described_ = true;
    const int32_t from = ue::StructSize(Cls("/Script/Engine.SkinnedMeshComponent")), to = ue::StructSize(Cls("/Script/Engine.PoseableMeshComponent"));
    Logf("NET_PUPPET BoneSpaceTransforms search: %d bones, SkinnedMeshComponent %d bytes, PoseableMeshComponent %d bytes, named %d", bones, from, to, Offset(comp, "BoneSpaceTransforms"));
    int listed = 0;
    for (int32_t at = 0; at + static_cast<int32_t>(sizeof(Array)) <= (std::max)(to, 0x1800) && listed < 12; at += 8) {
        if (!Readable(comp + at, sizeof(Array))) continue;
        const Array a = ue::At<Array>(comp, at);
        if (a.num != bones || a.max < bones || !a.data) continue;
        ++listed;
        if (!Readable(a.data, 96)) { Logf("NET_PUPPET   0x%x: num %d max %d (data unreadable)", at, a.num, a.max); continue; }
        const double* d = static_cast<const double*>(a.data); const float* f = static_cast<const float*>(a.data);
        Logf("NET_PUPPET   0x%x: num %d max %d doubles %.3f %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f; floats %.3f %.3f %.3f %.3f", at, a.num, a.max, d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[8], f[0], f[1], f[2], f[3]);
    }
    if (!listed) Logf("NET_PUPPET   no array of %d items in the first 0x%x bytes", bones, (std::max)(to, 0x1800));
}
void SetupBones(Part& p) {
    const int32_t n = std::clamp(NumBones(p.comp), 0, static_cast<int32_t>(kMaxBones));
    p.bones.clear(); p.parents.clear();
    for (int32_t i = 0; i < n; ++i) p.bones.push_back(BoneName(p.comp, i));
    for (int32_t i = 0; i < n; ++i) {
        Call parent(p.comp, "GetParentBone"); Name up = parent.Set("BoneName", p.bones[static_cast<size_t>(i)]).Invoke() ? parent.Get<Name>("ReturnValue") : Name{};
        int at = -1; if (!up.None()) for (int32_t k = 0; k < i; ++k) if (p.bones[static_cast<size_t>(k)] == up) { at = k; break; }
        p.parents.push_back(at);
    }
    p.ref_ok = RefPose(p.comp, n, p.ref);
    p.pose_at = FindBoneSpace(p.comp, n, p.ref_ok ? RefMoves(p.ref) : std::vector<Vec3>{});
    if (p.pose_at < 0 && n > 8) DescribeBoneSpace(p.comp, n);
    Logf("NET_PUPPET part %u poses itself: %d bones (sent %u), reference pose %d, BoneSpaceTransforms at 0x%x%s", p.desc.id, n, p.desc.bones, p.ref_ok ? 1 : 0, p.pose_at,
         p.pose_at < 0 ? " (not found: every bone through SetBoneTransformByName)" : "");
}
void SetMaterials(Part& p) {
    for (size_t i = 0; i < p.desc.materials.size() && i < 64; ++i) {
        if (i < p.materials_set.size() && p.materials_set[i] == p.desc.materials[i]) continue;
        if (Object* m = FindAsset(p.desc.materials[i])) Call(p.comp, "SetMaterial").Set("ElementIndex", static_cast<int32_t>(i)).Set("Material", m).Invoke();
    }
    p.materials_set = p.desc.materials;
}
// The step CreatePart is in and the component it is making (a fault names the step and removes the half-made one).
int make_step_ = 0; Object* making_ = nullptr; Name making_name_{};
uintptr_t fault_at_ = 0; DWORD fault_code_ = 0;
bool CreatePart(const PartDesc& d) {
    make_step_ = 0; making_ = nullptr;
    const bool posed = d.kind == 1 && d.posed;
    const char* path = d.kind == 0 ? "/Script/Engine.StaticMeshComponent" : d.kind == 2 ? "/Script/HairStrandsCore.GroomComponent" :
                       d.kind == 3 ? "/Script/ChaosClothAssetEngine.ChaosClothComponent" : posed ? "/Script/Engine.PoseableMeshComponent" : "/Script/Engine.SkeletalMeshComponent";
    Object* cls = Cls(path), *root = Property(puppet_, "RootComponent");
    if (!cls || !root) return false;
    Object* asset = FindAsset(d.asset);
    if (!asset) { if (++assets_missing_ <= 10) Logf("NET_PUPPET part %u: asset %s not found", d.id, d.asset.c_str()); return false; }
    Part* parent = d.parent != kNone ? FindPart(d.parent) : nullptr, *leader = d.leader != kNone ? FindPart(d.leader) : nullptr;
    if ((d.parent != kNone && (!parent || !Same(parent->comp, parent->name))) || (d.leader != kNone && (!leader || !Same(leader->comp, leader->name)))) return false;
    if ((d.kind == 3 || (d.kind == 1 && !posed)) && !leader) return false;
    Object* parent_comp = parent ? parent->comp : root;
    Name socket{}; if (!d.socket.empty()) socket = ue::MakeName(Wide(d.socket).c_str());
    make_step_ = 1;
    Call add(puppet_, "AddComponentByClass");
    if (!add.Set("Class", cls).Set("bManualAttachment", true).Set("RelativeTransform", Transform{}).Set("bDeferredFinish", true).Invoke()) return false;
    Object* comp = add.Get<Object*>("ReturnValue");
    if (!comp || !ue::Alive(comp)) return false;
    making_ = comp; making_name_ = ue::NameOf(comp);
    make_step_ = 2;
    if (d.kind == 3) {
        Call set(comp, "SetClothAsset");
        if (!set.Set("InClothAsset", asset).Invoke()) { Call(comp, "K2_DestroyComponent").Set("Object", comp).Invoke(); making_ = nullptr; return false; }  // before it registers
    }
    if (d.kind == 2) {
        // Before it registers: the Groom asset, its binding, physics and simulation settings (as GroomForce copies them).
        const auto put = [&](const char* name, Object* value) { const int32_t at = Offset(comp, name); if (at >= 0) ue::At<Object*>(comp, at) = value; };
        put("GroomAsset", asset); put("BindingAsset", FindAsset(d.binding)); put("PhysicsAsset", FindAsset(d.physics));
        const int32_t main = Offset(comp, "bIsMainHair"), local = Offset(comp, "bForceLocalSpaceSimulation"), settings = Offset(comp, "SimulationSettings");
        if (main >= 0) ue::At<uint8_t>(comp, main) = d.main_hair;
        if (local >= 0) ue::At<uint8_t>(comp, local) = d.local_sim;
        if (settings >= 0 && d.settings.size() == kGroomSettingsBytes) {
            std::memcpy(static_cast<unsigned char*>(comp) + settings, d.settings.data(), kGroomSettingsBytes);
            std::memset(static_cast<unsigned char*>(comp) + settings + kLocalBoneString, 0, sizeof(ue::String));
        }
    }
    make_step_ = 3;
    Call(comp, "K2_AttachToComponent").Set("Parent", parent_comp).Set("SocketName", socket).Set("LocationRule", uint8_t{0}).Set("RotationRule", uint8_t{0})
        .Set("ScaleRule", uint8_t{0}).Set("bWeldSimulatedBodies", false).Invoke();
    make_step_ = 4;
    Call(puppet_, "FinishAddComponent").Set("Component", comp).Set("bManualAttachment", true).Set("RelativeTransform", Transform{}).Invoke();
    if (!ue::Alive(comp)) return false;
    Part p; p.desc = d; p.comp = comp; p.name = ue::NameOf(comp);
    if (d.kind != 2) {
        make_step_ = 5;
        Call(comp, "SetCollisionEnabled").Set("NewType", uint8_t{0}).Invoke();
        make_step_ = 6;
        bool set = true;
        if (d.kind == 0) set = Call(comp, "SetStaticMesh").Set("NewMesh", asset).Invoke();
        else if (d.kind == 1 && !posed) set = Call(comp, "SetSkeletalMeshAsset").Set("NewMesh", asset).Invoke();
        else if (posed) {
            Call update(comp, "SetSkinnedAssetAndUpdate");
            set = update.Set("NewMesh", asset).Set("bReinitPose", true).Invoke();
            if (!set) set = Call(comp, "SetSkeletalMesh").Set("NewMesh", asset).Set("bReinitPose", true).Invoke();
        }
        if (!set) { Call(comp, "K2_DestroyComponent").Set("Object", comp).Invoke(); making_ = nullptr; return false; }
        make_step_ = 7;
        SetMaterials(p);
        if (leader) {
            make_step_ = 8;
            Call(comp, "SetLeaderPoseComponent").Set("NewLeaderBoneComponent", leader->comp).Set("bForceUpdate", true).Set("bInFollowerShouldTickPose", false).Invoke();
            make_step_ = 9;
            Call(comp, "AddTickPrerequisiteComponent").Set("PrerequisiteComponent", leader->comp).Invoke();
        }
    }
    if (d.kind == 3) {  // a worn part always simulates, from the attached pose (as a look's cloth parts)
        make_step_ = 11;
        Call(comp, "SetEnableSimulation").Set("bEnable", true).Invoke();
        Call(comp, "ResumeSimulation").Invoke();
        Call(comp, "ForceNextUpdateTeleportAndReset").Invoke();
    }
    if (posed) { make_step_ = 12; SetupBones(p); }
    make_step_ = 10;
    parts_.push_back(std::move(p));
    making_ = nullptr;
    return true;
}
int CreateFaultFilter(const EXCEPTION_POINTERS* e) {
    fault_code_ = e->ExceptionRecord->ExceptionCode; fault_at_ = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
// The half-made component of a faulted step goes.
void DropMaking() {
    __try { if (making_ && Same(making_, making_name_)) Call(making_, "K2_DestroyComponent").Set("Object", making_).Invoke(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
    making_ = nullptr;
}
std::vector<std::string> refused_;  // assets whose part faulted: never tried again
// 1 made, 0 not (yet), -1 it faulted.
int CreatePartGuarded(const PartDesc& d) {
    __try { return CreatePart(d) ? 1 : 0; }
    __except (CreateFaultFilter(GetExceptionInformation())) { DropMaking(); return -1; }
}
// A parts list: parts gone or changed in shape go (with the ones hanging on them or following them), new ones are made
// in the list's order (what they hang on first), materials follow.
void TakeParts(const std::vector<unsigned char>& blob) {
    Reader r{blob.data(), blob.size()};
    const uint32_t serial = r.Get<uint32_t>(); const uint16_t n = r.Get<uint16_t>();
    std::vector<PartDesc> list;
    for (uint16_t i = 0; i < n && !r.bad && i < kMaxParts; ++i) list.push_back(ReadDesc(r));
    if (r.bad) { Logf("NET_PUPPET parts list #%u unreadable (%zu bytes)", serial, blob.size()); return; }
    if (parts_in_valid_ && serial == parts_in_serial_) return;  // sent again
    const auto desc_of = [&](uint16_t id) -> const PartDesc* { for (const PartDesc& d : list) if (d.id == id) return &d; return nullptr; };
    std::vector<uint16_t> drop;
    for (const Part& p : parts_) { const PartDesc* d = desc_of(p.desc.id); if (!d || !SameShape(*d, p.desc) || !Same(p.comp, p.name)) drop.push_back(p.desc.id); }
    for (bool more = true; more;) {
        more = false;
        for (const Part& p : parts_) {
            if (std::find(drop.begin(), drop.end(), p.desc.id) != drop.end()) continue;
            if (std::find(drop.begin(), drop.end(), p.desc.parent) != drop.end() || std::find(drop.begin(), drop.end(), p.desc.leader) != drop.end()) { drop.push_back(p.desc.id); more = true; }
        }
    }
    for (Part& p : parts_) if (std::find(drop.begin(), drop.end(), p.desc.id) != drop.end()) DropPart(p);
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [](const Part& p) { return !p.comp; }), parts_.end());
    size_t made = 0, failed = 0;
    for (const PartDesc& d : list) {
        if (Part* p = FindPart(d.id)) { p->desc = d; SetMaterials(*p); continue; }
        if (std::find(refused_.begin(), refused_.end(), d.asset) != refused_.end()) continue;
        _snprintf_s(stage_part_, sizeof(stage_part_), _TRUNCATE, "part %u %s %s", d.id, d.kind == 0 ? "static" : d.kind == 1 ? (d.posed ? "posed" : "skinned") : d.kind == 2 ? "groom" : "cloth", d.asset.c_str());
        stage_ = "part";
        const int result = CreatePartGuarded(d);
        stage_ = "parts";
        if (result > 0) ++made;
        else {
            ++failed;
            if (result < 0) {
                refused_.push_back(d.asset);
                const uintptr_t base = ue::Base();
                Logf("NET_PUPPET %s faulted at step %d code=0x%08lx at PBZ+0x%llx; that part is left out", stage_part_, make_step_, fault_code_,
                     static_cast<unsigned long long>(fault_at_ >= base ? fault_at_ - base : fault_at_));
            }
        }
    }
    part_failures_ += static_cast<int>(failed);
    parts_in_serial_ = serial; parts_in_valid_ = true; ++parts_updates_;
    Logf("NET_PUPPET parts list #%u: %zu parts, -%zu +%zu (failed %zu)", serial, list.size(), drop.size(), made, failed);
}
// A pose: newer than the last one taken (by its sequence number), it becomes the newest and the one before it the
// one blended from.
void TakePose(const std::vector<unsigned char>& blob, ULONGLONG received) {
    Reader r{blob.data(), blob.size()};
    r.Get<uint32_t>(); const uint32_t seq = r.Get<uint32_t>();
    if (r.bad || (pose_in_valid_ && static_cast<int32_t>(seq - pose_in_seq_) <= 0)) return;
    Transform root{}; root.translation[0] = r.Get<double>(); root.translation[1] = r.Get<double>(); root.translation[2] = r.Get<double>();
    { const Quat q = r.Q(); root.rotation[0] = q.x; root.rotation[1] = q.y; root.rotation[2] = q.z; root.rotation[3] = q.w; }
    const uint16_t n = r.Get<uint16_t>();
    for (uint16_t i = 0; i < n && !r.bad; ++i) {
        const uint16_t id = r.Get<uint16_t>(); const uint8_t flags = r.Get<uint8_t>();
        if (flags & kGone) continue;
        const Vec3 t = r.Move(); const Quat q = r.Q(); const Vec3 s = (flags & kScaled) ? r.Move() : Vec3{1, 1, 1};
        Part* p = FindPart(id);
        if (p) { const Transform rel = Make(q, t, s); p->rel_prev = p->has_rel ? p->rel_curr : rel; p->rel_curr = rel; p->has_rel = true; p->want_shown = (flags & kShown) ? 1 : 0; }
        if (!(flags & kBones)) continue;
        const uint16_t count = r.Get<uint16_t>();
        const bool keep = p && p->desc.posed && count == p->bones.size();
        std::vector<Bone> bones; if (keep) bones.resize(count);
        for (uint16_t b = 0; b < count && !r.bad; ++b) {
            const uint8_t bf = r.Get<uint8_t>(); Bone bone;
            const RefBone ref = keep && p->ref_ok && b < p->ref.size() ? p->ref[b] : RefBone{};
            if (bf & kBoneRef) {
                const uint16_t same = static_cast<uint16_t>((bf >> 4) + 1);
                for (uint16_t k = 0; k < same && b + k < count; ++k) {
                    const RefBone rk = keep && p->ref_ok && b + k < p->ref.size() ? p->ref[b + k] : RefBone{};
                    if (keep) { bones[b + k].q = rk.q; bones[b + k].t = rk.t; bones[b + k].s = rk.s; }
                }
                b = static_cast<uint16_t>(b + same - 1); continue;
            }
            bone.q = r.Q();
            if (bf & kBoneMoved) bone.t = r.Move();
            else if (bf & kBoneNear) { const int16_t x = r.Get<int16_t>(), y = r.Get<int16_t>(), z = r.Get<int16_t>(); bone.t = {ref.t.x + x * 0.01, ref.t.y + y * 0.01, ref.t.z + z * 0.01}; }
            else bone.t = ref.t;
            if (bf & kBoneScaled) bone.s = r.Move();
            if (keep) bones[b] = bone;
        }
        if (keep && !r.bad) { p->prev = p->have_bones ? p->curr : bones; p->curr = std::move(bones); p->have_bones = true; }
    }
    if (r.bad) return;
    root_prev_ = root_valid_ ? root_curr_ : root; root_curr_ = root; root_valid_ = true;
    pose_in_seq_ = seq; pose_in_valid_ = true; pose_in_prev_at_ = pose_in_at_ ? pose_in_at_ : received - kPoseMs; pose_in_at_ = received;
}
// The newest pose blended in: the root (beside the hero in the loop), every part's place and shown state, the bones.
void ShowPose(ULONGLONG now, bool loop) {
    if (!root_valid_) return;
    const double span = static_cast<double>((std::max<ULONGLONG>)(pose_in_at_ - pose_in_prev_at_, 10));
    const double k = check_until_ ? 1.0 : std::clamp(static_cast<double>(now - pose_in_at_) / span, 0.0, 1.0);
    Transform root = Blend(root_prev_, root_curr_, k);
    if (loop) { root.translation[0] += offset_.x; root.translation[1] += offset_.y; root.translation[2] += offset_.z; }
    if (Object* own = Property(puppet_, "RootComponent")) {
        Transform at{}; const bool jump = !root_placed_ || (WorldOf(own, at) && std::hypot(std::hypot(at.translation[0] - root.translation[0], at.translation[1] - root.translation[1]), at.translation[2] - root.translation[2]) > kSnapCm);
        Call(own, "K2_SetWorldTransform").Set("NewTransform", root).Set("bSweep", false).Set("bTeleport", jump).Invoke();
        root_placed_ = true;
    }
    int shown = 0;
    std::vector<Transform> locals, spaces;
    for (Part& p : parts_) {
        if (!Same(p.comp, p.name)) continue;
        if (p.want_shown != p.shown) { Call(p.comp, "SetHiddenInGame").Set("NewHidden", p.want_shown == 0).Set("bPropagateToChildren", false).Invoke(); p.shown = p.want_shown; }
        shown += p.shown > 0 ? 1 : 0;
        if (p.has_rel) {
            const Transform rel = Blend(p.rel_prev, p.rel_curr, k);
            if (!p.rel_applied || !Close(rel, p.rel_set)) { Call(p.comp, "K2_SetRelativeTransform").Set("NewTransform", rel).Set("bSweep", false).Set("bTeleport", false).Invoke(); p.rel_set = rel; p.rel_applied = true; }
        }
        if (!p.desc.posed || !p.have_bones || p.curr.size() != p.bones.size() || p.prev.size() != p.curr.size() || p.bones.empty()) continue;
        const size_t n = p.curr.size();
        locals.resize(n);
        for (size_t i = 0; i < n; ++i) {
            const Bone& a = p.prev[i]; const Bone& b = p.curr[i];
            locals[i] = Make(Nlerp(a.q, b.q, k), Lerp(a.t, b.t, k), Lerp(a.s, b.s, k));
        }
        bool written = false;
        if (p.pose_at >= 0 && Readable(p.comp + p.pose_at, sizeof(Array))) {
            const Array a = ue::At<Array>(p.comp, p.pose_at);
            if (a.num == static_cast<int32_t>(n) && a.data && Readable(a.data, n * sizeof(Transform))) {
                std::memcpy(a.data, locals.data(), n * sizeof(Transform));
                Call(p.comp, "SetBoneTransformByName").Set("BoneName", p.bones[0]).Set("InTransform", locals[0]).Set("BoneSpace", kBoneComponentSpace).Invoke();
                written = true;
            }
        }
        if (!written) {  // every bone in component space, parents first
            spaces.resize(n);
            for (size_t i = 0; i < n; ++i) {
                const int up = i < p.parents.size() ? p.parents[i] : -1;
                spaces[i] = up >= 0 && static_cast<size_t>(up) < i ? Compose(locals[i], spaces[static_cast<size_t>(up)]) : locals[i];
                Call(p.comp, "SetBoneTransformByName").Set("BoneName", p.bones[i]).Set("InTransform", spaces[i]).Set("BoneSpace", kBoneComponentSpace).Invoke();
            }
        }
        if (bone_writes_logged_ < 3) { ++bone_writes_logged_; Logf("NET_PUPPET part %u pose written: %zu bones (%s)", p.desc.id, n, written ? "BoneSpaceTransforms" : "SetBoneTransformByName per bone"); }
    }
    parts_shown_ = shown; ++poses_applied_;
}

// net check (tests, the loop): while it runs the newest pose is shown unblended. When a check is due, the hero's part
// places and posed bones (world) are captured as the pose is built; on the next frame, before anything new is applied,
// the puppet (less the loop's offset) is compared with that capture. (A skinned component's bones written this frame
// are read back only after the engine flips its bone buffers at the end of the frame: comparing in the same frame
// shows the pose before.)
#pragma warning(push)
#pragma warning(disable: 4324)
struct Captured { uint16_t id = 0; bool shown = false; Transform world{}; std::vector<Transform> bones; };
#pragma warning(pop)
std::vector<Captured> capture_next_, capture_shown_; uint32_t capture_next_seq_ = 0; bool capture_ready_ = false;
double check_bone_max_ = 0, check_part_max_ = 0, check_sum_ = 0; uint64_t check_bones_ = 0; int check_rounds_ = 0, check_shown_bad_ = 0; char check_worst_[200] = "-";
double Gap(const Transform& a, const Transform& b, bool offset) {
    const double dx = a.translation[0] - (offset ? offset_.x : 0) - b.translation[0], dy = a.translation[1] - (offset ? offset_.y : 0) - b.translation[1],
                 dz = a.translation[2] - (offset ? offset_.z : 0) - b.translation[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}
// Both sides read bones by name only (BoneOnlyWorld): the hero's sockets can be overridden by the game.
Transform BoneWorld(Object* comp, Object* fn, Name bone) { Transform w{}; WorldOf(comp, w); return BoneOnlyWorld(comp, fn, bone, ScaleOf(w)); }
// BuildPose, while a check is due: the hero as this pose shows it.
void Capture(uint32_t seq) {
    if (!check_until_ || GetTickCount64() < next_check_) return;
    capture_next_.clear();
    for (const Source& s : sources_) {
        if (!Same(s.comp, s.name)) continue;
        Captured c; c.id = s.desc.id; c.shown = s.shown; WorldOf(s.comp, c.world);
        if (s.desc.posed && (s.shown || s.needed)) for (Name bone : s.bones) c.bones.push_back(BoneWorld(s.comp, s.bone_fn, bone));
        capture_next_.push_back(std::move(c));
    }
    capture_next_seq_ = seq;
}
void CheckLoop(ULONGLONG now) {
    if (!check_until_) return;
    if (now >= check_until_) {
        Logf("NET_PUPPET check done: %d rounds, %llu bones, largest %.2f cm (%s), mean %.3f cm, part places largest %.2f cm, shown mismatches %d", check_rounds_,
             static_cast<unsigned long long>(check_bones_), check_bone_max_, check_worst_, check_bones_ ? check_sum_ / static_cast<double>(check_bones_) : 0.0, check_part_max_, check_shown_bad_);
        check_until_ = 0; capture_ready_ = false; return;
    }
    if (!capture_ready_) return;
    capture_ready_ = false; next_check_ = now + 500;
    double bone_max = 0, part_max = 0, sum = 0; size_t bones = 0, compared = 0; int shown_bad = 0; char worst[200] = "-"; uint16_t worst_place = 0;
    for (const Part& p : parts_) {
        if (!Same(p.comp, p.name)) continue;
        const Captured* c = nullptr; for (const Captured& q : capture_shown_) if (q.id == p.desc.id) { c = &q; break; }
        if (!c) continue;
        ++compared;
        if ((p.shown > 0) != c->shown) ++shown_bad;
        Transform a{}; WorldOf(p.comp, a);
        if (Gap(a, c->world, true) > part_max) { part_max = Gap(a, c->world, true); worst_place = p.desc.id; }
        if (!p.desc.posed || c->bones.size() != p.bones.size()) continue;
        Object* fn = BoneTransformFn(p.comp);
        for (size_t i = 0; i < p.bones.size(); ++i) {
            const double d = Gap(BoneWorld(p.comp, fn, p.bones[i]), c->bones[i], true);
            sum += d; ++bones;
            if (d > bone_max) {
                bone_max = d; char bone[64]{}, up[64] = "-"; ue::NameText(p.bones[i], bone, sizeof(bone));
                const int pi = i < p.parents.size() ? p.parents[i] : -1; double pd = -1;
                if (pi >= 0) { ue::NameText(p.bones[static_cast<size_t>(pi)], up, sizeof(up)); pd = Gap(BoneWorld(p.comp, fn, p.bones[static_cast<size_t>(pi)]), c->bones[static_cast<size_t>(pi)], true); }
                _snprintf_s(worst, sizeof(worst), _TRUNCATE, "part %u bone %s, its parent %s %.2f cm", p.desc.id, bone, up, pd);
                if (d > 5 && pi >= 0 && pd < 1 && i < p.curr.size()) {  // the bone itself: the puppet's local, the one received, the hero's (from the capture)
                    Transform mine{}; Call get(p.comp, "GetSocketTransform");
                    if (get.Set("InSocketName", p.bones[i]).Set("TransformSpace", kSpaceParentBone).Invoke()) mine = get.Get<Transform>("ReturnValue");
                    const Transform hero = RelativeTo(c->bones[i], c->bones[static_cast<size_t>(pi)]); const Bone& got = p.curr[i];
                    Logf("NET_PUPPET check   %s local: puppet t(%.2f %.2f %.2f) q(%.3f %.3f %.3f %.3f) s%.2f; received t(%.2f %.2f %.2f) q(%.3f %.3f %.3f %.3f) s%.2f; hero t(%.2f %.2f %.2f) q(%.3f %.3f %.3f %.3f) s%.2f",
                         bone, mine.translation[0], mine.translation[1], mine.translation[2], mine.rotation[0], mine.rotation[1], mine.rotation[2], mine.rotation[3], mine.scale[0],
                         got.t.x, got.t.y, got.t.z, got.q.x, got.q.y, got.q.z, got.q.w, got.s.x,
                         hero.translation[0], hero.translation[1], hero.translation[2], hero.rotation[0], hero.rotation[1], hero.rotation[2], hero.rotation[3], hero.scale[0]);
                }
            }
        }
    }
    ++check_rounds_; check_bones_ += bones; check_sum_ += sum; check_shown_bad_ += shown_bad; check_part_max_ = (std::max)(check_part_max_, part_max);
    if (bone_max > check_bone_max_) { check_bone_max_ = bone_max; strcpy_s(check_worst_, worst); }
    Logf("NET_PUPPET check: %zu parts, %zu bones, largest %.2f cm (%s), mean %.3f cm, part places largest %.2f cm (part %u), shown mismatches %d",
         compared, bones, bone_max, worst, bones ? sum / static_cast<double>(bones) : 0.0, part_max, worst_place, shown_bad);
}

// ---- the puppet ------------------------------------------------------------------------------------------------------
Transform At(const Vector& where, double yaw_degrees) {
    Transform t; t.translation[0] = where.x; t.translation[1] = where.y; t.translation[2] = where.z;
    t.rotation[2] = std::sin(yaw_degrees * kPi / 360.0); t.rotation[3] = std::cos(yaw_degrees * kPi / 360.0);
    return t;
}
int CountSpecs(Object* asc) {
    Object* container = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t specs = Offset(asc, "ActivatableAbilities"), items = container ? ue::Offset(container, "Items") : -1;
    if (!asc || specs < 0 || items < 0) return -1;
    return ue::At<Array>(asc, specs + items).num;
}
void ResetCounts() { poses_applied_ = assets_missing_ = parts_updates_ = frames_sent_ = 0; part_failures_ = 0; bone_writes_logged_ = 0; send_kbps_ = 0; refused_.clear(); }
// The puppet's own meshes (a plain Character's empty body): hidden, only the parts show.
void HideOwn() {
    for (const char* path : {"/Script/Engine.MeshComponent"})
        for (Object* c : Components(puppet_, Cls(path))) {
            if (std::any_of(parts_.begin(), parts_.end(), [&](const Part& p) { return p.comp == c; })) continue;
            Call(c, "SetHiddenInGame").Set("NewHidden", true).Set("bPropagateToChildren", false).Invoke();
        }
}
bool Spawn(const Context& c, ULONGLONG now, const char* why) {
    if (PuppetAlive()) { Note(L"替身已经在场", L"The puppet is already here"); return true; }
    // A plain Character, never a second hero: the hero's blueprint (BP_Renewal_Hun) took the player's input from its
    // BeginPlay (the hero could not attack and stayed in its guard: the release went to the puppet, 11-08).
    Object* cls = Cls("/Script/Engine.Character"), *statics = ue::Default("/Script/Engine.Default__GameplayStatics");
    Vector hero{};
    if (!cls || !statics || !Location(c.pawn, hero)) { Logf("NET_PUPPET spawn refused: hero not readable"); return false; }
    const double yaw = Yaw(c.pawn), r = yaw * kPi / 180.0;
    const Vector where{hero.x - std::sin(r) * kBesideCm, hero.y + std::cos(r) * kBesideCm, hero.z};
    const Transform t = At(where, yaw);
    Call begin(statics, "BeginDeferredActorSpawnFromClass");
    // 2: AdjustIfPossibleButAlwaysSpawn.
    if (!begin.Set("WorldContextObject", c.pawn).Set("ActorClass", cls).Set("SpawnTransform", t).Set("CollisionHandlingOverride", uint8_t{2}).Invoke()) {
        Logf("NET_PUPPET BeginDeferredActorSpawnFromClass failed"); return false;
    }
    Object* actor = begin.Get<Object*>("ReturnValue");
    if (!actor || !ue::Alive(actor)) { Logf("NET_PUPPET spawn returned no actor (class %s)", Text(cls).c_str()); Note(L"替身生成失败（见日志）", L"Puppet spawn failed (see the log)"); return false; }
    // Before BeginPlay: the local player must never be handed this pawn.
    const int32_t player_at = Offset(actor, "AutoPossessPlayer"), ai_at = Offset(actor, "AutoPossessAI");
    const int player_before = player_at >= 0 ? ue::At<uint8_t>(actor, player_at) : -1;
    if (player_at >= 0) ue::At<uint8_t>(actor, player_at) = 0;
    const int ai_mode = ai_at >= 0 ? ue::At<uint8_t>(actor, ai_at) : -1;
    if (!Call(statics, "FinishSpawningActor").Set("Actor", actor).Set("SpawnTransform", t).Invoke() || !ue::Alive(actor)) {
        Logf("NET_PUPPET FinishSpawningActor failed"); Note(L"替身生成失败（见日志）", L"Puppet spawn failed (see the log)"); return false;
    }
    puppet_ = actor; puppet_name_ = ue::NameOf(actor); hero_ = c.pawn; hero_name_ = ue::NameOf(c.pawn);
    // No collision with the hero, the enemies or their attacks, and no movement simulation (no gravity, no root motion):
    // it only shows the other player, placed every frame.
    Call(actor, "SetActorEnableCollision").Set("bNewActorEnableCollision", false).Invoke();
    Call(Property(actor, "CharacterMovement"), "SetComponentTickEnabled").Set("bEnabled", false).Invoke();
    Vector placed{};
    offset_ = Location(actor, placed) ? Vector{placed.x - hero.x, placed.y - hero.y, placed.z - hero.z} : Vector{where.x - hero.x, where.y - hero.y, 0};
    spawned_at_ = now; reports_ = 0; timings_ = 0; next_timing_ = 0; next_camera_ = now; next_scan_ = 0; next_parts_send_ = 0; auto_spawn_ = false;
    parts_.clear(); parts_in_valid_ = false; pose_in_valid_ = false; root_valid_ = false; root_placed_ = false; source_ = 0;
    cls_name_ = Text(cls); ResetCounts(); HideOwn();
    Logf("NET_PUPPET spawned %s (%s) beside %s why=%s AutoPossessPlayer %d->0 AutoPossessAI=%d offset=(%.0f %.0f %.0f)",
         Text(actor).c_str(), cls_name_.c_str(), Text(c.pawn).c_str(), why, player_before, ai_mode, offset_.x, offset_.y, offset_.z);
    Note(L"替身已生成", L"Puppet spawned");
    return true;
}
void Remove(const char* why) {
    if (PuppetAlive()) {
        ClearParts(why);
        Call(puppet_, "K2_DestroyActor").Invoke();
        Logf("NET_PUPPET removed (%s) poses=%u parts lists=%u missing assets=%u", why, poses_applied_, parts_updates_, assets_missing_);
    }
    parts_.clear(); parts_in_valid_ = false; pose_in_valid_ = false; root_valid_ = false; root_placed_ = false;
    puppet_ = nullptr; puppet_name_ = {}; controller_name_.clear(); source_ = 0;
}
// Controller and ability system at 0, 1 and 5 s after the spawn (for the log).
void Report(const Context& c, ULONGLONG now) {
    if (reports_ >= std::size(kReportsMs) || now - spawned_at_ < kReportsMs[reports_]) return;
    ++reports_;
    Object* controller = Property(puppet_, "Controller");
    controller_name_ = controller ? ClassName(controller) : "-";
    Call view(c.controller, "GetViewTarget");
    Object* target = view.Invoke() ? view.Get<Object*>("ReturnValue") : nullptr;
    Logf("NET_PUPPET report t=%llums controller=%s specs=%d hero_specs=%d view_target=%s player_pawn=%s parts=%zu shown=%d failed=%d poses=%u",
         static_cast<unsigned long long>(now - spawned_at_), controller_name_.c_str(), CountSpecs(Asc(puppet_)), CountSpecs(Asc(c.pawn)),
         Text(target).c_str(), Text(c.pawn).c_str(), parts_.size(), parts_shown_, part_failures_, poses_applied_);
}
// The player's camera stays on the hero.
void Guard(const Context& c, ULONGLONG now) {
    if (now - spawned_at_ > kCameraGuardMs && now < next_camera_) return;
    next_camera_ = now + 500;
    Call view(c.controller, "GetViewTarget");
    if (view.Invoke() && view.Get<Object*>("ReturnValue") == puppet_) {
        Call(c.controller, "SetViewTargetWithBlend").Set("NewViewTarget", c.pawn).Invoke();
        if (++camera_fixes_ <= 5) Logf("NET_PUPPET the camera went to the puppet; put back on the hero");
    }
}
// Time spent building and showing poses, logged every 10 s (the first six times after a spawn).
double Ms() { LARGE_INTEGER c, f; QueryPerformanceCounter(&c); QueryPerformanceFrequency(&f); return static_cast<double>(c.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart); }
void Timing(ULONGLONG now) {
    if (now < next_timing_) return;
    if (next_timing_ && timings_ < 6 && (builds_ || shows_)) {
        ++timings_;
        Logf("NET_PUPPET timing: build %.3f ms x%u, show %.3f ms x%u (per call), %.0f KB/s, %zu bones per pose (%zu in the reference pose)", builds_ ? build_ms_ / builds_ : 0.0, builds_, shows_ ? show_ms_ / shows_ : 0.0, shows_, send_kbps_, send_bones_, send_ref_);
    }
    build_ms_ = show_ms_ = 0; builds_ = shows_ = 0; next_timing_ = now + 10000;
}
// The hero's side (to the peer, or to its own puppet in the loop): its parts when they change (and every few seconds
// to the peer), its pose 30 times a second.
void Stream(Object* hero, ULONGLONG now, bool peer, bool loop) {
    stage_ = "scan"; ScanSources(hero, now);
    if (peer && now >= next_parts_send_ && !parts_blob_.empty()) { if (NetSteam::SendParts(parts_blob_)) bytes_window_ += parts_blob_.size(); next_parts_send_ = now + kPartsResendMs; }
    if (loop && parts_blob_ != loop_parts_) { loop_parts_ = parts_blob_; loop_parts_new_ = true; }
    if (now < next_pose_) return;
    next_pose_ = now - next_pose_ < kPoseMs ? next_pose_ + kPoseMs : now + kPoseMs;  // 30 a second on average, whatever the frame rate
    const ULONGLONG paced = static_cast<ULONGLONG>(static_cast<double>(last_pose_bytes_) * 1000.0 / kPoseBudget);
    if (paced > kPoseMs) next_pose_ += paced - kPoseMs;  // a big pose: the next one later
    stage_ = "pose";
    std::vector<unsigned char> pose;
    const double t0 = Ms();
    if (!BuildPose(hero, pose)) return;
    build_ms_ += Ms() - t0; ++builds_; last_pose_bytes_ = pose.size();
    ++frames_sent_;
    if (peer && NetSteam::SendPose(pose)) bytes_window_ += pose.size();
    if (loop) { loop_pose_ = std::move(pose); loop_pose_new_ = true; bytes_window_ += loop_pose_.size(); }
    if (now >= rate_at_ + kRateMs) {
        send_kbps_ = rate_at_ ? static_cast<double>(bytes_window_) / 1024.0 / (static_cast<double>(now - rate_at_) / 1000.0) : 0;
        bytes_window_ = 0; rate_at_ = now;
    }
}
void SendState(const Context& c, ULONGLONG now) {
    if (now < next_state_) return;
    next_state_ = now + kStateMs;
    NetSteam::State s; Vector at{};
    if (!Location(c.pawn, at)) return;
    const Vector v = Velocity(c.pawn);
    s.pos[0] = at.x; s.pos[1] = at.y; s.pos[2] = at.z; s.vel[0] = v.x; s.vel[1] = v.y; s.vel[2] = v.z; s.yaw = Yaw(c.pawn);
    s.dilation = Dilation(c.pawn);
    strncpy_s(s.map, local_map_.c_str(), _TRUNCATE);
    NetSteam::SendState(s);
}
// The puppet's side: the peer's parts and pose (or its own hero's, in the loop), shown.
void Show(ULONGLONG now, bool peer, bool loop) {
    std::vector<unsigned char> blob; uint64_t at = 0;
    if (peer) {
        NetSteam::State s; uint64_t when = 0;
        if (NetSteam::LatestState(s, when) && local_map_ != s.map) {
            const std::wstring map = Wide(s.map);
            note_[0] = L"对方在另一张地图：" + map; note_[1] = L"The peer is on another map: " + map;
            for (Part& p : parts_) if (Same(p.comp, p.name) && p.shown != 0) { Call(p.comp, "SetHiddenInGame").Set("NewHidden", true).Set("bPropagateToChildren", false).Invoke(); p.shown = 0; }
            return;
        }
        if (!note_[0].empty() && note_[0].rfind(L"对方在另一张地图", 0) == 0) { note_[0].clear(); note_[1].clear(); }
        stage_ = "parts"; if (NetSteam::TakeParts(blob)) TakeParts(blob);
        stage_ = "pose in"; if (NetSteam::TakePose(blob, at)) TakePose(blob, at);
    } else if (loop) {
        stage_ = "parts"; if (loop_parts_new_) { loop_parts_new_ = false; TakeParts(loop_parts_); }
        if (check_until_) { stage_ = "check"; CheckLoop(now); }  // the pose applied last frame, before anything new
        stage_ = "pose in";
        const bool fresh = loop_pose_new_;
        if (loop_pose_new_) { loop_pose_new_ = false; TakePose(loop_pose_, now); }
        if (fresh && check_until_ && capture_next_seq_ == pose_in_seq_ && !capture_next_.empty()) { capture_shown_ = std::move(capture_next_); capture_next_.clear(); capture_ready_ = true; }
    }
    stage_ = "show"; const double t0 = Ms(); ShowPose(now, loop && !peer); show_ms_ += Ms() - t0; ++shows_;
}
void Drive(const Context& c, ULONGLONG now, bool peer) {
    const int mode = peer ? 2 : mirror_ ? 1 : 0;
    if (mode != source_) {
        stage_ = "mode";
        ClearParts(mode == 2 ? "following the peer" : mode == 1 ? "showing my own hero" : "mirror off");
        loop_parts_.clear(); loop_parts_new_ = false; loop_pose_new_ = false; source_ = mode;
    }
    if (mode == 1) Stream(c.pawn, now, false, true);
    if (mode != 0) Show(now, peer, mode == 1);
}
void Publish(ULONGLONG now) {
    if (now < next_publish_) return;
    next_publish_ = now + kPublishMs;
    Snapshot s;
    s.alive = PuppetAlive(); s.mirror = mirror_; s.source = s.alive ? source_ : 0; s.cls = cls_name_; s.controller = controller_name_;
    s.parts = static_cast<int>(parts_.size()); s.parts_shown = parts_shown_; s.part_failures = part_failures_;
    s.poses = poses_applied_; s.assets_missing = assets_missing_; s.parts_lists = parts_updates_; s.frames_sent = frames_sent_;
    s.send_parts = static_cast<int>(sources_.size()); s.send_bones = static_cast<int>(send_bones_); s.send_kbps = send_kbps_;
    s.camera_fixes = camera_fixes_; s.possess_fixes = possess_fixes_;
    s.note[0] = note_[0]; s.note[1] = note_[1];
    AcquireSRWLockExclusive(&lock_); snapshot_ = std::move(s); ReleaseSRWLockExclusive(&lock_);
}
void Tick(bool transitioning) {
    const ULONGLONG now = GetTickCount64();
    last_tick_ = now;
    std::vector<Request> requests;
    AcquireSRWLockExclusive(&lock_); requests.swap(requests_); ReleaseSRWLockExclusive(&lock_);
    const bool peer = NetSteam::PeerReady();
    if (peer && !peer_was_ready_) { auto_spawn_ = true; next_parts_send_ = 0; }  // a new connection: both sides bring a puppet in, parts go again
    peer_was_ready_ = peer;
    stage_ = "tick";
    if (puppet_ && !PuppetAlive()) {
        Logf("NET_PUPPET gone (map change or destroyed by the game)");
        puppet_ = nullptr; parts_.clear(); parts_in_valid_ = false; pose_in_valid_ = false; root_valid_ = false; root_placed_ = false; source_ = 0; if (peer) auto_spawn_ = true;
    }
    Context c;
    if (transitioning || !ContextNow(c)) {
        for (Request r : requests) if (r == Request::MirrorOn || r == Request::MirrorOff) mirror_ = r == Request::MirrorOn;
        Publish(now); return;
    }
    // The player's controller ended up in the puppet: back into the hero.
    if (PuppetAlive() && c.pawn == puppet_) {
        if (Same(hero_, hero_name_)) Call(c.controller, "Possess").Set("InPawn", hero_).Invoke();
        if (++possess_fixes_ <= 5) Logf("NET_PUPPET the player's controller took the puppet; possessed the hero again (%s)", Text(Property(c.controller, "Pawn")).c_str());
        Publish(now); return;
    }
    if (hero_ != c.pawn) { sources_.clear(); next_scan_ = 0; }  // another hero pawn: its parts anew
    hero_ = c.pawn; hero_name_ = ue::NameOf(c.pawn);
    char map[64]{}; ue::NameText(ue::NameOf(c.world), map, sizeof(map)); local_map_ = map;
    stage_ = "requests";
    for (Request r : requests) {
        switch (r) {
        case Request::Spawn: stage_ = "spawn"; Spawn(c, now, "button"); break;
        case Request::Remove: Remove("button"); auto_spawn_ = false; Note(L"替身已移除", L"Puppet removed"); break;
        case Request::MirrorOn: mirror_ = true; break;
        case Request::MirrorOff: mirror_ = false; break;
        case Request::Check:
            check_until_ = now + 10000; next_check_ = 0; check_bone_max_ = check_part_max_ = check_sum_ = 0; check_bones_ = 0; check_rounds_ = check_shown_bad_ = 0; strcpy_s(check_worst_, "-");
            capture_next_.clear(); capture_shown_.clear(); capture_ready_ = false;
            Logf("NET_PUPPET check started (10 s; the loop: mirror on, no peer, puppet %s)", PuppetAlive() ? "here" : "not spawned");
            break;
        }
    }
    if (auto_spawn_ && !PuppetAlive()) { stage_ = "spawn"; Spawn(c, now, "peer connected"); }
    if (peer) { stage_ = "send"; SendState(c, now); Stream(c.pawn, now, true, false); }
    if (PuppetAlive()) {
        stage_ = "report"; Report(c, now);
        stage_ = "guard"; Guard(c, now);
        Drive(c, now, peer);
    }  // no puppet yet: the peer's newest parts and pose wait in NetSteam
    stage_ = "publish";
    if (PuppetAlive()) Timing(now);
    Publish(now);
    stage_ = "idle";
}
// After a fault the puppet goes (it stayed in the world as a second hero, out of reach of the remove button, 11-08).
void RemoveAfterFault() {
    __try { Remove("fault"); }
    __except (EXCEPTION_EXECUTE_HANDLER) { puppet_ = nullptr; Logf("NET_PUPPET the puppet could not be removed after the fault"); }
}
}  // namespace

void Initialize(Logger logger) { logger_ = logger; }
void OnGameThreadGuarded(bool transitioning) {
    if (disabled_) return;
    __try { Tick(transitioning); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        if (++faults_ >= 3) disabled_ = true;
        Logf("NET_PUPPET fault %lu/3 code=0x%08lx stage=%s part=%s%s", faults_, GetExceptionCode(), stage_, stage_part_, disabled_ ? "; puppet disabled" : "");
        RemoveAfterFault();
        parts_.clear(); sources_.clear(); source_ = 0; auto_spawn_ = false; stage_ = "idle";
    }
}
void Ask(Request request) {
    AcquireSRWLockExclusive(&lock_);
    if (requests_.size() < 16) requests_.push_back(request);
    ReleaseSRWLockExclusive(&lock_);
}
Snapshot Get() {
    AcquireSRWLockShared(&lock_); Snapshot s = snapshot_; ReleaseSRWLockShared(&lock_);
    return s;
}
}
