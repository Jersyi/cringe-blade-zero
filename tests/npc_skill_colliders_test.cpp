// NPC skin boss skills: the attack-collider tags copied onto the hero for each skin (npc_skill_colliders.generated.h).
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include "npc_skills.generated.h"
#include "npc_skill_colliders.generated.h"
#include "npc_skill_trails.generated.h"
#include "npc_fx_lookup.h"

namespace {
int failures = 0;
void Check(bool ok, const char* name) { if (!ok) { ++failures; std::printf("FAIL %s\n", name); } }
const NpcColliderTags* Row(const char* profile) {
    for (size_t i = 0; i < std::size(kNpcSkillSets); ++i) if (!std::strcmp(kNpcSkillSets[i].profile, profile)) return &kNpcColliderTagSets[i];
    return nullptr;
}
bool Has(const char* profile, const char* tag) {
    const NpcColliderTags* row = Row(profile);
    if (!row) return false;
    for (size_t i = 0; i < row->count; ++i) if (!std::strcmp(row->tags[i], tag)) return true;
    return false;
}
}
int main() {
    Check(std::size(kNpcColliderTagSets) == std::size(kNpcSkillSets), "one collider row per skill set");
    size_t tagged = 0, tags = 0;
    for (size_t i = 0; i < std::size(kNpcColliderTagSets); ++i) {
        const NpcColliderTags& row = kNpcColliderTagSets[i];
        Check((row.count == 0) == (row.tags == nullptr), "a row's count matches its array");
        Check(row.count <= 16, "at most 16 tags per skin");
        for (size_t k = 0; k < row.count; ++k) Check(row.tags[k] && row.tags[k][0] && !std::strchr(row.tags[k], '.'), "tags are component tags, not gameplay tags");
        tagged += row.count ? 1 : 0; tags += row.count;
        if (!kNpcSkillSets[i].skill_count) Check(row.count == 0, "a skin without skills has no collider tags");
    }
    // 魔渊: its sword's strong / stab boxes and the right-leg kick were never found on the hero.
    Check(Has("MoTian", "Attack Collider Strong") && Has("MoTian", "Attack Collider Stab") && Has("MoTian", "RightLeg"), "MoTian strong / stab / right leg");
    // 雷当 fights with fists and feet: body colliders only.
    Check(Has("LeiDangP2", "LeftArm") && Has("LeiDangP2", "RightArm") && Has("LeiDangP2", "LeftLeg") && Has("LeiDangP2", "RightLeg"), "LeiDang P2 limbs");
    Check(Has("ChiLianP2", "Snake Spine L") && Has("ChiLianP2", "Snake Thigh R"), "ChiLian P2 snake body");
    Check(Has("ChangHong", "Attack Collider Spear"), "ChangHong spear");
    Check(!Row("MBoss") || Row("MBoss")->count == 0, "MBoss has no combat blueprint");
    std::printf("  skins with collider tags %zu of %zu, tags %zu\n", tagged, std::size(kNpcColliderTagSets), tags);
    // Boss weapon configs lent to GAA_PBZWeaponTrail per skin (npc_skill_trails.generated.h): object paths of data assets.
    Check(std::size(kNpcTrailConfigSets) == std::size(kNpcSkillSets), "one trail config row per skill set");
    size_t trail_skins = 0;
    for (size_t i = 0; i < std::size(kNpcTrailConfigSets); ++i) {
        const NpcTrailConfigs& row = kNpcTrailConfigSets[i];
        Check((row.count == 0) == (row.configs == nullptr) && row.count <= 12, "a trail row's count matches its array");
        for (size_t k = 0; k < row.count; ++k) {
            const char* path = row.configs[k]; const char* dot = path ? std::strrchr(path, '.') : nullptr; const char* slash = path ? std::strrchr(path, '/') : nullptr;
            const size_t leaf = dot && slash && slash < dot ? static_cast<size_t>(dot - slash - 1) : 0;
            Check(path && path[0] == '/' && leaf && std::strlen(dot + 1) == leaf && !std::strncmp(slash + 1, dot + 1, leaf), "a config is /Pkg/Name.Name");
        }
        trail_skins += row.count ? 1 : 0;
    }
    const auto trails = [](const char* profile) -> const NpcTrailConfigs* {
        for (size_t i = 0; i < std::size(kNpcSkillSets); ++i) if (!std::strcmp(kNpcSkillSets[i].profile, profile)) return &kNpcTrailConfigSets[i];
        return nullptr; };
    Check(trails("HookMan") && trails("HookMan")->count >= 1 && std::strstr(trails("HookMan")->configs[0], "Weapon_HookMan"), "HookMan trails come from its own weapon config");
    Check(trails("ChangHong") && trails("ChangHong")->count >= 1 && std::strstr(trails("ChangHong")->configs[0], "ChangHong"), "ChangHong spear trails");
    std::printf("  skins with trail configs %zu of %zu\n", trail_skins, std::size(kNpcTrailConfigSets));
    // Effect lookups on a skin (NpcFxLookup::Rewrite): stand-ins are ids 100 + display index, the host is 1.
    {
        const auto stand_in = [](int value) { return value >= 100 ? value - 100 : -1; };
        int a[] = {102}; NpcFxLookup::Result r = NpcFxLookup::Rewrite(a, 1, 1, stand_in);
        Check(r.related && r.only && r.row == 2 && r.num == 1 && a[0] == 1, "a stand-in index answers the host, its row kept");
        int b[] = {1, 101, 102}; r = NpcFxLookup::Rewrite(b, 3, 1, stand_in);
        Check(r.related && !r.only && r.row == 1 && r.num == 1 && b[0] == 1, "all weapons: the host once, effects stay its own");
        int c[] = {1}; r = NpcFxLookup::Rewrite(c, 1, 1, stand_in);
        Check(r.related && !r.only && r.row == -1 && r.num == 1 && c[0] == 1, "index 0: unchanged, related");
        int d[] = {7, 8}; r = NpcFxLookup::Rewrite(d, 2, 1, stand_in);
        Check(!r.related && r.num == 2 && d[0] == 7 && d[1] == 8, "another avatar's weapons: untouched");
        int e[] = {103}; r = NpcFxLookup::Rewrite(e, 1, 0, stand_in);
        Check(r.related && !r.only && r.num == 1 && e[0] == 103, "no host: the stand-in stays");
    }
    std::printf("npc_skill_colliders_test failures=%d\n", failures);
    return failures ? 1 : 0;
}
