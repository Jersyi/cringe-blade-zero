#pragma once
#include "npc_combo_model.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// 招式派生: the hero's and each NPC look's move chain, and what the game-thread runner (npc_retarget) is playing.
// Any thread, no engine calls. The chains live in .combo files (combo_file.h) under combos\ next to version.dll; one
// of them is the active set (PBZDebugMenu.ini [NpcCombo] File). The first run moves the chains PBZDebugMenu.ini kept
// into default.combo. A change to the active file on disk is read again within about a second; a bad file is skipped
// (the loaded set stays) and logged.
namespace NpcCombo {
// The hero's own chain (no NPC look on): its key in the set.
constexpr char kHeroProfile[] = "Hero";
Config Load(const std::string& profile);
void Save(const std::string& profile, const Config& config);  // into the active file at once
// Every chain of the active set (BOSS 技能 preloading reads them all).
std::vector<std::pair<std::string, Config>> All();
// Bumped by every Save and every (re)load of the active set: the game thread and the page re-read on a change.
uint64_t Generation();
using Logger = void (*)(const char*);
void SetLogger(Logger logger);
void SetFolder(const std::wstring& folder);  // tests; default <version.dll folder>\combos
std::wstring Folder();
// The .combo files in the folder (names without the extension), the active one, and the last problem (empty: none).
struct Files { std::vector<std::wstring> names; std::wstring active, status; uint64_t serial = 0; };
Files GetFiles();
bool SelectFile(const std::wstring& name);  // read it and make it the active set; false: missing or bad (status says)
bool SaveFile();                            // write the active set again
bool SaveAs(const std::wstring& name);      // the active set into name.combo, which becomes the active file
// A path a Save As dialog picked (.combo added when missing): in the folder it is SaveAs(its name) (Folder, name set);
// anywhere else the active set is copied there and the active file stays (Copy: only the folder's files are listed).
enum class SavedTo { Failed, Folder, Copy };
SavedTo SaveToPath(const std::wstring& path, std::wstring& name);
bool Poll();                                // the folder and the active file read again when changed (true: reloaded)
void StartWatcher();                        // a background thread: Poll on every folder change and once a second

// The runner's state for the page: which chain node is playing (-1: none) and its last notice.
struct Run { std::string profile; bool running = false; int node = -1; std::wstring note; uint64_t serial = 0; };
void Publish(const std::string& profile, bool running, int node, const std::wstring& note);
Run GetRun();
// How long a move's montage lasted at its own rate when it last played (seconds; 0 when it has not played yet).
void RecordLength(const std::string& ability, float seconds);
float Length(const std::string& ability);
// A boss move's first montage, its length in its own time (seconds; 0 until the move played in a chain or in the 招式预览):
// the page's 裁剪 timeline. Any thread.
void RecordClip(const std::string& ability, float seconds);
float Clip(const std::string& ability);
// The hero's main weapon as a 武器属性 node last read it (game thread): its WeaponConfig name and strength
// multiplier (ok false: not readable, the hits keep x1). serial 0: never read.
struct WeaponInfo { std::wstring name; float scale = 1; bool ok = false; uint64_t serial = 0; };
void RecordWeapon(const std::wstring& name, float scale, bool ok);
WeaponInfo LastWeapon();
// 派生提示: the notice in the bottom-right of the game client a chain's move shows as it plays (PBZDebugMenu.ini
// [NpcCombo] ShowToast, default on; the NPC skill key's own casts keep theirs). Any thread.
bool ShowToast();
void SetShowToast(bool on);
// 派生显示: where the chain going on is, as the game thread runs it, for the overlay in the game's top-right corner
// (ComboMenu::HudTick). profile: whose chains run (also with no chain going on); trigger: the node the chain started
// at (-1: no chain); cursor: the node it is at (the move
// that played last, a 组合, or the start); playing: the move playing (-1: none; a 组合's step); queued: the move a kept
// press plays next (-1: none); button: the button the chain is on; wait_until: while it waits for the next press, when
// it gives up (GetTickCount64; 0: not waiting); at: when it was published (the game thread does so on every change and
// four times a second; an old one means the runner stopped).
// branch_at: when the pause of the branch after the cursor counts from (0: not yet). hold_down: a press waiting while
// its button is held (按住) went down then (0: none), hold_press is it. The overlay picks the row a press takes with them.
struct ChainState { std::string profile; int trigger = -1, cursor = -1, playing = -1, queued = -1, button = 0; unsigned long long wait_until = 0, at = 0;
                    unsigned long long branch_at = 0, hold_down = 0; Press hold_press; };
void PublishChain(const ChainState& state);
ChainState GetChain();
// 派生显示 mode (PBZDebugMenu.ini [NpcCombo] ChainHud): kHudOff (default), kHudDetailed (详细: the whole chain as a
// thumbnail, where it is lit) or kHudSimple (简略: the keys that take it on next and their moves). Any thread.
enum : int { kHudOff = 0, kHudDetailed = 1, kHudSimple = 2 };
int ChainHud();
void SetChainHud(int mode);
// 镜头跟随 (PBZDebugMenu.ini [NpcCombo] ViewTarget): the bone the 视角跟随 camera looks at, kViewPelvis (default),
// kViewNeck or kViewHead. Any thread; cached (the game thread reads it every frame).
enum : int { kViewPelvis = 0, kViewNeck = 1, kViewHead = 2 };
int ViewTarget();
void SetViewTarget(int target);
// 派生页的技能伤害 / 投技伤害 (PBZDebugMenu.ini [NpcCombo] ChainSkillDamage, ChainGrabDamage, ChainGrabValue): the
// damage of the BOSS 技能 and 投技 a chain plays, in place of the NPC 技能 page's and the 投技 page's (moves the chain
// does not play keep those). Skill: 0 follows the NPC 技能 page, else x kChainSkillMin..kChainSkillMax of the boss's own
// damage. Grab: mode kGrabFollow follows the 投技 page, else GrabExtras::Damage (0 原生, 1 倍率 value x, 2 at least
// value % of max health per hit). Any thread; cached.
constexpr float kChainSkillMin = 0.1f, kChainSkillMax = 10.0f;
float ChainSkillDamage();
void SetChainSkillDamage(float value);
enum : int { kGrabFollow = -1 };
struct ChainGrab { int mode = kGrabFollow; float value = 0; };
ChainGrab ChainGrabDamage();
void SetChainGrabDamage(const ChainGrab& grab);
}
