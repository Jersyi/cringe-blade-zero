#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

// Worker/UI posts plain values; all engine calls and names are resolved on the
// existing game thread. No render hooks or additional engine vtable are needed.
namespace BossRush {
using Logger = void (*)(const char*);
struct Options {
    bool override_rank = false;
    int32_t rank = 0;
    bool override_cycle = false;
    int32_t cycle_number = 1; // User-facing 1-based; convert at the verified API boundary.
    bool override_difficulty = false;
    int32_t difficulty = 2;  // Native enum value, never the combo-box index.
    bool auto_next = false;
    bool heal_on_start = true;
};
struct Entry {
    int32_t index = -1;
    std::string id;
    std::wstring name, map, detail;
    bool available = false;
};
struct Difficulty { int32_t value; std::wstring name; };
struct Snapshot {
    std::vector<Entry> entries;
    std::vector<Difficulty> difficulties;
    Options options;
    std::wstring status = L"Boss Rush: waiting for the game thread";
    std::wstring rank_description = L"Checking the game's Rank interface";
    bool ready = false, running = false, busy = false;
    bool rank_supported = false, cycle_supported = false, difficulty_supported = false;
    int32_t rank_min = 0, rank_max = 0, cycle_max = 100;
    int32_t current = -1, completed = 0, total = 0;
};
enum class Action { Teleport, DirectFight, StartRush, Next, Retry, Stop, ApplyOptions, Refresh };
void Initialize(Logger logger, const wchar_t* directory);
void OnGameThreadGuarded();
bool Request(Action action, int32_t entry, const Options& options);
Snapshot GetSnapshot();
bool IsTransitioning(); // Atomic snapshot flag only; no engine reads.
bool AllowsAppearanceChanges(); // Game thread only; blocks incomplete loading/spawn.
bool HandleCommand(const char* verb, const char* arguments);
}
