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
    std::printf("{\"checks\":%d,\"failures\":%d,\"runtime\":false}\n",checks,failures);
    return failures?1:0;
}
