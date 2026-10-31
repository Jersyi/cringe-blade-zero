#include "grab_extras.h"
#include "settings_store.h"
#include "ue_runtime.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <string>
#include <vector>

namespace GrabExtras {
namespace {
using ue::Object; using ue::Call;
constexpr wchar_t kSection[] = L"GrabExtras";
SRWLOCK lock_ = SRWLOCK_INIT;
Options options_;
std::wstring result_ = L"投技附加：尚未施放";
Logger logger_ = nullptr;

struct HiddenActor { Object* actor = nullptr; ue::Name name; bool was_hidden = false; };
struct alignas(16) Transform { double rotation[4]{0, 0, 0, 1}, translation[4]{}, scale[4]{1, 1, 1, 0}; };
struct Vector { double x = 0, y = 0, z = 0; };
struct Rotator { double pitch = 0, yaw = 0, roll = 0; };
struct Display { Object* component = nullptr; ue::Name name; bool visible = true; };
struct Session {
    bool active = false;
    Object* pawn = nullptr; ue::Name pawn_name;
    Object* target = nullptr; ue::Name target_name;
    Options options;
    float hp_begin = -1, hp_max = -1;
    std::wstring move;
    bool damage_done = false;
    std::vector<HiddenActor> hidden;
    std::vector<Display> displays;
    Object* weapons_component = nullptr;
    ULONGLONG next_scan = 0;
    // Invisible Boss-skeleton mesh replaying the player's montage.
    Object* proxy = nullptr; ue::Name proxy_name; Object* proxy_montage = nullptr; double proxy_scale = 1;
};
Session session_;
struct Attribute { alignas(8) unsigned char bytes[56]{}; };

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[600]{}; va_list args; va_start(args, format); vsprintf_s(text, format, args); va_end(args); logger_(text);
}
// Shown on the grab page and logged, so every damage outcome is traceable.
void Result(const wchar_t* format, ...) {
    wchar_t text[400]{}; va_list args; va_start(args, format); vswprintf_s(text, format, args); va_end(args);
    AcquireSRWLockExclusive(&lock_); result_ = text; ReleaseSRWLockExclusive(&lock_);
    char utf8[1200]{}; WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (logger_) { char line[1300]{}; sprintf_s(line, "GRAB_EXTRAS result %s", utf8); logger_(line); }
}
bool Same(Object* object, ue::Name name) { return object && ue::Alive(object) && ue::NameOf(object) == name; }
Object* Field(Object* cls, const char* name) {
    for (int supers = 0; cls && supers < 64; ++supers, cls = ue::At<Object*>(cls, 0x40)) {
        int count = 0;
        for (Object* p = ue::At<Object*>(cls, 0x50); p && ++count < 1024; p = ue::At<Object*>(p, 0x10))
            if (ue::NameIs(ue::At<ue::Name>(p, 0x20), name)) return p;
    }
    return nullptr;
}
// FBoolProperty bitfield (byte offset 0x71, field mask 0x73), as read elsewhere in this DLL.
bool Bool(Object* object, const char* property, bool& value) {
    Object* field = object ? Field(ue::ClassOf(object), property) : nullptr;
    if (!field || ue::At<uint8_t>(field, 0x70) != 1) return false;
    const int32_t offset = ue::At<int32_t>(field, static_cast<int32_t>(ue::layout::kPropOffset));
    const uint8_t byte_offset = ue::At<uint8_t>(field, 0x71), mask = ue::At<uint8_t>(field, 0x73);
    if (offset < 0 || byte_offset > 7 || !mask) return false;
    value = (ue::At<uint8_t>(object, offset + byte_offset) & mask) != 0; return true;
}
bool Readable(const void* p, size_t size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!p || !VirtualQuery(p, &info, sizeof(info)) || info.State != MEM_COMMIT || (info.Protect & (PAGE_GUARD | PAGE_NOACCESS))) return false;
    const uintptr_t at = reinterpret_cast<uintptr_t>(p), base = reinterpret_cast<uintptr_t>(info.BaseAddress);
    return at >= base && size <= info.RegionSize && at - base <= info.RegionSize - size;
}
template<class T> bool ReadField(Object* object, const char* name, T& value) {
    Object* field = ue::Alive(object) ? Field(ue::ClassOf(object), name) : nullptr;
    if (!field || ue::At<int32_t>(field, 0x34) != static_cast<int32_t>(sizeof(T))) return false;
    const int32_t offset = ue::At<int32_t>(field, static_cast<int32_t>(ue::layout::kPropOffset));
    if (offset < 0 || !Readable(object + offset, sizeof(T))) return false;
    value = ue::At<T>(object, offset); return true;
}
bool IsWeaponActor(Object* actor) { return ue::Alive(actor) && Field(ue::ClassOf(actor), "WeaponMesh") != nullptr; }
// Weapon actors attached anywhere under the player (hands, back, scabbards).
std::vector<Object*> WeaponActors(Object* pawn) {
    std::vector<Object*> result;
    Call attached(pawn, "GetAttachedActors");
    attached.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true);
    if (!attached.Invoke()) return result;
    const ue::Array list = attached.Get<ue::Array>("OutActors");
    if (list.data && list.num > 0 && list.num <= 256)
        for (int32_t i = 0; i < list.num; ++i) {
            Object* actor = static_cast<Object**>(list.data)[i];
            if (IsWeaponActor(actor)) result.push_back(actor);
        }
    ue::Free(list.data);
    return result;
}
void HideWeapons() {
    for (Object* actor : WeaponActors(session_.pawn)) {
        auto known = std::find_if(session_.hidden.begin(), session_.hidden.end(), [&](const HiddenActor& item) { return item.actor == actor; });
        bool hidden = false;
        if (!Bool(actor, "bHidden", hidden)) continue;
        if (known == session_.hidden.end()) {
            HiddenActor item; item.actor = actor; item.name = ue::NameOf(actor); item.was_hidden = hidden;
            session_.hidden.push_back(item);
        }
        // The grab's own ForceWeaponVisibility actions can show the weapon again; keep it hidden.
        if (!hidden) Call(actor, "SetActorHiddenInGame").Set("bNewHidden", true).Invoke();
    }
}
void RestoreWeapons() {
    uint32_t restored = 0;
    for (const HiddenActor& item : session_.hidden) {
        if (item.was_hidden || !Same(item.actor, item.name)) continue;
        if (Call(item.actor, "SetActorHiddenInGame").Set("bNewHidden", false).Invoke()) ++restored;
    }
    if (!session_.hidden.empty()) Logf("GRAB_EXTRAS weapons restored=%u tracked=%zu", restored, session_.hidden.size());
    session_.hidden.clear();
}
// ---- Source Boss weapon stand-in -------------------------------------------
// FRotator -> FQuat exactly as UE's FRotator::Quaternion (degrees, Z-up).
void Quaternion(const Rotator& r, double q[4]) {
    const double k = 3.14159265358979323846 / 360.0;
    const double sp = std::sin(r.pitch * k), cp = std::cos(r.pitch * k), sy = std::sin(r.yaw * k), cy = std::cos(r.yaw * k);
    const double sr = std::sin(r.roll * k), cr = std::cos(r.roll * k);
    q[0] = cr * sp * sy - sr * cp * cy; q[1] = -cr * sp * cy - sr * cp * sy;
    q[2] = cr * cp * sy - sr * sp * cy; q[3] = cr * cp * cy + sr * sp * sy;
}
Object* DefaultOf(const char* class_path) {
    const std::string text = class_path ? class_path : ""; const size_t dot = text.find_last_of('.');
    return dot == std::string::npos ? nullptr : ue::FindObject((text.substr(0, dot + 1) + "Default__" + text.substr(dot + 1)).c_str());
}
ue::Name Socket(Object* mesh, const char* text) {
    // Configs name either the bone (weapon_r) or a socket (weapon_r_Socket); use what the player mesh has.
    std::string base = text ? text : "";
    std::vector<std::string> tries{base};
    const size_t cut = base.size() > 7 ? base.size() - 7 : std::string::npos;
    if (cut != std::string::npos && (base.compare(cut, 7, "_Socket") == 0 || base.compare(cut, 7, "_socket") == 0)) tries.push_back(base.substr(0, cut));
    else { tries.push_back(base + "_Socket"); tries.push_back(base + "_socket"); }
    for (const std::string& name : tries) {
        const std::wstring wide(name.begin(), name.end());
        const ue::Name candidate = ue::MakeName(wide.c_str());
        Call exists(mesh, "DoesSocketExist");
        if (!candidate.None() && exists.Set("InSocketName", candidate).Invoke() && exists.Get<bool>("ReturnValue")) return candidate;
    }
    return {};
}
double Finite(const Transform& t) {
    double sum = 0; for (int i = 0; i < 4; ++i) sum += t.rotation[i] + t.translation[i] + t.scale[i];
    return sum;
}
// q * v, as UE's FQuat::RotateVector.
void Rotate(const double q[4], const double v[3], double out[3]) {
    const double tx = 2 * (q[1] * v[2] - q[2] * v[1]), ty = 2 * (q[2] * v[0] - q[0] * v[2]), tz = 2 * (q[0] * v[1] - q[1] * v[0]);
    out[0] = v[0] + q[3] * tx + q[1] * tz - q[2] * ty;
    out[1] = v[1] + q[3] * ty + q[2] * tx - q[0] * tz;
    out[2] = v[2] + q[3] * tz + q[0] * ty - q[1] * tx;
}
// child expressed in parent's space -> child relative to parent's parent (UE: child * parent).
Transform Compose(const Transform& child, const Transform& parent) {
    Transform out;
    const double* a = parent.rotation; const double* b = child.rotation;
    out.rotation[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out.rotation[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out.rotation[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out.rotation[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
    const double scaled[3] = {child.translation[0] * parent.scale[0], child.translation[1] * parent.scale[1], child.translation[2] * parent.scale[2]};
    double moved[3]{}; Rotate(parent.rotation, scaled, moved);
    for (int i = 0; i < 3; ++i) { out.translation[i] = moved[i] + parent.translation[i]; out.scale[i] = child.scale[i] * parent.scale[i]; }
    out.translation[3] = out.scale[3] = 0;
    return out;
}
// Rest pose of a weapon bone under its hand. Hun_Skin_Skeleton values (reference
// skeleton read for the NPC retarget work) stand in if the engine call is absent.
bool RestPose(Object* mesh, ue::Name bone, const char* bone_text, Transform& rest) {
    Call index(mesh, "GetBoneIndex"); Call ref(mesh, "GetRefPoseTransform");
    if (index.Set("BoneName", bone).Invoke() && index.Get<int32_t>("ReturnValue") >= 0 && ref.Ok() &&
        ref.Set("BoneIndex", index.Get<int32_t>("ReturnValue")).Invoke()) {
        rest = ref.Get<Transform>("ReturnValue");
        const double q = rest.rotation[0] * rest.rotation[0] + rest.rotation[1] * rest.rotation[1] + rest.rotation[2] * rest.rotation[2] + rest.rotation[3] * rest.rotation[3];
        if (std::isfinite(Finite(rest)) && std::fabs(q - 1) < .01) return true;
    }
    if (!_stricmp(bone_text, "weapon_r")) {
        rest = {}; const double r[4] = {-0.03743722994284844, 0.14845289272462656, -0.5255051772636891, -0.836901727280423};
        const double t[3] = {-8.475495119716072, 4.176469478690876, -0.73497382634093};
        for (int i = 0; i < 4; ++i) rest.rotation[i] = r[i];
        for (int i = 0; i < 3; ++i) rest.translation[i] = t[i];
        return true;
    }
    if (!_stricmp(bone_text, "weapon_l")) {
        rest = {}; const double r[4] = {0.8507658585360084, 0.5017682365531673, 0.14986540085191122, -0.04434469938233011};
        const double t[3] = {9.09385688287881, -3.2868063255821625, -0.7440141043209074};
        for (int i = 0; i < 4; ++i) rest.rotation[i] = r[i];
        for (int i = 0; i < 3; ++i) rest.translation[i] = t[i];
        return true;
    }
    return false;
}
Transform Inverse(const Transform& t) {
    Transform out;
    out.rotation[0] = -t.rotation[0]; out.rotation[1] = -t.rotation[1]; out.rotation[2] = -t.rotation[2]; out.rotation[3] = t.rotation[3];
    const double moved[3] = {-t.translation[0], -t.translation[1], -t.translation[2]};
    Rotate(out.rotation, moved, out.translation);
    return out;
}
// The borrowed Boss animation drives the player's weapon_r / weapon_l bone with
// the Boss's own local track (translation retarget mode Animation), so the
// weapon keeps the Boss's performance, but that track is relative to the Boss's
// rest grip. With the source skeleton's rest pose known, the stand-in stays on
// the animated weapon bone with a fixed correction K = player rest * Boss rest^-1:
// whenever the animation holds the weapon at the Boss's grip it sits at the
// player's grip. Without it the stand-in rides the hand at the player's rest
// grip. Other sockets (forearms, ...) are used as they are.
ue::Name Grip(Object* mesh, ue::Name socket, const double* boss_rest, Transform& relative, const char*& mode) {
    mode = "socket";
    ue::Name bone = socket;
    Call socket_bone(mesh, "GetSocketBoneName");
    if (socket_bone.Set("InSocketName", socket).Invoke() && !socket_bone.Get<ue::Name>("ReturnValue").None()) bone = socket_bone.Get<ue::Name>("ReturnValue");
    char bone_text[96]{}; ue::NameText(bone, bone_text, sizeof(bone_text));
    if (_strnicmp(bone_text, "weapon_", 7)) return socket;
    Call parent(mesh, "GetParentBone");
    if (!parent.Set("BoneName", bone).Invoke()) return socket;
    const ue::Name hand = parent.Get<ue::Name>("ReturnValue");
    char hand_text[96]{}; ue::NameText(hand, hand_text, sizeof(hand_text));
    if (_strnicmp(hand_text, "hand_", 5)) return socket;
    Transform rest; if (!RestPose(mesh, bone, bone_text, rest)) return socket;
    Transform local; // the socket's own offset on its bone; identity when the config names the bone
    if (!(bone == socket)) {
        Call offset(mesh, "GetSocketTransform");
        if (!offset.Set("InSocketName", socket).Set("TransformSpace", uint8_t{3}).Invoke()) return socket;
        local = offset.Get<Transform>("ReturnValue");
        if (!std::isfinite(Finite(local))) return socket;
    }
    if (boss_rest) {
        Transform boss;
        for (int i = 0; i < 4; ++i) boss.rotation[i] = boss_rest[i];
        for (int i = 0; i < 3; ++i) boss.translation[i] = boss_rest[4 + i];
        relative = Compose(Compose(relative, local), Compose(rest, Inverse(boss)));
        mode = "animated"; return bone;
    }
    relative = Compose(Compose(relative, local), rest);
    mode = "hand"; return hand;
}
// ---- Boss pose proxy ------------------------------------------------------------
// The grab montage is authored for the source Boss's skeleton. Played on the
// player it drives weapon_r with the Boss's track inside the player's body, so
// the weapon drifts off the hand or floats. An invisible mesh on the Boss's own
// skeleton, attached to the player's mesh and scaled to the player's height,
// replays the same montage at the same position every tick; the weapon hangs
// from that mesh's socket exactly as the Boss's weapon actor does.
bool CreateProxy(Object* pawn, Object* mesh, Object* asset, double scale) {
    Object* skeletal = ue::FindObject("/Script/Engine.SkeletalMeshComponent");
    if (!skeletal || !ue::Alive(asset) || !ue::Alive(mesh)) return false;
    Transform relative; relative.scale[0] = relative.scale[1] = relative.scale[2] = scale;
    Call add(pawn, "AddComponentByClass");
    if (!add.Set("Class", skeletal).Set("bManualAttachment", true).Set("RelativeTransform", relative).Set("bDeferredFinish", true).Invoke()) return false;
    Object* proxy = add.Get<Object*>("ReturnValue");
    if (!ue::Alive(proxy)) return false;
    const bool mesh_set = Call(proxy, "SetSkeletalMeshAsset").Set("NewMesh", asset).Invoke();
    Call(proxy, "SetCollisionEnabled").Set("NewType", uint8_t{0}).Invoke();
    Call attach(proxy, "K2_AttachToComponent");
    const bool attached = attach.Set("Parent", mesh).Set("SocketName", ue::Name{}).Set("LocationRule", uint8_t{0}).Set("RotationRule", uint8_t{0})
        .Set("ScaleRule", uint8_t{0}).Set("bWeldSimulatedBodies", false).Invoke() && attach.Get<bool>("ReturnValue");
    Call(pawn, "FinishAddComponent").Set("Component", proxy).Set("bManualAttachment", true).Set("RelativeTransform", relative).Invoke();
    if (!mesh_set || !attached) { Call(proxy, "K2_DestroyComponent").Set("Object", proxy).Invoke(); return false; }
    // Never drawn, but its bones must keep updating while hidden.
    Call(proxy, "SetVisibility").Set("bNewVisibility", false).Set("bPropagateToChildren", false).Invoke();
    if (Object* option = Field(ue::ClassOf(proxy), "VisibilityBasedAnimTickOption")) {
        const int32_t offset = ue::At<int32_t>(option, 0x44);
        if (offset > 0 && offset < 0x2000) ue::At<uint8_t>(proxy, offset) = 0; // AlwaysTickPoseAndRefreshBones
    }
    Call(proxy, "SetForcedLOD").Set("InNewForcedLOD", int32_t{1}).Invoke();
    Call(proxy, "SetAnimationMode").Set("InAnimationMode", uint8_t{1}).Invoke(); // AnimationSingleNode
    session_.proxy = proxy; session_.proxy_name = ue::NameOf(proxy); session_.proxy_scale = scale;
    return true;
}
void SyncProxy() {
    if (!session_.proxy || !Same(session_.proxy, session_.proxy_name) || !Same(session_.pawn, session_.pawn_name)) return;
    Object* mesh = nullptr; ReadField(session_.pawn, "Mesh", mesh);
    Call instance(mesh, "GetAnimInstance"); if (!instance.Invoke()) return;
    Object* anim = instance.Get<Object*>("ReturnValue"); if (!ue::Alive(anim)) return;
    Call active(anim, "GetCurrentActiveMontage"); if (!active.Invoke()) return;
    Object* montage = active.Get<Object*>("ReturnValue");
    if (!ue::Alive(montage)) return; // keep the last pose between montages
    if (montage != session_.proxy_montage) {
        if (!Call(session_.proxy, "PlayAnimation").Set("NewAnimToPlay", montage).Set("bLooping", false).Invoke()) return;
        Call(session_.proxy, "SetPlayRate").Set("Rate", 0.f).Invoke();
        session_.proxy_montage = montage;
        char name[160]{}; ue::NameText(ue::NameOf(montage), name, sizeof(name));
        Logf("GRAB_EXTRAS pose proxy plays %s", name);
    }
    Call position(anim, "Montage_GetPosition");
    if (position.Set("Montage", montage).Invoke())
        Call(session_.proxy, "SetPosition").Set("InPos", position.Get<float>("ReturnValue")).Set("bFireNotifies", false).Invoke();
}
void RemoveProxy() {
    if (session_.proxy && Same(session_.proxy, session_.proxy_name)) Call(session_.proxy, "K2_DestroyComponent").Set("Object", session_.proxy).Invoke();
    session_.proxy = nullptr; session_.proxy_montage = nullptr;
}
bool ShowWeapon(Object* pawn, const Weapon& weapon) {
    Object* mesh = nullptr; ReadField(pawn, "Mesh", mesh);
    Object* cdo = DefaultOf(weapon.class_path);
    Object* source = nullptr; ReadField(cdo, "WeaponMesh", source);
    if (!ue::Alive(mesh) || !ue::Alive(source) || !ue::Alive(weapon.actor_class)) { Logf("GRAB_EXTRAS boss weapon %s: template unavailable", weapon.class_path ? weapon.class_path : "?"); return false; }
    Object* skeletal = ue::FindObject("/Script/Engine.SkeletalMeshComponent"), *static_type = ue::FindObject("/Script/Engine.StaticMeshComponent");
    const bool is_skeletal = skeletal && ue::IsA(source, skeletal), is_static = static_type && ue::IsA(source, static_type);
    Object* asset = nullptr;
    if (is_skeletal) { Call get(source, "GetSkeletalMeshAsset"); if (get.Invoke()) asset = get.Get<Object*>("ReturnValue"); }
    else if (is_static) ReadField(source, "StaticMesh", asset);
    const ue::Name socket = Socket(mesh, weapon.socket);
    if (!ue::Alive(asset) || socket.None()) { Logf("GRAB_EXTRAS boss weapon %s: mesh=%d socket=%s missing", weapon.class_path, ue::Alive(asset) ? 1 : 0, weapon.socket); return false; }
    // Same placement as the Boss's own weapon actor: socket + the mesh template's relative transform.
    Transform relative; Vector location{}, scale{1, 1, 1}; Rotator rotation{};
    ReadField(source, "RelativeLocation", location); ReadField(source, "RelativeRotation", rotation); ReadField(source, "RelativeScale3D", scale);
    Quaternion(rotation, relative.rotation);
    relative.translation[0] = location.x; relative.translation[1] = location.y; relative.translation[2] = location.z;
    relative.scale[0] = scale.x; relative.scale[1] = scale.y; relative.scale[2] = scale.z;
    const char* mode = "socket";
    Object* parent = mesh;
    ue::Name attach_to = socket;
    const ue::Name proxy_socket = session_.proxy ? Socket(session_.proxy, weapon.socket) : ue::Name{};
    if (!proxy_socket.None()) {
        // Exactly the Boss weapon actor's placement on the Boss skeleton; the
        // weapon keeps its real size inside the height-scaled proxy.
        parent = session_.proxy; attach_to = proxy_socket; mode = "boss-pose";
        for (int i = 0; i < 3; ++i) relative.scale[i] /= session_.proxy_scale;
    } else attach_to = Grip(mesh, socket, weapon.rest, relative, mode);
    Call add(pawn, "AddComponentByClass");
    if (!add.Set("Class", is_skeletal ? skeletal : static_type).Set("bManualAttachment", true).Set("RelativeTransform", relative).Set("bDeferredFinish", true).Invoke()) return false;
    Object* component = add.Get<Object*>("ReturnValue");
    if (!ue::Alive(component)) return false;
    const bool mesh_set = is_skeletal ? Call(component, "SetSkeletalMeshAsset").Set("NewMesh", asset).Invoke()
                                      : Call(component, "SetStaticMesh").Set("NewMesh", asset).Invoke();
    Call(component, "SetCollisionEnabled").Set("NewType", uint8_t{0}).Invoke();
    Call attach(component, "K2_AttachToComponent");
    const bool attached = attach.Set("Parent", parent).Set("SocketName", attach_to).Set("LocationRule", uint8_t{0}).Set("RotationRule", uint8_t{0})
        .Set("ScaleRule", uint8_t{0}).Set("bWeldSimulatedBodies", false).Invoke() && attach.Get<bool>("ReturnValue");
    Call(pawn, "FinishAddComponent").Set("Component", component).Set("bManualAttachment", true).Set("RelativeTransform", relative).Invoke();
    ue::Array materials{};
    if (ReadField(source, "OverrideMaterials", materials) && materials.data && materials.num > 0 && materials.num <= 64)
        for (int32_t i = 0; i < materials.num; ++i)
            if (Object* material = static_cast<Object**>(materials.data)[i]) Call(component, "SetMaterial").Set("ElementIndex", i).Set("Material", material).Invoke();
    if (!mesh_set || !attached) {
        Call(component, "K2_DestroyComponent").Set("Object", component).Invoke();
        Logf("GRAB_EXTRAS boss weapon %s: mesh_set=%d attached=%d; removed", weapon.class_path, mesh_set ? 1 : 0, attached ? 1 : 0);
        return false;
    }
    Display display; display.component = component; display.name = ue::NameOf(component);
    session_.displays.push_back(display);
    char socket_text[96]{}, attach_text[96]{}; ue::NameText(socket, socket_text, sizeof(socket_text)); ue::NameText(attach_to, attach_text, sizeof(attach_text));
    Logf("GRAB_EXTRAS boss weapon shown %s socket=%s attached=%s mode=%s %s", weapon.class_path, socket_text, attach_text, mode, is_skeletal ? "skeletal" : "static");
    return true;
}
// The grab's own visibility actions (GAAction_ForceWeaponVisibility) call
// CharacterWeaponsComponent::SetArmed / SetWeaponForceHide on the executing
// character, i.e. on the player. SetWeaponForceHide (native 0x5e8f060 ->
// 0x5e95d60) appends the actor index to a TArray<int32> at +0x60 of the slot's
// current RuntimeWeaponSingleData (stride 0x120). Mirror that onto the stand-in.
bool WeaponShouldShow(Object* weapons) {
    static const ue::Name main_slot = ue::MakeName(L"Weapon.Slot.Main");
    if (!ue::Alive(weapons) || main_slot.None()) return true;
    Call armed(weapons, "IsArmed");
    if (armed.Set("SlotTag", main_slot).Invoke() && !armed.Get<bool>("ReturnValue")) return false;
    Object* field = Field(ue::ClassOf(weapons), "RuntimeWeaponData");
    if (!field || ue::At<int32_t>(field, 0x34) != 80) return true;
    const int32_t offset = ue::At<int32_t>(field, static_cast<int32_t>(ue::layout::kPropOffset));
    unsigned char* map = weapons + offset;
    if (offset <= 0 || !Readable(map, 56)) return true;
    unsigned char* rows = ue::At<unsigned char*>(map, 0); const int32_t count = ue::At<int32_t>(map, 8), free_count = ue::At<int32_t>(map, 52);
    if (!rows || count <= 0 || count > 64 || !Readable(rows, static_cast<size_t>(count) * 48)) return true;
    const uint32_t* bits = ue::At<uint32_t*>(map, 32) ? ue::At<uint32_t*>(map, 32) : reinterpret_cast<uint32_t*>(map + 16);
    for (int32_t i = 0; i < count; ++i) {
        if (free_count && (!Readable(bits + i / 32, 4) || !((bits[i / 32] >> (i % 32)) & 1))) continue;
        unsigned char* row = rows + static_cast<size_t>(i) * 48;
        if (!(ue::At<ue::Name>(row, 0) == main_slot)) continue;
        unsigned char* list = ue::At<unsigned char*>(row, 8); const int32_t weapons_count = ue::At<int32_t>(row, 16), current = ue::At<int32_t>(row, 24);
        if (!list || current < 0 || current >= weapons_count || weapons_count > 16 || !Readable(list + static_cast<size_t>(current) * 0x120, 0x120)) return true;
        unsigned char* single = list + static_cast<size_t>(current) * 0x120;
        const int32_t hidden = ue::At<int32_t>(single, 0x68), capacity = ue::At<int32_t>(single, 0x6c);
        if (hidden < 0 || hidden > capacity || capacity > 64) return true;
        return hidden == 0;
    }
    return true;
}
void UpdateDisplays() {
    if (session_.displays.empty()) return;
    const bool show = WeaponShouldShow(session_.weapons_component);
    for (Display& display : session_.displays) {
        if (display.visible == show || !Same(display.component, display.name)) continue;
        if (Call(display.component, "SetVisibility").Set("bNewVisibility", show).Set("bPropagateToChildren", true).Invoke()) display.visible = show;
    }
}
void RemoveDisplays() {
    for (const Display& display : session_.displays)
        if (Same(display.component, display.name)) Call(display.component, "K2_DestroyComponent").Set("Object", display.component).Invoke();
    if (!session_.displays.empty()) Logf("GRAB_EXTRAS boss weapon stand-ins removed=%zu", session_.displays.size());
    session_.displays.clear();
}
bool MakeAttribute(const wchar_t* text, Attribute& attribute) {
    Object* library = ue::Default("/Script/ProjectPBZ.Default__PBZBlueprintFunctionLibrary");
    Object* cls = ue::FindObject("/Script/CharacterBasics.BasicAttributeSet");
    if (!library || !cls) return false;
    const int32_t count = static_cast<int32_t>(std::wcslen(text) + 1); const ue::String name{text, count, count};
    Call make(library, "MakeAttribute");
    if (!make.Set("AttributeName", name).Set("Set", cls).Invoke()) return false;
    attribute = make.Get<Attribute>("ReturnValue");
    return ue::At<Object*>(attribute.bytes, 16) != nullptr;
}
void FreeAttribute(Attribute& attribute) {
    // FString@0 and FFieldPath::Path@32 come from native MakeAttribute (FMemory).
    ue::Free(ue::At<void*>(attribute.bytes, 0)); ue::Free(ue::At<void*>(attribute.bytes, 32)); attribute = {};
}
bool AttributeLayout() {
    Object* type = ue::FindObject("/Script/GameplayAbilities.GameplayAttribute");
    return type && ue::StructSize(type) == 56 && ue::Offset(type, "AttributeName") == 0 && ue::Offset(type, "Attribute") == 16 && ue::Offset(type, "AttributeOwner") == 48;
}
bool ReadAttribute(Object* character, const wchar_t* name, float& value) {
    Attribute attribute; if (!MakeAttribute(name, attribute)) { FreeAttribute(attribute); return false; }
    Call get(ue::Default("/Script/ProjectPBZ.Default__PBZBlueprintFunctionLibrary"), "GetAttributeFromCharacter");
    const bool ok = get.Set("Character", character).Set("Attribute", attribute).Invoke();
    value = ok ? get.Get<float>("ReturnValue") : 0;
    FreeAttribute(attribute);
    return ok && std::isfinite(value) && value >= 0 && value < 1e9f;
}
bool ReadHealth(Object* character, float& health, float& maximum) {
    if (!AttributeLayout() || !ReadAttribute(character, L"Health", health)) return false;
    return ReadAttribute(character, L"Health_Max", maximum) || ReadAttribute(character, L"MaxHealth", maximum);
}
// Additive change through the game's own attribute utility (same call the JC
// reward uses), so attribute callbacks such as death handling still run.
bool AddHealth(Object* character, float delta) {
    Attribute attribute; if (!MakeAttribute(L"Health", attribute)) { FreeAttribute(attribute); return false; }
    Call add(ue::Default("/Script/SGameAttributes.Default__AttributeUtils"), "ModifyCharacterAttributeValue");
    const bool ok = add.Set("Character", character).Set("Attribute", attribute).Set("ModiferOp", uint8_t{0}).Set("Value", delta).Invoke();
    FreeAttribute(attribute); return ok;
}
// Last resort under "ignore invulnerability" when the target's attribute set
// refuses the change (an invulnerable phase): write Health's base and current
// value directly, never below 1, so death stays with the game's own damage path.
bool ForceHealth(Object* character, float value) {
    Object* set = nullptr;
    if (!ReadField(character, "AttributeSetBase", set) || !ue::Alive(set)) return false;
    Object* field = Field(ue::ClassOf(set), "Health");
    Object* data = ue::FindObject("/Script/GameplayAbilities.GameplayAttributeData");
    if (!field || !data || ue::StructSize(data) != 16 || ue::Offset(data, "BaseValue") != 8 || ue::Offset(data, "CurrentValue") != 12) return false;
    const int32_t offset = ue::At<int32_t>(field, 0x44);
    if (offset < 0 || offset > 0x4000) return false;
    ue::At<float>(set, offset + 8) = value; ue::At<float>(set, offset + 12) = value;
    return true;
}
// Native damage last dealt by each move, so an invulnerable target can still
// take that move's damage under "ignore invulnerability".
constexpr wchar_t kDamageSection[] = L"GrabDamage";
constexpr float kFallbackShare = .06f;
void ApplyDamage() {
    const Options& o = session_.options;
    if (!Same(session_.target, session_.target_name)) { Result(L"投技附加伤害：目标已不存在，未修改"); return; }
    float hp = 0, maximum = 0;
    if (session_.hp_begin < 0 || !ReadHealth(session_.target, hp, maximum)) { Result(L"投技附加伤害：无法读取目标生命，未修改"); return; }
    if (maximum <= 0) maximum = session_.hp_max;
    const float native = (std::max)(0.f, session_.hp_begin - hp);
    if (native > .01f && !session_.move.empty()) SettingsStore::PutFloat(kDamageSection, session_.move.c_str(), native);
    if (hp <= 0) { Result(L"投技附加伤害：原生投技已击倒目标（原生伤害 %.0f）", native); return; }
    // A boss victim usually takes no native grab damage at all (the victim side
    // is authored for the player), so a zero is not proof of invulnerability.
    // Scaling modes then start from the stand-in; Native mode only when told
    // to ignore invulnerability.
    const bool blocked = native <= .01f;
    if (o.damage == Damage::Native && !(blocked && o.ignore_invulnerable)) { Result(L"投技伤害：原生 %.0f", native); return; }
    float base = native;
    if (blocked) {
        base = session_.move.empty() ? -1.f : SettingsStore::GetFloat(kDamageSection, session_.move.c_str(), -1.f);
        if (!std::isfinite(base) || base <= .01f) base = maximum > 0 ? maximum * kFallbackShare : 0.f;
    }
    float wanted = base;
    if (o.damage == Damage::Multiplier) wanted = base * std::clamp(o.multiplier, .1f, 10.f);
    else if (o.damage == Damage::Percent && maximum > 0) wanted = (std::max)(base, maximum * std::clamp(o.percent, 1.f, 100.f) / 100.f);
    float delta = native - wanted; // negative removes health
    if (hp + delta < 0) delta = -hp;
    if (std::fabs(delta) < .01f) { Result(L"投技附加伤害：原生伤害 %.0f 已满足设置，未追加", native); return; }
    if (!AddHealth(session_.target, delta)) { Result(L"投技附加伤害：游戏属性接口调用失败，未修改"); return; }
    float after = 0, ignored = 0; bool read = ReadHealth(session_.target, after, ignored);
    if (read && o.ignore_invulnerable && delta < 0 && std::fabs(after - hp) < .5f) {
        const float forced = (std::max)(1.f, hp + delta);
        const bool written = ForceHealth(session_.target, forced);
        read = ReadHealth(session_.target, after, ignored);
        Logf("GRAB_EXTRAS damage refused by target attributes; forced=%d %.1f->%.1f readback=%.1f", written ? 1 : 0, hp, forced, read ? after : -1.f);
    }
    Result(L"投技伤害：原生 %.0f → 合计 %.0f（生命 %.0f → %.0f，上限 %.0f）", native, native - delta, session_.hp_begin, read ? after : hp + delta, maximum);
    Logf("GRAB_EXTRAS damage mode=%d native=%.1f base=%.1f%s total=%.1f hp=%.1f->%.1f max=%.1f ignore_invulnerable=%d readback=%d",
        static_cast<int>(o.damage), native, base, blocked ? "(stand-in)" : "", native - delta, session_.hp_begin, read ? after : -1.f, maximum,
        o.ignore_invulnerable ? 1 : 0, read ? 1 : 0);
}
Options Sanitize(Options o) {
    o.multiplier = std::isfinite(o.multiplier) ? std::clamp(o.multiplier, .1f, 10.f) : 2.f;
    o.percent = std::isfinite(o.percent) ? std::clamp(o.percent, 1.f, 100.f) : 30.f;
    if (o.damage != Damage::Multiplier && o.damage != Damage::Percent) o.damage = Damage::Native;
    return o;
}
}
void Initialize(Logger logger) {
    logger_ = logger;
    Options o;
    o.hide_weapon = SettingsStore::GetBool(kSection, L"HideWeapon", true);
    o.damage = static_cast<Damage>(SettingsStore::GetInt(kSection, L"DamageMode", 0));
    o.multiplier = SettingsStore::GetFloat(kSection, L"Multiplier", 2.f);
    o.percent = SettingsStore::GetFloat(kSection, L"Percent", 30.f);
    o.ignore_invulnerable = SettingsStore::GetBool(kSection, L"IgnoreInvulnerable", false);
    AcquireSRWLockExclusive(&lock_); options_ = Sanitize(o); ReleaseSRWLockExclusive(&lock_);
}
void SetOptions(const Options& value) {
    const Options o = Sanitize(value);
    AcquireSRWLockExclusive(&lock_); options_ = o; ReleaseSRWLockExclusive(&lock_);
    SettingsStore::PutBool(kSection, L"HideWeapon", o.hide_weapon);
    SettingsStore::PutInt(kSection, L"DamageMode", static_cast<int>(o.damage));
    SettingsStore::PutFloat(kSection, L"Multiplier", o.multiplier);
    SettingsStore::PutFloat(kSection, L"Percent", o.percent);
    SettingsStore::PutBool(kSection, L"IgnoreInvulnerable", o.ignore_invulnerable);
}
Options GetOptions() { AcquireSRWLockShared(&lock_); const Options o = options_; ReleaseSRWLockShared(&lock_); return o; }
std::wstring LastResult() { AcquireSRWLockShared(&lock_); std::wstring text = result_; ReleaseSRWLockShared(&lock_); return text; }
void Begin(unsigned char* pawn, unsigned char* target, const Weapon* weapons, size_t weapon_count, const char* move) {
    if (session_.active) Finish(false);
    session_ = {};
    if (!ue::Alive(pawn) || !ue::Alive(target)) return;
    session_.active = true; session_.options = GetOptions();
    for (const char* c = move; c && *c; ++c) session_.move.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    session_.pawn = pawn; session_.pawn_name = ue::NameOf(pawn);
    session_.target = target; session_.target_name = ue::NameOf(target);
    float hp = 0, maximum = 0;
    if (ReadHealth(target, hp, maximum)) { session_.hp_begin = hp; session_.hp_max = maximum; }
    if (session_.options.hide_weapon) {
        HideWeapons();
        Call holder(pawn, "GetCharacterWeaponsComponent", "/Script/SGameCombatSystem.CharacterWeaponsHolder");
        if (holder.Invoke()) session_.weapons_component = holder.Get<Object*>("ReturnValue");
        for (size_t i = 0; weapons && i < weapon_count && i < kMaxWeapons; ++i) if (weapons[i].proxy_mesh) {
            Object* mesh = nullptr; ReadField(pawn, "Mesh", mesh);
            if (CreateProxy(pawn, mesh, weapons[i].proxy_mesh, weapons[i].proxy_scale > .05 ? weapons[i].proxy_scale : 1.0)) { SyncProxy(); break; }
        }
        for (size_t i = 0; weapons && i < weapon_count && i < kMaxWeapons; ++i) ShowWeapon(pawn, weapons[i]);
        UpdateDisplays();
    }
    session_.next_scan = GetTickCount64() + 100;
    Logf("GRAB_EXTRAS begin hide_weapon=%d own_weapons=%zu boss_weapons=%zu/%zu target_hp=%.1f/%.1f damage_mode=%d", session_.options.hide_weapon ? 1 : 0,
        session_.hidden.size(), session_.displays.size(), weapon_count, session_.hp_begin, session_.hp_max, static_cast<int>(session_.options.damage));
}
void Tick() {
    if (!session_.active || !session_.options.hide_weapon) return;
    if (!Same(session_.pawn, session_.pawn_name)) return;
    // Cheap re-check of known actors every tick; full re-scan for newly
    // attached weapons a few times per second.
    for (const HiddenActor& item : session_.hidden) {
        bool hidden = true;
        if (Same(item.actor, item.name) && Bool(item.actor, "bHidden", hidden) && !hidden)
            Call(item.actor, "SetActorHiddenInGame").Set("bNewHidden", true).Invoke();
    }
    if (GetTickCount64() >= session_.next_scan) { session_.next_scan = GetTickCount64() + 250; HideWeapons(); }
    SyncProxy();
    UpdateDisplays();
}
void Finish(bool completed) {
    if (!session_.active) return;
    Logf("GRAB_EXTRAS finish completed=%d damage_done=%d", completed ? 1 : 0, session_.damage_done ? 1 : 0);
    if (Same(session_.pawn, session_.pawn_name)) { RemoveDisplays(); RemoveProxy(); RestoreWeapons(); }
    else { session_.displays.clear(); session_.hidden.clear(); }
    if (completed && !session_.damage_done) ApplyDamage();
    session_ = {};
}
bool Active() { return session_.active; }
void SourceEnded() {
    if (!session_.active || session_.damage_done) return;
    session_.damage_done = true; ApplyDamage();
}
}
