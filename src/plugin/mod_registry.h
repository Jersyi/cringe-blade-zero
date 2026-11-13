#pragma once
// 模块注册表 (module registry): the reflective surface the plugin API (and nothing else yet) uses to read and change the
// MOD's modules. A module describes itself once, in its own file, as a table of
//   - properties: a typed value (bool / int / float / enum / text) with a range, a default, where the module itself keeps it
//     in PBZDebugMenu.ini, and a getter and setter;
//   - actions: a named verb with a text argument and a text result;
//   - events: names the module raises (Emit) for subscribers.
// and registers it (Register / ModuleBuilder). The registry owns no state of the module: every getter, setter and action
// is the module's own function, so the module keeps its own threading rules, and says them in `Thread`.
//
// Threads (the table is what plugins are told, see PLUGIN_API.md):
//   Game    the module's state lives on the game thread: its functions are only called there (a read from another thread
//           is refused with PBZ_E_THREAD).
//   Any     the functions are thread safe (atomics or the module's own lock): reads may come from any thread.
//   Worker  the state belongs to the panel / Worker thread, and the functions only post a request or read a snapshot
//           (the Request / Snapshot boundary the pages already use); an effect can come a panel cycle later. Reads: any thread.
// Writes (property sets, actions from other threads) never run on the caller's thread: the plugin host queues them and
// calls the module's setter on the game thread, so a setter written for "the game thread calls me" is always right.
// Every call into a module is behind a structured-exception guard: a module that faults is switched off for the session.
//
// Registration may happen on any thread, at any time; the tables are copied.
#include "pbz_combo_api.h"
#include <cstdint>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace ModRegistry {
enum class Type { Bool, Int, Float, Enum, Text };
enum class Thread { Game, Any, Worker };
const char* ThreadName(Thread thread);  // "game" / "any" / "worker"
const char* TypeName(Type type);        // "bool" / "int" / "float" / "enum" / "text"

struct Prop {
    std::string name, label, note;
    Type type = Type::Int;
    bool read_only = false;
    double min = 0, max = 1, def = 0;          // Enum: 0 .. options-1
    std::vector<std::string> options;          // Enum: the names, the value is the index
    std::string ini;                           // "Section.Key": where the module itself keeps the value ("" : not kept)
    std::function<bool(double&)> get_num;      // Bool / Int / Float / Enum
    std::function<bool(double)> set_num;       // false: the module could not do it now
    std::function<bool(std::string&)> get_text;      // Text
    std::function<bool(const std::string&)> set_text;
};
struct Action {
    std::string name, label, args, note;       // args: what the text argument is ("" : none)
    bool persistent = false;                   // writes ini or files (false: only changes the current state)
    std::function<bool(const std::string& arg, std::string& out)> run;  // false: failed, `out` is why
};
struct Event {
    std::string name, label, format;           // format: what the event's text holds
};
struct Module {
    std::string name, label, description;
    Thread thread = Thread::Game;
    std::vector<Prop> props;
    std::vector<Action> actions;
    std::vector<Event> events;
};

// Names: modules are lowercase letters, digits and '_' (1..32 bytes); properties, actions and events letters, digits and '_'
// (1..40). Limits: 64 modules, 128 of each kind per module. False: refused (a bad name, a duplicate, a limit, a prop without
// its accessors, an enum without options).
bool Register(Module module);
bool Unregister(const std::string& name);
bool Has(const std::string& module);
uint64_t Serial();  // changes whenever the tables do

// ---- reading the tables -------------------------------------------------------------------------------------------
// TSV (one item per line, columns tab separated; see pbz_combo_api.h for the columns).
std::string ModuleList();
PbzResult PropList(const std::string& module, std::string& out);
PbzResult ActionList(const std::string& module, std::string& out);
PbzResult EventList(const std::string& module, std::string& out);
struct PropInfo { Type type = Type::Int; bool read_only = false; double min = 0, max = 1; Thread thread = Thread::Game; std::vector<std::string> options; };
PbzResult FindProp(const std::string& module, const std::string& prop, PropInfo& out);
PbzResult FindAction(const std::string& module, const std::string& action);
bool HasEvent(const std::string& module, const std::string& event);

// ---- properties ---------------------------------------------------------------------------------------------------
// The value as text (bool 0/1, int, float, enum index, text). `game_thread`: the caller is on the game thread; a Game
// module read from elsewhere is PBZ_E_THREAD. A module that faulted is PBZ_E_STATE.
PbzResult Get(const std::string& module, const std::string& prop, std::string& out, bool game_thread);
// Checks `value` against the property (type, range, enum index or name, text length) without changing anything;
// `canonical` is the text Apply takes. PBZ_E_NOT_FOUND / PBZ_E_ARG (also for a read-only property).
PbzResult Check(const std::string& module, const std::string& prop, const std::string& value, std::string& canonical);
// Calls the setter with a canonical value (game thread; the plugin host calls it from its frame). PBZ_E_STATE: the module
// refused or faulted (`why`).
PbzResult Apply(const std::string& module, const std::string& prop, const std::string& canonical, std::string& why);
// Runs an action (game thread). PBZ_E_STATE: it failed (`out` is why) or its module faulted.
PbzResult Invoke(const std::string& module, const std::string& action, const std::string& arg, std::string& out);

// ---- events -------------------------------------------------------------------------------------------------------
// A module raises an event (any thread; text up to 200 bytes). Kept only while someone listens (the plugin host says so).
void Emit(const std::string& module, const std::string& event, const std::string& text = std::string());
struct Emitted { std::string module, event, text; uint64_t ms = 0; };
void SetListening(bool on);
bool Listening();
std::vector<Emitted> Drain();  // the plugin host, once a frame
// A module with no event hook of its own raises its events from a poller: a function the plugin host calls once a frame on the game
// thread, only while a plugin listens, that compares the module's snapshot with the last one it saw and Emits what changed. It
// runs behind the same guard as every module function (a poller that faults is dropped). Its module must be registered.
bool AddPoller(const std::string& module, std::function<void()> poll);
void RunPollers();  // the plugin host, game thread, once a frame

// ---- the command bus ----------------------------------------------------------------------------------------------
// Whitelisted commands ("hair.status", "bones.list"): a module adds the ones it knows are safe to run on a plugin's word.
//   Diagnostic  only prints or writes the log; changes nothing that outlives the session
//   State       changes the current state (not saved)
//   Persist     writes PBZDebugMenu.ini or files (a plugin needs PBZ_CAP_INI as well)
enum class Kind { Diagnostic, State, Persist };
const char* KindName(Kind kind);
struct Command {
    std::string name, args, note;              // name: "module.verb", lowercase letters, digits, '_', '.' (3..48 bytes)
    Thread thread = Thread::Game;              // Worker: runs when the panel thread pumps (PumpWorker)
    Kind kind = Kind::Diagnostic;
    std::function<bool(const std::string& args, std::string& out)> run;  // false: failed, `out` is why
};
bool RegisterCommand(Command command);       // false: a bad name, a duplicate, no function
std::string CommandList();                    // TSV
bool FindCommand(const std::string& name, Command& out);
// Runs the command now, on this thread, behind the guard. PBZ_E_NOT_FOUND / PBZ_E_STATE (it failed: `out` is why).
PbzResult RunCommand(const std::string& name, const std::string& args, std::string& out);
// Worker commands wait here for the panel thread (PumpWorker, once per panel cycle; proxy.cpp). `allow` asks whether a job may still
// run (its owner may have lost the permission); `done` gets every finished job (owner, ok, the line, the output).
struct WorkerJob { std::string owner, name, args, line; };
bool PostWorker(WorkerJob job);               // any thread; false when 64 are waiting
size_t PumpWorker(bool (*allow)(const WorkerJob& job), void (*done)(const std::string& owner, bool ok, const std::string& line, const std::string& out));

// ---- logging and tests --------------------------------------------------------------------------------------------
using LogSink = void (*)(const char* utf8);
void SetLog(LogSink sink);
void ResetForTest();

// ---- a terse way to write a table ---------------------------------------------------------------------------------
// ModuleBuilder("npc_body", "身材 / 骨骼", "...", Thread::Worker)
//     .Bool("enabled", "启用", [] { return Enabled(); }, [](bool v) { SetEnabled(v); }, "NpcBody.Enabled")
//     .Int("slots", "槽位", 1, 8, 4, getter, setter)
//     .Action("restore", "还原", "", false, [](const std::string&, std::string& out) { return Restore(out); })
//     .Event("applied", "外观套用完成", "外观 id")
//     .Commit();
// Setters may return void or bool (false: could not do it now).
class ModuleBuilder {
public:
    ModuleBuilder(std::string name, std::string label, std::string description, Thread thread);
    template <class G, class S> ModuleBuilder& Bool(const char* name, const char* label, G get, S set, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Bool, 0, 1, ini, note);
        p.get_num = [get](double& v) { v = get() ? 1 : 0; return true; };
        p.set_num = Wrap<S, bool>(set, [](double v) { return v != 0; });
        return Add(std::move(p));
    }
    template <class G, class S> ModuleBuilder& Int(const char* name, const char* label, int min, int max, int def, G get, S set, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Int, min, max, ini, note); p.def = def;
        p.get_num = [get](double& v) { v = static_cast<double>(get()); return true; };
        p.set_num = Wrap<S, int>(set, [](double v) { return static_cast<int>(v < 0 ? v - 0.5 : v + 0.5); });
        return Add(std::move(p));
    }
    template <class G, class S> ModuleBuilder& Float(const char* name, const char* label, double min, double max, double def, G get, S set, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Float, min, max, ini, note); p.def = def;
        p.get_num = [get](double& v) { v = static_cast<double>(get()); return true; };
        p.set_num = Wrap<S, float>(set, [](double v) { return static_cast<float>(v); });
        return Add(std::move(p));
    }
    template <class G, class S> ModuleBuilder& Enum(const char* name, const char* label, std::vector<std::string> options, int def, G get, S set, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Enum, 0, options.empty() ? 0 : static_cast<double>(options.size()) - 1, ini, note); p.def = def; p.options = std::move(options);
        p.get_num = [get](double& v) { v = static_cast<double>(get()); return true; };
        p.set_num = Wrap<S, int>(set, [](double v) { return static_cast<int>(v); });
        return Add(std::move(p));
    }
    template <class G, class S> ModuleBuilder& Text(const char* name, const char* label, G get, S set, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Text, 0, 0, ini, note);
        p.get_text = [get](std::string& v) { v = get(); return true; };
        p.set_text = [set](const std::string& v) { return Call1(set, v); };
        return Add(std::move(p));
    }
    // A read-only property (no setter): the type is as for the writable ones.
    template <class G> ModuleBuilder& ReadOnlyBool(const char* name, const char* label, G get, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Bool, 0, 1, ini, note); p.read_only = true;
        p.get_num = [get](double& v) { v = get() ? 1 : 0; return true; };
        return Add(std::move(p));
    }
    template <class G> ModuleBuilder& ReadOnlyInt(const char* name, const char* label, G get, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Int, -2147483648.0, 2147483647.0, ini, note); p.read_only = true;
        p.get_num = [get](double& v) { v = static_cast<double>(get()); return true; };
        return Add(std::move(p));
    }
    template <class G> ModuleBuilder& ReadOnlyFloat(const char* name, const char* label, G get, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Float, -1e30, 1e30, ini, note); p.read_only = true;
        p.get_num = [get](double& v) { v = static_cast<double>(get()); return true; };
        return Add(std::move(p));
    }
    template <class G> ModuleBuilder& ReadOnlyText(const char* name, const char* label, G get, const char* ini = "", const char* note = "") {
        Prop p = Make(name, label, Type::Text, 0, 0, ini, note); p.read_only = true;
        p.get_text = [get](std::string& v) { v = get(); return true; };
        return Add(std::move(p));
    }
    ModuleBuilder& Action(const char* name, const char* label, const char* args, bool persistent, std::function<bool(const std::string&, std::string&)> run);
    ModuleBuilder& Event(const char* name, const char* label, const char* format = "");
    bool Commit() { return Register(std::move(module_)); }
    Module& Table() { return module_; }

private:
    // A setter that returns void always succeeds; one that returns bool says whether it could.
    template <class S, class V> static bool Call1(const S& set, const V& v) {
        if constexpr (std::is_void_v<std::invoke_result_t<const S&, const V&>>) { set(v); return true; }
        else return static_cast<bool>(set(v));
    }
    template <class S, class V, class Conv> static std::function<bool(double)> Wrap(S set, Conv conv) {
        return [set, conv](double v) { const V value = static_cast<V>(conv(v)); return Call1(set, value); };
    }
    static Prop Make(const char* name, const char* label, Type type, double min, double max, const char* ini, const char* note);
    ModuleBuilder& Add(Prop prop) { module_.props.push_back(std::move(prop)); return *this; }
    Module module_;
};
}  // namespace ModRegistry
