// 毛发 / Groom 面板的开关 in the command bus: the switches the hair page and its hotkeys (F6 / F8 / F9 ...) flip. HairControls lives on the
// panel / Worker thread and its calls write the game's memory from there, so these are Worker-thread commands: a plugin's call is queued and
// run when the panel thread takes its turn (ComboPlugins::PumpWorker), the answer comes back as host/command_done.
#include "groom_force.h"
#include "hair_controls.h"
#include "mod_registry.h"
#include "module_apis.h"
#include "plugin_text.h"
#include <cstdio>
#include <string>

namespace ModuleApis {
namespace {
using ModRegistry::Kind;
using ModRegistry::Thread;

void Add(HairControls* hair, const char* name, const char* note, Kind kind, bool (HairControls::*toggle)()) {
    ModRegistry::Command c; c.name = name; c.note = note; c.kind = kind; c.thread = Thread::Worker;
    c.run = [hair, toggle](const std::string&, std::string& out) {
        const bool ok = (hair->*toggle)();
        char text[64]{}; _snprintf_s(text, sizeof(text), _TRUNCATE, "status=0x%lx", hair->Status());
        out = text;
        return ok;
    };
    ModRegistry::RegisterCommand(std::move(c));
}
}  // namespace

void RegisterHair(HairControls* hair) {
    if (!hair) return;
    Add(hair, "hair.toggle_mesh_groom", "F8：主角头发在 Mesh 和 Groom 之间切换", Kind::State, &HairControls::ToggleHair);
    Add(hair, "hair.toggle_bounds", "F9：显示 AABB 包围盒", Kind::State, &HairControls::ToggleBounds);
    Add(hair, "hair.toggle_force", "F6：强制 Groom", Kind::State, &HairControls::ToggleForce);
    Add(hair, "hair.toggle_outfit", "Shift+F6：解锁全部外观", Kind::State, &HairControls::ToggleOutfit);
    Add(hair, "hair.toggle_triggers", "Shift+F9：关闭自适应扳机（写 ini）", Kind::Persist, &HairControls::ToggleTriggers);
    ModRegistry::Command restore; restore.name = "hair.restore"; restore.note = "把毛发面板改过的东西都还原"; restore.kind = Kind::State; restore.thread = Thread::Worker;
    restore.run = [hair](const std::string&, std::string& out) { hair->Restore(); char t[64]{}; _snprintf_s(t, sizeof(t), _TRUNCATE, "status=0x%lx", hair->Status()); out = t; return true; };
    ModRegistry::RegisterCommand(std::move(restore));
    ModRegistry::Command status; status.name = "hair.status"; status.note = "毛发面板的状态位和 Force Groom 的状态"; status.kind = Kind::Diagnostic; status.thread = Thread::Worker;
    status.run = [hair](const std::string&, std::string& out) {
        wchar_t force[320]{}, outfit[200]{}; GroomForce::Status(force, 320); GroomForce::OutfitStatus(outfit, 200);
        char text[900]{};
        _snprintf_s(text, sizeof(text), _TRUNCATE, "status=0x%lx force=%ls outfit=%ls", hair->Status(), force, outfit);
        out = text; return true;
    };
    ModRegistry::RegisterCommand(std::move(status));
}
}  // namespace ModuleApis
