// 招式派生的选项 in the module registry (module "combo"). The same seven switches api->get_option / set_option reach, as properties,
// so a plugin's UI page can bind to them, plus the 镜头取景 option added with the 主角体型 node. Setters are NpcCombo's own (any
// thread, saved at once); the game thread reads them every frame.
#include "mod_registry.h"
#include "module_apis.h"
#include "npc_combo.h"
#include <algorithm>

namespace ModuleApis {
void RegisterCombo() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    ModuleBuilder("combo", "招式派生", "招式派生页顶栏的选项：技能 / 投技伤害、派生显示、镜头跟随与取景、派生提示。派生表本身用 edit_* 编辑", Thread::Any)
        .Float("skill_damage", "技能伤害", 0.0, NpcCombo::kChainSkillMax, 0.0,
               [] { return NpcCombo::ChainSkillDamage(); },
               [](float v) { NpcCombo::SetChainSkillDamage(v <= 0 ? 0.f : std::clamp(v, NpcCombo::kChainSkillMin, NpcCombo::kChainSkillMax)); },
               "NpcCombo.ChainSkillDamage", "0 跟随 NPC 技能页，否则 0.1~10 倍 BOSS 自己的伤害")
        .Int("grab_damage_mode", "投技伤害方式", NpcCombo::kGrabFollow, 2, NpcCombo::kGrabFollow,
             [] { return NpcCombo::ChainGrabDamage().mode; },
             [](int v) { NpcCombo::ChainGrab g = NpcCombo::ChainGrabDamage(); g.mode = v; NpcCombo::SetChainGrabDamage(g); },
             "NpcCombo.ChainGrabDamage", "-1 跟随投技页，0 原生，1 倍率，2 每段至少最大生命的百分比")
        .Float("grab_damage_value", "投技伤害数值", 0.0, 1000.0, 0.0,
               [] { return NpcCombo::ChainGrabDamage().value; },
               [](float v) { NpcCombo::ChainGrab g = NpcCombo::ChainGrabDamage(); g.value = v; NpcCombo::SetChainGrabDamage(g); },
               "NpcCombo.ChainGrabValue", "倍率或百分比，看投技伤害方式")
        .Enum("chain_hud", "派生显示", {"off", "detailed", "simple"}, NpcCombo::kHudOff,
              [] { return NpcCombo::ChainHud(); }, [](int v) { NpcCombo::SetChainHud(v); }, "NpcCombo.ChainHud", "游戏右上角的派生链叠加：关 / 详细 / 简略")
        .Enum("view_target", "视角跟随盯的骨骼", {"pelvis", "neck", "head"}, NpcCombo::kViewPelvis,
              [] { return NpcCombo::ViewTarget(); }, [](int v) { NpcCombo::SetViewTarget(v); }, "NpcCombo.ViewTarget")
        .Enum("view_scale", "镜头取景", {"height", "ratio"}, 0,
              [] { return NpcCombo::ViewScale(); }, [](int v) { NpcCombo::SetViewScale(v); }, "NpcCombo.ViewScale", "按模型身高 / 按与主角的体型比例推远镜头")
        .Bool("show_toast", "派生提示", [] { return NpcCombo::ShowToast(); }, [](bool v) { NpcCombo::SetShowToast(v); }, "NpcCombo.ShowToast", "招式播放时游戏右下角的提示")
        .Commit();
}
}  // namespace ModuleApis
