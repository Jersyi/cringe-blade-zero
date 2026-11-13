// Offline checks for npc_hero_size.h: 主角体型's look-over-hero height ratio, the scale it shows the look at, its easing
// and the 镜头取景 按体型比例 push-out of the 视角跟随 camera.
#include "npc_hero_size.h"
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int failures = 0, checks = 0;
void Check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
bool Near(double a, double b) { return std::abs(a - b) < 1e-9; }
}

int main() {
    using namespace NpcHeroSize;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    // 彭猛 (user log 11-13, pid 23652: its head 252.3 cm over the hero's); a hero head at 160 cm here.
    const double peng = LookRatio(160, 412.3, 95, 240);
    Check(Near(peng, 412.3 / 160), "the head heights first");
    Check(Near(LookRatio(160, 0, 95, 190), 2.0) && Near(LookRatio(0, 0, 95, 190), 2.0), "no head: the pelvis heights");
    Check(LookRatio(0, 0, 0, 0) == 0 && LookRatio(nan, 300, nan, nan) == 0, "no bones, NaN: unknown");
    Check(Near(LookRatio(160, 5000, 95, 190), 2.0) && LookRatio(160, 10, 95, 9) == 0, "a head ratio out of range falls back to the pelvis; under 10 cm: unknown");
    Check(Near(Target(peng), 160 / 412.3) && Near(Target(0.8), 1.25), "shown at the hero's height over the look's: a big look smaller, a small one larger");
    Check(Target(0) == 1 && Target(nan) == 1 && Target(20) == 1 && Target(0.1) == 1, "an unknown ratio leaves the look at its own size");

    Check(Step(0, true, 100) == 0.5 && Step(0.5, true, 150) == 1 && Step(1, false, 50) == 0.75 && Step(0.75, false, 500) == 0, "200 ms in, 200 ms out");
    Check(Step(nan, true, 50) == 0.25 && Step(0.3, true, nan) == 0.3 && Step(0.3, true, -10) == 0.3 && Step(2, false, 0) == 1, "NaN, bad steps and out-of-range progress are safe");
    Check(Shown(0, 0.4) == 1 && Near(Shown(1, 0.4), 0.4) && Near(Shown(0.5, 0.4), 0.7), "smoothstep from the look's size to the target");
    Check(Shown(1, nan) == 1 && Shown(1, -2) == 1 && Shown(nan, 0.4) == 1, "a bad target or progress: the look's own size");
    double p = 0; int frames = 0;
    while (p < 1 && frames < 100) { p = Step(p, true, 1000.0 / 60); ++frames; }
    Check(frames >= 12 && frames <= 13 && Near(Shown(p, Target(peng)), 160 / 412.3), "at 60 fps the hero's size in 12-13 frames");
    double s = Shown(p, Target(peng)), last = s; bool steady = true;
    while (p > 0) { p = Step(p, false, 1000.0 / 60); s = Shown(p, Target(peng)); steady = steady && s >= last; last = s; }
    Check(steady && s == 1, "back to its own size without overshooting");

    Check(Near(Grow(peng, 1), peng) && Near(Grow(peng, Target(peng)), 1), "按体型比例: the look's ratio, 1 at the hero's size");
    Check(Near(Grow(0.8, 1), 0.8) && Near(Grow(0, 1, 2.45), 2.45) && Grow(0.1, 1) == 1 && Grow(nan, nan) == 1, "a small look brings the camera in; unknown: the fallback, else 1");
    Check(Grow(12, 2) == kGrowMax && Grow(0.25, 1) == kGrowMin, "clamped 0.4..12.5");

    // The game camera's lift. User log 11-13 pid 33952: 彭猛 head 427.3 / hero 175.0 cm, lift 252.3; 长弘 208.1 / 175.0.
    const double pm = LookRatio(175.0, 427.3, 109.2, 249.7), ch = LookRatio(175.0, 208.1, 109.2, 124.4);
    Check(Near(LiftAt(252.3, pm, 1), 252.3) && std::abs(LiftAt(252.3, pm, Target(pm))) < 1e-9, "彭猛: raised 252 cm at its own size, not at all at the hero's");
    Check(std::abs(LiftAt(33.1, ch, Target(ch))) < 1e-9 && std::abs(LiftAt(252.3, pm, Shown(0.5, Target(pm))) - (427.3 * Shown(0.5, Target(pm)) - 175.0)) < 0.01,
          "长弘 too; halfway, the head shown then over the hero's");
    Check(Near(LiftAt(-30, 0.8, 1), -30) && std::abs(LiftAt(-30, 0.8, Target(0.8))) < 1e-9, "a small look's lowered camera comes back up at the hero's size");
    Check(LiftAt(40, 0, 0.5) == 40 && LiftAt(40, 1.01, 0.5) == 40 && LiftAt(40, pm, nan) == 40 && LiftAt(nan, pm, 1) == 0, "unknown ratio, ratio near 1, bad scale: as it is; NaN lift: none");
    std::printf("npc_hero_size_test checks=%d failures=%d\n", checks, failures);
    return failures ? 1 : 0;
}
