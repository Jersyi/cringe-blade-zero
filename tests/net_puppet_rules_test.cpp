#include "net_puppet_rules.h"
#include <cstdio>
#include <limits>
int main() {
    using namespace NetPuppetRules; int checks=0,failures=0;
    const auto check=[&](bool v){++checks;if(!v)++failures;};
    check(AssetPath("/Game/FX/NS_Fire.NS_Fire"));
    for(const char* p:{"/Engine/Transient.X","/Script/Engine.Actor","/Game/../Bad.X","bad","/Game\\Bad.X"})check(!AssetPath(p));
    std::vector<Description> parts{{1,None,None,100,1,1},{2,1,None,0,4,0},{3,1,None,0,5,0}};
    check(Descriptions(parts));
    auto rigid=parts;rigid[0].render_static=1;check(Descriptions(rigid));
    rigid[0].render_static=2;check(!Descriptions(rigid));rigid=parts;rigid[1].render_static=1;check(!Descriptions(rigid));
    for(int i=0;i<3;++i){auto p=parts;p[static_cast<size_t>(i)].id=0;check(!Descriptions(p));}
    auto p=parts;p[1].id=1;check(!Descriptions(p));p=parts;p[0].parent=2;check(!Descriptions(p));
    p=parts;p[2].leader=300;check(!Descriptions(p));p=parts;p[1].posed=1;check(!Descriptions(p));
    p=parts;p[0].bones=1501;check(!Descriptions(p));p=parts;p[0].kind=7;check(!Descriptions(p));
    check(AcceptPose(3,3,true,1,0,true));check(AcceptPose(3,3,true,0,0xffffffff,true));
    check(!AcceptPose(2,3,true,100,0,true));check(!AcceptPose(3,3,true,1,1,true));check(!AcceptPose(3,3,false,1,0,false));
    for(int i=0;i<10000;++i){check(!AcceptPose(7,8,true,static_cast<uint32_t>(i+1),0,true));}
    check(SweepHit({-100,0,0},{100,0,0},{0,0,0},20));
    check(!SweepHit({-100,100,0},{100,100,0},{0,0,0},20));
    check(!SweepHit({-1000,0,0},{1000,0,0},{0,0,0},20));
    check(!SweepHit({0,0,0},{std::numeric_limits<double>::quiet_NaN(),0,0},{0,0,0},20));
    std::vector<Description> chain{{1,None,None,100,1,1},{2,1,None,0,6,0},{3,1,None,0,4,0,2}};
    check(Descriptions(chain));chain[2].spline=1;check(!Descriptions(chain));chain[2].spline=4;check(!Descriptions(chain));
    chain[2].spline=2;chain[1].spline=3;check(!Descriptions(chain));chain[1].spline=None;chain[2].kind=5;check(!Descriptions(chain));
    for(int status=0;status<256;++status){
        check(ProjectileVisible(true,static_cast<uint8_t>(status),false,false)==(status==2));
        check(!ProjectileVisible(false,static_cast<uint8_t>(status),false,false));
        check(!ProjectileVisible(true,static_cast<uint8_t>(status),true,false));
        check(!ProjectileVisible(true,static_cast<uint8_t>(status),false,true));
    }
    Duel duel;check(!duel.Hit(0));duel.BeginAttack();check(duel.Hit(0));check(duel.Hit(1));
    for(int i=0;i<10000;++i)check(!duel.Hit(i%2));
    check(duel.health[0]==900&&duel.health[1]==900);
    for(int i=0;i<9;++i){duel.BeginAttack();check(duel.Hit(0));}
    duel.BeginAttack();check(!duel.Hit(1));check(duel.health[0]==0&&duel.health[1]==900);
    check(!duel.Hit(-1));check(!duel.Hit(2));
    check(HairMask("/Hero/Hair/Hun_Hair_Band.Hun_Hair_Band")==0);
    check(HairMask("/Hero/Hair/Hun_Head.Hun_Head")==0);
    check(HairMask("/Hero/Hair/Hun_BrowLashCard.Hun_BrowLashCard")==6);
    check(HairMask("/Hero/Cloth/CA_Hun_Hair01.CA_Hun_Hair01")==1);
    for(int available=0;available<16;++available)for(bool groom:{false,true}){
        std::vector<HairChoice> hair{{1,true,!!(available&1)},{1,false,!!(available&2)},{2,true,!!(available&4)},{6,false,!!(available&8)}};
        auto shown=ChooseHair(hair,groom);unsigned g=0,c=0;
        for(size_t i=0;i<hair.size();++i)if(shown[i]){check(hair[i].available);(hair[i].groom?g:c)|=hair[i].mask;}
        check(!(g&c));check(!!((g|c)&1)==!!(available&3));check(!!((g|c)&6)==!!(available&12));
    }
    check(ChooseHair({{1,true,true},{1,false,true}},true)==std::vector<bool>({true,false}));
    check(ChooseHair({{2,true,true},{6,false,true}},true)==std::vector<bool>({false,true}));
    check(ChooseHair({{2,true,true},{4,true,true},{6,false,true}},true)==std::vector<bool>({true,true,false}));
    std::printf("net_puppet_rules checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
