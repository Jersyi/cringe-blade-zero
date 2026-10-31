#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Worker/UI only submits options. All UObject operations run on the game thread.
namespace AirSkills {
using Logger = void (*)(const char*);
using InputFocus=bool (*)();
struct Options {
    bool air_skills = false;
    bool stomp_jump = false;
    bool slow_fall = true; // Only after a confirmed JC; no effect while JC is off.
    bool reward_shachi=true;
    bool reward_pure_shachi=true;
    bool reward_spirit_shadow=true;
    // Authored Blueprint cooldown and charges share the same time mark.
    bool reward_skills=true;
    bool reward_weapon_switch=true;
    bool reward_guard_dodge=true; // Only the native L1 stance SpecialDash cooldown effect.
    float bounce_speed = 750.0f; // cm/s, native LaunchCharacter Z override.
    float enemy_radius = 250.0f; // cm from the two capsule surfaces, including vertical distance.
};
struct SkillState {
    std::wstring ability, input_action;
    double cooldown = 0;
    int32_t charges = 0, maximum_charges = 0;
    bool restored = false;
};
struct ResourceReward {
    float before=0,after=0,maximum=0;
    bool known=false,verified=false;
};
struct Snapshot {
    Options options;
    bool ready = false, active = false, jump_input_ready = false;
    bool restore_pending = false;
    bool jump_priority_ready=false,slow_fall_active=false,gravity_control_yielded=false;
    uint32_t cloned_bindings = 0, relaxed_tags = 0, owned_abilities = 0, stomp_targets = 0;
    uint32_t strong_references = 0;
    uint32_t scanned_input_assets = 0, scanned_input_actions = 0, scanned_ability_specs = 0;
    uint32_t bounces = 0, cooldown_restores = 0;
    uint32_t jump_cancels=0;
    uint32_t jump_edges=0,scanned_pawns=0,enemy_ai_rejected=0,enemy_team_rejected=0,enemy_health_rejected=0;
    float nearest_enemy_surface=-1;
    int32_t jump_priority=0;
    float base_gravity=0,current_gravity=0,down_speed_cap=0;
    uint32_t restored_charges = 0, failed_restores = 0;
    float last_shachi = 0, last_shachi_max = 0;
    ResourceReward pure_shachi,spirit_shadow;
    uint32_t weapon_switch_resets=0,weapon_switch_failures=0,last_weapon_switch_slots=0;
    bool last_weapon_switch_verified=false;
    bool guard_dodge_known=false,last_guard_dodge_verified=false;
    int32_t guard_dodge_effects_before=0,guard_dodge_effects_after=0;
    uint32_t guard_dodge_resets=0,guard_dodge_failures=0;
    std::vector<SkillState> skills;
    std::vector<std::wstring> input_assets, unadapted_skills;
    std::wstring status = L"空中技能/跳跃打断与附近敌人 JC 默认关闭";
    std::wstring jc_status = L"近敌 JC 已关闭；需单独勾选并点击应用";
};
// JC requires a side-effect-free foreground getter; nullptr safely disables
// input/cancel/launch/reward while keeping air adaptation/restoration available.
void Initialize(Logger logger,InputFocus can_gameplay_input=nullptr);
bool RequestOptions(const Options& options);
Snapshot GetSnapshot();
void OnGameThreadGuarded();
// Restore the old player's cloned input pointers and removed ability tags before travel.
// Selected options are retained, and applied only after a different world is available.
// Must be checked by every caller. False means some live original input/tag
// state or counted strong references have not been restored; do not travel.
bool BeforeTravel();
bool HandleCommand(const char* verb, const char* arguments);
}
