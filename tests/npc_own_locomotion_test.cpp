// Offline checks for npc_own_locomotion.h: a look's own idle / walk / run picked by the hero's speed and heading, the
// fallbacks when the boss lacks a direction, the play rate from the loop's root travel and the body weight's easing.
#include "npc_own_locomotion.h"
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
using namespace NpcOwnLocomotion;
unsigned Bits(std::initializer_list<int32_t> slots) { unsigned b = 0; for (int32_t s : slots) b |= 1u << s; return b; }

void TestGait() {
    Check(Gait(-1, 0) == 0 && Gait(0, 10) == 0 && Gait(0, 31) == 1, "idle until 30 cm/s");
    Check(Gait(1, 20) == 1 && Gait(1, 14) == 0, "walk until under 15 cm/s");
    Check(Gait(1, 421) == 2 && Gait(2, 380) == 2 && Gait(2, 359) == 1, "run from 420, walk again under 360");
    Check(Gait(0, 600) == 2, "a standing start at run speed runs at once");
    Check(Gait(2, std::nan("")) == 0 && Gait(7, 0) == 0, "NaN speed stands; a bad gait is idle");
}
void TestDirection() {
    Check(Direction(0, -1) == kForward && Direction(180, -1) == kBackward && Direction(-180, -1) == kBackward, "axes");
    Check(Direction(90, -1) == kRight && Direction(-90, -1) == kLeft, "UE +Y is right");
    Check(Direction(50, kForward) == kForward && Direction(56, kForward) == kForward && Direction(58, kForward) == kRight, "forward kept 12 degrees past its edge");
    Check(Direction(40, kRight) == kRight && Direction(32, kRight) == kForward, "right kept down to 33 degrees");
    Check(Direction(-145, kLeft) == kLeft && Direction(-150, kLeft) == kBackward, "left kept to -147 degrees");
    Check(Direction(170, -1) == kBackward && Direction(-170, kBackward) == kBackward && Direction(530, -1) == kBackward, "backward across +-180, wrapped headings");
    Check(Direction(std::nan(""), kLeft) == kLeft && Direction(std::nan(""), -1) == kForward, "NaN heading keeps the direction");
}
void TestPick() {
    const unsigned yuu = Bits({kIdle, kWalk + kForward, kWalk + kBackward, kWalk + kLeft, kWalk + kRight, kRun + kForward});  // 玉妖
    Check(Pick(yuu, 0, kForward) == kIdle && Pick(yuu, 1, kLeft) == kWalk + kLeft && Pick(yuu, 2, kForward) == kRun, "玉妖: idle, side walk, forward run");
    Check(Pick(yuu, 2, kBackward) == kWalk + kBackward && Pick(yuu, 2, kRight) == kWalk + kRight, "running backward / sideways without that run: the walk of that way");
    const unsigned forward_only = Bits({kIdle, kWalk, kRun});  // a mount's three loops
    Check(Pick(forward_only, 1, kLeft) == kWalk && Pick(forward_only, 2, kBackward) == kRun, "only forward loops: forward");
    const unsigned walks = Bits({kIdle, kWalk, kWalk + kBackward, kWalk + kLeft, kWalk + kRight});
    Check(Pick(walks, 2, kForward) == kWalk && Pick(walks, 2, kLeft) == kWalk + kLeft, "no run at all: the walks");
    Check(Pick(Bits({kIdle}), 2, kRight) == kIdle && Pick(Bits({kWalk}), 0, kForward) == kWalk && Pick(Bits({kRun}), 0, kForward) == kRun, "idle only / no idle");
    Check(Pick(0, 1, kForward) == -1 && Pick(yuu, 1, 9) == kWalk, "nothing: -1; a bad direction is forward");
}
void TestRate() {
    Check(Rate(kIdle, 300, 200) == 1 && Rate(kWalk, 300, 0) == 1 && Rate(kWalk, 300, 20) == 1, "idle, unknown travel and in-place loops play at 1");
    Check(std::abs(Rate(kWalk, 300, 200) - 1.5) < 1e-12 && std::abs(Rate(kRun + kLeft, 250, 500) - 0.6) < 1e-12, "speed over travel");
    Check(Rate(kRun, 2000, 400) == kRateMax && Rate(kWalk, 10, 300) == kRateMin, "clamped 0.6..1.6");
    Check(Rate(kWalk, std::nan(""), 300) == 1 && Rate(9, 300, 200) == 1, "NaN speed, bad slot: 1");
}
void TestEase() {
    Check(Ease(0, true, 125) == 0.5 && Ease(0.5, true, 200) == 1, "rises in 250 ms");
    Check(Ease(1, false, 50) == 0.5 && Ease(0.5, false, 80) == 0, "falls in 100 ms");
    Check(Ease(std::nan(""), true, 25) == 0.1 && Ease(2, true, 0) == 1 && Ease(0.3, true, -5) == 0.3, "NaN, out of range and negative steps are safe");
}
void TestTravel() {
    Travel t;
    t.Sample(kWalk, 0.0, 0, 0);
    for (int i = 1; i <= 30; ++i) t.Sample(kWalk, i / 30.0, i * 200.0 / 30.0, 0);  // 200 cm/s forward
    Check(std::abs(t.Speed(kWalk) - 200) < 1e-9, "a second of root motion: 200 cm/s");
    t.Sample(kWalk, 0.01, 0, 0); t.Sample(kWalk, 0.02, 2, 0);  // wrap: the jump back adds nothing
    Check(std::abs(t.Speed(kWalk) - 200) < 1e-9, "a wrap adds nothing, the next step 200 cm/s again");
    t.Sample(kRun, 0.5, 900, 0); t.Sample(kRun, 0.9, 1000, 0);  // switch, then a 0.4 s jump
    Check(t.Speed(kRun) == 0, "a switch and a jump over 0.25 s add nothing");
    Travel in_place; for (int i = 0; i <= 30; ++i) in_place.Sample(kWalk + kLeft, i / 30.0, 0, 0);
    Check(in_place.Speed(kWalk + kLeft) == 0 && Rate(kWalk + kLeft, 250, in_place.Speed(kWalk + kLeft)) == 1, "an in-place loop measures 0 and plays at 1");
    Travel short_run; short_run.Sample(kRun, 0, 0, 0); short_run.Sample(kRun, 0.2, 100, 0);
    Check(short_run.Speed(kRun) == 0, "under half a second measured: unknown");
}
}

int main() {
    TestGait();
    TestDirection();
    TestPick();
    TestRate();
    TestEase();
    TestTravel();
    std::printf("npc_own_locomotion_test checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
