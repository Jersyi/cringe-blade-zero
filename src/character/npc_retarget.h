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
using ReleaseAppearance = bool (*)();
struct Entry { int32_t index; std::string id; std::wstring name, detail; };
struct Snapshot {
    std::vector<Entry> entries;
    std::wstring status = L"NPC 重定向：进入场景后可用";
    std::wstring selected;
    bool ready = false, active = false, busy = false;
    bool cleanup_pending = false, disabled = false, appearance_gate = false, experimental = false;
    int32_t parts = 0, sockets = 0, retargeted_bones = 0;
};
enum class Action { Apply, Restore, Inspect };
void Initialize(Logger logger, ReleaseAppearance release_appearance = nullptr);
bool Request(Action action, int32_t entry = -1);
// Any thread, no engine calls. Off queues a normal guarded Restore.
bool SetExperimental(bool enabled);
Snapshot GetSnapshot();
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
