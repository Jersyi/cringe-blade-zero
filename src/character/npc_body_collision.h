#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// Hero body collision resized to an NPC skin, and the skin's skill cycle. Pure math, tested offline
// (npc_pose_transfer_test); npc_retarget.cpp reads and writes the live components.
namespace NpcBodyCollision {
// The movement capsule the skin asks for: its combat blueprint's own capsule when readable and within
// [low, high] x the hero's half-height, else the hero capsule scaled by the measured stature ratio
// (clamped). ratio sizes the other body shapes.
struct CapsulePlan { double radius=0,half=0,ratio=1;const char* source="none";bool valid=false; };
inline CapsulePlan PlanCapsule(double hero_radius,double hero_half,double npc_radius,double npc_half,double stature_ratio,double low,double high) {
    CapsulePlan plan;
    if(!std::isfinite(hero_radius)||!std::isfinite(hero_half)||hero_radius<=1||hero_half<hero_radius||!(low>0)||!(high>=low))return plan;
    if(std::isfinite(npc_radius)&&std::isfinite(npc_half)&&npc_radius>1&&npc_half>=npc_radius){
        const double ratio=npc_half/hero_half;
        if(ratio>=low&&ratio<=high){plan.radius=npc_radius;plan.half=npc_half;plan.ratio=ratio;plan.source="blueprint";}
    }
    if(plan.half<=0&&std::isfinite(stature_ratio)&&stature_ratio>0){
        const double ratio=std::clamp(stature_ratio,low,high);
        plan.radius=hero_radius*ratio;plan.half=hero_half*ratio;plan.ratio=ratio;plan.source="stature";
    }
    if(plan.half<=0)return plan;
    plan.half=(std::max)(plan.half,plan.radius);plan.valid=true;return plan;
}
// What the hero keeps of it: the radius only. The game's crouch / skill-end code puts the half-height,
// the mesh offset and BaseTranslationOffset back to the class default on its own (the 2026-11-03 log:
// 60/120 back to 45/96 after every boss skill), so a taller capsule cannot hold and the body sank and
// the camera dropped with it. The radius never exceeds the hero's half-height, which would drag the
// half-height up with it.
inline double KeptRadius(const CapsulePlan& plan,double hero_half){return plan.valid?(std::min)(plan.radius,hero_half):0;}
// Another body shape (dodge / shield / touch volumes) keeps its place on the bigger or smaller body:
// its size scales by the capsule ratio, and its offset scales about the feet (pivot_z: the feet height
// in its parent's space, -half-height under the capsule, 0 under the mesh). On a socket only the size does.
struct Shape { double location[3]{},scale[3]{1,1,1}; };
inline Shape ScaleShape(const Shape& in,double ratio,bool on_socket,double pivot_z=0) {
    Shape out=in;
    for(size_t i=0;i<3;++i){out.scale[i]=in.scale[i]*ratio;if(on_socket)continue;const double pivot=i==2?pivot_z:0;out.location[i]=pivot+(in.location[i]-pivot)*ratio;}
    return out;
}
// L3 + R3 casts the skin's skills in random rounds: every skill once per round in a shuffled order,
// then a new shuffle, never starting with the skill that ended the last round. A skill the hero
// cannot start is only skipped for the rest of its round.
struct SkillRound {
    std::vector<size_t> order;size_t pos=0,last=SIZE_MAX;uint64_t state=0x9E3779B97F4A7C15ull;
    void Reset(uint64_t seed){order.clear();pos=0;last=SIZE_MAX;state=seed?seed:0x9E3779B97F4A7C15ull;}
    uint64_t Random(){state^=state>>12;state^=state<<25;state^=state>>27;return state*0x2545F4914F6CDD1Dull;} // xorshift64*
    size_t Next(size_t count){
        if(!count)return 0;
        if(order.size()!=count||pos>=count){
            order.resize(count);for(size_t i=0;i<count;++i)order[i]=i;
            for(size_t i=count-1;i>0;--i)std::swap(order[i],order[static_cast<size_t>(Random()%(i+1))]);
            if(count>1&&order[0]==last)std::swap(order[0],order[1+static_cast<size_t>(Random()%(count-1))]);
            pos=0;
        }
        last=order[pos++];return last;
    }
    size_t Left(size_t count)const{return order.size()==count&&pos<count?count-pos:0;}
};
}
