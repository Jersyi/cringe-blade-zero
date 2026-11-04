// NPC skin boss skills: the attack-collider tags copied onto the hero for each skin (npc_skill_colliders.generated.h).
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iterator>
#include "npc_skills.generated.h"
#include "npc_skill_colliders.generated.h"

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
    std::printf("npc_skill_colliders_test failures=%d\n", failures);
    return failures ? 1 : 0;
}
