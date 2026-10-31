#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Temporary native player outfit assembly. This does not retarget NPC meshes
// or alter Skeleton assets. UI/worker posts requests; all UE work runs on Tick.
namespace CharacterSwap {
using Logger = void (*)(const char*);
using ReleaseExternalAppearance = bool (*)(); // GT only; false means retry, no reverse Apply.
struct Entry { int32_t index; std::string id; std::wstring name, detail; bool advanced; };
struct Snapshot {
    std::vector<Entry> entries;
    std::wstring status = L"进入场景后可切换主角兼容模型";
    std::wstring original, selected;
    bool ready = false, active = false, busy = false;
};
enum class Action { Replace, Restore };
void Initialize(Logger logger, ReleaseExternalAppearance release_external = nullptr);
bool Request(Action action, int32_t entry = -1);
Snapshot GetSnapshot();
void OnGameThreadGuarded();
// GT only: cancel queued/reapply intent and verify owned suppression release.
// A failed live restore retains its component/baseline for retry.
bool ReleaseForExternal();
// Call on the game thread immediately before intentional map travel, while the
// old component is alive. Restores its temporary save/config state, then waits
// for the new world before reapplying the selected appearance.
void BeforeTravel();
bool HandleCommand(const char* verb, const char* arguments);
}
