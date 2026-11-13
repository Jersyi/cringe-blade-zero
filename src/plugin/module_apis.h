#pragma once
// 各模块在「模块注册表」里的描述表 (module registry tables). Each module describes itself in its own file next to its code
// (src/<folder>/<module>_api.cpp, one Register function); RegisterAll calls them once from the worker at start. They only use the
// modules' public headers, so a module's code does not change for the plugin API. sdk/pbz_combo_api.h and docs/PLUGIN_API.md
// list what each table holds (module_list, prop_list, action_list, event_list, command_list).
class HairControls;

namespace ModuleApis {
using Logger = void (*)(const char*);
void RegisterAll(Logger logger, HairControls* hair);

// One per module (defined in the module's folder). Each is safe to call more than once (a second call is refused by the registry).
void RegisterCombo();        // character/combo_api.cpp      招式派生的选项
void RegisterNpcLook();      // character/npc_look_api.cpp   NPC 重定向 / 外观套用 (NpcRetarget)
void RegisterCharacter();    // character/character_api.cpp  角色替换 (CharacterSwap)
void RegisterBody();         // character/npc_body_api.cpp   身材 / 骨骼 (NpcBody, NpcBridge) and the 骨骼变换 runtime (bones)
void RegisterBossRush();     // boss/boss_rush_api.cpp       Boss 连战
void RegisterBossAi();       // boss/boss_ai_api.cpp         Boss AI 参数
void RegisterAir();          // air/air_skills_api.cpp       空中技能 / JC
void RegisterGrab();         // grab/motian_grab_api.cpp     投技
void RegisterWeapons();      // weapons/weapon_presets_api.cpp 武器预设
void RegisterPad();          // pad/pad_api.cpp              手柄（自适应扳机）
void RegisterHair(HairControls* hair);  // hair/hair_api.cpp 毛发 / Groom 面板的开关（面板线程的命令）
void RegisterHub();          // ui/hub_api.cpp               面板本身（显示 / 隐藏 / 切页，面板线程的命令）
}
