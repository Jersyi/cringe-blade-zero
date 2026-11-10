#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "npc_combo_model.h"

namespace ue { using Object = unsigned char; }  // as in ue_runtime.h

// BOSS 援护 (and the move preview built on it): a copy of a boss, an actor of its own next to the hero, casts one of
// that boss's skills. The game's own SpawnAI makes it (as the boss rush and the test dummy do); its AI is held
// stopped, so it acts only when told; its skill runs on its own ability system with the boss's weapons, effects and
// bullets, aimed at the hero's lock target (or the nearest enemy); the hero's input and moves are never touched.
// It appears in front of the target with the game's blink (瞬身, no travel), casts, and leaves with the blink again.
// Up to kSlots bosses are in at once; a call while all are busy takes the place of the oldest.
//
// The chain's 援护 node (NpcCombo::NodeType::Assist) calls Request with the node's modifiers. 加速 is the boss's own
// time dilation (here); 伤害倍率, 武器属性, 强制受击, 无视防御, 霸体 and 无敌 act through NpcRetarget's damage and tag code
// on what Casting reports. `npcretarget assist <skin> <skill> [near]` calls one by hand (BOSS_ASSIST log lines).
namespace BossAssist {
using Logger = void (*)(const char*);
constexpr size_t kSlots = 3;
// The modifiers of one call. node: the chain node that called it (-1: by hand); interrupt (允许中断): the node's next call
// takes this one's place at once (else the call before plays on beside it while a slot is free).
struct Mods {
    int node = -1; int speed = 100; float damage = 1.0f; int force = -1; bool armor = false, invincible = false, ignore_guard = false, interrupt = false;
};
void Initialize(Logger logger);
// Any thread. monster / ability: package paths (/PBZCombatAsset/.../BP_Monster_X, .../GA_X); beside: spawn in view at
// the hero's side and cast where it stands (no jump), for the probe and the preview.
void Request(const std::string& monster, const std::string& ability, const Mods& mods, bool beside = false);
void Stop();
// GT only: the bosses in now (spawned and not gone), their modifiers and whether their skill is casting.
struct Cast { ue::Object* boss = nullptr; Mods mods; bool casting = false; uint64_t since = 0; };
size_t Casting(Cast* out, size_t max);
// GT only, the move preview (MovePreview): one boss of its own (not one of the kSlots, no blink, no jump) at `at`
// facing `yaw` (degrees), casting `ability` at `target` again and again, put back on its spot between casts, until
// StopPreview. An `ability` that is a montage ("..._Montage.<name>": a grab's executor side) is played on the boss
// instead of cast (a grab needs a target that can be grabbed). false: not spawned (logged). PreviewBoss: that boss
// while it is in.
bool StartPreview(const std::string& monster, const std::string& ability, const double (&at)[3], double yaw, ue::Object* target);
void StopPreview();
ue::Object* PreviewBoss();
// GT only: the preview boss's 加速, held on its CustomTimeDilation every frame: percent, or with 2+ curve points the
// curve at its montage's position (each cast follows the curve from its start).
void PreviewSpeed(int percent, const std::vector<NpcCombo::CurvePoint>& curve);
// GT only: the preview move's 裁剪 (ms of its montage; 0 / 0: all of it): each cast starts at `from` and ends at `to`,
// and a 加速 curve runs over that part. The montage's length goes to NpcCombo::RecordClip (the page's timeline).
void PreviewTrim(int from_ms, int to_ms, const std::vector<int>& holds = {}, bool held = false);
// Tests ("npcretarget preview arm 0|1"): the preview boss's Main weapon slot armed at its spawn (default on).
void PreviewOption(const std::string& what, int value);
void OnGameThreadGuarded();
}
