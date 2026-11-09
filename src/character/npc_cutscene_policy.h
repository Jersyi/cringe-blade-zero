#pragma once
#include <cstdint>

namespace NpcCutscene {
// A new actor/mesh/animation generation gets its own retry window. Transient
// initialization failures may recover; a persistently unsupported host must
// not rebuild expensive components every maintenance tick.
struct Retry {
    unsigned failures = 0;
    uint64_t next = 0;
    bool Ready(uint64_t now) const { return failures < 4 && now >= next; }
    void Failed(uint64_t now) { if (failures < 4) ++failures; next = now + (uint64_t{500} << (failures ? failures-1 : 0)); }
    void Reset() { *this = {}; }
};
inline int Rank(bool shown, bool animated, bool bound, int kind, bool current) {
    if (!shown || !animated || kind < 0) return -1;
    return (bound ? 100 : 0) + kind*2 + (current ? 1 : 0);
}
// Sequencer can report a short gap between subsequences. Keep the existing
// live host through that gap, but never keep a destroyed/replaced component.
struct Playback {
    uint64_t last_playing = 0;
    bool seen = false;
    bool Hold(bool playing, bool host_alive, uint64_t now) {
        if (playing) { seen=true; last_playing=now; }
        return host_alive && (playing || (seen && now-last_playing < 750));
    }
    void Reset() { *this = {}; }
};
}
