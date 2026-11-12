#pragma once
#include "bone_motion_model.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <string>
#include <utility>
#include <vector>

// 招式派生 (move chains) for NPC skins, the part without engine or window calls: move kinds, the chain the page
// edits, how a press walks it (Follow / MoveRate) and its ini text. Shared by the page (combo_menu), the game-thread
// runner in npc_retarget and tests/npc_combo_test.
namespace NpcCombo {
// What a boss ability is, from its name (GA_<boss>[_P2]_<kind>_...): the page lists a skin's moves by these.
enum class Kind : int { Light, Heavy, Dodge, DodgeAttack, Special, Skill, Ultimate, Combo, Other, Count };
inline const wchar_t* KindLabel(Kind kind) {
    static constexpr const wchar_t* kLabels[] = {L"轻攻击", L"重攻击", L"闪避", L"闪避攻击", L"特殊攻击", L"技能", L"绝技", L"连招", L"其他"};
    const int index = static_cast<int>(kind);
    return index >= 0 && index < static_cast<int>(Kind::Count) ? kLabels[index] : L"";
}
inline bool StartsWithNoCase(const std::string& text, const char* prefix) {
    const size_t n = std::strlen(prefix);
    return text.size() >= n && !_strnicmp(text.c_str(), prefix, n);
}
inline bool Contains(const std::string& text, const char* part) {
    std::string a = text, b = part;
    for (char& c : a) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : b) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return a.find(b) != std::string::npos;
}
// The ability's own name from a package path ("/PBZCombatAsset/.../GA_X" -> "GA_X").
inline std::string ShortName(const std::string& package) {
    const size_t slash = package.find_last_of('/');
    return slash == std::string::npos ? package : package.substr(slash + 1);
}
inline std::vector<std::string> Tokens(const std::string& name) {
    std::vector<std::string> out; size_t start = 0;
    while (start <= name.size()) {
        const size_t end = name.find('_', start);
        const std::string token = name.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!token.empty()) out.push_back(token);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return out;
}
inline bool DodgeWord(const std::string& token) {
    for (const char* word : {"Dodge", "Dash", "BackStep", "Roll", "FarDodge", "SideStep"})
        if (StartsWithNoCase(token, word)) return true;
    return false;
}
// A dodge that attacks (DodgeAttack, DodgeBackAtk, SideStepAtk) is its own kind; otherwise the first token that
// names a kind decides, so GA_X_LightAttack_01_ForCa is a light attack.
inline Kind Classify(const std::string& ability) {
    const std::vector<std::string> tokens = Tokens(ShortName(ability));
    for (const std::string& t : tokens)
        if (t == "DodgeAttack" || (DodgeWord(t) && (Contains(t, "Atk") || Contains(t, "Attack")))) return Kind::DodgeAttack;
    // The AI's auto combos (GA_X_Attack_AutoCombo_Close) are combos, not the light attack their first word says.
    for (const std::string& t : tokens) if (t == "AutoCombo") return Kind::Combo;
    // tokens: "GA", the boss, then its phase / kind / number.
    for (size_t i = !tokens.empty() && tokens[0] == "GA" ? 2 : 1; i < tokens.size(); ++i) {
        const std::string& t = tokens[i];
        if (t == "LightAttack" || t == "Attack" || t == "Atk" || t == "ZuiAtk" || t == "MiddleAttack") return Kind::Light;
        if (t == "HeavyAttack" || t == "Heavy") return Kind::Heavy;
        if (t == "SpecialMove" || DodgeWord(t)) return Kind::Dodge;
        if (t == "SpecialAttack" || t == "Special" || t == "RangeAttack") return Kind::Special;
        if (t == "CA" || t == "Ca" || t == "CriticalArt" || t == "Ultimate") return Kind::Ultimate;
        if (t == "AutoCombo" || t == "Combo" || t == "ComboMix") return Kind::Combo;
        if (t == "Skill" || t == "SkillAttack") return Kind::Skill;
    }
    return Kind::Other;
}

// What starts a chain: the NPC skill key, or one of the hero's own actions (its ability tag family, from
// InputConfig_Hun). A hero action with a chain wired to it is cancelled the moment it starts and the chain's next
// move plays instead, so its button drives the chain.
// Execution (处决): the hero's executions, paired (Ability.Action.Execution) or simple (Ability.Action.Hun.SimpleExecution);
// its node may name one weapon (`weapon`): then only that weapon's executions are replaced.
// Combo (自定义组合键): the hero's buttons the node lists (`keys`), pressed in order (each within `window` ms of the one
// before) or together (`chord`, within kChordMs); read from the game's input, never from an ability.
// Shachi (影之武装): the hero's secondary-weapon skill (InputConfig_Hun's AIM_Hun_ShachiAbility, IA_ShachiAbility; the
// weapons the game files call 杀器 / ShaqiWeapon and its menus 影之武装; they spend 杀气). Stance (格挡): holding the
// guard (PBZDeflectSystem's IA_Stance; every hero stance GA derives BP_StanceAction_Base: Ability.Action.Stance). Values
// are saved in files: new triggers go at the end; kTriggerOrder is the order the page lists them in. SimpleExecution
// (简易处决): the hero's quick execution (Ability.Action.Hun.SimpleExecution); a 处决 trigger takes it too.
enum class Trigger : int { NpcKey, Light, Heavy, Shachi, Special, CombatArt, Jump, Dash, ContinuousDash, PerfectDodge, DeflectDash, SpecialDash, AnyDodge, Execution, Combo, Keys,
                           Stance, SimpleExecution, Count };
// The hero's buttons as the page lists them (the NPC key apart).
inline constexpr Trigger kTriggerOrder[] = {Trigger::Light, Trigger::Heavy, Trigger::Shachi, Trigger::Special, Trigger::CombatArt, Trigger::Stance, Trigger::Jump,
                                            Trigger::Dash, Trigger::ContinuousDash, Trigger::PerfectDodge, Trigger::DeflectDash, Trigger::SpecialDash, Trigger::AnyDodge,
                                            Trigger::Execution, Trigger::SimpleExecution, Trigger::Combo, Trigger::Keys};
static_assert(std::size(kTriggerOrder) + 1 == static_cast<size_t>(Trigger::Count), "every hero trigger is listed once");
inline const wchar_t* TriggerLabel(Trigger trigger) {
    static constexpr const wchar_t* kLabels[] = {L"NPC 技能键", L"主角轻攻击", L"主角重攻击", L"主角影之武装", L"主角特殊攻击", L"主角战技", L"主角跳跃",
                                                 L"主角闪避", L"主角连续闪避", L"主角完美闪避", L"主角偏斜闪避", L"主角特殊闪避", L"主角任意闪避", L"主角处决",
                                                 L"自定义组合键", L"自定义按键", L"主角格挡", L"主角简易处决"};
    static_assert(std::size(kLabels) == static_cast<size_t>(Trigger::Count));
    const int index = static_cast<int>(trigger);
    return index >= 0 && index < static_cast<int>(Trigger::Count) ? kLabels[index] : L"";
}
inline const char* TriggerTag(Trigger trigger) {
    static constexpr const char* kTags[] = {nullptr, "Ability.Action.Hun.Light", "Ability.Action.Hun.Heavy", "Ability.Action.Hun.ShachiAbility",
                                            "Ability.Action.Hun.Special", "Ability.Action.Hun.CombatArt", "Ability.Action.Hun.Jump",
                                            "Ability.Action.Hun.Dash", "Ability.Action.Hun.ContinuousDash", "Ability.Action.Hun.PerfectDodge",
                                            "Ability.Action.Hun.DeflectDash", "Ability.Action.Hun.SpecialDash", nullptr, "Ability.Action.Execution", nullptr, nullptr,
                                            "Ability.Action.Stance", "Ability.Action.Hun.SimpleExecution"};
    static_assert(std::size(kTags) == static_cast<size_t>(Trigger::Count));
    const int index = static_cast<int>(trigger);
    return index >= 0 && index < static_cast<int>(Trigger::Count) ? kTags[index] : nullptr;
}
inline bool IsDodge(Trigger trigger) { return trigger >= Trigger::Dash && trigger <= Trigger::AnyDodge; }
// Tag family test as the game's own matching: "A.B" covers "A.B" and "A.B.C", never "A.BC".
inline bool InFamily(const char* tag, const char* family) {
    if (!tag || !family) return false;
    const size_t n = std::strlen(family);
    return !_strnicmp(tag, family, n) && (!tag[n] || tag[n] == '.');
}
// Bit per Trigger for one ability tag (0 when it is none of the hero actions).
inline unsigned ActionBits(const char* tag) {
    unsigned bits = 0;
    for (int i = 0; i < static_cast<int>(Trigger::Count); ++i)
        if (InFamily(tag, TriggerTag(static_cast<Trigger>(i)))) bits |= 1u << i;
    if (InFamily(tag, "Ability.Action.Hun.SimpleExecution")) bits |= 1u << static_cast<int>(Trigger::Execution);  // a 处决 trigger takes it too
    return bits;
}
inline bool Fires(Trigger trigger, unsigned bits) {
    if (trigger == Trigger::NpcKey) return false;
    if (trigger == Trigger::AnyDodge) {
        unsigned dodges = 0;
        for (int i = static_cast<int>(Trigger::Dash); i < static_cast<int>(Trigger::AnyDodge); ++i) dodges |= 1u << i;
        return (bits & dodges) != 0;
    }
    return (bits & (1u << static_cast<int>(trigger))) != 0;
}

// Skill: a boss move (ability = its short GA name); Interval: ms between two moves; Trigger: a chain's start
// (trigger = its Trigger, ms = how long after a move ends the next press still continues the chain, on = in use);
// AutoDodge: the skin's dodge the stick points to (back when it is centred); Branch: which button the next press
// is, and after how long a pause, picks where the chain goes. Modifiers do not play: a node points at them (its
// `mods`, drawn as a straight arrow). Speed (加速, speed = the multiplier in percent) and Armor (霸体: hit reactions
// do not interrupt) take a trigger (every move of its chain) or a move (that move alone; a move's own speed goes
// before its chain's); Weapon (武器属性) takes triggers only: their chains' boss moves hit with the hero's main weapon
// strength; Interrupt (允许中断) takes a trigger or a move, as 霸体 does: such a move may be cut at any time, by the
// next move of the chain or by the hero's own action. Many nodes may point at one modifier. Grab (投技) plays the
// grab move chosen on the 投技 page on the nearest target, as a move of the chain; it takes 允许中断 only (never sped
// up or pointed at another modifier): a paired grab is cut by the chain under 允许中断 (its own, its 组合's or its
// trigger's) or at its 组合's 派生点, and a copy of the boss's body stays on the spot and plays it out while the hero goes
// on (as the next boss, or as the hero); without either it plays to its end. BossSkill (BOSS 技能) plays one move of another look: the hero takes that
// look (`boss`, an NPC skin id) behind a blink, casts `ability`, and takes the look from before back afterwards; it
// takes modifiers and a 派生点 like any move. WeaponMove (武器派生) plays one ability of a hero weapon (`weapon`, its
// backpack id; `ability`, a GA the weapon grants): the hero holds that weapon for it (swapped behind a blink when it is
// not the one in hand, and back afterwards); a combo stage (GA_Hun_Long_AA ...) starts the weapon's native combo
// there. Damage (伤害倍率, speed = percent 0-1000) is a modifier like 加速: the moves pointing at it, or every move of
// a trigger pointing at it, deal that share of their damage. Invincible (无敌) takes a trigger or a move, as 霸体 does:
// while such a move plays the hero cannot be hit (the game's own invulnerable tags). Look (变身, `boss` = an NPC skin
// id) and HeroLook (变回主角) are moves that change the hero's look behind the blink and keep it: the chain goes on
// from them once the look is on, and when it ends the hero stays in that look (a BOSS 技能 after them changes from it
// and comes back to it). They take no modifiers and no 派生点. KeepLook (保留变身) is a modifier a BOSS 技能 (or a
// trigger, every BOSS 技能 of its chain; or a frame, the ones it holds) points at: the look that BOSS 技能 takes is
// kept, as a 变身 node's is (the hero does not change back after it, nor when the chain ends). ForceHit (强制受击,
// `hit` = its HitKind) takes a trigger or a move, as 霸体 does: every enemy such a move hits plays that hit reaction of
// the game's (击飞, 击倒, 挑空, 击退, 破防, 硬直) whatever the hit itself was, through its 霸体 (endure) too. IgnoreGuard
// (无视防御) takes a trigger or a move, as 霸体 does: while such a move plays the enemies around cannot deflect (block or
// parry) it, so its hits land with their damage and hit reactions (the game's CharacterState.AIState.CanNotDeflect). PreInput
// (预输入, ms = kPreInputMin..kPreInputMax, 0: never) is the chain's own setting, one fixed node per chain that cannot be
// removed and takes no wires: a press made while a move plays is kept that long for the move's 派生点 (counted from the
// press, whatever path it takes), then cleared; one still kept at the 派生点 waits out the intervals / the branch row's
// pause after it and is cleared that long after it could play. WeaponActions
// (武器派生修改, `weapon` = a backpack id, ms = its reset time as a trigger's) starts chains as a trigger does, for one
// weapon: a row per hero action (`routes`: button = a Trigger of the hero's actions, next = its output); while that
// weapon is the hero's main weapon, that action (完美闪避, 处决, 简易处决, 格挡 ...) is cut and the row's wire plays
// instead. A 处决 row takes the quick ones too unless a 简易处决 row is there; a dodge's own row goes before 任意闪避.
// 空中派生 is a 武器派生修改 with air_set and no weapon: while the hero is in the air its rows (the NPC key or a hero
// action, never an execution) start their wires in place of the triggers, so the air has a set of chains of its own.
// A chain going on keeps going (a move that throws the hero up continues as wired); only a press that would start a
// chain is taken by it. In the text it is a 武器派生修改 with weapon "_" and an "s:" mark, inert in an older build.
// KeepChain (保持派生, ms = kKeepChainMin..kKeepChainMax) takes a trigger or a move, as 霸体 does: while the chain is at
// such a move (playing it, or waiting after it) and goes on from there, no press of the hero ends the chain. A press
// the chain does not take there (a dodge, an attack with no row, another trigger's button, the same trigger again) is
// left to the game, cutting the move if one plays (the chain's own presses still wait for the 派生点); the chain stays
// where it was and waits the node's ms (its own timer, in place of the trigger's reset time) from the move's end or
// from the last such press, whichever is later. A chain with nothing after its move ends as before.
// Assist (BOSS 援护, `boss` = an NPC skin id, `ability` = that look's move) does not move the hero: a press that reaches
// it calls a copy of that boss in, an actor of its own (it jumps in from outside the camera's view, casts the move at
// the hero's target and leaves), and the press goes on through it to the move after it, which the hero plays as usual
// (none: the chain waits at the 援护 as after a move that has ended). It takes the modifiers a move takes: 加速, 伤害倍率,
// 武器属性 (its trigger's), 强制受击, 无视防御, 霸体 and 无敌 act on the boss and its cast; under 允许中断 the node's next call
// takes the place of its boss still casting (else up to three bosses are in at once); 保持派生 keeps the chain at it.
// Hook (钩子) is the hero's own chain sickle (追命锁镰, the 影之武装 ShachiHook) as a move: the lock target (none: the
// nearest enemy, locked for it) is hooked, and the hero pulls it over (勾过来: GA_Hun_ShachiHook_Drag) or pulls itself
// over to it (勾过去, hook_self: GA_Hun_ShachiHook_HookMove). The weapon is held for it as for a 武器派生 (`weapon` and
// `ability` are set from the mode, SetHookMode); it takes the modifiers and the 派生点 a 武器派生 takes. In the text it is a
// 武器派生 of that ability with a "g:" mark, so an older build plays it as that 武器派生. 自动 (hook_auto, SetHookAuto,
// a "q:" mark; written as 勾过来) picks the mode when it plays: a target whose guard is broken (破防) is pulled over, any
// other the hero goes to. It is thrown where the player aims (the lock target, else an enemy under the crosshair, else
// the ground or the wall there, which the hero flies to, whatever the mode) up to hook_dist cm away, and hook_speed
// paces it (the hook move's time with the 加速 it takes, and the pull and flight it is carried on with); an "r:" mark
// keeps both when either is not its default.
// Air (滞空) takes a trigger or a move, as 霸体 does (a move the hero casts: a skill, a BOSS 技能, a 武器派生, the stick's
// dodge; or a frame, the ones it holds): the move throws the hero up air_lift cm when it starts (as a jump of that
// height, the hero's own gravity slowing it) and holds it in the air while it plays (air_hang percent of its gravity
// taken away: 100 hovers); each enemy it hits is thrown up air_foe_lift cm (once per move) and held likewise
// (air_foe_hang) while the move plays and a moment after its last hit. AirDefault (空中释放) is the chain's own: one fixed
// node per chain, as 预输入 (never removed, no wires), with the same four values and `on`: a move cast while the hero is
// in the air that points at no 滞空 (nor its frames or trigger) takes its values. Written as an unwired 间隔 with an
// "f:" (滞空) or "j:" (空中释放) mark, so an older build keeps every index and ignores them. 强制受击 击飞's `launch`
// (cm): the enemies it hits are thrown up that high (once per move; with a 滞空, the higher of the two lifts); a "u:"
// mark keeps it when it is not kLaunchDefault.
// 按住 (hold_ms, kHoldMin..kHoldMax): a trigger or a branch row with a hold takes its button only once it has been held
// that long; a press of that button that is let go sooner is a tap (rows and triggers without a hold take it). When
// one button has rows with holds, its press waits: the longest hold is taken the moment it is reached, otherwise the
// longest hold reached when the button is let go (a tap: none).
enum class NodeType : int { Skill, Interval, Trigger, AutoDodge, Branch, Speed, Weapon, Armor, Interrupt, Grab, BossSkill, WeaponMove, Damage, Group, Teleport,
                            Invincible, Look, HeroLook, KeepLook, ForceHit, PreInput, WeaponActions, IgnoreGuard, KeepChain, Assist, Hook, Trim, Air, AirDefault, BoneTransform,
                            BossEffect };
// One row of a branch: a press of `button` (a Trigger, the NPC key included) at least pause_ms after the press
// before it goes on to `next`. A 自定义组合键 / 自定义按键 row is pressed with its own keys, as such a trigger is: `keys`
// (the hero's buttons / keyboard, mouse and pad keys) in order (each within `window` ms, default kKeyWindowDefault)
// or together (`chord`); 自定义按键 with `swallow` cancels the game's action of its keys.
// hold_ms: 按住, how long the button must be held for this row (0: any press).
struct Route { int button = static_cast<int>(Trigger::Light); int pause_ms = 0; int next = -1; std::vector<int> keys; bool chord = false; int window = 400; bool swallow = false;
               int hold_ms = 0; };
// The modifier slots of a node's `mods`: the 加速, 霸体, 武器属性, 允许中断, 伤害倍率, 无敌, 保留变身, 强制受击, 无视防御,
// 保持派生, 裁剪 and 滞空 node it points at (-1 none).
enum ModSlot : int { kModSpeed, kModArmor, kModWeapon, kModInterrupt, kModDamage, kModInvincible, kModKeepLook, kModForceHit, kModIgnoreGuard, kModKeepChain, kModTrim,
                     kModAir, kModBone, kModEffect, kModSlots };
// Every slot empty (-1), however many slots there are.
inline constexpr std::array<int, kModSlots> NoMods() { std::array<int, kModSlots> mods{}; for (size_t i = 0; i < mods.size(); ++i) mods[i] = -1; return mods; }
// A node on the page's canvas: x, y its top-left (96-dpi units); next the node its exec output is wired to (-1
// none). A branch has no `next`: each route is an output of its own. A modifier has neither. A move's `ms` is its
// 派生点: how long after it starts the next move may cut it (0: when it ends).
// 瞬移: how far in front of the target the hero lands (capsule centre to centre, cm) and how high above the ground.
constexpr int kTeleportDistMin = 50, kTeleportDistMax = 1000, kTeleportDistDefault = 150, kTeleportDistStep = 10;
constexpr int kTeleportLiftMin = 0, kTeleportLiftMax = 300, kTeleportLiftDefault = 30, kTeleportLiftStep = 5;
// 滞空 / 空中释放: lifts (cm) and hangs (percent of the gravity taken away); a new 滞空's values and the 空中释放 ones.
constexpr int kAirLiftMin = 0, kAirLiftMax = 1000, kAirLiftStep = 10;
constexpr int kAirHangMin = 0, kAirHangMax = 100, kAirHangStep = 5;
constexpr int kAirSelfLiftNew = 150, kAirSelfHangNew = 70, kAirFoeLiftNew = 200, kAirFoeHangNew = 70;
constexpr int kAirSelfLiftDefault = 80, kAirSelfHangDefault = 60, kAirFoeLiftDefault = 80, kAirFoeHangDefault = 60;
// 强制受击 击飞: how high the enemies it hits are thrown (cm).
constexpr int kLaunchMin = 0, kLaunchMax = 1500, kLaunchDefault = 300, kLaunchStep = 10;
inline int ClampAirLift(int cm) { return std::clamp(cm, kAirLiftMin, kAirLiftMax); }
inline int ClampAirHang(int percent) { return std::clamp(percent, kAirHangMin, kAirHangMax); }
inline int ClampLaunch(int cm) { return std::clamp(cm, kLaunchMin, kLaunchMax); }
// BOSS 特效: one of the game's battle effects (kNpcBossEffects: `boss` its folder, `ability` its name) played when the
// move it is wired from starts (from a start: each move of its chains; from a 组合: each of its moves), at fx_place,
// for as long as the move plays (one that ends by itself ends sooner). Its damage: fx_damage percent of the max health
// of each enemy within fx_radius cm of the effect, fx_hits times kEffectHitGapMs apart from its start (0 %: none).
enum class EffectPlace : int { Weapon, Hero, Front, Target, Count };
constexpr int kEffectDamageMin = 0, kEffectDamageMax = 100, kEffectDamageNew = 10;
constexpr int kEffectRadiusMin = 100, kEffectRadiusMax = 2000, kEffectRadiusNew = 400, kEffectRadiusStep = 50;
constexpr int kEffectHitsMin = 1, kEffectHitsMax = 10, kEffectHitsNew = 1;
constexpr int kEffectHitGapMs = 300, kEffectFrontCm = 300;
inline int ClampEffectPlace(int place) { return place >= 0 && place < static_cast<int>(EffectPlace::Count) ? place : 0; }
inline int ClampEffectDamage(int percent) { return std::clamp(percent, kEffectDamageMin, kEffectDamageMax); }
inline int ClampEffectRadius(int cm) { return std::clamp(cm, kEffectRadiusMin, kEffectRadiusMax); }
inline int ClampEffectHits(int hits) { return std::clamp(hits, kEffectHitsMin, kEffectHitsMax); }
// 组合: its frame on the canvas (96-dpi units) and how deep frames may sit in one another.
constexpr int kGroupMinW = 300, kGroupMinH = 180, kGroupNewW = 460, kGroupNewH = 300, kGroupMaxSize = 4000, kGroupDepthMax = 4;
struct Node {
    NodeType type = NodeType::Skill; std::string ability; int ms = 0; int x = 0, y = 0; int next = -1;
    int trigger = 0; bool on = true; int speed = 100; std::vector<Route> routes; std::array<int, kModSlots> mods = NoMods();
    int hit = 0;  // 强制受击: its HitKind
    int launch = kLaunchDefault;  // 强制受击 击飞: how high the enemies are thrown (cm)
    // 滞空 / 空中释放: the hero's lift (cm) and hang (percent), the enemies' it hits; 空中释放 uses `on` too.
    int air_lift = kAirSelfLiftNew, air_hang = kAirSelfHangNew, air_foe_lift = kAirFoeLiftNew, air_foe_hang = kAirFoeHangNew;
    std::string boss;  // BOSS 技能: the look (NPC skin id) whose move `ability` is
    std::string weapon;  // 武器派生: the weapon (backpack id) whose ability `ability` is; 处决 trigger: only this weapon's (empty: any)
    int group = -1;  // the 组合 node this node sits in (-1: none)
    int w = 0, h = 0;  // 组合: its frame's size
    int dist = kTeleportDistDefault, lift = kTeleportLiftDefault;  // 瞬移
    std::vector<int> keys; bool chord = false; int window = 400;  // 自定义组合键 / 自定义按键: its buttons, together or in order (ms between two)
    bool swallow = false;  // 自定义按键: its keys' presses cancel the hero action they start (default: the game keeps them)
    int then = -1;  // 自定义组合键 / 自定义按键: its 继续派生 key (kThenOwn: its own keys again)
    // 加速's 曲线调节: the speed follows `points` over the move (t: percent of the montage's length, 0..100; speed:
    // percent, kSpeedMin..kSpeedMax) in place of the one `speed`. Points stay sorted by t, the first at 0 and the last
    // at 100. Kept when the curve is switched off (so switching it on again brings it back).
    bool curve = false;
    // BOSS 技能 / 投技's 不变身: the hero keeps the look it has; the move's animation plays on it (retargeted: the hero's
    // own bone lengths, the move's rotations) instead of taking the boss's look behind the blink. On a trigger or a
    // 武器派生修改 it is 整体不变身: every BOSS 技能 and 投技 of the chains it starts plays so (OwnLookFor).
    bool own_look = false;
    bool hook_self = false;  // 钩子: 勾过去 (the hero pulls itself to the target); false: 勾过来 (the target is pulled over)
    bool hook_auto = false;  // 钩子: 自动 (勾过来 a target whose guard is broken, else 勾过去); hook_self is what it plays as
    int hook_dist = 1500, hook_speed = 100;  // 钩子: the farthest aim it takes (cm) and its speed (percent): kHookDist*, kHookSpeed*
    int hold_ms = 0;  // a trigger's 按住: how long its button must be held to start the chain (0: any press)
    // BOSS 特效: where (EffectPlace), its damage (percent of max health), radius (cm) and hits.
    int fx_place = 0, fx_damage = kEffectDamageNew, fx_radius = kEffectRadiusNew, fx_hits = kEffectHitsNew;
    bool air_set = false;  // 武器派生修改 as 空中派生: its rows take the presses made in the air (no weapon)
    // 裁剪: the part of the BOSS 技能 pointing at it that plays, in its montage's own time (ms): from trim_from to trim_to
    // (0, or not after trim_from: to its end).
    int trim_from = 0, trim_to = 0;
    std::vector<int> trim_holds; // montage ms: hold the activating input here, release to continue
    struct CurvePoint { int t = 0; int speed = 100; };
    std::vector<CurvePoint> points;
    BoneMotion::Config bones; // per-move visual edits; serialized as an unwired interval for old builds
};
// placed: false when the text had no places (saved before the canvas), so the page lays the nodes out once.
struct Config { bool enabled = true; std::vector<Node> chain; bool placed = true; };
// kMaxNodes: no limit a page reaches in use; only a guard against a broken or hostile file (memory, drawing time).
constexpr size_t kMaxNodes = 10000, kMaxRoutes = 6;
constexpr int kTrimMax = 60000, kTrimStep = 50;  // 裁剪, ms of the move's montage
inline int ClampTrim(int ms) { return std::clamp(ms, 0, kTrimMax); }
inline std::vector<int> TrimHolds(std::vector<int> points){
    for(int& t:points)t=ClampTrim(t);std::sort(points.begin(),points.end());points.erase(std::unique(points.begin(),points.end()),points.end());
    if(points.size()>32)points.resize(32);return points;
}
constexpr int kIntervalMin = 50, kIntervalMax = 10000, kIntervalDefault = 500, kIntervalStep = 50;
constexpr int kResetMin = 100, kResetMax = 5000, kResetDefault = 1200, kResetStep = 100;
constexpr int kSpeedMin = 50, kSpeedMax = 1000, kSpeedDefault = 100, kSpeedNew = 150, kSpeedStep = 10;
constexpr int kCancelMin = 0, kCancelMax = 10000, kCancelStep = 50;
constexpr int kPauseMin = 0, kPauseMax = 10000, kPauseStep = 100;
constexpr int kDamageMin = 0, kDamageMax = 1000, kDamageDefault = 100, kDamageNew = 200, kDamageStep = 10;
constexpr int kPreInputMin = 0, kPreInputMax = 5000, kPreInputDefault = 300, kPreInputStep = 50;
constexpr int kKeepChainMin = 500, kKeepChainMax = 30000, kKeepChainDefault = 3000, kKeepChainStep = 100;
constexpr int kHoldMin = 0, kHoldMax = 5000, kHoldStep = 100;  // 按住, ms
inline int ClampHold(int ms) { return std::clamp(ms, kHoldMin, kHoldMax); }
// 钩子: the hero's chain sickle (the 影之武装 ShachiHook) and its two hook moves.
constexpr const char* kHookWeapon = "ShachiHook";
constexpr const char* kHookPull = "GA_Hun_ShachiHook_Drag";      // 勾过来
constexpr const char* kHookGo = "GA_Hun_ShachiHook_HookMove";    // 勾过去
// 钩子's reach (cm, from the hero: the game's own aim trace reaches 30 m) and speed (percent).
constexpr int kHookDistMin = 300, kHookDistMax = 3000, kHookDistDefault = 1500, kHookDistStep = 50;
constexpr int kHookSpeedMin = 50, kHookSpeedMax = 300, kHookSpeedDefault = 100, kHookSpeedStep = 10;
inline int ClampHookDist(int cm) { return std::clamp(cm, kHookDistMin, kHookDistMax); }
inline int ClampHookSpeed(int percent) { return std::clamp(percent, kHookSpeedMin, kHookSpeedMax); }
constexpr int kCanvasLimit = 20000;  // node positions stay within +-this
inline int ClampInterval(int ms) { return std::clamp(ms, kIntervalMin, kIntervalMax); }
inline int ClampReset(int ms) { return std::clamp(ms, kResetMin, kResetMax); }
inline int ClampSpeed(int percent) { return std::clamp(percent, kSpeedMin, kSpeedMax); }
// 加速 曲线: at most this many points; two of them never closer than kCurveGap percent of the move.
constexpr size_t kCurvePointsMax = 16;
constexpr int kCurveGap = 2;
using CurvePoint = Node::CurvePoint;
// A curve as the editor and the ini keep it: sorted by t, clamped, two points closer than kCurveGap merged (the later
// one kept), the ends at 0 and 100 (added at the nearest point's speed when missing), at most kCurvePointsMax points.
inline std::vector<CurvePoint> CleanCurve(std::vector<CurvePoint> points) {
    for (CurvePoint& p : points) { p.t = std::clamp(p.t, 0, 100); p.speed = ClampSpeed(p.speed); }
    std::stable_sort(points.begin(), points.end(), [](const CurvePoint& a, const CurvePoint& b) { return a.t < b.t; });
    std::vector<CurvePoint> out;
    for (const CurvePoint& p : points) {
        if (!out.empty() && p.t - out.back().t < kCurveGap) { out.back().speed = p.speed; if (p.t == 100) out.back().t = 100; continue; }
        out.push_back(p);
    }
    if (out.empty()) out.push_back({0, kSpeedDefault});
    if (out.front().t != 0) { if (out.front().t < kCurveGap) out.front().t = 0; else out.insert(out.begin(), CurvePoint{0, out.front().speed}); }
    if (out.back().t != 100) { if (100 - out.back().t < kCurveGap && out.size() > 1) out.back().t = 100; else out.push_back({100, out.back().speed}); }
    while (out.size() > kCurvePointsMax) out.erase(out.end() - 2);
    return out;
}
// A new curve: the node's one speed at the start and end, twice that (at most kSpeedMax) in the middle.
inline std::vector<CurvePoint> DefaultCurve(int speed) {
    const int base = ClampSpeed(speed);
    return {{0, base}, {50, ClampSpeed(base * 2)}, {100, base}};
}
// The curve's speed (percent, not rounded) at t01 (0..1 of the move): a monotone cubic through the points (no
// overshoot between two points, flat where a point is a peak or a dip), held at the ends.
inline double CurveSpeed(const std::vector<CurvePoint>& points, double t01) {
    if (points.empty()) return kSpeedDefault;
    if (points.size() == 1) return ClampSpeed(points[0].speed);
    const double t = std::clamp(t01, 0.0, 1.0) * 100.0;
    const size_t n = points.size();
    if (t <= points.front().t) return points.front().speed;
    if (t >= points.back().t) return points.back().speed;
    size_t k = 0;
    while (k + 2 < n && t >= points[k + 1].t) ++k;
    // Fritsch-Carlson slopes at the two ends of segment k.
    const auto delta = [&](size_t i) { const double h = points[i + 1].t - points[i].t; return h > 0 ? (points[i + 1].speed - points[i].speed) / h : 0.0; };
    const auto slope = [&](size_t i) {
        if (i == 0) return delta(0);
        if (i == n - 1) return delta(n - 2);
        const double a = delta(i - 1), b = delta(i);
        if (a * b <= 0) return 0.0;
        const double ha = points[i].t - points[i - 1].t, hb = points[i + 1].t - points[i].t;
        return 3 * (ha + hb) / ((2 * hb + ha) / a + (hb + 2 * ha) / b);
    };
    const double h = points[k + 1].t - points[k].t;
    if (!(h > 0)) return points[k + 1].speed;
    const double s = (t - points[k].t) / h, y0 = points[k].speed, y1 = points[k + 1].speed;
    const double m0 = slope(k) * h, m1 = slope(k + 1) * h;
    const double s2 = s * s, s3 = s2 * s;
    const double y = (2 * s3 - 3 * s2 + 1) * y0 + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * y1 + (s3 - s2) * m1;
    return std::clamp(y, static_cast<double>(kSpeedMin), static_cast<double>(kSpeedMax));
}
// The curve's mean speed over the move in montage time (percent): how long the move lasts is its length at this speed.
inline double CurveMeanSpeed(const std::vector<CurvePoint>& points) {
    // time = integral of dt / speed: the mean speed is the harmonic mean over t.
    double time = 0; const int steps = 200;
    for (int i = 0; i < steps; ++i) time += 1.0 / (std::max)(1.0, CurveSpeed(points, (i + 0.5) / steps));
    return time > 0 ? steps / time : kSpeedDefault;
}
// Where (0..1 of the montage) a move under the curve is after `fraction` (0..1) of the time it lasts: what a preview
// steps through when it plays the move at the curve's pace.
inline double CurveProgress(const std::vector<CurvePoint>& points, double fraction) {
    const int steps = 200; double total = 0;
    for (int i = 0; i < steps; ++i) total += 1.0 / (std::max)(1.0, CurveSpeed(points, (i + 0.5) / steps));
    const double want = std::clamp(fraction, 0.0, 1.0) * total; double at = 0;
    for (int i = 0; i < steps; ++i) {
        const double piece = 1.0 / (std::max)(1.0, CurveSpeed(points, (i + 0.5) / steps));
        if (at + piece >= want) return (i + (piece > 0 ? (want - at) / piece : 0)) / steps;
        at += piece;
    }
    return 1.0;
}
// The ini text of a curve: "~t:speed;t:speed;..." after a 加速's percent (an older build reads the percent alone).
inline std::string CurveText(const std::vector<CurvePoint>& points) {
    std::string text = "~";
    for (size_t i = 0; i < points.size(); ++i) text += (i ? ";" : "") + std::to_string(points[i].t) + ":" + std::to_string(points[i].speed);
    return text;
}
inline std::vector<CurvePoint> ParseCurve(const std::string& text) {
    std::vector<CurvePoint> points; size_t at = 0;
    while (at < text.size() && points.size() < kCurvePointsMax * 2) {
        size_t end = text.find(';', at); if (end == std::string::npos) end = text.size();
        const std::string piece = text.substr(at, end - at); const size_t colon = piece.find(':');
        if (colon != std::string::npos) points.push_back({std::atoi(piece.substr(0, colon).c_str()), std::atoi(piece.substr(colon + 1).c_str())});
        at = end + 1;
    }
    return points.empty() ? points : CleanCurve(points);  // nothing readable: no curve
}
inline int ClampPause(int ms) { return std::clamp(ms, kPauseMin, kPauseMax); }
inline int ClampDamage(int percent) { return std::clamp(percent, kDamageMin, kDamageMax); }
inline int ClampCancel(int ms) { return std::clamp(ms, kCancelMin, kCancelMax); }
inline int ClampPreInput(int ms) { return std::clamp(ms, kPreInputMin, kPreInputMax); }
inline int ClampKeepChain(int ms) { return std::clamp(ms, kKeepChainMin, kKeepChainMax); }
inline int ClampPosition(int v) { return std::clamp(v, -kCanvasLimit, kCanvasLimit); }
inline int ClampTeleportDist(int cm) { return std::clamp(cm, kTeleportDistMin, kTeleportDistMax); }
inline int ClampTeleportLift(int cm) { return std::clamp(cm, kTeleportLiftMin, kTeleportLiftMax); }
// 自定义组合键: 2 to 6 buttons; in order each within 100-2000 ms (default 400) of the one before; together within 150 ms.
constexpr size_t kComboKeysMin = 2, kComboKeysMax = 6;
constexpr int kKeyWindowMin = 100, kKeyWindowMax = 2000, kKeyWindowDefault = 400, kChordMs = 150;
inline int ClampKeyWindow(int ms) { return std::clamp(ms, kKeyWindowMin, kKeyWindowMax); }
// 强制受击: the hit reactions it may force, each a hit strength of the game's (Hitreact.Strength.Income.*; its
// PBZ_HitStatesConfig picks the state and the animation from it). Values are saved in files: new kinds go at the end.
enum class HitKind : int { Launch, Knockdown, Knockup, Knockback, Break, Stagger, Count };
inline int ClampHitKind(int kind) { return kind >= 0 && kind < static_cast<int>(HitKind::Count) ? kind : 0; }
inline const wchar_t* HitKindLabel(int kind) {
    static constexpr const wchar_t* kLabels[] = {L"击飞", L"击倒", L"挑空", L"击退", L"破防", L"硬直"};
    static_assert(std::size(kLabels) == static_cast<size_t>(HitKind::Count));
    return kLabels[ClampHitKind(kind)];
}
inline const char* HitKindTag(int kind) {
    static constexpr const char* kTags[] = {"Hitreact.Strength.Income.Launchback.Heavy", "Hitreact.Strength.Income.Knockdown", "Hitreact.Strength.Income.Knockup",
                                            "Hitreact.Strength.Income.Normal.Knockback.Heavy", "Hitreact.Strength.Income.Break.Start", "Hitreact.Strength.Income.Normal.Heavy"};
    static_assert(std::size(kTags) == static_cast<size_t>(HitKind::Count));
    return kTags[ClampHitKind(kind)];
}
// A key of a combo: the hero's attack / skill / jump buttons, or the dodge button (any dodge).
inline bool ValidComboKey(int key) {
    return (key >= static_cast<int>(Trigger::Light) && key <= static_cast<int>(Trigger::Jump)) || key == static_cast<int>(Trigger::AnyDodge) || key == static_cast<int>(Trigger::Stance);
}
inline const wchar_t* ComboKeyLabel(int key) {
    switch (static_cast<Trigger>(key)) {
    case Trigger::Light: return L"轻攻击"; case Trigger::Heavy: return L"重攻击"; case Trigger::Shachi: return L"影之武装";
    case Trigger::Special: return L"特殊攻击"; case Trigger::CombatArt: return L"战技"; case Trigger::Jump: return L"跳跃";
    case Trigger::AnyDodge: return L"闪避"; case Trigger::Stance: return L"格挡"; default: return L"";
    }
}
// 自定义按键: 1 to 6 keyboard keys / five mouse buttons / pad buttons / left-stick pushes, a code each: a virtual key
// (1..254, including left / right mouse buttons), kPadKey + a bit of the hotkey module's pad layout (the Xbox buttons; a
// PlayStation pad's sit on the same bits, and Steam Input's virtual pad reads as Xbox), or kStickKey + a way of the left
// stick (up / forward, down / back, left, right: a push past half way along its main axis is one press, as the
// motion inputs of a fighting game: 上推 -> 下推 -> 方块 in order).
constexpr int kPadKey = 0x100, kPadBits = 16, kStickWays = 4, kStickKey = kPadKey + kPadBits;
inline bool ValidRawKey(int code) {
    return (code > 0 && code < 0xFF) || (code >= kPadKey && code < kStickKey + kStickWays);
}
// A pad code: a button or a stick way (both read from the pad, by bit: the button bits, then the stick's after them).
inline bool IsPadKey(int code) { return code >= kPadKey && code < kStickKey + kStickWays; }
inline bool IsStickKey(int code) { return code >= kStickKey && code < kStickKey + kStickWays; }
// The pad's level as one mask over the pad codes: its buttons (bits 0..15) and its stick way (bits 16..19).
inline unsigned PadCodeBits(unsigned buttons, unsigned stick) {
    return (buttons & ((1u << kPadBits) - 1)) | ((stick & ((1u << kStickWays) - 1)) << kPadBits);
}
// A pad code's name: Xbox or PlayStation.
inline const wchar_t* PadKeyName(int code, bool ps) {
    static constexpr const wchar_t* kXbox[kPadBits + kStickWays] = {L"Up", L"Down", L"Left", L"Right", L"Menu", L"View", L"L3", L"R3",
                                                       L"LB", L"RB", L"A", L"B", L"X", L"Y", L"LT", L"RT", L"LS-Up", L"LS-Down", L"LS-Left", L"LS-Right"};
    static constexpr const wchar_t* kPs[kPadBits + kStickWays] = {L"Up", L"Down", L"Left", L"Right", L"Options", L"Create", L"L3", L"R3",
                                                     L"L1", L"R1", L"Cross", L"Circle", L"Square", L"Triangle", L"L2", L"R2", L"LS-Up", L"LS-Down", L"LS-Left", L"LS-Right"};
    return IsPadKey(code) ? (ps ? kPs : kXbox)[code - kPadKey] : L"";
}
// For the log: "pad:LB" or "vk:0x51".
inline std::string RawKeyToken(int code) {
    char text[24]{};
    if (IsPadKey(code)) { std::string out = "pad:"; for (const wchar_t* c = PadKeyName(code, false); *c; ++c) out += static_cast<char>(*c); return out; }
    std::snprintf(text, sizeof(text), "vk:0x%02X", code & 0xFF);
    return text;
}
// The NPC skill key's chord is part of these 自定义按键 keys: every one of its pad buttons (pad: the chord's PadButton
// bits) is among the keys' pad buttons, or (pad 0) its keyboard key is one of the keys. Pressing such keys fires that
// hotkey as well (11-11: keys R3 -> L3 and the PlayStation NPC skill key L3 + R3).
inline bool ChordInKeys(const std::vector<int>& keys, unsigned pad, unsigned key) {
    unsigned bits = 0; bool has = false;
    for (const int k : keys) {
        if (IsPadKey(k)) { if (!IsStickKey(k)) bits |= 1u << (k - kPadKey); }
        else if (key && static_cast<unsigned>(k) == key) has = true;
    }
    return pad ? (pad & ~bits) == 0 : has;
}
inline int ClampGroupW(int w) { return std::clamp(w, kGroupMinW, kGroupMaxSize); }
inline int ClampGroupH(int h) { return std::clamp(h, kGroupMinH, kGroupMaxSize); }
inline bool ValidButton(int button) { return button >= 0 && button < static_cast<int>(Trigger::Count); }
// A branch row's button: any trigger but 处决 (an execution is no press of the chain); a 自定义组合键 / 自定义按键 row
// has keys of its own.
inline bool ValidRouteButton(int button) {
    return ValidButton(button) && button != static_cast<int>(Trigger::Execution) && button != static_cast<int>(Trigger::SimpleExecution);
}
// A trigger that may name the weapon whose actions it takes (处决, 简易处决; empty: any).
inline bool WeaponTrigger(int trigger) { return trigger == static_cast<int>(Trigger::Execution) || trigger == static_cast<int>(Trigger::SimpleExecution); }
// A 武器派生修改 row's action: a hero action (never the NPC key or own keys).
inline bool ValidWeaponAction(int button) {
    return ValidButton(button) && button != static_cast<int>(Trigger::NpcKey);
}
// A 空中派生 row's press: the NPC key or a hero action (never an execution, nor own keys).
inline bool ValidAirAction(int button) {
    return button == static_cast<int>(Trigger::NpcKey) || (ValidWeaponAction(button) && !WeaponTrigger(button));
}
inline bool IsAirSet(const Node& node) { return node.type == NodeType::WeaponActions && node.air_set; }
// A node that starts chains (a trigger, a 武器派生修改), and one whose outputs are its rows (a branch, a 武器派生修改).
inline bool StartsChain(const Node& node) { return node.type == NodeType::Trigger || node.type == NodeType::WeaponActions; }
inline bool HasRows(const Node& node) { return node.type == NodeType::Branch || node.type == NodeType::WeaponActions; }
inline bool IsComboTrigger(const Node& node) { return node.type == NodeType::Trigger && node.trigger == static_cast<int>(Trigger::Combo); }
inline bool IsKeysTrigger(const Node& node) { return node.type == NodeType::Trigger && node.trigger == static_cast<int>(Trigger::Keys); }
// A trigger its own node's keys start (自定义组合键 / 自定义按键): a press names it (Press::combo).
inline bool OwnKeysTrigger(int trigger) { return trigger == static_cast<int>(Trigger::Combo) || trigger == static_cast<int>(Trigger::Keys); }
// 继续派生 (Node::then): what takes a 自定义组合键 / 自定义按键 chain on after its first move. kThenOwn: its own keys
// again (the default); a hero action or the NPC key (a button a branch row may have, never the own-keys kinds); for
// 自定义按键 also one key of its kind (kThenRaw + a raw key code). Pressing the own keys again then starts the chain
// over, as another trigger's would.
constexpr int kThenOwn = -1, kThenRaw = 0x1000;
// The chain's button while a 自定义按键's 继续派生 key takes it on: only a press of that key (Press::then) matches.
constexpr int kThenKeyButton = 0x2000;
inline bool ValidThenButton(int button) { return ValidRouteButton(button) && !OwnKeysTrigger(button); }
inline bool ValidThen(int trigger, int then) {
    if (then == kThenOwn) return true;
    if (!OwnKeysTrigger(trigger)) return false;
    if (then >= kThenRaw) return trigger == static_cast<int>(Trigger::Keys) && ValidRawKey(then - kThenRaw);
    return ValidThenButton(then);
}
inline bool HasThen(const Node& node) { return node.type == NodeType::Trigger && OwnKeysTrigger(node.trigger) && node.then != kThenOwn && ValidThen(node.trigger, node.then); }
// A 自定义按键's 继续派生 key (a raw key code; -1: none, or a hero action / its own keys).
inline int ThenRawKey(const Node& node) { return HasThen(node) && node.then >= kThenRaw ? node.then - kThenRaw : -1; }
// The button a chain started at trigger `node` is on after its first move: its own, or its 继续派生.
inline int ThenButton(const Node& node) { return !HasThen(node) ? node.trigger : node.then >= kThenRaw ? kThenKeyButton : node.then; }
// A node that plays something (a boss move, the stick's dodge, a grab, another look's move, a weapon's ability, a
// 瞬移 or a 组合 of them).
inline bool IsMove(const Node& node) {
    return node.type == NodeType::AutoDodge || node.type == NodeType::Group || node.type == NodeType::Teleport || node.type == NodeType::HeroLook ||
           (node.type == NodeType::Look && !node.boss.empty()) ||
           ((node.type == NodeType::Skill || node.type == NodeType::Grab) && !node.ability.empty()) ||
           ((node.type == NodeType::BossSkill || node.type == NodeType::Assist) && !node.ability.empty() && !node.boss.empty()) ||
           (node.type == NodeType::WeaponMove && !node.ability.empty() && !node.weapon.empty()) || node.type == NodeType::Hook;
}
// A move the hero plays with a weapon held for it (武器派生, 钩子: `weapon`, `ability`).
inline bool CastsWeapon(const Node& node) { return node.type == NodeType::WeaponMove || node.type == NodeType::Hook; }
// 钩子: its mode sets the weapon and the ability it casts.
// ResolveHook: the move it plays now (自动 stays 自动).
inline void ResolveHook(Node& node, bool self) { node.hook_self = self; node.weapon = kHookWeapon; node.ability = self ? kHookGo : kHookPull; }
inline void SetHookMode(Node& node, bool self) { node.hook_auto = false; ResolveHook(node, self); }
inline void SetHookAuto(Node& node) { ResolveHook(node, false); node.hook_auto = true; }
inline Node HookNode(int x, int y, bool self = false) { Node node; node.type = NodeType::Hook; node.x = x; node.y = y; SetHookMode(node, self); return node; }
inline bool HookAbility(const std::string& weapon, const std::string& ability) { return weapon == kHookWeapon && (ability == kHookPull || ability == kHookGo); }
// A node whose look and move are picked from a boss (BOSS 技能, BOSS 援护).
inline bool PicksBossMove(const Node& node) { return node.type == NodeType::BossSkill || node.type == NodeType::Assist; }
// A move node whose move is unknown (a bad name, a BOSS 技能 or 投技 not picked yet): a press passes through it.
inline bool EmptyMove(const Node& node) {
    return ((node.type == NodeType::Skill || node.type == NodeType::Grab) && node.ability.empty()) ||
           ((node.type == NodeType::BossSkill || node.type == NodeType::Assist) && (node.ability.empty() || node.boss.empty())) ||
           (node.type == NodeType::Look && node.boss.empty()) ||
           (node.type == NodeType::WeaponMove && (node.ability.empty() || node.weapon.empty()));
}
inline bool IsModifier(const Node& node) {
    return node.type == NodeType::Speed || node.type == NodeType::Weapon || node.type == NodeType::Armor || node.type == NodeType::Interrupt ||
           node.type == NodeType::Damage || node.type == NodeType::Invincible || node.type == NodeType::KeepLook || node.type == NodeType::ForceHit ||
           node.type == NodeType::IgnoreGuard || node.type == NodeType::KeepChain || node.type == NodeType::Trim || node.type == NodeType::Air || node.type == NodeType::BoneTransform ||
           node.type == NodeType::BossEffect;
}
// A modifier's slot in `mods` (-1: not a modifier).
inline int SlotOf(NodeType type) {
    return type == NodeType::Speed ? kModSpeed : type == NodeType::Armor ? kModArmor : type == NodeType::Weapon ? kModWeapon : type == NodeType::Interrupt ? kModInterrupt :
           type == NodeType::Damage ? kModDamage : type == NodeType::Invincible ? kModInvincible : type == NodeType::KeepLook ? kModKeepLook :
           type == NodeType::ForceHit ? kModForceHit : type == NodeType::IgnoreGuard ? kModIgnoreGuard : type == NodeType::KeepChain ? kModKeepChain : type == NodeType::Trim ? kModTrim :
           type == NodeType::Air ? kModAir : type == NodeType::BoneTransform ? kModBone : type == NodeType::BossEffect ? kModEffect : -1;
}
// A move 滞空 / 空中释放 act on: one the hero casts and moves in (not a 钩子 or a 瞬移, which carry the hero their own
// way, nor a 投技, a 援护 or a look change).
inline bool TakesAir(const Node& node) {
    return node.type == NodeType::Skill || node.type == NodeType::BossSkill || node.type == NodeType::WeaponMove || node.type == NodeType::AutoDodge;
}
// Independent animations played by the hero share the trim clock and cleanup path.
inline bool TakesTrim(const Node& node) { return TakesAir(node); }
inline bool HasAnimation(const Node& node) {
    return TakesTrim(node)||node.type==NodeType::Grab||node.type==NodeType::Assist||node.type==NodeType::Hook;
}
inline std::string DurationKey(const Node& node,const std::string& profile) {
    if(node.type==NodeType::AutoDodge)return "@dodge:"+profile;
    if(node.type==NodeType::Hook&&node.hook_auto)return "@hook:auto:"+profile;
    return node.ability;
}
// May node `from` point at modifier `mod`: a trigger may point at any; a move at 加速 / 霸体 / 允许中断 / 伤害倍率 / 无敌 /
// 强制受击 / 无视防御 / 保持派生;
// 保留变身 only a trigger, a BOSS 技能 or a frame (the BOSS 技能 it holds).
inline bool ModifierFits(const std::vector<Node>& chain, int from, int mod) {
    if (from < 0 || mod < 0 || static_cast<size_t>(from) >= chain.size() || static_cast<size_t>(mod) >= chain.size() || from == mod) return false;
    const Node& source = chain[static_cast<size_t>(from)]; const Node& target = chain[static_cast<size_t>(mod)];
    if (!IsModifier(target)) return false;
    if (target.type == NodeType::KeepLook) return StartsChain(source) || source.type == NodeType::BossSkill || source.type == NodeType::Group;
    if (target.type == NodeType::Trim) return TakesTrim(source);  // its own montage's part
    if (target.type == NodeType::Air) return StartsChain(source) || TakesAir(source) || source.type == NodeType::Group;
    if (target.type == NodeType::BossEffect) return StartsChain(source) || source.type == NodeType::Group || (IsMove(source) && source.type != NodeType::Look && source.type != NodeType::HeroLook);
    if (source.type == NodeType::Grab) return target.type == NodeType::Interrupt;
    return StartsChain(source) ||
           (target.type != NodeType::Weapon && IsMove(source) && source.type != NodeType::Grab && source.type != NodeType::Look && source.type != NodeType::HeroLook);
}
// A 变身 / 变回主角 node (set or not).
inline bool IsLookChange(const Node& node) { return node.type == NodeType::Look || node.type == NodeType::HeroLook; }
// Nodes with a 不变身 choice: a BOSS 技能 or a 投技 (both take a boss's look by default).
inline bool TakesOwnLook(const Node& node) { return node.type == NodeType::BossSkill || node.type == NodeType::Grab; }
// Nodes whose own_look is saved: those, and the chain starts (a trigger, a 武器派生修改: 整体不变身).
inline bool SavesOwnLook(const Node& node) { return TakesOwnLook(node) || node.type == NodeType::Trigger || node.type == NodeType::WeaponActions; }
// Can node `from` point at any modifier (has the pin for it)?
inline bool TakesModifiers(const Node& node) { return StartsChain(node) || (IsMove(node) && !IsLookChange(node)); }
// A node chains start from (a trigger or a 武器派生修改): no exec wire goes into it.
inline bool IsTrigger(const Config& config, int index) {
    return index >= 0 && static_cast<size_t>(index) < config.chain.size() && StartsChain(config.chain[static_cast<size_t>(index)]);
}
inline Node TriggerNode(Trigger trigger, int x, int y) { Node node; node.type = NodeType::Trigger; node.trigger = static_cast<int>(trigger); node.ms = kResetDefault; node.x = x; node.y = y; return node; }
// A branch with one row (the hero's light attack, no pause).
inline Node BranchNode(int x, int y) { Node node; node.type = NodeType::Branch; node.x = x; node.y = y; node.routes.push_back({}); return node; }
// A 武器派生修改 with no weapon yet and the rows 完美闪避, 处决, 简易处决, 格挡 (unwired).
inline Node WeaponActionsNode(int x, int y) {
    Node node; node.type = NodeType::WeaponActions; node.ms = kResetDefault; node.x = x; node.y = y;
    for (const Trigger t : {Trigger::PerfectDodge, Trigger::Execution, Trigger::SimpleExecution, Trigger::Stance}) { Route route; route.button = static_cast<int>(t); node.routes.push_back(route); }
    return node;
}
// Does the config have a 武器派生修改 (the hero's presses then name the main weapon)? A 空中派生 names none.
inline bool HasWeaponActions(const Config& config) {
    for (const Node& node : config.chain) if (node.type == NodeType::WeaponActions && !node.air_set) return true;
    return false;
}
// A 空中派生 with the rows 轻攻击, 重攻击, 影之武装, 战技 and NPC 技能键 (unwired).
inline Node AirSetNode(int x, int y) {
    Node node; node.type = NodeType::WeaponActions; node.air_set = true; node.ms = kResetDefault; node.x = x; node.y = y;
    for (const Trigger t : {Trigger::Light, Trigger::Heavy, Trigger::Shachi, Trigger::CombatArt, Trigger::NpcKey}) { Route route; route.button = static_cast<int>(t); node.routes.push_back(route); }
    return node;
}
// A 加速 (×1.5), 伤害倍率 (200%), 武器属性, 霸体, 允许中断, 无敌, 保留变身, 强制受击 (击飞), 无视防御 or 保持派生 (3 s) node;
// nothing points at it yet.
inline Node ModifierNode(NodeType type, int x, int y) {
    Node node; node.type = type; node.speed = type == NodeType::Damage ? kDamageNew : kSpeedNew; node.x = x; node.y = y;
    if (type == NodeType::KeepChain) node.ms = kKeepChainDefault;
    return node;
}
// The NPC skill key's trigger node (-1 when there is none).
inline int KeyTrigger(const Config& config) {
    for (size_t i = 0; i < config.chain.size(); ++i)
        if (config.chain[i].type == NodeType::Trigger && config.chain[i].trigger == static_cast<int>(Trigger::NpcKey)) return static_cast<int>(i);
    return -1;
}
// Every config has its NPC skill key node.
inline void EnsureKey(Config& config) { if (KeyTrigger(config) < 0) config.chain.push_back(TriggerNode(Trigger::NpcKey, 24, 24)); }
// 预输入: the chain's one fixed node (never removed, no wires).
inline bool IsPreInput(const Node& node) { return node.type == NodeType::PreInput; }
// The chain's fixed settings (预输入, 空中释放): never removed, no wires, not in a frame.
inline bool IsSetting(const Node& node) { return node.type == NodeType::PreInput || node.type == NodeType::AirDefault; }
inline int PreInputNode(const Config& config) {
    for (size_t i = 0; i < config.chain.size(); ++i) if (IsPreInput(config.chain[i])) return static_cast<int>(i);
    return -1;
}
// How long a press made while a move plays is kept (ms; 0: until it plays or the chain ends).
inline int PreInputMs(const Config& config) {
    const int i = PreInputNode(config);
    return i >= 0 ? ClampPreInput(config.chain[static_cast<size_t>(i)].ms) : kPreInputDefault;
}
// Every chain the page and the runner load has its 预输入 node (kPreInputDefault), added above the other nodes.
inline void EnsurePreInput(Config& config) {
    if (PreInputNode(config) >= 0) return;
    int x = 24, y = 24;
    if (!config.chain.empty()) {
        x = y = kCanvasLimit;
        for (const Node& n : config.chain) { x = (std::min)(x, n.x); y = (std::min)(y, n.y); }
    }
    Node node; node.type = NodeType::PreInput; node.ms = kPreInputDefault; node.x = ClampPosition(x); node.y = ClampPosition(y - 120);
    config.chain.push_back(node);
}
// 空中释放: the chain's other fixed node (on, kAir*Default), placed right of the 预输入.
inline int AirDefaultNode(const Config& config) {
    for (size_t i = 0; i < config.chain.size(); ++i) if (config.chain[i].type == NodeType::AirDefault) return static_cast<int>(i);
    return -1;
}
inline Node AirDefaultSetting(int x, int y) {
    Node node; node.type = NodeType::AirDefault; node.on = true; node.x = ClampPosition(x); node.y = ClampPosition(y);
    node.air_lift = kAirSelfLiftDefault; node.air_hang = kAirSelfHangDefault; node.air_foe_lift = kAirFoeLiftDefault; node.air_foe_hang = kAirFoeHangDefault;
    return node;
}
inline void EnsureAirDefault(Config& config) {
    if (AirDefaultNode(config) >= 0) return;
    const int pre = PreInputNode(config);
    int x = 24, y = 24;
    if (pre >= 0) { x = config.chain[static_cast<size_t>(pre)].x + 460; y = config.chain[static_cast<size_t>(pre)].y; }
    else if (!config.chain.empty()) {
        x = y = kCanvasLimit;
        for (const Node& n : config.chain) { x = (std::min)(x, n.x); y = (std::min)(y, n.y); }
        y -= 120;
    }
    config.chain.push_back(AirDefaultSetting(x, y));
}

// A node's exec outputs: one, one per row of a branch or a 武器派生修改, none for a modifier (its wire is not an exec
// wire).
inline size_t OutCount(const Node& node) { return HasRows(node) ? node.routes.size() : IsModifier(node) || IsSetting(node) ? 0 : 1; }
inline int OutOf(const Node& node, size_t out) {
    if (IsModifier(node) || IsSetting(node)) return -1;
    if (HasRows(node)) return out < node.routes.size() ? node.routes[out].next : -1;
    return out == 0 ? node.next : -1;
}
inline void SetOut(Node& node, size_t out, int to) {
    if (IsModifier(node) || IsSetting(node)) return;
    if (HasRows(node)) { if (out < node.routes.size()) node.routes[out].next = to; }
    else if (out == 0) node.next = to;
}
// Is `to` wired from any output of `node`?
inline bool Feeds(const Node& node, int to) {
    for (size_t o = 0; o < OutCount(node); ++o) if (OutOf(node, o) == to) return true;
    return false;
}
// The nodes the wires reach from node `from`, depth first (a branch's routes in row order); each node once.
inline std::vector<size_t> Order(const Config& config, int from) {
    std::vector<size_t> order;
    const int count = static_cast<int>(config.chain.size());
    if (from < 0 || from >= count) return order;
    std::vector<bool> seen(config.chain.size(), false); seen[static_cast<size_t>(from)] = true;
    std::vector<int> stack;
    const auto push = [&](size_t i) { const Node& n = config.chain[i]; for (size_t o = OutCount(n); o-- > 0;) stack.push_back(OutOf(n, o)); };
    push(static_cast<size_t>(from));
    while (!stack.empty()) {
        const int i = stack.back(); stack.pop_back();
        if (i < 0 || i >= count || seen[static_cast<size_t>(i)]) continue;
        seen[static_cast<size_t>(i)] = true; order.push_back(static_cast<size_t>(i)); push(static_cast<size_t>(i));
    }
    return order;
}
// The trigger (or 武器派生修改) whose wires reach a node (-1: none); the first one wins.
inline int TriggerOf(const Config& config, size_t node) {
    for (size_t t = 0; t < config.chain.size(); ++t) {
        if (!StartsChain(config.chain[t])) continue;
        if (t == node) return static_cast<int>(t);
        for (const size_t i : Order(config, static_cast<int>(t))) if (i == node) return static_cast<int>(t);
    }
    return -1;
}
// Does a BOSS 技能 / 投技 `move` of the chain `trigger` started play 不变身: its own, or its start's 整体不变身?
inline bool OwnLookFor(const Config& config, int trigger, int move) {
    const int count = static_cast<int>(config.chain.size());
    if (move < 0 || move >= count || !TakesOwnLook(config.chain[static_cast<size_t>(move)])) return false;
    if (config.chain[static_cast<size_t>(move)].own_look) return true;
    return trigger >= 0 && trigger < count && StartsChain(config.chain[static_cast<size_t>(trigger)]) && config.chain[static_cast<size_t>(trigger)].own_look;
}
// Does a BOSS 技能 / 投技 play 不变身 whichever chain plays it: its own, or every start whose wires reach it (or a 组合
// it sits in) is 整体不变身 (one at least). The page's hint and the look preload go by it.
inline bool OwnLookEverywhere(const Config& config, size_t node) {
    if (node >= config.chain.size() || !TakesOwnLook(config.chain[node])) return false;
    if (config.chain[node].own_look) return true;
    std::vector<int> holders{static_cast<int>(node)};
    for (int at = config.chain[node].group, depth = 0; at >= 0 && static_cast<size_t>(at) < config.chain.size() && depth <= kGroupDepthMax;
         at = config.chain[static_cast<size_t>(at)].group, ++depth) holders.push_back(at);
    bool any = false;
    for (size_t t = 0; t < config.chain.size(); ++t) {
        if (!StartsChain(config.chain[t])) continue;
        const std::vector<size_t> order = Order(config, static_cast<int>(t));
        if (std::none_of(order.begin(), order.end(), [&](size_t i) { return std::find(holders.begin(), holders.end(), static_cast<int>(i)) != holders.end(); })) continue;
        if (!config.chain[t].own_look) return false;
        any = true;
    }
    return any;
}
// All first-trigger owners in one traversal. An earlier trigger's reachable
// subgraph is already complete, so later triggers never need to walk it again.
inline std::vector<int> Owners(const Config& config) {
    std::vector<int> owners(config.chain.size(),-1),stack;
    for(size_t root=0;root<config.chain.size();++root){
        if(!StartsChain(config.chain[root])||owners[root]>=0)continue;
        stack.push_back(static_cast<int>(root));
        while(!stack.empty()){
            const int index=stack.back();stack.pop_back();
            if(index<0||static_cast<size_t>(index)>=owners.size()||owners[static_cast<size_t>(index)]>=0)continue;
            owners[static_cast<size_t>(index)]=static_cast<int>(root);
            const auto& node=config.chain[static_cast<size_t>(index)];
            for(size_t out=0;out<OutCount(node);++out)stack.push_back(OutOf(node,out));
        }
    }
    return owners;
}
// Wires output `out` of `from` (a branch's route row; 0 otherwise) to `to` (-1: unwired). No exec wire goes into a
// trigger, a modifier or the node itself, and none comes out of a modifier.
inline void Link(Config& config, int from, int to, size_t out = 0) {
    const int count = static_cast<int>(config.chain.size());
    if (from < 0 || from >= count || to < -1 || to >= count || from == to) return;
    Node& node = config.chain[static_cast<size_t>(from)];
    if (IsModifier(node) || IsSetting(node)) return;
    if (to >= 0 && (IsTrigger(config, to) || IsModifier(config.chain[static_cast<size_t>(to)]) || IsSetting(config.chain[static_cast<size_t>(to)]))) return;
    SetOut(node, out, to);
}
// Points node `from` at modifier `mod` (replacing what it pointed at in that modifier's slot); false when it may not.
inline bool LinkModifier(Config& config, int from, int mod) {
    if (!ModifierFits(config.chain, from, mod)) return false;
    config.chain[static_cast<size_t>(from)].mods[static_cast<size_t>(SlotOf(config.chain[static_cast<size_t>(mod)].type))] = mod;
    return true;
}
// Breaks node `from`'s arrows to modifiers (slot -1: all of them).
inline void UnlinkModifiers(Config& config, int from, int slot = -1) {
    if (from < 0 || static_cast<size_t>(from) >= config.chain.size()) return;
    for (int s = 0; s < kModSlots; ++s) if (slot < 0 || slot == s) config.chain[static_cast<size_t>(from)].mods[static_cast<size_t>(s)] = -1;
}
// Breaks every arrow into modifier `mod`.
inline void UnlinkInto(Config& config, int mod) {
    for (Node& node : config.chain) for (int& m : node.mods) if (m == mod) m = -1;
}
// How many nodes point at modifier `mod`, and how many of them are triggers.
struct Pointers { int nodes = 0, triggers = 0; };
inline Pointers PointersTo(const Config& config, int mod) {
    Pointers out;
    for (const Node& node : config.chain) for (const int m : node.mods) if (m == mod && m >= 0) { ++out.nodes; if (StartsChain(node)) ++out.triggers; }
    return out;
}
// Adds `node` wired in after output `out` of `after` (-1: unwired); what that output fed now follows the new node
// (a branch takes it on its first row).
inline int InsertAfter(Config& config, int after, Node node, size_t out = 0) {
    if (config.chain.size() >= kMaxNodes) return -1;
    const bool wire = after >= 0 && static_cast<size_t>(after) < config.chain.size() && !StartsChain(node) && !IsModifier(node) && !IsSetting(node) &&
                      out < OutCount(config.chain[static_cast<size_t>(after)]);
    const int fed = wire ? OutOf(config.chain[static_cast<size_t>(after)], out) : -1;
    node.next = -1; node.mods.fill(-1);
    for (Route& route : node.routes) route.next = -1;
    if (node.type == NodeType::Branch && node.routes.empty()) node.routes.push_back({});
    SetOut(node, 0, fed);
    config.chain.push_back(node);
    const int index = static_cast<int>(config.chain.size()) - 1;
    if (wire) Link(config, after, index, out);
    return index;
}
// The end of a trigger's main line: its wires followed (a branch by its first row) until one is unwired.
inline int Tail(const Config& config, int trigger) {
    const int count = static_cast<int>(config.chain.size());
    if (trigger < 0 || trigger >= count) return trigger;
    std::vector<bool> seen(config.chain.size(), false);
    int at = trigger;
    while (true) {
        seen[static_cast<size_t>(at)] = true;
        const int next = OutOf(config.chain[static_cast<size_t>(at)], 0);
        if (next < 0 || next >= count || seen[static_cast<size_t>(next)]) return at;
        at = next;
    }
}
// Removes a node; every exec wire into it goes on to what its first output fed, so the chain stays joined; arrows
// into a removed modifier go. The 预输入 and 空中释放 nodes stay.
inline void Remove(Config& config, size_t index) {
    if (index >= config.chain.size() || IsSetting(config.chain[index])) return;
    const int gone = static_cast<int>(index);
    const int parent = config.chain[index].group;  // a removed 组合's nodes go to the frame it sat in
    int after = OutOf(config.chain[index], 0); if (after == gone) after = -1;
    config.chain.erase(config.chain.begin() + static_cast<std::ptrdiff_t>(index));
    const auto shift = [gone](int link) { return link > gone ? link - 1 : link; };
    after = shift(after);
    for (size_t i = 0; i < config.chain.size(); ++i) {
        Node& node = config.chain[i];
        for (int& m : node.mods) m = m == gone ? -1 : shift(m);
        if (node.group >= 0) node.group = node.group == gone ? (parent == gone ? -1 : shift(parent)) : shift(node.group);
        if (node.group == static_cast<int>(i)) node.group = -1;
        for (size_t o = 0; o < OutCount(node); ++o) {
            const int link = OutOf(node, o);
            int to = link == gone ? after : shift(link);
            if (to == static_cast<int>(i)) to = -1;  // never a wire to itself
            SetOut(node, o, to);
        }
    }
}

// What a press is: the hero action's tag bits, or the NPC skill key; weapon: the hero's main weapon (backpack id) then,
// for a 处决 trigger that names one and for 武器派生修改.
// combo: the 自定义组合键 trigger whose keys were pressed (-1: none); branch, row: the 自定义组合键 / 自定义按键 branch row
// whose keys were pressed (-1: none; bits then hold that row's button). then: a press of the 继续派生 key of the
// 自定义按键 chain going on (bits 0: it starts nothing).
// hold_ms: how long the button has been held when the press is taken (按住; 0: the moment it went down).
// airborne: the hero was in the air at the press (a 空中派生 takes it).
struct Press { unsigned bits = 0; bool key = false; std::string weapon; int combo = -1; int branch = -1, row = -1; bool then = false; int hold_ms = 0; bool airborne = false; };
// An unmapped native action is a detour, not an instruction to reset a live chain.
// Explicit graph keys/rows keep their own routing and reset semantics.
inline bool NativeDetour(const Press& press,bool active) {
    return active&&press.bits&&!press.key&&press.combo<0&&press.branch<0&&!press.then;
}
inline bool Matches(int button, const Press& press) {
    if (button == kThenKeyButton) return press.then;
    if (press.then) return false;
    if (button == static_cast<int>(Trigger::NpcKey)) return press.key;
    return !press.key && ValidButton(button) && Fires(static_cast<Trigger>(button), press.bits);
}
// Where a press goes from `from` (a trigger, or the move that played last): the move it plays (-1: none), the
// intervals on the way (ms from that move's start, or from the press after a trigger), the button the chain is on
// after it and the pause of the branch row taken (0: none). `button` is the one the chain is on now: a wire straight
// to a move means the same button again. A branch takes the row of the pressed button whose pause has passed
// (since_ms: since arriving at the branch on this visit, the move before at its 派生点 or end plus the intervals
// before the branch, ComboPlayback::NodeClock; -1: not there yet, or a chain starting), the longest such pause when several
// do; an unwired row never matches. A 自定义组合键 / 自定义按键 row matches only its own keys (Press::branch / row).
// branch, row: the branch row the hop took (-1: none). assists: the BOSS 援护 nodes the press passed on its way to
// `move` (they are called in as it plays); a press that meets nothing to play after a 援护 has that 援护 as its move.
// hold_ms: the 按住 of the branch row the hop took (0: none).
struct Hop { int move = -1; int wait_ms = 0; int button = 0; int pause_ms = 0; int branch = -1, row = -1; std::vector<int> assists; int hold_ms = 0; };
// 派生显示 简略: the presses that take the chain on from `from` (the node it is at; `button`: the one it is on), each
// with the move it plays. A wire on to a move is `button` again; a 按键分支 there gives a row each (its button, 按住
// and pause; a 自定义组合键 / 自定义按键 row's keys are the branch's row); a 武器派生修改 a row each. Intervals and bad
// names on the way are passed, so are 援护 (a 援护 with nothing after it, or a branch after it, is the move). At most
// `limit` (the first rows).
struct NextInput { int button = 0; int move = -1; int hold_ms = 0; int pause_ms = 0; int branch = -1, row = -1; };
inline std::vector<NextInput> NextInputs(const Config& config, int from, int button, size_t limit = 8);
// Is this row's press its keys (a 自定义组合键 / 自定义按键 row), and are they enough to press (2 to 6 buttons; 1 to 6 keys)?
inline bool KeysRoute(const Route& route) { return OwnKeysTrigger(route.button); }
inline bool KeysRouteReady(const Route& route) {
    if (!KeysRoute(route) || route.keys.size() > kComboKeysMax) return false;
    const bool raw = route.button == static_cast<int>(Trigger::Keys);
    if (route.keys.size() < (raw ? size_t{1} : kComboKeysMin)) return false;
    for (const int key : route.keys) if (raw ? !ValidRawKey(key) : !ValidComboKey(key)) return false;
    return true;
}
// How specific a hero action is among those one press fires: a dodge's own over 任意闪避, 简易处决 over 处决.
inline int ActionRank(int button) { return button == static_cast<int>(Trigger::AnyDodge) || button == static_cast<int>(Trigger::Execution) ? 0 : 1; }
// The row of a 武器派生修改 a press takes (-1: none): its weapon is the press's (a 空中派生: the press was made in the
// air), the row is wired and fires on the press; the most specific such row (ActionRank), the first of those.
inline int WeaponActionsRow(const Node& node, const Press& press, int node_index=-1) {
    if (node.type != NodeType::WeaponActions || !node.on) return -1;
    if (node.air_set ? !press.airborne : node.weapon.empty() || press.key || node.weapon != press.weapon) return -1;
    int best = -1;
    for (size_t r = 0; r < node.routes.size(); ++r) {
        const Route& route = node.routes[r];
        if (route.next < 0) continue;
        if (ClampHold(route.hold_ms) > press.hold_ms) continue;
        if (KeysRoute(route) && (node_index<0 || press.branch!=node_index || press.row!=static_cast<int>(r) || !KeysRouteReady(route))) continue;
        if (node.air_set ? !ValidAirAction(route.button) || !Matches(route.button, press) :
                           !ValidWeaponAction(route.button) || !Fires(static_cast<Trigger>(route.button), press.bits)) continue;
        if (best < 0 || ActionRank(route.button) > ActionRank(node.routes[static_cast<size_t>(best)].button) ||
            (ActionRank(route.button) == ActionRank(node.routes[static_cast<size_t>(best)].button) && ClampHold(route.hold_ms) > ClampHold(node.routes[static_cast<size_t>(best)].hold_ms))) best = static_cast<int>(r);
    }
    return best;
}
inline std::vector<NextInput> NextInputs(const Config& config, int from, int button, size_t limit) {
    std::vector<NextInput> out;
    const int count = static_cast<int>(config.chain.size());
    if (from < 0 || from >= count) return out;
    // Where a wire from `i` leads: a move, a 按键分支 (none after a 援护), or -1.
    const auto reach = [&](int i) {
        std::vector<bool> seen(config.chain.size(), false);
        int assist = -1;
        while (i >= 0 && i < count && !seen[static_cast<size_t>(i)]) {
            seen[static_cast<size_t>(i)] = true;
            const Node& node = config.chain[static_cast<size_t>(i)];
            if (node.type == NodeType::Interval || EmptyMove(node)) { i = node.next; continue; }
            if (node.type == NodeType::Assist) { assist = i; i = node.next; continue; }
            if (node.type == NodeType::Branch) return assist >= 0 ? assist : i;
            if (IsMove(node)) return i;
            break;
        }
        return assist;
    };
    const auto rows = [&](int at) {
        const Node& node = config.chain[static_cast<size_t>(at)];
        for (size_t r = 0; r < node.routes.size() && out.size() < limit; ++r) {
            const Route& route = node.routes[r];
            if (route.next < 0) continue;
            const int move = reach(route.next);
            if (move < 0 || config.chain[static_cast<size_t>(move)].type == NodeType::Branch) continue;
            NextInput in; in.button = route.button; in.move = move; in.hold_ms = ClampHold(route.hold_ms);
            in.pause_ms = node.type == NodeType::Branch ? ClampPause(route.pause_ms) : 0; in.branch = at; in.row = static_cast<int>(r);
            out.push_back(in);
        }
    };
    const Node& origin = config.chain[static_cast<size_t>(from)];
    if (HasRows(origin)) { rows(from); return out; }
    const int to = reach(origin.next);
    if (to < 0 || limit == 0) return out;
    if (config.chain[static_cast<size_t>(to)].type == NodeType::Branch) { rows(to); return out; }
    NextInput in; in.button = button; in.move = to; out.push_back(in);
    return out;
}
inline Hop Follow(const Config& config, int from, int button, const Press& press, int since_ms) {
    const int count = static_cast<int>(config.chain.size());
    if (from < 0 || from >= count || config.chain[static_cast<size_t>(from)].type == NodeType::Branch) return {};
    Hop hop; hop.button = button;
    std::vector<bool> seen(config.chain.size(), false); seen[static_cast<size_t>(from)] = true;
    const Node& origin = config.chain[static_cast<size_t>(from)];
    const bool then = HasThen(origin) && button == origin.trigger;  // its first move: the own keys, then the 继续派生 key
    int i = origin.next;
    if (config.chain[static_cast<size_t>(from)].type == NodeType::WeaponActions) {  // from the row of the press's action
        const Node& start = config.chain[static_cast<size_t>(from)];
        const int row = WeaponActionsRow(start, press, from);
        if (row < 0) return {};
        hop.button = start.routes[static_cast<size_t>(row)].button; i = start.routes[static_cast<size_t>(row)].next;
        hop.hold_ms = ClampHold(start.routes[static_cast<size_t>(row)].hold_ms);
    }
    // Past a BOSS 援护 a press that finds nothing more to play stops at the last 援护 (it is the hop's move).
    const auto rest = [&]() -> Hop {
        if (hop.assists.empty()) return {};
        Hop last = hop; last.move = last.assists.back(); last.assists.pop_back();
        if (then && last.branch < 0) last.button = ThenButton(origin);
        last.wait_ms = (std::min)(last.wait_ms, kIntervalMax * 4); return last;
    };
    while (i >= 0 && i < count && !seen[static_cast<size_t>(i)]) {
        seen[static_cast<size_t>(i)] = true;
        const Node& node = config.chain[static_cast<size_t>(i)];
        if (node.type == NodeType::Interval) { hop.wait_ms += ClampInterval(node.ms); i = node.next; continue; }
        if (EmptyMove(node)) { i = node.next; continue; }  // a bad name passes on
        if (node.type == NodeType::Assist) {  // called in on the way: the press goes on to what follows
            if (!Matches(hop.button, press)) return {};
            hop.assists.push_back(i); i = node.next; continue;
        }
        if (IsMove(node)) {
            if (!Matches(hop.button, press)) return {};
            if (then && hop.branch < 0) hop.button = ThenButton(origin);
            hop.move = i; hop.wait_ms = (std::min)(hop.wait_ms, kIntervalMax * 4); return hop;
        }
        if (node.type != NodeType::Branch) return rest();
        if (!hop.assists.empty()) return rest();  // a branch after a 援护 is the next press's
        // The longest hold the press has reached first (按住), then the longest pause.
        int best = -1;
        for (size_t r = 0; r < node.routes.size(); ++r) {
            const Route& route = node.routes[r];
            if (route.next < 0) continue;
            if (KeysRoute(route) ? press.branch != i || press.row != static_cast<int>(r) || !KeysRouteReady(route) : !Matches(route.button, press)) continue;
            const int pause = ClampPause(route.pause_ms), hold = ClampHold(route.hold_ms);
            if (pause > 0 && (since_ms < 0 || since_ms < pause)) continue;
            if (hold > press.hold_ms) continue;
            if (best < 0) { best = static_cast<int>(r); continue; }
            const Route& was = node.routes[static_cast<size_t>(best)];
            const int was_hold = ClampHold(was.hold_ms);
            if (hold > was_hold || (hold == was_hold && pause > ClampPause(was.pause_ms))) best = static_cast<int>(r);
        }
        if (best < 0) return {};
        hop.button = node.routes[static_cast<size_t>(best)].button; hop.pause_ms = ClampPause(node.routes[static_cast<size_t>(best)].pause_ms);
        hop.hold_ms = (std::max)(hop.hold_ms, ClampHold(node.routes[static_cast<size_t>(best)].hold_ms));
        hop.branch = i; hop.row = best;
        i = node.routes[static_cast<size_t>(best)].next;
    }
    return rest();
}
// The trigger a press starts (an `on` trigger with a chain whose button it is and whose 按住 the press has reached; -1
// none): the one with the longest such hold, the first of those. A 空中派生 with a row for a press made in the air goes
// first, then a 武器派生修改 of the press's weapon with a row for it.
inline int TriggerFor(const Config& config, const Press& press) {
    for (size_t i = 0; i < config.chain.size(); ++i) if (config.chain[i].air_set && WeaponActionsRow(config.chain[i], press, static_cast<int>(i)) >= 0) return static_cast<int>(i);
    for (size_t i = 0; i < config.chain.size(); ++i) if (WeaponActionsRow(config.chain[i], press, static_cast<int>(i)) >= 0) return static_cast<int>(i);
    int best = -1;
    for (size_t i = 0; i < config.chain.size(); ++i) {
        const Node& node = config.chain[i];
        if (node.type == NodeType::Trigger && node.on && node.next >= 0 && Matches(node.trigger, press) &&
            (!WeaponTrigger(node.trigger) || node.weapon.empty() || node.weapon == press.weapon) &&
            (!OwnKeysTrigger(node.trigger) || press.combo == static_cast<int>(i)) && ClampHold(node.hold_ms) <= press.hold_ms &&
            (best < 0 || ClampHold(node.hold_ms) > ClampHold(config.chain[static_cast<size_t>(best)].hold_ms)))
            best = static_cast<int>(i);
    }
    return best;
}
// The 按住 a trigger or a 武器派生修改 needs (a 武器派生修改: none).
inline int TriggerHold(const Config& config, int start) {
    return start >= 0 && static_cast<size_t>(start) < config.chain.size() && config.chain[static_cast<size_t>(start)].type == NodeType::Trigger ?
           ClampHold(config.chain[static_cast<size_t>(start)].hold_ms) : 0;
}
// The button a chain starts on from `start`: a trigger's own, or the 武器派生修改 row the press takes (-1: none).
inline int StartButton(const Config& config, int start, const Press& press) {
    if (start < 0 || static_cast<size_t>(start) >= config.chain.size()) return -1;
    const Node& node = config.chain[static_cast<size_t>(start)];
    if (node.type != NodeType::WeaponActions) return node.trigger;
    const int row = WeaponActionsRow(node, press, start);
    return row < 0 ? -1 : node.routes[static_cast<size_t>(row)].button;
}
// 自定义组合键 presses: one per frame with any of the hero's buttons going down (bits of every button down then).
struct KeyPress { unsigned bits = 0; unsigned long long at = 0; };
// Weapon-action chords allow a modifier already held (block, then E).
// Require a fresh edge of one member, so a held chord cannot fire every frame.
inline bool HeldActionChord(const std::vector<int>& keys,unsigned held,unsigned went){
    if(keys.size()<kComboKeysMin||keys.size()>kComboKeysMax)return false;bool edge=false;
    for(int key:keys){if(!ValidComboKey(key)||!Fires(static_cast<Trigger>(key),held))return false;edge=edge||Fires(static_cast<Trigger>(key),went);}return edge;
}
inline bool HeldRawChord(const std::vector<int>& keys,const std::vector<int>& held,const std::vector<int>& went){
    if(keys.empty()||keys.size()>kComboKeysMax)return false;bool edge=false;
    for(int key:keys){if(!ValidRawKey(key)||std::find(held.begin(),held.end(),key)==held.end())return false;edge=edge||std::find(went.begin(),went.end(),key)!=went.end();}return edge;
}
// In order: the newest presses are the keys one after another, each within `window` ms of the one before.
inline bool SequenceDone(const std::vector<KeyPress>& presses, const std::vector<int>& keys, int window) {
    if (keys.size() < kComboKeysMin || presses.size() < keys.size()) return false;
    const size_t base = presses.size() - keys.size();
    for (size_t k = 0; k < keys.size(); ++k) {
        const KeyPress& p = presses[base + k];
        if (!ValidComboKey(keys[k]) || !Fires(static_cast<Trigger>(keys[k]), p.bits)) return false;
        if (k && p.at > presses[base + k - 1].at + static_cast<unsigned long long>(ClampKeyWindow(window))) return false;
    }
    return true;
}
// Together: every key went down within `chord` ms up to the newest press, and the newest press is one of them.
inline bool ChordDone(const std::vector<KeyPress>& presses, const std::vector<int>& keys, int chord) {
    if (keys.size() < kComboKeysMin || presses.empty()) return false;
    const unsigned long long last = presses.back().at;
    bool newest = false;
    for (const int key : keys) {
        if (!ValidComboKey(key)) return false;
        bool found = false;
        for (size_t i = presses.size(); i-- > 0;) {
            if (presses[i].at + static_cast<unsigned long long>(chord) < last) break;
            if (Fires(static_cast<Trigger>(key), presses[i].bits)) { found = true; newest = newest || i + 1 == presses.size(); break; }
        }
        if (!found) return false;
    }
    return newest;
}
// 自定义按键 presses: one per key going down (keys down in one frame share the time).
struct RawPress { int code = 0; unsigned long long at = 0; };
// One key: its press. In order: the newest presses are the keys one after another, each within `window` ms of the one
// before. Together: every key went down within `chord` ms up to the newest press, which is one of them.
inline bool RawKeysDone(const std::vector<RawPress>& presses, const std::vector<int>& keys, bool together, int window, int chord) {
    if (keys.empty() || keys.size() > kComboKeysMax || presses.empty()) return false;
    for (const int key : keys) if (!ValidRawKey(key)) return false;
    if (keys.size() == 1) return presses.back().code == keys[0];
    if (!together) {
        if (presses.size() < keys.size()) return false;
        const size_t base = presses.size() - keys.size();
        for (size_t k = 0; k < keys.size(); ++k) {
            if (presses[base + k].code != keys[k]) return false;
            if (k && presses[base + k].at > presses[base + k - 1].at + static_cast<unsigned long long>(ClampKeyWindow(window))) return false;
        }
        return true;
    }
    const unsigned long long last = presses.back().at;
    if (std::find(keys.begin(), keys.end(), presses.back().code) == keys.end()) return false;
    for (const int key : keys) {
        bool found = false;
        for (size_t i = presses.size(); i-- > 0;) {
            if (presses[i].at + static_cast<unsigned long long>(chord) < last) break;
            if (presses[i].code == key) { found = true; break; }
        }
        if (!found) return false;
    }
    return true;
}
// The keys for the log: "pad:LB -> vk:0x51" in order, "+" together (a node's or a branch row's).
template <class Spec> inline std::string RawKeysToken(const Spec& node) {
    std::string out;
    for (const int key : node.keys) out += (out.empty() ? "" : node.chord ? " + " : " -> ") + RawKeyToken(key);
    return out;
}
// 加速 held on a montage: the rate to write now, 0 to leave it. `factor` is the 加速 (our rate over the game's when we
// first set it); `written` the rate we wrote last. A rate the game set since (a hit stop, a notify, the ability's own
// rate) is scaled by the factor; 0 (paused) is left alone.
inline double HeldRate(double current, double written, double factor) {
    if (!(current > 0) || !(factor > 0) || std::fabs(factor - 1) < 0.01) return 0;
    if (written > 0 && std::fabs(current - written) <= written * 0.01) return 0;
    return current * factor;
}
// The keys as text: "轻攻击 → 重攻击" in order, "轻攻击 + 重攻击" together (a node's or a branch row's).
template <class Spec> inline std::wstring ComboKeysText(const Spec& node) {
    std::wstring out;
    for (const int key : node.keys) out += (out.empty() ? L"" : node.chord ? L" + " : L" → ") + std::wstring(ComboKeyLabel(key));
    return out;
}
// The intervals straight after a move, before any branch: the move is slowed to fill them.
inline int SharedWait(const Config& config, int move) {
    const int count = static_cast<int>(config.chain.size());
    if (move < 0 || move >= count) return 0;
    std::vector<bool> seen(config.chain.size(), false); seen[static_cast<size_t>(move)] = true;
    int ms = 0, i = config.chain[static_cast<size_t>(move)].next;
    while (i >= 0 && i < count && !seen[static_cast<size_t>(i)] && config.chain[static_cast<size_t>(i)].type == NodeType::Interval) {
        seen[static_cast<size_t>(i)] = true; ms += ClampInterval(config.chain[static_cast<size_t>(i)].ms); i = config.chain[static_cast<size_t>(i)].next;
    }
    return (std::min)(ms, kIntervalMax * 4);
}
// The branch the next press after a move meets first (past intervals and bad names; -1: none).
inline int BranchAfter(const Config& config, int move) {
    const int count = static_cast<int>(config.chain.size());
    if (move < 0 || move >= count) return -1;
    std::vector<bool> seen(config.chain.size(), false); seen[static_cast<size_t>(move)] = true;
    int i = config.chain[static_cast<size_t>(move)].type == NodeType::Branch ? -1 : config.chain[static_cast<size_t>(move)].next;
    while (i >= 0 && i < count && !seen[static_cast<size_t>(i)]) {
        seen[static_cast<size_t>(i)] = true;
        const Node& node = config.chain[static_cast<size_t>(i)];
        if (node.type == NodeType::Branch) return i;
        if (node.type != NodeType::Interval && !EmptyMove(node)) return -1;
        i = node.next;
    }
    return -1;
}
// The longest pause a row of that branch waits for: the chain is kept that much longer after the last press.
inline int MaxPause(const Config& config, int move) {
    const int branch = BranchAfter(config, move);
    if (branch < 0) return 0;
    int most = 0;
    for (const Route& route : config.chain[static_cast<size_t>(branch)].routes) if (route.next >= 0) most = (std::max)(most, ClampPause(route.pause_ms));
    return most;
}
// A move's 派生点 in ms from its start (0: none, it is cut when it ends).
inline int CancelOf(const Config& config, int move) {
    if (move < 0 || static_cast<size_t>(move) >= config.chain.size() || !IsMove(config.chain[static_cast<size_t>(move)]) ||
        config.chain[static_cast<size_t>(move)].type == NodeType::Grab || config.chain[static_cast<size_t>(move)].type == NodeType::Assist ||
        IsLookChange(config.chain[static_cast<size_t>(move)])) return 0;
    return ClampCancel(config.chain[static_cast<size_t>(move)].ms);
}
// When the next move may cut a playing one, in ms from its start: its 派生点 or the end of the intervals straight
// after it, whichever comes first (0: neither, it is cut when it ends).
inline int CutMs(const Config& config, int move) {
    const int cancel = CancelOf(config, move), shared = SharedWait(config, move);
    if (cancel > 0 && shared > 0) return (std::min)(cancel, shared);
    return cancel > 0 ? cancel : shared;
}
// Does this press lead nowhere now but somewhere after a longer pause (a branch row it is too early for)?
inline bool TooEarly(const Config& config, int from, int button, const Press& press, int since_ms) {
    return Follow(config, from, button, press, since_ms).move < 0 && Follow(config, from, button, press, kPauseMax).move >= 0;
}
// ---- 预输入与停顿 ----
// A branch row's pause counts from arriving at the branch on this visit: the move before reaching its 派生点 (or its
// end) plus the intervals before the branch; a loading move or an earlier press spends none of it. The chain keeps
// one press (预输入). Until it plays - the move before reaches its 派生点 or ends, the intervals on the way are over,
// a kept pause row's pause is over - every newer press that leads somewhere takes its place (the last press wins);
// a press that leads nowhere leaves it. Before the 派生点 a kept press lives the chain's 预输入 ms from the press,
// whatever path it takes; one still alive at the 派生点 waits out the intervals and pause after it (they spend none
// of it). A press too early for every row of its button (no row now, one after a longer pause) is kept for the
// shortest such pause and plays then unless a newer press comes. After a move the chain waits for the next press the
// trigger's reset time from the move's end, and with a branch row waiting for its pause, until that pause (from
// arriving) plus the larger of the reset time and kPauseGraceMs (ComboPlayback::NodeClock::Deadline).
constexpr int kPauseGraceMs = 1500;
// The shortest pause after which this press leads somewhere, and where (move -1: it never does). since_ms as Follow.
struct Later { Hop hop; int pause_ms = 0; };
inline Later NextPause(const Config& config, int from, int button, const Press& press, int since_ms) {
    std::vector<int> pauses;
    for (const Node& node : config.chain)
        if (node.type == NodeType::Branch)
            for (const Route& route : node.routes) {
                const int pause = ClampPause(route.pause_ms);
                if (pause > 0 && pause > since_ms) pauses.push_back(pause);
            }
    std::sort(pauses.begin(), pauses.end());
    pauses.erase(std::unique(pauses.begin(), pauses.end()), pauses.end());
    for (const int pause : pauses) {
        const Hop hop = Follow(config, from, button, press, pause);
        if (hop.move >= 0) return {hop, pause};
    }
    return {};
}
// What a press does from `from`, since_ms after arriving at the branch (as Follow): a move now (Follow), or - too early for every row of
// its button - the move it reaches once the shortest such pause is over (later, ready_in_ms from now).
struct Decision { Hop hop; bool later = false; int ready_in_ms = 0; };
inline Decision Decide(const Config& config, int from, int button, const Press& press, int since_ms) {
    Decision decision;
    decision.hop = Follow(config, from, button, press, since_ms);
    if (decision.hop.move >= 0) return decision;
    const Later later = NextPause(config, from, button, press, since_ms);
    if (later.hop.move < 0) return decision;
    decision.hop = later.hop; decision.later = true; decision.ready_in_ms = later.pause_ms - (std::max)(since_ms, 0);
    return decision;
}
// The press before (the logs, and what a cleared 预输入 restores), after this one at `now`: a press that keeps or
// replaces a move now; a press kept for its pause leaves the one before. Pauses do not count from it.
inline uint64_t PressBefore(uint64_t last_press, const Decision& decision, uint64_t now) {
    return decision.hop.move >= 0 && !decision.later ? now : last_press;
}
// Does any move follow `from` on the wires (else the chain is over after it)?
inline bool Continues(const Config& config, int from) {
    for (const size_t i : Order(config, from)) if (IsMove(config.chain[i])) return true;
    return false;
}
// The modifier node `from` points at in a slot (-1: none, or `from` is not a node).
inline int ModOf(const Config& config, int from, int slot) {
    if (from < 0 || static_cast<size_t>(from) >= config.chain.size()) return -1;
    const int mod = config.chain[static_cast<size_t>(from)].mods[static_cast<size_t>(slot)];
    return mod >= 0 && static_cast<size_t>(mod) < config.chain.size() && SlotOf(config.chain[static_cast<size_t>(mod)].type) == slot ? mod : -1;
}
// 裁剪: the part of a move that plays (seconds of its montage). A BOSS 技能 pointing at a 裁剪 node (its own arrow only):
// from trim_from to trim_to; `to` 0 means to the end (also a trim_to not after trim_from).
struct TrimRange { bool on = false; double from = 0, to = 0; std::vector<int> holds; };
inline TrimRange TrimOf(const Node& trim) {
    TrimRange r; r.from = ClampTrim(trim.trim_from) / 1000.0;
    const int to = ClampTrim(trim.trim_to); r.to = to > ClampTrim(trim.trim_from) ? to / 1000.0 : 0;
    r.holds=TrimHolds(trim.trim_holds);r.on = r.from > 0 || r.to > 0 || !r.holds.empty(); return r;
}
inline TrimRange TrimFor(const Config& config, int index) {
    if (index < 0 || static_cast<size_t>(index) >= config.chain.size() || !TakesTrim(config.chain[static_cast<size_t>(index)])) return {};
    const int mod = ModOf(config, index, kModTrim);
    return mod >= 0 ? TrimOf(config.chain[static_cast<size_t>(mod)]) : TrimRange{};
}
// The part's span on a montage `length` s long (to the end when `to` is 0 or past it; 0 when nothing is left).
inline double TrimSpan(const TrimRange& r, double length) {
    const double end = r.to > 0 && (length <= 0 || r.to < length) ? r.to : length;
    return end > r.from ? end - r.from : 0;
}
// Where a 加速 curve is along a trimmed move: the part's own progress (0..1).
inline double TrimProgress(const TrimRange& r, double position, double length) {
    const double span = TrimSpan(r, length);
    return span > 0 ? std::clamp((position - r.from) / span, 0.0, 1.0) : (length > 0 ? std::clamp(position / length, 0.0, 1.0) : 0.0);
}
// The first BOSS 技能 pointing at a 裁剪 node (-1: none).
inline int TrimmedBy(const Config& config, int trim) {
    for (size_t i = 0; i < config.chain.size(); ++i) if (TakesTrim(config.chain[i]) && ModOf(config, static_cast<int>(i), kModTrim) == trim) return static_cast<int>(i);
    return -1;
}
// ---- 组合 ----
// A 组合 (Group) is one move of the chain made of the nodes in its frame (`group` = its index), played straight one
// after another without a press: each starts at the 派生点 of the one before (or its end, or the end of the intervals
// wired after it). Their order is the frame's own wires (a node's `next` to another node of the same frame), else
// left to right, then top to bottom; a 组合 in the frame plays its own nodes there. A press reaches the 组合 as one
// move, and after it the chain goes on from the 组合's own output. Modifiers: a node's own arrow first, then its
// frames' (innermost first), then the trigger's.
inline bool IsGroup(const Config& config, int index) {
    return index >= 0 && static_cast<size_t>(index) < config.chain.size() && config.chain[static_cast<size_t>(index)].type == NodeType::Group;
}
// May this node sit in a frame (moves, 组合 and intervals; never triggers, branches or modifiers)?
inline bool Groupable(const Node& node) {
    return !StartsChain(node) && !HasRows(node) && !IsModifier(node) && !IsSetting(node);
}
// Does node `index` sit in frame `group`, directly or through frames inside it?
inline bool InGroup(const Config& config, int index, int group) {
    const int count = static_cast<int>(config.chain.size());
    for (int at = index, depth = 0; at >= 0 && at < count && depth <= kGroupDepthMax + 1; ++depth) {
        at = config.chain[static_cast<size_t>(at)].group;
        if (at == group && group >= 0) return true;
    }
    return false;
}
// How many frames hold this node (0: none).
inline int GroupDepth(const Config& config, int index) {
    const int count = static_cast<int>(config.chain.size()); int depth = 0;
    for (int at = index; at >= 0 && at < count && depth <= kGroupDepthMax + 1;) { at = config.chain[static_cast<size_t>(at)].group; if (at >= 0) ++depth; }
    return depth;
}
// The deepest frame below (and including) a node: 1 for a 组合 with no 组合 inside, 0 for any other node.
inline int GroupHeight(const Config& config, int index, int guard = 0) {
    if (!IsGroup(config, index) || guard > kGroupDepthMax + 1) return 0;
    int most = 0;
    for (size_t i = 0; i < config.chain.size(); ++i) if (config.chain[i].group == index && static_cast<int>(i) != index) most = (std::max)(most, GroupHeight(config, static_cast<int>(i), guard + 1));
    return most + 1;
}
// May node `index` go into frame `group` (-1: out of every frame always may)? Not into itself or a frame inside it,
// and never deeper than kGroupDepthMax frames.
inline bool CanJoin(const Config& config, int index, int group) {
    const int count = static_cast<int>(config.chain.size());
    if (index < 0 || index >= count) return false;
    if (group < 0) return true;
    if (!IsGroup(config, group) || group == index || !Groupable(config.chain[static_cast<size_t>(index)])) return false;
    if (InGroup(config, group, index)) return false;  // the frame sits in the node
    return GroupDepth(config, group) + 1 + GroupHeight(config, index) <= kGroupDepthMax;
}
// The nodes straight in a frame, in play order: along the frame's own wires from each node no other node of the
// frame feeds, those starts and the nodes left over taken left to right, then top to bottom.
inline std::vector<int> Members(const Config& config, int group) {
    std::vector<int> members;
    if (!IsGroup(config, group)) return members;
    for (size_t i = 0; i < config.chain.size(); ++i) if (config.chain[i].group == group && static_cast<int>(i) != group) members.push_back(static_cast<int>(i));
    std::sort(members.begin(), members.end(), [&](int a, int b) {
        const Node& p = config.chain[static_cast<size_t>(a)]; const Node& q = config.chain[static_cast<size_t>(b)];
        return p.x != q.x ? p.x < q.x : p.y != q.y ? p.y < q.y : a < b;
    });
    const auto member = [&](int i) { return std::find(members.begin(), members.end(), i) != members.end(); };
    std::vector<int> order; std::vector<bool> seen(config.chain.size(), false);
    const auto walk = [&](int from) {
        for (int i = from; i >= 0 && member(i) && !seen[static_cast<size_t>(i)]; i = config.chain[static_cast<size_t>(i)].next) {
            seen[static_cast<size_t>(i)] = true; order.push_back(i);
        }
    };
    for (const int i : members) {
        bool fed = false;
        for (const int j : members) if (j != i && config.chain[static_cast<size_t>(j)].next == i) fed = true;
        if (!fed) walk(i);
    }
    for (const int i : members) walk(i);  // a frame's wires that only go round in a circle
    return order;
}
// What a 组合 plays, one move after another: its nodes in order, a 组合 among them by its own nodes, intervals
// left out (they hold the move before them, as on the chain).
inline std::vector<int> GroupSteps(const Config& config, int group, int guard = 0) {
    std::vector<int> steps;
    if (!IsGroup(config, group) || guard > kGroupDepthMax) return steps;
    for (const int i : Members(config, group)) {
        const Node& node = config.chain[static_cast<size_t>(i)];
        if (node.type == NodeType::Group) { const std::vector<int> inner = GroupSteps(config, i, guard + 1); steps.insert(steps.end(), inner.begin(), inner.end()); }
        else if (IsMove(node)) steps.push_back(i);
    }
    return steps;
}
// Canvas clipboard: a detached subgraph. The three unique settings/entry nodes
// stay in their original table. Selecting a group also selects its descendants.
inline bool ClipboardNode(const Node& node) {
    return !IsSetting(node)&&!(node.type==NodeType::Trigger&&node.trigger==static_cast<int>(Trigger::NpcKey));
}
inline std::vector<Node> CopyNodes(const Config& config,const std::vector<int>& selection) {
    std::vector<bool> picked(config.chain.size(),false);
    for(int index:selection)if(index>=0&&static_cast<size_t>(index)<picked.size())picked[static_cast<size_t>(index)]=true;
    for(size_t i=0;i<config.chain.size();++i){
        int parent=config.chain[i].group;
        for(int depth=0;parent>=0&&static_cast<size_t>(parent)<picked.size()&&depth<=kGroupDepthMax;++depth){
            if(picked[static_cast<size_t>(parent)]&&config.chain[static_cast<size_t>(parent)].type==NodeType::Group){picked[i]=true;break;}
            parent=config.chain[static_cast<size_t>(parent)].group;
        }
    }
    std::vector<int> map(config.chain.size(),-1);std::vector<Node> out;
    for(size_t i=0;i<picked.size();++i)if(picked[i]&&ClipboardNode(config.chain[i])){map[i]=static_cast<int>(out.size());out.push_back(config.chain[i]);}
    const auto link=[&](int target){return target>=0&&static_cast<size_t>(target)<map.size()?map[static_cast<size_t>(target)]:-1;};
    for(auto& node:out){node.next=link(node.next);node.group=link(node.group);for(int& mod:node.mods)mod=link(mod);for(auto& route:node.routes)route.next=link(route.next);}
    return out;
}
// Appends atomically; preserves relative geometry and returns the new indices.
inline std::vector<int> PasteNodes(Config& config,const std::vector<Node>& copied,int dx,int dy) {
    if(copied.empty()||copied.size()>kMaxNodes||config.chain.size()>kMaxNodes-copied.size()||
        std::any_of(copied.begin(),copied.end(),[](const Node& node){return !ClipboardNode(node);}))return {};
    int min_x=kCanvasLimit,max_x=-kCanvasLimit,min_y=kCanvasLimit,max_y=-kCanvasLimit;
    for(const auto& node:copied){min_x=(std::min)(min_x,node.x);max_x=(std::max)(max_x,node.x);min_y=(std::min)(min_y,node.y);max_y=(std::max)(max_y,node.y);}
    if(min_x<-kCanvasLimit||max_x>kCanvasLimit||min_y<-kCanvasLimit||max_y>kCanvasLimit)return {};
    dx=std::clamp(dx,-kCanvasLimit-min_x,kCanvasLimit-max_x);dy=std::clamp(dy,-kCanvasLimit-min_y,kCanvasLimit-max_y);
    const int base=static_cast<int>(config.chain.size());std::vector<Node> nodes=copied;std::vector<int> added;added.reserve(nodes.size());
    const auto link=[&](int target){return target>=0&&static_cast<size_t>(target)<nodes.size()?base+target:-1;};
    for(size_t i=0;i<nodes.size();++i){auto& node=nodes[i];node.x+=dx;node.y+=dy;node.next=link(node.next);node.group=link(node.group);
        for(int& mod:node.mods)mod=link(mod);for(auto& route:node.routes)route.next=link(route.next);added.push_back(base+static_cast<int>(i));}
    config.chain.reserve(config.chain.size()+nodes.size());config.chain.insert(config.chain.end(),nodes.begin(),nodes.end());config.placed=true;return added;
}
// The modifier for a move in a slot: its own arrow, else its frames' (innermost first), else the trigger's (-1: none).
inline int ModFor(const Config& config, int trigger, int move, int slot) {
    int mod = ModOf(config, move, slot);
    const int count = static_cast<int>(config.chain.size());
    for (int at = move, depth = 0; mod < 0 && at >= 0 && at < count && depth <= kGroupDepthMax; ++depth) {
        at = config.chain[static_cast<size_t>(at)].group;
        mod = ModOf(config, at, slot);
    }
    return mod >= 0 ? mod : ModOf(config, trigger, slot);
}
// The speed a move of `trigger`'s chain plays at, in percent: the 加速 node the move points at, else its frames',
// else the one the trigger points at, else 100 (a curve: its mean pace; the game thread follows the curve itself).
// A 钩子 plays at that times its own speed.
inline int SpeedFor(const Config& config, int trigger, int move) {
    const Node* own = move >= 0 && static_cast<size_t>(move) < config.chain.size() ? &config.chain[static_cast<size_t>(move)] : nullptr;
    if (own && own->type == NodeType::Grab) return kSpeedDefault;
    const int mod = ModFor(config, trigger, move, kModSpeed);
    int percent = kSpeedDefault;
    if (mod >= 0) {
        const Node& speed = config.chain[static_cast<size_t>(mod)];
        // 曲线调节: its mean pace (how long the move lasts under it).
        percent = speed.curve && speed.points.size() >= 2 ? ClampSpeed(static_cast<int>(std::lround(CurveMeanSpeed(speed.points)))) : ClampSpeed(speed.speed);
    }
    if (own && own->type == NodeType::Hook) percent = ClampSpeed(static_cast<int>(std::lround(percent * ClampHookSpeed(own->hook_speed) / 100.0)));
    return percent;
}
// A 钩子's own speed as a factor (1 for any other node).
inline double HookPace(const Config& config, int move) {
    const Node* own = move >= 0 && static_cast<size_t>(move) < config.chain.size() ? &config.chain[static_cast<size_t>(move)] : nullptr;
    return own && own->type == NodeType::Hook ? ClampHookSpeed(own->hook_speed) / 100.0 : 1.0;
}
// The 加速 node a move of `trigger`'s chain takes (as SpeedFor picks it), or null; a grab takes none.
inline const Node* SpeedNodeFor(const Config& config, int trigger, int move) {
    if (move >= 0 && static_cast<size_t>(move) < config.chain.size() && config.chain[static_cast<size_t>(move)].type == NodeType::Grab) return nullptr;
    const int mod = ModFor(config, trigger, move, kModSpeed);
    return mod < 0 ? nullptr : &config.chain[static_cast<size_t>(mod)];
}
// Does that 加速 follow a curve (2+ points, 曲线调节 on)?
inline bool CurveOn(const Node* speed) { return speed && speed->type == NodeType::Speed && speed->curve && speed->points.size() >= 2; }
// Is a move of `trigger`'s chain under 霸体 (the move, a frame it sits in or the trigger points at a 霸体 node)?
inline bool ArmorFor(const Config& config, int trigger, int move) {
    return ModFor(config, trigger, move, kModArmor) >= 0;
}
// Can the hero not be hit while a move of `trigger`'s chain plays (the move, a frame it sits in or the trigger points
// at a 无敌 node)?
inline bool InvincibleFor(const Config& config, int trigger, int move) {
    return ModFor(config, trigger, move, kModInvincible) >= 0;
}
// Can the enemies not deflect (block or parry) a move of `trigger`'s chain while it plays (the move, a frame it sits in
// or the trigger points at a 无视防御 node)? A grab is never deflected anyway.
inline bool IgnoreGuardFor(const Config& config, int trigger, int move) {
    if (move >= 0 && static_cast<size_t>(move) < config.chain.size() && config.chain[static_cast<size_t>(move)].type == NodeType::Grab) return false;
    return ModFor(config, trigger, move, kModIgnoreGuard) >= 0;
}
// 保持派生 at the chain's place `at` (a move it plays or waits after; a trigger, a 组合): its time in ms, from the move,
// a frame it sits in or the trigger (0: the regular reset window; native detours still preserve the cursor).
inline int KeepChainFor(const Config& config, int trigger, int at) {
    const int mod = ModFor(config, trigger, at, kModKeepChain);
    return mod < 0 ? 0 : ClampKeepChain(config.chain[static_cast<size_t>(mod)].ms);
}
// Is the look a BOSS 技能 of `trigger`'s chain takes kept after it (保留变身: the move, a frame it sits in or the
// trigger points at one)? Only a BOSS 技能 takes a look of its own.
inline bool KeepLookFor(const Config& config, int trigger, int move) {
    if (move < 0 || static_cast<size_t>(move) >= config.chain.size() || config.chain[static_cast<size_t>(move)].type != NodeType::BossSkill) return false;
    if (OwnLookFor(config, trigger, move)) return false;  // 不变身 takes no look to keep
    return ModFor(config, trigger, move, kModKeepLook) >= 0;
}
// May a move of `trigger`'s chain be cut at any time (the move, a frame it sits in or the trigger points at a
// 允许中断 node)? A grab too: the boss's body left on the spot plays the rest of it.
inline bool InterruptFor(const Config& config, int trigger, int move) {
    return ModFor(config, trigger, move, kModInterrupt) >= 0;
}
// The hit reaction a move of `trigger`'s chain forces on the enemies it hits (a HitKind: the 强制受击 node the move
// points at, else its frames', else the trigger's; -1 none). A grab plays its own.
inline int ForceHitFor(const Config& config, int trigger, int move) {
    if (move >= 0 && static_cast<size_t>(move) < config.chain.size() && config.chain[static_cast<size_t>(move)].type == NodeType::Grab) return -1;
    const int mod = ModFor(config, trigger, move, kModForceHit);
    return mod < 0 ? -1 : ClampHitKind(config.chain[static_cast<size_t>(mod)].hit);
}
// How high a move of `trigger`'s chain throws the enemies it hits under a 强制受击 击飞 (cm; 0: none, or not 击飞).
inline int ForceLaunchFor(const Config& config, int trigger, int move) {
    const int kind = ForceHitFor(config, trigger, move);
    if (kind != static_cast<int>(HitKind::Launch)) return 0;
    return ClampLaunch(config.chain[static_cast<size_t>(ModFor(config, trigger, move, kModForceHit))].launch);
}
// 滞空 for a move of `trigger`'s chain: the 滞空 node it points at (else its frames', else the trigger's); with none,
// the 空中释放 values when it is cast in the air (`airborne`) and 空中释放 is on. `fixed`: those are 空中释放's.
struct AirSet { bool on = false, fixed = false; int self_lift = 0, self_hang = 0, foe_lift = 0, foe_hang = 0; };
// The BOSS 特效 a move of `trigger`'s chain plays (its own, its frames', else the trigger's; -1: none or not picked).
inline int EffectFor(const Config& config, int trigger, int move) {
    if (move < 0 || static_cast<size_t>(move) >= config.chain.size()) return -1;
    const int mod = ModFor(config, trigger, move, kModEffect);
    if (mod < 0 || config.chain[static_cast<size_t>(mod)].type != NodeType::BossEffect || config.chain[static_cast<size_t>(mod)].ability.empty()) return -1;
    return mod;
}
inline AirSet AirFor(const Config& config, int trigger, int move, bool airborne) {
    AirSet set;
    if (move < 0 || static_cast<size_t>(move) >= config.chain.size() || !TakesAir(config.chain[static_cast<size_t>(move)])) return set;
    int from = ModFor(config, trigger, move, kModAir);
    if (from < 0) {
        from = AirDefaultNode(config);
        if (!airborne || from < 0 || !config.chain[static_cast<size_t>(from)].on) return set;
        set.fixed = true;
    }
    const Node& node = config.chain[static_cast<size_t>(from)];
    set.on = true; set.self_lift = ClampAirLift(node.air_lift); set.self_hang = ClampAirHang(node.air_hang);
    set.foe_lift = ClampAirLift(node.air_foe_lift); set.foe_hang = ClampAirHang(node.air_foe_hang);
    return set;
}
// The hero's skills 空中释放 acts on when the game plays them (not chain moves): 影之武装, 战技, 特殊攻击. The light and
// heavy attacks keep the game's own air attacks; dodges, jumps and 格挡 their own movement.
inline constexpr unsigned kOwnAirSkills = (1u << static_cast<int>(Trigger::Shachi)) | (1u << static_cast<int>(Trigger::CombatArt)) |
                                          (1u << static_cast<int>(Trigger::Special));
// Executions are played with their target: neither node acts on them.
inline constexpr unsigned kOwnAirNever = (1u << static_cast<int>(Trigger::Execution)) | (1u << static_cast<int>(Trigger::SimpleExecution));
// 滞空 / 空中释放 for one of the hero's own actions the game plays (ActionBits `bits`; the chain played no move for that
// press): the 滞空 node a hero trigger of that action points at (the trigger on, whether or not it has a chain), else,
// cast in the air (`airborne`) and one of kOwnAirSkills, the 空中释放 values when it is on. `trigger`: the trigger whose
// 滞空 it takes (-1: 空中释放 or none).
inline AirSet OwnAirFor(const Config& config, unsigned bits, bool airborne, int* trigger = nullptr) {
    AirSet set;
    if (trigger) *trigger = -1;
    if (!config.enabled || !bits || (bits & kOwnAirNever)) return set;
    const auto take = [&](const Node& node) {
        set.on = true; set.self_lift = ClampAirLift(node.air_lift); set.self_hang = ClampAirHang(node.air_hang);
        set.foe_lift = ClampAirLift(node.air_foe_lift); set.foe_hang = ClampAirHang(node.air_foe_hang);
    };
    for (size_t i = 0; i < config.chain.size(); ++i) {
        const Node& node = config.chain[i];
        if (node.type != NodeType::Trigger || !node.on || !Fires(static_cast<Trigger>(node.trigger), bits)) continue;
        const int mod = node.mods[kModAir];
        if (mod < 0 || static_cast<size_t>(mod) >= config.chain.size() || config.chain[static_cast<size_t>(mod)].type != NodeType::Air) continue;
        take(config.chain[static_cast<size_t>(mod)]);
        if (trigger) *trigger = static_cast<int>(i);
        return set;
    }
    const int from = AirDefaultNode(config);
    if (!airborne || !(bits & kOwnAirSkills) || from < 0 || !config.chain[static_cast<size_t>(from)].on) return set;
    take(config.chain[static_cast<size_t>(from)]); set.fixed = true;
    return set;
}
// The share of its damage a move of `trigger`'s chain deals, in percent: the 伤害倍率 node the move points at, else
// its frames', else the one the trigger points at, else 100. A grab measures its own damage (投技页).
inline int DamageFor(const Config& config, int trigger, int move) {
    if (move >= 0 && static_cast<size_t>(move) < config.chain.size() && config.chain[static_cast<size_t>(move)].type == NodeType::Grab) return kDamageDefault;
    const int mod = ModFor(config, trigger, move, kModDamage);
    return mod < 0 ? kDamageDefault : ClampDamage(config.chain[static_cast<size_t>(mod)].speed);
}
// ---- 瞬移 ----
// The distances tried for a landing, from the one asked for (never under `least`, the two capsules apart) down to
// `least` in `step` cm steps: a wall or a drop at one distance tries a shorter one.
inline std::vector<int> TeleportDistances(int want, int least, int step = 25) {
    std::vector<int> out;
    least = (std::max)(least, 1); want = (std::max)(want, least); step = (std::max)(step, 1);
    for (int d = want; d > least; d -= step) out.push_back(d);
    out.push_back(least);
    return out;
}
// The landing point `distance` cm in front of a target at (tx, ty) facing (fx, fy), and the yaw (degrees) that faces
// the target from there. A target with no facing (fx = fy = 0) is approached from the hero's side (hx, hy).
struct Landing { double x = 0, y = 0, yaw = 0; };
// `turn` (degrees) swings the point around the target from straight in front (the side tried when the front is taken).
inline Landing LandingAt(double tx, double ty, double fx, double fy, double hx, double hy, double distance, double turn = 0) {
    double len = std::hypot(fx, fy);
    if (!(len > 1e-6)) { fx = hx - tx; fy = hy - ty; len = std::hypot(fx, fy); }
    if (!(len > 1e-6)) { fx = 1; fy = 0; len = 1; }
    fx /= len; fy /= len;
    if (turn != 0) {
        const double r = turn * 3.14159265358979323846 / 180.0, c = std::cos(r), s = std::sin(r);
        const double rx = fx * c - fy * s, ry = fx * s + fy * c; fx = rx; fy = ry;
    }
    Landing out; out.x = tx + fx * distance; out.y = ty + fy * distance;
    out.yaw = std::atan2(ty - out.y, tx - out.x) * 180.0 / 3.14159265358979323846;
    return out;
}
// Does this trigger point at a 武器属性 node?
inline bool WeaponOn(const Config& config, int trigger) { return ModOf(config, trigger, kModWeapon) >= 0; }
// The nodes in a row after the NPC skill key, each wired to the next (tests, text saved before the canvas).
inline Config Linked(std::vector<Node> nodes) {
    Config config; config.chain = std::move(nodes);
    for (size_t i = 0; i < config.chain.size(); ++i) config.chain[i].next = i + 1 < config.chain.size() ? static_cast<int>(i + 1) : -1;
    Node key = TriggerNode(Trigger::NpcKey, 24, 24); key.next = config.chain.empty() ? -1 : 0;
    config.chain.push_back(key);
    return config;
}
// The montage play rate for a move: `speed` percent of its rate now, and slowed further so it ends with its interval
// when it would end sooner (never below 0.05). length_s: the montage's own length; rate: its play rate now;
// rate_scale: its RateScale. A move that lasts at least the interval keeps the speed (the next press cuts it).
inline double MoveRate(int speed, int interval_ms, double length_s, double rate, double rate_scale) {
    if (!(rate > 0)) return rate;
    const double base = rate * ClampSpeed(speed) / 100.0;
    if (interval_ms <= 0 || !(length_s > 0) || !(rate_scale > 0)) return base;
    const double lasts = length_s / (base * rate_scale), wanted = interval_ms / 1000.0;
    if (wanted <= lasts * 1.02) return base;
    return (std::max)(0.05, base * lasts / wanted);
}
// Seconds a montage lasts at a rate (what the page shows on a node).
inline double Lasts(double length_s, double rate, double rate_scale) {
    return length_s > 0 && rate > 0 && rate_scale > 0 ? length_s / (rate * rate_scale) : 0;
}

// ini text, node by node in index order: "E1|T:kind,reset,on,x,y,next|S:GA_X,x,y,next[,派生点]|I:500,x,y,next|A:x,y,next[,派生点]|
// K:x,y,button,pause,next,button,pause,next...|V:percent,x,y|W:x,y|H:x,y|C:x,y|G:x,y,next[,move]|X:look,GA,x,y,next[,cut]" (H = 霸体, C = 允许中断, G = 投技; move: the grab move's id, none for the 投技 page's pick;
// X = BOSS 技能: an NPC skin id and its move; Y = 武器派生: "Y:weapon,GA,x,y,next[,cut]"; D = 伤害倍率: "D:percent,x,y"; a 处决
// (or 简易处决) trigger names its weapon after next: "T:13,reset,on,x,y,next,weapon"; O = 武器派生修改:
// "O:x,y,weapon,reset,action,next,action,next..."; U = 组合: "U:x,y,w,h,next,cut,n;n;n" with the
// indices of the nodes in its frame; P = 瞬移: "P:distance,lift,x,y,next[,cut]"), then each arrow to a
// modifier as "L:from,modifier". Older text still reads: modifiers that pointed at their node ("V:percent,x,y,node",
// "W:x,y,node", "H:x,y,node" become that node's arrow), the first canvas ("B:start,x,y" = the key node,
// "R:hero:ability:on[:x:y]" replace nodes, which become a dodge trigger wired to the move) and the list before it
// ("E1|S:GA_X|I:500|R:5::1").
inline bool AbilityName(const std::string& name) {
    if (name.empty() || name.size() > 120) return false;
    for (char c : name) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    return true;
}
// A grab move's id (MotianGrab::Variant::id), e.g. "MoTian:GA_MoTian_GEx_01:BD_..:BP_BulletEvent_Grab_C_0" or "02".
inline bool GrabMoveId(const std::string& id) {
    if (id.empty() || id.size() > 160) return false;
    for (char c : id) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == ':' || c == '.')) return false;
    return true;
}
inline std::vector<std::string> Split(const std::string& text, char by) {
    std::vector<std::string> out; size_t start = 0;
    while (true) {
        const size_t end = text.find(by, start);
        out.push_back(text.substr(start, end == std::string::npos ? std::string::npos : end - start));
        if (end == std::string::npos) return out;
        start = end + 1;
    }
}
// A branch row's button in the text: "<button>", a 自定义组合键 / 自定义按键 row "<button>/<keys>/<0 in order | 1
// together>/<window ms>" (as such a trigger's), 自定义按键 also "/<1: swallow>". An older reader drops such a row.
inline std::string RouteButtonToken(const Route& route) {
    std::string token = std::to_string(route.button);
    if (!KeysRoute(route)) return token;
    const bool raw = route.button == static_cast<int>(Trigger::Keys);
    std::string keys;
    for (size_t k = 0; k < route.keys.size() && k < kComboKeysMax; ++k)
        if (raw ? ValidRawKey(route.keys[k]) : ValidComboKey(route.keys[k])) keys += (keys.empty() ? "" : ";") + std::to_string(route.keys[k]);
    token += "/" + (keys.empty() ? std::string("_") : keys) + "/" + (route.chord ? "1" : "0") + "/" + std::to_string(ClampKeyWindow(route.window));
    if (raw) token += route.swallow ? "/1" : "/0";
    return token;
}
inline Route RouteFromToken(const std::string& token) {
    Route route; route.button = std::atoi(token.c_str());
    if (!KeysRoute(route)) return route;
    const bool raw = route.button == static_cast<int>(Trigger::Keys);
    const std::vector<std::string> c = Split(token, '/');
    if (c.size() != 4 && !(raw && c.size() == 5)) return route;
    if (c[1] != "_") for (const std::string& k : Split(c[1], ';')) {
        const int key = std::atoi(k.c_str());
        if ((raw ? ValidRawKey(key) : ValidComboKey(key)) && route.keys.size() < kComboKeysMax) route.keys.push_back(key);
    }
    route.chord = c[2] == "1"; route.window = ClampKeyWindow(std::atoi(c[3].c_str()));
    route.swallow = raw && c.size() == 5 && c[4] == "1";
    return route;
}
inline std::wstring Encode(const Config& config) {
    std::string text = config.enabled ? "E1" : "E0";
    const auto pos = [](const Node& n) { return std::to_string(ClampPosition(n.x)) + "," + std::to_string(ClampPosition(n.y)); };
    const auto at = [&](const Node& n) { return pos(n) + "," + std::to_string(n.next); };
    for (const Node& node : config.chain) {
        switch (node.type) {
        case NodeType::Trigger:
            text += "|T:" + std::to_string(node.trigger) + "," + std::to_string(ClampReset(node.ms)) + "," + (node.on ? "1" : "0") + "," + at(node) +
                    (WeaponTrigger(node.trigger) && AbilityName(node.weapon) ? "," + node.weapon : std::string());
            if (OwnKeysTrigger(node.trigger)) {
                // ",<key;key;...>/<0 in order | 1 together>/<window ms>", 自定义按键 also "/<1: swallow the game's action>"; then
                // "/<继续派生>" when it has one
                const bool raw = node.trigger == static_cast<int>(Trigger::Keys);
                std::string keys;
                for (size_t k = 0; k < node.keys.size() && k < kComboKeysMax; ++k)
                    if (raw ? ValidRawKey(node.keys[k]) : ValidComboKey(node.keys[k])) keys += (keys.empty() ? "" : ";") + std::to_string(node.keys[k]);
                text += "," + (keys.empty() ? std::string("_") : keys) + "/" + (node.chord ? "1" : "0") + "/" + std::to_string(ClampKeyWindow(node.window));
                if (raw) text += node.swallow ? "/1" : "/0";
                if (HasThen(node)) text += "/" + std::to_string(node.then);  // 继续派生
            }
            break;
        case NodeType::Interval: text += "|I:" + std::to_string(ClampInterval(node.ms)) + "," + at(node); break;
        case NodeType::AutoDodge: text += "|A:" + at(node) + (ClampCancel(node.ms) ? "," + std::to_string(ClampCancel(node.ms)) : ""); break;
        case NodeType::Speed:
            // "V:percent[~t:s;t:s...[!]],x,y": the curve's points after "~" ("!" at their end: kept, but off).
            text += "|V:" + std::to_string(ClampSpeed(node.speed)) + (node.points.size() >= 2 ? CurveText(node.points) + (node.curve ? "" : "!") : std::string()) + "," + pos(node);
            break;
        case NodeType::Damage: text += "|D:" + std::to_string(ClampDamage(node.speed)) + "," + pos(node); break;
        case NodeType::WeaponMove:
            text += "|Y:" + (AbilityName(node.weapon) ? node.weapon : std::string("_")) + "," + (AbilityName(node.ability) ? node.ability : std::string("_")) + "," + at(node) +
                    (ClampCancel(node.ms) ? "," + std::to_string(ClampCancel(node.ms)) : "");
            break;
        case NodeType::Hook:  // a 武器派生 of its hook move; the "g:" mark below makes it a 钩子 again
            text += std::string("|Y:") + kHookWeapon + "," + (node.hook_self ? kHookGo : kHookPull) + "," + at(node) +
                    (ClampCancel(node.ms) ? "," + std::to_string(ClampCancel(node.ms)) : "");
            break;
        case NodeType::Weapon: text += "|W:" + pos(node); break;
        case NodeType::Trim: text += "|W:" + pos(node); break;  // the "c:" mark below makes it a 裁剪 again
        case NodeType::Armor: text += "|H:" + pos(node); break;
        case NodeType::Interrupt: text += "|C:" + pos(node); break;
        case NodeType::Invincible: text += "|N:" + pos(node); break;
        case NodeType::IgnoreGuard: text += "|Z:" + pos(node); break;
        case NodeType::KeepLook: text += "|E:" + pos(node); break;
        case NodeType::ForceHit: text += "|Q:" + std::to_string(ClampHitKind(node.hit)) + "," + pos(node); break;
        case NodeType::KeepChain: text += "|k:" + std::to_string(ClampKeepChain(node.ms)) + "," + pos(node); break;
        case NodeType::PreInput: text += "|J:" + std::to_string(ClampPreInput(node.ms)) + "," + pos(node); break;
        // An unwired 间隔 in an older build; the "f:" / "j:" marks below make them 滞空 / 空中释放 again.
        case NodeType::BoneTransform: case NodeType::Air: case NodeType::AirDefault: case NodeType::BossEffect:
            text += "|I:" + std::to_string(kIntervalDefault) + "," + pos(node) + ",-1"; break;
        case NodeType::Grab: text += "|G:" + at(node) + (GrabMoveId(node.ability) ? "," + node.ability : std::string()); break;
        case NodeType::Assist:
            text += "|a:" + (AbilityName(node.boss) ? node.boss : std::string("_")) + "," + (AbilityName(node.ability) ? node.ability : std::string("_")) + "," + at(node);
            break;
        case NodeType::BossSkill:
            text += "|X:" + (AbilityName(node.boss) ? node.boss : std::string("_")) + "," + (AbilityName(node.ability) ? node.ability : std::string("_")) + "," + at(node) +
                    (ClampCancel(node.ms) ? "," + std::to_string(ClampCancel(node.ms)) : "");
            break;
        case NodeType::Group: {
            std::string members;
            for (size_t i = 0; i < config.chain.size(); ++i)
                if (config.chain[i].group == static_cast<int>(&node - config.chain.data()) && &config.chain[i] != &node) members += (members.empty() ? "" : ";") + std::to_string(i);
            text += "|U:" + pos(node) + "," + std::to_string(ClampGroupW(node.w)) + "," + std::to_string(ClampGroupH(node.h)) + "," + std::to_string(node.next) + "," +
                    std::to_string(ClampCancel(node.ms)) + "," + members;
            break;
        }
        case NodeType::Look: text += "|M:" + (AbilityName(node.boss) ? node.boss : std::string("_")) + "," + at(node); break;
        case NodeType::HeroLook: text += "|F:" + at(node); break;
        case NodeType::Teleport:
            text += "|P:" + std::to_string(ClampTeleportDist(node.dist)) + "," + std::to_string(ClampTeleportLift(node.lift)) + "," + at(node) +
                    (ClampCancel(node.ms) ? "," + std::to_string(ClampCancel(node.ms)) : "");
            break;
        case NodeType::WeaponActions:
            text += "|O:" + pos(node) + "," + (AbilityName(node.weapon) ? node.weapon : std::string("_")) + "," + std::to_string(ClampReset(node.ms));
            for (const Route& route : node.routes)
                if (node.air_set ? ValidAirAction(route.button) : ValidWeaponAction(route.button)) text += "," + RouteButtonToken(route) + "," + std::to_string(route.next);
            break;
        case NodeType::Branch:
            text += "|K:" + std::to_string(ClampPosition(node.x)) + "," + std::to_string(ClampPosition(node.y));
            for (const Route& route : node.routes) text += "," + RouteButtonToken(route) + "," + std::to_string(ClampPause(route.pause_ms)) + "," + std::to_string(route.next);
            break;
        // A bad name keeps its slot (as an empty skill) so every wire still points at the same node.
        default: text += "|S:" + (AbilityName(node.ability) ? node.ability : std::string("_")) + "," + at(node) + (ClampCancel(node.ms) ? "," + std::to_string(ClampCancel(node.ms)) : ""); break;
        }
    }
    for (size_t i = 0; i < config.chain.size(); ++i)
        for (int s = 0; s < kModSlots; ++s) {
            const int mod = ModOf(config, static_cast<int>(i), s);
            if (mod >= 0) text += "|L:" + std::to_string(i) + "," + std::to_string(mod);
        }
    // b:node~config. Older readers keep an inert, unwired interval and ignore its modifier link.
    for(size_t i=0;i<config.chain.size();++i)if(config.chain[i].type==NodeType::BoneTransform)
        text+="|b:"+std::to_string(i)+"~"+BoneMotion::Encode(config.chain[i].bones);
    // 不变身 marks: "h:node", on a start node 整体不变身 (an older build ignores them; the node keeps its look change there).
    for (size_t i = 0; i < config.chain.size(); ++i)
        if (SavesOwnLook(config.chain[i]) && config.chain[i].own_look) text += "|h:" + std::to_string(i);
    // 钩子 marks: "g:node" (an older build plays the 武器派生 it is written as).
    for (size_t i = 0; i < config.chain.size(); ++i) if (config.chain[i].type == NodeType::Hook) text += "|g:" + std::to_string(i);
    // 空中派生 marks: "s:node" (an older build reads a 武器派生修改 with no weapon, which takes nothing).
    for (size_t i = 0; i < config.chain.size(); ++i) if (IsAirSet(config.chain[i])) text += "|s:" + std::to_string(i);
    // 钩子 自动 marks: "q:node" (an older build plays it as 勾过来).
    for (size_t i = 0; i < config.chain.size(); ++i) if (config.chain[i].type == NodeType::Hook && config.chain[i].hook_auto) text += "|q:" + std::to_string(i);
    // 钩子 reach marks: "r:node,cm,percent" when either is not its default (an older build throws it as before).
    for (size_t i = 0; i < config.chain.size(); ++i) {
        const Node& node = config.chain[i];
        if (node.type == NodeType::Hook && (ClampHookDist(node.hook_dist) != kHookDistDefault || ClampHookSpeed(node.hook_speed) != kHookSpeedDefault))
            text += "|r:" + std::to_string(i) + "," + std::to_string(ClampHookDist(node.hook_dist)) + "," + std::to_string(ClampHookSpeed(node.hook_speed));
    }
    // 滞空 marks: "f:node,lift,hang,foe lift,foe hang"; 空中释放: "j:node,on,lift,hang,foe lift,foe hang"; 击飞's
    // height: "u:node,cm" when not kLaunchDefault.
    const auto air = [](const Node& n) {
        return std::to_string(ClampAirLift(n.air_lift)) + "," + std::to_string(ClampAirHang(n.air_hang)) + "," + std::to_string(ClampAirLift(n.air_foe_lift)) + "," +
               std::to_string(ClampAirHang(n.air_foe_hang));
    };
    for (size_t i = 0; i < config.chain.size(); ++i) {
        const Node& node = config.chain[i];
        if (node.type == NodeType::Air) text += "|f:" + std::to_string(i) + "," + air(node);
        // BOSS 特效: "x:node,folder,name,place,damage,radius,hits" (an older build keeps an unwired 间隔).
        if (node.type == NodeType::BossEffect)
            text += "|x:" + std::to_string(i) + "," + (AbilityName(node.boss) ? node.boss : std::string("_")) + "," + (AbilityName(node.ability) ? node.ability : std::string("_")) + "," +
                    std::to_string(ClampEffectPlace(node.fx_place)) + "," + std::to_string(ClampEffectDamage(node.fx_damage)) + "," + std::to_string(ClampEffectRadius(node.fx_radius)) + "," +
                    std::to_string(ClampEffectHits(node.fx_hits));
        if (node.type == NodeType::AirDefault) text += "|j:" + std::to_string(i) + "," + (node.on ? "1," : "0,") + air(node);
        if (node.type == NodeType::ForceHit && ClampLaunch(node.launch) != kLaunchDefault) text += "|u:" + std::to_string(i) + "," + std::to_string(ClampLaunch(node.launch));
    }
    // 裁剪 marks: "c:node,from,to" (ms); an older build reads a 武器属性 there, and a BOSS 技能's arrow to it does not fit one.
    for (size_t i = 0; i < config.chain.size(); ++i)
        if (config.chain[i].type == NodeType::Trim){
            text += "|c:" + std::to_string(i) + "," + std::to_string(ClampTrim(config.chain[i].trim_from)) + "," + std::to_string(ClampTrim(config.chain[i].trim_to));
            const auto points=TrimHolds(config.chain[i].trim_holds);if(!points.empty()){text+="|z:"+std::to_string(i);for(int t:points)text+=","+std::to_string(t);}
        }
    // 按住 marks: "w:node,row,ms" (row -1: a trigger's own); an older build takes any press.
    for (size_t i = 0; i < config.chain.size(); ++i) {
        const Node& node = config.chain[i];
        if (node.type == NodeType::Trigger && ClampHold(node.hold_ms) > 0) text += "|w:" + std::to_string(i) + ",-1," + std::to_string(ClampHold(node.hold_ms));
        if (HasRows(node))
            for (size_t r = 0; r < node.routes.size(); ++r)
                if (ClampHold(node.routes[r].hold_ms) > 0) text += "|w:" + std::to_string(i) + "," + std::to_string(r) + "," + std::to_string(ClampHold(node.routes[r].hold_ms));
    }
    return std::wstring(text.begin(), text.end());
}
inline Config Decode(const std::wstring& wide) {
    Config config; std::string text;
    for (wchar_t c : wide) text.push_back(c < 128 ? static_cast<char>(c) : '?');
    struct OldReplace { int hero = 5; std::string ability; bool on = true; bool placed = false; int x = 0, y = 0; };
    std::vector<OldReplace> olds;
    std::vector<std::pair<int,BoneMotion::Config>> bone_marks;
    bool triggers = false, old_key = false, placed = true; int start = -1, start_x = 24, start_y = 24;
    std::vector<std::pair<int, int>> arrows;  // node -> modifier
    std::vector<int> own_looks;  // 不变身 BOSS 技能 / 投技 nodes
    std::vector<int> hooks, auto_hooks;  // 钩子 nodes (written as 武器派生), and those that are 自动
    std::vector<int> air_sets;  // 空中派生 nodes (written as 武器派生修改)
    struct ReachMark { int node = -1, dist = kHookDistDefault, speed = kHookSpeedDefault; };
    std::vector<ReachMark> reaches;  // 钩子 distance and speed
    struct HoldMark { int node = -1, row = -1, ms = 0; };
    std::vector<HoldMark> holds;  // 按住
    struct TrimMark { int node = -1, from = 0, to = 0; };
    std::vector<TrimMark> trims;  // 裁剪 (written as 武器属性)
    std::vector<std::pair<int,std::vector<int>>> charge_marks;
    struct AirMark { int node = -1; bool fixed = false, on = true; int lift = 0, hang = 0, foe_lift = 0, foe_hang = 0; };
    std::vector<AirMark> airs;  // 滞空 / 空中释放 (written as 间隔)
    std::vector<std::pair<int, int>> launches;  // 强制受击 击飞 heights
    std::vector<Node> effects;  // BOSS 特效 (written as 间隔): x the node, its folder, name and numbers
    std::vector<std::pair<int, std::string>> frames;  // 组合 -> its nodes ("3;4;7")
    const auto place = [](Node& node, const std::vector<std::string>& f, size_t first) {
        node.x = ClampPosition(std::atoi(f[first].c_str())); node.y = ClampPosition(std::atoi(f[first + 1].c_str())); node.next = std::atoi(f[first + 2].c_str());
    };
    for (const std::string& part : Split(text, '|')) {
        if (part == "E0") config.enabled = false;
        else if (part == "E1") config.enabled = true;
        else if (part.size() < 2 || part[1] != ':') continue;
        else if (part[0] == 'L') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() == 2) arrows.push_back({std::atoi(f[0].c_str()), std::atoi(f[1].c_str())});
        } else if(part[0]=='b') {
            const size_t sep=part.find('~',2);
            if(sep!=std::string::npos&&sep>2&&std::isdigit(static_cast<unsigned char>(part[2]))&&bone_marks.size()<kMaxNodes)
                bone_marks.emplace_back(std::atoi(part.substr(2,sep-2).c_str()),BoneMotion::Decode(part.substr(sep+1)));
        } else if (part[0] == 'h') {
            const std::string f = part.substr(2);
            if (!f.empty() && std::isdigit(static_cast<unsigned char>(f[0]))) own_looks.push_back(std::atoi(f.c_str()));
        } else if (part[0] == 'g') {
            const std::string f = part.substr(2);
            if (!f.empty() && std::isdigit(static_cast<unsigned char>(f[0]))) hooks.push_back(std::atoi(f.c_str()));
        } else if (part[0] == 'q') {
            const std::string f = part.substr(2);
            if (!f.empty() && std::isdigit(static_cast<unsigned char>(f[0]))) auto_hooks.push_back(std::atoi(f.c_str()));
        } else if (part[0] == 's') {
            const std::string f = part.substr(2);
            if (!f.empty() && std::isdigit(static_cast<unsigned char>(f[0]))) air_sets.push_back(std::atoi(f.c_str()));
        } else if (part[0] == 'r') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() == 3) reaches.push_back({std::atoi(f[0].c_str()), ClampHookDist(std::atoi(f[1].c_str())), ClampHookSpeed(std::atoi(f[2].c_str()))});
        } else if (part[0] == 'w') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() == 3) holds.push_back({std::atoi(f[0].c_str()), std::atoi(f[1].c_str()), ClampHold(std::atoi(f[2].c_str()))});
        } else if (part[0] == 'f' || part[0] == 'j') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            const size_t first = part[0] == 'j' ? 2 : 1;
            if (f.size() != first + 4) continue;
            AirMark mark; mark.node = std::atoi(f[0].c_str()); mark.fixed = part[0] == 'j'; mark.on = !mark.fixed || f[1] != "0";
            mark.lift = ClampAirLift(std::atoi(f[first].c_str())); mark.hang = ClampAirHang(std::atoi(f[first + 1].c_str()));
            mark.foe_lift = ClampAirLift(std::atoi(f[first + 2].c_str())); mark.foe_hang = ClampAirHang(std::atoi(f[first + 3].c_str()));
            airs.push_back(mark);
        } else if (part[0] == 'x') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 7 || f[0].empty() || f[0].find_first_not_of("0123456789") != std::string::npos || f[0].size() > 6 || effects.size() >= kMaxNodes) continue;
            Node fx; fx.type = NodeType::BossEffect; fx.x = std::atoi(f[0].c_str());
            fx.boss = AbilityName(f[1]) ? f[1] : std::string(); fx.ability = AbilityName(f[2]) ? f[2] : std::string();
            fx.fx_place = ClampEffectPlace(std::atoi(f[3].c_str())); fx.fx_damage = ClampEffectDamage(std::atoi(f[4].c_str()));
            fx.fx_radius = ClampEffectRadius(std::atoi(f[5].c_str())); fx.fx_hits = ClampEffectHits(std::atoi(f[6].c_str()));
            effects.push_back(fx);
        } else if (part[0] == 'u') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() == 2) launches.push_back({std::atoi(f[0].c_str()), ClampLaunch(std::atoi(f[1].c_str()))});
        } else if (part[0] == 'z') {
            const auto f=Split(part.substr(2),',');if(f.size()<2||f.size()>33||f[0].empty()||f[0].size()>5||f[0].find_first_not_of("0123456789")!=std::string::npos)continue;
            std::vector<int> points;bool valid=true;for(size_t j=1;j<f.size();++j){if(f[j].empty()||f[j].find_first_not_of("0123456789")!=std::string::npos||f[j].size()>5){valid=false;break;}points.push_back(ClampTrim(std::atoi(f[j].c_str())));}
            if(valid)charge_marks.push_back({std::atoi(f[0].c_str()),TrimHolds(points)});
        } else if (part[0] == 'c') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() == 3) trims.push_back({std::atoi(f[0].c_str()), ClampTrim(std::atoi(f[1].c_str())), ClampTrim(std::atoi(f[2].c_str()))});
        } else if (part[0] == 'B') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 3) continue;
            old_key = true; start = std::atoi(f[0].c_str());
            start_x = ClampPosition(std::atoi(f[1].c_str())); start_y = ClampPosition(std::atoi(f[2].c_str()));
        } else if (part[0] == 'J') {
            // 预输入: "ms,x,y", once per chain; it takes its place as it comes, so every index stays.
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 3 || PreInputNode(config) >= 0) continue;
            Node node; node.type = NodeType::PreInput; node.ms = ClampPreInput(std::atoi(f[0].c_str()));
            node.x = ClampPosition(std::atoi(f[1].c_str())); node.y = ClampPosition(std::atoi(f[2].c_str()));
            config.chain.push_back(node);
        } else if (config.chain.size() >= kMaxNodes) continue;
        else if (part[0] == 'T') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            const int kind = f.size() == 6 || f.size() == 7 ? std::atoi(f[0].c_str()) : -1;
            if (!ValidButton(kind)) continue;
            Node node = TriggerNode(static_cast<Trigger>(kind), 0, 0); node.ms = ClampReset(std::atoi(f[1].c_str())); node.on = f[2] != "0";
            place(node, f, 3);
            if (f.size() == 7 && WeaponTrigger(kind) && AbilityName(f[6])) node.weapon = f[6];
            if (f.size() == 7 && OwnKeysTrigger(kind)) {
                const bool raw = kind == static_cast<int>(Trigger::Keys);
                const std::vector<std::string> c = Split(f[6], '/');
                const size_t base = raw ? 4 : 3;  // then "/<继续派生>"
                if (c.size() == base || c.size() == base + 1) {
                    if (c[0] != "_") for (const std::string& k : Split(c[0], ';')) {
                        const int key = std::atoi(k.c_str());
                        if ((raw ? ValidRawKey(key) : ValidComboKey(key)) && node.keys.size() < kComboKeysMax) node.keys.push_back(key);
                    }
                    node.chord = c[1] == "1"; node.window = ClampKeyWindow(std::atoi(c[2].c_str()));
                    node.swallow = raw && c[3] == "1";
                    if (c.size() == base + 1) { const int then = std::atoi(c[base].c_str()); if (then != kThenOwn && ValidThen(kind, then)) node.then = then; }
                }
            }
            config.chain.push_back(node); triggers = true;
        } else if (part[0] == 'X') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 5 && f.size() != 6) continue;
            Node node; node.type = NodeType::BossSkill;
            if (AbilityName(f[0]) && f[0] != "_") node.boss = f[0];
            if (AbilityName(f[1]) && f[1] != "_") node.ability = f[1];
            place(node, f, 2);
            if (f.size() == 6) node.ms = ClampCancel(std::atoi(f[5].c_str()));
            config.chain.push_back(node);
        } else if (part[0] == 'a') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 5) continue;
            Node node; node.type = NodeType::Assist;
            if (AbilityName(f[0]) && f[0] != "_") node.boss = f[0];
            if (AbilityName(f[1]) && f[1] != "_") node.ability = f[1];
            place(node, f, 2);
            config.chain.push_back(node);
        } else if (part[0] == 'Y') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 5 && f.size() != 6) continue;
            Node node; node.type = NodeType::WeaponMove;
            if (AbilityName(f[0]) && f[0] != "_") node.weapon = f[0];
            if (AbilityName(f[1]) && f[1] != "_") node.ability = f[1];
            place(node, f, 2);
            if (f.size() == 6) node.ms = ClampCancel(std::atoi(f[5].c_str()));
            config.chain.push_back(node);
        } else if (part[0] == 'U') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 7) continue;
            Node node; node.type = NodeType::Group;
            node.x = ClampPosition(std::atoi(f[0].c_str())); node.y = ClampPosition(std::atoi(f[1].c_str()));
            node.w = ClampGroupW(std::atoi(f[2].c_str())); node.h = ClampGroupH(std::atoi(f[3].c_str()));
            node.next = std::atoi(f[4].c_str()); node.ms = ClampCancel(std::atoi(f[5].c_str()));
            frames.push_back({static_cast<int>(config.chain.size()), f[6]});
            config.chain.push_back(node);
        } else if (part[0] == 'P') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 5 && f.size() != 6) continue;
            Node node; node.type = NodeType::Teleport;
            node.dist = ClampTeleportDist(std::atoi(f[0].c_str())); node.lift = ClampTeleportLift(std::atoi(f[1].c_str()));
            place(node, f, 2);
            if (f.size() == 6) node.ms = ClampCancel(std::atoi(f[5].c_str()));
            config.chain.push_back(node);
        } else if (part[0] == 'Q') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 3) continue;
            Node node = ModifierNode(NodeType::ForceHit, ClampPosition(std::atoi(f[1].c_str())), ClampPosition(std::atoi(f[2].c_str())));
            node.hit = ClampHitKind(std::atoi(f[0].c_str()));
            config.chain.push_back(node);
        } else if (part[0] == 'D') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 3) continue;
            Node node = ModifierNode(NodeType::Damage, ClampPosition(std::atoi(f[1].c_str())), ClampPosition(std::atoi(f[2].c_str())));
            node.speed = ClampDamage(std::atoi(f[0].c_str()));
            config.chain.push_back(node);
        } else if (part[0] == 'M') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 4) continue;
            Node node; node.type = NodeType::Look;
            if (AbilityName(f[0]) && f[0] != "_") node.boss = f[0];
            place(node, f, 1);
            config.chain.push_back(node);
        } else if (part[0] == 'F') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 3) continue;
            Node node; node.type = NodeType::HeroLook; place(node, f, 0);
            config.chain.push_back(node);
        } else if (part[0] == 'G') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 3 && f.size() != 4) continue;
            Node node; node.type = NodeType::Grab; place(node, f, 0);
            if (f.size() == 4 && GrabMoveId(f[3])) node.ability = f[3];
            config.chain.push_back(node);
        } else if (part[0] == 'A') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 3 && f.size() != 4) continue;
            Node node; node.type = NodeType::AutoDodge; place(node, f, 0);
            if (f.size() == 4) node.ms = ClampCancel(std::atoi(f[3].c_str()));
            config.chain.push_back(node);
        } else if (part[0] == 'V' || part[0] == 'W' || part[0] == 'H' || part[0] == 'C' || part[0] == 'N' || part[0] == 'E' || part[0] == 'Z') {
            // "x,y" (after a speed's percent); an older one adds the node it pointed at, now that node's arrow.
            const std::vector<std::string> f = Split(part.substr(2), ',');
            const size_t first = part[0] == 'V' ? 1 : 0;
            if (f.size() != first + 2 && f.size() != first + 3) continue;
            Node node = ModifierNode(part[0] == 'V' ? NodeType::Speed : part[0] == 'W' ? NodeType::Weapon : part[0] == 'H' ? NodeType::Armor :
                                     part[0] == 'N' ? NodeType::Invincible : part[0] == 'E' ? NodeType::KeepLook : part[0] == 'Z' ? NodeType::IgnoreGuard :
                                     NodeType::Interrupt, 0, 0);
            if (first) {
                node.speed = ClampSpeed(std::atoi(f[0].c_str()));
                const size_t tilde = f[0].find('~');
                if (tilde != std::string::npos) {
                    std::string curve = f[0].substr(tilde + 1);
                    const bool off = !curve.empty() && curve.back() == '!';
                    if (off) curve.pop_back();
                    node.points = ParseCurve(curve); node.curve = !off && node.points.size() >= 2;
                }
            }
            node.x = ClampPosition(std::atoi(f[first].c_str())); node.y = ClampPosition(std::atoi(f[first + 1].c_str()));
            if (f.size() == first + 3) arrows.push_back({std::atoi(f[first + 2].c_str()), static_cast<int>(config.chain.size())});
            config.chain.push_back(node);
        } else if (part[0] == 'k') {
            // 保持派生: "ms,x,y".
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() != 3) continue;
            Node node = ModifierNode(NodeType::KeepChain, 0, 0); node.ms = ClampKeepChain(std::atoi(f[0].c_str()));
            node.x = ClampPosition(std::atoi(f[1].c_str())); node.y = ClampPosition(std::atoi(f[2].c_str()));
            config.chain.push_back(node);
        } else if (part[0] == 'O') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() < 4 || (f.size() - 4) % 2) continue;
            Node node; node.type = NodeType::WeaponActions;
            node.x = ClampPosition(std::atoi(f[0].c_str())); node.y = ClampPosition(std::atoi(f[1].c_str()));
            if (AbilityName(f[2]) && f[2] != "_") node.weapon = f[2];
            node.ms = ClampReset(std::atoi(f[3].c_str()));
            for (size_t r = 4; r + 1 < f.size() && node.routes.size() < kMaxRoutes; r += 2) {
                Route route = RouteFromToken(f[r]); route.next = std::atoi(f[r + 1].c_str());
                if (ValidWeaponAction(route.button) || ValidAirAction(route.button)) node.routes.push_back(route);  // which: once the "s:" marks are read
            }
            config.chain.push_back(node);
        } else if (part[0] == 'K') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            if (f.size() < 2 || (f.size() - 2) % 3) continue;
            Node node; node.type = NodeType::Branch;
            node.x = ClampPosition(std::atoi(f[0].c_str())); node.y = ClampPosition(std::atoi(f[1].c_str()));
            for (size_t r = 2; r + 2 < f.size() && node.routes.size() < kMaxRoutes; r += 3) {
                Route route = RouteFromToken(f[r]); route.pause_ms = ClampPause(std::atoi(f[r + 1].c_str())); route.next = std::atoi(f[r + 2].c_str());
                if (ValidRouteButton(route.button)) node.routes.push_back(route);
            }
            config.chain.push_back(node);
        } else if (part[0] == 'S' || part[0] == 'I') {
            const std::vector<std::string> f = Split(part.substr(2), ',');
            Node node; node.type = part[0] == 'I' ? NodeType::Interval : NodeType::Skill;
            if (node.type == NodeType::Interval) node.ms = ClampInterval(std::atoi(f[0].c_str()));
            else if (AbilityName(f[0]) && f[0] != "_") node.ability = f[0];
            else if (f.size() < 4) continue;  // a bad name in the old list: dropped as before
            if (f.size() == 4 || (f.size() == 5 && node.type == NodeType::Skill)) place(node, f, 1); else placed = false;
            if (f.size() == 5 && node.type == NodeType::Skill) node.ms = ClampCancel(std::atoi(f[4].c_str()));
            config.chain.push_back(node);
        } else if (part[0] == 'R') {
            const std::vector<std::string> f = Split(part.substr(2), ':');
            if (f.size() != 3 && f.size() != 5) continue;
            OldReplace r; r.hero = std::atoi(f[0].c_str());
            if (r.hero < 0 || r.hero > 5) continue;
            r.ability = AbilityName(f[1]) ? f[1] : std::string(); r.on = f[2] != "0";
            if (f.size() == 5) { r.placed = true; r.x = ClampPosition(std::atoi(f[3].c_str())); r.y = ClampPosition(std::atoi(f[4].c_str())); } else placed = false;
            olds.push_back(r);
        }
    }
    const int count = static_cast<int>(config.chain.size());
    for (Node& node : config.chain) {
        if (IsModifier(node) || IsSetting(node)) node.next = -1;
        for (size_t o = 0; o < OutCount(node); ++o) { const int to = OutOf(node, o); if (to < -1 || to >= count) SetOut(node, o, -1); }
    }
    if (!triggers) {
        // Before trigger nodes: the key node came from "B:" (first canvas) or the list order (before the canvas).
        if (!old_key) {
            for (int i = 0; i < count; ++i) config.chain[static_cast<size_t>(i)].next = i + 1 < count ? i + 1 : -1;
            start = count ? 0 : -1; placed = false;
        }
        Node key = TriggerNode(Trigger::NpcKey, start_x, start_y); key.next = start >= 0 && start < count ? start : -1;
        config.chain.push_back(key);
        // Old replace nodes (hero dodge 0..5 = Dash .. Any): a dodge trigger wired to the move, or to the stick's dodge.
        for (const OldReplace& r : olds) {
            if (config.chain.size() + 2 > kMaxNodes) break;
            Node trigger = TriggerNode(static_cast<Trigger>(static_cast<int>(Trigger::Dash) + r.hero), r.x, r.y); trigger.on = r.on;
            Node move; move.type = r.ability.empty() ? NodeType::AutoDodge : NodeType::Skill; move.ability = r.ability; move.x = r.x + 300; move.y = r.y;
            trigger.next = static_cast<int>(config.chain.size()) + 1;
            config.chain.push_back(trigger); config.chain.push_back(move);
        }
    }
    for(auto& mark:bone_marks)if(mark.first>=0&&mark.first<count){
        Node& n=config.chain[static_cast<size_t>(mark.first)];
        if(n.type==NodeType::Interval&&n.next<0){n.type=NodeType::BoneTransform;n.ms=0;n.bones=std::move(mark.second);}
    }
    // 滞空 / 空中释放 first too (unwired 间隔 in the text; 空中释放 once).
    for (const AirMark& a : airs) {
        if (a.node < 0 || a.node >= count) continue;
        Node& node = config.chain[static_cast<size_t>(a.node)];
        if (node.type != NodeType::Interval || node.next >= 0 || (a.fixed && AirDefaultNode(config) >= 0)) continue;
        node.type = a.fixed ? NodeType::AirDefault : NodeType::Air; node.ms = 0; node.on = a.on;
        node.air_lift = a.lift; node.air_hang = a.hang; node.air_foe_lift = a.foe_lift; node.air_foe_hang = a.foe_hang;
    }
    for (const Node& fx : effects) {  // x holds the node's index
        if (fx.x < 0 || fx.x >= count) continue;
        Node& node = config.chain[static_cast<size_t>(fx.x)];
        if (node.type != NodeType::Interval || node.next >= 0) continue;
        node.type = NodeType::BossEffect; node.ms = 0; node.boss = fx.boss; node.ability = fx.ability;
        node.fx_place = fx.fx_place; node.fx_damage = fx.fx_damage; node.fx_radius = fx.fx_radius; node.fx_hits = fx.fx_hits;
    }
    for (const auto& [i, cm] : launches)
        if (i >= 0 && i < count && config.chain[static_cast<size_t>(i)].type == NodeType::ForceHit) config.chain[static_cast<size_t>(i)].launch = cm;
    // 空中派生: its rows the air's (the NPC key too, no execution); a 武器派生修改's the weapon's.
    for (const int i : air_sets)
        if (i >= 0 && i < count && config.chain[static_cast<size_t>(i)].type == NodeType::WeaponActions) {
            config.chain[static_cast<size_t>(i)].air_set = true; config.chain[static_cast<size_t>(i)].weapon.clear(); }
    for (Node& node : config.chain)
        if (node.type == NodeType::WeaponActions)
            node.routes.erase(std::remove_if(node.routes.begin(), node.routes.end(), [&](const Route& r) {
                return node.air_set ? !ValidAirAction(r.button) : !ValidWeaponAction(r.button); }), node.routes.end());
    // 裁剪 nodes first (their arrows fit a 裁剪, not the 武器属性 they are written as).
    for (const TrimMark& t : trims) {
        if (t.node < 0 || t.node >= count) continue;
        Node& node = config.chain[static_cast<size_t>(t.node)];
        if (node.type == NodeType::Weapon) { node.type = NodeType::Trim; node.trim_from = t.from; node.trim_to = t.to; }
    }
    for(const auto& mark:charge_marks)if(mark.first>=0&&mark.first<count&&config.chain[static_cast<size_t>(mark.first)].type==NodeType::Trim)config.chain[static_cast<size_t>(mark.first)].trim_holds=mark.second;
    // Arrows only where they fit (one per slot: the last one read wins); no exec wire into a trigger, a modifier or
    // itself.
    for (const auto& arrow : arrows) if (arrow.first < count && arrow.second < count) LinkModifier(config, arrow.first, arrow.second);
    for (size_t i = 0; i < config.chain.size(); ++i) {
        Node& node = config.chain[i];
        if (IsModifier(node)) continue;
        for (size_t o = 0; o < OutCount(node); ++o) {
            const int to = OutOf(node, o);
            if (to >= 0 && (to == static_cast<int>(i) || StartsChain(config.chain[static_cast<size_t>(to)]) || IsModifier(config.chain[static_cast<size_t>(to)]) ||
                            IsSetting(config.chain[static_cast<size_t>(to)])))
                SetOut(node, o, -1);
        }
    }
    // Frames: each node in at most one (the first that names it), only where it may go.
    for (const auto& [frame, list] : frames) {
        if (list.empty()) continue;
        for (const std::string& item : Split(list, ';')) {
            const int i = std::atoi(item.c_str());
            if (item.empty() || i < 0 || i >= count || config.chain[static_cast<size_t>(i)].group >= 0) continue;
            if (CanJoin(config, i, frame)) config.chain[static_cast<size_t>(i)].group = frame;
        }
    }
    for (const int i : own_looks) if (i >= 0 && i < count && SavesOwnLook(config.chain[static_cast<size_t>(i)])) config.chain[static_cast<size_t>(i)].own_look = true;
    for (const int i : hooks) {
        if (i < 0 || i >= count) continue;
        Node& node = config.chain[static_cast<size_t>(i)];
        if (node.type == NodeType::WeaponMove && HookAbility(node.weapon, node.ability)) { node.type = NodeType::Hook; SetHookMode(node, node.ability == kHookGo); }
    }
    for (const int i : auto_hooks)
        if (i >= 0 && i < count && config.chain[static_cast<size_t>(i)].type == NodeType::Hook) SetHookAuto(config.chain[static_cast<size_t>(i)]);
    for (const ReachMark& r : reaches)
        if (r.node >= 0 && r.node < count && config.chain[static_cast<size_t>(r.node)].type == NodeType::Hook) {
            config.chain[static_cast<size_t>(r.node)].hook_dist = r.dist; config.chain[static_cast<size_t>(r.node)].hook_speed = r.speed; }
    for (const HoldMark& h : holds) {
        if (h.node < 0 || h.node >= count) continue;
        Node& node = config.chain[static_cast<size_t>(h.node)];
        if (h.row < 0 && node.type == NodeType::Trigger) node.hold_ms = h.ms;
        else if (h.row >= 0 && HasRows(node) && static_cast<size_t>(h.row) < node.routes.size()) node.routes[static_cast<size_t>(h.row)].hold_ms = h.ms;
    }
    EnsureKey(config);
    config.placed = placed;
    return config;
}
// The stick direction relative to the hero's facing as F / B / L / R (B when it is centred).
inline char Direction(double forward_dot, double right_dot) {
    if (std::hypot(forward_dot, right_dot) < 0.2) return 'B';
    if (std::fabs(forward_dot) >= std::fabs(right_dot)) return forward_dot >= 0 ? 'F' : 'B';
    return right_dot >= 0 ? 'R' : 'L';
}
}
