// 武器预设 in the module registry (module "weapons"): the second preset set, the swap, and the temporary weapon the move chain uses.
// Requests are posted (the game thread runs the verified native equip transaction); the snapshot is read-only. The event weapon_changed
// comes from a poller on the game thread (it can read the equipped main weapon's id there).
#include "mod_registry.h"
#include "module_apis.h"
#include "plugin_text.h"
#include "weapon_presets.h"
#include <string>

namespace ModuleApis {
namespace {
using PluginText::ToInt;
using PluginText::Utf8;
using PluginText::Words;

bool SetIndex(const std::string& arg, bool save, std::string& out) {
    int n = 0;
    if (!ToInt(Words(arg).empty() ? std::string() : Words(arg)[0], n) || n < 1 || n > 2) { out = "格式：1 或 2（第几套）"; return false; }
    const bool ok = save ? WeaponPresets::RequestSaveToSet(n - 1) : WeaponPresets::RequestSwitchToSet(n - 1);
    if (!ok) out = "现在做不了：" + Utf8(WeaponPresets::GetSnapshot().status);
    return ok;
}
bool Request(bool ok, std::string& out) {
    if (!ok) out = "现在做不了：" + Utf8(WeaponPresets::GetSnapshot().status);
    return ok;
}
void PollWeapon() {
    static bool seen = false; static std::string last;
    std::string id; const bool known = WeaponPresets::CurrentMainWeaponId(id);
    const WeaponPresets::Snapshot s = WeaponPresets::GetSnapshot();
    const std::string now = (known ? id : std::string()) + "\tset=" + std::to_string(s.current_set + 1);
    if (!seen) { seen = true; last = now; return; }
    if (now != last) { last = now; ModRegistry::Emit("weapons", "weapon_changed", known ? id : std::string()); }
}
}  // namespace

void RegisterWeapons() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    const bool ok = ModuleBuilder("weapons", "武器预设", "第二套武器预设、一键换套、读当前装备，以及招式派生用的临时武器。装备变动由游戏线程按原生流程做（先校验、失败回滚）", Thread::Any)
        .Bool("enabled", "第二套预设", [] { return WeaponPresets::GetSnapshot().enabled; }, [](bool v) { return WeaponPresets::RequestEnable(v); }, "", "默认关")
        .ReadOnlyBool("ready", "可用", [] { return WeaponPresets::GetSnapshot().ready; })
        .ReadOnlyBool("busy", "正在换装", [] { return WeaponPresets::GetSnapshot().busy; })
        .ReadOnlyBool("can_save", "可以存当前装备", [] { return WeaponPresets::GetSnapshot().can_save; })
        .ReadOnlyInt("current_set", "当前是第几套（1 或 2）", [] { return WeaponPresets::GetSnapshot().current_set + 1; })
        .ReadOnlyInt("swaps", "换套次数", [] { return static_cast<int>(WeaponPresets::GetSnapshot().swaps); })
        .ReadOnlyInt("failures", "失败次数", [] { return static_cast<int>(WeaponPresets::GetSnapshot().failures); })
        .ReadOnlyText("status", "状态", [] { return Utf8(WeaponPresets::GetSnapshot().status); })
        .ReadOnlyText("read_status", "读取装备的状态", [] { return Utf8(WeaponPresets::GetSnapshot().read_status); })
        .ReadOnlyText("temporary", "临时武器", [] { const WeaponPresets::TemporaryView t = WeaponPresets::GetTemporary(); return t.weapon + (t.changed ? " (changed)" : ""); })
        .Action("swap", "换到另一套", "", false, [](const std::string&, std::string& out) { return Request(WeaponPresets::RequestSwap(), out); })
        .Action("copy_current_to_other", "把当前装备复制到另一套", "", false, [](const std::string&, std::string& out) { return Request(WeaponPresets::RequestCopyCurrentToOther(), out); })
        .Action("read_current", "读当前实际装备", "", false, [](const std::string&, std::string& out) { return Request(WeaponPresets::RequestReadCurrent(), out); })
        .Action("save_to_set", "把当前装备存进某一套", "1 或 2", false, [](const std::string& a, std::string& out) { return SetIndex(a, true, out); })
        .Action("switch_to_set", "切换到某一套", "1 或 2（要先打开第二套预设）", false, [](const std::string& a, std::string& out) { return SetIndex(a, false, out); })
        .Action("take_temporary", "临时拿起一把武器", "武器 id [shaqi]（shaqi：杀器）", false, [](const std::string& arg, std::string& out) {
            const std::vector<std::string> w = Words(arg);
            if (w.empty() || w.size() > 2 || (w.size() == 2 && w[1] != "shaqi")) { out = "格式：武器 id [shaqi]"; return false; }
            return Request(WeaponPresets::RequestTemporaryWeapon(w[0], w.size() == 2), out); })
        .Action("restore_temporary", "放回临时武器", "", false, [](const std::string&, std::string& out) { return Request(WeaponPresets::RequestTemporaryRestore(), out); })
        .Event("weapon_changed", "装备的主武器变了", "现在装备的主武器 id（读不到为空）")
        .Commit();
    if (ok) ModRegistry::AddPoller("weapons", &PollWeapon);

    ModRegistry::Command status; status.name = "weapons.status"; status.kind = ModRegistry::Kind::Diagnostic; status.thread = Thread::Any; status.note = "武器预设的状态一行（同时写日志）";
    status.run = [](const std::string&, std::string& out) {
        const WeaponPresets::Snapshot s = WeaponPresets::GetSnapshot();
        out = std::string("enabled=") + (s.enabled ? "1" : "0") + " ready=" + (s.ready ? "1" : "0") + " set=" + std::to_string(s.current_set + 1) + " swaps=" + std::to_string(s.swaps) + " failures=" + std::to_string(s.failures) +
              " " + Utf8(s.status);
        return WeaponPresets::HandleCommand("weapons", "status");
    };
    ModRegistry::RegisterCommand(status);
}
}  // namespace ModuleApis
