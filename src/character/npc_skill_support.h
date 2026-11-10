#pragma once
#include <cstddef>
#include <string_view>

namespace NpcSkillSupport {
// DetectTraceDistance belongs to GA_WeaponTraceAttack_Base, not its PP receiver.
inline bool TraceReceiver(bool implements_interface,bool set_trace_target) {
    return implements_interface&&set_trace_target;
}
// Fire Meteor reuses Black GuiChai animations. Their 161 common reference bones
// have matching parents and weapon transforms (offline cooked-skeleton audit).
// Treat this authored family as own motion, without admitting unrelated rigs.
inline bool OwnMotionFamily(std::string_view profile,std::string_view skeleton) {
    return (profile=="GuiChaiRed"||profile=="GuiChaiRedP2")&&
        (skeleton=="GuiChai_Black_Skeleton"||skeleton=="GuiChai_Red_Skeleton"||skeleton=="GuiChai_Red_P2_Skeleton");
}
inline bool AuthoredWeaponIk(std::string_view profile,std::string_view bone) {
    return (profile=="GuiChaiRed"||profile=="GuiChaiRedP2")&&(bone=="ik_weapon_l"||bone=="ik_weapon_r");
}
inline bool MeshSpaceWeaponCopy(std::string_view profile) {
    return profile=="GuiChaiRed"||profile=="GuiChaiRedP2";
}
inline bool RedirectTrace(bool cast_live,bool own_source,bool trace_ability,bool compatible_receiver) {
    return cast_live&&own_source&&trace_ability&&compatible_receiver;
}
// Native activation may execute frame-zero actions synchronously. Its weapon/collider/FX
// lookups must see prepared resources; a refusal must leave no borrowed resources behind.
template<class Prepare,class Activate,class Rollback>
bool ActivateWithSupport(bool boss,Prepare&& prepare,Activate&& activate,Rollback&& rollback) {
    if (boss) prepare();
    if (activate()) return true;
    if (boss) rollback();
    return false;
}
// Borrowed support for a bare cast is not an applied NPC look. In particular, adding
// colliders/stand-ins/chains must not switch the combo editor's owner or reset its chain.
inline bool LookResources(bool bare,size_t weapons,size_t hidden,size_t colliders,size_t stand_ins,size_t chains) {
    return !bare&&(weapons||hidden||colliders||stand_ins||chains);
}
}
