#pragma once
#include <cstddef>
#include <cstdint>

// The shipping PBZ.exe this process runs. Steam and WeGame ship the same changelist with the same game code: every
// pinned RVA, global and engine layout is the same in both (update_port.py build wegame checks all of them). They
// differ only in the code protector: the Steam exe also carries Denuvo, so the protector section and every jump the
// protector put into the game code (the entries of the functions it wraps, switch tables) point elsewhere. Self-checks
// of such an entry hold the Steam bytes; CodeMatches swaps in the running build's bytes from the table in
// game_build.cpp, which update_port.py writes.
namespace GameBuild {
enum class Id : int { Steam = 0, WeGame = 1 };
void Set(Id id);  // once, when proxy.cpp has verified the exe, before any feature reads the game
Id Current();     // Steam until Set
const char* Name(Id id);
// The code at address (ue::Base() + a pinned RVA) equals `expected`, the Steam bytes. On Steam this is exactly
// !memcmp(address, expected, n). On another build the table's bytes for that build replace the Steam bytes they were
// made from; a row whose Steam bytes differ from `expected` is stale (the sources moved on without update_port.py
// build), so the check fails and its feature stays off.
bool CodeMatches(uintptr_t address, const void* expected, size_t n);

// The table, for tests and the log.
struct Bytes { Id build; uintptr_t pin; uint8_t offset, count; unsigned char steam[8], other[8]; };
const Bytes* Rows(size_t& count);
}
