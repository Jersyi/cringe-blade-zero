#include "npc_source_budget.h"
#include <cstdio>

int main() {
    using namespace NpcSourceBudget;
    int checks=0,failures=0;
    const auto check=[&](bool ok,const char* name) { ++checks; if (!ok) { ++failures; std::printf("FAIL %s\n",name); } };
    for (int bits=0;bits<16;++bits) {
        Policy original{0.75f,(bits&1)!=0,(bits&2)!=0,(bits&4)!=0,(bits&8)!=0},state=original;
        // Native API forbids reduced work when always-tick is set.
        if (original.always&&original.reduced) continue;
        Lease lease; int writes=0;
        const auto read=[&](Policy& out) { out=state; return true; };
        const auto write=[&](Policy p) { ++writes; state=p; return true; };
        check(lease.Hold(read,write)&&state.always&&!state.offscreen&&!state.reduced,"budgeted cinematic source stays on full animation updates");
        check(state.significance==original.significance&&state.interpolate==original.interpolate,"priority and interpolation are preserved");
        const int once=writes;
        for (int i=0;i<1000;++i) check(lease.Hold(read,write)&&writes==once,"stable maintenance does not spam the native setter");
        state.significance=0.2f; state.interpolate=!original.interpolate;
        Policy expected=original; expected.significance=state.significance; expected.interpolate=state.interpolate;
        check(lease.Restore(read,write)&&!lease.owned&&Equal(state,expected),"cutscene end restores owned flags and preserves native changes");
        const int restored=writes;
        check(lease.Restore(read,write)&&writes==restored,"second cleanup is a no-op");
    }
    {
        Lease lease; Policy state{1,false,true,true,false}; bool fail=true;
        const auto read=[&](Policy& out) { out=state; return true; };
        const auto write=[&](Policy p) { state=p; return !fail; };
        check(!lease.Hold(read,write)&&lease.owned,"partial acquisition retains rollback");
        check(!lease.Restore(read,write)&&lease.owned,"partial restoration can be retried");
        fail=false; check(lease.Restore(read,write)&&!lease.owned&&!state.always&&state.offscreen&&state.reduced,"retry completes restoration");
    }
    {
        Lease lease; Policy state{1,false,true,true,false}; int writes=0;
        const auto read=[&](Policy& out) { out=state; return true; };
        check(!lease.Hold(read,[&](Policy) { ++writes; return true; })&&lease.owned&&writes==1,"readback rejects a silent setter failure");
        check(lease.Restore(read,[](Policy) { return true; })&&!lease.owned,"unchanged original state needs no rollback write");
    }
    {
        Lease lease; Policy state{1,false,false,false,false};
        const auto read=[&](Policy& out) { out=state; return true; };
        const auto write=[&](Policy p) { state=p; return true; };
        check(lease.Hold(read,write),"acquire for foreign-change test");
        state.always=false; state.offscreen=true; state.reduced=true;
        const Policy changed=state;
        check(lease.Restore(read,write)&&Equal(state,changed),"foreign scheduling changes survive cleanup");
    }
    {
        Lease lease; int writes=0;
        check(!lease.Hold([](Policy&) { return false; },[&](Policy) { ++writes; return true; })&&!lease.owned&&!writes,"unreadable allocator makes no writes");
    }
    std::printf("{\"checks\":%d,\"failures\":%d,\"mocked_native_api\":true}\n",checks,failures);
    return failures?1:0;
}
