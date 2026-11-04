// Grab damage per damage event (GrabExtras::HitExtra): what each of the grab's own hits adds.
#include "grab_extras.h"
#include <cmath>
#include <cstdio>

namespace {
int failures = 0;
void Check(bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
bool Near(float a, float b) { return std::fabs(a - b) < .01f; }
using GrabExtras::Damage; using GrabExtras::HitExtra;
}
int main() {
    // Multiplier: every hit deals drop x multiplier, the presets up to x30 included.
    Check(Near(HitExtra(Damage::Multiplier, 100, 4.5f, 30, 10000), 350), "x4.5 adds 3.5 times the hit");
    Check(Near(HitExtra(Damage::Multiplier, 100, 30, 30, 10000), 2900), "x30 adds 29 times the hit");
    Check(Near(HitExtra(Damage::Multiplier, 100, 50, 30, 10000), 2900), "a multiplier above x30 is capped at x30");
    Check(Near(HitExtra(Damage::Multiplier, 100, .5f, 30, 10000), -50), "below x1 gives part of the hit back");
    Check(Near(HitExtra(Damage::Multiplier, 100, 1, 30, 10000), 0), "x1 adds nothing");
    // Three hits of one grab are each scaled, so the grab deals the native total x multiplier.
    float total = 0; for (float hit : {120.f, 80.f, 384.f}) total += hit + HitExtra(Damage::Multiplier, hit, 4.5f, 30, 14875);
    Check(Near(total, 584 * 4.5f), "every damage event of one grab is multiplied");
    // Percent: each hit deals at least that share of max health; a bigger native hit stays as it is.
    Check(Near(HitExtra(Damage::Percent, 100, 4.5f, 30, 10000), 2900), "30 % makes a 100 hit deal 3000 of 10000");
    Check(Near(HitExtra(Damage::Percent, 4000, 4.5f, 30, 10000), 0), "a hit above the percent floor is unchanged");
    Check(Near(HitExtra(Damage::Percent, 100, 4.5f, 30, 0), 0), "no max health, no percent floor");
    // Native, and no hit at all.
    Check(Near(HitExtra(Damage::Native, 100, 4.5f, 30, 10000), 0), "native mode adds nothing");
    Check(Near(HitExtra(Damage::Multiplier, 0, 4.5f, 30, 10000), 0), "no drop, no extra");
    Check(Near(HitExtra(Damage::Percent, -5, 4.5f, 30, 10000), 0), "a heal is not a hit");
    // Presets shown on the grab page.
    Check(std::size(GrabExtras::kMultiplierPresets) == 5 && GrabExtras::kMultiplierPresets[0] == 4.5f && GrabExtras::kMultiplierPresets[4] == 30.f &&
        GrabExtras::kMaxMultiplier == 30.f, "presets x4.5 / x10 / x15 / x20 / x30 within the x30 cap");
    std::printf("grab_damage_test failures=%d\n", failures);
    return failures ? 1 : 0;
}
