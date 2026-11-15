#pragma once
#include <cmath>
#include <cstddef>

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
// A Main-slot switch with Armed=false (GAA_SwitchWeapon) selects a config without drawing it. It puts the Boss's weapon
// away only when the Boss has that config: SetCurrentWeapon refuses an index past the slot and changes nothing (in-game
// probe, 11-15), so the Boss keeps what it holds. 沐小葵 01 switches to config 2 and the cloaked first phase has two
// configs: she keeps her sword. boss_configs: the Boss's Main configs (0: not known, taken as having it).
inline bool PutsAway(bool armed, int start_weapon, size_t boss_configs) {
    return !armed && start_weapon >= 0 && (boss_configs == 0 || static_cast<size_t>(start_weapon) < boss_configs);
}
// One part (primitive component) of the hero's own weapons while a grab holds it hidden (GrabExtras::HideWeapons). It goes
// back at the end to the hidden flag it had when the grab first saw it, unless something shows it while the grab holds it
// (another module's own restore: a boss skill's, a look change's): then it is shown at the end. Seen: the flag as read
// this tick; true when the grab must hide it again now.
struct HeldPart {
    bool restore_hidden = false;
    bool Seen(bool hidden_now) {
        if (hidden_now) return false;
        restore_hidden = false;
        return true;
    }
};
// An authored sheathe takes precedence over the idle-start exception. Native
// ForceWeaponVisibility remains authoritative even outside a switch window.
inline bool Show(bool unhidden, bool armed, bool armed_at_begin, bool native_sheathe, int ticks) {
    return unhidden && !native_sheathe && (armed || !armed_at_begin) && ticks >= 2;
}
}
