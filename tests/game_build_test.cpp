// GameBuild::CodeMatches on a fake image: Steam compares exactly; WeGame swaps in its protector-stub bytes only where a
// row says so and only over the Steam bytes the row was made from.
#include <windows.h>
#include <cstdio>
#include <cstring>
#include "game_build.h"
#include "ue_runtime.h"

namespace {
int failures = 0;
void Check(bool ok, const char* what, uintptr_t pin) {
    if (!ok) { ++failures; std::printf("FAIL %s (pin %#llx)\n", what, static_cast<unsigned long long>(pin)); }
}
}

int main() {
    Check(GameBuild::Current() == GameBuild::Id::Steam, "Steam until Set", 0);
    Check(!std::strcmp(GameBuild::Name(GameBuild::Id::Steam), "Steam") && !std::strcmp(GameBuild::Name(GameBuild::Id::WeGame), "WeGame"), "names", 0);
    size_t count = 0;
    const GameBuild::Bytes* rows = GameBuild::Rows(count);
    uintptr_t top = 0x2000;
    for (size_t i = 0; i < count; ++i) {
        const auto& row = rows[i];
        Check(row.build != GameBuild::Id::Steam, "rows are for other builds", row.pin);
        Check(row.count >= 1 && row.count <= sizeof(row.steam), "row length 1..8", row.pin);
        Check(std::memcmp(row.steam, row.other, row.count) != 0, "row changes something", row.pin);
        if (row.pin + 0x2000 > top) top = row.pin + 0x2000;
    }
    // The image: reserved address space with committed pages at its start and at each pin.
    auto* image = static_cast<unsigned char*>(VirtualAlloc(nullptr, top, MEM_RESERVE, PAGE_NOACCESS));
    if (!image || !VirtualAlloc(image, 0x1000, MEM_COMMIT, PAGE_READWRITE)) { std::printf("FAIL reserve\n"); return 1; }
    for (size_t i = 0; i < count; ++i)
        if (!VirtualAlloc(image + (rows[i].pin & ~uintptr_t(0xfff)), 0x2000, MEM_COMMIT, PAGE_READWRITE)) { std::printf("FAIL commit\n"); return 1; }
    ue::Attach(reinterpret_cast<uintptr_t>(image));
    for (size_t i = 0; i < count; ++i) {
        const auto& row = rows[i];
        unsigned char* code = image + row.pin;
        const uintptr_t address = reinterpret_cast<uintptr_t>(code);
        unsigned char steam[16]{}, stale[16]{};
        std::memcpy(steam + row.offset, row.steam, row.count);
        std::memcpy(stale, steam, sizeof(stale)); stale[row.offset] ^= 0xff;
        // The game shows this build's bytes.
        std::memset(code, 0, 32); std::memcpy(code + row.offset, row.other, row.count);
        GameBuild::Set(GameBuild::Id::Steam);
        Check(!GameBuild::CodeMatches(address, steam, sizeof(steam)), "Steam compares exactly (WeGame bytes refused)", row.pin);
        GameBuild::Set(row.build);
        Check(GameBuild::CodeMatches(address, steam, sizeof(steam)), "WeGame takes its row", row.pin);
        Check(!GameBuild::CodeMatches(address, stale, sizeof(stale)), "stale Steam bytes refused", row.pin);
        Check(GameBuild::CodeMatches(address + row.offset + row.count, steam + row.offset + row.count, 4), "outside the row: plain compare", row.pin);
        unsigned char wide[65]{};
        Check(!GameBuild::CodeMatches(address, wide, sizeof(wide)), "over 64 bytes refused", row.pin);
        // The game shows the Steam bytes.
        std::memcpy(code + row.offset, row.steam, row.count);
        Check(!GameBuild::CodeMatches(address, steam, sizeof(steam)), "WeGame refuses Steam bytes", row.pin);
        GameBuild::Set(GameBuild::Id::Steam);
        Check(GameBuild::CodeMatches(address, steam, sizeof(steam)), "Steam takes Steam bytes", row.pin);
    }
    // Code no row covers compares exactly on every build.
    // uint8_t, not unsigned char: update_port.py reads "unsigned char x[] = {...}" as a code self-check.
    const uint8_t same[8] = {1, 2, 3, 4, 5, 6, 7, 8}, other[8] = {1, 2, 3, 4, 5, 6, 7, 9};
    unsigned char* plain = image + 0x100;
    std::memcpy(plain, same, sizeof(same));
    GameBuild::Set(GameBuild::Id::WeGame);
    Check(GameBuild::CodeMatches(reinterpret_cast<uintptr_t>(plain), same, sizeof(same)), "no row: equal bytes match", 0);
    Check(!GameBuild::CodeMatches(reinterpret_cast<uintptr_t>(plain), other, sizeof(other)), "no row: different bytes refused", 0);
    Check(!GameBuild::CodeMatches(reinterpret_cast<uintptr_t>(image) - 1, same, 1), "below the image refused", 0);
    GameBuild::Set(GameBuild::Id::Steam);
    VirtualFree(image, 0, MEM_RELEASE);
    std::printf("game_build rows=%zu failures=%d\n", count, failures);
    return failures ? 1 : 0;
}
