#include "npc_combo_model.h"
#include "combo_weapons.h"
#include <cstdio>
int main(){using namespace NpcCombo;int checks=0,failed=0;auto check=[&](bool v){++checks;if(!v){++failed;std::printf("failed %d\n",checks);}};
    Config c;Node w=WeaponActionsNode(0,0);w.weapon="weapon_test";w.on=true;Route tap;tap.button=static_cast<int>(Trigger::Heavy);tap.next=1;Route hold=tap;hold.hold_ms=500;hold.next=2;Route longer=hold;longer.hold_ms=1000;longer.next=3;w.routes={tap,hold,longer};
    Node move;move.ability="GA_Test";c.chain={w,move,move,move};Press p;p.weapon=w.weapon;p.bits=1u<<static_cast<int>(Trigger::Heavy);
    for(int ms=0;ms<1600;++ms){p.hold_ms=ms;const int expected=ms<500?0:ms<1000?1:2;check(WeaponActionsRow(w,p)==expected);const auto hop=Follow(c,0,0,p,0);check(hop.move==expected+1&&hop.hold_ms==(expected==0?0:expected==1?500:1000));}
    for(int i=0;i<1000;++i){auto decoded=Decode(Encode(c));check(decoded.chain[0].routes.size()==3);check(decoded.chain[0].routes[1].hold_ms==500&&decoded.chain[0].routes[2].hold_ms==1000);}
    p.weapon="other";check(WeaponActionsRow(w,p)<0);p.weapon=w.weapon;w.routes[0].next=-1;p.hold_ms=499;check(WeaponActionsRow(w,p)<0);p.hold_ms=500;check(WeaponActionsRow(w,p)==1);
    w.air_set=true;w.weapon.clear();p.airborne=false;check(WeaponActionsRow(w,p)<0);p.airborne=true;check(WeaponActionsRow(w,p)==1);c.chain[0]=w;check(Decode(Encode(c)).chain[0].routes[1].hold_ms==500);
    check(ComboWeapons::HeldAttack("GA_Hun_BronzeSword_B_Hold"));check(ComboWeapons::HeldAttack("GA_Hun_Iron_BChargeAttack"));check(!ComboWeapons::HeldAttack("GA_Hun_Tang_HoldDeflect"));check(!ComboWeapons::HeldAttack("GA_TianShaShuangYue_ChargePassive"));
    int held=0;for(const auto& weapon:ComboWeaponsData::kWeapons)for(size_t i=0;i<weapon.count;++i)if(ComboWeapons::HeldAttack(weapon.moves[i].ability))++held;check(held>=10);
    Route special;special.button=static_cast<int>(Trigger::Keys);special.keys={81,69};special.chord=true;special.swallow=true;special.hold_ms=300;special.next=1;
    c.chain[0]=WeaponActionsNode(0,0);c.chain[0].weapon="weapon_test";c.chain[0].routes={special};p={};p.weapon="weapon_test";p.bits=1u<<static_cast<int>(Trigger::Keys);p.branch=0;p.row=0;p.hold_ms=300;
    check(TriggerFor(c,p)==0&&Follow(c,0,0,p,0).move==1);p.branch=2;check(TriggerFor(c,p)==-1);p.branch=0;p.weapon="other";check(TriggerFor(c,p)==-1);p.weapon="weapon_test";p.hold_ms=299;check(TriggerFor(c,p)==-1);
    check(HeldRawChord({81,69},{81,69},{69}));check(!HeldRawChord({81,69},{69},{69}));check(!HeldRawChord({81,69},{81,69},{}));check(!HeldRawChord({81,69},{81,69},{70}));
    const auto guard=static_cast<int>(Trigger::Stance),ability=static_cast<int>(Trigger::Shachi);const unsigned guard_bit=1u<<guard,ability_bit=1u<<ability;
    check(HeldActionChord({guard,ability},guard_bit|ability_bit,ability_bit));check(!HeldActionChord({guard,ability},ability_bit,ability_bit));check(!HeldActionChord({guard,ability},guard_bit|ability_bit,0));
    for(int i=0;i<500;++i){auto next=Decode(Encode(c));const auto& r=next.chain[0].routes[0];check(r.keys==special.keys&&r.chord&&r.swallow&&r.hold_ms==300&&r.next==1);}
    special.button=static_cast<int>(Trigger::Combo);special.keys={guard,ability};c.chain[0].routes={special};const auto next=Decode(Encode(c));check(next.chain[0].routes[0].keys==special.keys&&next.chain[0].routes[0].chord);
    std::printf("weapon_action_hold checks=%d failures=%d held_catalog_rows=%d\n",checks,failed,held);return failed?1:0;
}
