#include "combo_playback_rules.h"
#include <cstdio>
#include <initializer_list>
#include <limits>

int main() {
    int checks=0,failures=0;
    const auto check=[&](bool ok,const char* label) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n",label); } };
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
