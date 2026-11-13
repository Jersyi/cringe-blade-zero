#pragma once
// 派生插件 (sdk/pbz_combo_api.h, docs/PLUGIN_API.md): Windows x64 DLLs in <version.dll folder>\PBZPlugins\ that add
// virtual keys (插件信号) and node actions (插件节点) to the 招式派生 page, read the chain and drive it.
//
// Off by default ([Plugins] Enabled, the page's 派生插件 switch): nothing is loaded until it is turned on. Then every
// DLL there that exports PbzPluginMain is loaded on the game thread (Frame) and stays loaded until the game exits;
// turned off again, no plugin is called (only on_enabled) and its signals are up, its 判断 nodes pass nothing and its
// 动作 nodes do nothing. A plugin whose call faults (an access violation in it) is switched off for the session.
//
// Threads: the runtime (npc_retarget) calls Frame, Emit, HeroAbility, Decide and Run on the game thread; the page
// reads the views (Actions, Signals, Plugins) on its own. Plugin callbacks only ever run on the game thread.
#include "pbz_combo_api.h"
#include "plugin_ui.h"
#include <cstdint>
#include <string>
#include <vector>

namespace NpcCombo { struct Config; struct Press; }

namespace ComboPlugins {
// What the runtime answers for the API (game thread). Any member may be null (tests): the API call then fails with
// PBZ_E_STATE.
struct Bridge {
    const NpcCombo::Config* (*config)() = nullptr;        // the chain in use (null: none)
    bool (*state)(PbzChainState& out) = nullptr;           // size already set; fill the rest
    bool (*hero)(PbzHeroFacts& out) = nullptr;
    bool (*press)(int button, const char* source) = nullptr;  // into the chain as a hero button / the NPC key; true: taken
    void (*reset)(const char* reason) = nullptr;
    bool (*cancel)(int32_t handle) = nullptr;               // the hero's ability with that handle
    bool (*watched)(int code) = nullptr;                    // a start or row of the chain waits on that key now
    int (*trigger)() = nullptr;                             // the chain's start (-1: none)
    bool (*play)(int node, int button) = nullptr;           // play_node: that move node now (-1 button: the chain's)
};
using TextSink = void (*)(const char* utf8);
using NoticeSink = void (*)(const wchar_t* title, const wchar_t* detail);  // any thread (SkillToast::Show)
void Initialize(const Bridge& bridge, TextSink log, NoticeSink notify);

bool Enabled();                 // any thread
void SetEnabled(bool on);       // any thread; saved ([Plugins] Enabled)
// Game thread, once a frame before the chain is driven: loads the plugins (first frame they are on), delivers the
// events queued since the last frame, calls on_tick, then applies the presses, resets and signal changes asked for.
void Frame(uint64_t now);

// Events (game thread): queued, delivered at the next Frame. Listening: a plugin has on_event.
bool Listening();
void Emit(const PbzEvent& event);  // its strings are copied
// The hero's abilities as they start (game thread). WantsAbilities: a plugin has on_hero_ability.
bool WantsAbilities();
int HeroAbility(const PbzAbilityEvent& ability);  // PBZ_PASS / PBZ_CONSUME (any plugin consumed it)
// 插件信号: is that key down now (game thread; false for a key no plugin registered or while off).
bool SignalDown(int code);

// 插件节点. Decide is NpcCombo::PluginDecide: the output a press passing node `index` goes on through (-1: none).
// Results are kept for the frame (the chain asks more than once per press); off the game thread the last answer
// for the node is given and the plugin is not called. Run: the 动作 node `index` is played now.
int Decide(const NpcCombo::Config& config, int index, const NpcCombo::Press& press, int button);
void Run(const NpcCombo::Config& config, int index, int button, unsigned press_bits, bool npc_key, const char* table);

// Views for the page (any thread). Serial changes whenever any of them does.
struct ActionView {
    std::string id;                  // "plugin/action"
    std::wstring name, group, description, plugin;
    int kind = PBZ_ACTION_RUN;
    std::vector<std::wstring> outputs;
    bool has_value = false;
    std::wstring value_label, value_unit;
    int value_min = 0, value_max = 0, value_step = 1, value_default = 0;
};
struct SignalView { int code = 0; std::string id; std::wstring name, group, description, plugin; };
struct PluginView {
    std::string id; std::wstring file, name, version, author, description, error;  // error: why it is not in use
    bool loaded = false, faulted = false; size_t signals = 0, actions = 0;
    uint32_t requested = PBZ_CAP_COMBO, granted = PBZ_CAP_COMBO;  // PBZ_CAP_* bits: what describe_ex asked for / what the player allows
    std::wstring caps_reason;                                      // describe_ex's one line for the player
    uint32_t revision = 0;                                          // the api_revision it needs (0: any)
    bool script = false;                                            // a Lua script (PBZPlugins\scripts\*.lua), not a DLL: `file` is its file name, `error` why it is not in use
};
uint64_t Serial();
std::vector<ActionView> Actions();
bool FindAction(const std::string& id, ActionView& out);
std::vector<SignalView> Signals();
std::wstring SignalName(int code);  // empty: no plugin registered that key
std::vector<PluginView> Plugins();
bool Loaded();                       // the folder was read (Frame after turning on)
std::wstring Folder();               // <version.dll folder>\PBZPlugins

// Permissions (any thread): the player's grant for one plugin, saved in [PluginGrants] <id>. caps = PBZ_CAP_* bits (PBZ_CAP_COMBO
// is always on, and only what the plugin asked for in describe_ex counts). The plugin's on_caps runs at the next frame.
void SetGrant(const std::string& id, uint32_t caps);
// The plugins' UI pages (any thread) and the player operating one control (the panel thread): a bound control is written to
// its module property (or its action run) with the plugin's permission, any other goes to the plugin's on_ui, both at the next frame.
std::vector<PluginUi::Page> UiPages();
void UiInput(const std::string& owner, const std::string& page, const std::string& item, double value);
// Command-bus commands that belong to the panel thread (ModRegistry::Thread::Worker) run here: call once per panel cycle (proxy.cpp).
size_t PumpWorker();
// Registers the host's own module ("host") in the module registry; Initialize does. Tests that reset the registry call it again.
void RegisterModules();

// ---- Lua scripts (src/plugin/lua_host.cpp, interface revision 3) ----------------------------------------------------------
// A script is a plugin that is not a DLL: it is in the same list (PluginView::script), uses the same grants ([PluginGrants], by its
// id "lua.<file name without .lua>"), runs on the game thread only and only while 派生插件 is on. The script host registers itself
// here (it is not part of the plugin host: tests that do not link Lua simply have none).
struct ScriptHost {
    void (*load)();     // game thread: read ScriptFolder() and StartScript each *.lua
    void (*unload)();   // game thread: after RetireScripts (or at a test reset): close every script's Lua state
};
void SetScriptHost(const ScriptHost* host);
std::wstring ScriptFolder();     // <version.dll folder>\PBZPlugins\scripts
bool ScriptsEnabled();           // [Plugins] Scripts (default on): a kill switch, only read once 派生插件 is on
// Any thread. The scripts are stopped (their signals, actions, pages and subscriptions go) and read again at the next frame; the DLL
// plugins stay. This is what the plugin list's "reload scripts" action, the command plugins.reload_scripts and `plugins reload` in the
// command file call. ScriptsLoaded: the folder was read (a reload pending does not change it until the frame).
void RequestScriptReload();
bool ScriptsLoaded();
size_t ScriptCount();            // scripts in use (loaded, not failed)
bool HandleCommand(const char* argument);  // the command file's `plugins reload` / `plugins status` (any thread)
// For the script host, game thread. StartScript makes a plugin of its own from `entry` (run at once; the script host's entry runs the
// script) and returns it (null: refused, 32 scripts). Inside the entry SetScriptStatus says why a script did not load (shown in the
// list); later ScriptFailed switches the script off for the session with that message (one log line, a notice, the list shows it).
// RetireScripts stops every script plugin (hidden from Plugins() from then on).
PbzPlugin* StartScript(PbzPluginMainFn entry, const std::wstring& file);
void LogHost(const std::string& text);  // a line in the log, "PLUGIN <text>" (any thread)
void RetireScripts();
void SetScriptStatus(PbzPlugin* self, const std::string& message);
void ScriptFailed(PbzPlugin* self, const std::string& where, const std::string& message);

// A plugin from a DLL another loader put in the game (version.dll's PbzAttachPlugin export; any thread): queued, started
// on the game thread at the first frame 派生插件 is on.
PbzResult Attach(PbzPluginMainFn entry, const wchar_t* name);

// Tests: the API table, a plugin from its entry function (no DLL; loaded at once on this thread, which becomes the
// game thread), and everything cleared.
const PbzApi* Api();
PbzResult AddForTest(PbzPluginMainFn main, const wchar_t* name);
void ResetForTest();
}
