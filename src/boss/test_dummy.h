#pragma once
#include <string>

// 测试木桩 (招式派生 page): a common mob in front of the hero for trying chains on. It stands still and does not attack
// (the game's own GE_AIDisableActionAndLockhealth: AI.DisableAction; its walk speed held at 0; its AI never sent into
// combat) and does not die (that effect's Health_Min lock, its health refilled). It still takes hits, hit reactions,
// knock-ups and grabs. The page asks from its thread; the game thread spawns it with the game's SpawnAI (as the boss
// module does) and removes it. One at a time: asking again puts a new one in front of the hero.
namespace TestDummy {
using Logger = void (*)(const char*);
void Initialize(Logger logger);
void Request(bool spawn, double distance = 350.0);  // true: spawn (again, `distance` cm in front of the hero), false: remove
struct Snapshot { bool present = false, busy = false; std::wstring status; };
Snapshot GetSnapshot();
void OnGameThreadGuarded();
}
