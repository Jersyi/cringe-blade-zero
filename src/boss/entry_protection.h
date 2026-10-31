#pragma once
#include "ue_runtime.h"

namespace EntryProtection {
using Logger = void (*)(const char*);
struct Snapshot {
    bool actor_protected = false, tag_added = false, pause_owned = false;
    bool partial = false;
    int32_t original_tag_count = 0;
};
void Initialize(Logger logger);
// Game thread only. False can retain partial protection for a safe retry;
// consult HasProtection and always ReleaseProtection on cancel/finish.
bool Protect(ue::Object* pawn);
bool ReleaseProtection();
bool Pause(ue::Object* world, ue::Object* game_instance, bool requested);
bool ReleasePause(ue::Object* game_instance);
// Snapshot is safe on the worker/UI thread; it performs no engine reads.
Snapshot GetSnapshot();
bool HasProtection();
}
