#include "pad_gate.h"
#include <cstring>

namespace PadGate {
namespace {
constexpr unsigned kUp = 1u << 0, kDown = 1u << 1, kLeft = 1u << 2, kRight = 1u << 3, kMenu = 1u << 4, kView = 1u << 5;
constexpr unsigned kL3 = 1u << 6, kR3 = 1u << 7, kLB = 1u << 8, kRB = 1u << 9, kA = 1u << 10, kB = 1u << 11, kX = 1u << 12, kY = 1u << 13;
constexpr unsigned kLT = 1u << 14, kRT = 1u << 15;
constexpr unsigned kDpad = kUp | kDown | kLeft | kRight;

// Where the fields of one DualSense input report are.
struct Layout { size_t lx, l2, r2, b0, b1, b2, need; bool crc; };
bool LayoutOf(const uint8_t* r, size_t size, Layout& at) {
    if (!r || size < 10) return false;
    if (r[0] == 0x31 && size >= 78) { at = {2, 6, 7, 9, 10, 11, 12, true}; return true; }       // Bluetooth full
    if (r[0] != 0x01) return false;
    if (size == 10 || size >= 78) { at = {1, 8, 9, 5, 6, 7, 10, false}; return true; }        // Bluetooth short
    if (size >= 11) { at = {1, 5, 6, 8, 9, 10, 11, false}; return true; }                     // USB
    return false;
}
unsigned FromHat(unsigned hat) {
    unsigned mask = 0;
    if (hat > 7) return 0;
    if (hat == 0 || hat == 1 || hat == 7) mask |= kUp;
    if (hat >= 1 && hat <= 3) mask |= kRight;
    if (hat >= 3 && hat <= 5) mask |= kDown;
    if (hat >= 5 && hat <= 7) mask |= kLeft;
    return mask;
}
unsigned ToHat(unsigned dpad) {
    const bool up = (dpad & kUp) != 0, down = (dpad & kDown) != 0, left = (dpad & kLeft) != 0, right = (dpad & kRight) != 0;
    if (up && !down) return right && !left ? 1 : left && !right ? 7 : 0;
    if (down && !up) return right && !left ? 3 : left && !right ? 5 : 4;
    if (right && !left) return 2;
    if (left && !right) return 6;
    return 8;
}
uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc) {
    for (size_t i = 0; i < size; ++i) { crc ^= data[i]; for (int b = 0; b < 8; ++b) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return crc;
}
}  // namespace

View Filter(Gate& g, unsigned held, bool blocked, unsigned chord, ULONGLONG now) {
    View view;
    const unsigned pressed = held & ~g.last;
    g.last = held;
    g.chord_down &= held;
    if (blocked) {
        g.stale |= held;
        g.waiting = 0; g.replay = 0;
        view.neutral = true;
        g.hid = true;
        return view;
    }
    g.stale &= held;
    if (chord && (held & chord) == chord) {
        g.chord_down |= chord;
        g.waiting = 0;
    } else if (chord) {
        const unsigned fresh = pressed & chord & ~g.chord_down & ~g.stale;
        if (fresh) {
            if (!g.waiting) g.waiting_since = now;
            g.waiting |= fresh;
        }
        const unsigned early = g.waiting & ~held;
        if (early) { g.replay |= early; g.replay_until = now + kReplayMs; g.waiting &= ~early; }
        if (g.waiting && now - g.waiting_since >= kChordWaitMs) g.waiting = 0;
    } else {
        g.waiting = 0;
    }
    if (g.replay && now >= g.replay_until) g.replay = 0;
    const unsigned hidden = held & (g.stale | g.chord_down | g.waiting);
    view.buttons = (held & ~hidden) | g.replay;
    g.hid = hidden != 0 || g.replay != 0;
    return view;
}

unsigned FromXInput(WORD buttons, BYTE left, BYTE right) {
    unsigned mask = buttons & 0x03FFu;                // D-pad, Start, Back, thumbs, shoulders: the same bits
    mask |= (static_cast<unsigned>(buttons) >> 2) & (0xFu << 10);  // A B X Y
    if (left > kTriggerOn) mask |= kLT;
    if (right > kTriggerOn) mask |= kRT;
    return mask;
}
WORD ToXInput(unsigned buttons) {
    return static_cast<WORD>((buttons & 0x03FFu) | ((buttons << 2) & 0xF000u));
}
bool FilterXInput(Gate& gate, XPad& pad, bool blocked, unsigned chord, ULONGLONG now) {
    const unsigned held = FromXInput(pad.buttons, pad.left, pad.right);
    const View view = Filter(gate, held, blocked, chord, now);
    XPad out = pad;
    if (view.neutral) {
        out = {};
    } else {
        const unsigned hide = held & ~view.buttons, show = view.buttons & ~held;
        out.buttons = static_cast<WORD>((pad.buttons & ~ToXInput(hide)) | ToXInput(show));
        if (hide & kLT) out.left = 0;
        if (hide & kRT) out.right = 0;
        if (show & kLT) out.left = 255;
        if (show & kRT) out.right = 255;
    }
    if (!std::memcmp(&out, &pad, sizeof(out))) return false;
    pad = out;
    return true;
}

bool DualSenseButtons(const uint8_t* r, size_t size, unsigned& held) {
    Layout at{};
    if (!LayoutOf(r, size, at) || size < at.need) return false;
    const unsigned b0 = r[at.b0], b1 = r[at.b1];
    unsigned mask = FromHat(b0 & 0x0f);
    if (b0 & 0x10) mask |= kX;
    if (b0 & 0x20) mask |= kA;
    if (b0 & 0x40) mask |= kB;
    if (b0 & 0x80) mask |= kY;
    if (b1 & 0x01) mask |= kLB;
    if (b1 & 0x02) mask |= kRB;
    if ((b1 & 0x04) || r[at.l2] > kTriggerOn) mask |= kLT;
    if ((b1 & 0x08) || r[at.r2] > kTriggerOn) mask |= kRT;
    if (b1 & 0x10) mask |= kView;
    if (b1 & 0x20) mask |= kMenu;
    if (b1 & 0x40) mask |= kL3;
    if (b1 & 0x80) mask |= kR3;
    held = mask;
    return true;
}
bool FilterDualSense(Gate& gate, uint8_t* r, size_t size, bool blocked, unsigned chord, ULONGLONG now) {
    Layout at{};
    unsigned held = 0;
    if (!LayoutOf(r, size, at) || !DualSenseButtons(r, size, held)) return false;
    const View view = Filter(gate, held, blocked, chord, now);
    bool changed = false;
    const auto put = [&](size_t index, uint8_t value) { if (r[index] != value) { r[index] = value; changed = true; } };
    if (view.neutral) {
        for (size_t i = 0; i < 4; ++i) put(at.lx + i, 0x80);
        put(at.l2, 0); put(at.r2, 0);
        put(at.b0, 0x08); put(at.b1, 0);
        put(at.b2, static_cast<uint8_t>(r[at.b2] & ~0x07));  // PS, touchpad click, mute
    } else {
        const unsigned hide = held & ~view.buttons, show = view.buttons & ~held;
        if (hide | show) {
            const unsigned want = view.buttons;
            uint8_t b0 = r[at.b0], b1 = r[at.b1];
            if ((hide | show) & kDpad) b0 = static_cast<uint8_t>((b0 & 0xf0) | ToHat(want & kDpad));
            const struct { unsigned bit; uint8_t* byte; uint8_t mask; } kMap[] = {
                {kX, &b0, 0x10}, {kA, &b0, 0x20}, {kB, &b0, 0x40}, {kY, &b0, 0x80},
                {kLB, &b1, 0x01}, {kRB, &b1, 0x02}, {kLT, &b1, 0x04}, {kRT, &b1, 0x08},
                {kView, &b1, 0x10}, {kMenu, &b1, 0x20}, {kL3, &b1, 0x40}, {kR3, &b1, 0x80},
            };
            for (const auto& item : kMap) {
                if (hide & item.bit) *item.byte = static_cast<uint8_t>(*item.byte & ~item.mask);
                if (show & item.bit) *item.byte = static_cast<uint8_t>(*item.byte | item.mask);
            }
            put(at.b0, b0); put(at.b1, b1);
            if (hide & kLT) put(at.l2, 0);
            if (hide & kRT) put(at.r2, 0);
            if (show & kLT) put(at.l2, 255);
            if (show & kRT) put(at.r2, 255);
        }
    }
    if (changed && at.crc) {
        // Bluetooth input CRC: CRC-32 of 0xA1 and bytes 0..73, stored little-endian at 74.
        const uint8_t header = 0xa1;
        const uint32_t crc = ~Crc32(r, 74, Crc32(&header, 1, 0xffffffffu));
        std::memcpy(r + 74, &crc, sizeof(crc));
    }
    return changed;
}
}  // namespace PadGate
