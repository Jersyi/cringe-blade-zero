#include "npc_display_pool.h"
#include <cstdint>
#include <cstdio>

struct Bundle {int key=0,world=1;uint64_t used=0;int owned=3;bool can_destroy=true;};
int main(){
    int checks=0,failures=0;const auto check=[&](bool ok,const char* message){++checks;if(!ok){++failures;std::fprintf(stderr,"FAIL %s\n",message);}};
    NpcDisplayPool::Store<Bundle> pool;int destroyed=0;
    const auto drop=[&](Bundle& b){if(!b.can_destroy)return false;destroyed+=b.owned;b.owned=0;return true;};
    for(int i=0;i<4;++i){auto b=std::make_unique<Bundle>();b->key=i;b->used=static_cast<uint64_t>(i);check(pool.Put(b)&&!b,"store transfers sole ownership");}
    check(pool.Full(),"bounded residency");
    auto extra=std::make_unique<Bundle>();check(!pool.Put(extra)&&extra!=nullptr,"full store preserves caller ledger");
    pool.Items()[0]->can_destroy=false;
    check(!pool.MakeRoom(drop)&&pool.Items().size()==4&&destroyed==0,"failed eviction keeps resources recorded");
    pool.Items()[0]->can_destroy=true;
    check(pool.MakeRoom(drop)&&pool.Items().size()==3&&destroyed==3,"successful oldest eviction releases once");
    extra->key=4;extra->used=4;check(pool.Put(extra),"new look fills released slot");
    auto active=pool.Take([](const Bundle& b){return b.key==2;});
    check(active&&active->key==2&&pool.Items().size()==3,"activation removes standby ownership");
    check(!pool.Take([](const Bundle& b){return b.key==2;}),"same bundle cannot activate twice");
    active->used=10;check(pool.Put(active),"retired look returns without recreation");
    pool.Items()[0]->can_destroy=false;
    check(!pool.Clear(drop)&&pool.Items().size()==1&&pool.Items()[0]->owned==3,"partial shutdown keeps failed cleanup ledger");
    pool.Items()[0]->can_destroy=true;check(pool.Clear(drop)&&pool.Items().empty(),"cleanup retry finishes");
    int creates=0,hits=0;uint64_t serial=20;
    // Repeated switching uses the same resident records, then a world change
    // drops every old record. This tests ownership, not Unreal rendering.
    for(int round=0;round<2000;++round){
        const int key=round%4;
        auto b=pool.Take([&](const Bundle& item){return item.key==key&&item.world==1;});
        if(!b){b=std::make_unique<Bundle>();b->key=key;++creates;}else ++hits;
        b->used=serial++;check(pool.MakeRoom(drop),"switch leaves room");check(pool.Put(b),"switch returns ownership");check(pool.Items().size()<=4,"resident cap holds under repeated switching");
    }
    check(creates==4&&hits==1996,"warm repeated switches do not allocate new bundles");
    pool.Prune([](const Bundle& b){return b.world==2;},drop);check(pool.Items().empty(),"world change drains stale bundles");
    auto broken=std::make_unique<Bundle>();broken->owned=2;check(pool.Put(broken),"partially created bundle recorded");
    int attempts=0;
    const auto partial=[&](Bundle& b){++attempts;if(b.owned>0)--b.owned;return b.owned==0;};
    check(!pool.Clear(partial)&&pool.Items()[0]->owned==1,"partial native cleanup is retained");
    check(pool.Clear(partial)&&attempts==2,"remaining native cleanup retried once");
    std::printf("{\"checks\":%d,\"failures\":%d,\"switches\":2000,\"created\":%d,\"reused\":%d,\"runtime\":false}\n",checks,failures,creates,hits);
    return failures?1:0;
}
