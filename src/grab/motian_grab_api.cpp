// 投技 in the module registry (module "grab"): MotianGrab::Options as properties, the trigger and cancel requests, the move list and the
// snapshot. Requests are posted (the game thread runs them; the module saves the options).
#include "mod_registry.h"
#include "module_apis.h"
#include "module_options.h"
#include "motian_grab.h"
#include "plugin_text.h"
#include <string>

namespace ModuleApis {
namespace {
using PluginText::Utf8;

ModuleOptions::Pending<MotianGrab::Options> g_options;
MotianGrab::Options Current() { return g_options.Now([] { return MotianGrab::GetSnapshot().options; }); }
template <class F> bool Modify(F&& change) {
    MotianGrab::Options o = Current(); change(o);
    if (!MotianGrab::RequestOptions(o)) return false;
    g_options.Sent(o);
    return true;
}
}  // namespace

void RegisterGrab() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    ModuleBuilder("grab", "投技", "魔天的擒拿投技：选项、选招、触发。请求由游戏线程执行（要主角在地面、有目标等）。招式派生里的投技节点另有自己的通道，不受这里影响", Thread::Any)
        .Bool("enabled", "投技", [] { return Current().enabled; }, [](bool v) { return Modify([&](MotianGrab::Options& o) { o.enabled = v; }); }, "", "默认关")
        .Bool("allow_bosses", "允许人形 Boss", [] { return Current().allow_bosses; }, [](bool v) { return Modify([&](MotianGrab::Options& o) { o.allow_bosses = v; }); })
        .Bool("position_correction", "位置校正", [] { return Current().position_correction; }, [](bool v) { return Modify([&](MotianGrab::Options& o) { o.position_correction = v; }); })
        .Int("variant", "招式序号", 0, 999, 0, [] { return Current().variant; }, [](int v) { return Modify([&](MotianGrab::Options& o) { o.variant = v; }); }, "", "variants 动作列出可用序号；原版魔天的招式是 0~3")
        .Float("range", "范围（厘米，胶囊表面）", 0.0, 1000.0, 280.0, [] { return Current().range; }, [](float v) { return Modify([&](MotianGrab::Options& o) { o.range = v; }); }, "", "会被限制在所选招式自己的范围内")
        .Bool("tang_execution", "黑伤处决改为魔渊擒拿", [] { return Current().tang_execution; }, [](bool v) { return Modify([&](MotianGrab::Options& o) { o.tang_execution = v; }); }, "Grab.TangExecution")
        .Bool("transform", "投技时变身", [] { return Current().transform; }, [](bool v) { return Modify([&](MotianGrab::Options& o) { o.transform = v; }); })
        .ReadOnlyBool("ready", "可用", [] { return MotianGrab::GetSnapshot().ready; })
        .ReadOnlyBool("active", "投技进行中", [] { return MotianGrab::GetSnapshot().active; })
        .ReadOnlyBool("paired", "已配对目标", [] { return MotianGrab::GetSnapshot().paired; })
        .ReadOnlyBool("cleanup_pending", "等待清理", [] { return MotianGrab::GetSnapshot().cleanup_pending; })
        .ReadOnlyBool("disabled", "已被停用（出错）", [] { return MotianGrab::GetSnapshot().disabled; })
        .ReadOnlyInt("attempts", "尝试次数", [] { return static_cast<int>(MotianGrab::GetSnapshot().attempts); })
        .ReadOnlyInt("activations", "成功次数", [] { return static_cast<int>(MotianGrab::GetSnapshot().activations); })
        .ReadOnlyText("status", "状态", [] { return Utf8(MotianGrab::GetSnapshot().status); })
        .Action("trigger", "触发一次投技", "", false, [](const std::string&, std::string& out) {
            if (!MotianGrab::RequestTrigger()) { out = "现在做不了：" + Utf8(MotianGrab::GetSnapshot().status); return false; }
            return true; })
        .Action("cancel", "取消", "", false, [](const std::string&, std::string&) { MotianGrab::RequestCancel(); return true; })
        .Action("variants", "招式列表", "", false, [](const std::string&, std::string& out) {
            for (const MotianGrab::Variant& v : MotianGrab::GetSnapshot().variants)
                out += std::to_string(v.index) + "\t" + Utf8(v.id) + "\t" + Utf8(v.name) + "\t" + Utf8(v.boss_id) + "\t" + (v.supported ? "1" : "0") + "\t" + std::to_string(v.max_range) + "\n";
            return true; })
        .Commit();

    ModRegistry::Command status; status.name = "grab.status"; status.kind = ModRegistry::Kind::Diagnostic; status.thread = Thread::Any; status.note = "投技的状态一行（同时写日志）";
    status.run = [](const std::string&, std::string& out) {
        const MotianGrab::Snapshot s = MotianGrab::GetSnapshot();
        out = std::string("enabled=") + (s.options.enabled ? "1" : "0") + " ready=" + (s.ready ? "1" : "0") + " active=" + (s.active ? "1" : "0") + " attempts=" + std::to_string(s.attempts) +
              " activations=" + std::to_string(s.activations) + " " + Utf8(s.status);
        return MotianGrab::HandleCommand("grab", "status");
    };
    ModRegistry::RegisterCommand(status);
}
}  // namespace ModuleApis
