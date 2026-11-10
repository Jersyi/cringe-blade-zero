#pragma once
#include <cstdint>
#include <string>

// 联机测试's puppet: a plain Character (/Script/Engine.Character: no game logic, no ability system, no input) beside
// the player, with no controller, standing in for the other player. Never a second hero of the hero's own class: that
// one's blueprint took the player's input from its BeginPlay (the hero could not attack and stayed in its guard, 11-08).
// Spawned deferred with AutoPossessPlayer off; if the player's controller or camera still ends up on it, they are put
// back on the hero (logged). A fault removes it (NET_PUPPET fault ... stage= names the step; a part whose making
// faulted is left out from then on).
// It shows the hero pose by pose, so every move, action (guard, jump, dodge, hit reactions) and grab matches:
// - The hero's side lists its parts (every mesh it shows: body, an NPC look's display and parts, weapons and other
//   actors hanging on it, Grooms, Chaos Cloth, and the actors a grab shows with it: the boss body left on the spot
//   and the target it holds) with assets, materials and where each hangs, and sends, 30 times a second, its pose: the
//   hero's root, each part's place and whether it shows, and the local bone transforms of the skinned parts that pose
//   themselves (the others take a leader's pose).
// - The puppet makes the same parts (a PoseableMeshComponent for each self-posing one, its bones written every frame)
//   and shows each pose blended from the one before, so it moves smoothly.
// - Following the peer (NetSteam, protocol 8): the peer's parts and pose, at the peer's place.
// - The loop (no peer, "mirror" on; the one-PC test): the local hero's own parts and pose go through the same encoding
//   straight to its puppet, which stands where it was spawned relative to the hero.
// - Mirror off without a peer: it stands still, empty.
// - It spawns by itself once a peer connects (and again after a map change while connected).
// NET_PUPPET log lines: spawn facts, controller / ASC / ability counts at 0, 1 and 5 s, parts lists, posed parts.
namespace NetPuppet {
using Logger = void (*)(const char*);
void Initialize(Logger logger);
void OnGameThreadGuarded(bool transitioning);

// Check (tests, the loop only): for 10 s the newest pose is shown unblended and every bone of the puppet is compared
// with the hero's (NET_PUPPET check lines: the largest and mean distance in cm, part places, shown states).
enum class Request { Spawn, Remove, MirrorOn, MirrorOff, Check, Inspect, DuelOn, DuelOff };
void Ask(Request request);  // any thread
void SetHairMode(int mode, bool save=true); // 0 Groom (default), 1 cards; receiver only

struct Snapshot {
    bool alive = false, mirror = true;
    int source = 0;              // 0 stands still, 1 shows the local hero (the loop), 2 follows the peer
    std::string cls, controller; // the puppet's class and controller (class name; "-" none)
    int parts = 0, parts_shown = 0, part_failures = 0;
    int hair_mode=0, hair_grooms=0, hair_cards=0;
    uint32_t poses = 0, assets_missing = 0, parts_lists = 0, frames_sent = 0;
    int send_parts = 0, send_bones = 0; double send_kbps = 0;  // what the hero sends: parts, bones per pose, KB/s
    int fx_world=0,fx_owned=0,fx_active=0,fx_shown=0; uint32_t fx_starts=0;
    bool duel=false; int duel_health[2]{1000,1000}; uint32_t duel_hits[2]{},duel_attacks=0;
    int projectiles=0,projectile_parts=0,parts_dropped=0; uint32_t projectile_starts=0; // local sender diagnostic, visual components only
    int camera_fixes = 0, possess_fixes = 0;
    std::wstring note[2];        // [0] Chinese, [1] English
};
Snapshot Get();
}
