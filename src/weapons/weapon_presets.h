#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Input and UI enqueue requests; equipment calls run only on the game thread.
// A preset contains equipped IDs/selection, never weapon levels or inventory.
namespace WeaponPresets {
using Logger = void (*)(const char*);
using InputFocus = bool (*)();
struct PresetView {
    bool known = false;
    bool special_slot = false; // Actual runtime selection is outside canonical preset slots.
    std::vector<std::wstring> main_weapons, shaqi_weapons;
    int32_t main_index = 0, shaqi_index = 0;
};
struct Snapshot {
    bool enabled = false, ready = false, busy = false, has_alternate = false;
    bool touchpad_ready = false;
    bool can_save = false, current_special_slot = false;
    int32_t current_set = 0;
    uint32_t swaps = 0, failures = 0, swipes = 0;
    PresetView sets[2];
    PresetView current; // Most recent successfully read actual equipment, separate from saved banks.
    uint32_t live_reads = 0;
    uint64_t last_read_tick = 0; // GetTickCount64 at the last successful capture.
    std::wstring status = L"第二套武器预设已关闭";
    std::wstring read_status = L"尚未读取当前实际装备";
    std::wstring touchpad_status = L"触控板监听未开启";
};
void Initialize(Logger logger, InputFocus can_gameplay_input);
bool RequestEnable(bool enabled);
bool RequestSwap();
bool RequestCopyCurrentToOther();
bool RequestReadCurrent(); // Coalesced read-only request; safe while feature is off.
bool RequestSaveToSet(int32_t set_index); // 0/1, capture only, also selects logical bank; works while off.
bool RequestSwitchToSet(int32_t set_index); // 0/1, native equip mutation; requires feature enabled.
Snapshot GetSnapshot();
void WorkerTick(); // Own read-only HID input runs on a separate I/O thread.
void OnGameThreadGuarded();
bool BeforeTravel(); // No UObject references held; retire stale world identity.
void Shutdown();    // Worker thread: stops only this module's own HID reader.
bool HandleCommand(const char* verb, const char* arguments);
}
