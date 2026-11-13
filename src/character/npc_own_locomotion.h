#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// A look's own locomotion (2026-11-13): which of the boss's idle / walk / run loops (kOwnLocomotion) its pose puppet
// plays, at what rate, and how much of the display takes it. Any thread, no engine calls.
// Bones the hero has none of (玉妖's foot blades) always take the puppet; the boss locomotion option (BOSS 自带移动动画)
// eases the whole body onto it while the hero only moves (no montage, on the ground, no chain move).
namespace NpcOwnLocomotion {
// Loop slots: idle, then walk and run by direction (forward, backward, left, right).
enum : int32_t { kIdle = 0, kWalk = 1, kRun = 5, kLoops = 9 };
enum : int32_t { kForward = 0, kBackward = 1, kLeft = 2, kRight = 3 };
// Speed bands (cm/s): walk from 30 (idle again under 15), run from 420 (walk again under 360).
constexpr double kWalkCm = 30, kIdleCm = 15, kRunCm = 420, kWalkBackCm = 360;
inline int32_t Gait(int32_t gait, double speed) {
    if (!std::isfinite(speed)) speed = 0;
    gait = gait < 0 || gait > 2 ? 0 : gait;
    if (gait == 0 && speed > kWalkCm) gait = 1;
    if (gait == 1 && speed > kRunCm) gait = 2;
    if (gait == 2 && speed < kWalkBackCm) gait = 1;
    if (gait == 1 && speed < kIdleCm) gait = 0;
    return gait;
}
// heading: the velocity's direction in the actor's frame, degrees (0 forward, +90 right: UE's +Y). Each direction owns
// the quarter around its axis; the current one is kept until the heading is kDirKeepDeg past its edge.
constexpr double kDirKeepDeg = 12;
inline double Off(double heading, double axis) {
    double a = std::fmod(std::fabs(heading - axis), 360.0);
    return a > 180 ? 360 - a : a;
}
inline int32_t Direction(double heading, int32_t current) {
    if (!std::isfinite(heading)) return current >= 0 && current < 4 ? current : kForward;
    constexpr double axis[4] = {0, 180, -90, 90};
    if (current >= 0 && current < 4 && Off(heading, axis[current]) <= 45 + kDirKeepDeg) return current;
    int32_t best = kForward;
    for (int32_t d = 1; d < 4; ++d) if (Off(heading, axis[d]) < Off(heading, axis[best])) best = d;
    return best;
}
// The loop to play among those the look has (present: bit 1 << slot). Walking: that direction's walk, the forward walk,
// that direction's run, the forward run. Running: that direction's run, its walk, the forward run, the forward walk.
// Then idle; standing: idle, then the forward walk, then the forward run. -1 when the look has none.
inline int32_t Pick(unsigned present, int32_t gait, int32_t dir) {
    dir = dir < 0 || dir > 3 ? kForward : dir;
    const auto has = [&](int32_t slot) { return slot >= 0 && slot < kLoops && (present >> slot & 1u) != 0; };
    int32_t order[5] = {kIdle, kWalk, kRun, -1, -1};
    if (gait == 1) { order[0] = kWalk + dir; order[1] = kWalk; order[2] = kRun + dir; order[3] = kRun; order[4] = kIdle; }
    else if (gait == 2) { order[0] = kRun + dir; order[1] = kWalk + dir; order[2] = kRun; order[3] = kWalk; order[4] = kIdle; }
    for (const int32_t slot : order) if (has(slot)) return slot;
    return -1;
}
// Play rate of a walk / run loop: the hero's speed over the loop's own travel (cm/s, measured off its root motion while
// it plays), within kRateMin..kRateMax; 1 for idle or while the travel is unknown or under kTravelMinCm (in place).
constexpr double kRateMin = 0.6, kRateMax = 1.6, kTravelMinCm = 30;
inline double Rate(int32_t slot, double speed, double travel) {
    if (slot < kWalk || slot >= kLoops || !std::isfinite(speed) || !std::isfinite(travel) || travel < kTravelMinCm) return 1;
    return std::clamp(speed / travel, kRateMin, kRateMax);
}
// The body weight: toward 1 in kRiseMs while the hero only moves, toward 0 in kFallMs otherwise (a press leaves quickly).
constexpr double kRiseMs = 250, kFallMs = 100;
inline double Ease(double body, bool on, double dt_ms) {
    if (!std::isfinite(body)) body = 0;
    if (!std::isfinite(dt_ms) || dt_ms < 0) dt_ms = 0;
    body = std::clamp(body, 0.0, 1.0);
    return on ? (std::min)(1.0, body + dt_ms / kRiseMs) : (std::max)(0.0, body - dt_ms / kFallMs);
}
// Root-motion travel of the loop playing: two samples of the puppet's root at consecutive positions of the same loop add
// the distance between them over the time between them; a wrap, a switch or a jump over 0.25 s adds nothing.
struct Travel {
    double distance[kLoops]{}, seconds[kLoops]{};
    int32_t slot = -1; double position = 0, x = 0, y = 0;
    void Sample(int32_t at, double pos, double root_x, double root_y) {
        if (at >= 0 && at < kLoops && at == slot && pos > position && pos - position < 0.25 && std::isfinite(root_x + root_y)) {
            distance[at] += std::hypot(root_x - x, root_y - y); seconds[at] += pos - position;
        }
        slot = at; position = pos; x = root_x; y = root_y;
    }
    // cm/s once half a second of the loop was measured, else 0.
    double Speed(int32_t at) const { return at >= 0 && at < kLoops && seconds[at] >= 0.5 ? distance[at] / seconds[at] : 0; }
};
}
