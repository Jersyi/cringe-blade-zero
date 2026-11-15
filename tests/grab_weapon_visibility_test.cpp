#include "grab_weapon_visibility.h"
#include <cstdio>
#include <limits>
#include <initializer_list>
int main(){using namespace GrabWeaponVisibility;int checks=0,failed=0;auto check=[&](bool v){++checks;if(!v)++failed;};
    Window mux{0,4.547830581665039f};check(mux.Contains(0));check(mux.Contains(4.54f));check(!mux.Contains(mux.end));check(!mux.Contains(-1));check(!mux.Contains(std::numeric_limits<float>::quiet_NaN()));
    check(!Window{1,1}.Contains(1));check(!Window{2,1}.Contains(2));
    for(int i=0;i<1000;++i){const float t=i*.01f;const bool hidden=mux.Contains(t);check(Show(true,false,false,hidden,3)==!hidden);check(!Show(false,true,false,hidden,3));check(!Show(true,false,true,hidden,3));check(!Show(true,true,true,hidden,1));}
    // Pause and backwards/forwards clock jumps remain based on native time.
    for(float t:{2.f,2.f,2.f,5.f,1.f,6.f})check(Show(true,false,false,mux.Contains(t),4)==(t>=mux.end));
    check(Show(true,true,true,false,3));check(Show(true,false,false,false,3));
    NativeState state;check(!state.Hidden(false,false));check(state.Hidden(false,true));check(state.Hidden(false,false));check(!state.Hidden(true,false));
    state={};check(!state.Hidden(true,true));check(!state.Hidden(false,false));
    // Armed=false puts the weapon away only when the Boss has that config (沐小葵 01: config 2 of the first phase's 2: kept).
    check(!PutsAway(false,2,2));check(PutsAway(false,2,3));check(PutsAway(false,2,0));check(!PutsAway(true,2,3));check(!PutsAway(true,0,0));
    check(!PutsAway(false,-1,3));check(PutsAway(false,0,1));check(!PutsAway(false,1,1));
    // A held part: hidden at first sight stays hidden at the end; one shown while held is hidden again and shown at the end.
    {HeldPart p{true};check(!p.Seen(true));check(p.restore_hidden);check(p.Seen(false));check(!p.restore_hidden);check(!p.Seen(true));check(!p.restore_hidden);}
    {HeldPart p{false};check(!p.Seen(true));check(!p.restore_hidden);check(p.Seen(false));check(!p.restore_hidden);}
    std::printf("grab_weapon_visibility checks=%d failures=%d\n",checks,failed);return failed?1:0;
}
