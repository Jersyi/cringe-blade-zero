#include "pad_gate.h"
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } else std::printf("ok   %s\n", name); }

constexpr unsigned kUp = 1u << 0, kRight = 1u << 3, kMenu = 1u << 4, kView = 1u << 5, kA = 1u << 10, kB = 1u << 11, kY = 1u << 13, kLT = 1u << 14;
constexpr unsigned kChord = kView | kMenu;

uint32_t InputCrc(const uint8_t* data) {
    uint32_t crc = 0xffffffffu;
    auto step = [&](uint8_t byte) { crc ^= byte; for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); };
    step(0xa1);
    for (size_t i = 0; i < 74; ++i) step(data[i]);
    return ~crc;
}
// USB 0x01 (64 bytes) or Bluetooth 0x31 (78 bytes): sticks, triggers, hat + buttons at the report's base.
std::vector<uint8_t> DualSense(bool bluetooth, uint8_t hat, uint8_t face, uint8_t b1, uint8_t l2 = 0) {
    std::vector<uint8_t> r(bluetooth ? 78 : 64, 0);
    const size_t base = bluetooth ? 2 : 1;
    r[0] = bluetooth ? 0x31 : 0x01;
    if (bluetooth) r[1] = 0x10;
    r[base + 0] = 0x20; r[base + 1] = 0xe0; r[base + 2] = 0x80; r[base + 3] = 0x7f;  // left stick pushed
    r[base + 4] = l2; r[base + 6] = 0x33;
    r[base + 7] = static_cast<uint8_t>((face & 0xf0) | (hat & 0x0f));
    r[base + 8] = b1;
    r[base + 9] = 0x01;  // PS button
    r[base + 30] = 0x5a;  // touch / sensor bytes stay
    if (bluetooth) { const uint32_t crc = InputCrc(r.data()); std::memcpy(r.data() + 74, &crc, 4); }
    return r;
}
}  // namespace

int main() {
    using namespace PadGate;
    // Free play: the gate shows everything, the chord off.
    {
        Gate g;
        View v = Filter(g, kA | kUp, false, 0, 1000);
        Check(!v.neutral && v.buttons == (kA | kUp), "no panel, no chord: all shown");
    }
    // Panel up: at rest; buttons held then stay hidden after it, each until released.
    {
        Gate g;
        View v = Filter(g, kA, true, kChord, 1000);
        Check(v.neutral && v.buttons == 0, "panel up: pad at rest");
        v = Filter(g, kA | kB, true, kChord, 1016);
        Check(v.neutral, "panel up: new presses at rest too");
        v = Filter(g, kA | kB, false, kChord, 1032);
        Check(!v.neutral && v.buttons == 0, "panel gone: buttons held from it hidden");
        v = Filter(g, kA, false, kChord, 1048);
        Check(v.buttons == 0, "A still held: hidden");
        v = Filter(g, kA | kY, false, kChord, 1064);
        Check(v.buttons == kY, "a new press shows at once");
        v = Filter(g, kY, false, kChord, 1080);
        v = Filter(g, kA | kY, false, kChord, 1096);
        Check(v.buttons == (kA | kY), "A pressed again after release: shown");
    }
    // Panel chord: View then Menu within the wait: neither reaches the game, until both are released.
    {
        Gate g;
        View v = Filter(g, kView, false, kChord, 1000);
        Check(v.buttons == 0, "chord: first button held back");
        v = Filter(g, kView | kMenu, false, kChord, 1040);
        Check(v.buttons == 0, "chord: whole chord hidden");
        v = Filter(g, kView | kMenu, false, kChord, 2000);
        Check(v.buttons == 0, "chord: still hidden while held");
        v = Filter(g, kMenu, false, kChord, 2016);
        Check(v.buttons == 0, "chord: half released, the rest still hidden");
        v = Filter(g, 0, false, kChord, 2032);
        v = Filter(g, kMenu, false, kChord, 2048);
        Check(v.buttons == 0, "chord: Menu alone held back again");
        v = Filter(g, kMenu, false, kChord, 2048 + kChordWaitMs);
        Check(v.buttons == kMenu, "chord: Menu alone shown after the wait");
    }
    // A quick tap of a chord button still reaches the game (replayed after release).
    {
        Gate g;
        View v = Filter(g, kMenu, false, kChord, 1000);
        Check(v.buttons == 0, "tap: held back");
        v = Filter(g, 0, false, kChord, 1050);
        Check(v.buttons == kMenu, "tap: shown pressed after release");
        v = Filter(g, 0, false, kChord, 1050 + kReplayMs - 1);
        Check(v.buttons == kMenu, "tap: still shown within the replay");
        v = Filter(g, 0, false, kChord, 1050 + kReplayMs);
        Check(v.buttons == 0, "tap: released after the replay");
    }
    // Chord buttons held when the panel goes away (closed by the chord): hidden until released, no replay.
    {
        Gate g;
        Filter(g, kView | kMenu, true, kChord, 1000);
        View v = Filter(g, kView | kMenu, false, kChord, 1016);
        Check(v.buttons == 0, "closing chord: hidden");
        v = Filter(g, 0, false, kChord, 1032);
        Check(v.buttons == 0, "closing chord: released, nothing replayed");
    }
    // XInput: mapping and in-place filtering.
    {
        Check(FromXInput(0x1000 | 0x0010 | 0x0001, 0, 200) == (kA | kMenu | kUp | (1u << 15)), "XInput buttons map to the pad layout");
        Check(ToXInput(kA | kB | kMenu | kView) == (0x1000 | 0x2000 | 0x0010 | 0x0020), "pad layout maps back to XInput");
        Gate g;
        XPad pad{static_cast<WORD>(0x1000 | 0x0400), 200, 0, 12000, -3000, 5, 6};
        Check(FilterXInput(g, pad, true, kChord, 1000) && pad.buttons == 0 && pad.left == 0 && pad.lx == 0 && pad.ly == 0, "XInput panel up: at rest");
        XPad held{static_cast<WORD>(0x1000 | 0x4000), 200, 0, 12000, 0, 0, 0};
        Check(FilterXInput(g, held, false, kChord, 1016) && held.buttons == 0x4000 && held.left == 0 && held.lx == 12000,
            "XInput after the panel: A and LT held from it hidden, X and the stick pass");
        XPad view{0x0020, 0, 0, 0, 0, 0, 0};
        Gate h;
        Check(FilterXInput(h, view, false, kChord, 1000) && view.buttons == 0, "XInput View held back for the chord");
        XPad plain{0x1000, 0, 0, 0, 0, 0, 0};
        Gate free_gate;
        Check(!FilterXInput(free_gate, plain, false, kChord, 1000) && plain.buttons == 0x1000, "XInput A untouched");
    }
    // DualSense reports: USB and Bluetooth (CRC renewed), parse, neutral, hold-back of Create / Options.
    for (const bool bt : {false, true}) {
        const char* name = bt ? "Bluetooth" : "USB";
        const size_t base = bt ? 2 : 1;
        std::vector<uint8_t> r = DualSense(bt, 2, 0x20, 0x01, 200);  // D-pad right, Cross, L1, L2
        unsigned held = 0;
        Check(DualSenseButtons(r.data(), r.size(), held) && held == (kRight | kA | (1u << 8) | kLT), bt ? "DualSense BT parse" : "DualSense USB parse");
        Gate g;
        Check(FilterDualSense(g, r.data(), r.size(), true, kChord, 1000), name);
        bool rest = r[base] == 0x80 && r[base + 1] == 0x80 && r[base + 2] == 0x80 && r[base + 3] == 0x80 && r[base + 4] == 0 &&
            r[base + 7] == 0x08 && r[base + 8] == 0 && (r[base + 9] & 0x07) == 0 && r[base + 6] == 0x33 && r[base + 30] == 0x5a;
        Check(rest, bt ? "DualSense BT panel up: at rest, counter and sensors kept" : "DualSense USB panel up: at rest, counter and sensors kept");
        if (bt) { uint32_t crc = 0; std::memcpy(&crc, r.data() + 74, 4); Check(crc == InputCrc(r.data()), "DualSense BT CRC renewed"); }
        // After the panel: Cross still held -> hidden, Triangle new -> shown, D-pad right still held -> hidden.
        std::vector<uint8_t> after = DualSense(bt, 2, 0x20 | 0x80, 0x00);
        Check(FilterDualSense(g, after.data(), after.size(), false, kChord, 1016), bt ? "DualSense BT after panel changed" : "DualSense USB after panel changed");
        Check(after[base + 7] == (0x80 | 0x08), bt ? "DualSense BT after panel: held hidden, new shown" : "DualSense USB after panel: held hidden, new shown");
        // Create held back, then Create + Options: hidden.
        Gate c;
        std::vector<uint8_t> create = DualSense(bt, 8, 0, 0x10);
        FilterDualSense(c, create.data(), create.size(), false, kChord, 1000);
        Check((create[base + 8] & 0x10) == 0, bt ? "DualSense BT Create held back" : "DualSense USB Create held back");
        std::vector<uint8_t> both = DualSense(bt, 8, 0, 0x30);
        FilterDualSense(c, both.data(), both.size(), false, kChord, 1030);
        Check((both[base + 8] & 0x30) == 0, bt ? "DualSense BT Create + Options hidden" : "DualSense USB Create + Options hidden");
        std::vector<uint8_t> other(10, 0); other[0] = 0x05;
        Check(!FilterDualSense(c, other.data(), other.size(), true, kChord, 1040), bt ? "other report BT untouched" : "other report USB untouched");
    }
    // Short Bluetooth 0x01 report (before the full mode): buttons at 5..7, triggers at 8 / 9.
    {
        std::vector<uint8_t> r(78, 0);
        r[0] = 0x01; r[1] = r[2] = r[3] = r[4] = 0x80; r[5] = 0x20 | 0x08; r[6] = 0x20; r[8] = 0;
        unsigned held = 0;
        Check(DualSenseButtons(r.data(), r.size(), held) && held == (kA | kMenu), "short BT report parse");
    }
    std::printf(failures ? "%d FAILED\n" : "all passed\n", failures);
    return failures ? 1 : 0;
}
