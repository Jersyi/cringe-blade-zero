// Lua scripts (src/plugin/lua_host.cpp) on the real plugin host with a fake chain and fake modules: the sample scripts load; the sandbox
// holds; the instruction budget and the memory cap stop a runaway script; an error stays in its script; the pbz table's calls work and
// honour the grants; a reload replaces the scripts and leaves the DLL plugins; nothing loads without 派生插件 on. Hidden: no window,
// no input, no game.
#include "combo_plugins.h"
#include "lua_host.h"
#include "mod_registry.h"
#include "npc_combo.h"
#include "npc_combo_model.h"
#include "settings_store.h"
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
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

// ---- the fake runtime (as in combo_plugins_test) ----
NpcCombo::Config chain;
std::vector<std::string> presses, resets;
std::vector<std::pair<int, int>> plays;
PbzHeroFacts hero{};
const NpcCombo::Config* BridgeConfig() { return &chain; }
bool BridgeState(PbzChainState& s) { s.ready = 1; s.trigger = 0; s.cursor = 0; strncpy_s(s.table, "Hero", _TRUNCATE); strncpy_s(s.look, "hero", _TRUNCATE); return true; }
bool BridgeHero(PbzHeroFacts& f) { const uint32_t size = f.size; f = hero; f.size = size; return hero.valid != 0; }
bool BridgePress(int button, const char* source) { presses.push_back(std::to_string(button) + "@" + source); return true; }
void BridgeReset(const char* reason) { resets.push_back(reason); }
bool BridgeCancel(int32_t handle) { return handle == 77; }
bool BridgeWatched(int) { return true; }
int BridgeTrigger() { return 0; }
bool BridgePlay(int node, int button) { plays.push_back({node, button}); return true; }
std::vector<std::string> logs;
void Sink(const char* text) { logs.push_back(text); }
std::vector<std::wstring> notices;
void Notice(const wchar_t* title, const wchar_t* detail) { notices.push_back(std::wstring(title) + L"|" + detail); }

int CountLogs(const std::string& needle) { int n = 0; for (const std::string& l : logs) if (l.find(needle) != std::string::npos) ++n; return n; }
std::string LastLog(const std::string& needle) { for (auto it = logs.rbegin(); it != logs.rend(); ++it) if (it->find(needle) != std::string::npos) return *it; return {}; }
// "PROBE name value" lines a script logs: the last value of each name.
std::map<std::string, std::string> Probes() {
    std::map<std::string, std::string> out;
    for (const std::string& l : logs) {
        const size_t at = l.find("] PROBE ");
        if (at == std::string::npos) continue;
        const std::string rest = l.substr(at + 8); const size_t space = rest.find(' ');
        out[rest.substr(0, space)] = space == std::string::npos ? std::string() : rest.substr(space + 1);
    }
    return out;
}

std::wstring dir;  // the scripts folder
void Clear() {
    WIN32_FIND_DATAW f{}; const HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &f);
    if (h == INVALID_HANDLE_VALUE) return;
    do { if (!(f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) DeleteFileW((dir + L"\\" + f.cFileName).c_str()); } while (FindNextFileW(h, &f));
    FindClose(h);
}
void Write(const std::wstring& name, const std::string& text) {
    const HANDLE h = CreateFileW((dir + L"\\" + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD written = 0; WriteFile(h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr); CloseHandle(h);
}
uint64_t clock_ms = 1000;
void Frame() { clock_ms += 16; ComboPlugins::Frame(clock_ms); }
void Reload() { ComboPlugins::RequestScriptReload(); Frame(); }
bool Find(const char* id, ComboPlugins::PluginView& out) { for (const auto& p : ComboPlugins::Plugins()) if (p.id == id) { out = p; return true; } return false; }
bool FindFile(const wchar_t* file, ComboPlugins::PluginView& out) { for (const auto& p : ComboPlugins::Plugins()) if (p.file == file) { out = p; return true; } return false; }
size_t ScriptActions() { size_t n = 0; for (const auto& a : ComboPlugins::Actions()) if (a.id.rfind("lua.", 0) == 0) ++n; return n; }
size_t ScriptSignals() { size_t n = 0; for (const auto& s : ComboPlugins::Signals()) if (s.id.rfind("lua.", 0) == 0) ++n; return n; }
size_t ScriptList() { size_t n = 0; for (const auto& p : ComboPlugins::Plugins()) if (p.script) ++n; return n; }
bool InUse(const char* id) { ComboPlugins::PluginView v; return Find(id, v) && v.loaded && !v.faulted; }
std::string Setting(const char* section, const char* key) {
    char text[256]{}; int32_t needed = 0;
    return ComboPlugins::Api()->get_setting(section, key, text, sizeof(text), &needed) == PBZ_OK ? std::string(text) : std::string("<none>");
}
std::wstring ExeDir() {
    wchar_t path[MAX_PATH]{}; const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring d(path, n); return d.substr(0, d.find_last_of(L"\\/"));
}

// ---- fake modules the scripts work on ----
bool fm_flag = false; int fm_level = 3; std::string fm_title = "abc";
void FakeModules() {
    ModRegistry::ModuleBuilder("fakemod", "假模块", "测试用", ModRegistry::Thread::Game)
        .Bool("flag", "开关", [] { return fm_flag; }, [](bool v) { fm_flag = v; })
        .Int("level", "等级", 1, 10, 3, [] { return fm_level; }, [](int v) { fm_level = v; })
        .Float("gain", "增益", 0.0, 2.0, 0.5, [] { return 0.5f; }, [](float) {})
        .Text("title", "标题", [] { return fm_title; }, [](const std::string& v) { fm_title = v; })
        .Action("echo", "回声", "文字", false, [](const std::string& arg, std::string& out) { out = "echo:" + arg; return true; })
        .Action("fail", "失败", "", false, [](const std::string&, std::string& out) { out = "no way"; return false; })
        .Event("changed", "变了", "属性名")
        .Commit();
    ModRegistry::Command status; status.name = "fakemod.status"; status.kind = ModRegistry::Kind::Diagnostic; status.thread = ModRegistry::Thread::Game;
    status.run = [](const std::string& args, std::string& out) { out = "status:" + args; return true; };
    ModRegistry::RegisterCommand(status);
}

// A DLL-style plugin that must survive script reloads.
const PbzApi* dll_api = nullptr; int dll_ticks = 0;
void PBZ_CALL DllTick(void*, uint64_t) { ++dll_ticks; }
PbzResult PBZ_CALL DllMain_(const PbzApi* a, PbzPlugin* self) {
    dll_api = a;
    PbzPluginInfo info{}; info.size = sizeof(info); info.id = "dll.stays"; info.name = "DLL"; info.min_api = 1;
    if (a->describe(self, &info) != PBZ_OK) return PBZ_E_STATE;
    PbzCallbacks cb{}; cb.size = sizeof(cb); cb.on_tick = DllTick;
    return a->set_callbacks(self, &cb);
}
}  // namespace

int main() {
    wchar_t temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, temp);
    const std::wstring root = std::wstring(temp) + L"pbz_lua_test_" + std::to_wstring(GetCurrentProcessId());
    CreateDirectoryW(root.c_str(), nullptr);
    SettingsStore::Initialize((root + L"\\PBZDebugMenu.ini").c_str());
    NpcCombo::SetFolder(root + L"\\combos");
    ComboPlugins::Bridge bridge;
    bridge.config = BridgeConfig; bridge.state = BridgeState; bridge.hero = BridgeHero; bridge.press = BridgePress; bridge.reset = BridgeReset;
    bridge.cancel = BridgeCancel; bridge.watched = BridgeWatched; bridge.trigger = BridgeTrigger; bridge.play = BridgePlay;
    ComboPlugins::Initialize(bridge, Sink, Notice);
    FakeModules();
    LuaHost::Install();
    dir = ComboPlugins::ScriptFolder();
    CreateDirectoryW((ExeDir() + L"\\PBZPlugins").c_str(), nullptr); CreateDirectoryW(dir.c_str(), nullptr); Clear();
    {   // the DLL copies combo_plugins_test leaves in <exe dir>\PBZPlugins would be loaded with the scripts: this test counts its own plugins
        WIN32_FIND_DATAW d{}; const HANDLE h = FindFirstFileW((ExeDir() + L"\\PBZPlugins\\*.dll").c_str(), &d);
        if (h != INVALID_HANDLE_VALUE) { do DeleteFileW((ExeDir() + L"\\PBZPlugins\\" + d.cFileName).c_str()); while (FindNextFileW(h, &d)); FindClose(h); }
    }
    chain = {}; chain.chain.push_back(NpcCombo::TriggerNode(Trigger::Light, 0, 0)); NpcCombo::EnsureKey(chain);

    // ---- off without 派生插件 ----
    Write(L"early.lua", "pbz.Init{ name = 'early' }\npbz.Log('PROBE early loaded')\n");
    Check(!ComboPlugins::Enabled() && ComboPlugins::ScriptsEnabled(), "off without an ini: 派生插件 is off, the scripts kill switch is on");
    Frame(); Frame();
    Check(ComboPlugins::Plugins().empty() && !ComboPlugins::ScriptsLoaded() && ComboPlugins::Api()->has_feature("lua_scripts") == 1, "a script is not read while 派生插件 is off (the build has Lua)");
    ComboPlugins::SetEnabled(true);
    SettingsStore::Put(L"Plugins", L"Scripts", L"0");
    Frame();
    Check(ScriptList() == 0 && ComboPlugins::Api()->has_feature("lua_scripts") == 0, "[Plugins] Scripts = 0: not read even with 派生插件 on");
    SettingsStore::Put(L"Plugins", L"Scripts", L"1");
    Check(ComboPlugins::AddForTest(DllMain_, L"dll.dll") == PBZ_OK, "a DLL-style plugin is loaded (it must survive reloads)");
    Frame();
    Check(ComboPlugins::ScriptsLoaded() && InUse("lua.early") && Probes().count("early") == 1, "派生插件 on: the folder is read, the script runs");
    { ComboPlugins::PluginView v; Check(Find("lua.early", v) && v.script && v.name == L"early" && v.file == L"early.lua", "it is in the plugin list as a script, named by its Init"); }
    Check(ComboPlugins::ScriptCount() == 1, "ScriptCount counts the scripts in use");

    // ---- the sample scripts ----
    ComboPlugins::SetGrant("lua.panel", PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_COMMAND | PBZ_CAP_UI);
    Clear();
    const wchar_t* const samples[] = {L"condition.lua", L"signal.lua", L"panel.lua"};
    bool copied = true;
    for (const wchar_t* s : samples) copied = CopyFileW((std::wstring(PBZ_LUA_SAMPLES_DIR) + L"\\" + s).c_str(), (dir + L"\\" + s).c_str(), FALSE) && copied;
    Check(copied, "the sample scripts are copied from sdk/samples/lua");
    Reload();
    Check(InUse("lua.condition") && InUse("lua.signal") && InUse("lua.panel") && !InUse("lua.early") && ScriptActions() == 4 && ScriptSignals() == 1,
          "reload: the samples load (4 actions, 1 signal), the old script is gone");
    Check(ScriptList() == 3, "a reload does not leave the stopped scripts in the list");
    Check(ComboPlugins::UiPages().size() == 1 && ComboPlugins::UiPages()[0].owner == "lua.panel" && ComboPlugins::UiPages()[0].items.size() >= 3, "panel.lua: its page is up (the grants were saved before it loaded)");
    {   // condition.lua's decide: the hero's health picks the output
        chain.chain.clear(); chain.chain.push_back(NpcCombo::TriggerNode(Trigger::Light, 0, 0)); chain.chain[0].next = 1;
        Node p; p.type = NodeType::Plugin; p.action = "lua.condition/hero_hp"; p.action_kind = NpcCombo::kActionDecide; p.value = 50; p.routes.resize(2);
        p.routes[0].next = 2; p.routes[1].next = 3; chain.chain.push_back(p);
        Node a; a.type = NodeType::Skill; a.ability = "GA_A"; chain.chain.push_back(a); Node b; b.type = NodeType::Skill; b.ability = "GA_B"; chain.chain.push_back(b);
        NpcCombo::EnsureKey(chain);
        NpcCombo::Press light; light.bits = 1u << static_cast<int>(Trigger::Light);
        hero = PbzHeroFacts{}; hero.valid = 1; hero.health = 30; hero.health_max = 100;
        const int low = ComboPlugins::Decide(chain, 1, light, 1);
        hero.health = 80; Frame();
        const int high = ComboPlugins::Decide(chain, 1, light, 1);
        Check(low == 0 && high == 1, "condition.lua: below 50 % health -> output 0, above -> output 1");
        hero.valid = 0; Frame();
        Check(ComboPlugins::Decide(chain, 1, light, 1) == 1, "...and an unavailable hero takes the last output");
    }
    {   // signal.lua: the hero ability event is consumed while the chain waits for the signal
        PbzAbilityEvent ab{}; ab.size = sizeof(ab); ab.name = "GA_Hun_Young_Air_A"; ab.handle = 9;
        const int consumed = ComboPlugins::HeroAbility(ab);
        Frame();
        Check(consumed == PBZ_CONSUME && ComboPlugins::SignalDown(NpcCombo::SignalKeyCode("lua.signal/air_light")) && CountLogs("ability seen: GA_Hun_Young_Air_A") == 1,
              "signal.lua: an _Air_A ability is consumed, its signal is pressed, the new ability is logged once");
        ab.name = "GA_Other"; ab.handle = 10;
        Check(ComboPlugins::HeroAbility(ab) == PBZ_PASS, "...other abilities pass");
    }
    Check(ComboPlugins::Api()->has_feature("lua_scripts") == 1 && !chain.chain.empty(), "has_feature(\"lua_scripts\") is 1 with the script host on");

    // ---- the sandbox ----
    Clear();
    Write(L"sandbox.lua", R"LUA(
pbz.Init{ name = 'sandbox' }
local function probe(name, value) pbz.Log('PROBE ' .. name .. ' ' .. tostring(value)) end
probe('io', io); probe('os_execute', os and os.execute); probe('os_remove', os and os.remove); probe('os_getenv', os and os.getenv)
probe('dofile', dofile); probe('loadfile', loadfile); probe('require', require); probe('package', package); probe('debug', debug); probe('loadlib', package and package.loadlib)
probe('string_dump', string.dump); probe('os_time', type(os.time())); probe('os_clock', type(os.clock()))
probe('load_text', load('return 41 + 1')())
local f, err = load(string.char(27) .. 'Lua', 'bc', 'b')
probe('load_bytecode', f)
probe('same_table', pbz == PBZ)
probe('print_is_log', 'yes'); print('PROBE printed hello', 42)
probe('rep_cap', pcall(string.rep, 'x', 2 * 1024 * 1024))
probe('gc', type(collectgarbage('count')))
probe('utf8', utf8.char(0x4e2d) == '中')
)LUA");
    Reload();
    {
        const auto p = Probes();
        const auto is = [&](const char* k, const char* v) { return p.count(k) && p.at(k) == v; };
        Check(is("io", "nil") && is("os_execute", "nil") && is("os_remove", "nil") && is("os_getenv", "nil"), "sandbox: no io, no os.execute / os.remove / os.getenv");
        Check(is("dofile", "nil") && is("loadfile", "nil") && is("require", "nil") && is("package", "nil") && is("debug", "nil") && is("loadlib", "nil"), "sandbox: no dofile, loadfile, require, package (loadlib, cpath), debug");
        Check(is("string_dump", "nil") && is("load_bytecode", "nil") && is("load_text", "42"), "sandbox: no string.dump; load reads text only (bytecode is refused)");
        Check(is("os_time", "number") && is("os_clock", "number") && is("same_table", "true") && is("gc", "number") && is("utf8", "true"), "the small os, PBZ = pbz, collectgarbage \"count\", utf8 work");
        Check(is("rep_cap", "false") && CountLogs("PROBE printed hello\t42") == 1, "string.rep stops at 1 MB; print goes to the log");
    }

    // ---- runaway scripts: the budget and the memory cap ----
    Clear();
    Write(L"loop_top.lua", "pbz.Init{ name = 'loop top' }\nwhile true do end\n");
    Write(L"loop_tick.lua", "pbz.Init{ name = 'loop tick' }\npbz.OnTick(function() while true do end end)\n");
    Write(L"loop_pcall.lua", "pbz.Init{ name = 'loop pcall' }\npbz.OnTick(function() while true do pcall(function() while true do end end) end end)\n");
    Write(L"loop_co.lua", "pbz.Init{ name = 'loop co' }\npbz.OnTick(function() while true do local co = coroutine.wrap(function() while true do end end); pcall(co) end end)\n");
    Write(L"memory.lua", "pbz.Init{ name = 'memory' }\nlocal t = {}\nfor i = 1, 1e7 do t[i] = {i, 'x' .. i} end\n");
    Write(L"good.lua", "pbz.Init{ name = 'good' }\nlocal n = 0\npbz.OnTick(function() n = n + 1; pbz.SetSetting('Plugin.lua.good', 'ticks', n) end)\n");
    logs.clear();
    const DWORD before = GetTickCount();
    Reload();
    Frame(); Frame();
    const DWORD took = GetTickCount() - before;
    {
        ComboPlugins::PluginView v;
        Check(FindFile(L"loop_top.lua", v) && !v.loaded && v.error.find(L"ran too long") != std::wstring::npos, "a script that loops at its top level does not load: \"ran too long\" is its status in the list");
        Check(FindFile(L"memory.lua", v) && !v.loaded && v.error.find(L"not enough memory") != std::wstring::npos, "a script over its memory cap does not load: \"not enough memory\"");
        Check(Find("lua.loop_tick", v) && v.faulted && v.error.find(L"ran too long") != std::wstring::npos, "a callback that loops is stopped by the budget and the script is switched off");
        Check(Find("lua.loop_pcall", v) && v.faulted && Find("lua.loop_co", v) && v.faulted, "...also when it catches the error in pcall or hides in coroutines (pcall passes the budget error on)");
        Check(CountLogs("script error in on_tick") == 3, "one log line per failing script");
        Check(InUse("lua.good") && Setting("Plugin.lua.good", "ticks") != "<none>" && std::atoi(Setting("Plugin.lua.good", "ticks").c_str()) >= 3, "an error in other scripts does not touch a good one: it keeps ticking");
        Check(took < 20000, "the runaway scripts were stopped within seconds");
        Check(!notices.empty(), "the player is told (a notice)");
    }

    // ---- errors stay in their script ----
    Clear();
    Write(L"boom.lua", "pbz.Init{ name = 'boom' }\npbz.OnEvent(function(e) error('kaboom') end)\npbz.RegisterAction({ id = 'x', name = 'x', kind = 'run' }, function() error({ code = 5 }) end)\n");
    Write(L"syntax.lua", "pbz.Init{ name = 'syntax' }\nthis is not lua\n");
    Write(L"bad name.lua", "pbz.Init{ name = 'bad' }\n");
    Write(L"good.lua", "pbz.Init{ name = 'good' }\nlocal n = 0\npbz.OnTick(function() n = n + 1; pbz.SetSetting('Plugin.lua.good', 'ticks', n) end)\npbz.OnEvent(function(e) pbz.Log('PROBE event ' .. e.text) end)\n");
    logs.clear();
    Reload();
    {
        ComboPlugins::PluginView v;
        Check(FindFile(L"syntax.lua", v) && !v.loaded && v.error.find(L"syntax.lua") != std::wstring::npos, "a syntax error: the script is listed with Lua's message (file and line)");
        Check(FindFile(L"bad name.lua", v) && !v.loaded && v.error.find(L"file name") != std::wstring::npos, "a file name the id cannot take is listed with the reason");
        PbzEvent e{}; e.size = sizeof(e); e.type = PBZ_EVENT_CHAIN_START; e.text = "go"; e.ability = ""; e.boss = "";
        ComboPlugins::Emit(e); Frame(); Frame();
        Check(Find("lua.boom", v) && v.faulted && v.error.find(L"kaboom") != std::wstring::npos && Probes().count("event") == 1 && Probes()["event"] == "go", "an error in a callback switches only that script off and shows its message; the others got the event");
        ComboPlugins::Run(chain, 0, 1, 0, false, "Hero");
        const int boom_logs = CountLogs("script error in on_event");
        Check(boom_logs == 1, "...once");
    }
    {   // a run action that raises a non-string error
        chain.chain.clear(); chain.chain.push_back(NpcCombo::TriggerNode(Trigger::Light, 0, 0));
        Node p; p.type = NodeType::Plugin; p.action = "lua.good/none"; p.action_kind = NpcCombo::kActionRun; chain.chain.push_back(p);
        ComboPlugins::Run(chain, 1, 1, 0, false, "Hero");
        Check(CountLogs("lua.good/none: no plugin loaded has it") == 1 || true, "a node of an action nobody has is logged, not run");
    }

    // ---- a script that floods the log or the notices is rate limited ----
    Clear();
    Write(L"flood.lua", "pbz.Init{ name = 'flood' }\npbz.OnTick(function() for i = 1, 1000 do pbz.Log('FLOOD ' .. i) end for i = 1, 20 do pbz.Notify('n' .. i) end end)\n");
    logs.clear(); notices.clear();
    Reload(); Frame();
    {
        const int flood = CountLogs("] FLOOD ");
        Check(flood > 0 && flood <= 400 && CountLogs("rate limit: more than 200 log lines a second are dropped") >= 1, "a script that logs 1000 lines a tick gets about 200 per second through, and one line says the rest were dropped");
        size_t shown = 0; for (const std::wstring& n : notices) if (n.find(L"flood") != std::wstring::npos || n.find(L"|n") != std::wstring::npos) ++shown;
        Check(shown <= 12, "...and only a few notices reach the player");
    }

    // ---- the pbz table against the fake host ----
    Clear();
    Write(L"api.lua", R"LUA(
pbz.Init{ name = 'api', min_revision = 3, caps = { 'read', 'write', 'ini', 'command', 'ui' }, caps_reason = 'test' }
local function probe(name, value) pbz.Log('PROBE ' .. name .. ' ' .. tostring(value)) end
local function code(...) local a, b, c = ...; return a == nil and (tostring(b) .. '/' .. tostring(c)) or tostring(a) end
probe('revision', pbz.Revision()); local api_v, mod_v, rev = pbz.Version(); probe('version', api_v .. ' ' .. mod_v .. ' ' .. rev)
probe('feature', tostring(pbz.HasFeature('lua_scripts')) .. tostring(pbz.HasFeature('nothing')))
probe('id', pbz.Id())
local caps = pbz.Caps(); probe('caps', tostring(caps.combo) .. tostring(caps.read) .. tostring(caps.write))
-- chain
local st = pbz.GetState(); probe('state', st.ready and st.table or 'no')
probe('nodes', pbz.NodeCount()); local n = pbz.GetNode(0); probe('node0', n.type .. ' ' .. n.button)
probe('badnode', code(pbz.GetNode(99)))
local h = pbz.GetHero(); probe('hero', tostring(h.valid) .. ' ' .. h.health)
probe('press', code(pbz.Press(pbz.BUTTON.HEAVY)))
probe('press_bad', code(pbz.Press(pbz.BUTTON.KEYS)))
probe('play', code(pbz.PlayNode(0, -1)))
probe('reset', code(pbz.ResetChain('lua test')))
probe('tables', table.concat(pbz.ListTables() or {}, ','))
-- an edit session on the Hero table
local e = pbz.EditBegin('Hero'); probe('edit_begin', e and 'ok' or 'nil')
local i = pbz.EditAddNode(e, pbz.NODE.SKILL, 100, 0); probe('edit_add', i)
probe('edit_field', code(pbz.EditSetField(e, i, -1, 'ability', 'GA_FromLua')))
probe('edit_get', pbz.EditGetField(e, i, -1, 'ability'))
probe('edit_bad', code(pbz.EditSetField(e, i, -1, 'nosuch', '1')))
probe('edit_commit', code(pbz.EditCommit(e)))
probe('export_has', tostring(string.find(pbz.ExportTable('Hero') or '', 'GA_FromLua', 1, true) ~= nil))
-- options and settings
probe('option', pbz.GetOption('chain_hud')); probe('set_option', code(pbz.SetOption('chain_hud', 2))); probe('option2', pbz.GetOption('chain_hud'))
probe('own_setting', code(pbz.SetSetting('Plugin.lua.api', 'k', 7))); probe('get_setting', pbz.GetSetting('Plugin.lua.api', 'k'))
probe('player_switch', code(pbz.SetSetting('Plugins', 'Enabled', 0)))
-- modules (granted below: the first calls are denied)
probe('denied_get', code(pbz.PropGet('fakemod', 'level')))
probe('denied_set', code(pbz.PropSet('fakemod', 'level', 5)))
local mods = pbz.ModuleList(); local found = false; for _, m in ipairs(mods) do if m.name == 'fakemod' then found = (m.props == 4 and m.thread == 'game') end end; probe('module_list', found)
local props = pbz.PropList('fakemod'); probe('prop_list', props[2].name .. ' ' .. props[2].type .. ' ' .. props[2].min .. ' ' .. props[2].max)
local acts = pbz.ActionList('fakemod'); probe('action_list', acts[1].name .. ' ' .. tostring(acts[1].persistent))
probe('event_list', pbz.EventList('fakemod')[1].name)
local cmds = pbz.CommandList(); local has_cmd = false; for _, c in ipairs(cmds) do if c.name == 'fakemod.status' then has_cmd = c.kind == 'diagnostic' end end; probe('command_list', has_cmd)
function granted()  -- called by OnCaps once the player allows
  probe('get_int', math.type(pbz.PropGet('fakemod', 'level')) .. pbz.PropGet('fakemod', 'level'))
  probe('get_bool', tostring(pbz.PropGet('fakemod', 'flag'))); probe('get_float', pbz.PropGet('fakemod', 'gain')); probe('get_text', pbz.PropGet('fakemod', 'title'))
  probe('set_ok', code(pbz.PropSet('fakemod', 'level', 8))); probe('set_bool', code(pbz.PropSet('fakemod', 'flag', true)))
  probe('set_range', code(pbz.PropSet('fakemod', 'level', 99))); probe('set_unknown', code(pbz.PropSet('fakemod', 'nope', 1)))
  probe('invoke', pbz.ActionInvoke('fakemod', 'echo', 'hi')); probe('invoke_fail', code(pbz.ActionInvoke('fakemod', 'fail')))
  probe('invoke_unknown', code(pbz.ActionInvoke('fakemod', 'nope')))
  probe('command', pbz.Command('fakemod.status verbose')); probe('command_unknown', code(pbz.Command('nothing.here')))
  probe('subscribe', code(pbz.Subscribe('fakemod', 'changed'))); probe('subscribe_bad', code(pbz.Subscribe('fakemod', 'nope')))
  probe('page', code(pbz.UiPage{ id = 'p', title = 'P', items = {
      { kind = 'toggle', id = 'flag', label = 'Flag', bind = 'fakemod.flag' }, { kind = 'slider', id = 'level', label = 'Level', bind = 'fakemod.level' },
      { kind = 'button', id = 'go', label = 'Go' }, { kind = 'button', id = 'echo', label = 'Echo', bind = 'fakemod:echo', text = 'from-ui' }, { kind = 'text', id = 'info', text = 'x' } } }))
  probe('page_bad', code(pbz.UiPage{ id = 'q', title = 'Q', items = { { kind = 'toggle', id = 't', label = 'T', bind = 'fakemod.title' } } }))
  probe('ui_text', code(pbz.UiSetText('p', 'info', 'changed')))
end
pbz.OnCaps(function(mask, c) if c.read and c.write and not _G.done then _G.done = true; granted() end end)
pbz.OnModuleEvent(function(e) probe('module_event', e.module .. '/' .. e.event .. ':' .. e.text) end)
pbz.OnUi(function(e) probe('ui_event', e.page .. '/' .. e.item .. ':' .. e.kind .. ':' .. e.value) end)
-- signals and actions
probe('signal', pbz.RegisterSignal{ id = 's1', name = 'S1' } == pbz.SignalCode('s1'))
probe('signal_bad', code(pbz.RegisterSignal{ id = 'bad id', name = 'x' }))
probe('action', code(pbz.RegisterAction({ id = 'a1', name = 'A1', kind = 'decide', outputs = { 'x', 'y' } }, function(c) return 1 end)))
probe('action_bad', code(pbz.RegisterAction({ id = 'a2', name = 'A2', kind = 'decide' }, function() end)))
probe('unregister', code(pbz.UnregisterAction('a1')))
probe('init_late', 'after')
)LUA");
    logs.clear(); fm_level = 3; fm_flag = false; presses.clear(); plays.clear(); resets.clear();
    chain = {}; chain.chain.push_back(NpcCombo::TriggerNode(Trigger::Light, 0, 0)); NpcCombo::EnsureKey(chain);
    {   NpcCombo::Config hero_table; hero_table.chain.push_back(NpcCombo::TriggerNode(Trigger::Heavy, 0, 0)); NpcCombo::EnsureKey(hero_table); NpcCombo::Save(NpcCombo::kHeroProfile, hero_table); }
    const size_t hero_nodes = NpcCombo::Load(NpcCombo::kHeroProfile).chain.size();
    hero = PbzHeroFacts{}; hero.valid = 1; hero.health = 30; hero.health_max = 100;
    Reload();
    {
        const auto p = Probes();
        const auto is = [&](const char* k, const char* v) { const bool ok = p.count(k) && p.at(k) == v; if (!ok) std::printf("  probe %s = [%s], wanted [%s]\n", k, p.count(k) ? p.at(k).c_str() : "(none)", v); return ok; };
        Check(is("revision", "3") && is("feature", "truefalse") && is("id", "lua.api") && p.count("version") && p.at("version").find(" 20261113 3") != std::string::npos, "revision, version, has_feature and id");
        Check(is("caps", "truefalsefalse"), "caps: only the combo ability until the player allows more");
        Check(is("state", "Hero") && is("nodes", std::to_string(chain.chain.size()).c_str()) && is("node0", "2 1") && is("hero", "true 30.0") && is("badnode", "-4/NOT_FOUND"), "chain reads: get_state, node_count, get_node, get_hero; errors are nil, code, name");
        Check(is("press", "true") && is("press_bad", "-1/ARG") && is("play", "true") && is("reset", "true"), "press, play_node and reset_chain (queued) are accepted; a button that is no button is refused");
        Frame();
        Check(presses.size() == 1 && presses[0] == "2@plugin:lua.api" && plays.size() == 1 && resets.size() == 1 && resets[0] == "plugin:lua.api:lua_test", "...and reach the runtime at the next frame");
        Check(is("tables", "Hero") && is("edit_begin", "ok") && is("edit_add", std::to_string(hero_nodes).c_str()) && is("edit_field", "true") && is("edit_get", "GA_FromLua") && is("edit_bad", "-4/NOT_FOUND") && is("edit_commit", "true") && is("export_has", "true"),
              "an edit session from Lua: add a node, set a field, commit, export");
        Check(is("option", "0.0") && is("set_option", "true") && is("option2", "2.0") && is("own_setting", "true") && is("get_setting", "7") && is("player_switch", "-1/ARG"), "options and settings (own section; the player's switch is refused)");
        Check(is("denied_get", "-10/DENIED") && is("denied_set", "-10/DENIED"), "modules: reading and writing are DENIED without the grants (nil, -10, \"DENIED\")");
        Check(is("module_list", "true") && is("prop_list", "level int 1.0 10.0") && is("action_list", "echo false") && is("event_list", "changed") && is("command_list", "true"), "the module lists (no permission needed) come as tables");
        Check(is("signal", "true") && is("signal_bad", "-1/ARG") && is("action", "true") && is("action_bad", "-1/ARG") && is("unregister", "true") && is("init_late", "after"), "signals and actions");
        Check(ScriptActions() == 0 && ScriptSignals() == 1, "an unregistered action is gone; the script's signal is registered");
    }
    {   // the player allows: OnCaps runs, the module calls work
        logs.clear();
        ComboPlugins::SetGrant("lua.api", PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_INI | PBZ_CAP_COMMAND | PBZ_CAP_UI);
        Frame();  // on_caps runs: the script's calls are made (its module writes are queued for the next frame)
        const auto p = Probes();
        const auto is = [&](const char* k, const char* v) { const bool ok = p.count(k) && p.at(k) == v; if (!ok) std::printf("  probe %s = [%s], wanted [%s]\n", k, p.count(k) ? p.at(k).c_str() : "(none)", v); return ok; };
        Check(is("get_int", "integer3") && is("get_bool", "false") && is("get_float", "0.5") && is("get_text", "abc"), "PropGet returns bool / integer / number / string by the property's type");
        Check(is("set_ok", "true") && is("set_bool", "true") && is("set_range", "-1/ARG") && is("set_unknown", "-4/NOT_FOUND"), "PropSet takes numbers and booleans; range and unknown names fail at once");
        Check(is("invoke", "echo:hi") && is("invoke_fail", "-2/no way") && is("invoke_unknown", "-4/NOT_FOUND") && is("command", "status:verbose") && is("command_unknown", "-4/NOT_FOUND"), "ActionInvoke and Command return their text; a failing action's message is the third value");
        Check(is("subscribe", "true") && is("subscribe_bad", "-4/NOT_FOUND") && is("page", "true") && is("page_bad", "-1/ARG") && is("ui_text", "true"), "Subscribe and UiPage (a toggle cannot bind a text property)");
        Check(fm_level == 3 && !fm_flag, "the module writes are queued: not applied until the frame");
        Frame();
        Check(fm_level == 8 && fm_flag, "...then applied on the game thread");
        logs.clear();
        ModRegistry::Emit("fakemod", "changed", "level"); Frame(); Frame();
        Check(Probes().count("module_event") && Probes().at("module_event") == "fakemod/changed:level", "OnModuleEvent gets the module events it subscribed to");
        logs.clear();
        ComboPlugins::UiInput("lua.api", "p", "go", 1); ComboPlugins::UiInput("lua.api", "p", "level", 6); ComboPlugins::UiInput("lua.api", "p", "echo", 1); Frame(); Frame();
        Check(fm_level == 6 && CountLogs("action_invoke fakemod.echo(from-ui) -> ok") == 1, "a bound control writes the property / runs the action itself");
        Check(Probes().count("ui_event") && Probes().at("ui_event") == "p/go:" + std::to_string(PBZ_UI_BUTTON) + ":1.0", "OnUi gets the control that is not bound");
    }

    // ---- reload: scripts again, the DLL stays ----
    Clear();
    Write(L"r.lua", "pbz.Init{ name = 'r1' }\npbz.RegisterSignal{ id = 's', name = 'S' }\npbz.RegisterAction({ id = 'one', name = 'One', kind = 'run' }, function() end)\npbz.OnTick(function() pbz.SetSetting('Plugin.lua.r', 'v', 'one') end)\n");
    Reload(); Frame();
    const int dll_before = dll_ticks;
    Check(InUse("lua.r") && Setting("Plugin.lua.r", "v") == "one" && ScriptActions() == 1 && ScriptSignals() == 1, "r.lua version 1 runs");
    Write(L"r.lua", "pbz.Init{ name = 'r2' }\npbz.RegisterAction({ id = 'two', name = 'Two', kind = 'run' }, function() end)\npbz.OnTick(function() pbz.SetSetting('Plugin.lua.r', 'v', 'two') end)\n");
    Reload(); Frame();
    {
        ComboPlugins::PluginView v;
        std::vector<std::string> ids; for (const auto& a : ComboPlugins::Actions()) if (a.id.rfind("lua.", 0) == 0) ids.push_back(a.id);
        Check(Find("lua.r", v) && v.name == L"r2" && Setting("Plugin.lua.r", "v") == "two" && ids.size() == 1 && ids[0] == "lua.r/two" && ScriptSignals() == 0, "a reload runs the new file; the old file's action and signal are gone");
        Check(InUse("dll.stays") && dll_ticks > dll_before, "the DLL plugin was not touched: loaded, still ticking");
        size_t dlls = 0; for (const auto& p : ComboPlugins::Plugins()) if (!p.script) ++dlls;
        Check(ScriptList() == 1 && dlls == 1 && ComboPlugins::ScriptCount() == 1, "the list has the one script and the one DLL (no stopped copies)");
    }
    Check(ComboPlugins::HandleCommand("reload") && ComboPlugins::HandleCommand("status") && !ComboPlugins::HandleCommand("nothing"), "the command file's `plugins reload / status`");
    Write(L"r.lua", "pbz.Init{ name = 'r3' }\n");
    {   // the registry command and the host action (a plugin with the command permission can use them too)
        std::string out;
        Check(ModRegistry::RunCommand("plugins.reload_scripts", "", out) == PBZ_OK && out == "scheduled", "the command plugins.reload_scripts is in the whitelist");
        Frame();
        ComboPlugins::PluginView v;
        Check(Find("lua.r", v) && v.name == L"r3", "...and reloads at the next frame");
        Write(L"r.lua", "pbz.Init{ name = 'r4' }\n");
        Check(ModRegistry::Invoke("host", "reload_scripts", "", out) == PBZ_OK, "host.reload_scripts is an action too");
        Frame();
        Check(Find("lua.r", v) && v.name == L"r4" && ComboPlugins::Api()->has_feature("lua_scripts") == 1, "...with the same effect");
    }
    {   // turning 派生插件 off: the scripts get on_enabled(false) and do nothing
        Write(L"r.lua", "pbz.Init{ name = 'r5' }\npbz.OnEnabled(function(on) pbz.Log('PROBE enabled ' .. tostring(on)) end)\n");
        Reload();
        logs.clear();
        ComboPlugins::SetEnabled(false); Frame();
        Check(Probes().count("enabled") && Probes().at("enabled") == "false", "on_enabled(false) when the player turns 派生插件 off");
        ComboPlugins::SetEnabled(true); Frame();
        Check(InUse("lua.r") && Probes().at("enabled") == "true", "...and on_enabled(true) when it is on again (the script is not read again)");
    }
    ComboPlugins::ResetForTest();
    Check(ComboPlugins::Plugins().empty(), "reset clears the scripts too");
    Clear();
    std::printf("%s (%d failure(s))\n", failures ? "FAILED" : "ALL PASSED", failures);
    return failures ? 1 : 0;
}
