#include "combo_plugins.h"
#include "combo_weapons.h"
#include "mod_registry.h"
#include "npc_combo.h"
#include "npc_combo_model.h"
#include "plugin_text.h"
#include "plugin_ui.h"
#include "settings_store.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <cmath>
#include <climits>
#include <cstdlib>
#include <type_traits>

// The plugin's record (sdk/pbz_combo_api.h names it; plugins only pass it back).
struct PbzPlugin {
    std::wstring file; HMODULE module = nullptr;
    std::string id; std::wstring name, version, author, description, error, caps_reason;
    uint32_t min_api = 0, min_revision = 0, requested = PBZ_CAP_COMBO;  // requested: describe_ex's caps (always with PBZ_CAP_COMBO)
    std::atomic<uint32_t> granted{PBZ_CAP_COMBO};                       // what the player allows (PBZ_CAP_*; any thread)
    bool described = false, ready = false, faulted = false;
    bool script = false, retired = false;                               // a Lua script (lua_host.cpp); retired: stopped by a reload, hidden for good
    PbzCallbacks callbacks{};
    std::vector<std::pair<std::string, std::string>> subs;              // module_subscribe (module, event; "*" for all), under lock_
};

namespace ComboPlugins {
namespace {
using NpcCombo::Node;
using NpcCombo::NodeType;

constexpr size_t kMaxPlugins = 32, kMaxSignalsEach = 256, kMaxActionsEach = 256, kMaxQueued = 1024, kMaxEvents = 512;
constexpr size_t kTextMax = 800;
constexpr const char* kModVersion = "20261113";

struct Signal {
    PbzPlugin* owner = nullptr; std::string id; std::wstring name, group, description; int code = 0;
    bool level = false, pulse_on = false; int pulses = 0;
};
struct Action {
    PbzPlugin* owner = nullptr; std::string id, short_id; std::wstring name, group, description;
    int kind = PBZ_ACTION_RUN; std::vector<std::wstring> outputs;
    bool has_value = false; std::wstring value_label, value_unit; int value_min = 0, value_max = 0, value_step = 1, value_default = 0;
    PbzDecideFn decide = nullptr; PbzRunFn run = nullptr; void* user = nullptr;
};
struct Op {
    enum class Kind { Fire, Set, Press, Reset, Notify, Play, SetProp, Invoke, Command, UiEvent, Caps } kind = Kind::Fire;
    int code = 0; bool down = false; int button = 0; std::string text; std::wstring title, wide;
    // SetProp: owner (the plugin's id), module, name (the property), value (canonical). Invoke: owner, module, name (the action),
    // value (the argument). Command: owner, value (the line). UiEvent: owner, module (the page), name (the item), code (its kind), number.
    // Caps: owner (the plugin whose grant changed).
    std::string owner, module, name, value; double number = 0;
};
struct QueuedEvent { PbzEvent event{}; std::string text, ability, boss; };
struct PrivateEvent { std::string owner, event, text; uint64_t ms = 0; };  // host/command_done, host/action_done: to the plugin that asked
struct DecideKey {
    int index = -1, button = 0, value = 0; unsigned bits = 0; bool key = false; std::string action;
    bool operator==(const DecideKey& o) const { return index == o.index && button == o.button && value == o.value && bits == o.bits && key == o.key && action == o.action; }
};

std::mutex lock_;  // the registry and the queue below; never held while a plugin is called
std::vector<std::unique_ptr<PbzPlugin>> plugins_;
std::vector<Signal> signals_;
std::vector<Action> actions_;
std::vector<Op> ops_;
std::map<int, int> last_answer_;  // the last Decide answer per node (for the page's thread)
std::atomic<uint64_t> serial_{1};
std::atomic<int> enabled_{-1};  // -1: not read yet
std::atomic<bool> loaded_{false};
std::atomic<bool> listening_{false}, wants_abilities_{false};
// Game thread only.
DWORD game_thread_ = 0;
PbzPlugin* loading_ = nullptr;  // the plugin whose PbzPluginMain runs now
std::vector<QueuedEvent> events_;
uint64_t frame_ = 0;
std::vector<std::pair<PbzPluginMainFn, std::wstring>> attached_;  // PbzAttachPlugin, under lock_: started at the next frame on
int last_on_ = -1;
std::vector<std::pair<DecideKey, int>> decided_; uint64_t decided_frame_ = 0;
Bridge bridge_;
TextSink log_ = nullptr;
NoticeSink notify_ = nullptr;
PbzApi api_{};
std::vector<PrivateEvent> private_events_;  // under lock_
std::vector<std::string> denied_logged_;    // "plugin|what" already logged as denied, under lock_
uint64_t ui_shown_ms_ = 0;                  // game thread: when the bound UI items were last read

bool GameThread() { return game_thread_ && GetCurrentThreadId() == game_thread_; }
void Log(const char* format, ...) {
    if (!log_) return;
    char line[1000]{}; va_list args; va_start(args, format); _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args); va_end(args);
    log_(line);
}
using PluginText::CopyOut;
using PluginText::CopyText;
using PluginText::ReadIn;
using PluginText::Short;
using PluginText::Utf8;
using PluginText::Wide;
using PluginText::WriteOut;
void Bump() { serial_.fetch_add(1); }
bool Usable(PbzPlugin* self) { return self && !self->faulted && (self->ready || self == loading_); }
// The DLL plugins (PBZPlugins folder and attached): the 32 limit is theirs; scripts have their own (kMaxScripts).
size_t DllCount() { size_t n = 0; for (const auto& p : plugins_) if (!p->script) ++n; return n; }
constexpr size_t kMaxScripts = 32;
const ScriptHost* script_host_ = nullptr;   // lua_host.cpp (null: no script support in this build / test)
std::atomic<bool> reload_scripts_{false};
bool scripts_loaded_ = false;               // game thread

// ---- guarded plugin calls -----------------------------------------------------------------------------------------
// A structured exception in the plugin (access violation, a C++ exception thrown through the C ABI) ends the call;
// the plugin is switched off for the session.
using Thunk = void (*)(void* context);
bool Guarded(Thunk thunk, void* context, DWORD& code) {
    __try { thunk(context); return true; }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Fault(PbzPlugin* plugin, const char* what, DWORD code) {
    if (!plugin || plugin->faulted) return;
    { std::lock_guard<std::mutex> hold(lock_); plugin->faulted = true; plugin->error = L"运行出错，本次游戏内已停用"; }
    PluginUi::RemoveOwner(plugin->id);
    Bump();
    Log("PLUGIN [%s] fault 0x%08lx in %s: switched off for this session", plugin->id.empty() ? Utf8(plugin->file).c_str() : plugin->id.c_str(), code, what);
    if (notify_) { const std::wstring text = (plugin->name.empty() ? plugin->file : plugin->name) + L" 运行出错，本次游戏内已停用"; notify_(L"派生插件", text.c_str()); }
}
template <class F> bool Call(PbzPlugin* plugin, const char* what, F&& body) {
    if (!plugin || plugin->faulted) return false;
    using Body = std::remove_reference_t<F>;
    DWORD code = 0;
    if (Guarded([](void* c) { (*static_cast<Body*>(c))(); }, const_cast<void*>(static_cast<const void*>(&body)), code)) return true;
    Fault(plugin, what, code); return false;
}

// ---- permissions --------------------------------------------------------------------------------------------------
// The player grants each plugin the capabilities it asked for in describe_ex ([PluginGrants] <id> = read,write,...);
// PBZ_CAP_COMBO is always granted. The grant is checked on every call, so taking it back works at once. This gates what the
// plugin may do through the API; it is not a sandbox (a plugin is native code in the game's process).
struct CapName { const char* name; uint32_t cap; };
constexpr CapName kCapNames[] = {{"read", PBZ_CAP_READ}, {"write", PBZ_CAP_WRITE}, {"ini", PBZ_CAP_INI}, {"command", PBZ_CAP_COMMAND}, {"ui", PBZ_CAP_UI}, {"ue", PBZ_CAP_UE}};
std::string CapsText(uint32_t caps) {
    std::string out;
    for (const CapName& c : kCapNames) if (caps & c.cap) out += (out.empty() ? "" : ",") + std::string(c.name);
    return out;
}
uint32_t ParseCaps(const std::string& text) {
    uint32_t caps = 0; size_t at = 0;
    while (at <= text.size()) {
        const size_t comma = text.find(',', at);
        std::string part = text.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        while (!part.empty() && part.front() == ' ') part.erase(part.begin());
        while (!part.empty() && part.back() == ' ') part.pop_back();
        for (const CapName& c : kCapNames) if (part == c.name) caps |= c.cap;
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return caps;
}
uint32_t StoredGrant(const std::string& id) { return ParseCaps(Utf8(SettingsStore::Get(L"PluginGrants", Wide(id.c_str()).c_str(), L""))); }
uint32_t GrantFor(const std::string& id, uint32_t requested) { return PBZ_CAP_COMBO | (StoredGrant(id) & requested & PBZ_CAP_ALL); }
bool Has(const PbzPlugin* self, uint32_t cap) { return self && (self->granted.load() & cap) == cap; }
// PBZ_OK, or PBZ_E_DENIED (logged once per plugin and call).
PbzResult Need(PbzPlugin* self, uint32_t cap, const char* what) {
    if (Has(self, cap)) return PBZ_OK;
    if (!self) return PBZ_E_ARG;
    const std::string key = self->id + "|" + what;
    bool first = false;
    { std::lock_guard<std::mutex> hold(lock_); if (std::find(denied_logged_.begin(), denied_logged_.end(), key) == denied_logged_.end()) { denied_logged_.push_back(key); first = true; } }
    if (first) Log("PLUGIN [%s] denied: %s needs %s (the player has not allowed it)", self->id.c_str(), what, CapsText(cap).c_str());
    return PBZ_E_DENIED;
}
std::string Trim(std::string text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.erase(text.begin());
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.pop_back();
    return text;
}
// A loaded, working plugin by id (null: none). Under lock_.
PbzPlugin* ReadyById(const std::string& id) {
    for (const auto& p : plugins_) if (p->ready && !p->faulted && p->id == id) return p.get();
    return nullptr;
}
void PushPrivate(const std::string& owner, const char* event, const std::string& text) {
    PrivateEvent e; e.owner = owner; e.event = event; e.text = text.substr(0, 3000); e.ms = GetTickCount64();
    std::lock_guard<std::mutex> hold(lock_);
    if (private_events_.size() < kMaxEvents) private_events_.push_back(std::move(e));
}
bool Queue(Op&& op) {
    std::lock_guard<std::mutex> hold(lock_);
    if (ops_.size() >= kMaxQueued) return false;
    ops_.push_back(std::move(op)); return true;
}

// ---- the API ------------------------------------------------------------------------------------------------------
PbzResult DescribeCommon(PbzPlugin* self, const PbzPluginInfoEx& info) {
    if (!GameThread()) return PBZ_E_THREAD;
    if (!self || self != loading_) return PBZ_E_ARG;
    const std::string id = Short(info.id, 64);
    if (!NpcCombo::PluginIdPart(id)) { self->error = L"插件 id 不合法（字母、数字、_ . -，1~48 字节）"; return PBZ_E_ARG; }
    if (info.min_api > PBZ_COMBO_API_VERSION) { self->error = L"插件需要更新版本的豪之刃"; return PBZ_E_VERSION; }
    if (info.min_revision > PBZ_API_REVISION) { self->error = L"插件需要更新版本的豪之刃（接口修订 " + std::to_wstring(info.min_revision) + L"）"; return PBZ_E_VERSION; }
    std::lock_guard<std::mutex> hold(lock_);
    for (const auto& p : plugins_) if (p.get() != self && p->ready && p->id == id) { self->error = L"与已载入的插件 id 重复"; return PBZ_E_EXISTS; }
    self->id = id; self->name = Wide(info.name, 96); self->version = Wide(info.version, 32); self->author = Wide(info.author, 64);
    self->description = Wide(info.description, 400); self->min_api = info.min_api; self->min_revision = info.min_revision; self->described = true;
    self->requested = (info.caps & PBZ_CAP_ALL) | PBZ_CAP_COMBO; self->caps_reason = Wide(info.caps_reason, 200);
    self->granted = GrantFor(id, self->requested);
    if (self->name.empty()) self->name = Wide(id.c_str());
    Bump(); return PBZ_OK;
}
PbzResult PBZ_CALL Describe(PbzPlugin* self, const PbzPluginInfo* in) {
    PbzPluginInfo info{};
    if (!ReadIn(in, info)) return GameThread() ? PBZ_E_ARG : PBZ_E_THREAD;
    PbzPluginInfoEx ex{}; ex.size = sizeof(ex);
    ex.id = info.id; ex.name = info.name; ex.version = info.version; ex.author = info.author; ex.description = info.description; ex.min_api = info.min_api;
    ex.caps = PBZ_CAP_COMBO;
    return DescribeCommon(self, ex);
}
PbzResult PBZ_CALL DescribeEx(PbzPlugin* self, const PbzPluginInfoEx* in) {
    PbzPluginInfoEx info{};
    if (!ReadIn(in, info)) return GameThread() ? PBZ_E_ARG : PBZ_E_THREAD;
    return DescribeCommon(self, info);
}
uint32_t PBZ_CALL GrantedCaps(PbzPlugin* self) { return self && self->described ? self->granted.load() : 0; }
int32_t PBZ_CALL HasFeature(const char* name) {
    if (!name) return 0;
    const std::string n = Short(name, 32);
    for (const char* known : {"hero_size_node", "permissions", "modules", "module_events", "commands", "ui_pages"}) if (n == known) return 1;
    if (n == "lua_scripts") return script_host_ && ScriptsEnabled() ? 1 : 0;  // revision 3: this build reads Lua scripts (and the player allows it)
    return 0;
}
PbzResult PBZ_CALL SetCallbacks(PbzPlugin* self, const PbzCallbacks* in) {
    if (!GameThread()) return PBZ_E_THREAD;
    PbzCallbacks callbacks{};
    if (!Usable(self) || !self->described || !ReadIn(in, callbacks)) return PBZ_E_ARG;
    std::lock_guard<std::mutex> hold(lock_);
    self->callbacks = callbacks;
    bool listening = false, wants = false;
    for (const auto& p : plugins_) if ((p->ready || p.get() == self) && !p->faulted) { listening = listening || p->callbacks.on_event; wants = wants || p->callbacks.on_hero_ability; }
    listening_ = listening; wants_abilities_ = wants;
    return PBZ_OK;
}
void PBZ_CALL LogText(PbzPlugin* self, const char* text) {
    if (!text) return;
    const std::string id = self && !self->id.empty() ? self->id : std::string("?");
    Log("PLUGIN [%s] %s", id.c_str(), Short(text, kTextMax).c_str());
}
void PBZ_CALL NotifyText(PbzPlugin* self, const char* text) {
    if (!text || !*text) return;
    Op op; op.kind = Op::Kind::Notify; op.title = self && !self->name.empty() ? self->name : std::wstring(L"派生插件"); op.wide = Wide(text, 200);
    std::lock_guard<std::mutex> hold(lock_);
    if (ops_.size() < kMaxQueued) ops_.push_back(std::move(op));
}
uint64_t PBZ_CALL NowMs() { return GetTickCount64(); }
int32_t PBZ_CALL EnabledNow() { return Enabled() ? 1 : 0; }

Signal* FindSignal(const std::string& full) {
    for (Signal& s : signals_) if (s.id == full) return &s;
    return nullptr;
}
PbzResult PBZ_CALL RegisterSignal(PbzPlugin* self, const PbzSignalDesc* in, int32_t* key_code) {
    if (!GameThread()) return PBZ_E_THREAD;
    PbzSignalDesc desc{};
    if (!Usable(self) || !self->described || !ReadIn(in, desc)) return PBZ_E_ARG;
    const std::string id = Short(desc.id, 64);
    if (!NpcCombo::PluginIdPart(id) || !desc.name || !*desc.name) return PBZ_E_ARG;
    const std::string full = self->id + "/" + id; const int code = NpcCombo::SignalKeyCode(full);
    std::lock_guard<std::mutex> hold(lock_);
    for (Signal& s : signals_)
        if (s.id == full && s.owner == self) {  // registered again: an update (the key code stays)
            s.name = Wide(desc.name, 96); s.group = Wide(desc.group, 64); s.description = Wide(desc.description, 300);
            if (key_code) *key_code = s.code;
            Bump(); return PBZ_OK;
        }
    size_t own = 0;
    for (const Signal& s : signals_) {
        if (s.owner == self) ++own;
        if (s.id == full || s.code == code) {
            Log("PLUGIN [%s] signal %s refused: %s", self->id.c_str(), full.c_str(), s.id == full ? "registered already" : ("its key code is " + s.id + "'s").c_str());
            return PBZ_E_EXISTS;
        }
    }
    if (own >= kMaxSignalsEach) return PBZ_E_FULL;
    Signal s; s.owner = self; s.id = full; s.code = code; s.name = Wide(desc.name, 96); s.group = Wide(desc.group, 64); s.description = Wide(desc.description, 300);
    signals_.push_back(std::move(s));
    if (key_code) *key_code = code;
    Bump(); return PBZ_OK;
}
PbzResult SignalOp(PbzPlugin* self, const char* id, bool fire, bool down) {
    if (!Usable(self) || !id) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    std::lock_guard<std::mutex> hold(lock_);
    const Signal* s = FindSignal(self->id + "/" + Short(id, 64));
    if (!s) return PBZ_E_NOT_FOUND;
    if (ops_.size() >= kMaxQueued) return PBZ_E_FULL;
    Op op; op.kind = fire ? Op::Kind::Fire : Op::Kind::Set; op.code = s->code; op.down = down; ops_.push_back(std::move(op));
    return PBZ_OK;
}
PbzResult PBZ_CALL FireSignal(PbzPlugin* self, const char* id) { return SignalOp(self, id, true, true); }
PbzResult PBZ_CALL SetSignal(PbzPlugin* self, const char* id, int32_t down) { return SignalOp(self, id, false, down != 0); }
int32_t PBZ_CALL SignalWanted(PbzPlugin* self, const char* id) {
    if (!GameThread() || !Usable(self) || !id || !Enabled() || !bridge_.watched) return 0;
    int code = 0;
    { std::lock_guard<std::mutex> hold(lock_); const Signal* s = FindSignal(self->id + "/" + Short(id, 64)); if (!s) return 0; code = s->code; }
    return bridge_.watched(code) ? 1 : 0;
}

PbzResult PBZ_CALL RegisterAction(PbzPlugin* self, const PbzActionDesc* in) {
    if (!GameThread()) return PBZ_E_THREAD;
    PbzActionDesc desc{};
    if (!Usable(self) || !self->described || !ReadIn(in, desc)) return PBZ_E_ARG;
    const std::string id = Short(desc.id, 64), full = self->id + "/" + id;
    if (!NpcCombo::PluginIdPart(id) || !NpcCombo::ValidActionId(full) || !desc.name || !*desc.name) return PBZ_E_ARG;
    Action a; a.owner = self; a.id = full; a.short_id = id; a.name = Wide(desc.name, 96); a.group = Wide(desc.group, 64); a.description = Wide(desc.description, 300);
    if (desc.kind == PBZ_ACTION_DECIDE) {
        if (!desc.decide || desc.output_count < 1 || desc.output_count > static_cast<int32_t>(NpcCombo::kMaxRoutes) || !desc.outputs) return PBZ_E_ARG;
        for (int32_t k = 0; k < desc.output_count; ++k) {
            if (!desc.outputs[k]) return PBZ_E_ARG;
            a.outputs.push_back(Wide(desc.outputs[k], 48));
            if (a.outputs.back().empty()) a.outputs.back() = L"出口 " + std::to_wstring(k + 1);
        }
        a.kind = PBZ_ACTION_DECIDE; a.decide = desc.decide;
    } else if (desc.kind == PBZ_ACTION_RUN) {
        if (!desc.run) return PBZ_E_ARG;
        a.kind = PBZ_ACTION_RUN; a.run = desc.run;
    } else return PBZ_E_ARG;
    if (desc.has_value) {
        const int lo = NpcCombo::ClampPluginValue(desc.value_min), hi = NpcCombo::ClampPluginValue(desc.value_max);
        if (lo > hi) return PBZ_E_ARG;
        a.has_value = true; a.value_min = lo; a.value_max = hi; a.value_step = (std::max)(1, desc.value_step);
        a.value_default = std::clamp(desc.value_default, lo, hi);
        a.value_label = Wide(desc.value_label, 48); a.value_unit = Wide(desc.value_unit, 16);
        if (a.value_label.empty()) a.value_label = L"数值";
    }
    a.user = desc.user;
    std::lock_guard<std::mutex> hold(lock_);
    size_t own = 0;
    for (Action& x : actions_)
        if (x.id == full) {
            if (x.owner != self) return PBZ_E_EXISTS;
            x = std::move(a); Bump(); return PBZ_OK;  // registered again: an update
        }
    for (const Action& x : actions_) if (x.owner == self) ++own;
    if (own >= kMaxActionsEach) return PBZ_E_FULL;
    actions_.push_back(std::move(a));
    Bump(); return PBZ_OK;
}
PbzResult PBZ_CALL UnregisterSignal(PbzPlugin* self, const char* id) {
    if (!GameThread()) return PBZ_E_THREAD;
    if (!Usable(self) || !id) return PBZ_E_ARG;
    const std::string full = self->id + "/" + Short(id, 64);
    std::lock_guard<std::mutex> hold(lock_);
    const auto it = std::find_if(signals_.begin(), signals_.end(), [&](const Signal& s) { return s.owner == self && s.id == full; });
    if (it == signals_.end()) return PBZ_E_NOT_FOUND;
    signals_.erase(it); Bump(); return PBZ_OK;
}
PbzResult PBZ_CALL UnregisterAction(PbzPlugin* self, const char* id) {
    if (!GameThread()) return PBZ_E_THREAD;
    if (!Usable(self) || !id) return PBZ_E_ARG;
    const std::string full = self->id + "/" + Short(id, 64);
    std::lock_guard<std::mutex> hold(lock_);
    const auto it = std::find_if(actions_.begin(), actions_.end(), [&](const Action& a) { return a.owner == self && a.id == full; });
    if (it == actions_.end()) return PBZ_E_NOT_FOUND;
    actions_.erase(it); last_answer_.clear(); Bump(); return PBZ_OK;
}

const NpcCombo::Config* ConfigNow() { return bridge_.config ? bridge_.config() : nullptr; }
PbzResult PBZ_CALL GetState(PbzChainState* out) {
    if (!GameThread()) return PBZ_E_THREAD;
    if (!out || out->size < sizeof(uint32_t)) return PBZ_E_ARG;
    if (!bridge_.state) return PBZ_E_STATE;
    PbzChainState state{}; state.size = sizeof(state); state.trigger = state.cursor = state.playing = state.queued = -1;
    if (!bridge_.state(state)) return PBZ_E_STATE;
    state.now_ms = GetTickCount64();
    return WriteOut(out, state) ? PBZ_OK : PBZ_E_ARG;
}
int32_t PBZ_CALL NodeCount() {
    if (!GameThread()) return 0;
    const NpcCombo::Config* config = ConfigNow();
    return config ? static_cast<int32_t>(config->chain.size()) : 0;
}
PbzResult FillNode(const NpcCombo::Config* config, int32_t index, PbzNodeInfo* out) {
    if (!out || out->size < sizeof(uint32_t)) return PBZ_E_ARG;
    if (!config) return PBZ_E_STATE;
    if (index < 0 || static_cast<size_t>(index) >= config->chain.size()) return PBZ_E_NOT_FOUND;
    const Node& n = config->chain[static_cast<size_t>(index)];
    PbzNodeInfo info{}; info.size = sizeof(info);
    info.index = index; info.type = static_cast<int32_t>(n.type); info.enabled = NpcCombo::StartsChain(n) ? (n.on ? 1 : 0) : 1;
    info.x = n.x; info.y = n.y; info.next = NpcCombo::RowOutputs(n) ? -1 : n.next; info.group = n.group; info.ms = n.ms;
    info.button = n.type == NodeType::Trigger ? n.trigger : 0;
    info.route_count = static_cast<int32_t>(n.routes.size());
    info.key_count = static_cast<int32_t>((std::min)(n.keys.size(), std::size(info.keys)));
    for (int32_t k = 0; k < info.key_count; ++k) info.keys[k] = n.keys[static_cast<size_t>(k)];
    info.value = n.type == NodeType::Plugin ? n.value : 0; info.action_kind = n.type == NodeType::Plugin ? n.action_kind : 0;
    CopyText(info.ability, n.ability); CopyText(info.boss, n.boss); CopyText(info.weapon, n.weapon);
    if (n.type == NodeType::Plugin) CopyText(info.action, n.action);
    return WriteOut(out, info) ? PBZ_OK : PBZ_E_ARG;
}
PbzResult PBZ_CALL GetNode(int32_t index, PbzNodeInfo* out) {
    if (!GameThread()) return PBZ_E_THREAD;
    return FillNode(ConfigNow(), index, out);
}
PbzResult FillRoute(const NpcCombo::Config* config, int32_t node, int32_t row, PbzRouteInfo* out) {
    if (!out || out->size < sizeof(uint32_t)) return PBZ_E_ARG;
    if (!config) return PBZ_E_STATE;
    if (node < 0 || static_cast<size_t>(node) >= config->chain.size()) return PBZ_E_NOT_FOUND;
    const Node& n = config->chain[static_cast<size_t>(node)];
    if (row < 0 || static_cast<size_t>(row) >= n.routes.size()) return PBZ_E_NOT_FOUND;
    const NpcCombo::Route& r = n.routes[static_cast<size_t>(row)];
    PbzRouteInfo info{}; info.size = sizeof(info);
    info.button = n.type == NodeType::Plugin ? -1 : r.button; info.next = r.next; info.pause_ms = r.pause_ms; info.hold_ms = r.hold_ms; info.chord = r.chord ? 1 : 0;
    info.key_count = static_cast<int32_t>((std::min)(r.keys.size(), std::size(info.keys)));
    for (int32_t k = 0; k < info.key_count; ++k) info.keys[k] = r.keys[static_cast<size_t>(k)];
    return WriteOut(out, info) ? PBZ_OK : PBZ_E_ARG;
}
PbzResult PBZ_CALL GetRoute(int32_t node, int32_t row, PbzRouteInfo* out) {
    if (!GameThread()) return PBZ_E_THREAD;
    return FillRoute(ConfigNow(), node, row, out);
}
PbzResult PBZ_CALL GetHero(PbzHeroFacts* out) {
    if (!GameThread()) return PBZ_E_THREAD;
    if (!out || out->size < sizeof(uint32_t)) return PBZ_E_ARG;
    if (!bridge_.hero) return PBZ_E_STATE;
    PbzHeroFacts facts{}; facts.size = sizeof(facts);
    if (!bridge_.hero(facts)) { facts = PbzHeroFacts{}; facts.size = sizeof(facts); }
    return WriteOut(out, facts) ? PBZ_OK : PBZ_E_ARG;
}

bool PressableButton(int32_t button) {
    return button >= PBZ_BUTTON_NPC_KEY && button < PBZ_BUTTON_COUNT && button != PBZ_BUTTON_ANY_DODGE && button != PBZ_BUTTON_COMBO_KEYS && button != PBZ_BUTTON_KEYS;
}
PbzResult PBZ_CALL Press(PbzPlugin* self, int32_t button) {
    if (!Usable(self)) return PBZ_E_ARG;
    if (!PressableButton(button)) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    std::lock_guard<std::mutex> hold(lock_);
    if (ops_.size() >= kMaxQueued) return PBZ_E_FULL;
    Op op; op.kind = Op::Kind::Press; op.button = button; op.text = "plugin:" + self->id; ops_.push_back(std::move(op));
    return PBZ_OK;
}
PbzResult PBZ_CALL ResetChain(PbzPlugin* self, const char* reason) {
    if (!Usable(self)) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    std::string why = "plugin:" + self->id;
    if (reason && *reason) {
        why += ":";
        for (const char c : Short(reason, 48)) why += (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.') ? c : '_';
    }
    std::lock_guard<std::mutex> hold(lock_);
    if (ops_.size() >= kMaxQueued) return PBZ_E_FULL;
    Op op; op.kind = Op::Kind::Reset; op.text = why; ops_.push_back(std::move(op));
    return PBZ_OK;
}
PbzResult PBZ_CALL PlayNodeNow(PbzPlugin* self, int32_t node, int32_t button) {
    if (!Usable(self) || node < 0 || (button != -1 && !PressableButton(button))) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    std::lock_guard<std::mutex> hold(lock_);
    if (ops_.size() >= kMaxQueued) return PBZ_E_FULL;
    Op op; op.kind = Op::Kind::Play; op.code = node; op.button = button; op.text = self->id; ops_.push_back(std::move(op));
    return PBZ_OK;
}
PbzResult PBZ_CALL CancelHeroAbility(int32_t handle) {
    if (!GameThread()) return PBZ_E_THREAD;
    if (handle <= 0) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    if (!bridge_.cancel) return PBZ_E_STATE;
    return bridge_.cancel(handle) ? PBZ_OK : PBZ_E_NOT_FOUND;
}

PbzResult PBZ_CALL ListTables(char* buffer, int32_t size, int32_t* needed) {
    std::string text;
    for (const auto& [name, config] : NpcCombo::All()) { (void)config; text += (text.empty() ? "" : "\n") + name; }
    return CopyOut(text, buffer, size, needed);
}
PbzResult PBZ_CALL ExportTable(const char* table, char* buffer, int32_t size, int32_t* needed) {
    if (!table || !*table) return PBZ_E_ARG;
    const std::string want = Short(table, 64);
    for (const auto& [name, config] : NpcCombo::All())
        if (name == want) return CopyOut(Utf8(NpcCombo::Encode(config)), buffer, size, needed);
    return PBZ_E_NOT_FOUND;
}
PbzResult PBZ_CALL ImportTable(PbzPlugin* self, const char* table, const char* text) {
    if (!Usable(self) || !table || !*table || !text || !*text) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    const std::string want = Short(table, 64);
    bool known = want == NpcCombo::kHeroProfile;
    for (const auto& [name, config] : NpcCombo::All()) { (void)config; known = known || name == want; }
    if (!known) return PBZ_E_NOT_FOUND;
    const std::wstring wide = Wide(text, 16u * 1024u * 1024u);
    if (wide.empty()) return PBZ_E_ARG;
    const NpcCombo::Config config = NpcCombo::Decode(wide);
    NpcCombo::Save(want, config);
    Log("PLUGIN [%s] import_table %s: %zu nodes written", self->id.c_str(), want.c_str(), config.chain.size());
    return PBZ_OK;
}

// ---- table edits -------------------------------------------------------------------------------------------------
// A copy of a table that edit_* change; edit_commit writes it back once, through the save format (Encode, Decode:
// what does not fit is dropped as when the page saves).
struct EditSession { PbzPlugin* owner = nullptr; std::string table; NpcCombo::Config config; };
std::mutex edit_lock_;
std::map<int32_t, EditSession> edits_;
int32_t next_edit_ = 1;
constexpr size_t kMaxEditsEach = 8;
EditSession* EditOf(PbzPlugin* self, PbzEdit edit) {
    const auto it = edits_.find(edit);
    return it != edits_.end() && it->second.owner == self ? &it->second : nullptr;
}
bool TableKnown(const std::string& table) {
    if (table == NpcCombo::kHeroProfile) return true;
    for (const auto& [name, config] : NpcCombo::All()) { (void)config; if (name == table) return true; }
    return false;
}
// The fields edit_get_field / edit_set_field name: a node's (row -1) and a row's.
enum class FieldKind { Int, Bool, Text, List, Points, Mod, Type };
struct NodeField { const char* name = ""; FieldKind kind = FieldKind::Int; int Node::*i = nullptr; bool Node::*b = nullptr; std::string Node::*s = nullptr; std::vector<int> Node::*l = nullptr; int slot = -1; };
struct RouteField { const char* name = ""; FieldKind kind = FieldKind::Int; int NpcCombo::Route::*i = nullptr; bool NpcCombo::Route::*b = nullptr; std::vector<int> NpcCombo::Route::*l = nullptr; };
const char* const kSlotNames[] = {"speed", "armor", "weapon", "interrupt", "damage", "invincible", "keep_look", "force_hit", "ignore_guard", "keep_chain", "trim", "air", "bone", "effect", "hero_size"};
static_assert(std::size(kSlotNames) == static_cast<size_t>(NpcCombo::kModSlots), "a name per modifier slot");
const std::vector<NodeField>& NodeFields() {
    static const std::vector<NodeField> fields = [] {
        std::vector<NodeField> f;
        const auto i = [&](const char* n, int Node::*m) { NodeField x; x.name = n; x.kind = FieldKind::Int; x.i = m; f.push_back(x); };
        const auto b = [&](const char* n, bool Node::*m) { NodeField x; x.name = n; x.kind = FieldKind::Bool; x.b = m; f.push_back(x); };
        const auto s = [&](const char* n, std::string Node::*m) { NodeField x; x.name = n; x.kind = FieldKind::Text; x.s = m; f.push_back(x); };
        const auto l = [&](const char* n, std::vector<int> Node::*m) { NodeField x; x.name = n; x.kind = FieldKind::List; x.l = m; f.push_back(x); };
        { NodeField x; x.name = "type"; x.kind = FieldKind::Type; f.push_back(x); }
        s("ability", &Node::ability); s("boss", &Node::boss); s("weapon", &Node::weapon); s("action", &Node::action);
        i("ms", &Node::ms); i("x", &Node::x); i("y", &Node::y); i("next", &Node::next); i("trigger", &Node::trigger); b("on", &Node::on);
        i("speed", &Node::speed); i("hit", &Node::hit); i("launch", &Node::launch);
        i("air_lift", &Node::air_lift); i("air_hang", &Node::air_hang); i("air_foe_lift", &Node::air_foe_lift); i("air_foe_hang", &Node::air_foe_hang);
        i("group", &Node::group); i("w", &Node::w); i("h", &Node::h); i("dist", &Node::dist); i("lift", &Node::lift);
        l("keys", &Node::keys); b("chord", &Node::chord); i("window", &Node::window); b("swallow", &Node::swallow); i("then", &Node::then);
        b("curve", &Node::curve); { NodeField x; x.name = "points"; x.kind = FieldKind::Points; f.push_back(x); }
        b("own_look", &Node::own_look); b("hook_self", &Node::hook_self); b("hook_auto", &Node::hook_auto); i("hook_dist", &Node::hook_dist); i("hook_speed", &Node::hook_speed);
        i("hold_ms", &Node::hold_ms); i("fx_place", &Node::fx_place); i("fx_damage", &Node::fx_damage); i("fx_radius", &Node::fx_radius); i("fx_hits", &Node::fx_hits);
        i("action_kind", &Node::action_kind); i("value", &Node::value); b("air_set", &Node::air_set);
        i("trim_from", &Node::trim_from); i("trim_to", &Node::trim_to); l("trim_holds", &Node::trim_holds);
        static std::vector<std::string> slot_names;
        for (const char* n : kSlotNames) slot_names.push_back(std::string("mod.") + n);
        for (size_t k = 0; k < slot_names.size(); ++k) { NodeField x; x.name = slot_names[k].c_str(); x.kind = FieldKind::Mod; x.slot = static_cast<int>(k); f.push_back(x); }
        return f;
    }();
    return fields;
}
const std::vector<RouteField>& RouteFields() {
    using NpcCombo::Route;
    static const std::vector<RouteField> fields = [] {
        std::vector<RouteField> f;
        const auto i = [&](const char* n, int Route::*m) { RouteField x; x.name = n; x.kind = FieldKind::Int; x.i = m; f.push_back(x); };
        const auto b = [&](const char* n, bool Route::*m) { RouteField x; x.name = n; x.kind = FieldKind::Bool; x.b = m; f.push_back(x); };
        i("button", &Route::button); i("pause_ms", &Route::pause_ms); i("next", &Route::next);
        { RouteField x; x.name = "keys"; x.kind = FieldKind::List; x.l = &Route::keys; f.push_back(x); }
        b("chord", &Route::chord); i("window", &Route::window); b("swallow", &Route::swallow); i("hold_ms", &Route::hold_ms);
        return f;
    }();
    return fields;
}
bool ParseInt(const std::string& text, int& out) {
    if (text.empty() || text.size() > 24) return false;
    char* end = nullptr; const long long v = std::strtoll(text.c_str(), &end, 10);
    if (!end || *end) return false;
    out = static_cast<int>(std::clamp<long long>(v, INT32_MIN, INT32_MAX)); return true;
}
bool ParseBool(const std::string& text, bool& out) {
    if (text == "1" || text == "true" || text == "on") { out = true; return true; }
    if (text == "0" || text == "false" || text == "off") { out = false; return true; }
    return false;
}
bool ParseList(const std::string& text, std::vector<int>& out) {
    out.clear();
    if (text.empty()) return true;
    size_t at = 0;
    while (at <= text.size()) {
        const size_t comma = text.find(',', at);
        int v = 0; if (!ParseInt(text.substr(at, comma == std::string::npos ? std::string::npos : comma - at), v) || out.size() >= 64) return false;
        out.push_back(v);
        if (comma == std::string::npos) break;
        at = comma + 1;
    }
    return true;
}
std::string ListText(const std::vector<int>& list) {
    std::string out; for (const int v : list) out += (out.empty() ? "" : ",") + std::to_string(v); return out;
}
PbzResult GetNodeField(const Node& n, const std::string& name, std::string& out) {
    for (const NodeField& f : NodeFields()) {
        if (name != f.name) continue;
        switch (f.kind) {
        case FieldKind::Type: out = std::to_string(static_cast<int>(n.type)); break;
        case FieldKind::Int: out = std::to_string(n.*f.i); break;
        case FieldKind::Bool: out = n.*f.b ? "1" : "0"; break;
        case FieldKind::Text: out = n.*f.s; break;
        case FieldKind::List: out = ListText(n.*f.l); break;
        case FieldKind::Points: out.clear(); for (const Node::CurvePoint& p : n.points) out += (out.empty() ? "" : ",") + std::to_string(p.t) + ":" + std::to_string(p.speed); break;
        case FieldKind::Mod: out = std::to_string(n.mods[static_cast<size_t>(f.slot)]); break;
        }
        return PBZ_OK;
    }
    return PBZ_E_NOT_FOUND;
}
PbzResult SetNodeField(Node& n, const std::string& name, const std::string& value) {
    for (const NodeField& f : NodeFields()) {
        if (name != f.name) continue;
        int v = 0; bool on = false; std::vector<int> list;
        switch (f.kind) {
        case FieldKind::Type:
            if (!ParseInt(value, v) || v < 0 || v > static_cast<int>(NodeType::HeroSize)) return PBZ_E_ARG;
            n.type = static_cast<NodeType>(v); break;
        case FieldKind::Int: if (!ParseInt(value, v)) return PBZ_E_ARG; n.*f.i = v; break;
        case FieldKind::Bool: if (!ParseBool(value, on)) return PBZ_E_ARG; n.*f.b = on; break;
        case FieldKind::Text: if (value.size() > 200) return PBZ_E_ARG; n.*f.s = value; break;
        case FieldKind::List: if (!ParseList(value, list)) return PBZ_E_ARG; n.*f.l = list; break;
        case FieldKind::Points: {
            std::vector<Node::CurvePoint> points; size_t at = 0;
            while (!value.empty() && at <= value.size()) {
                const size_t comma = value.find(',', at); const std::string item = value.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
                const size_t colon = item.find(':'); Node::CurvePoint p;
                if (colon == std::string::npos || !ParseInt(item.substr(0, colon), p.t) || !ParseInt(item.substr(colon + 1), p.speed) || points.size() >= 64) return PBZ_E_ARG;
                points.push_back(p);
                if (comma == std::string::npos) break;
                at = comma + 1;
            }
            n.points = points; break;
        }
        case FieldKind::Mod: if (!ParseInt(value, v)) return PBZ_E_ARG; n.mods[static_cast<size_t>(f.slot)] = v; break;
        }
        return PBZ_OK;
    }
    return PBZ_E_NOT_FOUND;
}
PbzResult GetRouteField(const NpcCombo::Route& r, const std::string& name, std::string& out) {
    for (const RouteField& f : RouteFields()) {
        if (name != f.name) continue;
        out = f.kind == FieldKind::Int ? std::to_string(r.*f.i) : f.kind == FieldKind::Bool ? std::string(r.*f.b ? "1" : "0") : ListText(r.*f.l);
        return PBZ_OK;
    }
    return PBZ_E_NOT_FOUND;
}
PbzResult SetRouteField(NpcCombo::Route& r, const std::string& name, const std::string& value) {
    for (const RouteField& f : RouteFields()) {
        if (name != f.name) continue;
        int v = 0; bool on = false; std::vector<int> list;
        if (f.kind == FieldKind::Int) { if (!ParseInt(value, v)) return PBZ_E_ARG; r.*f.i = v; }
        else if (f.kind == FieldKind::Bool) { if (!ParseBool(value, on)) return PBZ_E_ARG; r.*f.b = on; }
        else { if (!ParseList(value, list)) return PBZ_E_ARG; r.*f.l = list; }
        return PBZ_OK;
    }
    return PBZ_E_NOT_FOUND;
}
// A node as the page makes a new one of that type.
Node NewNode(NodeType type, int x, int y) {
    Node probe; probe.type = type;
    if (NpcCombo::IsModifier(probe)) return NpcCombo::ModifierNode(type, x, y);
    if (type == NodeType::Branch) return NpcCombo::BranchNode(x, y);
    if (type == NodeType::WeaponActions) return NpcCombo::WeaponActionsNode(x, y);
    Node node; node.type = type; node.x = x; node.y = y;
    if (type == NodeType::Trigger) { node.trigger = static_cast<int>(NpcCombo::Trigger::Light); node.ms = NpcCombo::kResetDefault; }
    if (type == NodeType::Interval) node.ms = NpcCombo::kIntervalDefault;
    return node;
}
PbzResult PBZ_CALL EditBegin(PbzPlugin* self, const char* table, PbzEdit* edit) {
    if (!Usable(self) || !table || !*table || !edit) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    const std::string want = Short(table, 64);
    if (!TableKnown(want)) return PBZ_E_NOT_FOUND;
    NpcCombo::Config config = NpcCombo::Load(want);
    std::lock_guard<std::mutex> hold(edit_lock_);
    size_t own = 0; for (const auto& [id, e] : edits_) { (void)id; if (e.owner == self) ++own; }
    if (own >= kMaxEditsEach) return PBZ_E_FULL;
    const int32_t id = next_edit_++; if (next_edit_ <= 0) next_edit_ = 1;
    edits_[id] = EditSession{self, want, std::move(config)};
    *edit = id; return PBZ_OK;
}
PbzResult PBZ_CALL EditCommit(PbzPlugin* self, PbzEdit edit) {
    if (!Usable(self)) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    EditSession session;
    {
        std::lock_guard<std::mutex> hold(edit_lock_);
        EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
        session = std::move(*e); edits_.erase(edit);
    }
    const NpcCombo::Config saved = NpcCombo::Decode(NpcCombo::Encode(session.config));  // as the page saves it
    NpcCombo::Save(session.table, saved);
    Log("PLUGIN [%s] edit %s committed: %zu nodes", self->id.c_str(), session.table.c_str(), saved.chain.size());
    return PBZ_OK;
}
PbzResult PBZ_CALL EditCancel(PbzPlugin* self, PbzEdit edit) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    if (!EditOf(self, edit)) return PBZ_E_NOT_FOUND;
    edits_.erase(edit); return PBZ_OK;
}
int32_t PBZ_CALL EditNodeCount(PbzPlugin* self, PbzEdit edit) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    const EditSession* e = EditOf(self, edit);
    return e ? static_cast<int32_t>(e->config.chain.size()) : 0;
}
PbzResult PBZ_CALL EditGetNode(PbzPlugin* self, PbzEdit edit, int32_t index, PbzNodeInfo* out) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    const EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    return FillNode(&e->config, index, out);
}
PbzResult PBZ_CALL EditGetRoute(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t row, PbzRouteInfo* out) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    const EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    return FillRoute(&e->config, node, row, out);
}
PbzResult PBZ_CALL EditAddNode(PbzPlugin* self, PbzEdit edit, int32_t type, int32_t x, int32_t y, int32_t* index) {
    if (type < 0 || type > static_cast<int32_t>(NodeType::HeroSize)) return PBZ_E_ARG;
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    if (e->config.chain.size() >= NpcCombo::kMaxNodes) return PBZ_E_FULL;
    e->config.chain.push_back(NewNode(static_cast<NodeType>(type), x, y));
    if (index) *index = static_cast<int32_t>(e->config.chain.size()) - 1;
    return PBZ_OK;
}
PbzResult PBZ_CALL EditRemoveNode(PbzPlugin* self, PbzEdit edit, int32_t index) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    if (index < 0 || static_cast<size_t>(index) >= e->config.chain.size()) return PBZ_E_NOT_FOUND;
    if (NpcCombo::IsSetting(e->config.chain[static_cast<size_t>(index)])) return PBZ_E_STATE;  // 预输入 / 空中释放 stay
    NpcCombo::Remove(e->config, static_cast<size_t>(index));
    return PBZ_OK;
}
PbzResult PBZ_CALL EditGetField(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t row, const char* field, char* buffer, int32_t size, int32_t* needed) {
    if (!field) return PBZ_E_ARG;
    const std::string name = Short(field, 48); std::string out;
    {
        std::lock_guard<std::mutex> hold(edit_lock_);
        const EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
        if (node == -1) { if (name != "enabled") return PBZ_E_NOT_FOUND; out = e->config.enabled ? "1" : "0"; }
        else {
            if (node < 0 || static_cast<size_t>(node) >= e->config.chain.size()) return PBZ_E_NOT_FOUND;
            const Node& n = e->config.chain[static_cast<size_t>(node)];
            PbzResult r = PBZ_E_NOT_FOUND;
            if (row < 0) r = GetNodeField(n, name, out);
            else if (static_cast<size_t>(row) < n.routes.size()) r = GetRouteField(n.routes[static_cast<size_t>(row)], name, out);
            if (r != PBZ_OK) return r;
        }
    }
    return CopyOut(out, buffer, size, needed);
}
PbzResult PBZ_CALL EditSetField(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t row, const char* field, const char* value) {
    if (!field || !value) return PBZ_E_ARG;
    const std::string name = Short(field, 48), text = Short(value, 4096);
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    if (node == -1) {
        bool on = false;
        if (name != "enabled") return PBZ_E_NOT_FOUND;
        if (!ParseBool(text, on)) return PBZ_E_ARG;
        e->config.enabled = on; return PBZ_OK;
    }
    if (node < 0 || static_cast<size_t>(node) >= e->config.chain.size()) return PBZ_E_NOT_FOUND;
    Node& n = e->config.chain[static_cast<size_t>(node)];
    if (row < 0) return SetNodeField(n, name, text);
    if (static_cast<size_t>(row) >= n.routes.size()) return PBZ_E_NOT_FOUND;
    return SetRouteField(n.routes[static_cast<size_t>(row)], name, text);
}
PbzResult PBZ_CALL EditAddRoute(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t* row) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    if (node < 0 || static_cast<size_t>(node) >= e->config.chain.size()) return PBZ_E_NOT_FOUND;
    Node& n = e->config.chain[static_cast<size_t>(node)];
    if (!NpcCombo::RowOutputs(n)) return PBZ_E_STATE;
    if (n.routes.size() >= NpcCombo::kMaxRoutes) return PBZ_E_FULL;
    NpcCombo::Route route; if (n.type == NodeType::Plugin) route.button = 0;
    n.routes.push_back(route);
    if (row) *row = static_cast<int32_t>(n.routes.size()) - 1;
    return PBZ_OK;
}
PbzResult PBZ_CALL EditRemoveRoute(PbzPlugin* self, PbzEdit edit, int32_t node, int32_t row) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    if (node < 0 || static_cast<size_t>(node) >= e->config.chain.size()) return PBZ_E_NOT_FOUND;
    Node& n = e->config.chain[static_cast<size_t>(node)];
    if (row < 0 || static_cast<size_t>(row) >= n.routes.size()) return PBZ_E_NOT_FOUND;
    n.routes.erase(n.routes.begin() + row);
    return PBZ_OK;
}
PbzResult PBZ_CALL EditLink(PbzPlugin* self, PbzEdit edit, int32_t from, int32_t output, int32_t to) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    const int count = static_cast<int>(e->config.chain.size());
    if (from < 0 || from >= count || to < -1 || to >= count || output < 0) return PBZ_E_ARG;
    if (static_cast<size_t>(output) >= NpcCombo::OutCount(e->config.chain[static_cast<size_t>(from)])) return PBZ_E_NOT_FOUND;
    NpcCombo::Link(e->config, from, to, static_cast<size_t>(output));
    return NpcCombo::OutOf(e->config.chain[static_cast<size_t>(from)], static_cast<size_t>(output)) == to ? PBZ_OK : PBZ_E_ARG;  // a wire it may not have
}
PbzResult PBZ_CALL EditLinkModifier(PbzPlugin* self, PbzEdit edit, int32_t from, int32_t modifier) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    const int count = static_cast<int>(e->config.chain.size());
    if (from < 0 || from >= count || modifier < 0 || modifier >= count) return PBZ_E_ARG;
    return NpcCombo::LinkModifier(e->config, from, modifier) ? PBZ_OK : PBZ_E_ARG;
}
PbzResult PBZ_CALL EditUnlinkModifier(PbzPlugin* self, PbzEdit edit, int32_t from, int32_t slot) {
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    if (from < 0 || static_cast<size_t>(from) >= e->config.chain.size() || slot < -1 || slot >= NpcCombo::kModSlots) return PBZ_E_ARG;
    NpcCombo::UnlinkModifiers(e->config, from, slot);
    return PBZ_OK;
}
PbzResult PBZ_CALL EditExport(PbzPlugin* self, PbzEdit edit, char* buffer, int32_t size, int32_t* needed) {
    std::string text;
    {
        std::lock_guard<std::mutex> hold(edit_lock_);
        const EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
        text = Utf8(NpcCombo::Encode(e->config));
    }
    return CopyOut(text, buffer, size, needed);
}
PbzResult PBZ_CALL EditImport(PbzPlugin* self, PbzEdit edit, const char* text) {
    if (!text || !*text) return PBZ_E_ARG;
    const std::wstring wide = Wide(text, 16u * 1024u * 1024u);
    if (wide.empty()) return PBZ_E_ARG;
    NpcCombo::Config config = NpcCombo::Decode(wide);
    std::lock_guard<std::mutex> hold(edit_lock_);
    EditSession* e = EditOf(self, edit); if (!e) return PBZ_E_NOT_FOUND;
    e->config = std::move(config); return PBZ_OK;
}
PbzResult PBZ_CALL EditFieldNames(int32_t row_fields, char* buffer, int32_t size, int32_t* needed) {
    std::string text;
    if (row_fields) for (const RouteField& f : RouteFields()) text += (text.empty() ? "" : "\n") + std::string(f.name);
    else for (const NodeField& f : NodeFields()) text += (text.empty() ? "" : "\n") + std::string(f.name);
    return CopyOut(text, buffer, size, needed);
}

// ---- options and settings -----------------------------------------------------------------------------------------
PbzResult PBZ_CALL GetOption(const char* name, double* value) {
    if (!name || !value) return PBZ_E_ARG;
    const std::string n = Short(name, 48);
    if (n == "skill_damage") *value = NpcCombo::ChainSkillDamage();
    else if (n == "grab_damage_mode") *value = NpcCombo::ChainGrabDamage().mode;
    else if (n == "grab_damage_value") *value = NpcCombo::ChainGrabDamage().value;
    else if (n == "chain_hud") *value = NpcCombo::ChainHud();
    else if (n == "view_target") *value = NpcCombo::ViewTarget();
    else if (n == "show_toast") *value = NpcCombo::ShowToast() ? 1 : 0;
    else if (n == "plugins") *value = Enabled() ? 1 : 0;
    else return PBZ_E_NOT_FOUND;
    return PBZ_OK;
}
PbzResult PBZ_CALL SetOption(PbzPlugin* self, const char* name, double value) {
    if (!Usable(self) || !name || !std::isfinite(value)) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    const std::string n = Short(name, 48);
    const int whole = static_cast<int>(std::lround(std::clamp(value, -1e6, 1e6)));
    if (n == "skill_damage") NpcCombo::SetChainSkillDamage(value <= 0 ? 0.f : std::clamp(static_cast<float>(value), NpcCombo::kChainSkillMin, NpcCombo::kChainSkillMax));
    else if (n == "grab_damage_mode" || n == "grab_damage_value") {
        NpcCombo::ChainGrab grab = NpcCombo::ChainGrabDamage();
        if (n == "grab_damage_mode") { if (whole < NpcCombo::kGrabFollow || whole > 2) return PBZ_E_ARG; grab.mode = whole; }
        else { if (value < 0) return PBZ_E_ARG; grab.value = static_cast<float>(value); }
        NpcCombo::SetChainGrabDamage(grab);
    }
    else if (n == "chain_hud") { if (whole < NpcCombo::kHudOff || whole > NpcCombo::kHudSimple) return PBZ_E_ARG; NpcCombo::SetChainHud(whole); }
    else if (n == "view_target") { if (whole < NpcCombo::kViewPelvis || whole > NpcCombo::kViewHead) return PBZ_E_ARG; NpcCombo::SetViewTarget(whole); }
    else if (n == "show_toast") NpcCombo::SetShowToast(whole != 0);
    else if (n == "plugins") return PBZ_E_ARG;  // the player's switch
    else return PBZ_E_NOT_FOUND;
    Log("PLUGIN [%s] set_option %s=%g", self->id.c_str(), n.c_str(), value);
    Bump(); return PBZ_OK;
}
bool IniName(const std::string& text) {
    if (text.empty() || text.size() > 64) return false;
    for (const unsigned char c : text) if (c < 0x20 || c == '[' || c == ']' || c == '=' || c == ';') return false;
    return true;
}
PbzResult PBZ_CALL GetSetting(const char* section, const char* key, char* buffer, int32_t size, int32_t* needed) {
    if (!section || !key) return PBZ_E_ARG;
    const std::string s = Short(section, 80), k = Short(key, 80);
    if (!IniName(s) || !IniName(k)) return PBZ_E_ARG;
    const std::wstring value = SettingsStore::Get(Wide(s.c_str()).c_str(), Wide(k.c_str()).c_str(), L"\x1e");
    if (value == L"\x1e") return PBZ_E_NOT_FOUND;
    return CopyOut(Utf8(value), buffer, size, needed);
}
PbzResult PBZ_CALL SetSetting(PbzPlugin* self, const char* section, const char* key, const char* value) {
    if (!Usable(self) || !section || !key || !value) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    const std::string s = Short(section, 80), k = Short(key, 80), v = Short(value, 1024);
    if (!IniName(s) || !IniName(k) || v.find_first_of("\r\n") != std::string::npos) return PBZ_E_ARG;
    if (s == "Plugins" || s == "PluginGrants") return PBZ_E_ARG;  // the player's switches ([Plugins] Enabled, the grants)
    const bool own = s == "NpcCombo" || s == "Plugin." + self->id;  // what PBZ_CAP_COMBO covers
    if (!own) { const PbzResult denied = Need(self, PBZ_CAP_INI, "set_setting"); if (denied != PBZ_OK) return denied; }
    SettingsStore::Put(Wide(s.c_str()).c_str(), Wide(k.c_str()).c_str(), Wide(v.c_str(), 1024));
    Log("PLUGIN [%s] set_setting [%s] %s=%s", self->id.c_str(), s.c_str(), k.c_str(), v.c_str());
    return PBZ_OK;
}

struct FlatMove { const ComboWeapons::Weapon* weapon; const ComboWeapons::Move* move; };
const std::vector<FlatMove>& WeaponMoves() {
    static const std::vector<FlatMove> moves = [] {
        std::vector<FlatMove> out;
        for (const ComboWeapons::Weapon& w : ComboWeaponsData::kWeapons) for (size_t i = 0; i < w.count; ++i) out.push_back({&w, &w.moves[i]});
        return out;
    }();
    return moves;
}
int32_t PBZ_CALL WeaponMoveCount() { return static_cast<int32_t>(WeaponMoves().size()); }
PbzResult PBZ_CALL GetWeaponMove(int32_t index, PbzWeaponMove* out) {
    if (!out || out->size < sizeof(uint32_t)) return PBZ_E_ARG;
    if (index < 0 || static_cast<size_t>(index) >= WeaponMoves().size()) return PBZ_E_NOT_FOUND;
    const FlatMove& m = WeaponMoves()[static_cast<size_t>(index)];
    PbzWeaponMove move{}; move.size = sizeof(move);
    CopyText(move.weapon, m.weapon->id ? m.weapon->id : ""); CopyText(move.ability, m.move->ability ? m.move->ability : "");
    move.kind = m.move->kind; move.shaqi = m.weapon->shaqi ? 1 : 0; CopyText(move.stage, m.move->stage ? m.move->stage : "");
    return WriteOut(out, move) ? PBZ_OK : PBZ_E_ARG;
}

// ---- modules, commands, UI pages (revision 2) ---------------------------------------------------------------------
// The text out of a call that may not want it: no buffer and no size = the output is dropped (the call is made for what it does).
PbzResult Reply(PbzResult r, const std::string& out, char* buffer, int32_t size, int32_t* needed) {
    if (!buffer && size == 0) { if (needed) *needed = static_cast<int32_t>((std::min)(out.size(), static_cast<size_t>(INT32_MAX - 1))) + 1; return r; }
    const PbzResult copied = CopyOut(out, buffer, size, needed);
    return r != PBZ_OK ? r : copied;
}
PbzResult PBZ_CALL ModuleListApi(char* buffer, int32_t size, int32_t* needed) { return CopyOut(ModRegistry::ModuleList(), buffer, size, needed); }
PbzResult ListOf(PbzResult (*list)(const std::string&, std::string&), const char* module, char* buffer, int32_t size, int32_t* needed) {
    if (!module) return PBZ_E_ARG;
    std::string text; const PbzResult r = list(Short(module, 48), text);
    return r != PBZ_OK ? r : CopyOut(text, buffer, size, needed);
}
PbzResult PBZ_CALL PropListApi(const char* module, char* buffer, int32_t size, int32_t* needed) { return ListOf(&ModRegistry::PropList, module, buffer, size, needed); }
PbzResult PBZ_CALL ActionListApi(const char* module, char* buffer, int32_t size, int32_t* needed) { return ListOf(&ModRegistry::ActionList, module, buffer, size, needed); }
PbzResult PBZ_CALL EventListApi(const char* module, char* buffer, int32_t size, int32_t* needed) { return ListOf(&ModRegistry::EventList, module, buffer, size, needed); }

PbzResult PBZ_CALL PropGetApi(PbzPlugin* self, const char* module, const char* prop, char* buffer, int32_t size, int32_t* needed) {
    if (!Usable(self) || !module || !prop) return PBZ_E_ARG;
    const PbzResult denied = Need(self, PBZ_CAP_READ, "prop_get"); if (denied != PBZ_OK) return denied;
    std::string out; const PbzResult r = ModRegistry::Get(Short(module, 48), Short(prop, 48), out, GameThread());
    return r != PBZ_OK ? r : CopyOut(out, buffer, size, needed);
}
PbzResult PBZ_CALL PropSetApi(PbzPlugin* self, const char* module, const char* prop, const char* value) {
    if (!Usable(self) || !module || !prop || !value) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    const PbzResult denied = Need(self, PBZ_CAP_WRITE, "prop_set"); if (denied != PBZ_OK) return denied;
    Op op; op.kind = Op::Kind::SetProp; op.owner = self->id; op.module = Short(module, 48); op.name = Short(prop, 48);
    const PbzResult r = ModRegistry::Check(op.module, op.name, Short(value, 600), op.value);
    if (r != PBZ_OK) return r;
    return Queue(std::move(op)) ? PBZ_OK : PBZ_E_FULL;
}
PbzResult PBZ_CALL ActionInvokeApi(PbzPlugin* self, const char* module, const char* action, const char* arg, int32_t flags, char* buffer, int32_t size, int32_t* needed) {
    if (!Usable(self) || !module || !action || (flags & ~PBZ_CALL_LATER)) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    const PbzResult denied = Need(self, PBZ_CAP_WRITE, "action_invoke"); if (denied != PBZ_OK) return denied;
    const std::string m = Short(module, 48), a = Short(action, 48), argument = Short(arg, 400);
    const PbzResult found = ModRegistry::FindAction(m, a); if (found != PBZ_OK) return found;
    if (flags & PBZ_CALL_LATER) {
        Op op; op.kind = Op::Kind::Invoke; op.owner = self->id; op.module = m; op.name = a; op.value = argument;
        if (needed) *needed = 1;
        if (buffer && size > 0) buffer[0] = 0;
        return Queue(std::move(op)) ? PBZ_OK : PBZ_E_FULL;
    }
    if (!GameThread()) return PBZ_E_THREAD;
    std::string out; const PbzResult r = ModRegistry::Invoke(m, a, argument, out);
    Log("PLUGIN [%s] action_invoke %s.%s(%s) -> %s", self->id.c_str(), m.c_str(), a.c_str(), argument.substr(0, 60).c_str(), r == PBZ_OK ? "ok" : "failed");
    return Reply(r, out, buffer, size, needed);
}
bool Subscribed(const PbzPlugin* p, const std::string& module, const std::string& event) {  // under lock_
    for (const auto& [m, e] : p->subs) if (m == "*" || (m == module && (e == "*" || e == event))) return true;
    return false;
}
PbzResult PBZ_CALL ModuleSubscribeApi(PbzPlugin* self, const char* module, const char* event, int32_t enable) {
    if (!Usable(self)) return PBZ_E_ARG;
    const PbzResult denied = Need(self, PBZ_CAP_READ, "module_subscribe"); if (denied != PBZ_OK) return denied;
    std::string m = module && *module ? Short(module, 48) : "*", e = event && *event ? Short(event, 48) : "*";
    if (m == "*") e = "*";
    else if (!ModRegistry::Has(m) || (e != "*" && !ModRegistry::HasEvent(m, e))) return PBZ_E_NOT_FOUND;
    {
        std::lock_guard<std::mutex> hold(lock_);
        const auto it = std::find(self->subs.begin(), self->subs.end(), std::make_pair(m, e));
        if (!enable) { if (it != self->subs.end()) self->subs.erase(it); return PBZ_OK; }
        if (it != self->subs.end()) return PBZ_OK;
        if (self->subs.size() >= 64) return PBZ_E_FULL;
        self->subs.emplace_back(m, e);
    }
    ModRegistry::SetListening(true);
    return PBZ_OK;
}

PbzResult PBZ_CALL CommandListApi(char* buffer, int32_t size, int32_t* needed) { return CopyOut(ModRegistry::CommandList(), buffer, size, needed); }
PbzResult PBZ_CALL CommandApi(PbzPlugin* self, const char* line, int32_t flags, char* buffer, int32_t size, int32_t* needed) {
    if (!Usable(self) || !line || (flags & ~PBZ_CALL_LATER)) return PBZ_E_ARG;
    if (!Enabled()) return PBZ_E_DISABLED;
    const PbzResult denied = Need(self, PBZ_CAP_COMMAND, "command"); if (denied != PBZ_OK) return denied;
    const std::string text = Trim(Short(line, 400));
    const size_t space = text.find(' ');
    const std::string name = text.substr(0, space), args = space == std::string::npos ? std::string() : Trim(text.substr(space + 1));
    ModRegistry::Command command;
    if (!ModRegistry::FindCommand(name, command)) return PBZ_E_NOT_FOUND;
    if (command.kind == ModRegistry::Kind::Persist) { const PbzResult ini = Need(self, PBZ_CAP_INI, "command (persistent)"); if (ini != PBZ_OK) return ini; }
    Log("PLUGIN [%s] command %s", self->id.c_str(), text.c_str());
    if (command.thread == ModRegistry::Thread::Worker) {  // the panel thread runs it (PumpWorker); the answer comes as host/command_done
        if (!ModRegistry::PostWorker({self->id, name, args, text})) return PBZ_E_FULL;
        return Reply(PBZ_OK, std::string(), buffer, size, needed);
    }
    if (flags & PBZ_CALL_LATER) {
        Op op; op.kind = Op::Kind::Command; op.owner = self->id; op.value = text;
        if (!Queue(std::move(op))) return PBZ_E_FULL;
        return Reply(PBZ_OK, std::string(), buffer, size, needed);
    }
    if (!GameThread()) return PBZ_E_THREAD;
    std::string out; const PbzResult r = ModRegistry::RunCommand(name, args, out);
    return Reply(r, out, buffer, size, needed);
}

PbzResult PBZ_CALL UiPageApi(PbzPlugin* self, const PbzUiPage* page) {
    if (!GameThread()) return PBZ_E_THREAD;
    PbzUiPage in{};
    if (!Usable(self) || !self->described || !ReadIn(page, in)) return PBZ_E_ARG;
    const PbzResult denied = Need(self, PBZ_CAP_UI, "ui_page"); if (denied != PBZ_OK) return denied;
    const PbzResult r = PluginUi::Declare(self->id, self->name, in);
    if (r == PBZ_OK) Bump();
    return r;
}
PbzResult PBZ_CALL UiRemovePageApi(PbzPlugin* self, const char* page) {
    if (!GameThread()) return PBZ_E_THREAD;
    if (!Usable(self) || !page) return PBZ_E_ARG;
    const PbzResult r = PluginUi::Remove(self->id, Short(page, 64));
    if (r == PBZ_OK) Bump();
    return r;
}
PbzResult PBZ_CALL UiSetValueApi(PbzPlugin* self, const char* page, const char* item, double value) {
    if (!Usable(self) || !page || !item) return PBZ_E_ARG;
    const PbzResult denied = Need(self, PBZ_CAP_UI, "ui_set_value"); if (denied != PBZ_OK) return denied;
    return PluginUi::SetValue(self->id, Short(page, 64), Short(item, 64), value);
}
PbzResult PBZ_CALL UiSetTextApi(PbzPlugin* self, const char* page, const char* item, const char* text) {
    if (!Usable(self) || !page || !item || !text) return PBZ_E_ARG;
    const PbzResult denied = Need(self, PBZ_CAP_UI, "ui_set_text"); if (denied != PBZ_OK) return denied;
    return PluginUi::SetText(self->id, Short(page, 64), Short(item, 64), Wide(text, 1200));
}

// The values of bound UI items (a toggle on a module property ...) are read here, on the game thread, a few times a second.
bool SplitBind(const std::string& bind, std::string& module, std::string& name) {
    const size_t at = bind.find_first_of(".:");
    if (at == std::string::npos || at == 0 || at + 1 >= bind.size()) return false;
    module = bind.substr(0, at); name = bind.substr(at + 1); return true;
}
void RefreshBound(uint64_t now) {
    if (now < ui_shown_ms_ + 200) return;
    ui_shown_ms_ = now;
    for (const PluginUi::Binding& b : PluginUi::Bindings()) {
        bool readable = false;
        { std::lock_guard<std::mutex> hold(lock_); const PbzPlugin* p = ReadyById(b.owner); readable = p && Has(p, PBZ_CAP_READ); }
        std::string module, prop, text;
        if (!readable || !SplitBind(b.bind, module, prop) || ModRegistry::Get(module, prop, text, true) != PBZ_OK) continue;
        if (b.kind == PBZ_UI_LABEL || b.kind == PBZ_UI_TEXT) PluginUi::SetShown(b.owner, b.page, b.item, 0, Wide(text.c_str(), 1200), true);
        else PluginUi::SetShown(b.owner, b.page, b.item, std::strtod(text.c_str(), nullptr), std::wstring(), false);
    }
}

void BuildApi() {
    api_ = PbzApi{};
    api_.size = sizeof(PbzApi); api_.version = PBZ_COMBO_API_VERSION; api_.mod_version = kModVersion;
    api_.describe = Describe; api_.set_callbacks = SetCallbacks; api_.log = LogText; api_.notify = NotifyText; api_.now_ms = NowMs; api_.enabled = EnabledNow;
    api_.register_signal = RegisterSignal; api_.fire_signal = FireSignal; api_.set_signal = SetSignal; api_.signal_wanted = SignalWanted;
    api_.register_action = RegisterAction;
    api_.get_state = GetState; api_.node_count = NodeCount; api_.get_node = GetNode; api_.get_route = GetRoute; api_.get_hero = GetHero;
    api_.press = Press; api_.reset_chain = ResetChain; api_.cancel_hero_ability = CancelHeroAbility;
    api_.list_tables = ListTables; api_.export_table = ExportTable; api_.import_table = ImportTable;
    api_.weapon_move_count = WeaponMoveCount; api_.get_weapon_move = GetWeaponMove;
    api_.unregister_signal = UnregisterSignal; api_.unregister_action = UnregisterAction; api_.play_node = PlayNodeNow;
    api_.edit_begin = EditBegin; api_.edit_commit = EditCommit; api_.edit_cancel = EditCancel; api_.edit_node_count = EditNodeCount;
    api_.edit_get_node = EditGetNode; api_.edit_get_route = EditGetRoute; api_.edit_add_node = EditAddNode; api_.edit_remove_node = EditRemoveNode;
    api_.edit_get_field = EditGetField; api_.edit_set_field = EditSetField; api_.edit_add_route = EditAddRoute; api_.edit_remove_route = EditRemoveRoute;
    api_.edit_link = EditLink; api_.edit_link_modifier = EditLinkModifier; api_.edit_unlink_modifier = EditUnlinkModifier;
    api_.edit_export = EditExport; api_.edit_import = EditImport; api_.edit_field_names = EditFieldNames;
    api_.get_option = GetOption; api_.set_option = SetOption; api_.get_setting = GetSetting; api_.set_setting = SetSetting;
    // revision 2
    api_.api_revision = PBZ_API_REVISION; api_.has_feature = HasFeature; api_.describe_ex = DescribeEx; api_.granted_caps = GrantedCaps;
    api_.module_list = ModuleListApi; api_.prop_list = PropListApi; api_.action_list = ActionListApi; api_.event_list = EventListApi;
    api_.prop_get = PropGetApi; api_.prop_set = PropSetApi; api_.action_invoke = ActionInvokeApi; api_.module_subscribe = ModuleSubscribeApi;
    api_.command_list = CommandListApi; api_.command = CommandApi;
    api_.ui_page = UiPageApi; api_.ui_remove_page = UiRemovePageApi; api_.ui_set_value = UiSetValueApi; api_.ui_set_text = UiSetTextApi;
}

// ---- loading --------------------------------------------------------------------------------------------------------
// The plugin's entry, then whether it described itself; on failure its registrations go.
void Start(PbzPlugin* p, PbzPluginMainFn entry) {
    PbzResult result = PBZ_E_STATE;
    loading_ = p;
    const bool ran = Call(p, "PbzPluginMain", [&] { result = entry(&api_, p); });
    loading_ = nullptr;
    const bool ok = ran && result == PBZ_OK && p->described;
    {
        std::lock_guard<std::mutex> hold(lock_);
        p->ready = ok;
        if (!ok) {
            if (p->error.empty()) p->error = !ran ? L"入口函数出错" : !p->described ? L"入口函数没有调用 describe" : L"入口函数返回失败（" + std::to_wstring(result) + L"）";
            signals_.erase(std::remove_if(signals_.begin(), signals_.end(), [&](const Signal& s) { return s.owner == p; }), signals_.end());
            actions_.erase(std::remove_if(actions_.begin(), actions_.end(), [&](const Action& a) { return a.owner == p; }), actions_.end());
            p->callbacks = PbzCallbacks{};
            p->subs.clear();
            PluginUi::RemoveOwner(p->id);
        }
        bool listening = false, wants = false;
        for (const auto& q : plugins_) if (q->ready && !q->faulted) { listening = listening || q->callbacks.on_event; wants = wants || q->callbacks.on_hero_ability; }
        listening_ = listening; wants_abilities_ = wants;
    }
    Bump();
    size_t signals = 0, actions = 0;
    { std::lock_guard<std::mutex> hold(lock_); for (const Signal& s : signals_) if (s.owner == p) ++signals; for (const Action& a : actions_) if (a.owner == p) ++actions; }
    if (ok) Log("PLUGIN [%s] loaded: %s %s by %s (%s) signals=%zu actions=%zu", p->id.c_str(), Utf8(p->name).c_str(), Utf8(p->version).c_str(), Utf8(p->author).c_str(),
                Utf8(p->file).c_str(), signals, actions);
    else Log("PLUGIN %s not loaded: %s (result %d)", Utf8(p->file).c_str(), Utf8(p->error).c_str(), result);
}
struct LoadArgs { const wchar_t* path; HMODULE module; };
void LoadAll() {
    loaded_ = true;
    const std::wstring folder = Folder();
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((folder + L"\\*.dll").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do { if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(found.cFileName); } while (FindNextFileW(find, &found) && files.size() < 256);
        FindClose(find);
    }
    std::sort(files.begin(), files.end());
    Log("PLUGIN folder %s: %zu DLL(s)", Utf8(folder).c_str(), files.size());
    for (const std::wstring& name : files) {
        if (DllCount() >= kMaxPlugins) { Log("PLUGIN %s skipped: at most %zu plugins", Utf8(name).c_str(), kMaxPlugins); continue; }
        auto record = std::make_unique<PbzPlugin>(); PbzPlugin* p = record.get(); p->file = name;
        { std::lock_guard<std::mutex> hold(lock_); plugins_.push_back(std::move(record)); }
        const std::wstring path = folder + L"\\" + name;
        LoadArgs args{path.c_str(), nullptr};
        DWORD code = 0;
        const bool ran = Guarded([](void* c) { auto& a = *static_cast<LoadArgs*>(c); a.module = LoadLibraryExW(a.path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS); }, &args, code);
        if (!ran) { p->error = L"载入时出错"; p->faulted = true; Log("PLUGIN %s fault 0x%08lx while loading", Utf8(name).c_str(), code); Bump(); continue; }
        if (!args.module) { p->error = L"无法载入（缺少依赖或不是 x64 DLL）"; Log("PLUGIN %s LoadLibrary failed (error %lu)", Utf8(name).c_str(), GetLastError()); Bump(); continue; }
        p->module = args.module;
        const auto entry = reinterpret_cast<PbzPluginMainFn>(reinterpret_cast<void*>(GetProcAddress(args.module, "PbzPluginMain")));
        if (!entry) { p->error = L"没有导出 PbzPluginMain"; Log("PLUGIN %s has no PbzPluginMain: unloaded", Utf8(name).c_str()); FreeLibrary(args.module); p->module = nullptr; Bump(); continue; }
        Start(p, entry);
    }
    Bump();
}

// ---- per frame ----------------------------------------------------------------------------------------------------
std::vector<PbzPlugin*> Ready() {
    std::vector<PbzPlugin*> out;
    std::lock_guard<std::mutex> hold(lock_);
    for (const auto& p : plugins_) if (p->ready && !p->faulted) out.push_back(p.get());
    return out;
}
// The queued module writes, actions, commands, UI events and grant changes (game thread). The owner's grant is checked again
// here: the player may have taken it back since the call was queued.
PbzPlugin* AllowedOwner(const std::string& owner, uint32_t cap) {
    std::lock_guard<std::mutex> hold(lock_);
    PbzPlugin* p = ReadyById(owner);
    return p && Has(p, cap) ? p : nullptr;
}
void RunOp(const Op& op) {
    switch (op.kind) {
    case Op::Kind::SetProp: {
        if (!AllowedOwner(op.owner, PBZ_CAP_WRITE)) { Log("PLUGIN [%s] prop_set %s.%s skipped: no longer allowed", op.owner.c_str(), op.module.c_str(), op.name.c_str()); return; }
        std::string why; const PbzResult r = ModRegistry::Apply(op.module, op.name, op.value, why);
        Log("PLUGIN [%s] prop_set %s.%s=%s%s%s", op.owner.c_str(), op.module.c_str(), op.name.c_str(), op.value.substr(0, 60).c_str(), r == PBZ_OK ? "" : " failed: ", r == PBZ_OK ? "" : why.c_str());
        return;
    }
    case Op::Kind::Invoke: {
        if (!AllowedOwner(op.owner, PBZ_CAP_WRITE)) { Log("PLUGIN [%s] action %s.%s skipped: no longer allowed", op.owner.c_str(), op.module.c_str(), op.name.c_str()); return; }
        std::string out; const PbzResult r = ModRegistry::Invoke(op.module, op.name, op.value, out);
        Log("PLUGIN [%s] action_invoke %s.%s(%s) -> %s", op.owner.c_str(), op.module.c_str(), op.name.c_str(), op.value.substr(0, 60).c_str(), r == PBZ_OK ? "ok" : "failed");
        PushPrivate(op.owner, "action_done", std::string(r == PBZ_OK ? "ok" : "fail") + "\t" + op.module + "." + op.name + (op.value.empty() ? "" : " " + op.value) + "\t" + out);
        return;
    }
    case Op::Kind::Command: {
        const size_t space = op.value.find(' ');
        const std::string name = op.value.substr(0, space), args = space == std::string::npos ? std::string() : Trim(op.value.substr(space + 1));
        ModRegistry::Command command; std::string out; PbzResult r = PBZ_E_NOT_FOUND;
        if (!AllowedOwner(op.owner, PBZ_CAP_COMMAND)) { r = PBZ_E_DENIED; out = "denied"; }
        else if (ModRegistry::FindCommand(name, command) && command.kind == ModRegistry::Kind::Persist && !AllowedOwner(op.owner, PBZ_CAP_INI)) { r = PBZ_E_DENIED; out = "denied"; }
        else r = ModRegistry::RunCommand(name, args, out);
        Log("PLUGIN [%s] command %s -> %s", op.owner.c_str(), op.value.c_str(), r == PBZ_OK ? "ok" : "failed");
        PushPrivate(op.owner, "command_done", std::string(r == PBZ_OK ? "ok" : "fail") + "\t" + op.value + "\t" + out);
        return;
    }
    case Op::Kind::UiEvent: {
        PbzPlugin* p = nullptr; { std::lock_guard<std::mutex> hold(lock_); p = ReadyById(op.owner); }
        if (!p || !p->callbacks.on_ui) return;
        PbzUiEvent e{}; e.size = sizeof(e); e.page = op.module.c_str(); e.item = op.name.c_str(); e.kind = op.code; e.value = op.number; e.now_ms = GetTickCount64();
        const auto fn = p->callbacks.on_ui; void* user = p->callbacks.user; const PbzUiEvent* ptr = &e;
        Call(p, "on_ui", [&] { fn(user, ptr); });
        return;
    }
    case Op::Kind::Caps: {
        PbzPlugin* p = nullptr; { std::lock_guard<std::mutex> hold(lock_); p = ReadyById(op.owner); }
        if (!p || !p->callbacks.on_caps) return;
        const auto fn = p->callbacks.on_caps; void* user = p->callbacks.user; const uint32_t granted = p->granted.load();
        Call(p, "on_caps", [&] { fn(user, granted); });
        return;
    }
    default: return;
    }
}
void DeliverModuleEvent(PbzPlugin* p, const std::string& module, const std::string& event, const std::string& text, uint64_t ms) {
    if (!p->callbacks.on_module_event) return;
    PbzModuleEvent e{}; e.size = sizeof(e); e.module = module.c_str(); e.event = event.c_str(); e.text = text.c_str(); e.now_ms = ms;
    const auto fn = p->callbacks.on_module_event; void* user = p->callbacks.user; const PbzModuleEvent* ptr = &e;
    Call(p, "on_module_event", [&] { fn(user, ptr); });
}
void Deliver() {
    std::vector<QueuedEvent> events; events.swap(events_);
    const std::vector<PbzPlugin*> ready = Ready();
    for (QueuedEvent& q : events) {
        q.event.text = q.text.c_str(); q.event.ability = q.ability.c_str(); q.event.boss = q.boss.c_str();
        for (PbzPlugin* p : ready) if (p->callbacks.on_event) {
            const auto fn = p->callbacks.on_event; void* user = p->callbacks.user; const PbzEvent* e = &q.event;
            Call(p, "on_event", [&] { fn(user, e); });
        }
    }
    // Module events (raised by modules on any thread; only the plugins that subscribed, and may read), and the answers to
    // commands and queued actions (only the plugin that asked).
    const std::vector<ModRegistry::Emitted> raised = ModRegistry::Drain();
    std::vector<PrivateEvent> answers; { std::lock_guard<std::mutex> hold(lock_); answers.swap(private_events_); }
    for (const ModRegistry::Emitted& m : raised)
        for (PbzPlugin* p : ready) {
            if (!p->callbacks.on_module_event || !Has(p, PBZ_CAP_READ)) continue;
            bool wants = false; { std::lock_guard<std::mutex> hold(lock_); wants = Subscribed(p, m.module, m.event); }
            if (wants) DeliverModuleEvent(p, m.module, m.event, m.text, m.ms);
        }
    for (const PrivateEvent& a : answers)
        for (PbzPlugin* p : ready) if (p->id == a.owner) DeliverModuleEvent(p, "host", a.event, a.text, a.ms);
}
}  // namespace

void Initialize(const Bridge& bridge, TextSink log, NoticeSink notify) {
    bridge_ = bridge; log_ = log; notify_ = notify;
    ModRegistry::SetLog(log);
    BuildApi();
    RegisterModules();
    NpcCombo::PluginDecide = &Decide;
}
bool Enabled() {
    int on = enabled_.load();
    if (on < 0) { on = SettingsStore::GetBool(L"Plugins", L"Enabled", false) ? 1 : 0; enabled_ = on; }
    return on == 1;
}
void SetEnabled(bool on) {
    enabled_ = on ? 1 : 0;
    SettingsStore::PutBool(L"Plugins", L"Enabled", on);
    Bump();
}
void Frame(uint64_t now) {
    game_thread_ = GetCurrentThreadId();
    ++frame_;
    const bool on = Enabled();
    if (on && !loaded_) LoadAll();
    if (last_on_ != (on ? 1 : 0)) {
        const bool first = last_on_ < 0; last_on_ = on ? 1 : 0;
        if (!on) { std::lock_guard<std::mutex> hold(lock_); for (Signal& s : signals_) { s.level = false; s.pulse_on = false; s.pulses = 0; } }
        if (!first || !on) for (PbzPlugin* p : Ready()) if (p->callbacks.on_enabled) {
            const auto fn = p->callbacks.on_enabled; void* user = p->callbacks.user; const int32_t value = on ? 1 : 0;
            Call(p, "on_enabled", [&] { fn(user, value); });
        }
        if (loaded_) Log("PLUGIN 派生插件 %s", on ? "on" : "off");
    }
    if (!on) { events_.clear(); ModRegistry::Drain(); std::lock_guard<std::mutex> hold(lock_); ops_.clear(); private_events_.clear(); return; }
    {   // attached from outside (PbzAttachPlugin): started here, on the game thread
        std::vector<std::pair<PbzPluginMainFn, std::wstring>> attached;
        { std::lock_guard<std::mutex> hold(lock_); attached.swap(attached_); }
        for (const auto& [entry, name] : attached) {
            if (DllCount() >= kMaxPlugins) { Log("PLUGIN %s skipped: at most %zu plugins", Utf8(name).c_str(), kMaxPlugins); continue; }
            auto record = std::make_unique<PbzPlugin>(); PbzPlugin* p = record.get(); p->file = name;
            { std::lock_guard<std::mutex> hold(lock_); plugins_.push_back(std::move(record)); }
            Start(p, entry);
        }
    }
    if (script_host_ && ScriptsEnabled()) {  // Lua scripts: read the folder once, again when a reload was asked for (the DLLs stay)
        if (reload_scripts_.exchange(false)) { RetireScripts(); script_host_->unload(); scripts_loaded_ = false; Log("PLUGIN scripts: reload"); }
        if (!scripts_loaded_) { scripts_loaded_ = true; script_host_->load(); }
    }
    ModRegistry::RunPollers();  // modules with no event hook of their own raise theirs from their snapshots
    Deliver();
    for (PbzPlugin* p : Ready()) if (p->callbacks.on_tick) {
        const auto fn = p->callbacks.on_tick; void* user = p->callbacks.user;
        Call(p, "on_tick", [&] { fn(user, now); });
    }
    std::vector<Op> ops;
    { std::lock_guard<std::mutex> hold(lock_); ops.swap(ops_); }
    for (Op& op : ops) {
        switch (op.kind) {
        case Op::Kind::Fire: case Op::Kind::Set: {
            std::lock_guard<std::mutex> hold(lock_);
            for (Signal& s : signals_) if (s.code == op.code) { if (op.kind == Op::Kind::Fire) s.pulses = (std::min)(s.pulses + 1, 8); else s.level = op.down; }
            break;
        }
        case Op::Kind::Press: if (bridge_.press) bridge_.press(op.button, op.text.c_str()); break;
        case Op::Kind::Reset: if (bridge_.reset) bridge_.reset(op.text.c_str()); break;
        case Op::Kind::Notify: if (notify_) notify_(op.title.c_str(), op.wide.c_str()); break;
        case Op::Kind::Play:
            if (bridge_.play && !bridge_.play(op.code, op.button)) Log("PLUGIN [%s] play_node %d: not played (no chain, or not a move node)", op.text.c_str(), op.code);
            break;
        default: RunOp(op); break;  // module writes, actions, commands, UI events, grant changes
        }
    }
    RefreshBound(now);
    // A pulse: down for one frame, then up for one (two pulses in a row are two presses).
    std::lock_guard<std::mutex> hold(lock_);
    for (Signal& s : signals_) {
        if (s.pulse_on) s.pulse_on = false;
        else if (s.pulses > 0) { --s.pulses; s.pulse_on = true; }
    }
}
bool Listening() { return listening_ && Enabled(); }
void Emit(const PbzEvent& event) {
    if (!Listening() || !GameThread() || events_.size() >= kMaxEvents) return;
    QueuedEvent q; q.event = event; q.event.size = sizeof(PbzEvent); q.event.now_ms = event.now_ms ? event.now_ms : GetTickCount64();
    q.text = Short(event.text, 200); q.ability = Short(event.ability, 200); q.boss = Short(event.boss, 100);
    events_.push_back(std::move(q));
}
bool WantsAbilities() { return wants_abilities_ && Enabled(); }
int HeroAbility(const PbzAbilityEvent& ability) {
    if (!WantsAbilities() || !GameThread()) return PBZ_PASS;
    PbzAbilityEvent e = ability; e.size = sizeof(e);
    int out = PBZ_PASS;
    for (PbzPlugin* p : Ready()) if (p->callbacks.on_hero_ability) {
        const auto fn = p->callbacks.on_hero_ability; void* user = p->callbacks.user; int32_t answer = PBZ_PASS;
        if (Call(p, "on_hero_ability", [&] { answer = fn(user, &e); }) && answer == PBZ_CONSUME) out = PBZ_CONSUME;
    }
    return out;
}
bool SignalDown(int code) {
    if (!NpcCombo::IsSignalKey(code) || !Enabled()) return false;
    std::lock_guard<std::mutex> hold(lock_);
    for (const Signal& s : signals_) if (s.code == code) return !s.owner->faulted && (s.level || s.pulse_on);
    return false;
}

int Decide(const NpcCombo::Config& config, int index, const NpcCombo::Press& press, int button) {
    if (index < 0 || static_cast<size_t>(index) >= config.chain.size() || !Enabled()) return -1;
    const Node& node = config.chain[static_cast<size_t>(index)];
    if (!NpcCombo::PluginDecides(node) || node.action.empty()) return -1;
    if (!GameThread()) {
        std::lock_guard<std::mutex> hold(lock_);
        const auto it = last_answer_.find(index); return it == last_answer_.end() ? -1 : it->second;
    }
    if (decided_frame_ != frame_) { decided_.clear(); decided_frame_ = frame_; }
    DecideKey key; key.index = index; key.button = button; key.value = node.value; key.bits = press.bits; key.key = press.key; key.action = node.action;
    for (const auto& [k, answer] : decided_) if (k == key) return answer;
    PbzDecideFn fn = nullptr; void* user = nullptr; PbzPlugin* owner = nullptr; int outputs = 0; std::string short_id;
    {
        std::lock_guard<std::mutex> hold(lock_);
        for (const Action& a : actions_)
            if (a.id == node.action && a.kind == PBZ_ACTION_DECIDE && a.owner->ready && !a.owner->faulted) {
                fn = a.decide; user = a.user; owner = a.owner; outputs = static_cast<int>(a.outputs.size()); short_id = a.short_id; break; }
    }
    int answer = -1;
    if (fn) {
        const char* table = "";
        PbzChainState state{}; state.size = sizeof(state);
        if (bridge_.state && bridge_.state(state)) table = state.table;
        PbzNodeCall call{}; call.size = sizeof(call); call.node = index; call.trigger = bridge_.trigger ? bridge_.trigger() : -1; call.button = button; call.value = node.value;
        call.press_bits = press.bits; call.press_npc_key = press.key ? 1 : 0; call.output_count = outputs; call.action = short_id.c_str(); call.table = table; call.now_ms = GetTickCount64();
        int32_t got = -1;
        if (Call(owner, "decide", [&] { got = fn(user, &call); })) answer = got >= 0 && got < outputs ? got : -1;
    }
    if (decided_.size() < 256) decided_.push_back({key, answer});
    std::lock_guard<std::mutex> hold(lock_);
    last_answer_[index] = answer;
    return answer;
}
void Run(const NpcCombo::Config& config, int index, int button, unsigned press_bits, bool npc_key, const char* table) {
    if (index < 0 || static_cast<size_t>(index) >= config.chain.size() || !Enabled() || !GameThread()) return;
    const Node& node = config.chain[static_cast<size_t>(index)];
    if (!NpcCombo::PluginRuns(node)) return;
    PbzRunFn fn = nullptr; void* user = nullptr; PbzPlugin* owner = nullptr; std::string short_id;
    {
        std::lock_guard<std::mutex> hold(lock_);
        for (const Action& a : actions_)
            if (a.id == node.action && a.kind == PBZ_ACTION_RUN && a.owner->ready && !a.owner->faulted) { fn = a.run; user = a.user; owner = a.owner; short_id = a.short_id; break; }
    }
    if (!fn) { Log("PLUGIN node=%d action %s: no plugin loaded has it (nothing played)", index, node.action.c_str()); return; }
    PbzNodeCall call{}; call.size = sizeof(call); call.node = index; call.trigger = bridge_.trigger ? bridge_.trigger() : -1; call.button = button; call.value = node.value;
    call.press_bits = press_bits; call.press_npc_key = npc_key ? 1 : 0; call.action = short_id.c_str(); call.table = table ? table : ""; call.now_ms = GetTickCount64();
    Call(owner, "run", [&] { fn(user, &call); });
}

uint64_t Serial() { return serial_.load(); }
std::vector<ActionView> Actions() {
    std::vector<ActionView> out;
    std::lock_guard<std::mutex> hold(lock_);
    for (const Action& a : actions_) {
        if (!a.owner->ready) continue;
        ActionView v; v.id = a.id; v.name = a.name; v.group = a.group; v.description = a.description; v.plugin = a.owner->name; v.kind = a.kind; v.outputs = a.outputs;
        v.has_value = a.has_value; v.value_label = a.value_label; v.value_unit = a.value_unit; v.value_min = a.value_min; v.value_max = a.value_max;
        v.value_step = a.value_step; v.value_default = a.value_default;
        out.push_back(std::move(v));
    }
    return out;
}
bool FindAction(const std::string& id, ActionView& out) {
    for (ActionView& v : Actions()) if (v.id == id) { out = std::move(v); return true; }
    return false;
}
std::vector<SignalView> Signals() {
    std::vector<SignalView> out;
    std::lock_guard<std::mutex> hold(lock_);
    for (const Signal& s : signals_) if (s.owner->ready) out.push_back({s.code, s.id, s.name, s.group, s.description, s.owner->name});
    return out;
}
std::wstring SignalName(int code) {
    std::lock_guard<std::mutex> hold(lock_);
    for (const Signal& s : signals_) if (s.code == code && s.owner->ready) return s.name;
    return {};
}
std::vector<PluginView> Plugins() {
    std::vector<PluginView> out;
    std::lock_guard<std::mutex> hold(lock_);
    for (const auto& p : plugins_) {
        if (p->retired) continue;  // a script a reload stopped: its new copy is in the list
        PluginView v; v.id = p->id; v.file = p->file; v.name = p->name; v.version = p->version; v.author = p->author; v.description = p->description; v.error = p->error;
        v.script = p->script;
        v.loaded = p->ready; v.faulted = p->faulted; v.requested = p->requested; v.granted = p->granted.load(); v.caps_reason = p->caps_reason; v.revision = p->min_revision;
        for (const Signal& s : signals_) if (s.owner == p.get()) ++v.signals;
        for (const Action& a : actions_) if (a.owner == p.get()) ++v.actions;
        out.push_back(std::move(v));
    }
    return out;
}
bool Loaded() { return loaded_; }

// ---- Lua scripts (lua_host.cpp): plugins that are not DLLs ---------------------------------------------------------------
void SetScriptHost(const ScriptHost* host) { script_host_ = host; }
std::wstring ScriptFolder() { return Folder() + L"\\scripts"; }
bool ScriptsEnabled() { return SettingsStore::GetBool(L"Plugins", L"Scripts", true); }  // a kill switch; scripts only load once 派生插件 is on
void RequestScriptReload() { reload_scripts_ = true; Bump(); }
bool ScriptsLoaded() { return scripts_loaded_; }
size_t ScriptCount() {
    size_t n = 0;
    std::lock_guard<std::mutex> hold(lock_);
    for (const auto& p : plugins_) if (p->script && !p->retired && p->ready && !p->faulted) ++n;
    return n;
}
PbzPlugin* StartScript(PbzPluginMainFn entry, const std::wstring& file) {
    if (!entry || !GameThread()) return nullptr;
    size_t scripts = 0; for (const auto& p : plugins_) if (p->script && !p->retired) ++scripts;
    if (scripts >= kMaxScripts) { Log("PLUGIN %s skipped: at most %zu scripts", Utf8(file).c_str(), kMaxScripts); return nullptr; }
    auto record = std::make_unique<PbzPlugin>(); PbzPlugin* p = record.get(); p->file = file; p->script = true;
    { std::lock_guard<std::mutex> hold(lock_); plugins_.push_back(std::move(record)); }
    Start(p, entry);
    return p;
}
void LogHost(const std::string& text) { Log("PLUGIN %s", text.substr(0, 700).c_str()); }
void RetireScripts() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> hold(lock_);
        for (const auto& p : plugins_) if (p->script && !p->retired) { p->retired = true; p->ready = false; p->callbacks = PbzCallbacks{}; p->subs.clear(); ids.push_back(p->id); }
        const auto gone = [](const PbzPlugin* owner) { return owner->script && owner->retired; };
        signals_.erase(std::remove_if(signals_.begin(), signals_.end(), [&](const Signal& s) { return gone(s.owner); }), signals_.end());
        actions_.erase(std::remove_if(actions_.begin(), actions_.end(), [&](const Action& a) { return gone(a.owner); }), actions_.end());
        ops_.erase(std::remove_if(ops_.begin(), ops_.end(), [&](const Op& op) { return !op.owner.empty() && std::find(ids.begin(), ids.end(), op.owner) != ids.end(); }), ops_.end());
        private_events_.erase(std::remove_if(private_events_.begin(), private_events_.end(), [&](const PrivateEvent& e) { return std::find(ids.begin(), ids.end(), e.owner) != ids.end(); }), private_events_.end());
        bool listening = false, wants = false;
        for (const auto& q : plugins_) if (q->ready && !q->faulted) { listening = listening || q->callbacks.on_event; wants = wants || q->callbacks.on_hero_ability; }
        listening_ = listening; wants_abilities_ = wants;
        last_answer_.clear();
    }
    decided_.clear();
    for (const std::string& id : ids) PluginUi::RemoveOwner(id);
    Bump();
}
void SetScriptStatus(PbzPlugin* self, const std::string& message) {
    if (self) self->error = Wide(message.substr(0, 300).c_str(), 300);
}
void ScriptFailed(PbzPlugin* self, const std::string& where, const std::string& message) {
    if (!self || self->faulted) return;
    std::string text = message.substr(0, 400);
    for (char& c : text) if (c == '\n') c = '|'; else if (c == '\r' || c == '\t') c = ' ';
    { std::lock_guard<std::mutex> hold(lock_); self->faulted = true; self->error = L"脚本出错，本次游戏内已停用：" + Wide(text.c_str(), 300); }
    PluginUi::RemoveOwner(self->id);
    Bump();
    Log("PLUGIN [%s] script error in %s: %s (switched off for this session)", self->id.empty() ? Utf8(self->file).c_str() : self->id.c_str(), where.c_str(), text.c_str());
    if (notify_) { const std::wstring note = (self->name.empty() ? self->file : self->name) + L" 脚本出错，本次游戏内已停用"; notify_(L"派生插件", note.c_str()); }
}
bool HandleCommand(const char* argument) {
    const std::string a = argument ? Trim(argument) : std::string();
    if (a == "reload") { RequestScriptReload(); return true; }
    if (a == "status") {
        size_t scripts = 0, failed = 0;
        for (const PluginView& p : Plugins()) if (p.script) { ++scripts; if (!p.loaded) ++failed; }
        Log("PLUGINS status: enabled=%d scripts=%zu not_in_use=%zu folder=%s", Enabled() ? 1 : 0, scripts, failed, Utf8(ScriptFolder()).c_str());
        return true;
    }
    return false;
}

// ---- permissions, UI pages, the panel thread's turn (for the panel and proxy.cpp) -----------------------------------
void SetGrant(const std::string& id, uint32_t caps) {
    if (!PluginText::IdText(id)) return;
    caps &= PBZ_CAP_ALL & ~PBZ_CAP_COMBO;
    SettingsStore::Put(L"PluginGrants", Wide(id.c_str()).c_str(), Wide(CapsText(caps).c_str()));
    bool known = false;
    {
        std::lock_guard<std::mutex> hold(lock_);
        for (const auto& p : plugins_) if (p->described && p->id == id) { p->granted = GrantFor(id, p->requested); known = true; }
        if (known && ops_.size() < kMaxQueued) { Op op; op.kind = Op::Kind::Caps; op.owner = id; ops_.push_back(std::move(op)); }  // on_caps, next frame
    }
    const std::string names = CapsText(caps);
    Log("PLUGIN [%s] player grants: %s%s", id.c_str(), names.empty() ? "(none)" : names.c_str(), known ? "" : " (plugin not loaded)");
    Bump();
}
std::vector<PluginUi::Page> UiPages() { return PluginUi::Snapshot(); }
void UiInput(const std::string& owner, const std::string& page, const std::string& item, double value) {
    if (!Enabled() || !std::isfinite(value)) return;
    PluginUi::Item it;
    if (!PluginUi::FindItem(owner, page, item, it) || it.kind == PBZ_UI_LABEL || it.kind == PBZ_UI_TEXT) return;
    PbzPlugin* p = nullptr; { std::lock_guard<std::mutex> hold(lock_); p = ReadyById(owner); }
    if (!p) return;
    Op op; op.owner = owner;
    if (it.bound()) {  // the host writes the property (or runs the action) on the plugin's behalf, with the plugin's permission
        std::string module, name;
        if (!SplitBind(it.bind, module, name) || Need(p, PBZ_CAP_WRITE, "ui (bound control)") != PBZ_OK) return;
        op.module = module; op.name = name;
        if (it.kind == PBZ_UI_BUTTON) { op.kind = Op::Kind::Invoke; op.value = Utf8(it.text).substr(0, 400); Queue(std::move(op)); return; }
        ModRegistry::PropInfo info;
        if (ModRegistry::FindProp(module, name, info) != PBZ_OK) return;
        char text[64]{};
        if (info.type == ModRegistry::Type::Float) _snprintf_s(text, sizeof(text), _TRUNCATE, "%.9g", value);
        else _snprintf_s(text, sizeof(text), _TRUNCATE, "%lld", static_cast<long long>(std::llround(value)));
        std::string canonical;
        if (ModRegistry::Check(module, name, text, canonical) != PBZ_OK) return;  // outside the property's range
        op.kind = Op::Kind::SetProp; op.value = canonical;
        if (Queue(std::move(op))) PluginUi::SetValue(owner, page, item, value);
        return;
    }
    op.kind = Op::Kind::UiEvent; op.module = page; op.name = item; op.code = it.kind; op.number = it.kind == PBZ_UI_BUTTON ? 1 : value;
    if (Queue(std::move(op)) && it.kind != PBZ_UI_BUTTON) PluginUi::SetValue(owner, page, item, value);  // the control shows it at once
}
namespace {
bool WorkerAllow(const ModRegistry::WorkerJob& job) {
    if (!Enabled() || !AllowedOwner(job.owner, PBZ_CAP_COMMAND)) return false;
    ModRegistry::Command command;
    return !(ModRegistry::FindCommand(job.name, command) && command.kind == ModRegistry::Kind::Persist && !AllowedOwner(job.owner, PBZ_CAP_INI));
}
void WorkerDone(const std::string& owner, bool ok, const std::string& line, const std::string& out) {
    Log("PLUGIN [%s] command %s -> %s", owner.c_str(), line.c_str(), ok ? "ok" : "failed");
    PushPrivate(owner, "command_done", std::string(ok ? "ok" : "fail") + "\t" + line + "\t" + out);
}
}  // namespace
size_t PumpWorker() { return ModRegistry::PumpWorker(&WorkerAllow, &WorkerDone); }
void RegisterModules() {
    ModRegistry::ModuleBuilder("host", "宿主", "豪之刃 MOD 本身：接口修订、版本、派生插件的状态；命令和排队动作的结果事件", ModRegistry::Thread::Any)
        .ReadOnlyInt("api_revision", "接口修订", [] { return static_cast<int>(PBZ_API_REVISION); })
        .ReadOnlyText("mod_version", "MOD 版本", [] { return std::string(kModVersion); })
        .ReadOnlyBool("plugins_enabled", "派生插件已打开", [] { return Enabled(); }, "Plugins.Enabled")
        .ReadOnlyInt("plugins_loaded", "已载入的插件数", [] { int n = 0; for (const PluginView& p : Plugins()) if (p.loaded && !p.faulted) ++n; return n; })
        .ReadOnlyInt("scripts_loaded", "在用的 Lua 脚本数", [] { return static_cast<int>(ScriptCount()); })
        .ReadOnlyBool("scripts_enabled", "允许 Lua 脚本", [] { return ScriptsEnabled(); }, "Plugins.Scripts", "只有玩家能改（ini）")
        .Action("reload_scripts", "重新读取 Lua 脚本", "", false, [](const std::string&, std::string&) { RequestScriptReload(); return true; })
        .Event("command_done", "命令完成", "只回给发命令的插件（不用订阅）：ok 或 fail、TAB、命令行、TAB、输出")
        .Event("action_done", "排队的动作完成", "只回给发起的插件（不用订阅）：ok 或 fail、TAB、模块.动作 参数、TAB、输出")
        .Commit();
    ModRegistry::Command reload; reload.name = "plugins.reload_scripts"; reload.kind = ModRegistry::Kind::State; reload.thread = ModRegistry::Thread::Any;
    reload.note = "停掉所有 Lua 脚本并重新读取脚本文件夹（DLL 插件不动）；下一帧在游戏线程上做";
    reload.run = [](const std::string&, std::string& out) { RequestScriptReload(); out = "scheduled"; return true; };
    ModRegistry::RegisterCommand(std::move(reload));
}
std::wstring Folder() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Folder), &self)) return L"PBZPlugins";
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(self, path, MAX_PATH);
    if (!length || length >= MAX_PATH) return L"PBZPlugins";
    std::wstring dir(path, length);
    const size_t slash = dir.find_last_of(L"\\/");
    return (slash == std::wstring::npos ? std::wstring(L".") : dir.substr(0, slash)) + L"\\PBZPlugins";
}

PbzResult Attach(PbzPluginMainFn entry, const wchar_t* name) {
    if (!entry) return PBZ_E_ARG;
    std::wstring label = name && *name ? std::wstring(name, wcsnlen(name, 120)) : std::wstring(L"(attached)");
    label = L"[外部] " + label;
    std::lock_guard<std::mutex> hold(lock_);
    if (attached_.size() + DllCount() >= kMaxPlugins) return PBZ_E_FULL;
    attached_.push_back({entry, label});
    Log("PLUGIN %s attached from outside PBZPlugins: starts once 派生插件 is on", Utf8(label).c_str());
    return PBZ_OK;
}
const PbzApi* Api() { if (!api_.size) BuildApi(); return &api_; }
PbzResult AddForTest(PbzPluginMainFn entry, const wchar_t* name) {
    if (!api_.size) BuildApi();
    game_thread_ = GetCurrentThreadId(); loaded_ = true;
    auto record = std::make_unique<PbzPlugin>(); PbzPlugin* p = record.get(); p->file = name ? name : L"test";
    { std::lock_guard<std::mutex> hold(lock_); plugins_.push_back(std::move(record)); }
    Start(p, entry);
    return p->ready ? PBZ_OK : PBZ_E_STATE;
}
void ResetForTest() {
    if (script_host_) script_host_->unload();  // the Lua states hold PbzPlugin pointers: closed before the records go
    scripts_loaded_ = false; reload_scripts_ = false;
    std::lock_guard<std::mutex> hold(lock_);
    plugins_.clear(); signals_.clear(); actions_.clear(); ops_.clear(); last_answer_.clear(); attached_.clear();
    private_events_.clear(); denied_logged_.clear(); ui_shown_ms_ = 0;
    events_.clear(); decided_.clear(); listening_ = false; wants_abilities_ = false; loaded_ = false; last_on_ = -1; enabled_ = -1; Bump();
    PluginUi::ResetForTest(); ModRegistry::SetListening(false);
}
}  // namespace ComboPlugins
