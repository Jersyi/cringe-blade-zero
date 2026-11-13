#pragma once
// 主角体型 (the NpcCombo HeroSize modifier) and 镜头取景 按体型比例 (NpcCombo::kScaleHeroRatio): the look's height over
// the hero's, the scale a 主角体型 move shows the look at, its easing, and the 视角跟随 camera's push-out by the shown
// size. Pure; offline checks in tests/npc_hero_size_test.cpp.
#include <algorithm>
#include <cmath>

namespace NpcHeroSize {
constexpr double kRatioMin = 0.2, kRatioMax = 12.0;  // a look's height over the hero's outside this: unknown
constexpr double kEaseMs = 200;                      // to the hero's size and back
constexpr double kGrowMin = 0.4, kGrowMax = 12.5;    // the camera's push-out under 按体型比例

inline double Ratio(double hero, double npc) {
    if (!(std::isfinite(hero) && std::isfinite(npc) && hero > 10 && npc > 10)) return 0;
    const double r = npc / hero;
    return r >= kRatioMin && r <= kRatioMax ? r : 0;
}
// The look's height over the hero's: the reference-pose head heights (cm over the root), else the pelvis heights;
// 0: unknown (no such bones, or a ratio outside kRatioMin..kRatioMax).
inline double LookRatio(double hero_head, double npc_head, double hero_pelvis, double npc_pelvis) {
    const double head = Ratio(hero_head, npc_head);
    return head > 0 ? head : Ratio(hero_pelvis, npc_pelvis);
}
inline bool Known(double look_ratio) { return look_ratio >= kRatioMin && look_ratio <= kRatioMax; }
// The scale 主角体型 shows the look at: the hero's height over the look's (1: unknown).
inline double Target(double look_ratio) { return Known(look_ratio) ? 1 / look_ratio : 1; }
// Its progress (0: the look's own size, 1: the hero's) one step on: up while `on`, down otherwise, over kEaseMs.
inline double Step(double progress, bool on, double dt_ms) {
    if (!std::isfinite(progress)) progress = 0;
    progress = std::clamp(progress, 0.0, 1.0);
    if (!(std::isfinite(dt_ms) && dt_ms > 0)) return progress;
    return std::clamp(progress + (on ? 1 : -1) * dt_ms / kEaseMs, 0.0, 1.0);
}
// The scale shown at that progress: smoothstep from 1 to the target.
inline double Shown(double progress, double target) {
    const double p = std::clamp(std::isfinite(progress) ? progress : 0.0, 0.0, 1.0), s = p * p * (3 - 2 * p);
    return 1 + ((std::isfinite(target) && target > 0 ? target : 1) - 1) * s;
}
// The game camera's lift (NPC_CAMERA: raised by the look's head height over the hero's, `lift` cm at the look's own
// size) while the look is shown at `shown`: the shown head's height over the hero's, none at the hero's size (11-13:
// 彭猛 shown at the hero's size kept a camera raised 252 cm). An unknown ratio or one near 1: `lift` as it is.
inline double LiftAt(double lift, double look_ratio, double shown) {
    if (!std::isfinite(lift)) return 0;
    if (!Known(look_ratio) || std::abs(look_ratio - 1) < 0.02 || !(std::isfinite(shown) && shown > 0)) return lift;
    return lift * (look_ratio * shown - 1) / (look_ratio - 1);
}
// 镜头取景 按体型比例: the camera's push-out, the shown model's height over the hero's now (the look's ratio, else
// `fallback`, times the scale it is shown at).
inline double Grow(double look_ratio, double shown, double fallback = 1) {
    const double r = Known(look_ratio) ? look_ratio : Known(fallback) ? fallback : 1;
    const double s = std::isfinite(shown) && shown > 0 ? shown : 1;
    return std::clamp(r * s, kGrowMin, kGrowMax);
}
}
