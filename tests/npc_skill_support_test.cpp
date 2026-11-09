#include "npc_skill_support.h"
#include "npc_pose_transfer.h"
#include <array>
#include <cstdio>
#include <vector>
#include <cstring>
#include <iterator>
#include "npc_skills.generated.h"
#include "npc_chains.generated.h"
#include "npc_weapon_displays.generated.h"
#include "npc_weapon_sets.generated.h"
#include "npc_skill_colliders.generated.h"

int main() {
    int checks=0,failed=0;
    auto check=[&](bool ok,const char* name){++checks;if(!ok){++failed;std::printf("FAIL %s\n",name);}};
    check(NpcSkillSupport::TraceReceiver(true,true),"fire PP with trace interface and SetTraceTarget is valid without ability-only DetectTraceDistance");
    check(!NpcSkillSupport::TraceReceiver(false,true)&&!NpcSkillSupport::TraceReceiver(true,false),"unrelated or incomplete PP receivers stay native");
    for(const char* profile:{"GuiChaiRed","GuiChaiRedP2"}){
        for(const char* skeleton:{"GuiChai_Black_Skeleton","GuiChai_Red_Skeleton","GuiChai_Red_P2_Skeleton"})
            check(NpcSkillSupport::OwnMotionFamily(profile,skeleton),"authored fire family preserves weapon travel");
        check(!NpcSkillSupport::OwnMotionFamily(profile,"Hun_Skin_Skeleton")&&!NpcSkillSupport::OwnMotionFamily(profile,"MoTian_Skeleton"),"unrelated rigs keep foreign translation guard");
    }
    check(!NpcSkillSupport::OwnMotionFamily("MuXiaoKui","GuiChai_Black_Skeleton"),"fire-family exception never applies to another look");
    {
        // Source weapon bones use Animation translation, like Hun's weapon_l/r.
        // A chain-blade track travels metres away from the hand; it must survive
        // the actual pose-transfer evaluator, and return to its authored rest.
        using namespace NpcPoseTransfer;
        std::vector<Bone> source(3),target(3);
        for(size_t i=0;i<3;++i){source[i].name=target[i].name=i+1;source[i].parent=target[i].parent=i?0:-1;}
        source[1].ref.translation[0]=10;source[2].ref.translation[0]=-10;
        target[1].ref.translation[0]=2;target[2].ref.translation[0]=-2;
        Plan plan;std::string why;check(Build(plan,source,target,{},why),"chain-blade pose plan builds");
        plan.source_mode.assign(3,kAnimation);plan.own_mode.assign(3,kAnimation);plan.own_length={0,2,2};
        Cache cache;Prepare(plan,cache);const char* reason="";
        Transform pose[3]{};pose[1].translation[0]=302;pose[2].translation[0]=-402;
        check(Evaluate(plan,pose,3,cache,reason,1,false)&&cache.target_local[1].translation[0]==2,"reproduces old frozen chain-blade translation");
        for(const char* profile:{"GuiChaiRed","GuiChaiRedP2"}){
            const bool own=NpcSkillSupport::OwnMotionFamily(profile,"GuiChai_Black_Skeleton");
            check(Evaluate(plan,pose,3,cache,reason,1,own)&&std::abs(cache.target_local[1].translation[0]-302)<1e-9&&
                std::abs(cache.target_local[2].translation[0]+402)<1e-9,"both chain blades preserve authored flight on fire-family moves");
        }
        pose[1].translation[0]=2;pose[2].translation[0]=-2;
        check(Evaluate(plan,pose,3,cache,reason,1,true)&&std::abs(cache.target_local[1].translation[0]-2)<1e-9&&
            std::abs(cache.target_local[2].translation[0]+2)<1e-9,"chain blades retract with the original track");
    }
    std::array<int,5> resources{};
    for(int mode=0;mode<3;++mode) for(int attempt=0;attempt<1000;++attempt) {
        const bool boss=mode!=0,bare=mode==1,accepted=attempt%3!=0;
        int prepares=0,rollbacks=0,activations=0;
        bool opening_hit=false,opening_fx=false;
        const bool result=NpcSkillSupport::ActivateWithSupport(boss,[&]{++prepares;resources={2,1,4,1,3};},[&]{
            ++activations;
            if(boss){
                check(resources==std::array<int,5>{2,1,4,1,3},"frame-zero sees all resources");
                opening_hit=resources[2]>0&&resources[3]>0;
                opening_fx=resources[0]>0&&resources[4]>0;
                check(NpcSkillSupport::LookResources(bare,resources[0],resources[1],resources[2],resources[3],resources[4])==!bare,"bare support preserves chain owner");
            }else check(resources==std::array<int,5>{},"hero native cast borrows nothing");
            return accepted;
        },[&]{++rollbacks;resources={};});
        check(result==accepted&&activations==1,"one activation, native result retained");
        check(prepares==(boss?1:0)&&rollbacks==(boss&&!accepted?1:0),"rollback only failed boss activation");
        if(!accepted)check(resources==std::array<int,5>{},"rejected activation releases all resources before retry");
        if(boss&&accepted)check(opening_hit&&opening_fx,"opening hits and effects can resolve their holders");
        resources={}; // normal ability end
    }
    for(int mask=0;mask<32;++mask)for(bool bare:{false,true})
        check(NpcSkillSupport::LookResources(bare,mask&1,mask&2,mask&4,mask&8,mask&16)==(!bare&&mask!=0),"all ownership combinations");
    for(int mask=0;mask<16;++mask)
        check(NpcSkillSupport::RedirectTrace(mask&1,mask&2,mask&4,mask&8)==(mask==15),"trace redirect requires this cast's source, ability and compatible receiver");
    check(std::size(kNpcSkillSets)==std::size(kNpcWeaponDisplays)&&std::size(kNpcSkillSets)==std::size(kNpcColliderTagSets),"support tables cover every skill owner");
    for(const char* profile:{"GuiChaiRed","GuiChaiRedP2"}) {
        bool found=false;
        for(size_t i=0;i<std::size(kNpcSkillSets);++i) {
            const auto& row=kNpcSkillSets[i]; if(std::strcmp(row.profile,profile))continue;found=true;
            check(row.weapon_count==2&&kNpcWeaponDisplays[i]>=2,"fire meteor has both native weapons and display holders");
            check(std::strcmp(row.weapons[0].socket,row.weapons[1].socket)!=0,"fire meteor left/right weapon sockets are distinct");
            int chains=0; bool left=false,right=false,middle=false;
            for(const auto& c:kNpcChains)if(!std::strcmp(c.monster,row.monster)&&c.auto_activate){
                ++chains;left|=!std::strcmp(c.chain,"BP_ChainComponent_L");right|=!std::strcmp(c.chain,"BP_ChainComponent_R");middle|=!std::strcmp(c.chain,"BP_ChainComponent_M");
                check(*c.spline&&*c.niagara,"chain has native spline and effect");
            }
            check(chains==3&&left&&right&&middle,"fire meteor left/right/middle chains retained");
            bool blade=false,mid=false;for(size_t tag=0;tag<kNpcColliderTagSets[i].count;++tag){blade|=!std::strcmp(kNpcColliderTagSets[i].tags[tag],"Attack Collider");mid|=!std::strcmp(kNpcColliderTagSets[i].tags[tag],"Weapon_Mid");}
            check(blade&&(std::strcmp(profile,"GuiChaiRed")||mid),"native blade tags and phase-one middle-chain tag retained");
        }
        check(found,"fire meteor phase is present");
    }
    std::printf("checks=%d failures=%d\n",checks,failed);return failed?1:0;
}
