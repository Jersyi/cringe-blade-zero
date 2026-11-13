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
        // 预输入 is one rule for every path: before the derivation gate a press
        // lives `keep` ms from the press (from the move's start when it came
        // while the move loaded), whatever follows the move. A press still
        // alive when the gate opens waits out the intervals / branch pause after
        // the gate (they spend none of it) and expires `keep` ms after it could play.
        const uint64_t limit=press+static_cast<uint64_t>(keep);
        if(!open)return now>limit;
        if(reached>limit)return true;  // the gate opened too late: never revived
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
// 自定义按键 while a chain goes on: a hero action one of the keys brought (seen within `ms` of a watched key going
// down) that the chain does not take is part of those keys, not the hero's own action: it neither cuts the move
// playing nor drops the kept press, and the game's action from it is cancelled. Outside a chain 吞掉 decides.
struct KeyPart {
    uint64_t until=0;
    void Note(uint64_t now,uint64_t ms){until=now+ms;}
    bool Takes(uint64_t now,bool chain_on,bool leads)const{return chain_on&&!leads&&now<until;}
};
// Echoes. The game replays a press it held back during a chain move once, as that move (or the chain's move after it)
// ends. While a chain goes on, a hero action matching a press read from the input during its moves is that replay;
// once the chain is over with no chain after it, only within kEchoGraceMs of its last move's end. 11-12 (twice): a
// timeout reset 1.2 s after the last move, and the hero's own attack a second later was taken for the echo of a press
// made 2.4 s before, during that move, and cancelled: the attack did not come out.
// over_at: when the chain's moves stopped (its last move's end, else the reset); 0: no chain ended yet.
constexpr uint64_t kEchoGraceMs=400;
inline bool EchoOpen(bool chain_on,uint64_t now,uint64_t over_at){
    return chain_on||(over_at&&now<over_at+kEchoGraceMs);
}
// A hero action the chain takes: the other hero action abilities running with it that are cancelled with it. One input
// can start several (a dodge its dash family), and a perfect dodge starts while its dash still runs: cancelling only
// the one that brought the press left the rest playing beside the chain's move (the hero went on dodging under the
// boss move). fresh: started with the press (this frame); bits: its action family; pressed: the press's.
// A jump the hero is in stays unless the press is a jump (its rise and fall are the game's).
inline bool CancelWithPress(bool fresh,unsigned bits,unsigned pressed,unsigned dodges,unsigned jump){
    if(!bits)return false;
    if((bits&jump)&&!(pressed&jump))return false;
    return fresh||(bits&dodges)!=0;
}
// A dodge family action cancelled for the chain's move while no move of the chain is on the hero: its montage is
// stopped at once (one blending out still moves the hero by its root motion) and on the ground its speed goes.
inline bool StopDodgeMotion(unsigned cancelled,unsigned dodges,bool chain_move_on){
    return (cancelled&dodges)!=0&&!chain_move_on;
}
// A press the chain left to the game during a move (a native detour) and the borrowed look after that move. As the move
// ends the look stays only for the hero's own action of that press: one running then, or a move cut for it (允许中断);
// else it goes back as after any move. Later it stays while a montage plays that is not the move's own (`move_tail`:
// the one playing when the move ended). 11-13: a heavy attack pressed as a boss move finished kept the boss look on,
// held by that move's montage playing out, and the hero stood still until it ended ("按重击会罚站").
inline bool DetourKeepsLook(bool detour,bool cut_for_hero,bool hero_action){ return detour&&(cut_for_hero||hero_action); }
inline bool DetourHolds(bool montage,bool move_tail){ return montage&&!move_tail; }
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
