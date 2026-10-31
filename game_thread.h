#pragma once
#include <windows.h>
#include <cstdint>

// Runs a callback at the start of every UGameEngine::Tick. Only the engine
// object's vtable pointer is redirected to a private copy whose Tick slot is
// replaced; executable pages and the shared vtable stay untouched.
namespace GameThread {
using Callback = void (*)();
// Runs on the game thread right after the callback raised a structured exception (code, faulting
// instruction, faults so far). The callback keeps running until kMaxFaults; `disabled` marks the last call.
using FaultHandler = void (*)(DWORD code, uintptr_t address, DWORD faults, bool disabled);
constexpr DWORD kMaxFaults = 3;
bool Install(void* engine, uintptr_t expected_tick, Callback callback, FaultHandler on_fault, char* why, size_t why_size);
// Called on the game thread after the engine tick. Null until set. Faults here are swallowed.
void SetPost(Callback post);
bool Installed();
DWORD Faults();  // structured exceptions caught around the callback
}
