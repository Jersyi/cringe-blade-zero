#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "ue_runtime.h"

namespace NpcRetarget {
using Logger = void (*)(const char*);
// Game-thread adapter: cancel queued/reapply outfit intent and synchronously
// release its owned temporary session. True must mean verified release, rather
// than an inactive UI snapshot. The adapter must not apply another appearance.
// chain: a 招式派生 look change (the game's dead flag alone does not hold it); why: the gate that holds it, or on
// success a note (nullptr: none).
using ReleaseAppearance = bool (*)(bool chain, const char** why);
struct Entry { int32_t index; std::string id; std::wstring name, detail; };
struct Snapshot {
    std::vector<Entry> entries;
    std::wstring status = L"NPC 重定向：进入场景后可用";
    std::wstring selected;
    bool ready = false, active = false, busy = false;
    bool cleanup_pending = false, disabled = false, appearance_gate = false, experimental = false;
    int32_t parts = 0, sockets = 0, retargeted_bones = 0;
    int32_t skin = -1; // the applied skin's row (kProfiles), -1 without one
    int32_t chain = -1; // whose 招式派生 chain runs: the skin, or while a BOSS 技能 look is borrowed the look before (-1 the hero)
};
enum class Action { Apply, Restore, Inspect };
void Initialize(Logger logger, ReleaseAppearance release_appearance = nullptr);
bool Request(Action action, int32_t entry = -1);
// Any thread, no engine calls. While an NPC skin is on, the game thread casts the skin's boss skills
// (kNpcSkillSets) in random rounds on the hero's own ASC, locked on the nearest enemy; while one runs
// the boss's weapons hang on the NPC display and the hero's weapons are hidden. False when no skin is active.
bool RequestSkill();
// Any thread, no engine calls. NPC skill damage ([NpcSkill] DamageMultiplier, kSkillDamageMin..Max,
// default 3): every health drop of a hostile pawn while a skin's boss skill runs, and for the effects it
// leaves up to 2 s after, is scaled to this many times the native damage. 1 = the boss's own damage.
constexpr float kSkillDamageDefault = 3.0f, kSkillDamageMin = 1.0f, kSkillDamageMax = 10.0f;
float SkillDamageMultiplier();
bool SetSkillDamageMultiplier(float multiplier); // clamped, persisted; false when not finite
// Game thread. The NPC display component while a skin is on, else null: a grab's boss weapon follows
// its hands. size: the skin's height over the hero's (rest pelvis), 1 without a skin; the grab
// scales the boss weapon by it, the display and the victim keep their own size.
unsigned char* GrabDisplay(double& size);
// Any thread, no engine calls. Off queues a normal guarded Restore.
bool SetExperimental(bool enabled);
Snapshot GetSnapshot();
// Any thread, no engine calls: a skin's moves for the move-chain page (招式派生), as GA names: its AI skills and its
// own dodges (directions F / B / L / R, 0 when the name does not say), each once; and a move's notice label.
// skin -1: the hero's own chain (profile NpcCombo::kHeroProfile): its abilities named GA_*, as last read in game.
struct SkinMoves { std::string profile; std::wstring name; std::vector<std::string> skills, dodges; std::vector<char> directions; };
bool GetSkinMoves(int32_t skin, SkinMoves& moves);
std::wstring MoveLabel(const std::string& ability);
// Any thread, no engine calls: the skin row (kProfiles) that plays a grab move's boss (MotianGrab::Variant boss_id and
// source): of that boss's skins (GrabBoss::Key), the one whose blueprint or moves share the longest folder with the
// grab ability (a P2 grab gets the P2 look), then the one named like the boss; -1 when no skin plays it.
int32_t SkinForGrab(const std::wstring& boss_id, const std::wstring& source);
void OnGameThreadGuarded();
// Game thread only. False means cleanup must be retried before travel/outfit changes.
bool BeforeTravel();
// Game thread only. Cancels NPC intent, waits for idle before restoring a live
// primary, and returns false while any original/owned resource needs cleanup.
// Call this gate before applying a Hun outfit. It never requests an outfit.
bool ReleaseForExternal();
// Game thread only, no engine calls. Includes unfinished rollback resources.
// Native outfit/Groom writers must yield while this returns true.
bool OwnsAppearance();
// GT only. New source-driver sessions do not edit shared Skeleton/socket
// assets, so unrelated NPC spawns retain the player display. Legacy unfinished
// shared-edit ledgers still require verified release before an intersection.
bool BeforeActorClassSpawn(ue::Object* actor_class);
bool HandleCommand(const char* verb, const char* arguments);
}
