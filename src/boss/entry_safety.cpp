#include "entry_safety.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace EntrySafety {
namespace {
using ue::Object; using ue::Call; using ue::Name; using ue::Array;
struct Vector { double x = 0, y = 0, z = 0; };
struct Rotation { double pitch = 0, yaw = 0, roll = 0; };
struct Color { float r = 0, g = 0, b = 0, a = 0; };
struct Hit { unsigned char bytes[256]{}; };
struct Floor { unsigned char bytes[272]{}; };
struct Identity { Object* object = nullptr; Name name; };
struct Hold {
    Identity pawn, movement;
    uint8_t mode = 0, custom = 0;
    bool safe = false;
    Vector safe_location;
};
static_assert(sizeof(Vector) == 24 && sizeof(Rotation) == 24 && sizeof(Hit) == 256 && sizeof(Floor) == 272);
constexpr double kPi = 3.14159265358979323846;
Logger logger_ = nullptr;
Hold hold_;
std::vector<Hold> retired_;
volatile LONG held_snapshot_ = 0, safe_snapshot_ = 0;
ULONGLONG last_failure_ = 0;

void Log(const char* format, ...) {
    if (!logger_) return;
    char message[768]{}; va_list args; va_start(args, format); vsprintf_s(message, format, args); va_end(args); logger_(message);
}
bool Fail(const char* text) {
    const ULONGLONG now = GetTickCount64();
    if (now >= last_failure_ + 1000) { Log("ENTRY_SAFETY refused: %s; pawn remains held", text); last_failure_ = now; }
    return false;
}
bool Same(Identity value) { return ue::Alive(value.object) && ue::NameOf(value.object) == value.name; }
Identity Id(Object* object) { return {object, object ? ue::NameOf(object) : Name{}}; }
void Publish() {
    InterlockedExchange(&held_snapshot_, hold_.pawn.object ? 1 : 0);
    InterlockedExchange(&safe_snapshot_, hold_.pawn.object && hold_.safe ? 1 : 0);
}
bool Finite(Vector p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
bool Finite(const Transform& transform) {
    double norm = 0;
    for (int i = 0; i < 4; ++i) { if (!std::isfinite(transform.rotation[i])) return false; norm += transform.rotation[i] * transform.rotation[i]; }
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(transform.translation[i]) || !std::isfinite(transform.scale[i]) || transform.scale[i] <= 0 || transform.scale[i] > 1000) return false;
    return norm > .25 && norm < 4;
}
double Distance2(Vector a, Vector b) { const double x = a.x-b.x, y = a.y-b.y, z = a.z-b.z; return x*x+y*y+z*z; }
Object* Type(const char* path) { return ue::FindObject(path); }
Object* Library() { return ue::Default("/Script/Engine.Default__KismetSystemLibrary"); }
Object* Property(Object* object, const char* name) {
    if (!ue::Alive(object)) return nullptr;
    const int32_t offset = ue::Offset(ue::ClassOf(object), name);
    return offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
}
uintptr_t Field(Object* type, const char* name) {
    for (size_t guard = 0; type && guard < 64; ++guard, type = ue::At<Object*>(type, ue::layout::kSuper)) {
        uintptr_t field = ue::At<uintptr_t>(type, ue::layout::kChildProperties);
        for (size_t i = 0; field && i < 2048; ++i, field = ue::At<uintptr_t>(reinterpret_cast<void*>(field), ue::layout::kPropNext))
            if (ue::NameIs(ue::At<Name>(reinterpret_cast<void*>(field), ue::layout::kPropName), name)) return field;
    }
    return 0;
}
bool Bool(const void* data, Object* type, const char* name, bool& result) {
    const uintptr_t field = Field(type, name);
    if (!field || ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x70) != 1) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    const uint8_t extra = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x71), mask = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x73);
    if (offset < 0 || extra > 7 || !mask) return false;
    result = (ue::At<uint8_t>(const_cast<void*>(data), offset + extra) & mask) != 0; return true;
}
bool Number(const void* data, Object* type, const char* name, double& result) {
    const uintptr_t field = Field(type, name);
    if (!field) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset),
        size = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropElementSize);
    if (offset < 0) return false;
    if (size == 4) result = ue::At<float>(const_cast<void*>(data), offset);
    else if (size == 8) result = ue::At<double>(const_cast<void*>(data), offset);
    else return false;
    return std::isfinite(result);
}
bool ReadLocation(Object* actor, Vector& result) {
    Call get(actor, "K2_GetActorLocation");
    if (!get.Slot("ReturnValue", sizeof(result)) || !get.Invoke()) return false;
    result = get.Get<Vector>("ReturnValue"); return Finite(result);
}
bool ZeroMotion(Object* pawn, Object* movement) {
    const bool stopped = Call(movement, "StopMovementImmediately").Invoke() && Call(movement, "ClearAccumulatedForces").Invoke();
    // Clear held input too, without synthesizing any input or cancelling abilities.
    Call consume(pawn, "ConsumeMovementInputVector"); if (consume.Ok()) consume.Invoke();
    return stopped;
}
bool Freeze(Hold& value) {
    if (!Same(value.pawn) || !Same(value.movement)) return false;
    Object* movement = value.movement.object;
    const int32_t mode = ue::Offset(ue::ClassOf(movement), "MovementMode"), custom = ue::Offset(ue::ClassOf(movement), "CustomMovementMode");
    if (mode < 0 || custom < 0) return false;
    const uint8_t observed = ue::At<uint8_t>(movement, mode);
    // Initialization may replace an original MOVE_None before the next hold.
    if (value.mode == 0 && observed > 0 && observed <= 6) {
        value.mode = observed; value.custom = ue::At<uint8_t>(movement, custom);
    }
    if (!Call(movement, "SetMovementMode").Set("NewMovementMode", uint8_t{0}).Set("NewCustomMode", uint8_t{0}).Invoke()) return false;
    return ue::At<uint8_t>(movement, mode) == 0 && ZeroMotion(value.pawn.object, movement);
}
void Prune() {
    retired_.erase(std::remove_if(retired_.begin(), retired_.end(), [](const Hold& item) {
        return !Same(item.pawn) || !Same(item.movement);
    }), retired_.end()); // Never call a movement function on a collected owner.
    if (hold_.pawn.object && (!Same(hold_.pawn) || !Same(hold_.movement))) { hold_ = {}; Publish(); }
}
Object* WorldOf(Object* actor) {
    Object* level = Type("/Script/Engine.Level"), *world = Type("/Script/Engine.World");
    for (Object* outer = actor; outer; outer = ue::OuterOf(outer)) {
        if (!ue::Alive(outer)) return nullptr;
        if (level && ue::IsA(outer, level)) return Property(outer, "OwningWorld");
        if (world && ue::IsA(outer, world)) return outer;
    }
    return nullptr;
}
bool KillZ(Object* pawn, double& value) {
    Object* world = WorldOf(pawn), *level = Property(world, "PersistentLevel"), *settings_class = Type("/Script/Engine.WorldSettings");
    if (!world || !level || !settings_class) return false;
    bool found = false;
    ue::ForEachObject([&](Object* object) {
        if (!ue::IsTemplate(object) && ue::Alive(object) && ue::OuterOf(object) == level && ue::IsA(object, settings_class)) {
            found = Number(object, ue::ClassOf(object), "KillZ", value); return !found;
        }
        return true;
    });
    return found;
}
bool Capsule(Object* pawn, float& half_height, float& radius) {
    Object* capsule = Property(pawn, "CapsuleComponent");
    Call height(capsule, "GetScaledCapsuleHalfHeight"), width(capsule, "GetScaledCapsuleRadius");
    if (!height.Invoke() || !width.Invoke()) return false;
    half_height = height.Get<float>("ReturnValue"); radius = width.Get<float>("ReturnValue");
    return std::isfinite(half_height) && std::isfinite(radius) && radius >= 1 && radius <= 2000 && half_height >= radius && half_height <= 5000;
}
bool Layout() {
    Object* hit = Type("/Script/Engine.HitResult"), *floor = Type("/Script/Engine.FindFloorResult");
    return hit && floor && ue::StructSize(hit) == sizeof(Hit) && ue::StructSize(floor) == sizeof(Floor);
}
struct PawnList {
    Array array;
    ~PawnList() { ue::Free(array.data); }
    bool Load(Object* context, Object* pawn) {
        Call get(ue::Default("/Script/Engine.Default__GameplayStatics"), "GetAllActorsOfClass");
        if (!get.Set("WorldContextObject", context).Set("ActorClass", Type("/Script/Engine.Pawn")).Invoke()) return false;
        array = get.Get<Array>("OutActors");
        if (array.num < 1 || array.num > 8192 || array.max < array.num || !array.data) return false;
        bool has_self = false;
        for (int32_t i = 0; i < array.num; ++i) if (static_cast<Object**>(array.data)[i] == pawn) has_self = true;
        return has_self;
    }
};
bool BreakHit(const Hit& hit, bool& blocking, bool& penetration, Vector& point, Vector& normal, Object*& actor) {
    Call split(ue::Default("/Script/Engine.Default__GameplayStatics"), "BreakHitResult");
    // BreakHitResult belongs to GameplayStatics in this exact build.
    if (!split.Ok()) split = Call(Library(), "BreakHitResult");
    if (!split.Set("Hit", hit).Invoke()) return false;
    blocking = split.Get<bool>("bBlockingHit"); penetration = split.Get<bool>("bInitialOverlap");
    point = split.Get<Vector>("ImpactPoint"); normal = split.Get<Vector>("ImpactNormal"); actor = split.Get<Object*>("HitActor");
    return Finite(point) && Finite(normal);
}
bool TraceGround(Object* context, const PawnList& ignore, Vector start, Vector end, Hit& hit) {
    Call trace(Library(), "LineTraceSingle");
    if (!trace.Set("WorldContextObject", context).Set("Start", start).Set("End", end).Set("TraceChannel", uint8_t{0})
        .Set("bTraceComplex", false).Set("ActorsToIgnore", ignore.array).Set("DrawDebugType", uint8_t{0})
        .Set("bIgnoreSelf", true).Set("TraceColor", Color{}).Set("TraceHitColor", Color{}).Set("DrawTime", 0.f).Invoke() || !trace.Get<bool>("ReturnValue")) return false;
    hit = trace.Get<Hit>("OutHit"); return true;
}
bool ClearCapsule(Object* context, const PawnList& ignore, Vector center, float half_height, float radius) {
    Call trace(Library(), "CapsuleTraceSingle");
    // A zero-length sweep requests initial overlaps. Floor clearance is 4 cm.
    if (!trace.Set("WorldContextObject", context).Set("Start", center).Set("End", center)
        .Set("Radius", radius).Set("HalfHeight", half_height).Set("TraceChannel", uint8_t{0})
        .Set("bTraceComplex", false).Set("ActorsToIgnore", ignore.array).Set("DrawDebugType", uint8_t{0})
        .Set("bIgnoreSelf", true).Set("TraceColor", Color{}).Set("TraceHitColor", Color{}).Set("DrawTime", 0.f).Invoke()) return false;
    return !trace.Get<bool>("ReturnValue");
}
bool FloorAt(Object* movement, Vector location) {
    Call find(movement, "K2_FindFloor");
    if (!find.Slot("FloorResult", sizeof(Floor)) || !find.Set("CapsuleLocation", location).Invoke()) return false;
    const Floor floor = find.Get<Floor>("FloorResult");
    Object* type = Type("/Script/Engine.FindFloorResult");
    bool blocking = false, walkable = false;
    if (!Bool(floor.bytes, type, "bBlockingHit", blocking) || !Bool(floor.bytes, type, "bWalkableFloor", walkable) || !blocking || !walkable) return false;
    double distance = 0;
    if (!Number(floor.bytes, type, "FloorDist", distance) || distance < -1 || distance > 12) return false;
    const uintptr_t field = Field(type, "HitResult");
    if (!field || ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropElementSize) != sizeof(Hit)) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    if (offset < 0 || offset + sizeof(Hit) > sizeof(Floor)) return false;
    bool penetrating = false;
    return Bool(floor.bytes + offset, Type("/Script/Engine.HitResult"), "bStartPenetrating", penetrating) && !penetrating;
}
bool Retire() {
    if (!hold_.pawn.object) return true;
    Prune(); if (!hold_.pawn.object) return true;
    if (retired_.size() >= 32) return Fail("too many live retired owners; retaining current hold");
    if (!Freeze(hold_)) return Fail("cannot reconfirm old owner's frozen mode");
    Log("ENTRY_SAFETY old owner retired MOVE_None safe=%d; baseline retained until GC/reclaim", hold_.safe ? 1 : 0);
    retired_.push_back(hold_); hold_ = {}; Publish(); return true;
}
}

void Initialize(Logger logger) { logger_ = logger; }
bool HoldPawn(Object* pawn) {
    Prune();
    if (!ue::Alive(pawn)) return Fail("pawn is not live");
    Object* movement = Property(pawn, "CharacterMovement");
    if (!movement) movement = Property(pawn, "CharMoveComp");
    if (!ue::Alive(movement)) return Fail("movement component not initialized");
    if (Same(hold_.pawn) && hold_.pawn.object == pawn && Same(hold_.movement) && hold_.movement.object == movement)
        return Freeze(hold_); // Preserve original mode and successful placement proof.
    if (!Retire()) return false;
    const auto prior = std::find_if(retired_.begin(), retired_.end(), [&](const Hold& item) {
        return item.pawn.object == pawn && Same(item.pawn) && item.movement.object == movement && Same(item.movement);
    });
    if (prior != retired_.end()) { hold_ = *prior; hold_.safe = false; retired_.erase(prior); }
    else {
        const int32_t mode = ue::Offset(ue::ClassOf(movement), "MovementMode"), custom = ue::Offset(ue::ClassOf(movement), "CustomMovementMode");
        if (mode < 0 || custom < 0 || ue::At<uint8_t>(movement, mode) > 6) return Fail("movement mode layout invalid");
        hold_.pawn = Id(pawn); hold_.movement = Id(movement); hold_.mode = ue::At<uint8_t>(movement, mode); hold_.custom = ue::At<uint8_t>(movement, custom);
    }
    Publish();
    if (!Freeze(hold_)) return Fail("MOVE_None / stop / clear-forces failed");
    Log("ENTRY_SAFETY hold acquired original_mode=%u custom=%u", hold_.mode, hold_.custom); return true;
}
Transform ParkingTransform(const Transform& raw) {
    Transform result = raw;
    // Authored spawner scale belongs to the Boss, never to the player capsule.
    // Keep the live avatar's scale/orientation while parking at the target XY.
    if (Same(hold_.pawn)) {
        Call get(hold_.pawn.object, "GetTransform");
        if (get.Slot("ReturnValue", sizeof(Transform)) && get.Invoke()) {
            const Transform current = get.Get<Transform>("ReturnValue");
            if (Finite(current)) {
                std::memcpy(result.rotation, current.rotation, sizeof(result.rotation));
                std::memcpy(result.scale, current.scale, sizeof(result.scale));
            }
        }
    }
    result.translation[2] += 3000;
    double kill = 0;
    if (Same(hold_.pawn) && KillZ(hold_.pawn.object, kill)) result.translation[2] = (std::max)(result.translation[2], kill + 5000);
    return result;
}
bool MakeBossTransform(Object* cls, const Transform& raw, Transform& result) {
    if (!ue::Alive(cls) || !Finite(raw)) return Fail("invalid boss class / authored transform");
    Object* cdo = nullptr;
    ue::ForEachObject([&](Object* object) {
        if (ue::Alive(object) && ue::IsDefault(object) && ue::ClassOf(object) == cls) { cdo = object; return false; }
        return true;
    });
    Object* capsule = Property(cdo, "CapsuleComponent"); double half_height = 0;
    if (!capsule || !Number(capsule, ue::ClassOf(capsule), "CapsuleHalfHeight", half_height) || half_height < 1 || half_height > 5000)
        return Fail("boss CDO capsule half-height unavailable");
    result = raw; result.translation[2] += half_height * raw.scale[2];
    Log("ENTRY_SAFETY boss center Z %.3f -> %.3f (CDO half=%.3f scaleZ=%.3f)", raw.translation[2], result.translation[2], half_height, raw.scale[2]);
    return Finite(result);
}
bool PlacePlayer(Object* pawn, Object* game_instance, const Transform& raw, bool original) {
    if (!Finite(raw) || !ue::Alive(game_instance) || !HoldPawn(pawn) || !Layout()) return Fail("placement inputs / hit-floor layout invalid");
    hold_.safe = false; Publish();
    float height = 0, radius = 0;
    if (!Capsule(pawn, height, radius)) return Fail("scaled player capsule unavailable");
    PawnList ignore;
    if (!ignore.Load(game_instance, pawn)) return Fail("cannot enumerate all current-world pawns to ignore");
    const Vector anchor{raw.translation[0], raw.translation[1], raw.translation[2]};
    double kill = 0; const bool has_kill = KillZ(pawn, kill);
    const double expected_floor = original ? anchor.z - height : anchor.z;
    const double yaw = std::atan2(2*(raw.rotation[3]*raw.rotation[2] + raw.rotation[0]*raw.rotation[1]),
                                  1-2*(raw.rotation[1]*raw.rotation[1] + raw.rotation[2]*raw.rotation[2]));
    Rotation saved_rotation;
    if (original) {
        Call split(ue::Default("/Script/Engine.Default__KismetMathLibrary"), "BreakTransform");
        if (!split.Set("InTransform", raw).Invoke()) return Fail("cannot read captured original rotation");
        saved_rotation = split.Get<Rotation>("Rotation");
    }
    constexpr double angles[] = {kPi, .75*kPi, -.75*kPi, .5*kPi, -.5*kPi, .25*kPi, -.25*kPi, 0};
    std::vector<Vector> candidates;
    if (original) candidates.push_back(anchor);
    const std::array<double,3> radii = original ? std::array<double,3>{150,300,500} : std::array<double,3>{650,900,1250};
    for (double distance : radii) for (double angle : angles)
        candidates.push_back({anchor.x + std::cos(yaw + angle)*distance, anchor.y + std::sin(yaw + angle)*distance, anchor.z});
    double walkable = .70;
    Call slope(hold_.movement.object, "GetWalkableFloorZ");
    if (slope.Invoke()) {
        const float value = slope.Get<float>("ReturnValue");
        if (std::isfinite(value) && value >= 0 && value <= 1) walkable = (std::max)(walkable, static_cast<double>(value));
    }
    size_t tried = 0;
    for (const Vector& candidate : candidates) {
        ++tried; Hit hit;
        // Trace from below ordinary ceilings. A captured player transform is a
        // capsule center, so its original XY only needs a trace from that air.
        const double trace_top = original ? anchor.z + 20 : anchor.z + (std::max)(250., height*2. + radius + 50.);
        if (!TraceGround(game_instance, ignore, {candidate.x,candidate.y,trace_top}, {candidate.x,candidate.y,anchor.z-6000}, hit)) continue;
        bool blocking = false, penetrating = false; Vector point, normal; Object* surface = nullptr;
        if (!BreakHit(hit, blocking, penetrating, point, normal, surface) || !blocking || penetrating || normal.z < walkable ||
            std::fabs(point.z - expected_floor) > 600 || (has_kill && point.z <= kill + 100) ||
            (surface && ue::IsA(surface, Type("/Script/Engine.Pawn")))) continue;
        const double slope_clearance = radius * (1. / normal.z - 1.) + 4.;
        const Vector center{point.x,point.y,point.z + height + slope_clearance};
        if (!ClearCapsule(game_instance, ignore, center, height, radius)) continue;
        Rotation rotation = original ? saved_rotation : Rotation{0,std::atan2(anchor.y-center.y,anchor.x-center.x)*180/kPi,0};
        Call teleport(pawn, "K2_TeleportTo");
        if (!teleport.Set("DestLocation", center).Set("DestRotation", rotation).Invoke() || !teleport.Get<bool>("ReturnValue")) continue;
        Vector actual;
        if (!ReadLocation(pawn, actual) || Distance2(actual, center) > 150*150 ||
            (!original && std::hypot(actual.x-anchor.x,actual.y-anchor.y) < 600) ||
            !FloorAt(hold_.movement.object, actual) || !ClearCapsule(game_instance, ignore, actual, height, radius)) continue;
        if (!Freeze(hold_)) return Fail("cannot hold confirmed position");
        hold_.safe = true; hold_.safe_location = actual; Publish();
        Log("ENTRY_SAFETY placed original=%d candidate=%zu/%zu actual=(%.3f,%.3f,%.3f) boss_distance=%.3f floorZ=%.3f walkable=1",
            original ? 1 : 0, tried, candidates.size(), actual.x,actual.y,actual.z, std::hypot(actual.x-anchor.x,actual.y-anchor.y),point.z);
        return true;
    }
    Freeze(hold_); return Fail("no collision-free walkable candidate; no movement release");
}
bool ReleasePawn() {
    Prune();
    if (!Same(hold_.pawn) || !Same(hold_.movement) || !hold_.safe) return Fail("release requires a live held owner and verified safe floor");
    Vector actual;
    if (!ReadLocation(hold_.pawn.object, actual) || Distance2(actual, hold_.safe_location) > 30*30 || !FloorAt(hold_.movement.object, actual)) {
        hold_.safe = false; Publish(); Freeze(hold_); return Fail("owner moved or floor changed since placement");
    }
    if (!ZeroMotion(hold_.pawn.object, hold_.movement.object)) return Fail("cannot clear motion before release");
    const uint8_t mode = hold_.mode ? hold_.mode : uint8_t{1}, custom = hold_.mode ? hold_.custom : uint8_t{0};
    if (!Call(hold_.movement.object, "SetMovementMode").Set("NewMovementMode", mode).Set("NewCustomMode", custom).Invoke()) return Fail("restore movement mode failed");
    const int32_t offset = ue::Offset(ue::ClassOf(hold_.movement.object), "MovementMode");
    if (offset < 0 || ue::At<uint8_t>(hold_.movement.object, offset) != mode || !ZeroMotion(hold_.pawn.object, hold_.movement.object)) {
        Freeze(hold_); return Fail("movement restore readback failed");
    }
    Log("ENTRY_SAFETY safe release movement=%u (original=%u; zero input/velocity/forces)", mode,hold_.mode);
    hold_ = {}; Publish(); return true;
}
bool BeforeTravel() { return Retire(); }
bool HasHold() { return InterlockedCompareExchange(&held_snapshot_,0,0) != 0; }
bool IsSafe() { return InterlockedCompareExchange(&safe_snapshot_,0,0) != 0; }
}
