#pragma once
#include "ue_runtime.h"
#include <cstdint>
#include <string>
#include <vector>

namespace BossAI {
using Logger = void (*)(const char*);
enum class Kind { Probability, Weight, BehaviorCooldown, RemainingCooldown };
struct Parameter {
    int32_t index = -1;
    Kind kind = Kind::Probability;
    std::wstring key, label, unit, route;
    float current = 0, baseline = 0, minimum = 0, maximum = 1;
    bool changed = false;
};
struct Snapshot {
    uint64_t generation = 0;
    bool ready = false;
    std::wstring target, status = L"等待原生 Boss AI 实例";
    std::vector<Parameter> parameters;
};
void Initialize(Logger logger);
// Game thread: bind the actual combat pawn after the spawner produced it.
// Restores the previous live target first. Never binds CDOs/archetypes.
bool BindTarget(ue::Object* boss);
// Game thread, before retry/travel/Stop. Returns false if a live modification
// could not be restored; retain the target to allow a later retry.
bool ReleaseTarget();
void OnGameThreadGuarded();
Snapshot GetSnapshot();
// UI/worker: pass the generation from the displayed Snapshot.
bool RequestSet(uint64_t generation, int32_t index, float value);
bool RequestRefresh(uint64_t generation);
bool RequestRestore(uint64_t generation);
}
