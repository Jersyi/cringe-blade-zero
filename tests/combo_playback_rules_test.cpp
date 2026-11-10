#include "combo_playback_rules.h"
#include "npc_combo_model.h"
#include <cstdio>
#include <initializer_list>
#include <limits>

int main() {
    int checks=0,failures=0;
    const auto check=[&](bool ok,const char* label) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n",label); } };
    {
        using ComboPlayback::RestartFinished;
        for(unsigned state=0;state<16;++state)
            check(RestartFinished(state&1,state&2,state&4,state&8)==(state==0),"implicit restart requires full completion of loading, group, ability and movement");
        ComboPlayback::NodeClock restart;
        for(int presses=0;presses<100;++presses){
            // The captured failure: a 437 ms tail already allows derivation,
            // but the tracked ability remains active. Repeated input cannot
            // open the independent restart clock.
            if(RestartFinished(false,false,true,false))restart.Reach(1437+presses);
            check(!restart.open,"mashing cannot turn a tail restart into an ordinary derivation");
        }
        check(restart.BufferExpired(1801,1437,300,0,0,0,false),"pending restart still honors pre-input expiry");
        restart.Reset();check(!RestartFinished(false,false,true,false)&&!restart.open,"expiry and a new press still wait for the original tail");
        if(RestartFinished(false,false,false,false))restart.Reach(2200);
        check(restart.open&&restart.Due(2100,400,0,0,false)==2600,"trigger interval begins only after tail completion");
        check(!RestartFinished(false,true,false,false),"completed group step does not restart before remaining steps");
        check(!RestartFinished(false,false,false,true),"charge, grab, teleport and hook cleanup must finish first");
        NpcCombo::Config chain;chain.chain.push_back(NpcCombo::TriggerNode(NpcCombo::Trigger::Light,0,0));
        chain.chain.push_back({NpcCombo::NodeType::Skill,"GA_First"});chain.chain.push_back({NpcCombo::NodeType::Skill,"GA_Tail"});
        chain.chain[0].next=1;chain.chain[1].next=2;const int light=static_cast<int>(NpcCombo::Trigger::Light);NpcCombo::Press press;press.bits=1u<<light;
        check(NpcCombo::Follow(chain,2,light,press,0).move<0&&NpcCombo::Follow(chain,0,light,press,0).move==1,"keeping tail cursor prevents second press from following the trigger directly");
        chain.chain[2].next=1;
        check(NpcCombo::Follow(chain,2,light,press,0).move==1,"an explicit loop remains a normal wired derivation");
    }
    {
        ComboPlayback::NodeClock clock;
        check(clock.BufferExpired(1401,1000,400,0,0,0,false),"plain pre-input expires during an uninterruptible animation");
        check(!clock.BufferExpired(1400,1000,400,0,0,0,false),"plain pre-input includes its deadline");
        check(!clock.BufferExpired(20000,1000,400,500,0,1000,true),"intentional pause cannot expire before arrival");
        check(clock.Elapsed(10000)==-1&&clock.Due(100,500,0,1000,true)==UINT64_MAX,"loading cannot spend branch pause or expire pre-input");
        clock.Reach(10000);clock.Reach(11000);
        check(clock.BufferExpired(10000,9000,400,0,0,0,false),"opening the derivation gate never revives expired plain input");
        check(clock.Elapsed(10500,500)==0&&clock.Elapsed(10499,500)==-1,"branch starts after the preceding interval");
        check(clock.reached==10000,"repeated polls never restart the current visit");
        check(clock.Due(9000,800,500,1000,true)==11800,"early buffered input waits prefix, pause and trailing interval sequentially");
        check(clock.Due(12000,800,500,1000,true)==12300,"late selection starts its trailing interval at selection");
        check(clock.Due(9000,800,0,0,false)==10800,"plain interval begins at outgoing gate, not skill start");
        check(clock.Deadline(11000,1200,500,1000,1500)==13000,"reset protects a pending branch pause and its input window");
        check(clock.Deadline(10000,500,3000,0,1500)==13500,"long interval cannot time out before node arrival");
        NpcCombo::Config graph;
        const int light=static_cast<int>(NpcCombo::Trigger::Light);
        graph.chain.push_back({NpcCombo::NodeType::Skill,"GA_A"});
        auto branch=NpcCombo::BranchNode(0,0);branch.routes={{light,0,2},{light,1000,3}};
        graph.chain.push_back(branch);graph.chain.push_back({NpcCombo::NodeType::Skill,"GA_B"});graph.chain.push_back({NpcCombo::NodeType::Skill,"GA_C"});graph.chain[0].next=1;
        NpcCombo::Press press;press.bits=1u<<light;
        clock.Reset();
        check(NpcCombo::Decide(graph,0,light,press,clock.Elapsed(50000)).hop.move==2,"a long playing move does not prematurely select its pause branch");
        clock.Reach(50000);
        check(NpcCombo::Decide(graph,0,light,press,clock.Elapsed(50999)).hop.move==2&&NpcCombo::Decide(graph,0,light,press,clock.Elapsed(51000)).hop.move==3,"pause branch opens exactly one second after arrival");
        clock.Reset();clock.Reach(60000);
        check(NpcCombo::Decide(graph,0,light,press,clock.Elapsed(60000)).hop.move==2,"looping back clears previously satisfied pause branch");
        for(int visit=0;visit<1000;++visit){
            clock.Reset();check(clock.Elapsed(20000+visit*100)==-1,"same-node loop never inherits the preceding pause");
            clock.Reach(20000+visit*100);check(clock.Elapsed(20000+visit*100)==0,"each visit begins at zero");
            check(clock.Deadline(20500+visit*100,1200,0,0,1500)==21700+visit*100,"each finished move receives a fresh reset window");
        }
    }
    {
        ComboPlayback::ChargePoints points;const std::vector<int> marks{100,500,900,1500,2000};
        check(!points.Step(marks,.499,.2,2,true),"trim skips markers before the kept range");
        check(points.Step(marks,.6,.2,2,true)&&points.at==.5,"speed overshoot catches first marker at its authored time");
        for(int i=0;i<1000;++i)check(points.Step(marks,.5,.2,2,true)&&points.at==.5,"holding never advances the marker");
        check(!points.Step(marks,.5,.2,2,false),"release resumes once");
        check(!points.Step(marks,1.0,.2,2,false),"unheld marker is passed rather than armed later");
        check(points.Step(marks,1.6,.2,2,true)&&points.at==1.5,"another marker can hold in the same cast");
        check(!points.Step(marks,1.5,.2,2,false)&&!points.Step(marks,2,.2,2,true),"end marker never prevents trimmed end");
        points={};check(points.Step(marks,.5,.2,2,true),"new cast re-arms markers");
        for(double rate:{.5,1.,2.,10.}){
            NpcCombo::TrimRange range{true,1,3,{1500,2500}};ComboPlayback::ChargePoints run;
            double position=1;int stops=0;double frozen_progress=-1;
            while(position<3){const bool held=run.Step(range.holds,position,range.from,range.to,true);
                if(held){++stops;position=run.at;frozen_progress=NpcCombo::TrimProgress(range,position,4);
                    for(int tick=0;tick<20;++tick)check(run.Step(range.holds,position,1,3,true)&&NpcCombo::TrimProgress(range,position,4)==frozen_progress,"curve progress is frozen while charging");
                    run.Step(range.holds,position,1,3,false);
                }
                const double speed=rate*NpcCombo::CurveSpeed({{0,50},{100,200}},NpcCombo::TrimProgress(range,position,4))/100.;position+=speed/120.;
            }
            check(stops==2,"both authored points survive fixed speed and acceleration curve");
        }
    }
    using ComboPlayback::HeroMayCut;
    check(!HeroMayCut(true,false,false,0,false),"native action waits before the next-move gate");
    check(HeroMayCut(true,false,false,0,true),"native dodge opens at the next-move gate without a modifier");
    check(HeroMayCut(true,false,true,0,false)&&HeroMayCut(true,false,false,500,false),"explicit interrupt and keep-chain still open early");
    check(!HeroMayCut(false,false,true,500,true)&&!HeroMayCut(true,true,true,500,true),"loading moves and paired grabs cannot be cut by native input");
    ComboPlayback::NativeInput input;
    input.Arm(100,19,0x80,1000);
    check(!input.Take(100,19,1100,300,false),"early dodge remains buffered while gate is closed");
    check(input.Take(100,19,1300,300,true)==0x80,"buffered dodge is accepted on the inclusive deadline");
    check(!input.Take(100,19,1301,300,true),"one dodge never cancels twice");
    input.Arm(100,19,0x80,1000);
    check(!input.Take(100,19,1301,300,true),"expired dodge cannot cut a later recovery");
    input.Arm(100,19,0x80,1000);
    check(!input.Take(101,19,1100,300,true),"same node cast again does not inherit a pending dodge");
    input.Arm(100,19,0x80,1000);
    check(!input.Take(100,20,1100,300,true),"another node does not inherit a pending dodge");
    input.Arm(100,19,0x80,1000);input.Arm(100,19,0x10,1100);
    check(input.Take(100,19,1400,0,true)==0x10,"latest native skill wins and unlimited pre-input remains supported");
    using namespace NpcCombo;
    Config chain;
    // Use explicit nodes, independent of serialized text versions.
    chain.chain.resize(3);chain.chain[0].type=NodeType::Trigger;chain.chain[0].trigger=static_cast<int>(Trigger::Light);chain.chain[0].next=1;
    chain.chain[1].type=NodeType::Skill;chain.chain[1].ability="GA_Test_A";chain.chain[1].next=2;
    chain.chain[2].type=NodeType::Skill;chain.chain[2].ability="GA_Test_B";chain.chain[2].next=-1;
    const int light=static_cast<int>(Trigger::Light);
    int cursor=1;
    for(unsigned bits:{0x80u,1u<<static_cast<int>(Trigger::Special),1u<<static_cast<int>(Trigger::CombatArt)}) {
        Press native;native.bits=bits;
        check(Follow(chain,cursor,light,native,200).move<0,"unmapped native action has no chain branch");
        if(!NativeDetour(native,cursor>=0))cursor=-1;
        check(cursor==1,"native dodge or skill preserves the last chain node");
    }
    Press attack;attack.bits=1u<<light;
    check(Follow(chain,cursor,light,attack,500).move==2,"next attack resumes at B rather than restarting A");
    chain.chain[1].ms=450;
    for(uint64_t time:{449u,450u,451u})
        check(HeroMayCut(true,false,false,0,time>=static_cast<uint64_t>(CutMs(chain,1)))==(time>=450),"native cancel uses the configured move gate boundary");
    Node fork=BranchNode(0,0);fork.routes={{static_cast<int>(Trigger::Dash),0,2}};
    chain.chain.push_back(fork);chain.chain[1].next=3;
    Press dodge;dodge.bits=0x80;
    check(Follow(chain,1,light,dodge,500).move==2,"an explicitly wired dodge still routes to its skill before native fallback");
    Press explicit_key;explicit_key.bits=0x80;explicit_key.combo=0;
    check(!NativeDetour(explicit_key,true),"explicit custom-key trigger keeps its routing semantics");
    explicit_key.combo=-1;explicit_key.branch=1;
    check(!NativeDetour(explicit_key,true),"explicit graph row is not a native detour");
    check(!NativeDetour(attack,false),"completed chain is not kept indefinitely");
    ComboPlayback::LiftOnce hero,foe;
    check(!hero.Take(10,2,true,0),"maintenance does not consume a lift");
    check(hero.Take(10,2,true,80),"first lift accepted");
    for (int i=0;i<2000;++i) check(!hero.Take(10,2,true,80),"same cast never re-launches");
    check(foe.Take(10,2,false,300),"another actor has an independent lift");
    check(!foe.Take(10,2,false,0),"repeat hit maintains hold only");
    check(!foe.Take(10,2,false,500),"same cast cannot stack a larger launch");
    check(hero.Take(11,2,true,80),"next cast of same node can lift");
    check(hero.Take(11,3,true,80),"different node has independent identity");
    check(hero.Take(0,-1,true,80)&&hero.Take(0,-1,true,80),"diagnostic commands remain repeatable");
    for (double rate:{0.5,1.0,2.0,10.0}) {
        const float clock=ComboPlayback::TrimClock(0.3f,0.3f,1.4,rate);
        check(std::fabs(0.3+(clock-0.3)*rate-1.4)<0.00001,"timeline and montage reach trimmed start together");
        check(std::fabs(ComboPlayback::TrimClock(clock,1.4f,0,rate)-(clock-1.4/rate))<0.00001,"editing start backwards or clearing trim rewinds");
    }
    check(std::isfinite(ComboPlayback::TrimClock(0,0,1,std::numeric_limits<double>::quiet_NaN())),"invalid play rate uses safe default");
    std::printf("{\"checks\":%d,\"failures\":%d,\"duplicate_calls\":2000,\"runtime\":false}\n",checks,failures);
    return failures?1:0;
}
