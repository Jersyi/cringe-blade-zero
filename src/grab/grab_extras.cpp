#include "grab_extras.h"
#include "grab_weapon_visibility.h"
#include "settings_store.h"
#include "ue_runtime.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
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

// The hero's own weapons while the grab shows the Boss's (HideWeapons): each weapon actor, its own hidden flag when the
// grab first saw it (was_hidden) and as last read (last_hidden), and its parts the grab hides.
struct HiddenPart { Object* component = nullptr; ue::Name name; GrabWeaponVisibility::HeldPart held; bool logged = false; };
struct HiddenActor { Object* actor = nullptr; ue::Name name; bool was_hidden = false, last_hidden = false; std::vector<HiddenPart> parts; };
struct alignas(16) Transform { double rotation[4]{0, 0, 0, 1}, translation[4]{}, scale[4]{1, 1, 1, 0}; };
struct Vector { double x = 0, y = 0, z = 0; };
struct Rotator { double pitch = 0, yaw = 0, roll = 0; };
// parent / socket / base: where the stand-in hangs and its relative location there; hand: the hand
// bone above that socket, which the skin display's same-named hand replaces (FollowSkinHands).
#pragma warning(push)
#pragma warning(disable : 4324)  // Transform is 16-byte aligned: the struct pads to it
struct Display {
    Object* component = nullptr; ue::Name name; bool visible = true;
    Object* parent = nullptr; ue::Name parent_name, socket, hand; double base[3]{}; bool follow_refused = false;
    // Held (FollowSkinHands): it hangs on the visible mesh's hand bone (holder) with the Boss's grip; else on its parent
    // at its socket with `attached`. A switch keeps it where it is and blends the relative from `from` over kSwitchMs.
    Transform attached; Object* holder = nullptr; ue::Name holder_name; Transform from; ULONGLONG switched = 0;
    Transform last_grip; bool has_grip = false;
    // As the boss's own weapon actor hangs: its socket on the boss skeleton, the mesh template's relative transform.
    std::string boss_socket; Transform own_relative; Object* asset = nullptr; bool skeletal = false;
    // direct: on the look's own socket (a Boss look whose weapon bones follow the Boss pose, grab looksocket on, or a Boss
    // look without a pose proxy), nothing set per tick once the blend is done. smooth / smooth_clock: the grip last placed on the hero's hand (low-passed); last_switch: the
    // hand / pose switch.
    bool direct = false; Transform smooth; bool has_smooth = false; double smooth_clock = 0; ULONGLONG last_switch = 0;
    // A Boss look driving its own weapon bones (TrackLookWeapon): the weapon's place in the look's fist, and the Boss pose's
    // weapon in the Boss pose's fist, the tick before.
    Transform look_grip, boss_grip; bool look_has_grip = false;
};
#pragma warning(pop)
// What a grab cut by the move chain still owes its target (HandOff / HandOffEnd).
struct Owed {
    int32_t id = 0; Object* target = nullptr; ue::Name target_name; std::wstring move; float native = 0, hp_max = -1; uint32_t hits = 0; Options options;
};
std::vector<Owed> owed_;
int32_t owed_serial_ = 0;
struct Session {
    bool active = false;
    Object* pawn = nullptr; ue::Name pawn_name;
    Object* target = nullptr; ue::Name target_name;
    Options options;
    float hp_begin = -1, hp_max = -1;
    std::wstring move;
    bool damage_done = false;
    // The target's Health.CurrentValue, read every tick to find the grab's own damage events.
    Object* health_set = nullptr; ue::Name health_set_name; int32_t health_offset = -1; float last_hp = -1;
    float native = 0, extra = 0; uint32_t hits = 0, refused = 0, forced = 0;
    std::vector<HiddenActor> hidden;
    std::vector<Display> displays;
    Object* weapons_component = nullptr;
    ULONGLONG next_scan = 0;
    // Invisible Boss-skeleton mesh replaying the player's montage.
    Object* proxy = nullptr; ue::Name proxy_name; Object* proxy_montage = nullptr; double proxy_scale = 1;
    // The player's montage position at the last tick (proxy_seen: of proxy_montage) and its length: the proxy is set
    // one step ahead of it (SyncProxy), the step the player's montage takes this frame.
    float proxy_last = -1, proxy_length = 0; bool proxy_seen = false, own_hand_logged = false;
    // The grab's own montage on the player (Posing): the one playing at Begin, or the first within
    // kMontageSettleMs; pose_over once another montage or none replaces it.
    Object* grab_montage = nullptr; ULONGLONG began = 0; bool pose_over = false;
    // Current model / executing boss (skin size * hero_over_boss): the boss weapon's size. The player's
    // model size scales the pose proxy. Neither the models nor the victim are moved or scaled.
    double scale = 1, model = 1;
    // How well a stand-in stays in the visible fist (logged at the end): each tick, the hand offset wanted now minus
    // the one its last placement gives in the socket's current frame (what was drawn), and how far the fist moved.
    // How still the weapon sits in the fist while held (logged at the end): the grip's move a tick, and the switches.
    uint32_t hold_frames = 0, hold_switches = 0; double hold_sum = 0, hold_max = 0;
    ULONGLONG probe_at = 0;  // GRAB_EXTRAS probe lines (where the stand-ins are, twice a second)
    // The player's main slot drawn when the grab began (sheathed: the grab still shows the boss weapon in hand), and the
    // stand-ins' last shown state with its reasons (logged when it changes).
    bool armed_known = false, armed_at_begin = true; int shown_logged = -1;
    // Ticks since Begin: the stand-ins show from the second one on (the proxy's first pose is not the move's yet, and the
    // first placement put the weapon about 90 cm off for a frame).
    int ticks = 0;
    // A Boss look with the weapon on the Boss pose (logged at the end): the look's hand against the Boss pose's hand that
    // holds it, each tick from the one the stand-ins show on (pose_first: that tick's).
    uint32_t pose_ticks = 0; double pose_hand_sum = 0, pose_hand_max = 0, pose_first = -1;
    // Native sheathe (NativeSheathe): the success timeline's Main-slot switches with Armed=false, the ability and timeline
    // whose task clock times them, that clock's last value, and the latch.
    Object* source_ability = nullptr; ue::Name source_name;
    Object* source_timeline = nullptr; ue::Name timeline_name;
    std::vector<GrabWeaponVisibility::Window> sheathe_windows;
    float visibility_time = 0;
    GrabWeaponVisibility::NativeState native_visibility;
    // A Boss look whose own weapon bones follow the Boss pose (DriveLookWeapons): the bones asked for and how many it drives;
    // the stand-ins then hang on the look's own sockets. Logged at the end (TrackLookWeapon): while the Boss holds the
    // weapon, how far it moved in the look's fist a tick, and how far the Boss pose's own weapon moved in its fist (what the
    // move itself does); always, how far it is from the Boss pose's place.
    std::vector<std::string> look_names; size_t look_bones = 0;
    uint32_t look_ticks = 0, look_held = 0; double look_step_sum = 0, look_step_max = 0, look_boss_sum = 0, look_boss_max = 0;
    double boss_step_sum = 0, boss_step_max = 0;
    // Proxy sync (logged at the end): each tick before the proxy is set again, its montage time off the player's, i.e. how
    // far the two drifted apart over the frame just drawn (ms); proxy_rate: the play rate the proxy runs at.
    uint32_t sync_ticks = 0; double sync_sum = 0, sync_max = 0; float proxy_rate = 0;
    // The pair in step (TrackPairDrift, one line per segment): a segment runs while the hero and the target play the same two
    // montages. Each tick the target's montage step against the hero's goes to "steady" (both advancing, at different rates
    // or dilations) or, when one side moved 50 ms or more further in that tick, to "jumps" (a timeline jump, a section
    // change, a restart). Rates: the montage's play rate x the mesh's GlobalAnimRateScale; dilation: the actor's own.
    struct PairSegment {
        Object* hero = nullptr; Object* target = nullptr;
        float hero_start = 0, target_start = 0, hero_at = 0, target_at = 0;
        float hero_rate[2]{}, target_rate[2]{}, hero_dilation[2]{}, target_dilation[2]{};
        uint32_t ticks = 0, jumps = 0; double steady = 0, jumped = 0, min = 0, max = 0;
    };
    PairSegment pair; bool pair_on = false; uint32_t pair_segments = 0;
};
Session session_;
// Diagnostics (grab looksocket auto|on|off|proxy, not saved), for a look on the Boss's own skeleton: 0 lets the look's own
// weapon bones follow the Boss pose and hangs the weapon on the look's socket (default; on the Boss pose's socket when the
// look cannot), 1 hangs it on the look's own socket with the look's own weapon bone (as 11-12 to 11-14), 2 re-hangs it on
// the look's hand while the Boss holds it (as on the hero's own body).
// 3 keeps it on the Boss pose's socket even when the look could drive its own weapon bones (the first 11-15 fix).
std::atomic<int> look_mode_{0};
// Diagnostics (grab proxysync rate|predict, not saved): 1 runs the proxy at the player's montage time and play rate
// (default), 0 sets it one step ahead at rate 0 (11-08..11-15).
std::atomic<int> proxy_sync_{1};
// Diagnostics (grab weaponhide parts|actor, not saved): 1 (default) hides the hero's weapons part by part (each primitive
// component's bHiddenInGame) and leaves the weapon actor's own hidden flag to the game; 0 hides the weapon actors as
// before 11-15 (SetActorHiddenInGame, the flag the game draws and sheathes with).
std::atomic<int> weapon_hide_{1};
SkinDisplay skin_display_ = nullptr;
SkinWeapons skin_weapons_ = nullptr;
constexpr ULONGLONG kMontageSettleMs = 300, kMontageWaitMs = 1000;
// A skin hand further than this from the hand the weapon hangs on is a wrong bone, not a pose.
constexpr double kFollowMaxCm = 150;
struct Attribute { alignas(8) unsigned char bytes[56]{}; };

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[600]{}; va_list args; va_start(args, format); _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, args); va_end(args); logger_(text);
}
// Shown on the grab page and logged, so every damage outcome is traceable.
void Result(const wchar_t* format, ...) {
    wchar_t text[400]{}; va_list args; va_start(args, format); _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, args); va_end(args);
    AcquireSRWLockExclusive(&lock_); result_ = text; ReleaseSRWLockExclusive(&lock_);
    char utf8[1200]{}; WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, static_cast<int>(sizeof(utf8)), nullptr, nullptr);
    if (logger_) { char line[1300]{}; _snprintf_s(line, sizeof(line), _TRUNCATE, "GRAB_EXTRAS result %s", utf8); logger_(line); }
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
bool PutBool(Object* object, const char* property, bool value) {
    Object* field = object ? Field(ue::ClassOf(object), property) : nullptr;
    if (!field || ue::At<uint8_t>(field, 0x70) != 1) return false;
    const int32_t offset = ue::At<int32_t>(field, static_cast<int32_t>(ue::layout::kPropOffset));
    const uint8_t byte_offset = ue::At<uint8_t>(field, 0x71), byte_mask = ue::At<uint8_t>(field, 0x72), mask = ue::At<uint8_t>(field, 0x73);
    if (offset < 0 || byte_offset > 7 || !mask || !byte_mask) return false;
    uint8_t& byte = ue::At<uint8_t>(object, offset + byte_offset);
    byte = static_cast<uint8_t>((byte & ~mask) | (value ? byte_mask : 0)); return true;
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
// The primitive components of a weapon actor (meshes, effects; not collision shapes, hidden in game anyway).
std::vector<Object*> WeaponParts(Object* actor) {
    std::vector<Object*> result;
    Object* primitive = ue::FindObject("/Script/Engine.PrimitiveComponent"), *shape = ue::FindObject("/Script/Engine.ShapeComponent");
    if (!primitive) return result;
    Call get(actor, "K2_GetComponentsByClass");
    if (!get.Set("ComponentClass", primitive).Invoke()) return result;
    const ue::Array list = get.Get<ue::Array>("ReturnValue");
    if (list.data && list.num > 0 && list.num <= 256)
        for (int32_t i = 0; i < list.num; ++i) {
            Object* part = static_cast<Object**>(list.data)[i];
            if (ue::Alive(part) && !(shape && ue::IsA(part, shape))) result.push_back(part);
        }
    ue::Free(list.data);
    return result;
}
std::string NameString(Object* object) { char text[160]{}; if (object) ue::NameText(ue::NameOf(object), text, sizeof(text)); return text; }
std::string WeaponSocket(Object* actor) {
    Call root(actor, "K2_GetRootComponent"); ue::Name socket;
    if (!root.Invoke() || !ReadField(root.Get<Object*>("ReturnValue"), "AttachSocketName", socket)) return "?";
    char text[96]{}; ue::NameText(socket, text, sizeof(text)); return text;
}
bool WeaponShouldShow(Object* weapons, bool& armed_now);
bool MainDrawn() { bool armed = true; WeaponShouldShow(session_.weapons_component, armed); return armed; }
// The hero's own weapons are hidden part by part (each primitive component's bHiddenInGame) for the whole grab, each
// part's own flag kept, and shown again at the end as it was (GrabWeaponVisibility::HeldPart). The weapon actor's own
// hidden flag stays the game's: it shows a drawn weapon and hides a sheathed one with it, and the look change's blink
// hides and shows the hero's actors with it. Until 11-15 the grab hid the actors themselves, read that flag at Begin,
// hid every show during the grab again and showed at the end only those shown at Begin: a weapon the game drew during
// the grab, or one the blink had hidden right then, stayed hidden while the hero held it (user, 11-15: "武器会莫名消失").
// A new weapon actor or part (the game attaches one) is hidden on the next scan.
void HideWeapons() {
    const bool parts = weapon_hide_.load(std::memory_order_relaxed) != 0;
    for (Object* actor : WeaponActors(session_.pawn)) {
        auto known = std::find_if(session_.hidden.begin(), session_.hidden.end(), [&](const HiddenActor& item) { return item.actor == actor; });
        bool hidden = false;
        if (!Bool(actor, "bHidden", hidden)) continue;
        const bool fresh = known == session_.hidden.end();
        if (fresh) {
            HiddenActor item; item.actor = actor; item.name = ue::NameOf(actor); item.was_hidden = item.last_hidden = hidden;
            session_.hidden.push_back(item); known = std::prev(session_.hidden.end());
        }
        if (!parts) {  // as before 11-15 (grab weaponhide actor): the grab's own ForceWeaponVisibility can show it again; keep it hidden
            if (!hidden && Call(actor, "SetActorHiddenInGame").Set("bNewHidden", true).Invoke()) known->last_hidden = true;
            if (fresh) Logf("GRAB_EXTRAS hero weapon %s on %s: actor hidden=%d (hidden as a whole)", NameString(actor).c_str(), WeaponSocket(actor).c_str(), hidden ? 1 : 0);
            continue;
        }
        size_t added = 0, shown = 0;
        for (Object* part : WeaponParts(actor)) {
            if (std::any_of(known->parts.begin(), known->parts.end(), [&](const HiddenPart& p) { return p.component == part; })) continue;
            bool part_hidden = false; if (!Bool(part, "bHiddenInGame", part_hidden)) continue;
            HiddenPart p; p.component = part; p.name = ue::NameOf(part); p.held.restore_hidden = part_hidden;
            known->parts.push_back(p); ++added;
            if (!part_hidden && Call(part, "SetHiddenInGame").Set("NewHidden", true).Set("bPropagateToChildren", false).Invoke()) ++shown;
        }
        if (fresh || added)
            Logf("GRAB_EXTRAS hero weapon %s on %s: actor hidden=%d; %zu parts %s (%zu were shown)", NameString(actor).c_str(), WeaponSocket(actor).c_str(), hidden ? 1 : 0, added,
                 fresh ? "hidden" : "added and hidden", shown);
    }
}
// Every tick while the grab holds them: a part something else showed is hidden again (and shown at the end); with the
// actors hidden as a whole (weaponhide actor), a shown actor is hidden again. A change of the actor's own flag (the game
// drawing or sheathing, a blink) is logged.
void HoldWeapons() {
    const bool parts = weapon_hide_.load(std::memory_order_relaxed) != 0;
    const ULONGLONG t = GetTickCount64() - session_.began;
    for (HiddenActor& item : session_.hidden) {
        bool hidden = true;
        if (!Same(item.actor, item.name) || !Bool(item.actor, "bHidden", hidden)) continue;
        if (!parts) { if (!hidden) Call(item.actor, "SetActorHiddenInGame").Set("bNewHidden", true).Invoke(); continue; }
        if (hidden != item.last_hidden) {
            item.last_hidden = hidden;
            Logf("GRAB_EXTRAS hero weapon %s %s at %llu ms, not by the grab (main slot drawn %d)", NameString(item.actor).c_str(), hidden ? "hidden" : "shown", t, MainDrawn() ? 1 : 0);
        }
        for (HiddenPart& p : item.parts) {
            bool part_hidden = true;
            if (!Same(p.component, p.name) || !Bool(p.component, "bHiddenInGame", part_hidden) || !p.held.Seen(part_hidden)) continue;
            Call(p.component, "SetHiddenInGame").Set("NewHidden", true).Set("bPropagateToChildren", false).Invoke();
            if (!p.logged) {
                p.logged = true;
                Logf("GRAB_EXTRAS hero weapon %s part %s shown at %llu ms, not by the grab: hidden again, shown at the end", NameString(item.actor).c_str(), NameString(p.component).c_str(), t);
            }
        }
    }
}
void RestoreWeapons() {
    const bool parts = weapon_hide_.load(std::memory_order_relaxed) != 0;
    uint32_t restored = 0; size_t shown = 0;
    for (const HiddenActor& item : session_.hidden) {
        if (!Same(item.actor, item.name)) continue;
        if (!parts) { if (!item.was_hidden && Call(item.actor, "SetActorHiddenInGame").Set("bNewHidden", false).Invoke()) ++restored; }
        else for (const HiddenPart& p : item.parts)
            if (!p.held.restore_hidden && Same(p.component, p.name) && Call(p.component, "SetHiddenInGame").Set("NewHidden", false).Set("bPropagateToChildren", false).Invoke()) ++restored;
        bool hidden = true; Bool(item.actor, "bHidden", hidden); if (!hidden) ++shown;
        if (hidden != item.was_hidden)
            Logf("GRAB_EXTRAS hero weapon %s after the grab: %s (%s at the start) on %s", NameString(item.actor).c_str(), hidden ? "hidden" : "shown", item.was_hidden ? "hidden" : "shown",
                 WeaponSocket(item.actor).c_str());
    }
    if (!session_.hidden.empty())
        Logf("GRAB_EXTRAS weapons restored=%u %s tracked=%zu: %zu weapon actors shown after the grab (main slot drawn %d)", restored, parts ? "parts" : "actors", session_.hidden.size(), shown,
             MainDrawn() ? 1 : 0);
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
// from that mesh's socket exactly as the Boss's weapon actor does. It shook
// until 11-08: never rendered, the mesh's update-rate optimisation evaluated it
// every few frames only (now off), and the position it was given was the
// player's of the frame before (now one step ahead: the step the player's
// montage took since the tick before, which it takes again this frame).
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
    const bool every_frame = PutBool(proxy, "bEnableUpdateRateOptimizations", false);  // a never rendered mesh is evaluated every 4th frame otherwise
    const bool after = Call(proxy, "AddTickPrerequisiteComponent").Set("PrerequisiteComponent", mesh).Invoke();
    Call(proxy, "SetAnimationMode").Set("InAnimationMode", uint8_t{1}).Invoke(); // AnimationSingleNode
    session_.proxy = proxy; session_.proxy_name = ue::NameOf(proxy); session_.proxy_scale = scale;
    Logf("GRAB_EXTRAS pose proxy scale=%.3f updateRateOptimizationsOff=%d ticksAfterPlayer=%d", scale, every_frame ? 1 : 0, after ? 1 : 0);
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
        Call(session_.proxy, "SetPlayRate").Set("Rate", 0.f).Invoke(); session_.proxy_rate = 0;
        session_.proxy_montage = montage; session_.proxy_seen = false; session_.proxy_last = -1;
        Call length(montage, "GetPlayLength"); session_.proxy_length = length.Invoke() ? length.Get<float>("ReturnValue") : 0.f;
        char name[160]{}; ue::NameText(ue::NameOf(montage), name, sizeof(name));
        Logf("GRAB_EXTRAS pose proxy plays %s", name);
    }
    Call position(anim, "Montage_GetPosition");
    if (!position.Set("Montage", montage).Invoke()) return;
    const float now = position.Get<float>("ReturnValue");
    if (!std::isfinite(now)) return;
    // How far the proxy drifted from the player over the frame just drawn: its montage time now against the player's.
    Call where(session_.proxy, "GetPosition");
    if (session_.proxy_seen && where.Invoke()) {
        const double drift = std::fabs(static_cast<double>(where.Get<float>("ReturnValue")) - now) * 1000;
        if (std::isfinite(drift) && drift < 250) { ++session_.sync_ticks; session_.sync_sum += drift; session_.sync_max = (std::max)(session_.sync_max, drift); }
    }
    float at = now, rate = 0;
    if (proxy_sync_.load(std::memory_order_relaxed)) {
        // Same time, same rate (11-15): the proxy is a component of the player's actor, so the coming frame advances both by
        // the same scaled delta (time dilation included) and the proxy is drawn at the player's montage time of that frame.
        // Predicting one step ahead missed whenever the frame time or a slow-motion changed (the next step is not the last).
        Call play(anim, "Montage_GetPlayRate"), playing(anim, "Montage_IsPlaying");
        rate = play.Set("Montage", montage).Invoke() ? play.Get<float>("ReturnValue") : 1.f;
        if (!std::isfinite(rate) || rate < 0 || rate > 10) rate = 1;
        if (playing.Set("Montage", montage).Invoke() && !playing.Get<bool>("ReturnValue")) rate = 0;  // paused
    } else if (session_.proxy_seen) {
        // One step ahead (11-08..11-15): the step since the tick before, when it is a plain forward step (not a jump to
        // another section, a stop or a restart), kept inside the montage.
        const float step = now - session_.proxy_last;
        if (step > 0 && step < .25f) at = now + step;
        if (session_.proxy_length > 0 && at > session_.proxy_length) at = session_.proxy_length;
    }
    session_.proxy_last = now; session_.proxy_seen = true;
    if (rate != session_.proxy_rate) { Call(session_.proxy, "SetPlayRate").Set("Rate", rate).Invoke(); session_.proxy_rate = rate; }
    Call(session_.proxy, "SetPosition").Set("InPos", at).Set("bFireNotifies", false).Invoke();
}
void StopLookWeapons() {
    if (session_.look_bones && skin_weapons_) skin_weapons_(nullptr, nullptr, 0);
    session_.look_bones = 0;
}
void RemoveProxy() {
    StopLookWeapons();  // the look stops reading the proxy before it goes
    if (session_.proxy && Same(session_.proxy, session_.proxy_name)) Call(session_.proxy, "K2_DestroyComponent").Set("Object", session_.proxy).Invoke();
    session_.proxy = nullptr; session_.proxy_montage = nullptr;
}
Object* CurrentMontage(Object* pawn) {
    Object* mesh = nullptr; ReadField(pawn, "Mesh", mesh);
    if (!ue::Alive(mesh)) return nullptr;
    Call instance(mesh, "GetAnimInstance"); if (!instance.Invoke()) return nullptr;
    Object* anim = instance.Get<Object*>("ReturnValue"); if (!ue::Alive(anim)) return nullptr;
    Call active(anim, "GetCurrentActiveMontage"); if (!active.Invoke()) return nullptr;
    Object* montage = active.Get<Object*>("ReturnValue");
    return ue::Alive(montage) ? montage : nullptr;
}
// A montage on an actor's mesh (its current active montage): its position, its play rate times the mesh's
// GlobalAnimRateScale, and the actor's own time dilation. False when none / unreadable.
bool MontageAt(Object* actor, Object*& montage, float& at, float& rate, float& dilation) {
    Object* mesh = nullptr; ReadField(actor, "Mesh", mesh);
    if (!ue::Alive(mesh)) return false;
    Call instance(mesh, "GetAnimInstance"); if (!instance.Invoke()) return false;
    Object* anim = instance.Get<Object*>("ReturnValue"); if (!ue::Alive(anim)) return false;
    Call active(anim, "GetCurrentActiveMontage"); if (!active.Invoke()) return false;
    montage = active.Get<Object*>("ReturnValue"); if (!ue::Alive(montage)) return false;
    Call position(anim, "Montage_GetPosition"); if (!position.Set("Montage", montage).Invoke()) return false;
    at = position.Get<float>("ReturnValue");
    Call play(anim, "Montage_GetPlayRate"); rate = play.Set("Montage", montage).Invoke() ? play.Get<float>("ReturnValue") : 1.f;
    float scale = 1; if (ReadField(mesh, "GlobalAnimRateScale", scale) && std::isfinite(scale)) rate *= scale;
    dilation = 1; ReadField(actor, "CustomTimeDilation", dilation);
    return std::isfinite(at) && std::isfinite(rate) && std::isfinite(dilation);
}
void LogPairSegment(const char* why) {
    const Session::PairSegment& s = session_.pair;
    if (!s.ticks) return;
    char hero_name[128]{}, target_name[128]{};
    if (ue::Alive(s.hero)) ue::NameText(ue::NameOf(s.hero), hero_name, sizeof(hero_name));
    if (ue::Alive(s.target)) ue::NameText(ue::NameOf(s.target), target_name, sizeof(target_name));
    Logf("GRAB_EXTRAS pair in step #%u (%s): target %s %.2f->%.2f s (rate %.2f..%.2f, dilation %.2f..%.2f), hero %s %.2f->%.2f s "
         "(rate %.2f..%.2f, dilation %.2f..%.2f) over %u ticks: the target %+.0f ms off the hero (steady %+.0f, %u jumps %+.0f; "
         "range %+.0f..%+.0f ms; - = behind)", session_.pair_segments + 1, why,
         target_name, s.target_start, s.target_at, s.target_rate[0], s.target_rate[1], s.target_dilation[0], s.target_dilation[1],
         hero_name, s.hero_start, s.hero_at, s.hero_rate[0], s.hero_rate[1], s.hero_dilation[0], s.hero_dilation[1],
         s.ticks, s.steady + s.jumped, s.steady, s.jumps, s.jumped, s.min, s.max);
}
void TrackPairDrift() {
    if (session_.pair_segments >= 8 || !Same(session_.target, session_.target_name)) return;
    Object* hero = nullptr, *target = nullptr;
    float hero_at = 0, target_at = 0, hero_rate = 1, target_rate = 1, hero_dilation = 1, target_dilation = 1;
    const bool seen = MontageAt(session_.pawn, hero, hero_at, hero_rate, hero_dilation) &&
        MontageAt(session_.target, target, target_at, target_rate, target_dilation);
    Session::PairSegment& s = session_.pair;
    if (session_.pair_on && (!seen || hero != s.hero || target != s.target)) {
        LogPairSegment(seen ? "a montage changed" : "a montage stopped"); ++session_.pair_segments; session_.pair_on = false;
    }
    if (!seen) return;
    const auto widen = [](float (&range)[2], float value) { range[0] = (std::min)(range[0], value); range[1] = (std::max)(range[1], value); };
    if (!session_.pair_on) {
        s = Session::PairSegment{}; s.hero = hero; s.target = target;
        s.hero_start = s.hero_at = hero_at; s.target_start = s.target_at = target_at;
        s.hero_rate[0] = s.hero_rate[1] = hero_rate; s.target_rate[0] = s.target_rate[1] = target_rate;
        s.hero_dilation[0] = s.hero_dilation[1] = hero_dilation; s.target_dilation[0] = s.target_dilation[1] = target_dilation;
        session_.pair_on = true; return;
    }
    const double step = (static_cast<double>(target_at) - s.target_at) - (static_cast<double>(hero_at) - s.hero_at);
    s.hero_at = hero_at; s.target_at = target_at;
    widen(s.hero_rate, hero_rate); widen(s.target_rate, target_rate); widen(s.hero_dilation, hero_dilation); widen(s.target_dilation, target_dilation);
    if (!std::isfinite(step)) return;
    if (std::fabs(step) >= .05) { ++s.jumps; s.jumped += step * 1000; } else s.steady += step * 1000;
    ++s.ticks; s.min = (std::min)(s.min, s.steady + s.jumped); s.max = (std::max)(s.max, s.steady + s.jumped);
}
void UpdatePosing() {
    if (session_.pose_over) return;
    Object* montage = CurrentMontage(session_.pawn);
    const ULONGLONG ms = GetTickCount64() - session_.began;
    if (montage == session_.grab_montage) return;
    // Right after Begin the paired montage may still be replacing the move the grab started from.
    if (montage && (!session_.grab_montage || ms < kMontageSettleMs)) { session_.grab_montage = montage; return; }
    if (!session_.grab_montage && ms < kMontageWaitMs) return;
    session_.pose_over = true;
    char name[160] = "none"; if (montage) ue::NameText(ue::NameOf(montage), name, sizeof(name));
    Logf("GRAB_EXTRAS grab pose over after %llu ms (now %s)", ms, name);
}
// The hand bone a socket hangs under (the socket's bone or one of its parents), or None.
ue::Name HandBone(Object* mesh, ue::Name socket) {
    ue::Name bone = socket;
    Call socket_bone(mesh, "GetSocketBoneName");
    if (socket_bone.Set("InSocketName", socket).Invoke() && !socket_bone.Get<ue::Name>("ReturnValue").None()) bone = socket_bone.Get<ue::Name>("ReturnValue");
    for (int depth = 0; depth < 6 && !bone.None(); ++depth) {
        char text[96]{}; ue::NameText(bone, text, sizeof(text));
        if (!_strnicmp(text, "hand_", 5)) return bone;
        Call parent(mesh, "GetParentBone");
        if (!parent.Set("BoneName", bone).Invoke()) break;
        bone = parent.Get<ue::Name>("ReturnValue");
    }
    return {};
}
bool SocketLocation(Object* mesh, ue::Name socket, Vector& at) {
    Call get(mesh, "GetSocketLocation");
    if (!get.Set("InSocketName", socket).Invoke()) return false;
    at = get.Get<Vector>("ReturnValue");
    return std::isfinite(at.x) && std::isfinite(at.y) && std::isfinite(at.z);
}
// With an NPC skin the player is drawn by another mesh, whose hands are not where the proxy's (or the hidden player's)
// are; without one (the hero's own body: 不变身, a boss no look plays) the player's arms keep their own lengths, so the
// pose proxy's hand (the Boss's proportions at the player's height) is not where the player's fist is either. While the
// Boss holds the weapon (its socket within kHoldEnterCm of the hand bone; let go past kHoldLeaveCm) a stand-in hangs
// on the visible mesh's own hand bone with the Boss's grip (the weapon relative to the proxy's hand, its world size
// kept): the engine carries it with that hand in the same frame, so it sits in the fist and turns with it. Moving it
// by the hand offset from our tick (which runs before the world's) was a frame late and kept the Boss's wrist turn: it
// floated off the fist and shook (11-08). A weapon the move lets go of (MoTian plants his in the ground for the throw)
// hangs on its parent's socket as attached, where the Boss's pose puts it. A switch keeps the weapon where it is and
// blends to its new place over kSwitchMs.
constexpr double kHoldEnterCm = 35, kHoldLeaveCm = 60;
constexpr ULONGLONG kSwitchMs = 150;
double Gap(const Vector& a, const Vector& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
double Gap(const Transform& a, const Transform& b) {
    return std::sqrt((a.translation[0] - b.translation[0]) * (a.translation[0] - b.translation[0]) + (a.translation[1] - b.translation[1]) * (a.translation[1] - b.translation[1]) +
                     (a.translation[2] - b.translation[2]) * (a.translation[2] - b.translation[2]));
}
// a * b (apply b, then a), as FQuat's operator*.
void QuatMul(const double a[4], const double b[4], double out[4]) {
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}
double Safe(double v) { return std::fabs(v) > 1e-4 ? v : 1; }
bool SocketWorld(Object* mesh, ue::Name socket, Transform& out) {
    Call get(mesh, "GetSocketTransform");
    if (!get.Set("InSocketName", socket).Set("TransformSpace", uint8_t{0}).Invoke()) return false;
    out = get.Get<Transform>("ReturnValue");
    return std::isfinite(Finite(out));
}
// The relative that puts `world` on a parent frame (UE: world = relative * parent), per-axis scale.
Transform RelativeTo(const Transform& world, const Transform& parent) {
    Transform out; const double inverse[4] = {-parent.rotation[0], -parent.rotation[1], -parent.rotation[2], parent.rotation[3]};
    QuatMul(inverse, world.rotation, out.rotation);
    const double offset[3] = {world.translation[0] - parent.translation[0], world.translation[1] - parent.translation[1], world.translation[2] - parent.translation[2]};
    double local[3]{}; Rotate(inverse, offset, local);
    for (int i = 0; i < 3; ++i) { out.translation[i] = local[i] / Safe(parent.scale[i]); out.scale[i] = world.scale[i] / Safe(parent.scale[i]); }
    out.translation[3] = out.scale[3] = 0;
    return out;
}
// The Boss's grip on a visible hand: the weapon's world (as the proxy holds it) relative to the proxy's hand, its
// offset and size kept in world units on the visible hand.
Transform GripOn(const Transform& weapon, const Transform& boss_hand, const Transform& hand) {
    Transform out; const double inverse[4] = {-boss_hand.rotation[0], -boss_hand.rotation[1], -boss_hand.rotation[2], boss_hand.rotation[3]};
    QuatMul(inverse, weapon.rotation, out.rotation);
    const double offset[3] = {weapon.translation[0] - boss_hand.translation[0], weapon.translation[1] - boss_hand.translation[1], weapon.translation[2] - boss_hand.translation[2]};
    double local[3]{}; Rotate(inverse, offset, local);
    for (int i = 0; i < 3; ++i) { out.translation[i] = local[i] / Safe(hand.scale[i]); out.scale[i] = weapon.scale[i] / Safe(hand.scale[i]); }
    out.translation[3] = out.scale[3] = 0;
    return out;
}
Transform Blend(const Transform& a, const Transform& b, double t) {
    Transform out; const double dot = a.rotation[0] * b.rotation[0] + a.rotation[1] * b.rotation[1] + a.rotation[2] * b.rotation[2] + a.rotation[3] * b.rotation[3];
    const double sign = dot < 0 ? -1 : 1; double length = 0;
    for (int i = 0; i < 4; ++i) { out.rotation[i] = a.rotation[i] * (1 - t) + b.rotation[i] * sign * t; length += out.rotation[i] * out.rotation[i]; }
    length = std::sqrt(length) > 1e-9 ? std::sqrt(length) : 1;
    for (int i = 0; i < 4; ++i) out.rotation[i] /= length;
    for (int i = 0; i < 3; ++i) { out.translation[i] = a.translation[i] + (b.translation[i] - a.translation[i]) * t; out.scale[i] = a.scale[i] + (b.scale[i] - a.scale[i]) * t; }
    out.translation[3] = out.scale[3] = 0;
    return out;
}
// Hang the stand-in on (mesh, socket) where it is now; `from` is that place relative to the new frame.
bool Rehang(Display& d, Object* mesh, ue::Name socket, Object* holder) {
    Call world(d.component, "K2_GetComponentToWorld"); Transform frame{};
    if (!world.Invoke() || !SocketWorld(mesh, socket, frame)) return false;
    const Transform now = world.Get<Transform>("ReturnValue");
    Call attach(d.component, "K2_AttachToComponent");
    if (!attach.Set("Parent", mesh).Set("SocketName", socket).Set("LocationRule", uint8_t{1}).Set("RotationRule", uint8_t{1}).Set("ScaleRule", uint8_t{1})
             .Set("bWeldSimulatedBodies", false).Invoke() || !attach.Get<bool>("ReturnValue")) return false;
    d.from = RelativeTo(now, frame); d.switched = GetTickCount64(); d.last_switch = d.switched;
    d.holder = holder; d.holder_name = holder ? ue::NameOf(holder) : ue::Name{}; d.has_grip = false; d.has_smooth = false; d.direct = false;
    ++session_.hold_switches;
    return true;
}
// The skeleton asset a skeletal mesh component draws with (nullptr: none / unreadable).
Object* SkeletonOf(Object* component) {
    if (!ue::Alive(component)) return nullptr;
    Call get(component, "GetSkeletalMeshAsset"); Object* asset = get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    Object* skeleton = nullptr; if (ue::Alive(asset)) ReadField(asset, "Skeleton", skeleton);
    return ue::Alive(skeleton) ? skeleton : nullptr;
}
double Seconds() { LARGE_INTEGER hz{}, t{}; QueryPerformanceFrequency(&hz); QueryPerformanceCounter(&t); return static_cast<double>(t.QuadPart) / static_cast<double>(hz.QuadPart); }
// The grip on the hero's hand is low-passed (kGripTauS) so the proxy's sampling (a frame behind the fist, at a montage
// time predicted from the last step) does not shake it; a jump past kGripSnapCm (a new grip) is taken at once. A hand /
// Boss-pose switch waits kSwitchDwellMs after the last one, so the weapon at the hold threshold does not flip each tick.
constexpr double kGripTauS = .035, kGripSnapCm = 25;
constexpr ULONGLONG kSwitchDwellMs = 250;
// The look bones a Boss look's own weapon sockets hang on (weapon_* only: a scabbard rides the body as the look poses it).
std::vector<std::string> LookWeaponBones(Object* skin) {
    std::vector<std::string> names;
    for (const Display& d : session_.displays) {
        if (d.boss_socket.empty() || !Same(d.component, d.name)) continue;
        const ue::Name socket = Socket(skin, d.boss_socket.c_str()); if (socket.None()) continue;
        ue::Name bone = socket; Call socket_bone(skin, "GetSocketBoneName");
        if (socket_bone.Set("InSocketName", socket).Invoke() && !socket_bone.Get<ue::Name>("ReturnValue").None()) bone = socket_bone.Get<ue::Name>("ReturnValue");
        char text[96]{}; ue::NameText(bone, text, sizeof(text));
        if (_strnicmp(text, "weapon", 6) || std::find(names.begin(), names.end(), std::string(text)) != names.end()) continue;
        names.emplace_back(text);
    }
    return names;
}
// A Boss look drives its own weapon bones from the pose proxy (NpcRetarget::DriveGrabWeapons): while the Boss holds the
// weapon, the look's weapon bone keeps the Boss's grip on the look's own hand, in the look's own pose of the same frame;
// free, it goes where the proxy has it. Called each tick: the first call starts it, the next ones refresh the proxy ->
// look frame. 0: not applied (the stand-ins stay on the Boss pose's socket).
size_t DriveLookWeapons(Object* skin) {
    if (!skin_weapons_ || !skin || !session_.proxy || !Same(session_.proxy, session_.proxy_name)) return 0;
    if (session_.look_names.empty()) session_.look_names = LookWeaponBones(skin);
    if (session_.look_names.empty()) return 0;
    std::vector<const char*> list; for (const std::string& name : session_.look_names) list.push_back(name.c_str());
    return skin_weapons_(session_.proxy, list.data(), list.size());
}
// Per tick, a stand-in on a look-driven Boss look: how far it is from where the Boss pose has it (the proxy's socket with
// the same relative), and while the Boss holds it (that socket within kLookHeldCm of the proxy's hand) how far it moved in
// the look's fist since the tick before (0 = rigid in the hand, nothing to see shake).
constexpr double kLookHeldCm = 20;
void TrackLookWeapon(Display& d, Object* skin, Object* proxy) {
    Call world(d.component, "K2_GetComponentToWorld"); Transform boss_socket{}, boss_hand{}, look_hand{};
    if (!world.Invoke() || !SocketWorld(proxy, d.socket, boss_socket)) return;
    const Transform weapon = world.Get<Transform>("ReturnValue"), boss_weapon = Compose(d.attached, boss_socket);
    const double off = Gap(weapon, boss_weapon);
    ++session_.look_ticks; session_.look_boss_sum += off; session_.look_boss_max = (std::max)(session_.look_boss_max, off);
    const bool held = !d.hand.None() && SocketWorld(proxy, d.hand, boss_hand) && Gap(boss_socket, boss_hand) < kLookHeldCm && SocketWorld(skin, d.hand, look_hand);
    if (!held) { d.look_has_grip = false; return; }
    const Transform grip = RelativeTo(weapon, look_hand), boss_grip = RelativeTo(boss_weapon, boss_hand);
    if (d.look_has_grip) {
        const double step = Gap(grip, d.look_grip), boss_step = Gap(boss_grip, d.boss_grip);
        ++session_.look_held; session_.look_step_sum += step; session_.look_step_max = (std::max)(session_.look_step_max, step);
        session_.boss_step_sum += boss_step; session_.boss_step_max = (std::max)(session_.boss_step_max, boss_step);
    }
    d.look_grip = grip; d.boss_grip = boss_grip; d.look_has_grip = true;
}
void FollowSkinHands() {
    double size = 1;
    Object* skin = skin_display_ ? reinterpret_cast<Object*>(skin_display_(size)) : nullptr;
    if (!ue::Alive(skin)) skin = nullptr;
    Object* own = nullptr;
    if (!skin && session_.proxy && Same(session_.proxy, session_.proxy_name)) { ReadField(session_.pawn, "Mesh", own); if (!ue::Alive(own)) own = nullptr; }
    // A look on the Boss's own skeleton (11-15): the stand-in stays all grab where ShowWeapon hung it, on the pose proxy's
    // socket. The proxy is the Boss's own mesh playing the move in sync, so that socket is where the Boss's weapon actor
    // hangs, and the engine carries the weapon with it in the same frame; nothing is set per tick. The look's hand follows
    // that pose within a few cm (MoTian 02: 0.5-3.8 cm). Its own weapon bone does not: it is the hero's weapon_r retargeted
    // under a turned hand, 38-111 cm from the hand where the Boss's is 87-237 cm (the planted weapon of the throw), so the
    // weapon on it floated beside the hips (11-12..11-14). Moving the weapon between the look's hand, its socket and the
    // proxy each tick was a frame late and every switch a jump: the user saw it shake (11-15).
    Object* proxy = session_.proxy && Same(session_.proxy, session_.proxy_name) ? session_.proxy : nullptr;
    Object* skin_skeleton = skin ? SkeletonOf(skin) : nullptr;
    const bool boss_look = skin_skeleton && (!proxy || SkeletonOf(proxy) == skin_skeleton);
    const int mode = look_mode_.load(std::memory_order_relaxed);
    const ULONGLONG now = GetTickCount64();
    for (Display& d : session_.displays) {
        if (!Same(d.component, d.name)) continue;
        const bool on_proxy = proxy && d.parent == proxy && Same(d.parent, d.parent_name);
        // The look's own weapon bones follow the Boss pose (11-15, second fix): every stand-in hangs on the look's own socket
        // as the Boss's weapon actor hangs on the Boss, and nothing is set per tick once there. The weapon bone keeps the
        // Boss's grip on the look's own fist in the look's own pose of that frame, so it cannot lag or shake against it; on
        // the proxy's socket it was the proxy's fist, a few cm off the drawn one and sampled at a predicted time.
        if (boss_look && on_proxy && mode == 0 && session_.look_bones && !d.boss_socket.empty()) {
            const ue::Name own_socket = Socket(skin, d.boss_socket.c_str());
            if (!own_socket.None()) {
                if (!d.direct || d.holder != skin) {
                    if (!Rehang(d, skin, own_socket, skin)) continue;
                    d.direct = true;
                    if (session_.ticks < 2) { Call(d.component, "K2_SetRelativeTransform").Set("NewTransform", d.own_relative).Set("bSweep", false).Set("bTeleport", true).Invoke(); d.switched = 0; }
                    char socket_text[96]{}; ue::NameText(own_socket, socket_text, sizeof(socket_text));
                    Logf("GRAB_EXTRAS boss weapon on the look's own socket %s; the look's weapon bones follow the Boss pose (%zu)", socket_text, session_.look_bones);
                }
                if (d.switched) {
                    const double t = std::clamp(static_cast<double>(now - d.switched) / kSwitchMs, 0.0, 1.0);
                    Call(d.component, "K2_SetRelativeTransform").Set("NewTransform", t < 1 ? Blend(d.from, d.own_relative, t) : d.own_relative).Set("bSweep", false).Set("bTeleport", true).Invoke();
                    if (t >= 1) d.switched = 0;
                }
                if (session_.ticks >= 2) TrackLookWeapon(d, skin, proxy);
                continue;
            }
        }
        if (boss_look && on_proxy && (mode == 0 || mode == 3)) {
            if (d.holder || d.direct) { if (!Rehang(d, d.parent, d.socket, nullptr)) continue; }  // moved off by another mode: back
            if (d.switched) {
                const double t = std::clamp(static_cast<double>(now - d.switched) / kSwitchMs, 0.0, 1.0);
                Call(d.component, "K2_SetRelativeTransform").Set("NewTransform", t < 1 ? Blend(d.from, d.attached, t) : d.attached).Set("bSweep", false).Set("bTeleport", true).Invoke();
                if (t >= 1) d.switched = 0;
            }
            Transform look_hand{}, boss_hand{};
            if (session_.ticks >= 2 && !d.hand.None() && SocketWorld(skin, d.hand, look_hand) && SocketWorld(proxy, d.hand, boss_hand)) {
                const double gap = Gap(look_hand, boss_hand);
                if (!session_.pose_ticks) {
                    session_.pose_first = gap;
                    Logf("GRAB_EXTRAS Boss look: the weapon stays on the Boss pose's socket all grab (as the Boss's weapon actor); tick %d look hand %.1f cm off the Boss pose's", session_.ticks, gap);
                }
                ++session_.pose_ticks; session_.pose_hand_sum += gap; session_.pose_hand_max = (std::max)(session_.pose_hand_max, gap);
            }
            if (session_.ticks >= 2) TrackLookWeapon(d, skin, proxy);  // the same fist measure as the look-driven way (A/B)
            continue;
        }
        const ue::Name own_socket = boss_look && (mode == 1 || !on_proxy) && !d.boss_socket.empty() ? Socket(skin, d.boss_socket.c_str()) : ue::Name{};
        if (!own_socket.None()) {
            if (!d.direct || d.holder != skin) {
                if (!Rehang(d, skin, own_socket, skin)) continue;
                d.direct = true;
                char socket_text[96]{}; ue::NameText(own_socket, socket_text, sizeof(socket_text));
                Logf("GRAB_EXTRAS boss weapon on the look's own socket %s (the Boss's skeleton): as the Boss's weapon actor hangs, nothing set per tick", socket_text);
            }
            if (!d.switched) continue;
            const double t = std::clamp(static_cast<double>(now - d.switched) / kSwitchMs, 0.0, 1.0);
            Call(d.component, "K2_SetRelativeTransform").Set("NewTransform", t < 1 ? Blend(d.from, d.own_relative, t) : d.own_relative).Set("bSweep", false).Set("bTeleport", true).Invoke();
            if (t >= 1) d.switched = 0;
            continue;
        }
        if (d.hand.None() || !Same(d.parent, d.parent_name)) continue;
        const bool lost = d.holder && (!Same(d.holder, d.holder_name) || d.direct);  // the look it hung on is gone: back on the parent below
        if (lost) { d.holder = nullptr; d.direct = false; }
        Object* visible = skin ? skin : (own && d.parent == session_.proxy ? own : nullptr);
        Transform socket{}, boss_hand{}, hand{};
        const bool poses = SocketWorld(d.parent, d.socket, socket) && SocketWorld(d.parent, d.hand, boss_hand);
        const bool seen = visible && poses && SocketWorld(visible, d.hand, hand);
        if (seen && !d.follow_refused && Gap(hand, boss_hand) > kFollowMaxCm) {
            Logf("GRAB_EXTRAS skin hand %.1f cm from the stand-in's hand; it stays on the Boss pose", Gap(hand, boss_hand));
            d.follow_refused = true;
        }
        const double grip_gap = poses ? Gap(socket, boss_hand) : 1e9;
        bool held = seen && !d.follow_refused && grip_gap < (d.holder == visible ? kHoldLeaveCm : kHoldEnterCm);
        const bool settled = !d.last_switch || now >= d.last_switch + kSwitchDwellMs;
        if (!lost && !settled && seen && held != (d.holder == visible)) held = d.holder == visible;  // too soon after the last switch
        if (held && d.holder != visible) { if (!Rehang(d, visible, d.hand, visible)) continue; }
        else if (!held && (d.holder || lost)) { if (!Rehang(d, d.parent, d.socket, nullptr)) continue; }
        Transform target = d.attached;
        if (held) {
            target = GripOn(Compose(d.attached, socket), boss_hand, hand);
            const double clock = Seconds();
            if (d.has_smooth && Gap(target, d.smooth) < kGripSnapCm) {
                const double dt = std::clamp(clock - d.smooth_clock, 0.0, .1);
                target = Blend(d.smooth, target, 1 - std::exp(-dt / kGripTauS));
            }
            d.smooth = target; d.has_smooth = true; d.smooth_clock = clock;
            if (d.has_grip) {
                const double step = Gap(target, d.last_grip);
                ++session_.hold_frames; session_.hold_sum += step; session_.hold_max = (std::max)(session_.hold_max, step);
            }
            d.last_grip = target; d.has_grip = true;
        }
        const double t = d.switched ? std::clamp(static_cast<double>(now - d.switched) / kSwitchMs, 0.0, 1.0) : 1.0;
        if (t >= 1 && !held && !d.switched) continue;  // never moved: as attached
        const Transform place = t < 1 ? Blend(d.from, target, t) : target;
        if (t >= 1 && !held) d.switched = 0;  // back on the parent as attached: nothing more to set
        Call(d.component, "K2_SetRelativeTransform").Set("NewTransform", place).Set("bSweep", false).Set("bTeleport", true).Invoke();
    }
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
    const Transform own_relative = relative;
    const char* mode = "socket";
    Object* parent = mesh;
    ue::Name attach_to = socket;
    const ue::Name proxy_socket = session_.proxy ? Socket(session_.proxy, weapon.socket) : ue::Name{};
    if (!proxy_socket.None()) {
        // Exactly the Boss weapon actor's placement on the Boss skeleton; the proxy has the current
        // model's size, so the weapon is scaled with it (current model / boss).
        parent = session_.proxy; attach_to = proxy_socket; mode = "boss-pose";
    } else {
        attach_to = Grip(mesh, socket, weapon.rest, relative, mode);
        for (int i = 0; i < 3; ++i) relative.scale[i] *= session_.scale;
    }
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
    display.parent = parent; display.parent_name = ue::NameOf(parent); display.socket = attach_to;
    for (int i = 0; i < 3; ++i) display.base[i] = relative.translation[i];
    display.attached = relative;
    display.hand = HandBone(parent, attach_to);
    display.boss_socket = weapon.socket ? weapon.socket : ""; display.own_relative = own_relative; display.asset = asset; display.skeletal = is_skeletal;
    session_.displays.push_back(display);
    char socket_text[96]{}, attach_text[96]{}; ue::NameText(socket, socket_text, sizeof(socket_text)); ue::NameText(attach_to, attach_text, sizeof(attach_text));
    Logf("GRAB_EXTRAS boss weapon shown %s socket=%s attached=%s mode=%s %s scale=%.3f", weapon.class_path, socket_text, attach_text, mode, is_skeletal ? "skeletal" : "static", session_.scale);
    return true;
}
// The grab's own visibility actions (GAAction_ForceWeaponVisibility) call
// CharacterWeaponsComponent::SetArmed / SetWeaponForceHide on the executing
// character, i.e. on the player. SetWeaponForceHide (native 0x5e8f060 ->
// 0x5e95d60) appends the actor index to a TArray<int32> at +0x60 of the slot's
// current RuntimeWeaponSingleData (stride 0x120). Mirror that onto the stand-in.
// armed: the main slot drawn (true when unreadable); the return: no force-hide on it (true when unreadable).
bool WeaponShouldShow(Object* weapons, bool& armed_now) {
    static const ue::Name main_slot = ue::MakeName(L"Weapon.Slot.Main");
    armed_now = true;
    if (!ue::Alive(weapons) || main_slot.None()) return true;
    Call armed(weapons, "IsArmed");
    if (armed.Set("SlotTag", main_slot).Invoke() && !armed.Get<bool>("ReturnValue")) armed_now = false;
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
// The success timeline's Main-slot switches (GAA_SwitchWeapon), read from the move itself, not a boss-name list. One with
// Armed=false selects a weapon config without drawing it (a config says whether its pieces show undrawn:
// CharacterWeaponConfig::IsUnarmShown) and puts the Boss's weapon away for its window, when the Boss has that config
// (boss_configs: its Main configs; GrabWeaponVisibility::PutsAway). On the player the switch itself is refused, so the
// window and the Boss's configs, not the player's slot, say it. 沐小葵 01 switches to config 2 that way for 0.0001-4.5478 s,
// but the cloaked first phase it belongs to has two configs: natively nothing changes and she holds her sword all grab
// (user, 11-15: "沐小葵应该有一招投技是带武器的 查看一下带斗篷那个"); her montage moves weapon_r with hand_l to 1.33 s and
// hand_r after (offline ACL decode of MuXiaoKui_GEx_01). The weapon the user saw in 沐小葵's grabs (11-15: "沐小葵的投技还是
// 有武器") was the second phase's: that look fights barehanded (grab_weapons.generated.h lists no weapon for its grabs).
// Armed=true (独眼 01's bow) draws: shown.
void CaptureNativeVisibility(Object* ability, Object* timeline, size_t boss_configs) {
    if (!ue::Alive(ability) || !ue::Alive(timeline)) return;
    Object* event = ue::FindObject("/Script/LegoAbility.LGAActionEvent");
    Object* switch_class = ue::FindObject("/PBZCombatAsset/Prototypes/Actions/GAA_SwitchWeapon.GAA_SwitchWeapon_C");
    const int32_t stride = ue::StructSize(event);
    const int32_t start = ue::Offset(event, "StartTime"), duration = ue::Offset(event, "duration"),
        trigger = ue::Offset(event, "TriggerTimeOffset"), end_trigger = ue::Offset(event, "EndTriggerTimeOffset"),
        action_at = ue::Offset(event, "Action");
    if (!switch_class || stride <= 0 || stride > 1024 || start < 0 || duration < 0 || trigger < 0 || end_trigger < 0 || action_at < 0 ||
        start + 4 > stride || duration + 4 > stride || trigger + 4 > stride || end_trigger + 4 > stride || action_at + 8 > stride) return;
    ue::Array actions;
    if (!ReadField(timeline, "Actions", actions) || actions.num <= 0 || actions.num > 512 || actions.num > actions.max ||
        !Readable(actions.data, static_cast<size_t>(actions.num) * stride)) return;
    const ue::Name main_slot = ue::MakeName(L"Weapon.Slot.Main");
    for (int32_t i = 0; i < actions.num; ++i) {
        Object* row = static_cast<Object*>(actions.data) + static_cast<size_t>(i) * stride;
        Object* action = ue::At<Object*>(row, action_at); bool armed = true, reset = false; ue::Name slot; int32_t start_weapon = -1, end_weapon = -1;
        if (!ue::Alive(action) || !ue::IsA(action, switch_class) || !Bool(action, "Armed", armed) ||
            !ReadField(action, "SlotTag", slot) || !(slot == main_slot)) continue;
        Bool(action, "ResetOnEnd", reset); ReadField(action, "StartWeapon", start_weapon); ReadField(action, "EndWeapon", end_weapon);
        const float at = ue::At<float>(row, start), length = ue::At<float>(row, duration);
        GrabWeaponVisibility::Window window{at + ue::At<float>(row, trigger), at + length + ue::At<float>(row, end_trigger)};
        const bool away = GrabWeaponVisibility::PutsAway(armed, start_weapon, boss_configs) && window.Contains(window.begin);
        char note[128] = "";
        if (away) std::snprintf(note, sizeof(note), " (puts the weapon away: the stand-ins hide)");
        else if (!armed) std::snprintf(note, sizeof(note), " (the Boss has %zu configs: natively nothing changes, the weapon stays)", boss_configs);
        Logf("GRAB_EXTRAS native Main switch to %d armed=%d resetOnEnd=%d end=%d from %.4f to %.4f s%s", start_weapon, armed ? 1 : 0, reset ? 1 : 0, end_weapon,
             window.begin, window.end, note);
        if (away) session_.sheathe_windows.push_back(window);
    }
    session_.source_ability = ability; session_.source_name = ue::NameOf(ability);
    session_.source_timeline = timeline; session_.timeline_name = ue::NameOf(timeline);
    Logf("GRAB_EXTRAS native visibility windows=%zu", session_.sheathe_windows.size());
}
bool NativeSheathe(bool armed) {
    if (session_.sheathe_windows.empty()) return false;
    // Use the task's game clock so pause, rate changes and jumps stay in sync.
    // An unavailable clock keeps the last time until the grab's normal cleanup.
    if (Same(session_.source_ability, session_.source_name) && Same(session_.source_timeline, session_.timeline_name)) {
        ue::Array tasks;
        Object* task_class = ue::FindObject("/Script/LegoAbility.LGATask_PlayTimeline");
        if (task_class && ReadField(session_.source_ability, "ActiveTasks", tasks) && tasks.num > 0 && tasks.num <= 64 &&
            tasks.num <= tasks.max && Readable(tasks.data, static_cast<size_t>(tasks.num) * sizeof(Object*))) {
            for (int32_t i = 0; i < tasks.num; ++i) {
                Object* task = static_cast<Object**>(tasks.data)[i]; Object* timeline = nullptr; float time = 0;
                if (ue::Alive(task) && ue::IsA(task, task_class) && ReadField(task, "Timeline", timeline) && timeline == session_.source_timeline &&
                    ReadField(task, "CurrentTime", time) && std::isfinite(time) && time >= 0) { session_.visibility_time = time; break; }
            }
        }
    }
    // Do not hide when the native switch kept the slot armed (e.g. condition rejected).
    const bool authored_now=std::any_of(session_.sheathe_windows.begin(), session_.sheathe_windows.end(),
        [](const auto& window) { return window.Contains(session_.visibility_time); });
    // End can restore a cached weapon without drawing it. Stay sheathed until
    // the native slot is drawn again; don't flash a stand-in at the window end.
    return session_.native_visibility.Hidden(armed,authored_now);
}
void UpdateDisplays() {
    if (session_.displays.empty()) return;
    // The grab's own force-hides, and a sheathe during it; a sword already sheathed when the grab began (the grab from
    // idle) does not hide the boss's weapon: the Boss always grabs with his in hand (it hid the stand-in all along, 11-08).
    bool armed = true;
    const bool unhidden = WeaponShouldShow(session_.weapons_component, armed);
    if (!session_.armed_known) { session_.armed_known = true; session_.armed_at_begin = armed; }
    const bool native_sheathe = NativeSheathe(armed);
    const bool show = GrabWeaponVisibility::Show(unhidden, armed, session_.armed_at_begin, native_sheathe, session_.ticks);
    if (session_.shown_logged != (show ? 1 : 0)) {
        session_.shown_logged = show ? 1 : 0;
        Logf("GRAB_EXTRAS stand-ins %s (main slot drawn %d, drawn at the start %d, force-hidden %d, native-sheathe %d time %.4f, tick %d)", show ? "shown" : "hidden", armed ? 1 : 0,
             session_.armed_at_begin ? 1 : 0, unhidden ? 0 : 1, native_sheathe ? 1 : 0, session_.visibility_time, session_.ticks);
    }
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
constexpr uint32_t kHitLogs = 12;
// Health.CurrentValue of the target's attribute set, the value the grab's damage events lower.
bool BindHealth(Object* target) {
    Object* set = nullptr;
    if (!ReadField(target, "AttributeSetBase", set) || !ue::Alive(set)) return false;
    Object* field = Field(ue::ClassOf(set), "Health");
    Object* data = ue::FindObject("/Script/GameplayAbilities.GameplayAttributeData");
    if (!field || !data || ue::StructSize(data) != 16 || ue::Offset(data, "CurrentValue") != 12) return false;
    const int32_t offset = ue::At<int32_t>(field, static_cast<int32_t>(ue::layout::kPropOffset));
    if (offset < 0 || offset > 0x4000) return false;
    session_.health_set = set; session_.health_set_name = ue::NameOf(set); session_.health_offset = offset + 12;
    return true;
}
bool HealthNow(float& value) {
    if (session_.health_offset < 0 || !Same(session_.health_set, session_.health_set_name) ||
        !Readable(session_.health_set + session_.health_offset, sizeof(float))) return false;
    value = ue::At<float>(session_.health_set, session_.health_offset);
    return std::isfinite(value) && value >= 0 && value < 1e9f;
}
// Each of the grab's own damage events (a drop of the target's health while the executor's
// grab plays) is scaled the moment it lands, through the game's attribute call, so death and
// hit handling stay the game's own. A lethal extra kills right there, as a lethal native hit
// would. A change the target's attributes refuse is written directly only with "ignore
// invulnerability", and never below 1.
void ScaleHits() {
    float health = 0; if (!HealthNow(health)) return;
    const float drop = session_.last_hp - health; session_.last_hp = health;
    if (!(drop > .01f)) return;
    const Options& o = session_.options;
    session_.native += drop; ++session_.hits;
    float want = HitExtra(o.damage, drop, o.multiplier, o.percent, session_.hp_max);
    if (want > health) want = health;
    float done = 0; bool forced = false;
    if (std::fabs(want) > .01f) {
        float after = health;
        if (AddHealth(session_.target, -want) && HealthNow(after)) done = health - after;
        if (want > 0 && done < .5f && o.ignore_invulnerable && ForceHealth(session_.target, (std::max)(1.f, health - want)) && HealthNow(after)) {
            done = health - after; forced = true; ++session_.forced;
        }
        if (std::fabs(done) < .01f) ++session_.refused;
        session_.last_hp = after;
    }
    session_.extra += done;
    if (session_.hits <= kHitLogs)
        Logf("GRAB_EXTRAS hit %u native=%.1f extra=%.1f wanted=%.1f left=%.0f mode=%d x%.2f pct=%.0f%s", session_.hits, drop, done, want, session_.last_hp,
            static_cast<int>(o.damage), static_cast<double>(o.multiplier), static_cast<double>(o.percent), forced ? " forced" : "");
}
void ApplyDamage() {
    const Options& o = session_.options;
    if (!Same(session_.target, session_.target_name)) { Result(L"投技附加伤害：目标已不存在，未修改"); return; }
    ScaleHits(); // a hit in the frame the executor's grab ended
    float hp = 0, maximum = 0;
    if (session_.hp_begin < 0 || !ReadHealth(session_.target, hp, maximum)) { Result(L"投技附加伤害：无法读取目标生命，未修改"); return; }
    if (maximum <= 0) maximum = session_.hp_max;
    if (session_.health_offset < 0) {
        Result(L"投技附加伤害：无法逐段读取目标生命，未修改");
        Logf("GRAB_EXTRAS damage per-hit health unavailable; native total=%.1f, nothing added", (std::max)(0.f, session_.hp_begin - hp)); return;
    }
    const float native = session_.native;
    if (native > .01f && !session_.move.empty()) SettingsStore::PutFloat(kDamageSection, session_.move.c_str(), native);
    if (native > .01f) {
        if (o.damage == Damage::Native) Result(L"投技伤害：原生 %.0f", native);
        else Result(L"投技伤害：原生 %.0f → 合计 %.0f（生命 %.0f → %.0f，上限 %.0f，共 %u 段命中）", native, native + session_.extra, session_.hp_begin, hp, maximum, session_.hits);
        Logf("GRAB_EXTRAS damage mode=%d per-hit hits=%u native=%.1f extra=%.1f total=%.1f refused=%u forced=%u hp=%.1f->%.1f max=%.1f x%.2f pct=%.0f ignore_invulnerable=%d",
            static_cast<int>(o.damage), session_.hits, native, session_.extra, native + session_.extra, session_.refused, session_.forced, session_.hp_begin, hp, maximum,
            static_cast<double>(o.multiplier), static_cast<double>(o.percent), o.ignore_invulnerable ? 1 : 0);
        return;
    }
    // No damage event reached the target (a boss victim side usually deals none, or the target is
    // invulnerable). Only "ignore invulnerability" deals the move's damage, directly and once.
    if (!o.ignore_invulnerable) {
        Result(L"投技伤害：目标没有受到投技的伤害事件，未追加（开启“无视无敌”才直接扣血）");
        Logf("GRAB_EXTRAS damage mode=%d no damage event reached the target; ignore_invulnerable=0, nothing added", static_cast<int>(o.damage)); return;
    }
    if (hp <= 0) { Result(L"投技附加伤害：目标已倒下，未追加"); return; }
    float base = session_.move.empty() ? -1.f : SettingsStore::GetFloat(kDamageSection, session_.move.c_str(), -1.f);
    if (!std::isfinite(base) || base <= .01f) base = maximum > 0 ? maximum * kFallbackShare : 0.f;
    const float wanted = (std::min)(hp, base + HitExtra(o.damage, base, o.multiplier, o.percent, maximum));
    if (wanted < .01f) { Result(L"投技附加伤害：没有可用的伤害基数，未追加"); return; }
    if (!AddHealth(session_.target, -wanted)) { Result(L"投技附加伤害：游戏属性接口调用失败，未修改"); return; }
    float after = 0, ignored = 0; bool read = ReadHealth(session_.target, after, ignored);
    if (read && std::fabs(after - hp) < .5f) {
        const float forced = (std::max)(1.f, hp - wanted);
        const bool written = ForceHealth(session_.target, forced);
        read = ReadHealth(session_.target, after, ignored);
        Logf("GRAB_EXTRAS damage refused by target attributes; forced=%d %.1f->%.1f readback=%.1f", written ? 1 : 0, hp, forced, read ? after : -1.f);
    }
    Result(L"投技伤害（无视无敌）：直接扣除 %.0f（生命 %.0f → %.0f，上限 %.0f）", read ? hp - after : wanted, hp, read ? after : hp - wanted, maximum);
    Logf("GRAB_EXTRAS damage mode=%d direct base=%.1f wanted=%.1f hp=%.1f->%.1f max=%.1f ignore_invulnerable=1 readback=%d",
        static_cast<int>(o.damage), base, wanted, hp, read ? after : -1.f, maximum, read ? 1 : 0);
}
Options Sanitize(Options o) {
    o.multiplier = std::isfinite(o.multiplier) ? std::clamp(o.multiplier, .1f, kMaxMultiplier) : 2.f;
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
namespace {
struct ChainDamage { bool on = false; Damage mode = Damage::Native; float value = 0; ULONGLONG until = 0; };
ChainDamage chain_damage_;  // game thread
}
void SetChainDamage(Damage mode, float value, unsigned long long until_ms) { chain_damage_ = {true, mode, value, until_ms}; }
void ClearChainDamage() { chain_damage_ = {}; }
std::wstring LastResult() { AcquireSRWLockShared(&lock_); std::wstring text = result_; ReleaseSRWLockShared(&lock_); return text; }
void Begin(unsigned char* pawn, unsigned char* target, const Weapon* weapons, size_t weapon_count, const char* move, double hero_over_boss, unsigned char* source_ability, unsigned char* source_timeline,
           size_t boss_weapon_configs) {
    if (session_.active) Finish(false);
    session_ = {};
    if (!ue::Alive(pawn) || !ue::Alive(target)) return;
    session_.active = true; session_.options = GetOptions();
    if (chain_damage_.on && GetTickCount64() <= chain_damage_.until) {  // the chain's 投技伤害
        Options o = session_.options; o.damage = chain_damage_.mode;
        if (o.damage == Damage::Multiplier) o.multiplier = chain_damage_.value; else if (o.damage == Damage::Percent) o.percent = chain_damage_.value;
        session_.options = Sanitize(o);
        Logf("GRAB_EXTRAS chain damage: mode=%d value=%.2f (the combo page's, not the grab page's)", static_cast<int>(session_.options.damage), static_cast<double>(chain_damage_.value));
    }
    chain_damage_ = {};
    for (const char* c = move; c && *c; ++c) session_.move.push_back(static_cast<wchar_t>(static_cast<unsigned char>(*c)));
    session_.pawn = pawn; session_.pawn_name = ue::NameOf(pawn);
    session_.target = target; session_.target_name = ue::NameOf(target);
    session_.began = GetTickCount64(); session_.grab_montage = CurrentMontage(pawn);
    CaptureNativeVisibility(source_ability, source_timeline, boss_weapon_configs);
    double size = 1; if (skin_display_ && !ue::Alive(reinterpret_cast<Object*>(skin_display_(size)))) size = 1;
    if (!std::isfinite(size) || size < .1 || size > 10) size = 1;
    if (!std::isfinite(hero_over_boss) || hero_over_boss < .1 || hero_over_boss > 10) hero_over_boss = 1;
    session_.model = size; session_.scale = size * hero_over_boss;
    Logf("GRAB_EXTRAS grab size: model %.3f x hero/boss %.3f = boss weapon scale %.3f (victim as authored)", size, hero_over_boss, session_.scale);
    float hp = 0, maximum = 0;
    if (ReadHealth(target, hp, maximum)) { session_.hp_begin = hp; session_.hp_max = maximum; }
    if (!BindHealth(target) || !HealthNow(session_.last_hp)) { session_.health_offset = -1; Logf("GRAB_EXTRAS per-hit health unavailable; damage events are not scaled"); }
    if (session_.options.hide_weapon) {
        Call holder(pawn, "GetCharacterWeaponsComponent", "/Script/SGameCombatSystem.CharacterWeaponsHolder");
        if (holder.Invoke()) session_.weapons_component = holder.Get<Object*>("ReturnValue");
        HideWeapons();
        for (size_t i = 0; weapons && i < weapon_count && i < kMaxWeapons; ++i) if (weapons[i].proxy_mesh) {
            Object* mesh = nullptr; ReadField(pawn, "Mesh", mesh);
            if (CreateProxy(pawn, mesh, weapons[i].proxy_mesh, (weapons[i].proxy_scale > .05 ? weapons[i].proxy_scale : 1.0) * session_.model)) { SyncProxy(); break; }
        }
        for (size_t i = 0; weapons && i < weapon_count && i < kMaxWeapons; ++i) ShowWeapon(pawn, weapons[i]);
        double look_size = 1; Object* skin = skin_display_ ? reinterpret_cast<Object*>(skin_display_(look_size)) : nullptr;
        if (ue::Alive(skin) && session_.proxy && look_mode_.load(std::memory_order_relaxed) == 0 && SkeletonOf(skin) && SkeletonOf(skin) == SkeletonOf(session_.proxy)) {
            session_.look_bones = DriveLookWeapons(skin);
            Logf("GRAB_EXTRAS Boss look: %zu weapon bones of the look follow the Boss pose (%s)", session_.look_bones,
                 session_.look_bones ? "the stand-ins hang on the look's own sockets" : "not applied: the stand-ins stay on the Boss pose's socket");
        }
        UpdateDisplays(); FollowSkinHands();
    }
    session_.next_scan = GetTickCount64() + 100;
    Logf("GRAB_EXTRAS begin hide_weapon=%d own_weapons=%zu boss_weapons=%zu/%zu target_hp=%.1f/%.1f damage_mode=%d x%.2f pct=%.0f ignore_invulnerable=%d perHit=%d",
        session_.options.hide_weapon ? 1 : 0, session_.hidden.size(), session_.displays.size(), weapon_count, session_.hp_begin, session_.hp_max,
        static_cast<int>(session_.options.damage), static_cast<double>(session_.options.multiplier), static_cast<double>(session_.options.percent),
        session_.options.ignore_invulnerable ? 1 : 0, session_.health_offset >= 0 ? 1 : 0);
}
double Distance(const Vector& a, const Vector& b) { return std::sqrt((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)); }
// Where each stand-in is and whether it draws: its place, size and visibility, the player's hand and the proxy's.
void ProbeDisplays() {
    Object* own = nullptr; ReadField(session_.pawn, "Mesh", own);
    Call actor(session_.pawn, "K2_GetActorLocation"); const Vector pawn = actor.Invoke() ? actor.Get<Vector>("ReturnValue") : Vector{};
    const ULONGLONG t = GetTickCount64() - session_.began;
    double size = 1;
    Object* skin = skin_display_ ? reinterpret_cast<Object*>(skin_display_(size)) : nullptr;
    if (!ue::Alive(skin)) skin = nullptr;
    for (const Display& d : session_.displays) {
        if (!Same(d.component, d.name)) { Logf("GRAB_EXTRAS probe t=%llu stand-in gone", t); continue; }
        Call at(d.component, "K2_GetComponentLocation"), scale(d.component, "K2_GetComponentScale"), shown(d.component, "IsVisible");
        const Vector w = at.Invoke() ? at.Get<Vector>("ReturnValue") : Vector{}, sc = scale.Invoke() ? scale.Get<Vector>("ReturnValue") : Vector{};
        const bool visible = shown.Invoke() && shown.Get<bool>("ReturnValue");
        bool hidden = false; Bool(d.component, "bHiddenInGame", hidden);
        Vector hand{}, proxy_hand{}, boss_socket{}, look_socket{};
        const bool has_hand = ue::Alive(own) && !d.hand.None() && SocketLocation(own, d.hand, hand);
        const bool has_proxy = Same(d.parent, d.parent_name) && !d.hand.None() && SocketLocation(d.parent, d.hand, proxy_hand);
        // Where the Boss pose puts the weapon (the proxy's socket), and the look's own socket against it.
        const bool has_socket = Same(d.parent, d.parent_name) && !d.socket.None() && SocketLocation(d.parent, d.socket, boss_socket);
        const ue::Name own_socket = skin && !d.boss_socket.empty() ? Socket(skin, d.boss_socket.c_str()) : ue::Name{};
        const bool has_look = !own_socket.None() && SocketLocation(skin, own_socket, look_socket);
        Object* attached = nullptr; ReadField(d.component, "AttachParent", attached);
        char hand_name[64]{}; ue::NameText(d.hand, hand_name, sizeof(hand_name));
        const char* on = d.direct ? "look socket" : attached == d.parent ? "boss pose" : d.holder && attached == d.holder ? "hand" : "other";
        Logf("GRAB_EXTRAS probe t=%llu visible=%d hiddenInGame=%d on=%s weapon=(%.0f,%.0f,%.0f) scale=%.2f %s: to the player's hand %.1f cm, to the proxy's %.1f cm, "
             "proxy hand to the player's %.1f cm; to the Boss pose's socket %.1f cm, look socket off it %.1f cm; pawn=(%.0f,%.0f,%.0f)",
             t, visible ? 1 : 0, hidden ? 1 : 0, on, w.x, w.y, w.z, sc.x, hand_name, has_hand ? Distance(w, hand) : -1.0, has_proxy ? Distance(w, proxy_hand) : -1.0,
             has_hand && has_proxy ? Distance(hand, proxy_hand) : -1.0, has_socket ? Distance(w, boss_socket) : -1.0,
             has_socket && has_look ? Distance(look_socket, boss_socket) : -1.0, pawn.x, pawn.y, pawn.z);
        // Each pose's socket against its own hand: the Boss pose holds the weapon where its montage puts it; a look whose
        // socket stays far from its own hand while the proxy's is close does not move that bone.
        Vector look_hand{};
        const bool has_look_hand = skin && !d.hand.None() && SocketLocation(skin, d.hand, look_hand);
        Logf("GRAB_EXTRAS probe t=%llu socket to its own hand: Boss pose %.1f cm, look %.1f cm; look hand to the proxy's %.1f cm, to the player's %.1f cm", t,
             has_socket && has_proxy ? Distance(boss_socket, proxy_hand) : -1.0, has_look && has_look_hand ? Distance(look_socket, look_hand) : -1.0,
             has_look_hand && has_proxy ? Distance(look_hand, proxy_hand) : -1.0, has_look_hand && has_hand ? Distance(look_hand, hand) : -1.0);
    }
}
void Tick() {
    if (!session_.active || !Same(session_.pawn, session_.pawn_name)) return;
    if (session_.ticks < 1000) ++session_.ticks;
    if (GetTickCount64() >= session_.probe_at) { session_.probe_at = GetTickCount64() + 500; ProbeDisplays(); }
    if (!session_.damage_done && Same(session_.target, session_.target_name)) ScaleHits();
    TrackPairDrift();
    UpdatePosing();
    if (!session_.options.hide_weapon) return;
    // Cheap re-check of known weapons every tick; full re-scan for newly attached weapons a few times per second.
    HoldWeapons();
    if (GetTickCount64() >= session_.next_scan) { session_.next_scan = GetTickCount64() + 250; HideWeapons(); }
    SyncProxy();
    if (session_.look_bones) {  // refresh the proxy -> look frame; a look that went away (or changed) stops it
        double look_size = 1; Object* skin = skin_display_ ? reinterpret_cast<Object*>(skin_display_(look_size)) : nullptr;
        if (!ue::Alive(skin) || !DriveLookWeapons(skin)) { StopLookWeapons(); Logf("GRAB_EXTRAS Boss look: the look's weapon bones stopped following the Boss pose (look gone)"); }
    }
    UpdateDisplays();
    FollowSkinHands();
}
void Finish(bool completed) {
    if (!session_.active) return;
    Logf("GRAB_EXTRAS finish completed=%d damage_done=%d", completed ? 1 : 0, session_.damage_done ? 1 : 0);
    if (session_.hold_frames || session_.hold_switches)
        Logf("GRAB_EXTRAS fist hold frames=%u: the grip moved mean=%.2f max=%.2f cm a tick in the hand; switches hand/boss pose=%u", session_.hold_frames,
             session_.hold_frames ? session_.hold_sum / session_.hold_frames : 0.0, session_.hold_max, session_.hold_switches);
    if (session_.pose_ticks)
        Logf("GRAB_EXTRAS Boss look: the weapon stayed on the Boss pose's socket; the look's hand off the Boss pose's hand mean=%.1f max=%.1f cm over %u ticks (first %.1f cm)",
             session_.pose_hand_sum / session_.pose_ticks, session_.pose_hand_max, session_.pose_ticks, session_.pose_first);
    if (session_.look_ticks)
        Logf("GRAB_EXTRAS Boss look weapon (%s): in the look's fist the weapon moved mean=%.3f max=%.3f cm a tick (the Boss pose's own in its fist "
             "mean=%.3f max=%.3f) over %u held ticks; off the Boss pose's place mean=%.1f max=%.1f cm over %u ticks",
             session_.look_bones ? "the look's weapon bones follow the Boss pose" : "on the Boss pose's socket",
             session_.look_held ? session_.look_step_sum / session_.look_held : 0.0, session_.look_step_max,
             session_.look_held ? session_.boss_step_sum / session_.look_held : 0.0, session_.boss_step_max,
             session_.look_held, session_.look_boss_sum / session_.look_ticks, session_.look_boss_max, session_.look_ticks);
    if (session_.pair_on) { LogPairSegment("at the end"); session_.pair_on = false; }
    if (session_.sync_ticks)
        Logf("GRAB_EXTRAS proxy sync (%s): the proxy's montage time off the player's mean=%.2f max=%.2f ms over %u ticks", proxy_sync_.load(std::memory_order_relaxed) ? "same time and rate" : "one step ahead",
             session_.sync_sum / session_.sync_ticks, session_.sync_max, session_.sync_ticks);
    StopLookWeapons();
    if (Same(session_.pawn, session_.pawn_name)) { RemoveDisplays(); RemoveProxy(); RestoreWeapons(); }
    else { session_.displays.clear(); session_.hidden.clear(); }
    if (completed && !session_.damage_done) ApplyDamage();
    session_ = {};
}
bool Active() { return session_.active; }
bool Posing() { return session_.active && !session_.pose_over; }
void SetSkinDisplay(SkinDisplay provider) { skin_display_ = provider; }
void SetSkinWeapons(SkinWeapons provider) { skin_weapons_ = provider; }
void SetLookSocketMode(int mode) {
    look_mode_.store(mode >= 1 && mode <= 3 ? mode : 0, std::memory_order_relaxed);
    Logf("GRAB_EXTRAS look socket mode %s", mode == 1 ? "on (the look's own socket with its own weapon bone, as 11-12)" : mode == 2 ? "off (the look's hand while held, as on the hero's body)"
         : mode == 3 ? "proxy (the Boss pose's socket all grab, the first 11-15 fix)" : "auto (the look's weapon bones follow the Boss pose; on its own socket)");
}
void SetWeaponHideMode(int mode) {
    weapon_hide_.store(mode ? 1 : 0, std::memory_order_relaxed);
    Logf("GRAB_EXTRAS weapon hide %s", mode ? "parts (each part's own hidden flag; the actor's stays the game's; default)" : "actors (SetActorHiddenInGame, before 11-15)");
}
void SetProxySync(int mode) {
    proxy_sync_.store(mode ? 1 : 0, std::memory_order_relaxed);
    Logf("GRAB_EXTRAS proxy sync %s", mode ? "same time and rate (default)" : "one step ahead at rate 0 (11-08..11-15)");
}
bool ChangeHealth(unsigned char* character, float delta) {
    return ue::Alive(character) && std::isfinite(delta) && AttributeLayout() && AddHealth(character, delta);
}
bool CharacterHealth(unsigned char* character, float& health, float& maximum) {
    health = maximum = 0;
    return ue::Alive(character) && ReadHealth(reinterpret_cast<Object*>(character), health, maximum);
}
bool ChangeMaxHealth(unsigned char* character, float delta) {
    if (!ue::Alive(character) || !std::isfinite(delta) || !AttributeLayout()) return false;
    for (const wchar_t* name : {L"Health_Max", L"MaxHealth"}) {
        Attribute attribute; if (!MakeAttribute(name, attribute)) { FreeAttribute(attribute); continue; }
        Call add(ue::Default("/Script/SGameAttributes.Default__AttributeUtils"), "ModifyCharacterAttributeValue");
        const bool ok = add.Set("Character", character).Set("Attribute", attribute).Set("ModiferOp", uint8_t{0}).Set("Value", delta).Invoke();
        FreeAttribute(attribute); if (ok) return true;
    }
    return false;
}
void SourceEnded() {
    if (!session_.active || session_.damage_done) return;
    session_.damage_done = true; ApplyDamage();
}
bool HandOff(unsigned char* owner, unsigned char* body, HandOffInfo& out) {
    out = {};
    if (!session_.active || !ue::Alive(owner) || !ue::Alive(body)) return false;
    out.target = Same(session_.target, session_.target_name) ? session_.target : nullptr;
    // The boss weapons in the boss's hands: copies on the clone's own sockets (it plays the grab on the boss skeleton).
    Object* skeletal = ue::FindObject("/Script/Engine.SkeletalMeshComponent"), *static_type = ue::FindObject("/Script/Engine.StaticMeshComponent");
    for (const Display& display : session_.displays) {
        if (!Same(display.component, display.name) || !display.visible || !ue::Alive(display.asset) || display.boss_socket.empty()) continue;
        const ue::Name socket = Socket(body, display.boss_socket.c_str());
        if (socket.None()) { Logf("GRAB_EXTRAS hand-off weapon: no socket %s on the boss body", display.boss_socket.c_str()); continue; }
        Call add(owner, "AddComponentByClass");
        if (!add.Set("Class", display.skeletal ? skeletal : static_type).Set("bManualAttachment", true).Set("RelativeTransform", display.own_relative)
                 .Set("bDeferredFinish", true).Invoke()) continue;
        Object* component = add.Get<Object*>("ReturnValue");
        if (!ue::Alive(component)) continue;
        const bool mesh_set = display.skeletal ? Call(component, "SetSkeletalMeshAsset").Set("NewMesh", display.asset).Invoke()
                                               : Call(component, "SetStaticMesh").Set("NewMesh", display.asset).Invoke();
        Call(component, "SetCollisionEnabled").Set("NewType", uint8_t{0}).Invoke();
        Call attach(component, "K2_AttachToComponent");
        const bool attached = attach.Set("Parent", body).Set("SocketName", socket).Set("LocationRule", uint8_t{0}).Set("RotationRule", uint8_t{0})
            .Set("ScaleRule", uint8_t{0}).Set("bWeldSimulatedBodies", false).Invoke() && attach.Get<bool>("ReturnValue");
        Call(owner, "FinishAddComponent").Set("Component", component).Set("bManualAttachment", true).Set("RelativeTransform", display.own_relative).Invoke();
        if (!mesh_set || !attached) { Call(component, "K2_DestroyComponent").Set("Object", component).Invoke(); continue; }
        Call count(display.component, "GetNumMaterials");
        const int32_t materials = count.Invoke() ? count.Get<int32_t>("ReturnValue") : 0;
        for (int32_t i = 0; i < materials && i < 64; ++i) {
            Call get(display.component, "GetMaterial");
            if (get.Set("ElementIndex", i).Invoke()) if (Object* material = get.Get<Object*>("ReturnValue")) Call(component, "SetMaterial").Set("ElementIndex", i).Set("Material", material).Invoke();
        }
        ++out.weapons;
    }
    // What is left of the damage: the hits so far are counted, the rest goes with the clone.
    if (!session_.damage_done && Same(session_.target, session_.target_name)) ScaleHits();
    session_.damage_done = true;
    Owed owed; owed.id = ++owed_serial_; owed.target = session_.target; owed.target_name = session_.target_name; owed.move = session_.move;
    owed.native = session_.native; owed.hp_max = session_.hp_max; owed.hits = session_.hits; owed.options = session_.options;
    owed_.push_back(owed); out.damage = owed.id;
    if (owed_.size() > 8) owed_.erase(owed_.begin());
    Logf("GRAB_EXTRAS hand-off to the boss body: weapons=%zu/%zu damage so far native=%.1f hits=%u (the rest when the body's grab ends)",
        out.weapons, session_.displays.size(), session_.native, session_.hits);
    return true;
}
void HandOffEnd(int32_t damage, bool completed) {
    const auto found = std::find_if(owed_.begin(), owed_.end(), [&](const Owed& o) { return o.id == damage; });
    if (found == owed_.end()) return;
    const Owed o = *found; owed_.erase(found);
    if (!completed) { Logf("GRAB_EXTRAS hand-off %d dropped: the boss body was removed before its grab ended; nothing added", o.id); return; }
    if (!Same(o.target, o.target_name)) { Result(L"投技分身：目标已不存在，未追加伤害"); return; }
    float hp = 0, maximum = 0;
    if (!ReadHealth(o.target, hp, maximum)) { Result(L"投技分身：无法读取目标生命，未追加伤害"); return; }
    if (hp <= 0) { Result(L"投技分身：目标已倒下"); return; }
    if (maximum <= 0) maximum = o.hp_max;
    const float recorded = o.move.empty() ? -1.f : SettingsStore::GetFloat(kDamageSection, o.move.c_str(), -1.f);
    float base = 0; const char* from = "none";
    if (std::isfinite(recorded) && recorded > .01f) { base = (std::max)(0.f, recorded - o.native); from = "recorded"; }
    else if (o.native < .01f && o.options.ignore_invulnerable && maximum > 0) { base = maximum * kFallbackShare; from = "fallback"; }
    if (base < .01f) {
        Result(L"投技分身：这招没有剩余伤害的记录，未追加");
        Logf("GRAB_EXTRAS hand-off %d damage: none left (recorded=%.1f native before the cut=%.1f ignore_invulnerable=%d)", o.id, recorded, o.native, o.options.ignore_invulnerable ? 1 : 0);
        return;
    }
    const float wanted = (std::min)(hp, base + HitExtra(o.options.damage, base, o.options.multiplier, o.options.percent, maximum));
    if (!AddHealth(o.target, -wanted)) { Result(L"投技分身：游戏属性接口调用失败，未修改"); return; }
    float after = 0, ignored = 0; bool read = ReadHealth(o.target, after, ignored);
    if (read && std::fabs(after - hp) < .5f && o.options.ignore_invulnerable) {
        const float forced = (std::max)(1.f, hp - wanted);
        ForceHealth(o.target, forced); read = ReadHealth(o.target, after, ignored);
    }
    Result(L"投技分身：补上这招剩下的伤害 %.0f（生命 %.0f → %.0f）", read ? hp - after : wanted, hp, read ? after : hp - wanted);
    Logf("GRAB_EXTRAS hand-off %d damage base=%.1f (%s) wanted=%.1f hp=%.1f->%.1f max=%.1f mode=%d", o.id, base, from, wanted, hp, read ? after : -1.f, maximum,
        static_cast<int>(o.options.damage));
}
}
