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
bool Poll();                                // the folder and the active file read again when changed (true: reloaded)
void StartWatcher();                        // a background thread: Poll on every folder change and once a second

// The runner's state for the page: which chain node is playing (-1: none) and its last notice.
struct Run { std::string profile; bool running = false; int node = -1; std::wstring note; uint64_t serial = 0; };
void Publish(const std::string& profile, bool running, int node, const std::wstring& note);
Run GetRun();
// How long a move's montage lasted at its own rate when it last played (seconds; 0 when it has not played yet).
void RecordLength(const std::string& ability, float seconds);
float Length(const std::string& ability);
// The hero's main weapon as a 武器属性 node last read it (game thread): its WeaponConfig name and strength
// multiplier (ok false: not readable, the hits keep x1). serial 0: never read.
struct WeaponInfo { std::wstring name; float scale = 1; bool ok = false; uint64_t serial = 0; };
void RecordWeapon(const std::wstring& name, float scale, bool ok);
WeaponInfo LastWeapon();
// 派生提示: the notice in the bottom-right of the game client a chain's move shows as it plays (PBZDebugMenu.ini
// [NpcCombo] ShowToast, default on; the NPC skill key's own casts keep theirs). Any thread.
bool ShowToast();
void SetShowToast(bool on);
}
