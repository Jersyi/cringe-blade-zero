#pragma once
#include <cstdint>
#include <cmath>

namespace ComboPlayback {
// Owned by one tracked actor. Maintenance calls (zero height) never consume a lift.
// Token zero is reserved for explicit diagnostic commands, which may be repeated.
struct LiftOnce {
    uint64_t token=0; int node=-1; bool hero=false,used=false;
    bool Take(uint64_t cast,int source,bool self,double height) {
        if (height<=0.5) return false;
        if (cast&&used&&token==cast&&node==source&&hero==self) return false;
        token=cast; node=source; hero=self; used=true; return true;
    }
};
// Lego's timeline advances in skill time; montage position includes its play rate.
inline float TrimClock(float clock,float position,double from,double rate) {
    const double r=std::isfinite(rate)&&rate>0.01?rate:1.0;
    return static_cast<float>(clock+(from-(position>=0?position:0))/r);
}
}
