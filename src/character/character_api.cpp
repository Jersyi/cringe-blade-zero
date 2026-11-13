// 角色替换 in the module registry (module "character"): CharacterSwap's requests and snapshot. The game thread does the work; the
// event swap_changed comes from a poller that compares the snapshot.
#include "character_swap.h"
#include "mod_registry.h"
#include "module_apis.h"
#include "plugin_text.h"
#include <string>

namespace ModuleApis {
namespace {
using PluginText::Utf8;
using PluginText::Words;

void PollSwap() {
    static bool seen = false, was_active = false; static std::wstring was_selected;
    const CharacterSwap::Snapshot s = CharacterSwap::GetSnapshot();
    const std::wstring now = s.active ? s.selected : std::wstring();
    if (!seen) { seen = true; was_active = s.active; was_selected = now; return; }
    if (s.active != was_active || now != was_selected) {
        was_active = s.active; was_selected = now;
        ModRegistry::Emit("character", "swap_changed", Utf8(now));
    }
}
}  // namespace

void RegisterCharacter() {
    using ModRegistry::ModuleBuilder;
    using ModRegistry::Thread;
    const bool ok = ModuleBuilder("character", "角色替换", "把主角换成游戏自带的兼容模型（原生外观装配，不改骨架）。请求由游戏线程执行", Thread::Any)
        .ReadOnlyBool("ready", "可用", [] { return CharacterSwap::GetSnapshot().ready; })
        .ReadOnlyBool("active", "已替换", [] { return CharacterSwap::GetSnapshot().active; })
        .ReadOnlyBool("busy", "正在处理", [] { return CharacterSwap::GetSnapshot().busy; })
        .ReadOnlyText("original", "原来的模型", [] { return Utf8(CharacterSwap::GetSnapshot().original); })
        .ReadOnlyText("selected", "当前选择", [] { return Utf8(CharacterSwap::GetSnapshot().selected); })
        .ReadOnlyText("status", "状态", [] { return Utf8(CharacterSwap::GetSnapshot().status); })
        .Action("replace", "替换", "模型 id 或序号（list 动作给出）", false, [](const std::string& arg, std::string& out) {
            const CharacterSwap::Snapshot state = CharacterSwap::GetSnapshot();
            const std::vector<std::string> words = Words(arg);
            int32_t entry = -1;
            if (!words.empty()) for (const CharacterSwap::Entry& e : state.entries) if (e.id == words[0] || std::to_string(e.index) == words[0]) entry = e.index;
            if (entry < 0) { out = "没有这个模型（用 list 动作看 id）"; return false; }
            if (!CharacterSwap::Request(CharacterSwap::Action::Replace, entry)) { out = "现在做不了：" + Utf8(state.status); return false; }
            return true; })
        .Action("restore", "还原", "", false, [](const std::string&, std::string& out) {
            if (!CharacterSwap::Request(CharacterSwap::Action::Restore)) { out = "现在做不了"; return false; }
            return true; })
        .Action("list", "模型列表", "", false, [](const std::string&, std::string& out) {
            for (const CharacterSwap::Entry& e : CharacterSwap::GetSnapshot().entries) out += std::to_string(e.index) + "\t" + e.id + "\t" + Utf8(e.name) + "\n";
            return true; })
        .Event("swap_changed", "替换结果变了", "现在替换成的模型；还原时为空")
        .Commit();
    if (ok) ModRegistry::AddPoller("character", &PollSwap);
}
}  // namespace ModuleApis
