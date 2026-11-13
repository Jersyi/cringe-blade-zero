// 空中技能 / JC in the module registry (module "air_skills"): AirSkills::Options as properties, its snapshot read-only. The options are
// posted with RequestOptions (the game thread applies them; the module saves them) and refused outside their ranges, as the page does.
#include "air_skills.h"
#include "mod_registry.h"
#include "module_apis.h"
#include "module_options.h"
#include "plugin_text.h"
#include <cstdio>
#include <string>

namespace ModuleApis {
namespace {
using PluginText::Utf8;

ModuleOptions::Pending<AirSkills::Options> g_options;
AirSkills::Options Current() { return g_options.Now([] { return AirSkills::GetSnapshot().options; }); }
template <class F> bool Modify(F&& change) {
    AirSkills::Options o = Current(); change(o);
    if (!AirSkills::RequestOptions(o)) return false;
    g_options.Sent(o);
    return true;
}
}  // namespace

void RegisterAir() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    ModuleBuilder("air_skills", "空中技能 / JC", "空中技能、踩头跳（近敌 JC）、缓降和各项奖励。改动由游戏线程在下一帧应用并保存；旅行（换图）前会先还原", Thread::Any)
        .Bool("air_skills", "空中技能", [] { return Current().air_skills; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.air_skills = v; }); }, "", "默认关")
        .Bool("stomp_jump", "近敌 JC（踩头跳）", [] { return Current().stomp_jump; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.stomp_jump = v; }); }, "", "默认关")
        .Bool("slow_fall", "JC 后缓降", [] { return Current().slow_fall; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.slow_fall = v; }); })
        .Bool("reward_shachi", "奖励：影之武装", [] { return Current().reward_shachi; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.reward_shachi = v; }); })
        .Bool("reward_pure_shachi", "奖励：纯净影之武装", [] { return Current().reward_pure_shachi; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.reward_pure_shachi = v; }); })
        .Bool("reward_spirit_shadow", "奖励：灵影", [] { return Current().reward_spirit_shadow; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.reward_spirit_shadow = v; }); })
        .Bool("reward_skills", "奖励：技能冷却和次数", [] { return Current().reward_skills; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.reward_skills = v; }); })
        .Bool("reward_weapon_switch", "奖励：换武器冷却", [] { return Current().reward_weapon_switch; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.reward_weapon_switch = v; }); })
        .Bool("reward_guard_dodge", "奖励：格挡闪避冷却", [] { return Current().reward_guard_dodge; }, [](bool v) { return Modify([&](AirSkills::Options& o) { o.reward_guard_dodge = v; }); })
        .Float("bounce_speed", "弹起速度（厘米 / 秒）", 100.0, 3000.0, 750.0, [] { return Current().bounce_speed; }, [](float v) { return Modify([&](AirSkills::Options& o) { o.bounce_speed = v; }); })
        .Float("enemy_radius", "近敌范围（厘米）", 10.0, 1500.0, 250.0, [] { return Current().enemy_radius; }, [](float v) { return Modify([&](AirSkills::Options& o) { o.enemy_radius = v; }); })
        .ReadOnlyBool("ready", "可用", [] { return AirSkills::GetSnapshot().ready; })
        .ReadOnlyBool("active", "已生效", [] { return AirSkills::GetSnapshot().active; })
        .ReadOnlyBool("restore_pending", "等待还原", [] { return AirSkills::GetSnapshot().restore_pending; })
        .ReadOnlyInt("bounces", "弹起次数", [] { return static_cast<int>(AirSkills::GetSnapshot().bounces); })
        .ReadOnlyInt("jump_cancels", "跳跃打断次数", [] { return static_cast<int>(AirSkills::GetSnapshot().jump_cancels); })
        .ReadOnlyText("status", "状态", [] { return Utf8(AirSkills::GetSnapshot().status); })
        .ReadOnlyText("jc_status", "JC 状态", [] { return Utf8(AirSkills::GetSnapshot().jc_status); })
        .Commit();

    ModRegistry::Command status; status.name = "air_skills.status"; status.kind = ModRegistry::Kind::Diagnostic; status.thread = Thread::Any; status.note = "空中技能和 JC 的状态一行（同时写日志）";
    status.run = [](const std::string&, std::string& out) {
        const AirSkills::Snapshot s = AirSkills::GetSnapshot();
        out = Utf8(s.status) + " | " + Utf8(s.jc_status);
        return AirSkills::HandleCommand("air", "status");
    };
    ModRegistry::RegisterCommand(status);
    ModRegistry::Command tags; tags.name = "air_skills.tags"; tags.kind = ModRegistry::Kind::Diagnostic; tags.thread = Thread::Any; tags.note = "下一帧把空中技能涉及的标签写进日志";
    tags.run = [](const std::string&, std::string& out) { out = "写进日志"; return AirSkills::HandleCommand("air", "tags"); };
    ModRegistry::RegisterCommand(tags);
}
}  // namespace ModuleApis
