#include "view_follow_state.h"
#include <cstdio>
#include <limits>

int main(){
    int checks=0,failures=0;
    const auto check=[&](bool ok,const char* why){++checks;if(!ok){++failures;std::printf("FAIL %s\n",why);}};
    using namespace ViewFollowState;
    check(SameCast(5,10,0,0,5,10,41,1000),"loading-to-active keeps the captured pre-seek baseline");
    check(SameCast(5,10,41,1000,5,10,0,0),"blend-out does not become a different cast");
    check(!SameCast(5,10,41,1000,5,11,41,1000),"same-node loop is a new camera visit");
    check(!SameCast(5,10,41,1000,5,10,42,1001),"replacement cast cannot retain old baseline");
    check(!SameCast(-1,0,41,1000,-1,0,41,1200),"reused native ability handle still starts a new camera session");
    check(!SameCast(5,10,41,1000,6,10,41,1000),"different node starts a new session");
    Lease lease;
    check(lease.Due(0,false,false),"new camera receives even an initial zero offset");
    lease.Sent(0,true);
    check(!lease.Due(99,false,true)&&lease.Due(100,false,true),"constant nonzero offset is restored within one hundred milliseconds");
    check(!lease.Due(1000,false,false),"stationary zero offset has no periodic writes");
    lease.Sent(100,true);check(lease.Due(101,true,true),"body motion is followed immediately");
    lease.Sent(101,false);
    check(!lease.Due(200,true,true)&&lease.Due(201,true,true),"failed native setter retries without a per-frame failure storm");
    lease.Invalidate();check(lease.Due(102,false,true),"replacement camera bypasses old failure backoff");
    lease.Sent(102,true);
    check(IsPoseJump(.03,4.55),"cropped start captures pre-seek baseline");
    check(!IsPoseJump(4.56,4.55),"small charge-point correction does not invalidate camera lease");
    check(IsPoseJump(4.55,.9),"rewind and replay capture baseline too");
    check(!IsPoseJump(1,std::numeric_limits<double>::quiet_NaN()),"invalid seek cannot poison state");
    check(!IsPoseJump(1,-1),"negative destination is not a pose jump");
    check(IsPoseJump(-1,1.8),"unknown source position still captures before seeking");
    struct Vec {double x=0,y=0,z=0;};
    const auto near=[](double a,double b){return std::abs(a-b)<1e-8;};
    // User log, pid 18068, node 14, two trimmed MoTian Skill_06 visits:
    // rise 726/730 cm, zero obstacle hits, old heuristic latched at 167/192 cm.
    // These are the production pivot calculations, not rendered-camera acceptance.
    const auto first=DesiredOffset<Vec>(16,0,726),second=DesiredOffset<Vec>(18,0,730);
    check(near(first.z,691)&&near(second.z,695),"logged open-air leaps retain full rise beyond the sway dead zone");
    check(first.x==0&&second.x==0,"small horizontal sway remains filtered during vertical leap");
    const auto still=DesiredOffset<Vec>(15,20,35),below=DesiredOffset<Vec>(0,0,-730);
    check(still.x==0&&still.y==0&&still.z==0,"dead-zone boundary stays stationary");
    check(near(below.z,-695),"downward displaced poses follow symmetrically");
    const auto lunge=DesiredOffset<Vec>(300,400,0),far=DesiredOffset<Vec>(0,2000,2000);
    check(near(lunge.x,285)&&near(lunge.y,380),"horizontal movement retains direction and twenty-five centimetre margin");
    check(near(far.y,1175)&&near(far.z,1165),"large displacement keeps the existing twelve metre safety range");
    const auto roof=ObstacleLimited(second,232);
    check(near(roof.z,192),"confirmed roof hit still clips pivot forty centimetres before contact");
    const auto diagonal=ObstacleLimited(lunge,140);
    check(near(diagonal.x,60)&&near(diagonal.y,80),"diagonal obstruction preserves direction");
    check(ObstacleLimited(second,20).z==0,"close obstruction never reverses offset");
    check(near(ObstacleLimited(second,1000).z,695),"trace imprecision cannot extend offset past desired pivot");
    check(ObstacleLimited(Vec{},100).z==0,"zero offset has no divide-by-zero");
    for(int frame=0;frame<120;++frame){
        // A short obstruction and a subsequent high held pose. Clearing the hit
        // must immediately release the target even well beyond the old 250ms guard.
        auto goal=DesiredOffset<Vec>(18,0,730);
        const bool hit=frame>=15&&frame<30;
        if(hit)goal=ObstacleLimited(goal,232);
        check(near(goal.z,hit?192:695),"obstacle removal never leaves a persistent follow ceiling");
    }
    // Replay a stable high camera offset after a cropped leap. The native camera
    // clears its additives on several mode changes while the body does not move.
    lease={};double camera=0;uint64_t reset_at=0;int writes=0;
    for(uint64_t now=0;now<=6000;now+=10){
        if(now==310||now==1570||now==4110){camera=0;reset_at=now;}
        if(lease.Due(now,false,true)){camera=263;lease.Sent(now,true);++writes;}
        check(!reset_at||now<reset_at+100||camera==263,"stable trimmed pose regains camera follow after native mode clears offset");
    }
    check(writes==61,"stable recovery stays bounded at ten refreshes per second");
    // Component replacement starts with no additive; failed first acquisition
    // must not permanently disable this visit's camera following.
    lease.Sent(7000,false);lease.Invalidate();camera=0;
    if(lease.Due(7010,false,true)){camera=263;lease.Sent(7010,true);}
    check(camera==263,"late component acquisition restores current offset without rebasing the pose");
    for(uint64_t visit=20;visit<1020;++visit)
        check(!SameCast(5,visit,41,1000,5,visit+1,41,1000),"loop visits never carry the old trimmed pose baseline");
    // 取景: the model's height without the culling widening. User log 11-13 pid 23652: 彭猛 (PengMengSeq, no physics
    // asset, BoundsScale 2) bounds 981 cm, framed from 2267 cm; 左殇：真龙之躯 (scale 1) 238 cm.
    check(near(ModelHeight(490.5,2.0),490.5)&&near(ModelHeight(119,1),238),"彭猛's doubled bounds halved; a scale of 1 kept");
    check(near(ModelHeight(119,0),238)&&near(ModelHeight(119,std::numeric_limits<double>::quiet_NaN()),238)&&near(ModelHeight(119,-2),238),"unread, NaN or negative scale counts as 1");
    check(ModelHeight(0,1)==180&&ModelHeight(std::numeric_limits<double>::quiet_NaN(),1)==180,"no bounds: 180 cm");
    check(ModelHeight(40,1)==120&&ModelHeight(5000,1)==2500&&ModelHeight(5000,2)==2500,"clamped to 1.2..25 m");
    {   // 主角体型 (user log 11-13 pid 33952): 彭猛 shown at x0.409 kept the 490 cm framing, 435 cm x2.45 = 1068 cm.
        const double shown=175.0/427.3,own=ModelHeight(490.5*shown,2.0,shown);
        check(near(own,490.5)&&near(ModelHeight(490.5,2.0,1),490.5),"measured while shrunk: still its own height");
        const double grow=(std::max)(1.0,own*shown/200),before=(std::max)(1.0,own/200);
        check(std::abs(435*before-1068)<2&&435*grow<440,"framed at the hero's size: about the game camera's 435 cm, not 10.7 m");
        check(near(ModelHeight(119,1,0),238)&&near(ModelHeight(119,1,std::numeric_limits<double>::quiet_NaN()),238),"an unread shown scale counts as 1");
    }
    {   // The camera's push-out (DriveViewFollow: the game camera's relation, 462 cm in that log, x max(1, height / 200 cm)).
        const double native=462,before=native*(std::max)(1.0,ModelHeight(490.5,1)/200),after=native*(std::max)(1.0,ModelHeight(490.5,2)/200);
        check(std::abs(before-2266)<1&&std::abs(after-1133)<1,"彭猛 framed from 11.3 m (half its height of view, as the hero), not 22.7 m");
        check(near(native*(std::max)(1.0,ModelHeight(85,1)/200),native),"a model up to 2 m keeps the game camera's relation");
    }
    // 取景: axes round trip, distance by size, relation pushed out along itself, never looking up from below.
    for(int yaw=-180;yaw<=180;yaw+=45)for(int pitch=-60;pitch<=60;pitch+=30){
        const auto a=ViewAxes<Vec>(yaw,pitch);const Vec w{123,-45,67};const Vec back=ToWorld(a,ToLocal(a,w));
        check(near(back.x,w.x)&&near(back.y,w.y)&&near(back.z,w.z),"view axes convert world and local both ways");
        check(near(Length(a.f),1)&&near(Length(a.u),1)&&std::abs(Dot(a.f,a.u))<1e-9,"view axes are orthonormal");
    }
    check(near(FrameDistance(180,90,16.0/9.0,0.6),180/(2*0.5625*0.6)),"frame distance fills 60% of the view height");
    check(FrameDistance(800,90,16.0/9.0,0.6)>FrameDistance(250,90,16.0/9.0,0.6)*3,"a bigger model is framed from farther away");
    check(near(FrameDistance(180,0,0,0),FrameDistance(180,90,16.0/9.0,0.6)),"bad fov, aspect or fill fall back to defaults");
    {   const Vec native{-350,40,90};
        const Vec same=FrameRelation(native,200);check(near(same.x,-350)&&near(same.z,90),"a model the native camera already frames keeps its framing");
        const Vec pushed=FrameRelation(native,1200);check(near(Length(pushed),1200)&&near(pushed.y/pushed.x,native.y/native.x),"a big model pushes the eye out along the native relation");
        const Vec none=FrameRelation(Vec{1,0,0},500);check(near(none.x,-500)&&none.z==0,"no native relation: straight behind");
        const Vec low=LookDownAtLeast(Vec{-400,0,-300},10);check(near(low.z,400*std::tan(10*3.14159265358979323846/180)),"an eye below the body is raised to look down");
        const Vec high=LookDownAtLeast(Vec{-400,0,200},10);check(near(high.z,200),"an eye already above stays");
    }
    std::printf("{\"checks\":%d,\"failures\":%d,\"runtime\":false}\n",checks,failures);
    return failures?1:0;
}
