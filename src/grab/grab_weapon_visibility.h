#pragma once
#include <cmath>

namespace GrabWeaponVisibility {
struct Window {
    float begin = 0, end = 0;
    bool Contains(float time) const {
        return std::isfinite(time) && std::isfinite(begin) && std::isfinite(end) &&
            begin >= 0 && end > begin && time >= begin && time < end;
    }
};
struct NativeState {
    bool sheathe_observed=false;
    bool Hidden(bool armed,bool authored_now){
        sheathe_observed=sheathe_observed||(!armed&&authored_now);
        return sheathe_observed&&!armed;
    }
};
// An authored sheathe takes precedence over the idle-start exception. Native
// ForceWeaponVisibility remains authoritative even outside a switch window.
inline bool Show(bool unhidden, bool armed, bool armed_at_begin, bool native_sheathe, int ticks) {
    return unhidden && !native_sheathe && (armed || !armed_at_begin) && ticks >= 2;
}
}
