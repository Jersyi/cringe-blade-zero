#pragma once
#include <cstdint>
#include <cmath>
#include <algorithm>

namespace ViewFollowState {
// A node number is not a cast identity: loops may immediately replay that node.
inline bool SameCast(int old_node,uint64_t old_visit,int64_t old_handle,uint64_t old_start,
                     int node,uint64_t visit,int64_t handle,uint64_t start){
    if(old_node!=node||(node>=0&&old_visit!=visit))return false;
    if(node<0||(old_handle>0&&handle>0))return old_handle==handle&&old_start==start;
    return true; // loading -> active or active -> blend-out within one visit
}
struct Lease {
    uint64_t refresh_at=0,retry_at=0;bool dirty=true,failed=false;
    void Invalidate(){dirty=true;failed=false;retry_at=0;}
    bool Due(uint64_t now,bool changed,bool nonzero)const{
        return !(failed&&now<retry_at)&&(dirty||changed||(nonzero&&now>=refresh_at));
    }
    void Sent(uint64_t now,bool ok){
        failed=!ok;retry_at=now+100;
        if(ok){dirty=false;refresh_at=now+100;}
    }
};
inline bool IsPoseJump(double position,double destination){
    return std::isfinite(destination)&&destination>=0&&
        !(position>=0&&std::isfinite(position)&&std::abs(destination-position)<0.1);
}
// Pose displacement, not camera-to-body distance, drives the pivot. A jump can
// bring the body close to the eye in open air; that is not a shortened camera arm.
template<class Vector> Vector DesiredOffset(double dx,double dy,double dz){
    constexpr double dead=25,rise=35,limit=1200;
    Vector goal{};const double flat=std::sqrt(dx*dx+dy*dy);
    if(flat>dead){const double k=((std::min)(flat,limit)-dead)/flat;goal.x=dx*k;goal.y=dy*k;}
    if(std::abs(dz)>rise)goal.z=(dz>0?1:-1)*((std::min)(std::abs(dz),limit)-rise);
    return goal;
}
// Only a confirmed trace hit restricts this frame's pivot. No persistent ceiling:
// once the obstruction clears the next frame can follow the complete pose again.
template<class Vector> Vector ObstacleLimited(Vector goal,double hit_distance){
    const double len=std::sqrt(goal.x*goal.x+goal.y*goal.y+goal.z*goal.z);
    if(len<=0)return goal;
    const double keep=std::clamp((hit_distance-40)/len,0.0,1.0);
    return {goal.x*keep,goal.y*keep,goal.z*keep};
}
}
