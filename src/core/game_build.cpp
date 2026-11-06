#include "game_build.h"
#include <atomic>
#include <cstring>
#include <iterator>
#include "ue_runtime.h"

namespace GameBuild {
namespace {
std::atomic<int> current_{static_cast<int>(Id::Steam)};
// Code bytes at pins that differ on another build from the Steam bytes the self-checks hold: {build, pin, offset from
// the pin, count, Steam bytes, that build's bytes}. update_port.py build <name> rewrites a build's rows (only jumps
// into the protector section qualify); apply keeps the pin addresses current. The empty last row ends the table.
constexpr Bytes kRows[] = {  // exe-pins exe-build-bytes
    {Id::WeGame, 0x12df700, 1, 4, {0xcb,0x31,0x6a,0x0e}, {0x7b,0x43,0x55,0x0e}},  // debug_supplement.cpp:35 row
    {Id::WeGame, 0x57f0870, 1, 4, {0x0b,0x31,0x17,0x11}, {0x7b,0xbc,0xe0,0x0f}},  // debug_supplement.cpp:1171 row, debug_supplement.cpp:1172 row
    {Id::WeGame, 0x5e28b90, 1, 4, {0x4b,0xf3,0xde,0x11}, {0x0b,0xdb,0x7f,0x10}},  // air_skills.cpp:446 remove
    {Id::WeGame, 0x698fbf0, 1, 4, {0xcb,0x85,0xaa,0x12}, {0x7b,0xe4,0x0c,0x11}},  // weapon_presets.cpp:191 row
    {},
};
}

void Set(Id id) { current_.store(static_cast<int>(id)); }
Id Current() { return static_cast<Id>(current_.load()); }
const char* Name(Id id) { return id == Id::WeGame ? "WeGame" : "Steam"; }

bool CodeMatches(uintptr_t address, const void* expected, size_t n) {
    const Id build = Current();
    if (build == Id::Steam) return !std::memcmp(reinterpret_cast<const void*>(address), expected, n);
    unsigned char want[64]{};
    if (n > sizeof(want) || address < ue::Base()) return false;
    std::memcpy(want, expected, n);
    const uintptr_t rva = address - ue::Base();
    for (const Bytes& row : kRows) {
        if (row.build != build || !row.count) continue;
        for (size_t i = 0; i < row.count && i < sizeof(row.steam); ++i) {
            const uintptr_t at = row.pin + row.offset + i;
            if (at < rva || at >= rva + n) continue;
            unsigned char& b = want[at - rva];
            if (b != row.steam[i]) return false;
            b = row.other[i];
        }
    }
    return !std::memcmp(reinterpret_cast<const void*>(address), want, n);
}

const Bytes* Rows(size_t& count) { count = std::size(kRows) - 1; return kRows; }
}
