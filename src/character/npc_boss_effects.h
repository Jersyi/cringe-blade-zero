#pragma once
// BOSS 特效: the game's battle effects by folder (npc_boss_effects.generated.h, from scripts/gen_boss_effects.py).
#include <string>
#include <vector>
#include "npc_boss_effects.generated.h"

namespace NpcBossEffects {
// The effect a BOSS 特效 node names (nullptr: not in this game's list).
inline const NpcBossEffect* Find(const std::string& name) {
    for (const NpcBossEffect& e : kNpcBossEffects) if (name == e.name) return &e;
    return nullptr;
}
// The folders in list order (each once).
inline std::vector<std::string> Folders() {
    std::vector<std::string> out;
    for (const NpcBossEffect& e : kNpcBossEffects) if (out.empty() || out.back() != e.folder) out.push_back(e.folder);
    return out;
}
// The Chinese name of a folder whose id is not a look's id (a look's id gets the look's name, ComboMenu): the bosses'
// own folders under another spelling, their 冰 / 火 / 毒 variants, the hero's folders and the plain enemy kinds.
// nullptr: none known (the id is shown).
inline const wchar_t* FolderAlias(const std::string& folder) {
    static constexpr struct { const char* id; const wchar_t* name; } kAliases[] = {
        {"MoYuan", L"魔渊"}, {"ZuoShang", L"左殇"}, {"ZuoShang_LongTi", L"左殇：真龙之躯"}, {"HanSui", L"寒岁"}, {"YanRu", L"炎茹"},
        {"LeiTing", L"雷婷"}, {"GuHe", L"蛊鹤"}, {"HuoLiuXing", L"火流星"}, {"BiHai", L"碧海大师残躯"}, {"BiYuan", L"毕远掌门残躯"},
        {"GreatBladeMan", L"毕峰"}, {"ArrowMan_DuYan", L"独眼弓客"}, {"Monster_XiaoXue", L"小雪"}, {"Bandit_JingYe", L"荆野"},
        {"LeiDang_Ice", L"雷当（冰）"}, {"LionDancer_Ice", L"烈焰（冰）"}, {"PengMeng_Ice", L"彭猛（冰）"}, {"YuDing_Ice", L"玉鼎（冰）"},
        {"Scavenger_Ice", L"活人偶清道夫（冰）"}, {"HuaShan", L"华山"},
        {"Common", L"通用"}, {"Weapon", L"主角武器"}, {"ShachiWeapon", L"杀器"}, {"Hun", L"主角"},
        {"ArrowMan", L"弓手"}, {"ArrowMan_Fire", L"弓手（火）"}, {"Bandit", L"山贼"}, {"CrossbowMan", L"弩手"},
        {"BombMan", L"投弹手"}, {"BombMan_Ice", L"投弹手（冰）"}, {"BombMan_Poison", L"投弹手（毒）"},
        {"MaskAssassin_Gold", L"面具刺客（金）"}, {"MaskAssassin_Iron", L"面具刺客（铁）"}, {"MaskAssassin_Jade", L"面具刺客（玉）"},
        {"Dogtamer", L"驯犬人"}, {"Doll", L"人偶"}, {"Monster_Doll_002", L"人偶 2"}, {"FeiZei", L"飞贼"}, {"FishMan", L"鱼人"},
        {"Fishman_Ice", L"鱼人（冰）"}, {"FistAddict", L"拳手"}, {"Horse", L"马"}, {"IllnessCrow", L"病鸦"}, {"MobMan", L"杂兵"},
        {"SickDog", L"病犬"}, {"Troop", L"士兵"},
    };
    for (const auto& a : kAliases) if (folder == a.id) return a.name;
    return nullptr;
}
// A short name for the list: the effect's name without "NS_" and its folder's prefix.
inline std::string ShortName(const NpcBossEffect& e) {
    std::string name = e.name;
    if (name.rfind("NS_", 0) == 0) name.erase(0, 3);
    const std::string folder = std::string(e.folder) + "_";
    if (name.rfind(folder, 0) == 0 && name.size() > folder.size()) name.erase(0, folder.size());
    return name;
}
}  // namespace NpcBossEffects
