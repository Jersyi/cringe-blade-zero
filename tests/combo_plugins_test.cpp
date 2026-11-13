// 派生插件 (ComboPlugins + sdk/pbz_combo_api.h): the model's plugin node and signal keys, the host's registry, queue,
// guarded calls, edit sessions, options and settings, and the two sample DLLs loaded from PBZPlugins\ as the game does.
// Hidden: no window, no input, no game.
#include "combo_plugins.h"
#include "mod_registry.h"
#include "npc_combo.h"
#include "npc_combo_model.h"
#include "settings_store.h"
#include <windows.h>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
using NpcCombo::Node;
using NpcCombo::NodeType;
using NpcCombo::Trigger;

// ---- the fake runtime ----
NpcCombo::Config chain;
std::vector<std::string> presses, resets;
std::vector<std::pair<int, int>> plays;
int watched_code = 0;
PbzHeroFacts hero{};
const NpcCombo::Config* BridgeConfig() { return &chain; }
bool BridgeState(PbzChainState& s) { s.ready = 1; s.trigger = 0; s.cursor = 0; strncpy_s(s.table, "Hero", _TRUNCATE); strncpy_s(s.look, "hero", _TRUNCATE); return true; }
bool BridgeHero(PbzHeroFacts& f) { const uint32_t size = f.size; f = hero; f.size = size; return hero.valid != 0; }
bool BridgePress(int button, const char* source) { presses.push_back(std::to_string(button) + "@" + source); return true; }
void BridgeReset(const char* reason) { resets.push_back(reason); }
bool BridgeCancel(int32_t handle) { return handle == 77; }
bool BridgeWatched(int code) { return code == watched_code; }
int BridgeTrigger() { return 0; }
bool BridgePlay(int node, int button) { plays.push_back({node, button}); return true; }
std::vector<std::string> logs;
void Sink(const char* text) { logs.push_back(text); }
std::vector<std::wstring> notices;
void Notice(const wchar_t* title, const wchar_t* detail) { notices.push_back(std::wstring(title) + L"|" + detail); }

// ---- an in-test plugin ----
const PbzApi* api = nullptr;
PbzPlugin* me = nullptr;
int answer = 0, decides = 0, runs = 0, ticks = 0, events = 0, enabled_calls = 0;
std::string last_event_text;
int32_t PBZ_CALL Pick(void*, const PbzNodeCall* call) { ++decides; return call->output_count == 2 ? answer : -1; }
void PBZ_CALL Act(void*, const PbzNodeCall* call) { ++runs; api->press(me, PBZ_BUTTON_HEAVY); (void)call; }
void PBZ_CALL OnEvent(void*, const PbzEvent* e) { ++events; last_event_text = e->text; }
void PBZ_CALL OnTick(void*, uint64_t) { ++ticks; }
void PBZ_CALL OnEnabled(void*, int32_t) { ++enabled_calls; }
int32_t PBZ_CALL OnAbility(void*, const PbzAbilityEvent* e) { return std::strcmp(e->name, "GA_Test_Air_A") == 0 ? PBZ_CONSUME : PBZ_PASS; }
int32_t signal_code = 0;
PbzResult PBZ_CALL TestMain(const PbzApi* a, PbzPlugin* self) {
    api = a; me = self;
    PbzPluginInfo info{}; info.size = sizeof(info); info.id = "test"; info.name = "测试插件"; info.version = "1"; info.min_api = 1;
    if (api->describe(self, &info) != PBZ_OK) return PBZ_E_STATE;
    PbzSignalDesc sig{}; sig.size = sizeof(sig); sig.id = "go"; sig.name = "出发";
    if (api->register_signal(self, &sig, &signal_code) != PBZ_OK) return PBZ_E_STATE;
    static const char* const outs[] = {"左", "右"};
    PbzActionDesc pick{}; pick.size = sizeof(pick); pick.id = "pick"; pick.name = "选边"; pick.kind = PBZ_ACTION_DECIDE; pick.output_count = 2; pick.outputs = outs; pick.decide = Pick;
    pick.has_value = 1; pick.value_label = "数"; pick.value_min = 0; pick.value_max = 10; pick.value_step = 2; pick.value_default = 4;
    PbzActionDesc act{}; act.size = sizeof(act); act.id = "act"; act.name = "动作"; act.kind = PBZ_ACTION_RUN; act.run = Act;
    if (api->register_action(self, &pick) != PBZ_OK || api->register_action(self, &act) != PBZ_OK) return PBZ_E_STATE;
    PbzCallbacks cb{}; cb.size = sizeof(cb); cb.on_event = OnEvent; cb.on_tick = OnTick; cb.on_enabled = OnEnabled; cb.on_hero_ability = OnAbility;
    return api->set_callbacks(self, &cb);
}
// A plugin whose decide faults: switched off, the chain goes on.
const PbzApi* bad_api = nullptr;
int32_t PBZ_CALL Crash(void*, const PbzNodeCall*) { volatile int* p = nullptr; return *p; }
PbzResult PBZ_CALL BadMain(const PbzApi* a, PbzPlugin* self) {
    bad_api = a;
    PbzPluginInfo info{}; info.size = sizeof(info); info.id = "bad"; info.name = "坏插件"; info.min_api = 1;
    a->describe(self, &info);
    static const char* const outs[] = {"一", "二"};
    PbzActionDesc d{}; d.size = sizeof(d); d.id = "crash"; d.name = "崩溃"; d.kind = PBZ_ACTION_DECIDE; d.output_count = 2; d.outputs = outs; d.decide = Crash;
    return a->register_action(self, &d);
}
PbzResult PBZ_CALL NoDescribe(const PbzApi*, PbzPlugin*) { return PBZ_OK; }

std::string Field(PbzEdit e, int node, int row, const char* name) {
    char text[512]{}; int32_t needed = 0;
    return api->edit_get_field(me, e, node, row, name, text, sizeof(text), &needed) == PBZ_OK ? std::string(text) : std::string("<none>");
}
std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{}; const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, n); return dir.substr(0, dir.find_last_of(L"\\/"));
}
// ---- revision 2: features, permissions, modules, events, commands, UI pages ----
const PbzApi* papi = nullptr;
PbzPlugin* pme = nullptr;
std::vector<std::string> pm_events, ui_events;
uint32_t caps_seen = 0;
void PBZ_CALL PmEvent(void*, const PbzModuleEvent* e) { pm_events.push_back(std::string(e->module) + "|" + e->event + "|" + e->text); }
void PBZ_CALL PmUi(void*, const PbzUiEvent* e) { ui_events.push_back(std::string(e->page) + "|" + e->item + "|" + std::to_string(e->kind) + "|" + std::to_string(static_cast<int>(e->value))); }
void PBZ_CALL PmCaps(void*, uint32_t granted) { caps_seen = granted; }
PbzResult PBZ_CALL PermMain(const PbzApi* a, PbzPlugin* self) {
    papi = a; pme = self;
    PbzPluginInfoEx info{}; info.size = sizeof(info); info.id = "perm"; info.name = "权限插件"; info.version = "1"; info.min_api = 1; info.min_revision = 2;
    info.caps = PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_INI | PBZ_CAP_COMMAND | PBZ_CAP_UI; info.caps_reason = "测试用";
    if (a->describe_ex(self, &info) != PBZ_OK) return PBZ_E_STATE;
    PbzCallbacks cb{}; cb.size = sizeof(cb); cb.on_module_event = PmEvent; cb.on_ui = PmUi; cb.on_caps = PmCaps;
    return a->set_callbacks(self, &cb);
}
PbzResult PBZ_CALL FutureMain(const PbzApi* a, PbzPlugin* self) {
    PbzPluginInfoEx info{}; info.size = sizeof(info); info.id = "future"; info.name = "需要新接口"; info.min_api = 1; info.min_revision = 99;
    return a->describe_ex(self, &info);
}
PbzResult PBZ_CALL PersistMain(const PbzApi* a, PbzPlugin* self) {
    PbzPluginInfoEx info{}; info.size = sizeof(info); info.id = "persist"; info.name = "保存的授权"; info.min_api = 1; info.caps = PBZ_CAP_READ | PBZ_CAP_WRITE;
    return a->describe_ex(self, &info);
}
// A plugin whose on_ui faults: its pages go with it.
void PBZ_CALL BadUi(void*, const PbzUiEvent*) { volatile int* p = nullptr; *p = 1; }
PbzResult PBZ_CALL UiBadMain(const PbzApi* a, PbzPlugin* self) {
    PbzPluginInfoEx info{}; info.size = sizeof(info); info.id = "uibad"; info.name = "坏界面"; info.min_api = 1; info.caps = PBZ_CAP_UI;
    if (a->describe_ex(self, &info) != PBZ_OK) return PBZ_E_STATE;
    PbzUiItem btn{}; btn.size = sizeof(btn); btn.kind = PBZ_UI_BUTTON; btn.id = "go"; btn.label = "点";
    const PbzUiItem* items[] = {&btn}; PbzUiPage page{}; page.size = sizeof(page); page.id = "p"; page.title = "页"; page.items = items; page.item_count = 1;
    if (a->ui_page(self, &page) != PBZ_OK) return PBZ_E_STATE;
    PbzCallbacks cb{}; cb.size = sizeof(cb); cb.on_ui = BadUi;
    return a->set_callbacks(self, &cb);
}

bool fm_flag = false; int fm_level = 3; std::string fm_title = "abc"; int fm_ticks = 0;
std::string PGet(const char* module, const char* prop, PbzResult* rc = nullptr) {
    char text[256]{}; int32_t needed = 0;
    const PbzResult r = papi->prop_get(pme, module, prop, text, sizeof(text), &needed);
    if (rc) *rc = r;
    return r == PBZ_OK ? std::string(text) : "<" + std::to_string(r) + ">";
}
int CountLogs(const char* needle) { int n = 0; for (const std::string& l : logs) if (l.find(needle) != std::string::npos) ++n; return n; }
bool Seen(const std::vector<std::string>& list, const std::string& what) { for (const std::string& s : list) if (s == what) return true; return false; }

void RevisionTwo() {
    ComboPlugins::SetEnabled(true);
    // the fake modules and commands the plugin works on
    ModRegistry::ModuleBuilder("fakemod", "假模块", "测试用", ModRegistry::Thread::Game)
        .Bool("flag", "开关", [] { return fm_flag; }, [](bool v) { fm_flag = v; }, "Fake.Flag")
        .Int("level", "等级", 1, 10, 3, [] { return fm_level; }, [](int v) { fm_level = v; }, "Fake.Level")
        .Text("title", "标题", [] { return fm_title; }, [](const std::string& v) { fm_title = v; })
        .ReadOnlyInt("ticks", "计数", [] { return fm_ticks; })
        .Action("echo", "回声", "文字", false, [](const std::string& arg, std::string& out) { out = "echo:" + arg; return true; })
        .Event("changed", "变了", "属性名")
        .Commit();
    static bool poll_emit = false;
    ModRegistry::AddPoller("fakemod", [] { if (poll_emit) { poll_emit = false; ModRegistry::Emit("fakemod", "changed", "from-poller"); } });
    ModRegistry::Command status; status.name = "fakemod.status"; status.args = "[详细]"; status.kind = ModRegistry::Kind::Diagnostic; status.thread = ModRegistry::Thread::Game;
    status.run = [](const std::string& args, std::string& out) { out = "status:" + args; return true; };
    ModRegistry::Command save; save.name = "workmod.save"; save.kind = ModRegistry::Kind::Persist; save.thread = ModRegistry::Thread::Worker;
    save.run = [](const std::string&, std::string& out) { out = "saved"; return true; };
    ModRegistry::RegisterCommand(status); ModRegistry::RegisterCommand(save);

    // ---- revision and features ----
    Check(PBZ_API_HAS(api, api_revision) && api->api_revision == PBZ_API_REVISION && PBZ_API_REVISION_OF(api) == 3 && std::string(api->mod_version) == "20261113", "the API table carries api_revision and the MOD version");
    Check(api->has_feature("hero_size_node") == 1 && api->has_feature("modules") == 1 && api->has_feature("module_events") == 1 && api->has_feature("permissions") == 1 && api->has_feature("commands") == 1 &&
          api->has_feature("ui_pages") == 1 && api->has_feature("no_such_thing") == 0 && api->has_feature(nullptr) == 0, "has_feature: what this MOD has");
    {   PbzApi older = *api; older.size = static_cast<uint32_t>(offsetof(PbzApi, api_revision));
        Check(PBZ_API_REVISION_OF(&older) == 1 && !PBZ_API_HAS(&older, has_feature) && PBZ_API_HAS(&older, set_setting), "a table from before revision 2 reads as revision 1 without the new functions"); }
    Check(api->granted_caps(me) == PBZ_CAP_COMBO, "a plugin that only calls describe has the combo capability");

    // ---- describe_ex ----
    ComboPlugins::SetGrant("persist", PBZ_CAP_READ | PBZ_CAP_COMMAND);  // before it loads: only what it asks for counts
    Check(ComboPlugins::AddForTest(FutureMain, L"future.dll") != PBZ_OK, "min_revision above the MOD's: the plugin is not used");
    bool future_refused = false;
    for (const auto& p : ComboPlugins::Plugins()) if (p.file == L"future.dll") future_refused = !p.loaded && p.error.find(L"99") != std::wstring::npos;
    Check(future_refused, "...and the player is told which revision it needs");
    Check(ComboPlugins::AddForTest(PersistMain, L"persist.dll") == PBZ_OK, "a plugin loads");
    uint32_t persist_caps = 0, persist_requested = 0;
    for (const auto& p : ComboPlugins::Plugins()) if (p.id == "persist") { persist_caps = p.granted; persist_requested = p.requested; }
    Check(persist_caps == (PBZ_CAP_COMBO | PBZ_CAP_READ) && persist_requested == (PBZ_CAP_COMBO | PBZ_CAP_READ | PBZ_CAP_WRITE), "a saved grant applies at load, limited to what the plugin asked for (command was not asked)");
    Check(ComboPlugins::AddForTest(PermMain, L"perm.dll") == PBZ_OK && papi->granted_caps(pme) == PBZ_CAP_COMBO, "describe_ex: nothing but the combo capability until the player allows more");
    {   bool shown = false;
        for (const auto& p : ComboPlugins::Plugins()) if (p.id == "perm") shown = p.requested == (PBZ_CAP_COMBO | PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_INI | PBZ_CAP_COMMAND | PBZ_CAP_UI) && p.caps_reason == L"测试用" && p.revision == 2;
        Check(shown, "the plugin list shows what it asked for, and why"); }

    // ---- not allowed: every call returns PBZ_E_DENIED ----
    char text[512]{}; int32_t needed = 0; PbzResult rc = PBZ_OK;
    Check(papi->module_list(text, sizeof(text), &needed) == PBZ_OK && std::string(text).find("fakemod\t假模块\tgame\t4\t1\t1\t测试用") != std::string::npos && std::string(text).find("host\t") != std::string::npos,
          "module_list needs no permission (it lists the host and the fake module)");
    Check(papi->prop_list("fakemod", text, sizeof(text), &needed) == PBZ_OK && papi->action_list("fakemod", text, sizeof(text), &needed) == PBZ_OK && papi->event_list("fakemod", text, sizeof(text), &needed) == PBZ_OK &&
          papi->command_list(text, sizeof(text), &needed) == PBZ_OK && std::string(text).find("fakemod.status") != std::string::npos && papi->prop_list("nope", text, sizeof(text), &needed) == PBZ_E_NOT_FOUND,
          "...nor do prop_list, action_list, event_list, command_list");
    Check(PGet("fakemod", "level", &rc) == "<-10>" && rc == PBZ_E_DENIED && papi->prop_set(pme, "fakemod", "level", "4") == PBZ_E_DENIED && papi->action_invoke(pme, "fakemod", "echo", "x", 0, text, sizeof(text), &needed) == PBZ_E_DENIED &&
          papi->module_subscribe(pme, "fakemod", "changed", 1) == PBZ_E_DENIED && papi->command(pme, "fakemod.status", 0, text, sizeof(text), &needed) == PBZ_E_DENIED, "reading, writing, actions, subscribing and commands are denied");
    {   PbzUiItem l{}; l.size = sizeof(l); l.kind = PBZ_UI_LABEL; l.id = "a"; l.text = "x"; const PbzUiItem* items[] = {&l}; PbzUiPage page{}; page.size = sizeof(page); page.id = "p"; page.items = items; page.item_count = 1;
        Check(papi->ui_page(pme, &page) == PBZ_E_DENIED, "a UI page is denied"); }
    Check(papi->set_setting(pme, "Elsewhere", "K", "1") == PBZ_E_DENIED && papi->set_setting(pme, "Plugin.perm", "K", "1") == PBZ_OK && papi->set_setting(pme, "NpcCombo", "PermTest", "1") == PBZ_OK,
          "set_setting: the plugin's own section and [NpcCombo] are open, others are denied");
    PGet("fakemod", "level"); papi->prop_set(pme, "fakemod", "level", "4");
    Check(CountLogs("PLUGIN [perm] denied: prop_get needs read") == 1 && CountLogs("PLUGIN [perm] denied: prop_set needs write") == 1, "a denial is logged once per plugin and call");

    // ---- read ----
    ComboPlugins::SetGrant("perm", PBZ_CAP_READ);
    Check(papi->granted_caps(pme) == (PBZ_CAP_COMBO | PBZ_CAP_READ), "the player's grant takes effect at once");
    ComboPlugins::Frame(20000);
    Check(caps_seen == (PBZ_CAP_COMBO | PBZ_CAP_READ), "on_caps tells the plugin at the next frame");
    Check(PGet("fakemod", "level") == "3" && PGet("fakemod", "flag") == "0" && PGet("fakemod", "title") == "abc" && PGet("fakemod", "ticks") == "0" && PGet("host", "api_revision") == "3" && PGet("host", "mod_version") == "20261113" &&
          PGet("host", "plugins_enabled") == "1", "prop_get with the read permission (module values and the host's own module)");
    {   PbzResult other = PBZ_OK; std::thread([&] { PGet("fakemod", "level", &other); }).join(); Check(other == PBZ_E_THREAD, "a Game module is only read on the game thread");
        std::thread([&] { PGet("host", "api_revision", &other); }).join(); Check(other == PBZ_OK, "an Any module from any thread"); }
    Check(PGet("fakemod", "nope", &rc) == "<-4>" && papi->prop_get(pme, "fakemod", "title", text, 2, &needed) == PBZ_E_BUFFER && needed == 4, "unknown properties; a short buffer says what is needed");

    // ---- write: queued to the game thread, applied at the next frame ----
    Check(papi->prop_set(pme, "fakemod", "level", "7") == PBZ_E_DENIED, "write is still denied with only the read permission");
    ComboPlugins::SetGrant("perm", PBZ_CAP_READ | PBZ_CAP_WRITE);
    Check(papi->prop_set(pme, "fakemod", "level", "7") == PBZ_OK && PGet("fakemod", "level") == "3", "prop_set queues: the module still has the old value");
    ComboPlugins::Frame(20016);
    Check(fm_level == 7 && PGet("fakemod", "level") == "7" && CountLogs("PLUGIN [perm] prop_set fakemod.level=7") == 1, "...the next frame calls the module's setter and logs it");
    Check(papi->prop_set(pme, "fakemod", "level", "11") == PBZ_E_ARG && papi->prop_set(pme, "fakemod", "level", "2.5") == PBZ_E_ARG && papi->prop_set(pme, "fakemod", "ticks", "1") == PBZ_E_ARG &&
          papi->prop_set(pme, "fakemod", "nope", "1") == PBZ_E_NOT_FOUND && papi->prop_set(pme, "nomod", "x", "1") == PBZ_E_NOT_FOUND && papi->prop_set(pme, "fakemod", "title", "a\nb") == PBZ_E_ARG,
          "bad values, read-only and unknown properties are refused at once");
    {   PbzResult other = PBZ_E_STATE; std::thread([&] { other = papi->prop_set(pme, "fakemod", "flag", "1"); }).join(); Check(other == PBZ_OK && !fm_flag, "any thread may write: it is queued");
        ComboPlugins::Frame(20032); Check(fm_flag, "...and the module's setter ran on the game thread's frame"); }
    papi->prop_set(pme, "fakemod", "level", "9");
    ComboPlugins::SetGrant("perm", PBZ_CAP_READ);  // taken back before the frame
    ComboPlugins::Frame(20048);
    Check(fm_level == 7 && CountLogs("prop_set fakemod.level skipped: no longer allowed") == 1, "a write queued before the player took the permission back is dropped");
    ComboPlugins::SetGrant("perm", PBZ_CAP_READ | PBZ_CAP_WRITE);

    // ---- actions ----
    Check(papi->action_invoke(pme, "fakemod", "echo", "hi", 0, text, sizeof(text), &needed) == PBZ_OK && std::string(text) == "echo:hi" && CountLogs("PLUGIN [perm] action_invoke fakemod.echo(hi) -> ok") == 1,
          "action_invoke runs at once on the game thread and returns text");
    Check(papi->action_invoke(pme, "fakemod", "echo", "hi", 0, nullptr, 0, nullptr) == PBZ_OK && papi->action_invoke(pme, "fakemod", "echo", "hi", 0, text, 3, &needed) == PBZ_E_BUFFER && needed == 8 &&
          papi->action_invoke(pme, "fakemod", "none", "", 0, text, sizeof(text), &needed) == PBZ_E_NOT_FOUND && papi->action_invoke(pme, "fakemod", "echo", "", 4, text, sizeof(text), &needed) == PBZ_E_ARG,
          "no buffer: the text is dropped; a short one reports the size; unknown action; unknown flags");
    {   PbzResult other = PBZ_OK; std::thread([&] { other = papi->action_invoke(pme, "fakemod", "echo", "x", 0, text, sizeof(text), &needed); }).join(); Check(other == PBZ_E_THREAD, "actions from another thread need PBZ_CALL_LATER"); }
    pm_events.clear();
    {   PbzResult other = PBZ_E_STATE; std::thread([&] { other = papi->action_invoke(pme, "fakemod", "echo", "later", PBZ_CALL_LATER, nullptr, 0, nullptr); }).join(); Check(other == PBZ_OK && pm_events.empty(), "PBZ_CALL_LATER queues the action"); }
    ComboPlugins::Frame(20064); ComboPlugins::Frame(20080);
    Check(Seen(pm_events, "host|action_done|ok\tfakemod.echo later\techo:later"), "its result comes back as host/action_done (no subscription needed)");

    // ---- module events ----
    Check(papi->module_subscribe(pme, "fakemod", "changed", 1) == PBZ_OK && papi->module_subscribe(pme, "fakemod", "nope", 1) == PBZ_E_NOT_FOUND && papi->module_subscribe(pme, "nomod", nullptr, 1) == PBZ_E_NOT_FOUND,
          "module_subscribe checks the module and event");
    pm_events.clear();
    ModRegistry::Emit("fakemod", "changed", "level");
    const bool at_once = !pm_events.empty();
    ComboPlugins::Frame(20096);
    Check(!at_once && Seen(pm_events, "fakemod|changed|level"), "an event a module raises reaches the subscriber at the next frame");
    pm_events.clear(); ModRegistry::Emit("fakemod", "other", "x"); ModRegistry::Emit("host", "command_done", "x");
    ComboPlugins::Frame(20128); ComboPlugins::Frame(20144);
    Check(pm_events.empty(), "other events are not delivered");
    Check(papi->module_subscribe(pme, nullptr, nullptr, 1) == PBZ_OK && papi->module_subscribe(pme, "fakemod", "changed", 0) == PBZ_OK, "subscribe to everything; take one back");
    ModRegistry::Emit("fakemod", "changed", "a"); ModRegistry::Emit("fakemod", "other", "b");
    ComboPlugins::Frame(20160);
    Check(Seen(pm_events, "fakemod|changed|a") && Seen(pm_events, "fakemod|other|b"), "a wildcard subscription gets them all");
    pm_events.clear(); poll_emit = true;
    ComboPlugins::Frame(20176); ComboPlugins::Frame(20192);
    Check(Seen(pm_events, "fakemod|changed|from-poller"), "an event a module's poller raises (from its snapshot, on the game thread) reaches the subscriber");
    ComboPlugins::SetGrant("perm", PBZ_CAP_WRITE);  // no read any more
    pm_events.clear(); ModRegistry::Emit("fakemod", "changed", "c");
    ComboPlugins::Frame(20192); ComboPlugins::Frame(20208);
    Check(pm_events.empty(), "without the read permission nothing is delivered");
    ComboPlugins::SetGrant("perm", PBZ_CAP_READ | PBZ_CAP_WRITE);

    // ---- the command bus ----
    Check(papi->command(pme, "fakemod.status verbose", 0, text, sizeof(text), &needed) == PBZ_E_DENIED, "commands need their own permission");
    ComboPlugins::SetGrant("perm", PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_COMMAND);
    Check(papi->command(pme, "fakemod.status  verbose now ", 0, text, sizeof(text), &needed) == PBZ_OK && std::string(text) == "status:verbose now" && CountLogs("PLUGIN [perm] command fakemod.status  verbose now") == 1,
          "command: a whitelisted game command runs at once, its text comes back, and the call is logged");
    Check(papi->command(pme, "nothing.here", 0, text, sizeof(text), &needed) == PBZ_E_NOT_FOUND && papi->command(pme, "rm -rf", 0, text, sizeof(text), &needed) == PBZ_E_NOT_FOUND && papi->command(pme, "fakemod.status", 9, text, sizeof(text), &needed) == PBZ_E_ARG,
          "only the whitelist runs");
    Check(papi->command(pme, "workmod.save", 0, nullptr, 0, nullptr) == PBZ_E_DENIED, "a command that writes ini or files also needs PBZ_CAP_INI");
    ComboPlugins::SetGrant("perm", PBZ_CAP_ALL);
    pm_events.clear();
    Check(papi->command(pme, "workmod.save", 0, nullptr, 0, nullptr) == PBZ_OK && pm_events.empty(), "a command that belongs to the panel thread is queued for it");
    Check(ComboPlugins::PumpWorker() == 1, "the panel thread's turn runs it");
    ComboPlugins::Frame(20224); ComboPlugins::Frame(20240);
    Check(Seen(pm_events, "host|command_done|ok\tworkmod.save\tsaved"), "its answer is host/command_done at the next frame");
    pm_events.clear();
    {   PbzResult other = PBZ_E_STATE; std::thread([&] { other = papi->command(pme, "fakemod.status later", PBZ_CALL_LATER, nullptr, 0, nullptr); }).join(); Check(other == PBZ_OK && pm_events.empty(), "PBZ_CALL_LATER queues a game command from any thread"); }
    ComboPlugins::Frame(20256); ComboPlugins::Frame(20272);
    Check(Seen(pm_events, "host|command_done|ok\tfakemod.status later\tstatus:later"), "...and answers the same way");
    papi->command(pme, "workmod.save", 0, nullptr, 0, nullptr);
    ComboPlugins::SetGrant("perm", PBZ_CAP_READ);
    pm_events.clear();
    ComboPlugins::PumpWorker(); ComboPlugins::SetGrant("perm", PBZ_CAP_ALL); ComboPlugins::Frame(20288); ComboPlugins::Frame(20304);
    Check(Seen(pm_events, "host|command_done|fail\tworkmod.save\tdenied"), "a queued panel-thread command is refused if the permission was taken back meanwhile");

    // ---- settings with PBZ_CAP_INI ----
    Check(papi->set_setting(pme, "Elsewhere", "K", "1") == PBZ_OK && papi->get_setting("Elsewhere", "K", text, sizeof(text), &needed) == PBZ_OK && std::string(text) == "1" &&
          papi->set_setting(pme, "Plugins", "AllowUe", "1") == PBZ_E_ARG && papi->set_setting(pme, "PluginGrants", "perm", "ue") == PBZ_E_ARG, "with PBZ_CAP_INI other sections are writable; the player's switches still are not");

    // ---- UI pages ----
    ComboPlugins::SetGrant("perm", PBZ_CAP_READ | PBZ_CAP_WRITE);
    {
        PbzUiItem toggle{}; toggle.size = sizeof(toggle); toggle.kind = PBZ_UI_TOGGLE; toggle.id = "flag"; toggle.label = "开关"; toggle.bind = "fakemod.flag";
        PbzUiItem slider{}; slider.size = sizeof(slider); slider.kind = PBZ_UI_SLIDER; slider.id = "level"; slider.label = "等级"; slider.bind = "fakemod.level";
        PbzUiItem title{}; title.size = sizeof(title); title.kind = PBZ_UI_LABEL; title.id = "title"; title.label = "标题"; title.bind = "fakemod.title";
        PbzUiItem own{}; own.size = sizeof(own); own.kind = PBZ_UI_SLIDER; own.id = "own"; own.label = "自己的"; own.min = 0; own.max = 100; own.step = 5; own.value = 20;
        PbzUiItem btn{}; btn.size = sizeof(btn); btn.kind = PBZ_UI_BUTTON; btn.id = "go"; btn.label = "执行";
        PbzUiItem echo{}; echo.size = sizeof(echo); echo.kind = PBZ_UI_BUTTON; echo.id = "echo"; echo.label = "回声"; echo.bind = "fakemod:echo"; echo.text = "from-ui";
        const PbzUiItem* items[] = {&toggle, &slider, &title, &own, &btn, &echo};
        PbzUiPage page{}; page.size = sizeof(page); page.id = "main"; page.title = "测试页"; page.items = items; page.item_count = 6;
        Check(papi->ui_page(pme, &page) == PBZ_E_DENIED, "a page needs PBZ_CAP_UI");
        ComboPlugins::SetGrant("perm", PBZ_CAP_ALL);
        Check(papi->ui_page(pme, &page) == PBZ_OK && ComboPlugins::UiPages().size() == 1 && ComboPlugins::UiPages()[0].owner == "perm" && ComboPlugins::UiPages()[0].owner_name == L"权限插件" && ComboPlugins::UiPages()[0].items.size() == 6,
              "ui_page declares a page; the panel reads it");
        {   PbzResult other = PBZ_OK; std::thread([&] { other = papi->ui_page(pme, &page); }).join(); Check(other == PBZ_E_THREAD, "pages are declared on the game thread"); }
        ComboPlugins::Frame(30000);
        PluginUi::Item it;
        Check(PluginUi::FindItem("perm", "main", "level", it) && it.value == 7 && PluginUi::FindItem("perm", "main", "flag", it) && it.value == 1 && PluginUi::FindItem("perm", "main", "title", it) && it.text == L"abc",
              "bound controls show the module's live values (refreshed on the game thread)");
        fm_level = 5; ComboPlugins::Frame(30050);
        Check(PluginUi::FindItem("perm", "main", "level", it) && it.value == 7, "...a few times a second, not every frame");
        ComboPlugins::Frame(30300);
        Check(PluginUi::FindItem("perm", "main", "level", it) && it.value == 5, "...so a change shows within a fraction of a second");
        ComboPlugins::UiInput("perm", "main", "level", 8);
        Check(PluginUi::FindItem("perm", "main", "level", it) && it.value == 8 && fm_level == 5, "the control shows the player's move at once; the module is written at the next frame");
        ComboPlugins::Frame(30316);
        Check(fm_level == 8 && CountLogs("PLUGIN [perm] prop_set fakemod.level=8") == 1, "...through the plugin's write permission, logged");
        ComboPlugins::UiInput("perm", "main", "level", 99);
        ComboPlugins::UiInput("perm", "main", "flag", 0);
        ComboPlugins::Frame(30332);
        Check(fm_level == 8 && !fm_flag, "a value outside the property's range is dropped; a toggle writes 0 / 1");
        ui_events.clear();
        ComboPlugins::UiInput("perm", "main", "own", 35); ComboPlugins::UiInput("perm", "main", "go", 1); ComboPlugins::UiInput("perm", "main", "title", 1); ComboPlugins::UiInput("perm", "main", "nope", 1);
        Check(ui_events.empty(), "controls the plugin handles wait for the frame");
        ComboPlugins::Frame(30348);
        Check(ui_events.size() == 2 && ui_events[0] == "main|own|" + std::to_string(PBZ_UI_SLIDER) + "|35" && ui_events[1] == "main|go|" + std::to_string(PBZ_UI_BUTTON) + "|1",
              "on_ui gets the unbound controls (a label and an unknown item do nothing)");
        pm_events.clear();
        ComboPlugins::UiInput("perm", "main", "echo", 1);
        ComboPlugins::Frame(30364); ComboPlugins::Frame(30380);
        Check(CountLogs("PLUGIN [perm] action_invoke fakemod.echo(from-ui) -> ok") == 1 && Seen(pm_events, "host|action_done|ok\tfakemod.echo from-ui\techo:from-ui"), "a button bound to an action runs it with the button's text");
        Check(papi->ui_set_value(pme, "main", "own", 55) == PBZ_OK && papi->ui_set_text(pme, "main", "title", "x") == PBZ_OK && PluginUi::FindItem("perm", "main", "own", it) && it.value == 55 && papi->ui_set_value(pme, "main", "nope", 1) == PBZ_E_NOT_FOUND,
              "ui_set_value / ui_set_text update what the page shows");
        ComboPlugins::SetGrant("perm", PBZ_CAP_READ);  // no write
        const int writes = CountLogs("prop_set fakemod.level=");
        ComboPlugins::UiInput("perm", "main", "level", 2); ComboPlugins::Frame(30396);
        Check(fm_level == 8 && CountLogs("prop_set fakemod.level=") == writes && CountLogs("PLUGIN [perm] denied: ui (bound control) needs write") == 1, "a bound control writes only while the plugin has the write permission");
        ComboPlugins::SetGrant("perm", PBZ_CAP_ALL);
        Check(papi->ui_remove_page(pme, "main") == PBZ_OK && ComboPlugins::UiPages().empty() && papi->ui_remove_page(pme, "main") == PBZ_E_NOT_FOUND, "ui_remove_page");
    }
    {   // a plugin whose on_ui faults loses its pages and is switched off
        ComboPlugins::SetGrant("uibad", PBZ_CAP_UI);
        Check(ComboPlugins::AddForTest(UiBadMain, L"uibad.dll") == PBZ_OK && ComboPlugins::UiPages().size() == 1, "a page declared during the entry point");
        ComboPlugins::UiInput("uibad", "p", "go", 1); ComboPlugins::Frame(30500);
        bool faulted = false; for (const auto& p : ComboPlugins::Plugins()) if (p.id == "uibad") faulted = p.faulted;
        Check(faulted && ComboPlugins::UiPages().empty(), "a fault in on_ui switches the plugin off and its pages go");
    }

    // ---- off ----
    ComboPlugins::SetEnabled(false);
    Check(papi->prop_set(pme, "fakemod", "level", "4") == PBZ_E_DISABLED && papi->command(pme, "fakemod.status", 0, text, sizeof(text), &needed) == PBZ_E_DISABLED &&
          papi->action_invoke(pme, "fakemod", "echo", "", 0, text, sizeof(text), &needed) == PBZ_E_DISABLED, "turned off: writes, commands and actions are refused");
    ModRegistry::Emit("fakemod", "changed", "off"); ComboPlugins::Frame(31000);
    ComboPlugins::SetEnabled(true);
    pm_events.clear(); ComboPlugins::Frame(31016); ComboPlugins::Frame(31032);
    Check(pm_events.empty(), "events raised while it was off are dropped");
    ModRegistry::ResetForTest(); ComboPlugins::RegisterModules();
    enabled_calls = 0;  // the off / on above was this block's, not the one the rest of the test counts
    notices.clear();
}

}  // namespace

int main() {
    // ---- the model ----
    const int code = NpcCombo::SignalKeyCode("test/go");
    Check(NpcCombo::IsSignalKey(code) && NpcCombo::ValidRawKey(code) && code == NpcCombo::SignalKeyCode("test/go") && code != NpcCombo::SignalKeyCode("test/stop"),
          "a signal's key code is its id's hash, in the signal range, a valid 自定义按键 key");
    Check(std::string(NpcCombo::RawKeyToken(code)).rfind("sig:0x", 0) == 0, "a signal key is logged as sig:0x....");
    Check(NpcCombo::ValidActionId("a.b/c-d_1") && !NpcCombo::ValidActionId("nope") && !NpcCombo::ValidActionId("a/b/c") && !NpcCombo::ValidActionId("中/文"), "action ids: plugin/action");

    chain = {};
    chain.chain.push_back(NpcCombo::TriggerNode(Trigger::Light, 0, 0)); chain.chain[0].next = 1;
    { Node p; p.type = NodeType::Plugin; p.action = "test/pick"; p.action_kind = NpcCombo::kActionDecide; p.value = 6; p.routes.resize(2);
      p.routes[0].button = 0; p.routes[0].next = 2; p.routes[1].button = 0; p.routes[1].next = 3; chain.chain.push_back(p); }
    { Node s; s.type = NodeType::Skill; s.ability = "GA_A"; chain.chain.push_back(s); }
    { Node s; s.type = NodeType::Skill; s.ability = "GA_B"; chain.chain.push_back(s); }
    { Node p; p.type = NodeType::Plugin; p.action = "test/act"; p.value = -3; chain.chain.push_back(p); }
    { Node k = NpcCombo::TriggerNode(Trigger::Keys, 0, 0); k.keys = {code}; k.next = 4; chain.chain.push_back(k); }
    NpcCombo::EnsureKey(chain);
    const std::wstring text = NpcCombo::Encode(chain);
    const NpcCombo::Config back = NpcCombo::Decode(text);
    Check(text.find(L"|p:1,1,6,test/pick,") != std::wstring::npos && text.find(L"|p:4,0,-3,test/act,") != std::wstring::npos, "plugin nodes are saved as p: marks");
    Check(back.chain.size() == chain.chain.size() && back.chain[1].type == NodeType::Plugin && back.chain[1].action == "test/pick" && back.chain[1].action_kind == NpcCombo::kActionDecide &&
          back.chain[1].value == 6 && back.chain[1].routes.size() == 2 && back.chain[1].routes[0].next == 2 && back.chain[1].routes[1].next == 3,
          "a 判断 node round-trips: action, kind, value, its outputs' wires");
    Check(back.chain[4].type == NodeType::Plugin && back.chain[4].action == "test/act" && back.chain[4].value == -3 && back.chain[4].routes.empty() &&
          NpcCombo::PluginRuns(back.chain[4]) && NpcCombo::IsMove(back.chain[4]), "a 动作 node round-trips and plays like a move");
    Check(back.chain[5].keys.size() == 1 && back.chain[5].keys[0] == code, "a signal key survives in a 自定义按键 start");
    {   // an older build reads the plugin node as an unwired 间隔 (the p: mark is ignored)
        std::wstring old = text; size_t at;
        while ((at = old.find(L"|p:")) != std::wstring::npos) { const size_t end = old.find(L'|', at + 1); old.erase(at, end == std::wstring::npos ? std::wstring::npos : end - at); }
        const NpcCombo::Config o = NpcCombo::Decode(old);
        Check(o.chain.size() == chain.chain.size() && o.chain[1].type == NodeType::Interval && o.chain[1].next < 0, "without the mark: an unwired 间隔 (old builds)");
    }
    Check(!NpcCombo::Groupable(back.chain[4]) && !NpcCombo::TakesModifiers(back.chain[4]), "a plugin node is never in a 组合 and takes no modifier");

    // ---- the host ----
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"pbz_plugins_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    SettingsStore::Initialize((root + L"\\PBZDebugMenu.ini").c_str());
    NpcCombo::SetFolder(root + L"\\combos");
    ComboPlugins::Bridge bridge;
    bridge.config = BridgeConfig; bridge.state = BridgeState; bridge.hero = BridgeHero; bridge.press = BridgePress; bridge.reset = BridgeReset;
    bridge.cancel = BridgeCancel; bridge.watched = BridgeWatched; bridge.trigger = BridgeTrigger; bridge.play = BridgePlay;
    ComboPlugins::Initialize(bridge, Sink, Notice);
    Check(NpcCombo::PluginDecide == &ComboPlugins::Decide, "the model asks the host at 判断 nodes");
    Check(!ComboPlugins::Enabled(), "派生插件 is off without an ini value");
    ComboPlugins::SetEnabled(true);
    Check(ComboPlugins::AddForTest(TestMain, L"test.dll") == PBZ_OK && signal_code == code, "a plugin loads; its signal's key code is the hash of plugin/signal");
    Check(ComboPlugins::AddForTest(NoDescribe, L"nodescribe.dll") != PBZ_OK, "a plugin that does not describe itself is refused");
    Check(ComboPlugins::Actions().size() == 2 && ComboPlugins::Signals().size() == 1 && ComboPlugins::SignalName(code) == L"出发", "its actions and signals are listed for the page");

    ComboPlugins::Frame(1000);
    Check(ticks == 1, "on_tick once a frame");
    NpcCombo::Press light; light.bits = 1u << static_cast<int>(Trigger::Light);
    answer = 1;
    NpcCombo::Hop hop = NpcCombo::Follow(chain, 0, static_cast<int>(Trigger::Light), light, 0);
    Check(hop.move == 3, "a press through a 判断 node goes on through the output the plugin picks");
    answer = 0;
    hop = NpcCombo::Follow(chain, 0, static_cast<int>(Trigger::Light), light, 0);
    Check(hop.move == 3 && decides == 1, "within a frame the answer is kept (asked once)");
    ComboPlugins::Frame(1016);
    hop = NpcCombo::Follow(chain, 0, static_cast<int>(Trigger::Light), light, 0);
    Check(hop.move == 2 && decides == 2, "the next frame asks again");
    answer = 5;
    ComboPlugins::Frame(1032);
    hop = NpcCombo::Follow(chain, 0, static_cast<int>(Trigger::Light), light, 0);
    Check(hop.move < 0, "an answer outside the outputs passes nothing");
    {   int other = -2;
        std::thread([&] { other = ComboPlugins::Decide(chain, 1, light, 1); }).join();
        Check(other == -1, "off the game thread the last answer is given, the plugin is not called"); }

    ComboPlugins::Run(chain, 4, 1, light.bits, false, "Hero");
    ComboPlugins::Frame(1048);
    Check(runs == 1 && presses.size() == 1 && presses[0] == "2@plugin:test", "a 动作 node's run is called; its press goes into the chain the next frame");

    PbzEvent e{}; e.size = sizeof(e); e.type = PBZ_EVENT_CHAIN_START; e.text = "hero-action"; e.ability = ""; e.boss = "";
    ComboPlugins::Emit(e);
    Check(events == 0, "events wait for the frame");
    ComboPlugins::Frame(1064);
    Check(events == 1 && last_event_text == "hero-action", "events are delivered with their text copied");

    Check(api->fire_signal(me, "go") == PBZ_OK && !ComboPlugins::SignalDown(code), "fire_signal: nothing before the frame");
    ComboPlugins::Frame(1080); const bool down1 = ComboPlugins::SignalDown(code);
    ComboPlugins::Frame(1096); const bool down2 = ComboPlugins::SignalDown(code);
    Check(down1 && !down2, "fire_signal: down for one frame, then up");
    api->fire_signal(me, "go"); api->fire_signal(me, "go");
    bool seq[4]; for (bool& d : seq) { ComboPlugins::Frame(2000); d = ComboPlugins::SignalDown(code); }
    Check(seq[0] && !seq[1] && seq[2] && !seq[3], "two fires are two presses (down, up, down, up)");
    api->set_signal(me, "go", 1); ComboPlugins::Frame(2100); ComboPlugins::Frame(2116);
    const bool held = ComboPlugins::SignalDown(code);
    api->set_signal(me, "go", 0); ComboPlugins::Frame(2132);
    Check(held && !ComboPlugins::SignalDown(code), "set_signal holds the key down until released");
    watched_code = code;
    Check(api->signal_wanted(me, "go") == 1 && api->fire_signal(me, "missing") == PBZ_E_NOT_FOUND, "signal_wanted asks the runtime; an unknown signal is refused");

    PbzAbilityEvent ab{}; ab.size = sizeof(ab); ab.name = "GA_Test_Air_A"; ab.handle = 77;
    Check(ComboPlugins::HeroAbility(ab) == PBZ_CONSUME, "on_hero_ability can consume a hero ability");
    ab.name = "GA_Other";
    Check(ComboPlugins::HeroAbility(ab) == PBZ_PASS && api->cancel_hero_ability(77) == PBZ_OK && api->cancel_hero_ability(5) == PBZ_E_NOT_FOUND, "others pass; cancel_hero_ability asks the runtime");

    PbzChainState st{}; st.size = sizeof(st);
    Check(api->get_state(&st) == PBZ_OK && st.ready == 1 && std::string(st.table) == "Hero", "get_state");
    { PbzChainState small{}; small.size = 8; small.trigger = 99;
      Check(api->get_state(&small) == PBZ_OK && small.size == 8 && small.trigger == 99, "an older, smaller struct gets only its own bytes"); }
    PbzNodeInfo ni{}; ni.size = sizeof(ni);
    Check(api->node_count() == static_cast<int32_t>(chain.chain.size()) && api->get_node(1, &ni) == PBZ_OK && ni.type == PBZ_NODE_PLUGIN && ni.value == 6 &&
          ni.action_kind == PBZ_ACTION_DECIDE && std::string(ni.action) == "test/pick" && ni.route_count == 2, "get_node reads a plugin node");
    PbzRouteInfo ri{}; ri.size = sizeof(ri);
    Check(api->get_route(1, 1, &ri) == PBZ_OK && ri.next == 3 && ri.button == -1, "get_route reads its outputs");
    { PbzResult r = PBZ_OK; std::thread([&] { r = api->get_state(&st); }).join(); Check(r == PBZ_E_THREAD, "game-thread functions refuse other threads"); }
    hero = PbzHeroFacts{}; hero.valid = 1; hero.health = 30; hero.health_max = 100;
    PbzHeroFacts hf{}; hf.size = sizeof(hf);
    Check(api->get_hero(&hf) == PBZ_OK && hf.valid && hf.health == 30, "get_hero");

    Check(api->play_node(me, 2, -1) == PBZ_OK && api->press(me, PBZ_BUTTON_ANY_DODGE) == PBZ_E_ARG && api->press(me, PBZ_BUTTON_KEYS) == PBZ_E_ARG, "play_node queued; presses that are no button refused");
    api->reset_chain(me, "bye now");
    ComboPlugins::Frame(3000);
    Check(plays.size() == 1 && plays[0].first == 2 && plays[0].second == -1 && resets.size() == 1 && resets[0] == "plugin:test:bye_now", "play_node and reset_chain reach the runtime");

    // re-register = update; unregister
    {
        static const char* const outs3[] = {"甲", "乙", "丙"};
        PbzActionDesc pick{}; pick.size = sizeof(pick); pick.id = "pick"; pick.name = "选三边"; pick.kind = PBZ_ACTION_DECIDE; pick.output_count = 3; pick.outputs = outs3; pick.decide = Pick;
        ComboPlugins::ActionView v;
        Check(api->register_action(me, &pick) == PBZ_OK && ComboPlugins::FindAction("test/pick", v) && v.name == L"选三边" && v.outputs.size() == 3 && ComboPlugins::Actions().size() == 2,
              "registering an action again updates it");
        PbzSignalDesc sig{}; sig.size = sizeof(sig); sig.id = "go"; sig.name = "出发！"; int32_t again = 0;
        Check(api->register_signal(me, &sig, &again) == PBZ_OK && again == code && ComboPlugins::SignalName(code) == L"出发！", "registering a signal again updates it (same key code)");
        Check(api->unregister_action(me, "act") == PBZ_OK && !ComboPlugins::FindAction("test/act", v) && api->unregister_action(me, "act") == PBZ_E_NOT_FOUND, "unregister_action");
        api->register_action(me, &pick);
    }

    // ---- edit sessions ----
    {
        NpcCombo::Config hero_table; hero_table.chain.push_back(NpcCombo::TriggerNode(Trigger::Heavy, 0, 0)); NpcCombo::EnsureKey(hero_table);
        NpcCombo::Save(NpcCombo::kHeroProfile, hero_table);
        PbzEdit ed = 0, none = 0;
        Check(api->edit_begin(me, "Hero", &ed) == PBZ_OK && ed > 0 && api->edit_node_count(me, ed) >= static_cast<int32_t>(hero_table.chain.size()), "edit_begin reads the table");
        Check(api->edit_begin(me, "NoSuchLook", &none) == PBZ_E_NOT_FOUND, "an unknown table is refused");
        const size_t base = static_cast<size_t>(api->edit_node_count(me, ed));
        int32_t skill = -1, plug = -1, speed = -1, row = -1;
        Check(api->edit_add_node(me, ed, PBZ_NODE_SKILL, 300, 0, &skill) == PBZ_OK && api->edit_set_field(me, ed, skill, -1, "ability", "GA_Hero_Light") == PBZ_OK &&
              api->edit_set_field(me, ed, skill, -1, "ms", "350") == PBZ_OK && api->edit_link(me, ed, 0, 0, skill) == PBZ_OK, "add a move, set its fields, wire it");
        Check(api->edit_add_node(me, ed, PBZ_NODE_PLUGIN, 600, 0, &plug) == PBZ_OK && api->edit_set_field(me, ed, plug, -1, "action", "test/pick") == PBZ_OK &&
              api->edit_set_field(me, ed, plug, -1, "action_kind", "1") == PBZ_OK && api->edit_add_route(me, ed, plug, &row) == PBZ_OK && row == 0 &&
              api->edit_add_route(me, ed, plug, &row) == PBZ_OK && api->edit_link(me, ed, plug, 1, skill) == PBZ_OK, "a 判断 node with two outputs");
        Check(api->edit_add_node(me, ed, PBZ_NODE_SPEED, 300, 200, &speed) == PBZ_OK && api->edit_link_modifier(me, ed, skill, speed) == PBZ_OK &&
              Field(ed, skill, -1, "mod.speed") == std::to_string(speed) && api->edit_link(me, ed, skill, 0, 0) == PBZ_E_ARG, "modifier arrows; no wire into a start");
        Check(api->edit_set_field(me, ed, speed, -1, "points", "0:100,50:300,100:100") == PBZ_OK && Field(ed, speed, -1, "points") == "0:100,50:300,100:100" &&
              api->edit_set_field(me, ed, 0, -1, "keys", "1,2,x") == PBZ_E_ARG && api->edit_set_field(me, ed, 0, -1, "nosuch", "1") == PBZ_E_NOT_FOUND, "lists, points, bad values and unknown fields");
        Check(api->edit_set_field(me, ed, -1, -1, "enabled", "0") == PBZ_OK && Field(ed, -1, -1, "enabled") == "0", "the table's own switch");
        char names[2048]{}; int32_t needed = 0;
        Check(api->edit_field_names(0, names, sizeof(names), &needed) == PBZ_OK && std::string(names).find("mod.effect") != std::string::npos &&
              api->edit_field_names(1, nullptr, 0, &needed) == PBZ_E_BUFFER && needed > 10, "edit_field_names lists node and row fields");
        Check(api->edit_commit(me, ed) == PBZ_OK && api->edit_commit(me, ed) == PBZ_E_NOT_FOUND, "edit_commit writes once");
        const NpcCombo::Config saved = NpcCombo::Load(NpcCombo::kHeroProfile);
        Check(!saved.enabled && saved.chain.size() == base + 3 && saved.chain[static_cast<size_t>(skill)].ability == "GA_Hero_Light" &&
              saved.chain[static_cast<size_t>(skill)].ms == 350 && saved.chain[0].next == skill && saved.chain[static_cast<size_t>(plug)].routes.size() == 2 &&
              saved.chain[static_cast<size_t>(plug)].routes[1].next == skill && saved.chain[static_cast<size_t>(skill)].mods[NpcCombo::kModSpeed] == speed,
              "the committed table is what the edits made");
        PbzEdit ed2 = 0; api->edit_begin(me, "Hero", &ed2);
        Check(api->edit_remove_node(me, ed2, skill) == PBZ_OK && api->edit_cancel(me, ed2) == PBZ_OK && NpcCombo::Load(NpcCombo::kHeroProfile).chain.size() == saved.chain.size(),
              "edit_cancel leaves the table as it was");
        char exported[8192]{};
        Check(api->export_table("Hero", exported, sizeof(exported), &needed) == PBZ_OK && std::strstr(exported, "|p:") != nullptr, "export_table gives the save text");
    }

    // ---- options and settings ----
    double v = -1;
    Check(api->set_option(me, "chain_hud", 2) == PBZ_OK && NpcCombo::ChainHud() == 2 && api->get_option("chain_hud", &v) == PBZ_OK && v == 2 &&
          api->set_option(me, "chain_hud", 9) == PBZ_E_ARG && api->set_option(me, "plugins", 0) == PBZ_E_ARG && api->get_option("nosuch", &v) == PBZ_E_NOT_FOUND, "options");
    api->set_option(me, "chain_hud", 0);
    char value[64]{}; int32_t needed = 0;
    Check(api->set_setting(me, "Plugin.test", "TestKey", "42") == PBZ_OK && api->get_setting("Plugin.test", "TestKey", value, sizeof(value), &needed) == PBZ_OK && std::string(value) == "42" &&
          api->set_setting(me, "NpcCombo", "TestKey", "1") == PBZ_OK && api->set_setting(me, "Plugins", "Enabled", "0") == PBZ_E_ARG && api->set_setting(me, "Plugins", "AllowUe", "1") == PBZ_E_ARG &&
          api->set_setting(me, "PluginGrants", "test", "read,write") == PBZ_E_ARG && api->get_setting("Plugin.test", "Missing", value, sizeof(value), &needed) == PBZ_E_NOT_FOUND,
          "settings: a plugin's own section and [NpcCombo] are open; the player's switches are not");
    Check(api->set_setting(me, "TestSection", "TestKey", "42") == PBZ_E_DENIED, "settings: any other section needs PBZ_CAP_INI (this plugin did not ask for it)");

    RevisionTwo();

    // ---- a faulting plugin ----
    Check(ComboPlugins::AddForTest(BadMain, L"bad.dll") == PBZ_OK, "the bad plugin loads");
    { Node bad; bad.type = NodeType::Plugin; bad.action = "bad/crash"; bad.action_kind = NpcCombo::kActionDecide; bad.routes.resize(2); chain.chain.push_back(bad); }
    ComboPlugins::Frame(4000);
    const int got = ComboPlugins::Decide(chain, static_cast<int>(chain.chain.size()) - 1, light, 1);
    bool faulted = false; for (const auto& p : ComboPlugins::Plugins()) if (p.id == "bad") faulted = p.faulted;
    Check(got == -1 && faulted && !notices.empty(), "a plugin that faults is switched off (and the player told); nothing else stops");

    // ---- off ----
    ComboPlugins::SetEnabled(false); ComboPlugins::Frame(5000);
    Check(enabled_calls == 1 && api->fire_signal(me, "go") == PBZ_E_DISABLED && ComboPlugins::Decide(chain, 1, light, 1) == -1, "turned off: on_enabled(0), signals and decisions stop");

    // ---- the sample DLLs, loaded from <exe dir>\PBZPlugins as the game does ----
    const std::wstring dir = ExeDir(), plugins = dir + L"\\PBZPlugins";
    CreateDirectoryW(plugins.c_str(), nullptr);
    const bool copied = CopyFileW((dir + L"\\pbz_conditions.dll").c_str(), (plugins + L"\\pbz_conditions.dll").c_str(), FALSE) &&
                        CopyFileW((dir + L"\\pbz_weapon_actions.dll").c_str(), (plugins + L"\\pbz_weapon_actions.dll").c_str(), FALSE) &&
                        CopyFileW((dir + L"\\pbz_mod_panel.dll").c_str(), (plugins + L"\\pbz_mod_panel.dll").c_str(), FALSE);
    Check(copied, "the sample plugins are built next to the test");
    ComboPlugins::ResetForTest();
    ComboPlugins::SetGrant("pbz.mod_panel", PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_COMMAND | PBZ_CAP_UI);  // the player's choice, saved in the ini
    ComboPlugins::SetEnabled(true);
    ComboPlugins::Frame(6000);
    size_t loaded = 0; for (const auto& p : ComboPlugins::Plugins()) if (p.loaded) ++loaded;
    Check(ComboPlugins::Loaded() && loaded == 3 && ComboPlugins::Actions().size() == 7 && ComboPlugins::Signals().size() == 3, "the three samples load: 7 actions, 3 signals");
    {   // 示例 3: it asked for permissions, drew its page from what this MOD has, and its bound label shows the host's value
        uint32_t asked = 0, allowed = 0;
        for (const auto& p : ComboPlugins::Plugins()) if (p.id == "pbz.mod_panel") { asked = p.requested; allowed = p.granted; }
        Check(asked == (PBZ_CAP_COMBO | PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_COMMAND | PBZ_CAP_UI) && allowed == asked, "pbz_mod_panel: asks for read / write / command / ui; the saved grant is applied at load");
        const std::vector<PluginUi::Page> pages = ComboPlugins::UiPages();
        Check(pages.size() == 1 && pages[0].owner == "pbz.mod_panel" && pages[0].id == "main" && pages[0].items.size() == 4 && pages[0].items[0].id == "version" && pages[0].items[0].bind == "host.mod_version" &&
              pages[0].items[1].id == "dump" && pages[0].items[2].id == "hub" && pages[0].items[3].kind == PBZ_UI_TEXT, "pbz_mod_panel: its page holds only what this MOD has (here: the host module, no combo / npc_look)");
        PluginUi::Item version;
        Check(PluginUi::FindItem("pbz.mod_panel", "main", "version", version) && version.text == L"20261113", "its bound label shows the host's mod_version");
        const int dumped = CountLogs("PLUGIN [pbz.mod_panel] module host:");
        ComboPlugins::UiInput("pbz.mod_panel", "main", "dump", 1); ComboPlugins::Frame(6016);
        Check(CountLogs("PLUGIN [pbz.mod_panel] module host:") == dumped + 1, "its button runs the plugin's own code (on_ui): it lists the modules in the log");
    }
    chain = {};
    { Node p; p.type = NodeType::Plugin; p.action = "pbz.conditions/hero_hp"; p.action_kind = NpcCombo::kActionDecide; p.value = 50; p.routes.resize(2); chain.chain.push_back(p); }
    hero = PbzHeroFacts{}; hero.valid = 1; hero.health = 30; hero.health_max = 100;
    const int low = ComboPlugins::Decide(chain, 0, light, 1);
    hero.health = 80; ComboPlugins::Frame(6016);
    const int high = ComboPlugins::Decide(chain, 0, light, 1);
    Check(low == 0 && high == 1, "主角血量: below 50 % -> 低于, above -> 不低于");
    watched_code = NpcCombo::SignalKeyCode("pbz.weapon_actions/air_light");
    ab.name = "GA_Hun_Young_Air_A"; ab.handle = 9; ab.chain_own = 0;
    const int consumed = ComboPlugins::HeroAbility(ab);
    ComboPlugins::Frame(6032);
    Check(consumed == PBZ_CONSUME && ComboPlugins::SignalDown(watched_code), "空中轻攻击: the action is taken from the game and its signal pressed while a chain waits for it");
    watched_code = 0; ab.handle = 10;
    Check(ComboPlugins::HeroAbility(ab) == PBZ_PASS, "nobody waits: the game keeps its action");
    // ---- PbzAttachPlugin: a DLL another loader put in the game (the UE4SS bridge) ----
    ComboPlugins::SetEnabled(false); ComboPlugins::Frame(7000);
    { PbzResult r = PBZ_E_STATE; std::thread([&] { r = ComboPlugins::Attach(TestMain, L"bridge"); }).join();
      Check(r == PBZ_OK && ComboPlugins::Attach(nullptr, L"x") == PBZ_E_ARG, "Attach from any thread; a null entry is refused"); }
    ComboPlugins::Frame(7016);
    auto attached = [] { for (const auto& p : ComboPlugins::Plugins()) if (p.file == L"[外部] bridge") return p.loaded ? 2 : 1; return 0; };
    Check(attached() == 0, "attached while 派生插件 is off: waits");
    ComboPlugins::SetEnabled(true); ComboPlugins::Frame(7032);
    Check(attached() == 2 && ComboPlugins::Actions().size() == 9 && ComboPlugins::Signals().size() == 4, "turned on: the attached plugin starts like the others");
    for (const std::string& l : logs) if (l.find("PLUGIN") != std::string::npos && l.find("loaded") != std::string::npos) std::printf("  log: %s\n", l.c_str());
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
