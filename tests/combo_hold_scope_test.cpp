#include "combo_hold_scope.h"
#include <cstdio>

int main(){
    int checks=0,failures=0;
    const auto check=[&](bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL %s\n",name);}};
    using ComboPlayback::HoldScope;
    const HoldScope hero{-1,24,200};
    check(hero.Matches(-1,24,200),"held input survives time passing in the same node visit");
    check(!hero.Matches(-1,24,201),"next node drops old hold");
    check(!hero.Matches(-1,25,200),"editing rows drops old indices even when node and owner match");
    check(!hero.Matches(4,24,200),"same index on another character is not the old chain");
    check(!HoldScope{}.Matches(-2,0,0),"unarmed scope cannot authorize input");
    // Simulate a held key spanning automatic group steps and looping through
    // the same node number. Only the originating visit may consume that press.
    int accepted=0;
    for(uint64_t visit=200;visit<1200;++visit){
        const bool current=hero.Matches(-1,24,visit);accepted+=current?1:0;
        check(current==(visit==200),"loop and automatic-step visits never replay an old hold");
    }
    check(accepted==1,"one pending hold authorizes at most its one visit");
    const HoldScope restarted{-1,24,1200};
    check(restarted.Matches(-1,24,1200)&&!hero.Matches(-1,24,1200),"a fresh press can arm after restart");
    std::printf("{\"checks\":%d,\"failures\":%d,\"runtime\":false}\n",checks,failures);
    return failures?1:0;
}
