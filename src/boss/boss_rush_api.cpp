// Boss 连战 in the module registry (module "boss_rush") and Boss AI 参数 (module "boss_ai"): the options the page edits, the
// requests it posts, and the snapshots it reads (any thread; the game thread carries the requests out). The event rush_changed comes
// from a poller that compares the snapshot.
#include "boss_ai.h"
#include "boss_rush.h"
#include "mod_registry.h"
#include "module_apis.h"
#include "module_options.h"
#include "plugin_text.h"
#include <cmath>
#include <cstdio>
#include <string>

namespace ModuleApis {
namespace {
using PluginText::ToDouble;
using PluginText::ToInt;
using PluginText::Utf8;
using PluginText::Words;

ModuleOptions::Pending<BossRush::Options> g_rush_options;
BossRush::Options RushOptions() { return g_rush_options.Now([] { return BossRush::GetSnapshot().options; }); }
template <class Change> bool ChangeRush(Change&& change) {
    BossRush::Options o = RushOptions(); change(o);
    if (!BossRush::Request(BossRush::Action::ApplyOptions, -1, o)) return false;
    g_rush_options.Sent(o);
    return true;
}
// A boss's id or its index in the catalog (-1: none).
int32_t FindEntry(const BossRush::Snapshot& s, const std::string& what) {
    for (const BossRush::Entry& e : s.entries) if (e.id == what || std::to_string(e.index) == what) return e.index;
    return -1;
}
bool Run(BossRush::Action action, bool needs_boss, const std::string& arg, std::string& out) {
    const BossRush::Snapshot s = BossRush::GetSnapshot();
    int32_t entry = s.current >= 0 ? s.current : 0;
    const std::vector<std::string> w = Words(arg);
    if (!w.empty()) { entry = FindEntry(s, w[0]); if (entry < 0) { out = "没有这个 Boss（用 list 动作看 id）"; return false; } }
    else if (needs_boss && s.entries.empty()) { out = "Boss 目录还没读到"; return false; }
    if (!BossRush::Request(action, needs_boss ? entry : -1, RushOptions())) { out = "现在做不了：" + Utf8(s.status); return false; }
    out = Utf8(s.status);
    return true;
}
void PollRush() {
    static bool seen = false; static bool running = false; static int32_t current = -1, completed = 0;
    const BossRush::Snapshot s = BossRush::GetSnapshot();
    if (!seen) { seen = true; running = s.running; current = s.current; completed = s.completed; return; }
    if (s.running != running || s.current != current || s.completed != completed) {
        running = s.running; current = s.current; completed = s.completed;
        ModRegistry::Emit("boss_rush", "rush_changed", std::string("running=") + (running ? "1" : "0") + " current=" + std::to_string(current) + " completed=" + std::to_string(completed) + "/" + std::to_string(s.total));
    }
}

// ---- Boss AI ----
const BossAI::Parameter* FindParameter(const BossAI::Snapshot& s, const std::string& what) {
    int index = -1; const bool numeric = ToInt(what, index);
    for (const BossAI::Parameter& p : s.parameters) if (p.index == index && numeric) return &p;
    for (const BossAI::Parameter& p : s.parameters) if (Utf8(p.key) == what) return &p;
    return nullptr;
}
}  // namespace

void RegisterBossRush() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    const bool rush = ModuleBuilder("boss_rush", "Boss 连战", "Boss 目录、直接挑战、连战的选项和进度。请求由游戏线程执行", Thread::Any)
        .Bool("override_rank", "改写 Rank", [] { return RushOptions().override_rank; }, [](bool v) { return ChangeRush([&](BossRush::Options& o) { o.override_rank = v; }); })
        .Int("rank", "Rank", 0, 9999, 0, [] { return RushOptions().rank; }, [](int v) { return ChangeRush([&](BossRush::Options& o) { o.rank = v; }); }, "", "范围看 rank_min / rank_max")
        .Bool("override_cycle", "改写周目", [] { return RushOptions().override_cycle; }, [](bool v) { return ChangeRush([&](BossRush::Options& o) { o.override_cycle = v; }); })
        .Int("cycle_number", "周目（从 1 数）", 1, 100, 1, [] { return RushOptions().cycle_number; }, [](int v) { return ChangeRush([&](BossRush::Options& o) { o.cycle_number = v; }); })
        .Bool("override_difficulty", "改写难度", [] { return RushOptions().override_difficulty; }, [](bool v) { return ChangeRush([&](BossRush::Options& o) { o.override_difficulty = v; }); })
        .Int("difficulty", "难度（游戏原生枚举值）", 0, 16, 2, [] { return RushOptions().difficulty; }, [](int v) { return ChangeRush([&](BossRush::Options& o) { o.difficulty = v; }); },
             "", "不是下拉框序号；可用值看 difficulties 动作")
        .Bool("auto_next", "自动下一个", [] { return RushOptions().auto_next; }, [](bool v) { return ChangeRush([&](BossRush::Options& o) { o.auto_next = v; }); })
        .Bool("heal_on_start", "开战回满血", [] { return RushOptions().heal_on_start; }, [](bool v) { return ChangeRush([&](BossRush::Options& o) { o.heal_on_start = v; }); })
        .ReadOnlyBool("ready", "可用", [] { return BossRush::GetSnapshot().ready; })
        .ReadOnlyBool("running", "连战进行中", [] { return BossRush::GetSnapshot().running; })
        .ReadOnlyBool("busy", "正在处理", [] { return BossRush::GetSnapshot().busy; })
        .ReadOnlyBool("transitioning", "正在切换场景", [] { return BossRush::IsTransitioning(); })
        .ReadOnlyInt("current", "当前 Boss 序号（-1：没有）", [] { return BossRush::GetSnapshot().current; })
        .ReadOnlyInt("completed", "已完成", [] { return BossRush::GetSnapshot().completed; })
        .ReadOnlyInt("total", "总数", [] { return BossRush::GetSnapshot().total; })
        .ReadOnlyInt("rank_min", "Rank 下限", [] { return BossRush::GetSnapshot().rank_min; })
        .ReadOnlyInt("rank_max", "Rank 上限", [] { return BossRush::GetSnapshot().rank_max; })
        .ReadOnlyText("status", "状态", [] { return Utf8(BossRush::GetSnapshot().status); })
        .Action("teleport", "传送到 Boss", "Boss id 或序号（省略：当前）", false, [](const std::string& a, std::string& o) { return Run(BossRush::Action::Teleport, true, a, o); })
        .Action("direct", "直接挑战", "Boss id 或序号（省略：当前）", false, [](const std::string& a, std::string& o) { return Run(BossRush::Action::DirectFight, true, a, o); })
        .Action("start", "开始连战", "从哪个 Boss 开始（省略：当前）", false, [](const std::string& a, std::string& o) { return Run(BossRush::Action::StartRush, true, a, o); })
        .Action("next", "下一个", "", false, [](const std::string& a, std::string& o) { return Run(BossRush::Action::Next, false, a, o); })
        .Action("retry", "重试", "", false, [](const std::string& a, std::string& o) { return Run(BossRush::Action::Retry, false, a, o); })
        .Action("stop", "停止连战", "", false, [](const std::string& a, std::string& o) { return Run(BossRush::Action::Stop, false, a, o); })
        .Action("refresh", "刷新目录", "", false, [](const std::string& a, std::string& o) { return Run(BossRush::Action::Refresh, false, a, o); })
        .Action("list", "Boss 目录", "", false, [](const std::string&, std::string& out) {
            for (const BossRush::Entry& e : BossRush::GetSnapshot().entries) out += std::to_string(e.index) + "\t" + e.id + "\t" + (e.available ? "1" : "0") + "\t" + Utf8(e.name) + "\t" + Utf8(e.map) + "\n";
            return true; })
        .Action("difficulties", "难度的原生枚举值", "", false, [](const std::string&, std::string& out) {
            for (const BossRush::Difficulty& d : BossRush::GetSnapshot().difficulties) out += std::to_string(d.value) + "\t" + Utf8(d.name) + "\n";
            return true; })
        .Event("rush_changed", "连战状态变了", "running=0/1 current=序号 completed=已完成/总数")
        .Commit();
    if (rush) ModRegistry::AddPoller("boss_rush", &PollRush);

    ModRegistry::Command status; status.name = "boss_rush.status"; status.kind = ModRegistry::Kind::Diagnostic; status.thread = Thread::Any; status.note = "连战的状态一行";
    status.run = [](const std::string&, std::string& out) {
        const BossRush::Snapshot s = BossRush::GetSnapshot();
        out = "ready=" + std::to_string(s.ready) + " busy=" + std::to_string(s.busy) + " running=" + std::to_string(s.running) + " current=" + std::to_string(s.current) + " completed=" + std::to_string(s.completed) + "/" +
              std::to_string(s.total) + " " + Utf8(s.status);
        return true;
    };
    ModRegistry::RegisterCommand(status);
}

void RegisterBossAi() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    ModuleBuilder("boss_ai", "Boss AI 参数", "原生 Boss AI 实例的概率 / 权重 / 冷却：读出来、改、还原。目标是正在打的 Boss；请求由游戏线程执行", Thread::Any)
        .ReadOnlyBool("ready", "已绑定 Boss", [] { return BossAI::GetSnapshot().ready; })
        .ReadOnlyText("target", "目标", [] { return Utf8(BossAI::GetSnapshot().target); })
        .ReadOnlyText("status", "状态", [] { return Utf8(BossAI::GetSnapshot().status); })
        .ReadOnlyInt("parameters", "参数个数", [] { return static_cast<int>(BossAI::GetSnapshot().parameters.size()); })
        .Action("list", "参数列表", "", false, [](const std::string&, std::string& out) {
            for (const BossAI::Parameter& p : BossAI::GetSnapshot().parameters) {
                char row[512]{};
                _snprintf_s(row, sizeof(row), _TRUNCATE, "%d\t%s\t%s\t%g\t%g\t%g\t%g\t%d\n", p.index, Utf8(p.key).c_str(), Utf8(p.label).c_str(), p.current, p.baseline, p.minimum, p.maximum, p.changed ? 1 : 0);
                out += row;
            }
            return true; })
        .Action("set", "改一个参数", "参数 key 或序号 值（list 动作给出，值在最小到最大之间）", false, [](const std::string& arg, std::string& out) {
            const BossAI::Snapshot s = BossAI::GetSnapshot();
            const std::vector<std::string> w = Words(arg);
            double value = 0;
            if (w.size() != 2 || !ToDouble(w[1], value)) { out = "格式：参数 值"; return false; }
            const BossAI::Parameter* p = FindParameter(s, w[0]);
            if (!p) { out = "没有这个参数"; return false; }
            if (value < p->minimum || value > p->maximum) { out = "值要在 " + std::to_string(p->minimum) + " 到 " + std::to_string(p->maximum) + " 之间"; return false; }
            if (!BossAI::RequestSet(s.generation, p->index, static_cast<float>(value))) { out = "现在做不了：" + Utf8(s.status); return false; }
            return true; })
        .Action("refresh", "重新读取", "", false, [](const std::string&, std::string& out) {
            if (!BossAI::RequestRefresh(BossAI::GetSnapshot().generation)) { out = "现在做不了"; return false; }
            return true; })
        .Action("restore", "全部还原", "", false, [](const std::string&, std::string& out) {
            if (!BossAI::RequestRestore(BossAI::GetSnapshot().generation)) { out = "现在做不了"; return false; }
            return true; })
        .Commit();
}
}  // namespace ModuleApis
