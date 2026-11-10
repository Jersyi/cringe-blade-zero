#include "boss_assist.h"
#include "npc_retarget.h"
#include "npc_combo.h"
#include "combo_playback_rules.h"
#include "trim_charge_runtime.h"
#include "ue_runtime.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

namespace BossAssist {
namespace {
using ue::Object; using ue::Call; using ue::Name; using ue::Array;
struct Vector { double x, y, z; };
struct Rotator { double pitch, yaw, roll; };
struct alignas(16) Transform { double rotation[4]{0,0,0,1}, translation[4]{}, scale[4]{1,1,1,0}; };
static_assert(sizeof(Transform) == 96, "PBZ native FTransform layout");
struct SoftClass { int32_t index, serial; Name package, asset; ue::String subpath; };
static_assert(sizeof(SoftClass) == 40, "TSoftClassPtr layout");
struct LockTarget { unsigned char bytes[16]{}; };

constexpr char kBlinkOutFx[] = "/PBZVFXPack/VFX/BattleFX/DangHui/NS_DangHui_Blink_301_Hide.NS_DangHui_Blink_301_Hide";
constexpr char kBlinkInFx[] = "/PBZVFXPack/VFX/BattleFX/DangHui/NS_DangHui_Blink_301_Show.NS_DangHui_Blink_301_Show";
// Where the boss appears (瞬身: no travel, the blink effect where it stands): kLandCm from the target, towards one side of
// the camera's view (kSideCm across, kBackCm towards the camera: that way), every call the other side; feet on the
// target's ground. beside: kNearSideCm beside the hero, on the hero's ground.
constexpr double kSideCm = 1500, kBackCm = 200, kLandCm = 260, kNearSideCm = 450, kPi = 3.14159265358979;
// 援护 on the hero's side: the game's own effect for the player's summons (the Shachi weapon's). Its AI task sets the
// Faction Component to Faction.Player and it tags the boss CharacterType.NPC: the boss's hits go to the hero's enemies.
constexpr char kAllyEffect[] = "/PBZCombatAsset/Prototypes/GameplayEffects/ShachiWeapon/GE_SummonFactionOverride.GE_SummonFactionOverride_C";
constexpr ULONGLONG kActivateRetryMs = 150, kActivateGiveUpMs = 3000, kCastTimeoutMs = 25000, kLeaveMs = 350;
constexpr DWORD kMaxFaults = 3;
constexpr uint32_t kSideLogs = 3;  // the team / faction probe lines, the first few calls only

enum class Phase { Idle, Casting, Leaving };
struct Job { std::string monster, ability; Mods mods; bool beside = false; };
struct Slot {
    Object* boss = nullptr; Name name{};
    Object* ability = nullptr; std::string ability_name;
    Object* enemy = nullptr; Name enemy_name{}; LockTarget target{}; bool lock_known = false;
    Phase phase = Phase::Idle; ULONGLONG phase_at = 0, next_try = 0, born = 0;
    int32_t handle = 0; bool seen_active = false; uint32_t tries = 0, ai_stops = 0;
    Vector land{}; Mods mods; float dilation = 1;
};

Logger logger_ = nullptr;
SRWLOCK requests_lock_ = SRWLOCK_INIT;
std::vector<Job> requests_;
bool stop_requested_ = false;
Object* engine_ = nullptr;
Slot slots_[kSlots];
// The move preview's boss: cast again kPreviewGapMs after each cast, from its spot.
constexpr ULONGLONG kPreviewGapMs = 700;
Slot preview_; Vector preview_home_{}; double preview_yaw_ = 0; ULONGLONG preview_next_ = 0; uint32_t preview_loops_ = 0;
// The preview's 加速 (PreviewSpeed): percent or a curve; the montage the curve was read on and its length; logs.
Object* preview_clip_ = nullptr;  // a montage preview (a grab's executor side): played, not cast
uint32_t preview_clip_tries_ = 0;
int preview_speed_ = 100; std::vector<NpcCombo::CurvePoint> preview_curve_; Object* preview_montage_ = nullptr; double preview_length_ = 0;
float preview_logged_ = -1; uint32_t preview_speed_logs_ = 0;
// The preview's 裁剪 (PreviewTrim, ms) and this cast's part: the start jumped to, the end cut.
std::vector<int> preview_trim_holds_;bool preview_hold_down_=false;
ComboPlayback::ChargePoints preview_charge_;TrimChargeRuntime::Pause preview_pause_;
int preview_trim_from_ = 0, preview_trim_to_ = 0; bool preview_trim_jumped_ = false, preview_trim_cut_ = false; uint32_t preview_trim_logs_ = 0;
bool preview_trim_dirty_ = false; unsigned preview_trim_wait_ = 0;
NpcCombo::TrimRange PreviewRange() { NpcCombo::Node n; n.trim_from = preview_trim_from_; n.trim_to = preview_trim_to_;n.trim_holds=preview_trim_holds_; return NpcCombo::TrimOf(n); }
double side_ = 1;
uint32_t side_logs_ = 0;
DWORD faults_ = 0; bool disabled_ = false;

void Logf(const char* format, ...) {
    if (!logger_) return;
    char text[900]{}; va_list arguments; va_start(arguments, format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, format, arguments); va_end(arguments);
    logger_(text);
}
std::string Text(Object* object) { char text[200]{}; if (object && ue::Alive(object)) ue::NameText(ue::NameOf(object), text, sizeof(text)); return text[0] ? text : "-"; }
std::string NameString(Name name) { char text[200]{}; ue::NameText(name, text, sizeof(text)); return text; }
int32_t Offset(Object* object, const char* property) { return object ? ue::Offset(ue::ClassOf(object), property) : -1; }
Object* Property(Object* object, const char* property) {
    const int32_t offset = Offset(object, property);
    Object* value = offset >= 0 ? ue::At<Object*>(object, offset) : nullptr;
    return value && ue::Alive(value) ? value : nullptr;
}
bool SameBoss(const Slot& s) { return s.boss && ue::Alive(s.boss) && ue::NameOf(s.boss) == s.name; }
bool SameEnemy(const Slot& s) { return s.enemy && ue::Alive(s.enemy) && ue::NameOf(s.enemy) == s.enemy_name; }
size_t SlotIndex(const Slot& s) { return &s == &preview_ ? 9 : static_cast<size_t>(&s - slots_); }  // 9: the preview's
struct Context { Object* world = nullptr; Object* instance = nullptr; Object* controller = nullptr; Object* pawn = nullptr; };
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
// "/Path/BP_X" -> the class /Path/BP_X.BP_X_C (or a full object path as given).
Object* LoadClass(const std::string& path) {
    std::string full = path;
    if (full.find('.') == std::string::npos) { const std::string name = full.substr(full.find_last_of('/') + 1); full += "." + name + "_C"; }
    if (Object* cls = ue::FindObject(full.c_str())) return cls;
    const size_t dot = full.find('.');
    SoftClass soft{};
    const std::wstring package = Wide(full.substr(0, dot)), asset = Wide(full.substr(dot + 1));
    soft.package = ue::MakeName(package.c_str()); soft.asset = ue::MakeName(asset.c_str());
    Call load(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LoadClassAsset_Blocking");
    Object* cls = load.Set("AssetClass", soft).Invoke() ? load.Get<Object*>("ReturnValue") : nullptr;
    return cls && ue::Alive(cls) ? cls : nullptr;
}
Object* LoadAsset(const char* full) {
    if (Object* asset = ue::FindObject(full)) return asset;
    const std::string text(full); const size_t dot = text.find('.');
    SoftClass soft{};
    const std::wstring package = Wide(text.substr(0, dot)), asset = Wide(text.substr(dot + 1));
    soft.package = ue::MakeName(package.c_str()); soft.asset = ue::MakeName(asset.c_str());
    Call load(ue::Default("/Script/Engine.Default__KismetSystemLibrary"), "LoadAsset_Blocking");
    Object* object = load.Set("Asset", soft).Invoke() ? load.Get<Object*>("ReturnValue") : nullptr;
    return object && ue::Alive(object) ? object : nullptr;
}
Object* AILibrary() {
    static Object* cached = nullptr;
    if (cached && ue::Alive(cached)) return cached;
    Object* cls = LoadClass("/SGameAI/FunctionLibrary/BP_SGameAIFunctionLibrary.BP_SGameAIFunctionLibrary_C");
    if (!cls) return nullptr;
    Object* result = nullptr;
    ue::ForEachObject([&](Object* object) {
        if (ue::Alive(object) && ue::IsDefault(object) && ue::ClassOf(object) == cls) { result = object; return false; }
        return true;
    });
    cached = result;
    return result;
}
bool Location(Object* actor, Vector& out) {
    Call where(actor, "K2_GetActorLocation"); if (!where.Invoke()) return false;
    out = where.Get<Vector>("ReturnValue"); return std::isfinite(out.x + out.y + out.z);
}
double Yaw(const Vector& from, const Vector& to) { return std::atan2(to.y - from.y, to.x - from.x) * 180.0 / kPi; }
void Face(Object* actor, double yaw) {
    Call set(actor, "K2_SetActorRotation"); set.Set("NewRotation", Rotator{0, yaw, 0}).Set("bTeleportPhysics", false).Invoke();
}
Object* Asc(Object* actor) {
    Call get(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "GetAbilitySystemComponent");
    Object* asc = get.Set("Actor", actor).Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    return asc && ue::Alive(asc) ? asc : nullptr;
}
Object* AIComponent(Object* actor) {
    Object* controller = Property(actor, "Controller");
    Object* component = Property(controller, "SGameAIComponent");
    return component && Property(component, "Pawn") == actor ? component : nullptr;
}
// Every frame: the boss's AI stays stopped (SGameAIComponent::StopAI, as the test dummy holds it).
void HoldAI(Slot& s) {
    Object* component = AIComponent(s.boss);
    const int32_t started = Offset(component, "bAIStarted");
    if (!component || started < 0 || ue::At<uint8_t>(component, started) == 0) return;
    Call(component, "StopAI").Invoke();
    if (Object* controller = Property(s.boss, "Controller")) Call(controller, "StopMovement").Invoke();
    if (++s.ai_stops <= 4) Logf("BOSS_ASSIST slot=%zu AI stopped on %s (%s) now=%d", SlotIndex(s), Text(s.boss).c_str(), s.ai_stops == 1 ? "first" : "started again",
                                ue::At<uint8_t>(component, started));
}
// The ability spec of `handle` on the ASC: whether it is there and its ActiveCount.
bool SpecState(Object* asc, int32_t handle, bool& present, uint8_t& active) {
    present = false; active = 0;
    Object* type = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpec"), *container = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t specs = Offset(asc, "ActivatableAbilities"), items = container ? ue::Offset(container, "Items") : -1, size = type ? ue::StructSize(type) : 0,
        handle_at = type ? ue::Offset(type, "Handle") : -1, count_at = type ? ue::Offset(type, "ActiveCount") : -1;
    if (specs < 0 || items < 0 || size <= 0 || handle_at < 0 || count_at < 0) return false;
    const Array array = ue::At<Array>(asc, specs + items);
    if (array.num < 0 || array.num > 4096 || (array.num && !array.data)) return false;
    for (int32_t i = 0; i < array.num; ++i) {
        unsigned char* row = static_cast<unsigned char*>(array.data) + static_cast<size_t>(i) * static_cast<size_t>(size);
        if (ue::At<int32_t>(row, handle_at) != handle) continue;
        present = true; active = ue::At<uint8_t>(row, count_at); return true;
    }
    return true;
}
// A spec already granted for the class (the boss's own set): its handle, else 0. count: specs on the ASC.
int32_t GrantedHandle(Object* asc, Object* cls, int32_t& count) {
    count = 0;
    Object* type = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpec"), *container = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t specs = Offset(asc, "ActivatableAbilities"), items = container ? ue::Offset(container, "Items") : -1, size = type ? ue::StructSize(type) : 0,
        handle_at = type ? ue::Offset(type, "Handle") : -1, ability_at = type ? ue::Offset(type, "Ability") : -1;
    if (specs < 0 || items < 0 || size <= 0 || handle_at < 0 || ability_at < 0) return 0;
    const Array array = ue::At<Array>(asc, specs + items);
    if (array.num < 0 || array.num > 4096 || (array.num && !array.data)) return 0;
    count = array.num;
    for (int32_t i = 0; i < array.num; ++i) {
        unsigned char* row = static_cast<unsigned char*>(array.data) + static_cast<size_t>(i) * static_cast<size_t>(size);
        Object* ability = ue::At<Object*>(row, ability_at);
        if (ability && ue::Alive(ability) && ue::ClassOf(ability) == cls) return ue::At<int32_t>(row, handle_at);
    }
    return 0;
}
// Reflection names on a class chain (functions or properties) holding any of the words, for the probe's log.
std::string Members(Object* object, const char* const* words, size_t count, bool functions) {
    std::string out; size_t listed = 0;  // words: lower case (NameContains compares the lowered name)
    Object* function_class = ue::FindObject("/Script/CoreUObject.Function");
    for (Object* c = object ? ue::ClassOf(object) : nullptr; c && listed < 40; c = ue::At<Object*>(c, ue::layout::kSuper)) {
        if (functions) {
            for (Object* f = ue::At<Object*>(c, ue::layout::kChildren); f && listed < 40; f = ue::At<Object*>(f, ue::layout::kFieldNext)) {
                if (!function_class || !ue::IsA(f, function_class)) continue;
                const Name name = ue::NameOf(f);
                if (count && !ue::NameContains(name, words, count)) continue;
                out += (out.empty() ? "" : ",") + NameString(name); ++listed;
            }
        } else {
            ue::Field fields[256]; const size_t n = ue::Fields(c, fields, 256);
            for (size_t i = 0; i < n && listed < 40; ++i) {
                const Name name = ue::MakeName(Wide(fields[i].name).c_str());
                if (count && !ue::NameContains(name, words, count)) continue;
                char value[96] = "";
                if (fields[i].size == 1) std::snprintf(value, sizeof(value), "=%u", ue::At<uint8_t>(object, fields[i].offset));
                else if (fields[i].size == 4) std::snprintf(value, sizeof(value), "=%d", ue::At<int32_t>(object, fields[i].offset));
                else if (fields[i].size == 8) {
                    Object* o = ue::At<Object*>(object, fields[i].offset);
                    if (o && ue::Alive(o)) std::snprintf(value, sizeof(value), "=%s", Text(o).c_str());
                    else { const std::string tag = NameString(ue::At<Name>(object, fields[i].offset)); std::snprintf(value, sizeof(value), "=%s", tag.empty() ? "0" : tag.c_str()); }  // a tag / name
                }
                out += (out.empty() ? "" : ",") + std::string(fields[i].name) + value; ++listed;
            }
        }
    }
    return out.empty() ? "-" : out;
}
uint8_t TeamOf(Object* actor, bool& ok) {
    Call team(ue::Default("/Script/SGameAI.Default__SGameAIController"), "GetTeam");
    ok = team.Set("Actor", actor).Invoke(); return ok ? team.Get<uint8_t>("ReturnValue") : 0;
}
// The probe's view of who is on whose side and what could change it (the first few calls).
void LogSides(const Slot& s, Object* hero) {
    if (side_logs_ >= kSideLogs) return;
    ++side_logs_;
    static const char* const kTeam[] = {"team", "faction", "camp", "attitude", "hostile", "relation"};
    bool a = false, b = false, c = false;
    const uint8_t mine = TeamOf(s.boss, a), theirs = TeamOf(hero, b), enemy = SameEnemy(s) ? TeamOf(s.enemy, c) : 0;
    Object* controller = Property(s.boss, "Controller");
    Logf("BOSS_ASSIST team boss=%u(%d) hero=%u(%d) enemy=%u(%d) controller=%s", mine, a, theirs, b, enemy, c, Text(controller).c_str());
    Logf("BOSS_ASSIST team functions controller: %s", Members(controller, kTeam, std::size(kTeam), true).c_str());
    Logf("BOSS_ASSIST team fields controller: %s", Members(controller, kTeam, std::size(kTeam), false).c_str());
    Object* faction = Property(s.boss, "Faction Component");
    Logf("BOSS_ASSIST faction component boss=%s class=%s fields: %s functions: %s", Text(faction).c_str(), faction ? Text(ue::ClassOf(faction)).c_str() : "-",
         faction ? Members(faction, kTeam, std::size(kTeam), false).c_str() : "-", faction ? Members(faction, kTeam, std::size(kTeam), true).c_str() : "-");
    Object* hero_faction = Property(hero, "Faction Component");
    Logf("BOSS_ASSIST faction component hero=%s fields: %s", Text(hero_faction).c_str(), hero_faction ? Members(hero_faction, kTeam, std::size(kTeam), false).c_str() : "-");
    if (side_logs_ == 1 && faction) {
        Logf("BOSS_ASSIST faction component all fields: %s", Members(faction, nullptr, 0, false).c_str());
        Logf("BOSS_ASSIST faction component all functions: %s", Members(faction, nullptr, 0, true).c_str());
    }
}
// An actor's faction: its FactionComponent's GetCurrnetFaction (the game's spelling; the tag is kept natively, no
// reflected field holds it). false: no component or no answer.
bool FactionOf(Object* actor, Name& tag) {
    Object* component = Property(actor, "Faction Component");
    Call get(component, "GetCurrnetFaction");
    if (!component || !get.Ok() || !get.Invoke()) return false;
    tag = get.Get<Name>("ReturnValue"); return true;
}
std::string FactionText(Object* actor) { Name tag{}; return FactionOf(actor, tag) ? (tag.None() ? std::string("none") : NameString(tag)) : std::string("-"); }
// FactionComponent::SetFaction(tag) on the actor; its one 8-byte parameter, whatever its name.
bool SetFaction(Object* actor, const Name& tag, std::string& how) {
    Object* component = Property(actor, "Faction Component");
    Object* function = component ? ue::FindFunction(ue::ClassOf(component), "SetFaction") : nullptr;
    if (!function) { how = "no SetFaction"; return false; }
    ue::Field params[4]{}; const size_t n = ue::Fields(function, params, 4);
    for (size_t i = 0; i < n; ++i) { if (i) how += ","; how += std::string(params[i].name) + "/" + std::to_string(params[i].size); }
    if (n < 1 || params[0].size != static_cast<int32_t>(sizeof(Name))) return false;
    Call call(component, function);
    return call.Set(params[0].name, tag).Invoke();
}
// The hero's side for the boss: the game's summon effect (kAllyEffect) on it, the hero's team id on its AI controller
// (BlueprintSetGenericTeamId: perception's attitude); when its faction tags still differ from the hero's after that,
// the hero's are copied in. Logged: teams and faction tags before and after.
void Ally(const Slot& s, Object* hero) {
    const std::string before = FactionText(s.boss);
    bool applied = false;
    Object* asc = Asc(s.boss), *effect = LoadClass(kAllyEffect);
    if (asc && effect) { Call apply(asc, "BP_ApplyGameplayEffectToSelf"); applied = apply.Set("GameplayEffectClass", effect).Set("Level", 1.0f).Invoke(); }
    bool ok = false; const uint8_t team = TeamOf(hero, ok);
    Object* controller = Property(s.boss, "Controller");
    bool set = false;
    if (ok && controller)
        if (Object* function = ue::FindFunction(ue::ClassOf(controller), "BlueprintSetGenericTeamId")) {
            ue::Field params[4]{}; const size_t n = ue::Fields(function, params, 4);
            if (n >= 1 && params[0].size == 1) { Call call(controller, function); set = call.Set(params[0].name, team).Invoke(); }
        }
    // Still not the hero's faction after the effect: FactionComponent::SetFaction with the hero's.
    Name mine{}, theirs{}; bool forced = false; std::string how;
    const std::string after_effect = FactionText(s.boss);
    if (FactionOf(hero, theirs) && !theirs.None() && (!FactionOf(s.boss, mine) || !(mine == theirs))) forced = SetFaction(s.boss, theirs, how);
    bool after_ok = false; const uint8_t after = TeamOf(s.boss, after_ok);
    Logf("BOSS_ASSIST slot=%zu ally effect=%d applied=%d team hero=%u set=%d boss now=%u(%d) faction before=%s afterEffect=%s now=%s hero=%s setFaction=%d(%s)", SlotIndex(s),
         effect ? 1 : 0, applied ? 1 : 0, team, set ? 1 : 0, after, after_ok ? 1 : 0, before.c_str(), after_effect.c_str(), FactionText(s.boss).c_str(), FactionText(hero).c_str(),
         forced ? 1 : 0, how.c_str());
}
float HalfHeight(Object* actor, float fallback) {
    Object* capsule = Property(actor, "CapsuleComponent");
    Call h(capsule, "GetScaledCapsuleHalfHeight");
    const float half = capsule && h.Invoke() ? h.Get<float>("ReturnValue") : 0.0f;
    return half > 5.0f && half < 2000.0f ? half : fallback;
}
// EMovementMode on the boss's CharacterMovement (MOVE_Walking 1, MOVE_Falling 3).
void MoveMode(Object* boss, uint8_t mode) {
    if (Object* movement = Property(boss, "CharacterMovement")) Call(movement, "SetMovementMode").Set("NewMovementMode", mode).Set("NewCustomMode", uint8_t{0}).Invoke();
}
void Blink(Object* actor, const char* fx) {
    Vector at{}; if (!Location(actor, at)) return;
    Object* system = LoadAsset(fx);
    if (!system) return;
    Call spawn(ue::Default("/Script/Niagara.Default__NiagaraFunctionLibrary"), "SpawnSystemAtLocation");
    spawn.Set("WorldContextObject", actor).Set("SystemTemplate", system).Set("Location", at).Set("Rotation", Rotator{}).Set("Scale", Vector{1, 1, 1})
        .Set("bAutoDestroy", true).Set("bAutoActivate", true).Set("PoolingMethod", uint8_t{0}).Set("bPreCullCheck", false).Invoke();
}
// 加速: the boss's own time (its moves, montages and timelines) runs at speed / 100.
void Dilate(Slot& s) {
    const int32_t at = Offset(s.boss, "CustomTimeDilation");
    s.dilation = std::clamp(s.mods.speed / 100.0f, 0.5f, 10.0f);
    if (at >= 0 && std::fabs(s.dilation - 1.0f) > 0.001f) ue::At<float>(s.boss, at) = s.dilation;
}
ULONGLONG Scaled(ULONGLONG ms, const Slot& s) { return static_cast<ULONGLONG>(double(ms) / (std::min)(1.0f, s.dilation)); }
void Leave(Slot& s, const char* why) {
    if (s.phase == Phase::Leaving || s.phase == Phase::Idle) return;
    Logf("BOSS_ASSIST slot=%zu leave reason=%s after %llums tries=%u seenActive=%d", SlotIndex(s), why, GetTickCount64() - s.phase_at, s.tries, s.seen_active ? 1 : 0);
    if (SameBoss(s)) {
        if (Object* asc = Asc(s.boss)) Call(asc, "CancelAllAbilities").Invoke();
        Blink(s.boss, kBlinkOutFx);
        Call hide(s.boss, "SetActorHiddenInGame"); hide.Set("bNewHidden", true).Invoke();
    }
    s.phase = Phase::Leaving; s.phase_at = GetTickCount64();
}
void Finish(Slot& s) {
    bool destroyed = false;
    if (SameBoss(s)) destroyed = Call(s.boss, "K2_DestroyActor").Invoke();
    Logf("BOSS_ASSIST slot=%zu gone destroyed=%d aiStops=%u", SlotIndex(s), destroyed ? 1 : 0, s.ai_stops);
    s = Slot{};
}
size_t ResetCooldown(Object* asc, int32_t handle, Object* ability_class);
bool OptimisationOff(Object* boss);
int Arm(Object* boss);
// Grants (when the boss's own set lacks it) and activates the skill. true: asked and the ASC said yes. A refusal
// while the game's own ability cooldown runs (ResetCooldown found a used mark) is retried once at once.
bool Activate(Slot& s) {
    Object* asc = Asc(s.boss);
    if (!asc) { Logf("BOSS_ASSIST slot=%zu no ability system on %s", SlotIndex(s), Text(s.boss).c_str()); return false; }
    if (s.handle <= 0) {
        int32_t specs = 0; s.handle = GrantedHandle(asc, s.ability, specs);
        const bool own = s.handle > 0;
        if (!own) {
            Call give(asc, "K2_GiveAbility");
            if (give.Set("AbilityClass", s.ability).Set("Level", int32_t{0}).Set("InputID", int32_t{-1}).Invoke()) s.handle = give.Get<int32_t>("ReturnValue");
        }
        Logf("BOSS_ASSIST slot=%zu ability %s %s handle=%d specs=%d", SlotIndex(s), s.ability_name.c_str(), own ? "from the boss's own set" : "granted", s.handle, specs);
        if (s.handle <= 0) return false;
    }
    ++s.tries;
    Call activate(asc, "TryActivateAbility");
    bool ok = activate.Set("AbilityToActivate", s.handle).Set("bAllowRemoteActivation", true).Invoke() && activate.Get<bool>("ReturnValue");
    size_t reset = 0;
    if (!ok && (reset = ResetCooldown(asc, s.handle, s.ability)) > 0) {
        Call again(asc, "TryActivateAbility");
        ok = again.Set("AbilityToActivate", s.handle).Set("bAllowRemoteActivation", true).Invoke() && again.Get<bool>("ReturnValue");
    }
    if (s.tries <= 3 || ok) Logf("BOSS_ASSIST slot=%zu activate try=%u ok=%d%s", SlotIndex(s), s.tries, ok ? 1 : 0, reset ? " (game cooldown reset)" : "");
    return ok;
}
// Aim: the hero's lock target on the boss's own targeting component, and its AI controller's focus.
void Aim(Slot& s) {
    if (!SameEnemy(s)) { Logf("BOSS_ASSIST slot=%zu aim: no enemy (the skill goes where the boss faces)", SlotIndex(s)); return; }
    Vector me{}, them{};
    if (Location(s.boss, me) && Location(s.enemy, them)) Face(s.boss, Yaw(me, them));
    Call get(s.boss, "GetTargetingComponent", "/Script/SGameCombatSystem.TargetingComponentHolder");
    Object* targeting = get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    bool locked = false;
    if (targeting && ue::Alive(targeting) && s.lock_known) {
        Call set(targeting, "SetLockTarget");
        locked = set.Set("Target", s.target).Invoke() && set.Get<bool>("ReturnValue");
    }
    Object* controller = Property(s.boss, "Controller");
    const bool focus = controller && Call(controller, "K2_SetFocus").Set("NewFocus", s.enemy).Invoke();
    static const char* const kTarget[] = {"target", "enemy", "focus"};
    Object* ai = AIComponent(s.boss);
    const bool list = side_logs_ <= 1 && ai;
    Logf("BOSS_ASSIST slot=%zu aim enemy=%s targeting=%s locked=%d focus=%d ai=%s%s%s", SlotIndex(s), Text(s.enemy).c_str(), Text(targeting).c_str(), locked ? 1 : 0, focus ? 1 : 0,
         Text(ai).c_str(), list ? " ai target functions: " : "", list ? Members(ai, kTarget, std::size(kTarget), true).c_str() : "");
}
// The slot a call goes to: the one its node's call before holds under 允许中断, else a free one, else the oldest.
Slot& PickSlot(const Job& job) {
    if (job.mods.node >= 0)
        for (Slot& s : slots_) if (s.phase != Phase::Idle && s.mods.node == job.mods.node && s.mods.interrupt) { Leave(s, "interrupted"); Finish(s); return s; }
    for (Slot& s : slots_) if (s.phase == Phase::Idle) return s;
    Slot* oldest = &slots_[0];
    for (Slot& s : slots_) if (s.born < oldest->born) oldest = &s;
    Leave(*oldest, "replaced"); Finish(*oldest);
    return *oldest;
}
void Start(const Job& job) {
    Context context;
    if (!ContextNow(context)) { Logf("BOSS_ASSIST refused: no player"); return; }
    Object* cls = LoadClass(job.monster), *ability = LoadClass(job.ability), *library = AILibrary();
    if (!cls || !ability || !library) { Logf("BOSS_ASSIST refused: class=%d ability=%d library=%d (%s, %s)", cls ? 1 : 0, ability ? 1 : 0, library ? 1 : 0, job.monster.c_str(), job.ability.c_str()); return; }
    const bool shared = NpcRetarget::ClassSharesEditedSkeleton(cls);
    // Target: the hero's lock (or the soft-lock pick), never changing the hero's own lock.
    unsigned char bytes[16]{}; Object* enemy = nullptr;
    const bool lock_known = NpcRetarget::HeroLockTarget(bytes, enemy);
    if (enemy && !ue::Alive(enemy)) enemy = nullptr;
    Vector hero{}; if (!Location(context.pawn, hero)) { Logf("BOSS_ASSIST refused: hero location"); return; }
    Vector aim = hero;
    if (enemy) Location(enemy, aim);
    else { Call ahead(context.pawn, "GetActorForwardVector"); if (ahead.Invoke()) { const Vector f = ahead.Get<Vector>("ReturnValue"); aim = {hero.x + f.x * 400, hero.y + f.y * 400, hero.z}; } }
    // The camera's sideways direction: the boss appears on one side of the target, every call the other.
    double cam_yaw = Yaw(hero, aim);
    if (Object* camera = Property(context.controller, "PlayerCameraManager")) { Call rot(camera, "GetCameraRotation"); if (rot.Invoke()) cam_yaw = rot.Get<Rotator>("ReturnValue").yaw; }
    const double cy = cam_yaw * kPi / 180.0, fx = std::cos(cy), fy = std::sin(cy), rx = -fy, ry = fx;
    side_ = -side_;
    Object* ground = job.beside || !enemy ? context.pawn : enemy;
    Vector base{}; if (!Location(ground, base)) base = job.beside ? hero : aim;
    const double floor_z = base.z - HalfHeight(ground, 90.0f);
    Vector land{};
    if (job.beside) land = {hero.x + rx * side_ * kNearSideCm, hero.y + ry * side_ * kNearSideCm, floor_z};
    else {
        const double dx = rx * side_ * kSideCm - fx * kBackCm, dy = ry * side_ * kSideCm - fy * kBackCm, flat = (std::max)(1.0, std::sqrt(dx * dx + dy * dy));
        land = {aim.x + dx / flat * kLandCm, aim.y + dy / flat * kLandCm, floor_z};
    }
    // Spawned over its spot (set down on it below, its own half height known then), facing the target.
    Transform at; at.translation[0] = land.x; at.translation[1] = land.y; at.translation[2] = land.z + 300;
    const double yaw = Yaw(land, aim) * kPi / 180.0;
    at.rotation[2] = std::sin(yaw / 2); at.rotation[3] = std::cos(yaw / 2);
    Slot& s = PickSlot(job);
    Call spawn(library, "SpawnAI");
    if (!spawn.Set("Class", cls).Set("SpawnTransform", at).Set("__WorldContext", context.instance).Invoke()) { Logf("BOSS_ASSIST refused: SpawnAI call"); return; }
    Object* actor = spawn.Get<Object*>("ReturnValue");
    if (!actor || !ue::Alive(actor)) { Logf("BOSS_ASSIST refused: SpawnAI returned none at (%.0f,%.0f,%.0f)", land.x, land.y, land.z); return; }
    s = Slot{};
    s.boss = actor; s.name = ue::NameOf(actor); s.ability = ability; s.ability_name = job.ability.substr(job.ability.find_last_of('/') + 1);
    s.enemy = enemy; s.enemy_name = enemy ? ue::NameOf(enemy) : Name{}; std::memcpy(s.target.bytes, bytes, sizeof(bytes)); s.lock_known = lock_known;
    s.mods = job.mods; s.born = GetTickCount64();
    Logf("BOSS_ASSIST slot=%zu spawned %s class=%s ability=%s node=%d beside=%d sharedSkeleton=%d lock=%d enemy=%s at=(%.0f,%.0f,%.0f) aim=(%.0f,%.0f,%.0f) camYaw=%.0f "
         "speed=%d damage=%.2f force=%d armor=%d invincible=%d ignoreGuard=%d interrupt=%d",
         SlotIndex(s), Text(actor).c_str(), Text(cls).c_str(), s.ability_name.c_str(), job.mods.node, job.beside ? 1 : 0, shared ? 1 : 0, lock_known ? 1 : 0, Text(enemy).c_str(),
         land.x, land.y, land.z, aim.x, aim.y, aim.z, cam_yaw, job.mods.speed, job.mods.damage, job.mods.force, job.mods.armor ? 1 : 0, job.mods.invincible ? 1 : 0,
         job.mods.ignore_guard ? 1 : 0, job.mods.interrupt ? 1 : 0);
    HoldAI(s);
    Dilate(s);
    OptimisationOff(s.boss);  // never hidden by the game's distance optimisation (see there)
    Arm(s.boss);              // its weapon in hand, as in a fight
    // Never in the hero's way: the boss's capsule lets pawns through.
    if (Object* capsule = Property(s.boss, "CapsuleComponent"))
        Call(capsule, "SetCollisionResponseToChannel").Set("Channel", uint8_t{2}).Set("NewResponse", uint8_t{0}).Invoke();
    LogSides(s, context.pawn);
    Ally(s, context.pawn);
    // 瞬身: on its spot at once (capsule centre its half height over the ground, a little more for a slope), the
    // blink effect there, and the skill.
    s.land = {land.x, land.y, floor_z + HalfHeight(s.boss, 90.0f) + 8};
    { Call put(s.boss, "K2_SetActorLocation"); put.Set("NewLocation", s.land).Set("bSweep", false).Set("bTeleport", true).Invoke(); }
    if (Object* movement = Property(s.boss, "CharacterMovement")) Call(movement, "StopMovementImmediately").Invoke();
    MoveMode(s.boss, 1);
    Blink(s.boss, kBlinkInFx);
    Logf("BOSS_ASSIST slot=%zu blink in at=(%.0f,%.0f,%.0f)", SlotIndex(s), s.land.x, s.land.y, s.land.z);
    s.phase = Phase::Casting; s.phase_at = GetTickCount64(); s.next_try = 0; Aim(s);
}
void TickSlot(Slot& s, ULONGLONG now) {
    if (s.phase == Phase::Idle) return;
    if (s.phase != Phase::Leaving && !SameBoss(s)) { Logf("BOSS_ASSIST slot=%zu boss gone (destroyed by the game) phase=%d", SlotIndex(s), static_cast<int>(s.phase)); s = Slot{}; return; }
    if (s.phase == Phase::Leaving) { if (now >= s.phase_at + kLeaveMs) Finish(s); return; }
    HoldAI(s);
    if (s.phase == Phase::Casting) {
        Object* asc = Asc(s.boss);
        bool present = false; uint8_t active = 0;
        if (asc && s.handle > 0) SpecState(asc, s.handle, present, active);
        if (active) { if (!s.seen_active) Logf("BOSS_ASSIST slot=%zu skill active after %llums", SlotIndex(s), now - s.phase_at); s.seen_active = true; }
        if (s.seen_active && !active) { Leave(s, "skill-ended"); return; }
        if (!s.seen_active && now >= s.next_try) {
            s.next_try = now + kActivateRetryMs;
            if (now > s.phase_at + kActivateGiveUpMs) { Leave(s, "not-activated"); return; }
            Activate(s);
        }
        if (now > s.phase_at + Scaled(kCastTimeoutMs, s)) Leave(s, "timeout");
    }
}
// ---- Reflection helpers ------------------------------------------------------------------------------
uintptr_t FieldOf(Object* type, const char* property) {
    for (; type; type = ue::At<Object*>(type, ue::layout::kSuper)) {
        uintptr_t field = ue::At<uintptr_t>(type, ue::layout::kChildProperties);
        for (size_t guard = 0; field && guard < 4096; ++guard) {
            if (ue::NameIs(ue::At<Name>(reinterpret_cast<void*>(field), ue::layout::kPropName), property)) return field;
            field = ue::At<uintptr_t>(reinterpret_cast<void*>(field), ue::layout::kPropNext);
        }
    }
    return 0;
}
// A reflected bool (bitfield or plain) on the object. false: no such bool.
bool BoolOf(Object* object, const char* property, bool& value) {
    const uintptr_t field = object ? FieldOf(ue::ClassOf(object), property) : 0;
    if (!field || ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x70) != 1) return false;
    const int32_t offset = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset);
    const uint8_t byte_offset = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x71), mask = ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x73);
    if (byte_offset > 7 || !mask) return false;
    value = (ue::At<uint8_t>(object, offset + byte_offset) & mask) != 0; return true;
}
// Weapon actors (a WeaponMesh component) attached anywhere under the actor, for the log: shown / all.
std::string WeaponCount(Object* actor) {
    size_t all = 0, shown = 0; Call attached(actor, "GetAttachedActors");
    if (attached.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Invoke()) {
        const Array list = attached.Get<Array>("OutActors");
        for (int32_t i = 0; list.data && i < list.num && i < 256; ++i) {
            Object* a = static_cast<Object**>(list.data)[i];
            if (!a || !ue::Alive(a) || Offset(a, "WeaponMesh") < 0) continue;
            ++all; bool hidden = true, visible = false;
            if (BoolOf(a, "bHidden", hidden) && !hidden && BoolOf(Property(a, "WeaponMesh"), "bVisible", visible) && visible) ++shown;
        }
        ue::Free(list.data);
    }
    return std::to_string(shown) + "/" + std::to_string(all);
}
// The spec row of the handle on the ASC (nullptr: none); type: the spec struct.
unsigned char* SpecRow(Object* asc, int32_t handle, Object*& type) {
    type = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpec");
    Object* container = ue::FindObject("/Script/GameplayAbilities.GameplayAbilitySpecContainer");
    const int32_t specs = Offset(asc, "ActivatableAbilities"), items = container ? ue::Offset(container, "Items") : -1, size = type ? ue::StructSize(type) : 0,
        handle_at = type ? ue::Offset(type, "Handle") : -1;
    if (specs < 0 || items < 0 || size <= 0 || handle_at < 0) return nullptr;
    const Array array = ue::At<Array>(asc, specs + items);
    if (array.num < 0 || array.num > 4096 || (array.num && !array.data)) return nullptr;
    for (int32_t i = 0; i < array.num; ++i) {
        unsigned char* row = static_cast<unsigned char*>(array.data) + static_cast<size_t>(i) * static_cast<size_t>(size);
        if (ue::At<int32_t>(row, handle_at) == handle) return row;
    }
    return nullptr;
}
// The spec's ability (the class default object) and its instances (ReplicatedInstances + NonReplicatedInstances).
Object* SpecAbility(Object* asc, int32_t handle, std::vector<Object*>* instances = nullptr) {
    Object* type = nullptr; unsigned char* row = SpecRow(asc, handle, type);
    if (!row) return nullptr;
    if (instances)
        for (const char* name : {"ReplicatedInstances", "NonReplicatedInstances"}) {
            const int32_t at = ue::Offset(type, name); if (at < 0) continue;
            const Array list = ue::At<Array>(row, at);
            for (int32_t k = 0; list.data && k < list.num && k < 16; ++k) { Object* a = static_cast<Object**>(list.data)[k]; if (a && ue::Alive(a)) instances->push_back(a); }
        }
    const int32_t ability_at = ue::Offset(type, "Ability");
    Object* ability = ability_at >= 0 ? ue::At<Object*>(row, ability_at) : nullptr;
    return ability && ue::Alive(ability) ? ability : nullptr;
}
// The game's own ability cooldown (BP_ActionAbilityBase: its K2_CanActivateAbility compares the instance's "Cooldown
// Mark Time", the world time of its last start, with its Cooldown Data; no tag, no cooldown effect): a boss move with a
// long cooldown (左殇真龙 Skill_14, 沐小葵's double-sword moves) was cast once, then every later TryActivateAbility said
// no. Back to the class default (-9999.99: never cast) on every instance of the spec. Returns how many it reset.
size_t ResetCooldown(Object* asc, int32_t handle, Object* ability_class) {
    const uintptr_t field = ability_class ? FieldOf(ability_class, "Cooldown Mark Time") : 0;
    if (!field) return 0;
    const int32_t at = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset), size = ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropElementSize);
    std::vector<Object*> instances; Object* cdo = SpecAbility(asc, handle, &instances);
    if (!cdo || (size != 8 && size != 4)) return 0;
    size_t reset = 0;
    for (Object* instance : instances) {
        if (!ue::IsA(instance, ability_class)) continue;
        if (size == 8) { const double base = ue::At<double>(cdo, at); if (ue::At<double>(instance, at) != base) { ue::At<double>(instance, at) = base; ++reset; } }
        else { const float base = ue::At<float>(cdo, at); if (ue::At<float>(instance, at) != base) { ue::At<float>(instance, at) = base; ++reset; } }
    }
    return reset;
}
// The game's distance optimisation (OptimizationProxyComponent, the NPC base's "OptimizationProxy"): beyond its
// MaxVisibleDistance (100 m) from the player its first wave check, about 0.85 s after the spawn, turns the NPC's mesh
// and everything attached to it (the weapon actors) invisible. The preview stage is kilometres from the player: every
// preview boss vanished with its weapons once its move began. Its own DisableAllOptimizations turns it off for the boss.
// DisableAllOptimizations alone left it hiding the weapons (static meshes) and, after the next wave check, the body
// again: its distance thresholds and the camera-sight rule are set so that every wave is out of reach as well.
bool OptimisationOff(Object* boss) {
    Object* proxy = Property(boss, "OptimizationProxy");
    if (!proxy) return false;
    const bool off = Call(proxy, "DisableAllOptimizations").Invoke();
    size_t pushed = 0;
    for (const char* name : {"MaxVisibleDistance", "DistanceToFirstOptimization", "DistanceToSecondOptimization", "DistanceToThirdOptimization", "DistanceToFourthOptimization"}) {
        const uintptr_t field = FieldOf(ue::ClassOf(proxy), name);
        if (!field || ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropElementSize) != 4) continue;
        ue::At<float>(proxy, ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset)) = 1.0e9f; ++pushed;
    }
    const uintptr_t sight = FieldOf(ue::ClassOf(proxy), "IgnoreCameraSight");
    if (sight && ue::At<uint8_t>(reinterpret_cast<void*>(sight), 0x70) == 1) {
        const int32_t at = ue::At<int32_t>(reinterpret_cast<void*>(sight), ue::layout::kPropOffset);
        const uint8_t byte = ue::At<uint8_t>(reinterpret_cast<void*>(sight), 0x71), mask = ue::At<uint8_t>(reinterpret_cast<void*>(sight), 0x73);
        if (byte <= 7 && mask) ue::At<uint8_t>(proxy, at + byte) |= mask;
    }
    return off && pushed == 5;
}
// The proxy's current wave (0: none), -1: unknown.
int Wave(Object* boss) {
    Object* proxy = Property(boss, "OptimizationProxy");
    Call get(proxy, "GetOptimizationWave");
    if (!proxy || !get.Invoke()) return -1;
    return static_cast<int>(get.Get<uint8_t>("ReturnValue"));
}
bool preview_arm_ = true;  // tests: "npcretarget preview arm 0|1"
// The boss's Main weapon slot armed (CharacterWeaponsComponent::SetArmed, what its AI does when a fight begins): a boss
// spawned with its AI held stays unarmed, and the weapon refresh at a move's start hid the weapon in its hand (长虹's
// spear, 鬼差's 大刀, 铁鬼's gauntlets ...). Returns -1: no weapons component, else whether it was armed before.
int Arm(Object* boss) {
    static const Name main = ue::MakeName(L"Weapon.Slot.Main");
    Call get(boss, "GetCharacterWeaponsComponent", "/Script/SGameCombatSystem.CharacterWeaponsHolder");
    Object* weapons = get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    if (!weapons || !ue::Alive(weapons) || main.None()) return -1;
    Call is(weapons, "IsArmed"); const bool before = is.Set("SlotTag", main).Invoke() && is.Get<bool>("ReturnValue");
    if (!before) Call(weapons, "SetArmed").Set("SlotTag", main).Set("Armed", true).Invoke();
    return before ? 1 : 0;
}
// Per weapon actor, for the log: name, actor hidden (h), mesh bVisible off (V0).
std::string WeaponDetail(Object* actor) {
    std::string out; Call attached(actor, "GetAttachedActors");
    if (!attached.Set("bResetArray", true).Set("bRecursivelyIncludeAttachedActors", true).Invoke()) return "-";
    const Array list = attached.Get<Array>("OutActors");
    for (int32_t i = 0; list.data && i < list.num && i < 256; ++i) {
        Object* a = static_cast<Object**>(list.data)[i];
        if (!a || !ue::Alive(a) || Offset(a, "WeaponMesh") < 0) continue;
        bool hidden = false, visible = true; BoolOf(a, "bHidden", hidden); BoolOf(Property(a, "WeaponMesh"), "bVisible", visible);
        std::string name = Text(ue::ClassOf(a)); if (name.rfind("BP_Weapon_", 0) == 0) name = name.substr(10);
        out += (out.empty() ? "" : ",") + name + (hidden ? ":h" : "") + (visible ? "" : ":V0");
    }
    ue::Free(list.data);
    return out.empty() ? "none" : out;
}
std::vector<Object*> Pawns() {
    std::vector<Object*> out; Context context; if (!ContextNow(context)) return out;
    Object* pawn_class = ue::FindObject("/Script/Engine.Pawn");
    Call all(ue::Default("/Script/Engine.Default__GameplayStatics"), "GetAllActorsOfClass");
    if (!pawn_class || !all.Set("WorldContextObject", context.world).Set("ActorClass", pawn_class).Invoke()) return out;
    const Array list = all.Get<Array>("OutActors");
    for (int32_t i = 0; list.data && i < list.num && i < 4096; ++i) { Object* a = static_cast<Object**>(list.data)[i]; if (a && ue::Alive(a)) out.push_back(a); }
    ue::Free(list.data);
    return out;
}
// ---- The move preview's boss -------------------------------------------------------------------------
// preview_world_pawns_: the pawns there when it came (any other pawn it instigated or owns is its own: its meta AI
// pawn, a move's summons; removed with it). preview_shown_: its body mesh and the parts under it that were visible
// then. Even with the optimisation off, each attached weapon actor's WeaponMesh is turned invisible once, at the
// proxy's first wave check (0.65-0.85 s after the spawn): a WeaponMesh hidden before kOptimiserMs is shown again at
// once. A move hides parts on purpose too (左殇真龙 Skill_14 five parts of its sword at 0.47 s, 沐小葵's switch to two
// swords): other weapon parts are shown again when the cast ends, the boss's own parts when the cast ends or after
// kHiddenMs hidden (never a boss gone for good in a long move).
constexpr ULONGLONG kHiddenMs = 4000, kOptimiserMs = 1500;
enum class Part : uint8_t { Body, WeaponMesh, Weapon };
struct Shown { Object* component; Part kind; };
std::vector<Object*> preview_world_pawns_;
std::vector<Shown> preview_shown_;
ULONGLONG preview_hidden_at_ = 0; uint32_t preview_reshows_ = 0, preview_refusals_ = 0, preview_tag_logs_ = 0;
// USceneComponent::bVisible (bitfield), looked up once.
struct VisibleFlag { int32_t offset = -1; uint8_t byte = 0, mask = 0; } visible_flag_;
bool PartVisible(Object* component, bool& visible) {
    if (visible_flag_.offset < 0) {
        Object* scene = ue::FindObject("/Script/Engine.SceneComponent");
        const uintptr_t field = scene ? FieldOf(scene, "bVisible") : 0;
        if (!field || ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x70) != 1) return false;
        visible_flag_ = {ue::At<int32_t>(reinterpret_cast<void*>(field), ue::layout::kPropOffset), ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x71),
                         ue::At<uint8_t>(reinterpret_cast<void*>(field), 0x73)};
        if (visible_flag_.byte > 7 || !visible_flag_.mask) { visible_flag_.offset = -1; return false; }
    }
    if (!component || !ue::Alive(component)) return false;
    visible = (ue::At<uint8_t>(component, visible_flag_.offset + visible_flag_.byte) & visible_flag_.mask) != 0;
    return true;
}
std::vector<Object*> Descendants(Object* component) {
    std::vector<Object*> out; Call get(component, "GetChildrenComponents");
    if (!component || !get.Set("bIncludeAllDescendants", true).Invoke()) return out;
    const Array list = get.Get<Array>("Children");
    for (int32_t i = 0; list.data && i < list.num && i < 1024; ++i) { Object* c = static_cast<Object**>(list.data)[i]; if (c && ue::Alive(c)) out.push_back(c); }
    ue::Free(list.data);
    return out;
}
void RememberShown(Object* boss) {
    preview_shown_.clear(); preview_hidden_at_ = 0; preview_reshows_ = 0;
    Object* mesh = Property(boss, "Mesh"); if (!mesh) return;
    std::vector<Object*> all = Descendants(mesh); all.insert(all.begin(), mesh);
    for (Object* c : all) {
        bool visible = false;
        if (!PartVisible(c, visible) || !visible) continue;
        Call owner(c, "GetOwner"); Object* actor = owner.Invoke() ? owner.Get<Object*>("ReturnValue") : nullptr;
        const Part kind = !actor || actor == boss ? Part::Body : Property(actor, "WeaponMesh") == c ? Part::WeaponMesh : Part::Weapon;
        preview_shown_.push_back({c, kind});
    }
}
// Shows again the remembered parts that are hidden (see preview_shown_). cast_end: a cast just ended.
void KeepShown(const Slot& s, ULONGLONG now, bool cast_end) {
    size_t weapons = 0, own = 0; bool body_hidden = false;
    const bool early = now < s.born + kOptimiserMs, body_due = cast_end || (preview_hidden_at_ && now >= preview_hidden_at_ + kHiddenMs);
    for (const Shown& part : preview_shown_) {
        bool visible = true;
        if (!PartVisible(part.component, visible) || visible) continue;
        if (part.kind == Part::Body) { body_hidden = true; if (!body_due) continue; }
        else if (!cast_end && !(part.kind == Part::WeaponMesh && early)) continue;
        if (Call(part.component, "SetVisibility").Set("bNewVisibility", true).Set("bPropagateToChildren", false).Invoke()) ++(part.kind == Part::Body ? own : weapons);
    }
    if (!body_hidden || own) preview_hidden_at_ = 0;
    else if (!preview_hidden_at_) preview_hidden_at_ = now;
    if ((weapons || own) && ++preview_reshows_ <= 4)
        Logf("BOSS_ASSIST preview %s shown again after %llums: %zu weapon parts, %zu own parts%s", s.ability_name.c_str(), now - s.born, weapons, own, cast_end ? " (cast end)" : "");
}
// Moves that need a state tag the held boss never gets on the stage (钩子男's ground moves: CharacterState.Space.Ground):
// the required tags it lacks, as loose tags on it. Returns how many it gave.
size_t GiveRequiredTags(const Slot& s) {
    Object* asc = Asc(s.boss), *ability = asc ? SpecAbility(asc, s.handle) : nullptr;
    if (!ability) return 0;
    std::vector<Name> missing; std::string names;
    for (const char* property : {"ActivationRequiredTags", "SourceRequiredTags"}) {
        const int32_t at = Offset(ability, property); if (at < 0) continue;
        const Array tags = ue::At<Array>(ability, at); if (tags.num <= 0 || tags.num > 64 || !tags.data) continue;
        for (int32_t i = 0; i < tags.num; ++i) {
            const Name tag = static_cast<const Name*>(tags.data)[i];
            Call has(asc, "HasMatchingGameplayTag", "/Script/GameplayTags.GameplayTagAssetInterface");
            if (has.Set("TagToCheck", tag).Invoke() && has.Get<bool>("ReturnValue")) continue;
            missing.push_back(tag); names += (names.empty() ? "" : ",") + NameString(tag);
        }
    }
    if (missing.empty()) return 0;
    struct TagContainer { Array tags, parents; };
    Object* container_type = ue::FindObject("/Script/GameplayTags.GameplayTagContainer");
    if (!container_type || ue::StructSize(container_type) != static_cast<int32_t>(sizeof(TagContainer))) return 0;
    const Array input{missing.data(), static_cast<int32_t>(missing.size()), static_cast<int32_t>(missing.size())};
    Call make(ue::Default("/Script/GameplayTags.Default__BlueprintGameplayTagLibrary"), "MakeGameplayTagContainerFromArray");
    if (!make.Set("GameplayTags", input).Invoke()) return 0;
    TagContainer made = make.Get<TagContainer>("ReturnValue");
    Call add(ue::Default("/Script/GameplayAbilities.Default__AbilitySystemBlueprintLibrary"), "AddLooseGameplayTags");
    const bool ok = add.Ok() && add.Set("Actor", s.boss).Set("GameplayTags", made).Set("bShouldReplicate", false).Invoke();
    ue::Free(made.tags.data); ue::Free(made.parents.data);
    if (++preview_tag_logs_ <= 2) Logf("BOSS_ASSIST preview %s gave the boss its missing required tags [%s] ok=%d", s.ability_name.c_str(), names.c_str(), ok ? 1 : 0);
    return ok ? missing.size() : 0;
}
// Pawns the preview boss left (instigated or owned by it, not there when it came).
void RemoveLeftovers(Object* boss) {
    size_t removed = 0;
    for (Object* pawn : Pawns()) {
        if (pawn == boss || std::find(preview_world_pawns_.begin(), preview_world_pawns_.end(), pawn) != preview_world_pawns_.end()) continue;
        if (Property(pawn, "Instigator") != boss && Property(pawn, "Owner") != boss) continue;
        if (Call(pawn, "K2_DestroyActor").Invoke()) ++removed;
    }
    if (removed) Logf("BOSS_ASSIST preview removed %zu pawns of its boss", removed);
    preview_world_pawns_.clear();
}
void DestroyPreviewBoss(Slot& s) {
    preview_pause_.Reset();preview_charge_={};
    if (SameBoss(s)) {
        if (Object* asc = Asc(s.boss)) Call(asc, "CancelAllAbilities").Invoke();
        Object* boss = s.boss;
        RemoveLeftovers(boss);
        Call(boss, "K2_DestroyActor").Invoke();
    }
    s = Slot{};
}
// Why TryActivateAbility said no (first and last try): the ability's tag conditions on the boss ('!' = failed) and the
// cooldown marks of its instances.
void PreviewRefusal(const Slot& s) {
    Object* asc = Asc(s.boss);
    if (!asc || ++preview_refusals_ > 2) return;
    std::vector<Object*> instances; Object* ability = SpecAbility(asc, s.handle, &instances);
    std::string text;
    static constexpr std::pair<const char*, bool> kChecks[] = {{"ActivationRequiredTags", true}, {"ActivationBlockedTags", false}, {"SourceRequiredTags", true}, {"SourceBlockedTags", false}};
    for (const auto& [property, want] : kChecks) {
        const int32_t at = Offset(ability, property); if (at < 0) continue;
        const Array tags = ue::At<Array>(ability, at); if (tags.num <= 0 || tags.num > 64 || !tags.data) continue;
        text += std::string(text.empty() ? "" : " ") + property + "=[";
        for (int32_t i = 0; i < tags.num; ++i) {
            const Name tag = static_cast<const Name*>(tags.data)[i];
            Call has(asc, "HasMatchingGameplayTag", "/Script/GameplayTags.GameplayTagAssetInterface");
            const char* mark = has.Set("TagToCheck", tag).Invoke() ? (has.Get<bool>("ReturnValue") == want ? "" : "!") : "?";
            text += (i ? "," : "") + std::string(mark) + NameString(tag);
        }
        text += "]";
    }
    std::string marks; const int32_t mark_at = Offset(ability, "Cooldown Mark Time");
    for (Object* instance : instances) { char one[64]; std::snprintf(one, sizeof(one), "%s%.2f", marks.empty() ? "" : ",", mark_at >= 0 ? ue::At<double>(instance, mark_at) : -1.0); marks += one; }
    Logf("BOSS_ASSIST preview refused %s try=%u %s cooldownMarks=[%s]", s.ability_name.c_str(), s.tries, text.empty() ? "tags=none" : text.c_str(), marks.c_str());
}
// The preview boss's time: the move's 加速 (PreviewSpeed), or its curve at the montage's position now (the start of
// the curve between casts). Written every frame; the slowest speed the move takes also stretches its waits (Scaled).
void HoldPreviewSpeed(Slot& s) {
    const int32_t at = Offset(s.boss, "CustomTimeDilation");
    if (at < 0) return;
    double factor = preview_speed_ / 100.0, slowest = factor;
    if (preview_curve_.size() >= 2) {
        factor = NpcCombo::CurveSpeed(preview_curve_, 0) / 100.0; slowest = factor;
        for (const NpcCombo::CurvePoint& p : preview_curve_) slowest = (std::min)(slowest, p.speed / 100.0);
        Object* anim = nullptr;
        if (Object* mesh = Property(s.boss, "Mesh")) { Call get(mesh, "GetAnimInstance"); if (get.Invoke()) anim = get.Get<Object*>("ReturnValue"); }
        Object* montage = nullptr;
        if (anim && ue::Alive(anim)) { Call get(anim, "GetCurrentActiveMontage"); if (get.Invoke()) montage = get.Get<Object*>("ReturnValue"); }
        if (montage && ue::Alive(montage)) {
            if (montage != preview_montage_) { Call length(montage, "GetPlayLength"); preview_montage_ = montage; preview_length_ = length.Invoke() ? length.Get<float>("ReturnValue") : 0; }
            Call pos(anim, "Montage_GetPosition");
            if (preview_length_ > 0 && pos.Set("Montage", montage).Invoke())
                factor = NpcCombo::CurveSpeed(preview_curve_, NpcCombo::TrimProgress(PreviewRange(), pos.Get<float>("ReturnValue"), preview_length_)) / 100.0;
        }
    }
    const float value = static_cast<float>(std::clamp(factor, 0.05, 10.0));
    s.dilation = static_cast<float>(std::clamp(slowest, 0.05, 10.0));
    float& dilation = ue::At<float>(s.boss, at);
    if (std::fabs(dilation - value) > 0.001f) dilation = value;
    if (std::fabs(preview_logged_ - value) > 0.25f && preview_speed_logs_ < 24) {
        ++preview_speed_logs_; preview_logged_ = value;
        Logf("BOSS_ASSIST preview speed x%.2f (%s, curve points %zu)", value, preview_curve_.size() >= 2 ? "curve" : "fixed", preview_curve_.size());
    }
}
// A montage preview: the clip playing on the preview boss now.
Object* PreviewAnim(Slot& s) {
    Object* mesh = Property(s.boss, "Mesh");
    if (!mesh) return nullptr;
    Call get(mesh, "GetAnimInstance");
    Object* anim = get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    return anim && ue::Alive(anim) ? anim : nullptr;
}
bool ClipPlaying(Slot& s) {
    Object* anim = PreviewAnim(s);
    if (!anim || !ue::Alive(preview_clip_)) return false;
    Call playing(anim, "Montage_IsPlaying");
    return playing.Set("Montage", preview_clip_).Invoke() && playing.Get<bool>("ReturnValue");
}
bool PlayClip(Slot& s) {
    Object* anim = PreviewAnim(s);
    if (!anim || !ue::Alive(preview_clip_)) return false;
    Call play(anim, "Montage_Play");
    const bool ok = play.Set("MontageToPlay", preview_clip_).Set("InPlayRate", 1.0f).Set("ReturnValueType", uint8_t{0}).Set("InTimeToStartMontageAt", 0.0f)
                        .Set("bStopAllMontages", true).Invoke() && play.Get<float>("ReturnValue") > 0;
    if (!ok && (++preview_clip_tries_ == 1 || preview_clip_tries_ == 20)) Logf("BOSS_ASSIST preview clip %s did not play (try %u)", Text(preview_clip_).c_str(), preview_clip_tries_);
    return ok;
}
// Seek the owning Lego timeline too: a montage-only seek is overwritten on the next tick.
bool PreviewReadable(const void* pointer,size_t bytes) {
    if (!pointer||!bytes) return false;
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(pointer,&info,sizeof(info))||info.State!=MEM_COMMIT||(info.Protect&(PAGE_NOACCESS|PAGE_GUARD))) return false;
    const uintptr_t p=reinterpret_cast<uintptr_t>(pointer),base=reinterpret_cast<uintptr_t>(info.BaseAddress);
    return p>=base&&p-base<=info.RegionSize&&bytes<=info.RegionSize-(p-base);
}
bool SeekPreviewClock(Slot& s,Object* anim,Object* montage,float position,double from) {
    Object* asc=Asc(s.boss); if (!asc||s.handle<=0) return false;
    Object* type=ue::FindObject("/Script/LegoAbility.LGATask_PlayTimeline");
    const int32_t time_at=type?ue::Offset(type,"CurrentTime"):-1; if (time_at<0) return false;
    std::vector<Object*> instances; SpecAbility(asc,s.handle,&instances);
    Object* best=nullptr; double gap=1e9;
    for (Object* ability:instances) {
        const int32_t tasks_at=Offset(ability,"ActiveTasks");
        if (tasks_at<0||!PreviewReadable(ability+tasks_at,sizeof(Array))) continue;
        const Array tasks=ue::At<Array>(ability,tasks_at);
        if (tasks.num<=0||tasks.num>64||tasks.max<tasks.num||!PreviewReadable(tasks.data,static_cast<size_t>(tasks.num)*sizeof(Object*))) continue;
        for (int32_t i=0;i<tasks.num;++i) {
            Object* task=static_cast<Object**>(tasks.data)[i];
            if (!ue::Alive(task)||!ue::IsA(task,type)||!PreviewReadable(task+time_at,sizeof(float))) continue;
            const float clock=ue::At<float>(task,time_at); if (!std::isfinite(clock)) continue;
            const double distance=std::fabs(static_cast<double>(clock)-position);
            if (distance<gap) { best=task; gap=distance; }
        }
    }
    if (!best) return false;
    Call rate(anim,"Montage_GetPlayRate");
    const double speed=rate.Set("Montage",montage).Invoke()?rate.Get<float>("ReturnValue"):1.0;
    const float before=ue::At<float>(best,time_at),after=ComboPlayback::TrimClock(before,position,from,speed);
    if (!Call(anim,"Montage_SetPosition").Set("Montage",montage).Set("NewPosition",static_cast<float>(from)).Invoke()) return false;
    ue::At<float>(best,time_at)=after;
    if (preview_trim_logs_<12) Logf("BOSS_ASSIST preview trim clock %.3f -> %.3f montage %.3f -> %.3f",before,after,position,from);
    return true;
}
// 裁剪 on the preview's cast: its montage's length recorded (the page's timeline); the part's start jumped to once a
// cast, and the cast ended (montage blended out, ability cancelled) at the part's end.
void TrimPreview(Slot& s) {
    if(preview_pause_.active){if(preview_hold_down_&&preview_pause_.Hold())return;preview_pause_.Release();preview_charge_.holding=false;}
    Object* anim = PreviewAnim(s); if (!anim || preview_clip_) return;
    Call get(anim, "GetCurrentActiveMontage"); Object* montage = get.Invoke() ? get.Get<Object*>("ReturnValue") : nullptr;
    if (!montage || !ue::Alive(montage)) return;
    if (montage != preview_montage_) { Call length(montage, "GetPlayLength"); preview_montage_ = montage; preview_length_ = length.Invoke() ? length.Get<float>("ReturnValue") : 0; }
    if (preview_length_ > 0) NpcCombo::RecordClip(s.ability_name.substr(0, s.ability_name.find('.')), static_cast<float>(preview_length_));
    NpcCombo::TrimRange r = PreviewRange(); if (preview_length_>0&&r.from>=preview_length_) r={};
    if (!r.on&&!preview_trim_dirty_) return;
    Call pos(anim, "Montage_GetPosition"); if (!pos.Set("Montage", montage).Invoke()) return;
    const float at = pos.Get<float>("ReturnValue");
    if (!preview_trim_jumped_) {
        const bool seek=r.from>0||preview_trim_dirty_;
        bool jump=false,clock=false;
        if (seek) {
            clock=SeekPreviewClock(s,anim,montage,at,r.from); jump=clock;
            if (!jump&&++preview_trim_wait_<12) return;  // task may be created after its montage
            if (!jump) jump=Call(anim,"Montage_SetPosition").Set("Montage",montage).Set("NewPosition",static_cast<float>(r.from)).Invoke();
            if (!jump) return;
        }
        preview_trim_jumped_=true; preview_trim_dirty_=false;
        if (++preview_trim_logs_<=12) Logf("BOSS_ASSIST preview trim %.2f-%.2fs of %.2fs (start %s)",r.from,r.from+NpcCombo::TrimSpan(r,preview_length_),preview_length_,clock?"timeline+montage":jump?"montage fallback":"kept");
        return;
    }
    if(preview_charge_.Step(r.holds,at,r.from,r.from+NpcCombo::TrimSpan(r,preview_length_),preview_hold_down_)){
        if(SeekPreviewClock(s,anim,montage,at,preview_charge_.at)&&preview_pause_.Begin(s.boss,anim,montage))return;
        preview_charge_.holding=false;
    }
    if (r.to > 0 && at >= r.to - 0.005 && !preview_trim_cut_) {
        preview_trim_cut_ = true;
        if (++preview_trim_logs_ <= 6) Logf("BOSS_ASSIST preview trim cut at %.2fs (the part ends at %.2fs)", at, r.to);
        Call(anim, "Montage_Stop").Set("InBlendOutTime", 0.12f).Set("Montage", montage).Invoke();
        if (Object* asc = Asc(s.boss); asc && s.handle > 0) Call(asc, "CancelAbilityHandle").Set("AbilityHandle", s.handle).Invoke();
    }
}
// The preview's boss: cast, then back on its spot facing the target, kPreviewGapMs, cast again.
void TickPreview(ULONGLONG now) {
    Slot& s = preview_;
    if (s.phase == Phase::Idle) return;
    if (!SameBoss(s)) { preview_pause_.Reset();preview_charge_={};Logf("BOSS_ASSIST preview boss gone (destroyed by the game)"); s = Slot{}; return; }
    HoldAI(s);
    if(!preview_pause_.active)HoldPreviewSpeed(s);
    Object* asc = Asc(s.boss);
    bool present = false; uint8_t active = 0;
    if (preview_clip_) active = ClipPlaying(s) ? 1 : 0;
    else if (asc && s.handle > 0) SpecState(asc, s.handle, present, active);
    KeepShown(s, now, false);
    if (active) { s.seen_active = true; TrimPreview(s); return; }
    if (s.seen_active) {  // a cast ended: back home for the next
        preview_pause_.Reset();preview_charge_={};preview_trim_jumped_ = preview_trim_cut_ = false; preview_trim_wait_ = 0;
        KeepShown(s, now, true);
        s.seen_active = false; s.tries = 0; preview_next_ = now + kPreviewGapMs; ++preview_loops_;
        if (preview_loops_ <= 3)
            Logf("BOSS_ASSIST preview %s cast %u ended after %llums, weapons shown %s%s%s wave=%d", s.ability_name.c_str(), preview_loops_, now - s.phase_at, WeaponCount(s.boss).c_str(),
                 preview_loops_ == 1 ? " " : "", preview_loops_ == 1 ? WeaponDetail(s.boss).c_str() : "", Wave(s.boss));
        Call put(s.boss, "K2_SetActorLocation"); put.Set("NewLocation", preview_home_).Set("bSweep", false).Set("bTeleport", true).Invoke();
        Face(s.boss, preview_yaw_);
        if (Object* movement = Property(s.boss, "CharacterMovement")) Call(movement, "StopMovementImmediately").Invoke();
        MoveMode(s.boss, 1);
        return;
    }
    if (now < preview_next_) return;
    preview_next_ = now + kActivateRetryMs;
    if (preview_clip_) {
        if (PlayClip(s)) { s.phase_at = now; return; }
        if (preview_clip_tries_ > 20) { Logf("BOSS_ASSIST preview clip %s never played: stopped", s.ability_name.c_str()); DestroyPreviewBoss(s); }
        return;
    }
    const bool ok = Activate(s);
    if (ok) { s.phase_at = now; return; }
    if (s.tries == 1 || s.tries == 21) PreviewRefusal(s);
    if (s.tries == 2) GiveRequiredTags(s);  // every cast: the game takes a loose state tag away again (钩子男's ground tag)
    if (s.tries > 20) { Logf("BOSS_ASSIST preview %s never activated: stopped", s.ability_name.c_str()); DestroyPreviewBoss(s); }
}
void Tick() {
    if (disabled_) return;
    std::vector<Job> jobs; bool stop = false;
    AcquireSRWLockExclusive(&requests_lock_); jobs.swap(requests_); stop = stop_requested_; stop_requested_ = false; ReleaseSRWLockExclusive(&requests_lock_);
    if (stop) for (Slot& s : slots_) Leave(s, "asked");
    for (const Job& job : jobs) Start(job);
    const ULONGLONG now = GetTickCount64();
    for (Slot& s : slots_) TickSlot(s, now);
    TickPreview(now);
}
}  // namespace

void Initialize(Logger logger) { logger_ = logger; }
void Request(const std::string& monster, const std::string& ability, const Mods& mods, bool beside) {
    AcquireSRWLockExclusive(&requests_lock_);
    if (requests_.size() < kSlots * 2) requests_.push_back({monster, ability, mods, beside});
    ReleaseSRWLockExclusive(&requests_lock_);
}
void Stop() { AcquireSRWLockExclusive(&requests_lock_); stop_requested_ = true; requests_.clear(); ReleaseSRWLockExclusive(&requests_lock_); }
size_t Casting(Cast* out, size_t max) {
    size_t n = 0;
    for (const Slot& s : slots_) {
        if (n >= max || s.phase == Phase::Idle || s.phase == Phase::Leaving || !SameBoss(s)) continue;
        out[n].boss = s.boss; out[n].mods = s.mods; out[n].casting = s.phase == Phase::Casting && s.seen_active; out[n].since = s.born; ++n;
    }
    return n;
}
bool StartPreview(const std::string& monster, const std::string& ability, const double (&at)[3], double yaw, Object* target) {
    StopPreview();
    Context context;
    if (!ContextNow(context)) return false;
    const bool clip = ability.find("_Montage.") != std::string::npos;
    Object* cls = LoadClass(monster), *skill = clip ? nullptr : LoadClass(ability), *library = AILibrary();
    Object* montage = clip ? LoadAsset(ability.c_str()) : nullptr;
    if (!cls || (!skill && !montage) || !library) {
        Logf("BOSS_ASSIST preview refused: class=%d ability=%d montage=%d library=%d (%s, %s)", cls ? 1 : 0, skill ? 1 : 0, montage ? 1 : 0, library ? 1 : 0, monster.c_str(), ability.c_str());
        return false;
    }
    preview_clip_ = montage; preview_clip_tries_ = 0;
    Transform where; where.translation[0] = at[0]; where.translation[1] = at[1]; where.translation[2] = at[2] + 300;  // set down below
    where.rotation[2] = std::sin(yaw * kPi / 360.0); where.rotation[3] = std::cos(yaw * kPi / 360.0);
    Call spawn(library, "SpawnAI");
    if (!spawn.Set("Class", cls).Set("SpawnTransform", where).Set("__WorldContext", context.instance).Invoke()) { Logf("BOSS_ASSIST preview refused: SpawnAI call"); return false; }
    Object* actor = spawn.Get<Object*>("ReturnValue");
    if (!actor || !ue::Alive(actor)) { Logf("BOSS_ASSIST preview refused: SpawnAI returned none"); return false; }
    Slot& s = preview_;
    s.boss = actor; s.name = ue::NameOf(actor); s.ability = skill; s.ability_name = ability.substr(ability.find_last_of('/') + 1);
    s.enemy = target && ue::Alive(target) ? target : nullptr; s.enemy_name = s.enemy ? ue::NameOf(s.enemy) : Name{};
    s.phase = Phase::Casting; s.born = s.phase_at = GetTickCount64();
    // Its spot: standing on the floor at `at` (capsule centre half its height above).
    double half = 90;
    if (Object* capsule = Property(s.boss, "CapsuleComponent")) { Call h(capsule, "GetScaledCapsuleHalfHeight"); if (h.Invoke()) half = h.Get<float>("ReturnValue"); }
    preview_home_ = {at[0], at[1], at[2] + half + 2}; preview_yaw_ = yaw; preview_next_ = s.born + 300; preview_loops_ = 0; preview_refusals_ = 0; preview_tag_logs_ = 0;
    preview_montage_ = nullptr; preview_length_ = 0; preview_logged_ = -1; preview_speed_logs_ = 0; preview_trim_jumped_ = preview_trim_cut_ = false; preview_trim_wait_ = 0; preview_trim_logs_ = 0;
    if (montage) s.ability_name = ability.substr(ability.find_last_of('.') + 1);
    preview_world_pawns_ = Pawns();
    const bool kept = OptimisationOff(s.boss);
    const int armed = preview_arm_ ? Arm(s.boss) : -2;
    RememberShown(s.boss);
    { Call put(s.boss, "K2_SetActorLocation"); put.Set("NewLocation", preview_home_).Set("bSweep", false).Set("bTeleport", true).Invoke(); }
    // Walking on the stage floor: a boss comes out of SpawnAI flying (movement mode 5), without the game's
    // CharacterState.Space.Ground tag that ground moves require (钩子男's).
    MoveMode(s.boss, 1);
    HoldAI(s);
    if (s.enemy) if (Object* controller = Property(s.boss, "Controller")) Call(controller, "K2_SetFocus").Set("NewFocus", s.enemy).Invoke();
    Logf("BOSS_ASSIST preview %s class=%s ability=%s at=(%.0f,%.0f,%.0f) yaw=%.0f target=%s optimisationOff=%d armedBefore=%d parts=%zu weapons shown %s [%s]", Text(actor).c_str(),
         Text(cls).c_str(), s.ability_name.c_str(), at[0], at[1], at[2], yaw, Text(s.enemy).c_str(), kept ? 1 : 0, armed, preview_shown_.size(), WeaponCount(s.boss).c_str(), WeaponDetail(s.boss).c_str());
    return true;
}
void StopPreview() {
    Slot& s = preview_;
    if (s.phase == Phase::Idle) return;
    Logf("BOSS_ASSIST preview %s stopped after %u casts", s.ability_name.c_str(), preview_loops_);
    DestroyPreviewBoss(s);
}
Object* PreviewBoss() { return preview_.phase != Phase::Idle && SameBoss(preview_) ? preview_.boss : nullptr; }
void PreviewOption(const std::string& what, int value) { if (what == "arm") preview_arm_ = value != 0; }
void PreviewTrim(int from_ms, int to_ms,const std::vector<int>& holds,bool held) {
    preview_hold_down_=held;const auto cleaned=NpcCombo::TrimHolds(holds);
    from_ms=NpcCombo::ClampTrim(from_ms); to_ms=NpcCombo::ClampTrim(to_ms);
    if (from_ms==preview_trim_from_&&to_ms==preview_trim_to_&&cleaned==preview_trim_holds_) return;
    preview_pause_.Reset();preview_charge_={};preview_trim_holds_=cleaned;
    preview_trim_from_=from_ms; preview_trim_to_=to_ms;
    // Editing either edge (including clearing the range) applies to the current preview.
    preview_trim_dirty_=true; preview_trim_jumped_=preview_trim_cut_=false; preview_trim_wait_=0;
}
void PreviewSpeed(int percent, const std::vector<NpcCombo::CurvePoint>& curve) {
    preview_speed_ = NpcCombo::ClampSpeed(percent);
    if (curve.size() != preview_curve_.size() || !std::equal(curve.begin(), curve.end(), preview_curve_.begin(),
                                                           [](const NpcCombo::CurvePoint& a, const NpcCombo::CurvePoint& b) { return a.t == b.t && a.speed == b.speed; }))
        preview_curve_ = curve;
}
void OnGameThreadGuarded() {
    __try { Tick(); }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        Logf("BOSS_ASSIST fault 0x%08lx", GetExceptionCode());
        for (Slot& s : slots_) { s.boss = nullptr; s.phase = Phase::Idle; }
        preview_.boss = nullptr; preview_.phase = Phase::Idle;
        if (++faults_ >= kMaxFaults) { disabled_ = true; Logf("BOSS_ASSIST disabled after %lu faults", faults_); }
    }
}
}  // namespace BossAssist
