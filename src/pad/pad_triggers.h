#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

// Forces the DualSense adaptive triggers off. The game sets trigger effects two ways and both are rewritten to "off":
// SDL3 HID output reports to the controller (USB 0x02 / Bluetooth 0x31, through the game executable's WriteFile,
// DeviceIoControl and GetProcAddress("HidD_SetOutputReport")), and ISteamInput::SetDualSenseTriggerEffect when Steam
// Input owns the pad (vtable slot read from steam_api64's flat thunk once the delay-loaded DLL is up). Only
// import-table and Steam vtable entries change, so this does not depend on the game build.
namespace PadTriggers {
void Install();                                  // DllMain, before the game's code runs
void Attach(void (*log)(const char*), const wchar_t* ini_path);  // worker: log sink, saved setting (nullptr: always off), Steam watcher
bool ForcedOff();
void SetForcedOff(bool off);                     // persisted in the ini
void Status(wchar_t* out, size_t size);

// Exposed for pad_triggers_test.
bool RewriteReport(uint8_t* report, size_t size, bool force_reset);  // true when the report was changed
int SteamSlotFromExport(const uint8_t* code);    // vtable slot of a "mov rax,[rcx]; jmp [rax+disp]" thunk, or -1
bool HookSteamInput(void* object, int slot);
void TrackForTest(HANDLE handle);
}
