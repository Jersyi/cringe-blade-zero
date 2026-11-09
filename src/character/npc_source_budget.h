#pragma once

// Reversible scheduling policy for a hidden cutscene animation source. Its
// allocator membership, significance and interpolation choice remain native.
namespace NpcSourceBudget {
struct Policy {
    float significance=0;
    bool always=false,offscreen=false,reduced=false,interpolate=false;
};
inline bool Equal(const Policy& a,const Policy& b) {
    return a.significance==b.significance&&a.always==b.always&&a.offscreen==b.offscreen&&a.reduced==b.reduced&&a.interpolate==b.interpolate;
}
inline Policy Held(Policy p) {
    p.always=true;
    // AlwaysTickPoseAndRefreshBones already includes hidden sources. Avoid the
    // allocator's separately capped "tick even if not rendered" queue.
    p.offscreen=false; p.reduced=false; return p;
}
inline Policy Restored(const Policy& saved,Policy now) {
    // Restore only fields still holding our writes. Native significance and
    // interpolation changes are never rolled back.
    if (now.always) now.always=saved.always;
    if (!now.offscreen) now.offscreen=saved.offscreen;
    if (!now.reduced) now.reduced=saved.reduced;
    return now;
}
struct Lease {
    bool owned=false;
    Policy saved;
    template<class Read,class Write> bool Hold(Read read,Write write) {
        Policy now;
        if (!read(now)) return false;
        if (!owned) { saved=now; owned=true; } // Keep rollback even after a partial setter failure.
        const Policy want=Held(now);
        if (!Equal(now,want)&&!write(want)) return false;
        Policy after;
        return read(after)&&Equal(after,want);
    }
    template<class Read,class Write> bool Restore(Read read,Write write) {
        if (!owned) return true;
        Policy now;
        if (!read(now)) return false;
        const Policy want=Restored(saved,now);
        if (!Equal(now,want)&&!write(want)) return false;
        Policy after;
        if (!read(after)||!Equal(after,want)) return false;
        owned=false; return true;
    }
};
}
