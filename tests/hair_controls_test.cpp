#include "hair_controls.h"
#include "combo_menu.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>

// This harness does not construct the combo page or its text input.
namespace ComboMenu { bool OwnsTextMessage(const MSG&) { return false; } bool HandleRecordingMessage(const MSG&) { return false; } }

namespace {
int failures = 0;
void Check(bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
constexpr uint64_t Pair(int32_t value) { return static_cast<uint32_t>(value) | (static_cast<uint64_t>(static_cast<uint32_t>(value)) << 32); }
void Commit(uintptr_t base, uintptr_t rva) {
    if (!VirtualAlloc(reinterpret_cast<void*>(base + (rva & ~uintptr_t{4095})), 4096, MEM_COMMIT, PAGE_READWRITE)) std::abort();
}
}
int main() {
    using namespace HairAddresses;
    // A fake image just large enough for the hair globals (they move with each game update).
    const uintptr_t span = ((std::max)({kHairWrapper, kAppliedHairMode, kViewModeCache, kShaderPrint}) + 0x20000) & ~uintptr_t{0xffff};
    const uintptr_t base = reinterpret_cast<uintptr_t>(VirtualAlloc(nullptr, span, MEM_RESERVE, PAGE_NOACCESS));
    if (!base) return 2;
    Commit(base, kHairWrapper); Commit(base, kViewModeCache); Commit(base, kAppliedHairMode); Commit(base, kShaderPrint);
    alignas(8) uint64_t hair = Pair(1), view = Pair(7), replacement = Pair(1);
    auto* hair_pointer = reinterpret_cast<uintptr_t*>(base + kHairWrapper + 16);
    auto* view_pointer = reinterpret_cast<uintptr_t*>(base + kViewModeCache);
    auto* shader = reinterpret_cast<int32_t*>(base + kShaderPrint);
    *hair_pointer = reinterpret_cast<uintptr_t>(&hair); *view_pointer = reinterpret_cast<uintptr_t>(&view); *shader = 0;
    {
        HairControls controls(base, false);
        Check(controls.ToggleHair() && hair == Pair(0), "Groom to mesh");
        Check(controls.ToggleHair() && hair == Pair(1), "Mesh to Groom");
        Check(controls.ToggleBounds() && view == Pair(32) && *shader == 1, "Bounds enables native values");
        Check(controls.ToggleBounds() && view == Pair(7) && *shader == 0, "Bounds off restores original");
        Check(controls.ToggleBounds(), "Bounds enable again");
        view = Pair(9); *shader = 0;
        Check(controls.ToggleBounds() && view == Pair(9) && *shader == 0, "External debug edits preserved");
        *view_pointer = 0;
        Check(!controls.ToggleBounds() && *shader == 0, "Uninitialized cache rejected");
        *view_pointer = reinterpret_cast<uintptr_t>(&view); view = Pair(41);
        Check(!controls.ToggleBounds() && view == Pair(41) && *shader == 0, "Unknown debug range rejected");
        hair = Pair(8);
        Check(!controls.ToggleHair() && hair == Pair(8), "Unknown hair range rejected");
        hair = Pair(1); *hair_pointer = reinterpret_cast<uintptr_t>(&replacement);
        Check(!controls.ToggleHair() && replacement == Pair(1), "CVar pointer identity change rejected");
        *hair_pointer = reinterpret_cast<uintptr_t>(&hair); hair = Pair(0);
        controls.Restore();
        Check(hair == Pair(0), "External hair edit preserved");
    }
    {
        hair = Pair(2); view = Pair(3); *shader = 1;
        HairControls controls(base, false);
        Check(controls.ToggleHair() && hair == Pair(1), "Both to Groom");
        Check(controls.ToggleBounds(), "Preexisting ShaderPrint accepted");
    }
    Check(hair == Pair(2) && view == Pair(3) && *shader == 1, "Destructor restores only owned original values");
    uint64_t result = 0;
    Check(!HairMemory::Read(1, &result, sizeof(result)), "Invalid read rejected");
    Check(!HairMemory::Exchange64(1, 0, 1), "Invalid atomic write rejected");
    Check(!HairMemory::Exchange64(reinterpret_cast<uintptr_t>(&hair) + 1, 0, 1), "Misaligned atomic write rejected");
    Check(!HairMemory::Exchange64(reinterpret_cast<uintptr_t>(&hair), Pair(99), 0) && hair == Pair(2), "Concurrent value CAS rejected");
    DWORD old = 0; VirtualProtect(reinterpret_cast<void*>(base + (kShaderPrint & ~uintptr_t{4095})), 4096, PAGE_READONLY, &old);
    Check(!HairMemory::Exchange32(base + kShaderPrint, 1, 0), "Readonly page rejected");
    VirtualFree(reinterpret_cast<void*>(base), 0, MEM_RELEASE);
    std::printf("hair_controls_test failures=%d\n", failures); return failures ? 1 : 0;
}
