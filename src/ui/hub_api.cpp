// 面板本身 in the command bus: show / hide the F10 panel and switch its page. The panel is Worker-thread UI ("Pages keep their existing
// request/snapshot boundary"), so these are Worker-thread commands: queued, run when the panel thread takes its turn
// (ComboPlugins::PumpWorker), the answer comes back as host/command_done.
#include "debug_hub.h"
#include "mod_registry.h"
#include "module_apis.h"
#include <cstring>
#include <string>

namespace ModuleApis {
namespace {
struct PageName { const char* name; DebugHub::Page page; };
constexpr PageName kPages[] = {
    {"boss_rush", DebugHub::Page::BossRush}, {"boss_ai", DebugHub::Page::BossAI}, {"character", DebugHub::Page::Character}, {"npc", DebugHub::Page::Npc},
    {"combo", DebugHub::Page::Combo}, {"air", DebugHub::Page::AirSkills}, {"grab", DebugHub::Page::MotianGrab}, {"groom", DebugHub::Page::Groom},
    {"weapons", DebugHub::Page::WeaponPresets}, {"hotkeys", DebugHub::Page::Hotkeys}, {"supplement", DebugHub::Page::Supplement},
    {"online", DebugHub::Page::Online}, {"donate", DebugHub::Page::Donate}, {"body", DebugHub::Page::BoneMotion},
};
}  // namespace

void RegisterHub() {
    using ModRegistry::Kind;
    using ModRegistry::Thread;
    ModRegistry::Command show; show.name = "hub.show"; show.args = "页名（boss_rush boss_ai character npc combo air grab groom weapons hotkeys supplement online donate body）";
    show.note = "显示面板并切到这一页"; show.kind = Kind::State; show.thread = Thread::Worker;
    show.run = [](const std::string& args, std::string& out) {
        for (const PageName& p : kPages) if (args == p.name) {
            if (!DebugHub::Enabled()) { out = "面板没有启用"; return false; }
            DebugHub::ShowPage(p.page);  // opens the panel on that page (a page request always shows it)
            return true;
        }
        out = "没有这一页"; return false;
    };
    ModRegistry::RegisterCommand(std::move(show));
    ModRegistry::Command toggle; toggle.name = "hub.toggle"; toggle.note = "显示 / 隐藏面板（同 F10）"; toggle.kind = Kind::State; toggle.thread = Thread::Worker;
    toggle.run = [](const std::string&, std::string& out) { if (!DebugHub::Enabled()) { out = "面板没有启用"; return false; } DebugHub::Toggle(); return true; };
    ModRegistry::RegisterCommand(std::move(toggle));
    ModRegistry::Command hide; hide.name = "hub.hide"; hide.note = "隐藏面板"; hide.kind = Kind::State; hide.thread = Thread::Worker;
    hide.run = [](const std::string&, std::string&) { DebugHub::Hide(); return true; };
    ModRegistry::RegisterCommand(std::move(hide));
    ModRegistry::Command status; status.name = "hub.status"; status.note = "面板是否在显示"; status.kind = Kind::Diagnostic; status.thread = Thread::Worker;
    status.run = [](const std::string&, std::string& out) { out = std::string("enabled=") + (DebugHub::Enabled() ? "1" : "0") + " visible=" + (DebugHub::Visible() ? "1" : "0"); return true; };
    ModRegistry::RegisterCommand(std::move(status));
}
}  // namespace ModuleApis
