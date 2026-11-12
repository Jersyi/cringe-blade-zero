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
// 取景: where the eye goes for the shown model. Axes of a view rotation (degrees): forward, right, up.
template<class V> struct Axes { V f,r,u; };
template<class V> Axes<V> ViewAxes(double yaw_deg,double pitch_deg){
    constexpr double k=3.14159265358979323846/180.0;
    const double cy=std::cos(yaw_deg*k),sy=std::sin(yaw_deg*k),cp=std::cos(pitch_deg*k),sp=std::sin(pitch_deg*k);
    return {V{cp*cy,cp*sy,sp},V{-sy,cy,0},V{-sp*cy,-sp*sy,cp}};
}
template<class V> double Dot(const V& a,const V& b){return a.x*b.x+a.y*b.y+a.z*b.z;}
template<class V> double Length(const V& a){return std::sqrt(Dot(a,a));}
template<class V> V ToLocal(const Axes<V>& a,const V& w){return {Dot(w,a.f),Dot(w,a.r),Dot(w,a.u)};}
template<class V> V ToWorld(const Axes<V>& a,const V& l){
    return {a.f.x*l.x+a.r.x*l.y+a.u.x*l.z,a.f.y*l.x+a.r.y*l.y+a.u.y*l.z,a.f.z*l.x+a.r.z*l.y+a.u.z*l.z};
}
// How far away a model `size` cm tall fills `fill` of the view's height (hfov: horizontal field of view in degrees,
// aspect: width / height).
inline double FrameDistance(double size,double hfov,double aspect,double fill){
    constexpr double k=3.14159265358979323846/360.0;
    if(!(hfov>10&&hfov<170))hfov=90;
    if(!(aspect>0.3&&aspect<5))aspect=16.0/9.0;
    if(!(fill>0.05&&fill<=1))fill=0.6;
    const double half_v=std::tan(hfov*k)/aspect;
    return size>0?size/(2*half_v*fill):0;
}
// The eye's place from the body in the view's axes: the game camera's own relation (`native`, measured with no offset
// on) pushed out along itself to at least `distance`; a relation too short to have a direction is straight behind.
template<class V> V FrameRelation(const V& native,double distance){
    const double len=Length(native);
    if(len<50)return V{-(std::max)(distance,50.0),0,0};
    const double k=distance>len?distance/len:1;
    return {native.x*k,native.y*k,native.z*k};
}
// A world relation (eye - body) raised so the eye looks down on the body by at least min_down degrees: never from below.
template<class V> V LookDownAtLeast(V rel,double min_down_deg){
    const double flat=std::sqrt(rel.x*rel.x+rel.y*rel.y);
    const double lowest=flat*std::tan(min_down_deg*3.14159265358979323846/180.0);
    if(rel.z<lowest)rel.z=lowest;
    return rel;
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
