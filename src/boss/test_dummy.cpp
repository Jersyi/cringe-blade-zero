#include "test_dummy.h"
#include "npc_retarget.h"
#include "ue_runtime.h"
#include <windows.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>

namespace TestDummy {
namespace {
using ue::Object; using ue::Call; using ue::Name; using ue::Array;
struct Vector { double x, y, z; };
struct alignas(16) Transform { double rotation[4]{0,0,0,1}, translation[4]{}, scale[4]{1,1,1,0}; };
static_assert(sizeof(Transform) == 96, "PBZ native FTransform layout");
struct SoftClass { int32_t index, serial; Name package, asset; ue::String subpath; };
static_assert(sizeof(SoftClass) == 40, "FSoftObjectPath layout");
// Plain humanoid mobs, the first that loads: no shield, no 教学 script.
constexpr const char* kClasses[] = {
    "/PBZCombatAsset/ActorData/Mobs/003_MaleThug/BP_Monster_MaleThug_Club_Yellow.BP_Monster_MaleThug_Club_Yellow_C",
    "/PBZCombatAsset/ActorData/Mobs/003_MaleThug/BP_Monster_MaleThug_Sword_Blue.BP_Monster_MaleThug_Sword_Blue_C",
    "/PBZCombatAsset/ActorData/Mobs/MaleDoll/BP_Monster_MaleDoll_Weaponless.BP_Monster_MaleDoll_Weaponless_C"};
// The game's own AI.DisableAction + Health_Min lock. The tag alone does not stop a mob's SGameAI (round 29: the mob
// still walked up and attacked), so HoldAI stops the AI itself; the effect stays for its health lock.
constexpr const char* kEffect = "/PBZCombatAsset/Prototypes/GameplayEffects/GE_AIDisableActionAndLockhealth.GE_AIDisableActionAndLockhealth_C";
constexpr double kDistance = 350.0;  // cm in front of the hero (the page); tests may ask for another
volatile double distance_ = kDistance;
constexpr ULONGLONG kCheckMs = 250, kEffectWaitMs = 5000;
constexpr DWORD kMaxFaults = 3;

Logger logger_ = nullptr;
SRWLOCK lock_ = SRWLOCK_INIT;
Snapshot snapshot_;
volatile LONG request_ = 0;  // 1 spawn, 2 remove
Object* engine_ = nullptr;
Object* dummy_ = nullptr; Name dummy_name_{};
const char* class_used_ = nullptr;
bool effect_on_ = false, effect_failed_logged_ = false, refill_failed_logged_ = false, dead_logged_ = false;
bool ai_missing_logged_ = false, ai_stuck_logged_ = false;
ULONGLONG spawned_at_ = 0, next_check_ = 0;
uint32_t refills_ = 0, ai_stops_ = 0;
Object* ai_component_class_ = nullptr;
DWORD faults_ = 0; bool disabled_ = false;

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[640]{}; va_list arguments; va_start(arguments, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments); va_end(arguments);
    logger_(text);
}
void Status(const wchar_t* format, ...) {
    wchar_t text[320]{}; va_list arguments; va_start(arguments, format);
    _vsnwprintf_s(text, _countof(text), _TRUNCATE, format, arguments); va_end(arguments);
    AcquireSRWLockExclusive(&lock_); snapshot_.status = text; ReleaseSRWLockExclusive(&lock_);
}
bool Same() { return dummy_ && ue::Alive(dummy_) && ue::NameOf(dummy_) == dummy_name_; }
void Publish(bool busy = false) {
    const bool present = Same();
    AcquireSRWLockExclusive(&lock_); snapshot_.present = present; snapshot_.busy = busy; ReleaseSRWLockExclusive(&lock_);
}
std::string NameText(Object* object) { char text[160]{}; if (object) ue::NameText(ue::NameOf(object), text, sizeof(text)); return text; }
int32_t Offset(Object* object, const char* property) { return object ? ue::Offset(ue::ClassOf(object), property) : -1; }
Object* Property(Object* object, const char* property) {
    const int32_t offset = Offset(object, property);
    return offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
}
struct Context { Object* world = nullptr; Object* instance = nullptr; Object* pawn = nullptr; };
bool ContextNow(Context& context) {
    if (!engine_ || !ue::Alive(engine_)) engine_ = ue::FindEngine();
    context.world = Property(Property(engine_, "GameViewport"), "World");
    if (!context.world || !ue::Alive(context.world)) return false;
    context.instance = Property(context.world, "OwningGameInstance");
    if (!context.instance || !ue::Alive(context.instance)) return false;
    const int32_t players = Offset(context.instance, "LocalPlayers");
    if (players < 0) return false;
    const Array list = ue::At<Array>(context.instance, players);
    if (!list.data || list.num <= 0 || list.num > 8) return false;
    context.pawn = Property(Property(static_cast<Object**>(list.data)[0], "PlayerController"), "Pawn");
    return context.pawn && ue::Alive(context.pawn);
}
std::wstring Wide(const std::string& text) { return std::wstring(text.begin(), text.end()); }
Object* LoadClass(const char* object_path) {
    if (Object* cls = ue::FindObject(object_path)) return cls;
    const std::string full(object_path); const size_t dot = full.find('.');
    if (dot == std::string::npos) return nullptr;
    SoftClass soft{};
    const std::wstring package = Wide(full.substr(0, dot)), asset = Wide(full.substr(dot + 1));
    soft.package = ue::MakeName(package.c_str()); soft.asset = ue::MakeName(asset.c_str());
    Call load(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LoadClassAsset_Blocking");
    Object* cls = load.Set("AssetClass", soft).Invoke() ? load.Get<Object*>("ReturnValue") : nullptr;
    return cls && ue::Alive(cls) ? cls : nullptr;
}
Object* DefaultOf(Object* cls) {
    Object* result = nullptr;
    ue::ForEachObject([&](Object* object) {
        if (ue::Alive(object) && ue::IsDefault(object) && ue::ClassOf(object) == cls) { result = object; return false; }
        return true;
    });
    return result;
}
// The game's AI function library (the one BP_AISpawner uses): SpawnAI = deferred spawn + FinishSpawning.
Object* AILibrary() {
    Object* cls = LoadClass("/SGameAI/FunctionLibrary/BP_SGameAIFunctionLibrary.BP_SGameAIFunctionLibrary_C");
    return cls ? DefaultOf(cls) : nullptr;
}
bool ReadAttribute(Object* actor, const char* property, float& out) {
    Object* set = Property(actor, "AttributeSetBase");
    Object* basic = ue::FindObject("/Script/CharacterBasics.BasicAttributeSet");
    Object* data = ue::FindObject("/Script/GameplayAbilities.GameplayAttributeData");
    const int32_t field = Offset(set, property), current = data ? ue::Offset(data, "CurrentValue") : -1;
    if (!set || !ue::Alive(set) || !basic || !ue::IsA(set, basic) || field < 0 || !data || ue::StructSize(data) != 16 || current != 12) return false;
    out = ue::At<float>(set, field + current);
    return std::isfinite(out);
}
Object* Asc(Object* actor) {
    Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetAbilitySystemComponent");
    Object* asc = get.Set("Actor", actor).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    return asc && ue::Alive(asc) ? asc : nullptr;
}
int EffectCount(Object* asc, Object* effect) {
    Call count(asc, "GetGameplayEffectCount");
    if (!count.Slot("ReturnValue", sizeof(int32_t)) || !count.Set("SourceGameplayEffect", effect).Set("bEnforceOnGoingCheck", true).Invoke()) return -1;
    return count.Get<int32_t>("ReturnValue");
}
// The mob's SGameAIComponent (on its SGameAIController), the one that runs its state tree, senses and attacks.
Object* AIComponent(Object* actor) {
    if (!ai_component_class_ || !ue::Alive(ai_component_class_)) ai_component_class_ = ue::FindObject("/Script/SGameAI.SGameAIComponent");
    Object* controller = Property(actor, "Controller");
    Object* component = controller && ue::Alive(controller) ? Property(controller, "SGameAIComponent") : nullptr;
    if (!component || !ue::Alive(component) || !ai_component_class_ || !ue::IsA(component, ai_component_class_) ||
        Property(component, "Pawn") != actor || Property(component, "AIController") != controller) return nullptr;
    return component;
}
// Every frame: the mob's AI stays stopped. SGameAIComponent::StopAI is the game's own switch (its blueprint's
// OnAIStopped stops the brain's logic; the game's ActivateAI is the StartAI side). The game may start it again
// (auto start, senses, combat events): bAIStarted is read each frame and the AI stopped again before it acts.
void HoldAI(ULONGLONG now) {
    Object* component = AIComponent(dummy_);
    const int32_t started = Offset(component, "bAIStarted");
    if (!component || started < 0) {
        if (!ai_missing_logged_ && now > spawned_at_ + kEffectWaitMs) {
            ai_missing_logged_ = true; Logf("TEST_DUMMY no SGameAIComponent on actor=%s after %llums: its AI is not held", NameText(dummy_).c_str(), now - spawned_at_);
            Status(L"测试木桩的 AI 读不到，可能还会行动；可移除后重新召唤");
        }
        return;
    }
    if (ue::At<uint8_t>(component, started) == 0) return;
    const bool invoked = Call(component, "StopAI").Invoke();
    Object* controller = Property(dummy_, "Controller");
    if (controller && ue::Alive(controller)) Call(controller, "StopMovement").Invoke();
    const bool stopped = ue::At<uint8_t>(component, started) == 0;
    if (stopped) {
        if (++ai_stops_ <= 6) Logf("TEST_DUMMY AI stopped on actor=%s (%s)", NameText(dummy_).c_str(), ai_stops_ == 1 ? "first" : "the game had started it again");
        if (ai_stuck_logged_) { ai_stuck_logged_ = false; Status(L"测试木桩已召唤在主角面前：不移动、不攻击、锁血"); }
    } else if (!ai_stuck_logged_) {
        ai_stuck_logged_ = true; Logf("TEST_DUMMY AI still started after StopAI (invoked=%d) on actor=%s", invoked ? 1 : 0, NameText(dummy_).c_str());
        Status(L"测试木桩的 AI 没能停下；可移除后重新召唤");
    }
}
bool Remove(const char* why) {
    bool destroyed = false;
    if (Same()) destroyed = Call(dummy_, "K2_DestroyActor").Invoke();
    if (dummy_) Logf("TEST_DUMMY removed actor=%s reason=%s destroyed=%d refills=%u ai_stops=%u", NameText(Same() ? dummy_ : nullptr).c_str(), why, destroyed ? 1 : 0, refills_, ai_stops_);
    dummy_ = nullptr; dummy_name_ = {}; effect_on_ = false; refills_ = 0; ai_stops_ = 0;
    Publish(); return true;
}
bool Fail(const wchar_t* text, const char* why) {
    Status(L"%s", text); Logf("TEST_DUMMY spawn refused: %s", why); Publish(); return false;
}
bool Spawn() {
    Context context;
    if (!ContextNow(context)) return Fail(L"没有可操作的主角，未召唤测试木桩", "no player");
    Remove("spawn again");
    Object* library = AILibrary();
    if (!library) return Fail(L"游戏 AI 创建接口没有加载，未召唤测试木桩", "SGameAI function library missing");
    Object* cls = nullptr;
    for (const char* path : kClasses) if ((cls = LoadClass(path))) { class_used_ = path; break; }
    if (!cls) return Fail(L"小怪蓝图加载失败，未召唤测试木桩", "no mob class loaded");
    if (!NpcRetarget::BeforeActorClassSpawn(cls)) return Fail(L"NPC 外观尚未恢复，未召唤可能共享骨架的小怪", "npc look holds a shared skeleton");
    // distance_ in front of the hero on the hero's floor, facing the hero.
    const double distance = distance_;
    Call where(context.pawn, "K2_GetActorLocation"), ahead(context.pawn, "GetActorForwardVector");
    if (!where.Invoke() || !ahead.Invoke()) return Fail(L"读不到主角位置，未召唤测试木桩", "hero location");
    const Vector at = where.Get<Vector>("ReturnValue"), forward = ahead.Get<Vector>("ReturnValue");
    double fx = forward.x, fy = forward.y; const double flat = std::sqrt(fx * fx + fy * fy);
    if (!(flat > 1e-3) || !std::isfinite(flat)) { fx = 1; fy = 0; } else { fx /= flat; fy /= flat; }
    double hero_half = 96, mob_half = 96;
    if (Object* capsule = Property(context.pawn, "CapsuleComponent")) {
        Call half(capsule, "GetScaledCapsuleHalfHeight");
        if (half.Slot("ReturnValue", sizeof(float)) && half.Invoke()) { const float v = half.Get<float>("ReturnValue"); if (v > 1 && v < 2000) hero_half = v; }
    }
    if (Object* capsule = Property(DefaultOf(cls), "CapsuleComponent")) {
        const int32_t offset = Offset(capsule, "CapsuleHalfHeight");
        if (offset >= 0) { const float v = ue::At<float>(capsule, offset); if (v > 1 && v < 2000) mob_half = v; }
    }
    Transform transform;
    transform.translation[0] = at.x + fx * distance; transform.translation[1] = at.y + fy * distance;
    transform.translation[2] = at.z - hero_half + mob_half + 5.0;
    const double yaw = std::atan2(-fy, -fx);
    transform.rotation[2] = std::sin(yaw / 2); transform.rotation[3] = std::cos(yaw / 2);
    if (!std::isfinite(transform.translation[0] + transform.translation[1] + transform.translation[2])) return Fail(L"主角位置无效，未召唤测试木桩", "position not finite");
    Call spawn(library, "SpawnAI");
    if (!spawn.Set("Class", cls).Set("SpawnTransform", transform).Set("__WorldContext", context.instance).Invoke())
        return Fail(L"游戏 SpawnAI 接口不匹配，未召唤测试木桩", "SpawnAI call");
    Object* actor = spawn.Get<Object*>("ReturnValue");
    if (!actor || !ue::Alive(actor)) return Fail(L"游戏没有生成小怪（位置被挡住？），换个位置再试", "SpawnAI returned none");
    dummy_ = actor; dummy_name_ = ue::NameOf(actor);
    effect_on_ = effect_failed_logged_ = refill_failed_logged_ = dead_logged_ = ai_missing_logged_ = ai_stuck_logged_ = false; refills_ = 0; ai_stops_ = 0;
    spawned_at_ = GetTickCount64(); next_check_ = 0;
    Logf("TEST_DUMMY spawned class=%s actor=%s at=(%.0f,%.0f,%.0f) %.0fcm in front of the hero, facing it",
         class_used_, NameText(actor).c_str(), transform.translation[0], transform.translation[1], transform.translation[2], distance);
    Status(L"测试木桩已召唤在主角面前：不移动、不攻击、锁血");
    HoldAI(spawned_at_);  // before its first frame of AI
    Publish(); return true;
}
// Every frame: its AI stopped (HoldAI). Every kCheckMs while it stands: the effect on, its walk speed 0, its health
// full again.
void Maintain() {
    if (!dummy_) return;
    if (!Same()) {
        Logf("TEST_DUMMY gone (map change or destroyed by the game) refills=%u ai_stops=%u", refills_, ai_stops_);
        dummy_ = nullptr; dummy_name_ = {}; Status(L"测试木桩已不在（换图或被游戏移除）"); Publish(); return;
    }
    const ULONGLONG now = GetTickCount64();
    HoldAI(now);
    if (now < next_check_) return;
    next_check_ = now + kCheckMs;
    if (Object* asc = Asc(dummy_)) if (Object* effect = LoadClass(kEffect)) {
        int count = EffectCount(asc, effect);
        if (count == 0) {
            Call apply(asc, "BP_ApplyGameplayEffectToSelf");
            apply.Set("GameplayEffectClass", effect).Set("Level", 1.0f).Invoke();
            count = EffectCount(asc, effect);
            if (count > 0) Logf("TEST_DUMMY effect %s on actor=%s%s", "GE_AIDisableActionAndLockhealth", NameText(dummy_).c_str(), effect_on_ ? " (again: the game had taken it off)" : "");
        }
        if (count > 0) effect_on_ = true;
        else if (count < 0 || (!effect_on_ && now > spawned_at_ + kEffectWaitMs)) {
            if (!effect_failed_logged_) { effect_failed_logged_ = true; Logf("TEST_DUMMY effect not on after %llums count=%d", now - spawned_at_, count);
                Status(L"测试木桩的停止行动 / 锁血效果没有生效；可移除后重新召唤"); }
        }
    }
    if (Object* movement = Property(dummy_, "CharacterMovement")) {
        const int32_t speed = Offset(movement, "MaxWalkSpeed");
        if (speed >= 0 && ue::At<float>(movement, speed) != 0.0f) ue::At<float>(movement, speed) = 0.0f;
    }
    float health = 0, maximum = 0;
    if (ReadAttribute(dummy_, "Health", health) && ReadAttribute(dummy_, "Health_Max", maximum) && maximum > 0 && health < maximum - 0.5f) {
        Call restore(ue::Default("/Script/ProjectPBZ.Default__PBZRestoreQueryLibrary"), "ResetHealthToMax");
        const bool ok = restore.Slot("ReturnValue", sizeof(bool)) && restore.Set("Target", dummy_).Invoke() && restore.Get<bool>("ReturnValue");
        if (ok) ++refills_;
        else if (!refill_failed_logged_) { refill_failed_logged_ = true; Logf("TEST_DUMMY health refill refused health=%.1f max=%.1f (the effect's Health_Min lock still holds)", health, maximum); }
    }
    Call dead(ue::Default("/Script/SGameAI.Default__SGameAIStateUtils"), "IsDead");
    if (dead.Set("Pawn", dummy_).Invoke() && dead.Get<bool>("ReturnValue") && !dead_logged_) {
        dead_logged_ = true; Logf("TEST_DUMMY died anyway health=%.1f max=%.1f effect=%d", health, maximum, effect_on_ ? 1 : 0);
        Status(L"测试木桩被击倒了（这一招绕过了锁血）；可重新召唤");
    }
}
void Tick() {
    if (disabled_) return;
    const LONG request = InterlockedExchange(&request_, 0);
    if (request == 1) { Publish(true); Spawn(); }
    else if (request == 2) { Remove("asked"); Status(L"测试木桩已移除"); }
    Maintain();
}
}

void Initialize(Logger logger) { logger_ = logger; }
void Request(bool spawn, double distance) { if (spawn) distance_ = distance; InterlockedExchange(&request_, spawn ? 1 : 2); AcquireSRWLockExclusive(&lock_); snapshot_.busy = true; ReleaseSRWLockExclusive(&lock_); }
Snapshot GetSnapshot() { AcquireSRWLockShared(&lock_); Snapshot copy = snapshot_; ReleaseSRWLockShared(&lock_); return copy; }
void OnGameThreadGuarded() {
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        dummy_ = nullptr; dummy_name_ = {};
        if (++faults_ >= kMaxFaults) disabled_ = true;
        Logf("TEST_DUMMY fault %lu/%lu: the test mob is forgotten%s", faults_, kMaxFaults, disabled_ ? "; 测试木桩 off until restart" : "");
        Publish();
    }
}
}
