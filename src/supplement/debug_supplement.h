#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Optional supplement container PBZDebugSupplement.pak/.utoc/.ucas under the game's Paks folder (normally Paks\~mods).
// It carries the debug Blueprint BP_CollectHelp and its dependencies, which the 11-04 game update deleted, byte-identical
// to the 10-29 build. The game does not mount extra paks itself, so the first game-thread tick mounts it. Without the
// container nothing here mounts, loads, spawns or calls anything.
namespace DebugSupplement {
using Logger = void (*)(const char*);
// One entry the Blueprint's Collect functions registered (debug_supplement_catalog.generated.h), in catalog order.
struct Item {
    std::wstring category, group, label;
    bool removed = false;    // its code reaches a native the 11-04 game emptied or a removed console command
    bool available = false;  // the loaded class has its event and every function it calls by name
};
// sequence counts every finished call request; ok means the event ran.
struct Result { uint64_t sequence = 0; int32_t item = -1; bool ok = false; };
struct Snapshot {
    uint64_t generation = 0;  // bumps whenever the published state changes
    bool present = false, mounted = false, loaded = false;
    std::wstring status;
    std::vector<Item> items;
    uint32_t calls = 0;
    std::vector<Result> results;  // the latest call results (run or refused), oldest first
};
void Initialize(Logger logger, const wchar_t* executable_directory);
bool Present();             // all three container files found at startup
Snapshot GetSnapshot();
bool RequestLoad();         // UI/worker: load the class and check every item against it
bool RequestCall(int32_t item);
bool HandleCommand(const char* verb, const char* argument);  // "supplement status|load|list|call <item>"
void OnGameThreadGuarded();
}
