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
    {
        using namespace NpcPoseTransfer;
        std::vector<Bone> source(4),target(4);
        for(size_t i=0;i<4;++i){source[i].name=target[i].name=i+1;source[i].parent=target[i].parent=i?0:-1;}
        source[1].ref.translation[0]=target[1].ref.translation[0]=10;
        source[2].ref=target[2].ref=source[1].ref; // IK rests on hand, then flies independently
        source[3].parent=target[3].parent=2;source[3].ref.translation[1]=target[3].ref.translation[1]=5;
        Plan plan;std::string why;check(Build(plan,source,target,{},why),"fire IK plan");
        plan.source_mode.assign(4,kAnimation);plan.own_mode.assign(4,kAnimation);plan.own_length={0,10,10,5};
        check(SetFollowers(plan,{-1,-1,1,-1}),"fire IK normal follower");
        Cache cache;Prepare(plan,cache);const char* reason="";Transform pose[4]{};pose[1].translation[0]=10;pose[2].translation[0]=310;pose[3]=pose[2];pose[3].translation[1]=5;
        check(Evaluate(plan,pose,4,cache,reason,1,true)&&std::abs(cache.target_cs[2].translation[0]-10)<1e-9,"reproduce IK flight overwritten by hand follower");
        plan.authored_follow={0,0,1,0};Prepare(plan,cache);
        for(double weight:{0.,.25,.5,.75,1.}){
            check(Evaluate(plan,pose,4,cache,reason,weight,true),"fire IK blend evaluates");
            check(std::abs(cache.target_cs[2].translation[0]-(10+300*weight))<1e-9,"fire IK authored travel blends from hand");
            check(std::abs(cache.target_cs[3].translation[0]-cache.target_cs[2].translation[0])<1e-9&&std::abs(cache.target_cs[3].translation[1]-5)<1e-9,"fire IK child follows corrected target");
        }
        check(Evaluate(plan,pose,4,cache,reason,1,false)&&std::abs(cache.target_cs[2].translation[0]-10)<1e-9,"unrelated montage retains normal IK guard");
        pose[2].translation[0]=pose[3].translation[0]=10;
        check(Evaluate(plan,pose,4,cache,reason,1,true)&&std::abs(cache.target_cs[2].translation[0]-10)<1e-9,"fire IK retracts");
        check(NpcSkillSupport::AuthoredWeaponIk("GuiChaiRed","ik_weapon_l")&&NpcSkillSupport::AuthoredWeaponIk("GuiChaiRedP2","ik_weapon_r"),"both fire phases and sides");
        check(!NpcSkillSupport::AuthoredWeaponIk("GuiChaiRed","ik_hand_r")&&!NpcSkillSupport::AuthoredWeaponIk("MoTian","ik_weapon_l"),"scoped to fire weapon IK");
    }
    {
        // Native CopyPose's bone-space branch derives each local from the SOURCE parent.
        // Mapping hand-parented weapon_l onto root-parented VB root_weapon_l therefore
        // feeds the fire post-process a hand-local value as its root-space target.
        using namespace NpcPoseTransfer;
        for(const char* profile:{"GuiChaiRed","GuiChaiRedP2"}) {
            check(NpcSkillSupport::MeshSpaceWeaponCopy(profile),"both fire phases require component-space virtual weapon copies");
            for(double travel:{0.,300.,700.,0.})for(double angle:{0.,.7,1.5}) {
                Transform root,hand,weapon;root.translation[0]=13;root.translation[2]=7;
                hand.translation[0]=40;hand.translation[1]=30;hand.translation[2]=140;
                hand.rotation[0]=std::sin(angle/2);hand.rotation[3]=std::cos(angle/2);
                weapon.translation[0]=45+travel;weapon.translation[1]=35;weapon.translation[2]=145;
                const Transform original=weapon;
                Transform wrong_local,wrong_cs,correct_local,correct_cs;
                check(NpcGripMath::RelativeNativeSafe(weapon,hand,wrong_local)&&NpcGripMath::ComposeMount(wrong_local,root,wrong_cs),"old native bone-space virtual mapping reproduced");
                double error=0;for(int a=0;a<3;++a)error+=std::pow(wrong_cs.translation[a]-weapon.translation[a],2);
                check(std::sqrt(error)>100,"old virtual target falls over one metre away from hammer");
                // Native mesh-space branch assigns weapon CS, then converts against TARGET parent.
                check(NpcGripMath::RelativeNativeSafe(weapon,root,correct_local)&&NpcGripMath::ComposeMount(correct_local,root,correct_cs),"native mesh-space virtual conversion");
                for(int a=0;a<3;++a)check(std::abs(correct_cs.translation[a]-weapon.translation[a])<1e-9,"flight and retract virtual target retains component position");
                check(!std::memcmp(&original,&weapon,sizeof(weapon)),"source pose is unchanged");
            }
        }
        check(!NpcSkillSupport::MeshSpaceWeaponCopy("MoTian")&&!NpcSkillSupport::MeshSpaceWeaponCopy("MuXiaoKui"),"other copy-pose paths retain their existing mode");
    }
    {
        // Pose streaming must derive locals before applying a nonuniform component scale.
        // The old world->local round trip changes a rotated spear's length even without quantization.
        using NpcPoseTransfer::Transform;
        for(double stretch:{1.2,1.5,2.0}) {
            Transform component,parent,child_local,child_cs,parent_world,child_world,wrong_local,wrong_cs,wrong_world,local,rebuilt_cs,rebuilt_world;
            component.scale[0]=stretch;parent.rotation[2]=std::sin(3.141592653589793/4);parent.rotation[3]=std::cos(3.141592653589793/4);
            child_local.translation[0]=150;child_local.translation[2]=-24;
            check(NpcGripMath::ComposeMount(child_local,parent,child_cs)&&NpcGripMath::ComposeMount(parent,component,parent_world)&&NpcGripMath::ComposeMount(child_cs,component,child_world),"scaled spear fixture composes");
            check(NpcGripMath::RelativeNativeSafe(child_world,parent_world,wrong_local)&&NpcGripMath::ComposeMount(wrong_local,parent,wrong_cs)&&NpcGripMath::ComposeMount(wrong_cs,component,wrong_world),"old world-space pose roundtrip reproduced");
            check(std::abs(wrong_world.translation[1]-child_world.translation[1])>20,"world roundtrip loses spear length under nonuniform scale");
            check(NpcGripMath::RelativeNativeSafe(child_cs,parent,local)&&NpcGripMath::ComposeMount(local,parent,rebuilt_cs)&&NpcGripMath::ComposeMount(rebuilt_cs,component,rebuilt_world),"component-space pose roundtrip composes");
            for(int axis=0;axis<3;++axis)check(std::abs(rebuilt_world.translation[axis]-child_world.translation[axis])<1e-9,"component-space locals preserve scaled spear position");
        }
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
