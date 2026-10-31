#include "game_thread.h"
#include "ue_runtime.h"
#include <cstdio>

namespace GameThread {
namespace {
using Tick = void(__fastcall*)(void*, float, bool);
void* engine_ = nullptr;
Tick original_ = nullptr;
Callback callback_ = nullptr;
PVOID post_ = nullptr;
FaultHandler on_fault_ = nullptr;
volatile LONG installed_ = 0, faults_ = 0, running_ = 0;
DWORD fault_code_ = 0;
uintptr_t fault_address_ = 0;

int Filter(const EXCEPTION_POINTERS* e) {
    fault_code_ = e->ExceptionRecord->ExceptionCode;
    fault_address_ = reinterpret_cast<uintptr_t>(e->ExceptionRecord->ExceptionAddress);
    return EXCEPTION_EXECUTE_HANDLER;
}
bool Guarded() {
    __try { callback_(); return true; }
    __except (Filter(GetExceptionInformation())) { return false; }
}
void Report(DWORD faults) {
    if (!on_fault_) return;
    __try { on_fault_(fault_code_, fault_address_, faults, faults >= kMaxFaults); }
    __except (EXCEPTION_EXECUTE_HANDLER) {}
}
void __fastcall HookedTick(void* self, float delta, bool idle) {
    // A fault is reported and the callback resumes after the handler reset its state; kMaxFaults
    // faults stop it for good. The engine tick itself always runs.
    if (self == engine_ && static_cast<DWORD>(InterlockedCompareExchange(&faults_, 0, 0)) < kMaxFaults &&
        !InterlockedExchange(&running_, 1)) {
        if (!Guarded()) Report(static_cast<DWORD>(InterlockedIncrement(&faults_)));
        InterlockedExchange(&running_, 0);
    }
    original_(self, delta, idle);
    if (self == engine_) {
        const auto post = reinterpret_cast<Callback>(InterlockedCompareExchangePointer(&post_, nullptr, nullptr));
        if (post) {
            __try { post(); }
            __except (EXCEPTION_EXECUTE_HANDLER) {}
        }
    }
}
bool Code(uintptr_t p) { return p >= ue::Base() && p < ue::Base() + 0x20000000; }
}

bool Install(void* engine, uintptr_t expected_tick, Callback callback, FaultHandler on_fault, char* why, size_t size) {
    if (InterlockedCompareExchange(&installed_, 0, 0)) return true;
    auto* const vtable = *reinterpret_cast<uintptr_t* volatile*>(engine);
    size_t count = 0;
    while (count < 1024 && Code(vtable[count])) ++count;
    if (count <= ue::layout::kTickSlot || vtable[ue::layout::kTickSlot] != expected_tick) {
        sprintf_s(why, size, "Tick slot mismatch (%zu entries)", count); return false;
    }
    // Keep vtable[-1] (MSVC complete-object locator) in front of the copy. Never freed:
    // the engine may still be inside a virtual call through it at process exit.
    auto* copy = static_cast<uintptr_t*>(VirtualAlloc(nullptr, (count + 1) * sizeof(uintptr_t), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!copy) { strcpy_s(why, size, "VirtualAlloc"); return false; }
    copy[0] = vtable[-1];
    for (size_t i = 0; i < count; ++i) copy[i + 1] = vtable[i];
    engine_ = engine; callback_ = callback; on_fault_ = on_fault; original_ = reinterpret_cast<Tick>(vtable[ue::layout::kTickSlot]);
    copy[1 + ue::layout::kTickSlot] = reinterpret_cast<uintptr_t>(&HookedTick);
    if (static_cast<uintptr_t>(InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(engine),
            static_cast<LONG64>(reinterpret_cast<uintptr_t>(copy + 1)), static_cast<LONG64>(reinterpret_cast<uintptr_t>(vtable)))) !=
        reinterpret_cast<uintptr_t>(vtable)) {
        strcpy_s(why, size, "engine vtable changed"); return false;
    }
    InterlockedExchange(&installed_, 1);
    return true;
}
void SetPost(Callback post) { InterlockedExchangePointer(&post_, reinterpret_cast<PVOID>(post)); }
bool Installed() { return InterlockedCompareExchange(&installed_, 0, 0) != 0; }
DWORD Faults() { return static_cast<DWORD>(InterlockedCompareExchange(&faults_, 0, 0)); }
}
