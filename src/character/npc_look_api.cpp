// NPC 重定向 / 外观套用 in the module registry (module "npc_look"): what NpcRetarget's public header offers any thread (requests that the
// game thread carries out, snapshots, the saved options). Nothing here touches npc_retarget.cpp. The event look_changed comes from a
// poller that compares the snapshot, so it needs no hook in the NpcRetarget code.
#include "mod_registry.h"
#include "module_apis.h"
#include "npc_retarget.h"
#include "plugin_text.h"
#include <string>

namespace ModuleApis {
namespace {
using PluginText::Utf8;
using PluginText::Words;

// "id" or the row number -> the entry's index (-1: none).
int32_t Resolve(const std::string& what, const NpcRetarget::Snapshot& state) {
    if (what.empty()) return -1;
    for (const NpcRetarget::Entry& e : state.entries) if (e.id == what || std::to_string(e.index) == what) return e.index;
    return -1;
}
bool Do(NpcRetarget::Action action, const std::string& arg, std::string& out) {
    const NpcRetarget::Snapshot state = NpcRetarget::GetSnapshot();
    int32_t entry = -1;
    if (action == NpcRetarget::Action::Apply) {
        entry = Resolve(Words(arg).empty() ? std::string() : Words(arg)[0], state);
        if (entry < 0) { out = "没有这个外观（用 list 动作看 id）"; return false; }
    }
    if (!NpcRetarget::Request(action, entry)) { out = "现在做不了：" + Utf8(state.status); return false; }
    out = Utf8(state.status);
    return true;
}
// The look change event: the applied look's name (empty when the hero's own body is back), from the snapshot's changes.
void PollLook() {
    static bool seen = false, was_active = false; static std::wstring was_selected;
    const NpcRetarget::Snapshot s = NpcRetarget::GetSnapshot();
    const std::wstring now = s.active ? s.selected : std::wstring();
    if (!seen) { seen = true; was_active = s.active; was_selected = now; return; }
    if (s.active != was_active || now != was_selected) {
        was_active = s.active; was_selected = now;
        ModRegistry::Emit("npc_look", "look_changed", Utf8(now));
    }
}
}  // namespace

void RegisterNpcLook() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    const bool ok = ModuleBuilder("npc_look", "NPC 外观", "NPC 重定向：把 NPC 的外观套到主角身上（BOSS 技能、招式派生的变身都用它）。请求由游戏线程执行，读到的是它最近发布的状态", Thread::Any)
        .Float("skill_damage", "NPC 技能伤害", NpcRetarget::kSkillDamageMin, NpcRetarget::kSkillDamageMax, NpcRetarget::kSkillDamageDefault,
               [] { return NpcRetarget::SkillDamageMultiplier(); }, [](float v) { return NpcRetarget::SetSkillDamageMultiplier(v); },
               "NpcSkill.DamageMultiplier", "外观带的 BOSS 技能打出的伤害是原生伤害的几倍；1 = BOSS 自己的伤害")
        .Bool("boss_locomotion", "BOSS 自带移动动画", [] { return NpcRetarget::BossLocomotion(); }, [](bool v) { NpcRetarget::SetBossLocomotion(v); },
              "NpcRetarget.BossLocomotion", "只移动时，外观播自己的待机 / 走 / 跑")
        .Bool("experimental", "实验性外观", [] { return NpcRetarget::GetSnapshot().experimental; }, [](bool v) { return NpcRetarget::SetExperimental(v); }, "", "关闭时会正常还原")
        .ReadOnlyBool("ready", "可用", [] { return NpcRetarget::GetSnapshot().ready; })
        .ReadOnlyBool("active", "外观已套用", [] { return NpcRetarget::GetSnapshot().active; })
        .ReadOnlyBool("busy", "正在处理", [] { return NpcRetarget::GetSnapshot().busy; })
        .ReadOnlyBool("cleanup_pending", "等待清理", [] { return NpcRetarget::GetSnapshot().cleanup_pending; })
        .ReadOnlyText("selected", "当前外观", [] { return Utf8(NpcRetarget::GetSnapshot().selected); })
        .ReadOnlyText("status", "状态", [] { return Utf8(NpcRetarget::GetSnapshot().status); })
        .ReadOnlyInt("skin", "套用的外观行（-1：没有）", [] { return NpcRetarget::GetSnapshot().skin; })
        .ReadOnlyInt("parts", "部件数", [] { return NpcRetarget::GetSnapshot().parts; })
        .Action("apply", "套用外观", "外观 id 或序号（list 动作给出）", false, [](const std::string& arg, std::string& out) { return Do(NpcRetarget::Action::Apply, arg, out); })
        .Action("restore", "还原主角", "", false, [](const std::string&, std::string& out) { return Do(NpcRetarget::Action::Restore, std::string(), out); })
        .Action("inspect", "检查当前外观（写日志）", "", false, [](const std::string&, std::string& out) { return Do(NpcRetarget::Action::Inspect, std::string(), out); })
        .Action("skill", "释放外观的 BOSS 技能", "", false, [](const std::string&, std::string& out) {
            if (!NpcRetarget::RequestSkill()) { out = "没有套用外观"; return false; }
            return true; })
        .Action("list", "外观列表", "", false, [](const std::string&, std::string& out) {
            for (const NpcRetarget::Entry& e : NpcRetarget::GetSnapshot().entries) out += std::to_string(e.index) + "\t" + e.id + "\t" + Utf8(e.name) + "\n";
            return true; })
        .Event("look_changed", "外观变了", "现在套着的外观名；还原成主角本身时为空")
        .Commit();
    if (ok) ModRegistry::AddPoller("npc_look", &PollLook);
}
}  // namespace ModuleApis
