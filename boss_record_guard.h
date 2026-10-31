#pragma once
#include "ue_runtime.h"

// Game-thread only. The caller holds the practice save gates throughout Restore.
namespace BossRecordGuard {
using Logger = void (*)(const char*);
void Initialize(Logger logger);
bool Capture(ue::Object* save);
bool Restore(ue::Object* save);
void Clear();
}
