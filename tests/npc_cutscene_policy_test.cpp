#include "npc_cutscene_policy.h"
#include "sequence_request.h"
#include "debug_supplement_catalog.generated.h"
#include <cstdio>
#include <cwchar>

int main() {
    int checks=0,failures=0;
    const auto check=[&](bool value,const char* name){++checks;if(!value){++failures;std::printf("FAIL %s\n",name);}};
    using namespace NpcCutscene;
    // Repeated shot changes, transient initialization failures, then a complete
    // cleanup/rebind. A failure storm is bounded, even after a long elapsed time.
    for(int scene=0;scene<100;++scene){
        Retry retry;uint64_t now=100000;
        for(unsigned attempt=0;attempt<4;++attempt){
            check(retry.Ready(now),"a ready generation may be applied");retry.Failed(now);
            check(!retry.Ready(now),"never rebuild immediately on a failed tick");
            for(uint64_t tick=now;tick<retry.next;tick+=250)check(!retry.Ready(tick),"respect backoff");
            now=retry.next;
        }
        check(!retry.Ready(now+60000),"persistent incompatibility stops at four attempts");
        retry.Reset();check(retry.Ready(now),"a new actor/mesh/anim generation is eligible");
        Playback playback;
        check(!playback.Hold(false,true,now),"no stale scene before first playback");
        check(playback.Hold(true,true,now),"active cinematic retained");
        check(playback.Hold(false,true,now+500),"short shot gap retained");
        check(!playback.Hold(false,true,now+750),"finished cinematic releases host");
        check(!playback.Hold(true,false,now+800),"destroyed actor never retained by grace");
        playback.Reset();check(!playback.Hold(false,true,now+810),"new world has no previous grace");
    }
    check(Rank(true,true,true,1,false)>Rank(true,true,false,3,true),"bound hero beats an unrelated surviving cinematic copy");
    check(Rank(true,true,true,3,true)>Rank(true,true,true,3,false),"same shot keeps its current host on ties");
    check(Rank(false,true,true,3,true)<0&&Rank(true,false,true,3,true)<0,"hidden/uninitialized actors are not candidates");
    check(Rank(true,true,false,-1,false)<0,"an unrelated actor sharing a skeleton is not a fallback hero");
    int catalog=0;
    for(const auto& item:DebugSupplementCatalog::kItems){
        if(std::wcscmp(item.category,L"播放Seq")||!*item.arg)continue;
        SupplementSequence::Key key;
        check(SupplementSequence::Parse(item.arg,key),"every authored row/actor/node key parses");++catalog;
    }
    check(catalog==280,"complete cinematic catalog covered");
    std::printf("checks=%d failures=%d catalog=%d\n",checks,failures,catalog);return failures?1:0;
}
