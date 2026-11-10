#pragma once
#include <cstdint>
#include <cmath>
#include <vector>

namespace ComboPlayback {
// An implicit same-trigger restart waits for completion, not a derivation point.
// Explicit wires (including loops) never use this gate.
inline bool RestartFinished(bool starting,bool group_has_next,bool ability_active,bool action_busy){
    return !starting&&!group_has_next&&!ability_active&&!action_busy;
}
// One visit to an outgoing path. Loading/animation before the derivation gate
// does not spend a branch's pause or an interval. Re-entering the same node
// creates a new visit; input history remains a separate concern.
struct NodeClock {
    uint64_t reached=0;bool open=false;
    void Reset(){*this={};}
    void Reach(uint64_t now){if(!open){reached=now;open=true;}}
    uint64_t Arrival(int prefix)const{return reached+static_cast<uint64_t>(prefix>0?prefix:0);}
    int Elapsed(uint64_t now,int prefix=0)const{
        const uint64_t at=Arrival(prefix);if(!open||now<at)return -1;
        return static_cast<int>((now-at)>0x7fffffff?0x7fffffff:now-at);
    }
    uint64_t Due(uint64_t press,int total,int prefix,int pause,bool branch)const{
        if(!open)return UINT64_MAX;
        if(!branch)return Arrival(total);
        const uint64_t ready=Arrival(prefix)+static_cast<uint64_t>(pause>0?pause:0);
        return (press>ready?press:ready)+static_cast<uint64_t>(total>prefix?total-prefix:0);
    }
    uint64_t Deadline(uint64_t ended,int reset,int prefix,int pause,int grace)const{
        const uint64_t delay=static_cast<uint64_t>(pause>0?pause:0);
        const uint64_t window=static_cast<uint64_t>(pause>0&&grace>reset?grace:reset);
        const uint64_t path=Arrival(prefix)+delay+window,finish=ended+static_cast<uint64_t>(reset);
        return path>finish?path:finish;
    }
    bool BufferExpired(uint64_t now,uint64_t press,int keep,int total,int prefix,int pause,bool branch)const{
        if(keep<=0)return false;
        // A plain pre-input still has a finite lifetime while an animation
        // cannot yield. Intentional path delays start only on arrival.
        if(total<=0&&pause<=0)return now>press+static_cast<uint64_t>(keep);
        if(!open)return false;
        const uint64_t due=Due(press,total,prefix,pause,branch);
        return now>(due>press?due:press)+static_cast<uint64_t>(keep);
    }
};
// Markers are montage coordinates, independent of dilation/curve speed. Each is
// consumed once per cast, including points crossed while the input was released.
struct ChargePoints {
    size_t next=0;double at=-1;bool holding=false;
    bool Step(const std::vector<int>& points,double position,double from,double end,bool down){
        if(holding){if(down)return true;holding=false;return false;}
        while(next<points.size()){
            const double point=points[next]/1000.0;
            if(point<from||point>=end){++next;continue;}
            if(position+0.00001<point)break;
            ++next;if(down){at=point;holding=true;return true;}
        }return false;
    }
};
// Native actions share the next-move gate, without cutting paired grabs or loading moves.
inline bool HeroMayCut(bool running,bool paired,bool interrupt,int keep,bool next_ready) {
    return running&&!paired&&(interrupt||keep>0||next_ready);
}
// A native press can wait for the same gate as a chain press. It belongs to one cast:
// a restart of the same node, timeout or newer chain press must never cut another move.
struct NativeInput {
    uint64_t cast=0,at=0; int node=-1; unsigned bits=0;
    void Arm(uint64_t token,int source,unsigned input,uint64_t now) { cast=token;node=source;bits=input;at=now; }
    unsigned Take(uint64_t token,int source,uint64_t now,int keep,bool ready) {
        if(cast!=token||node!=source||(keep>0&&now>at+static_cast<uint64_t>(keep))) { *this={};return 0; }
        if(!ready)return 0;
        const unsigned result=bits;*this={};return result;
    }
};
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
