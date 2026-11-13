#include "mod_registry.h"
#include "plugin_text.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>

namespace ModRegistry {
namespace {
using PluginText::Cell;

constexpr size_t kMaxModules = 64, kMaxMembers = 128, kMaxCommands = 256, kMaxEvents = 512, kMaxWorker = 64, kEventText = 200, kTextValue = 512;

struct Entry {
    Module module;
    std::atomic<bool> faulted{false};
};
std::mutex lock_;                                  // the tables and the queues; never held while a module function runs
std::vector<std::shared_ptr<Entry>> modules_;
std::vector<Command> commands_;
std::deque<WorkerJob> worker_;
struct Poller { std::string module; std::function<void()> poll; bool dead = false; };
std::vector<Poller> pollers_;
std::mutex events_lock_;
std::vector<Emitted> events_;
std::atomic<bool> listening_{false};
std::atomic<uint64_t> serial_{1};
LogSink log_ = nullptr;

void Log(const char* format, ...) {
    if (!log_) return;
    char line[800]{}; va_list args; va_start(args, format); _vsnprintf_s(line, sizeof(line), _TRUNCATE, format, args); va_end(args);
    log_(line);
}
void Bump() { serial_.fetch_add(1); }

bool Ident(const std::string& text, size_t most = 40, bool upper = true) {
    if (text.empty() || text.size() > most) return false;
    for (const unsigned char c : text) if (!(std::islower(c) || std::isdigit(c) || c == '_' || (upper && std::isupper(c)))) return false;
    return true;
}
bool CommandName(const std::string& text) {
    if (text.size() < 3 || text.size() > 48 || text.find('.') == std::string::npos || text.front() == '.' || text.back() == '.') return false;
    for (const unsigned char c : text) if (!(std::islower(c) || std::isdigit(c) || c == '_' || c == '.')) return false;
    return true;
}

// ---- guarded calls ------------------------------------------------------------------------------------------------
// A structured exception in a module's function (an access violation, a C++ exception thrown through) ends the call and
// switches the module off for the session.
bool Guarded(const std::function<void()>& body, DWORD& code) {
    __try { body(); return true; }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool Run(const std::shared_ptr<Entry>& entry, const char* what, const std::function<void()>& body) {
    DWORD code = 0;
    if (Guarded(body, code)) return true;
    if (entry) entry->faulted = true;
    Log("MODULE [%s] fault 0x%08lx in %s: switched off for this session", entry ? entry->module.name.c_str() : "?", code, what);
    return false;
}

std::shared_ptr<Entry> FindModule(const std::string& name) {
    std::lock_guard<std::mutex> hold(lock_);
    for (const auto& e : modules_) if (e->module.name == name) return e;
    return nullptr;
}
const Prop* FindIn(const Module& m, const std::string& name) {
    for (const Prop& p : m.props) if (p.name == name) return &p;
    return nullptr;
}

// ---- value text ---------------------------------------------------------------------------------------------------
std::string Format(Type type, double v) {
    char text[64]{};
    switch (type) {
    case Type::Bool: return v != 0 ? "1" : "0";
    case Type::Int: case Type::Enum: _snprintf_s(text, sizeof(text), _TRUNCATE, "%lld", static_cast<long long>(std::llround(v))); return text;
    default: _snprintf_s(text, sizeof(text), _TRUNCATE, "%.9g", v); return text;
    }
}
std::string Limit(Type type, double v) {  // a bound in the property table
    if (type == Type::Text) return "";
    if (type == Type::Float) { char text[64]{}; _snprintf_s(text, sizeof(text), _TRUNCATE, "%g", v); return text; }
    return Format(type, v);
}
bool ParseNumber(const std::string& text, bool integer, double& out) {
    if (text.empty() || text.size() > 40) return false;
    char* end = nullptr;
    if (integer) { const long long v = std::strtoll(text.c_str(), &end, 10); if (!end || *end) return false; out = static_cast<double>(v); return true; }
    const double v = std::strtod(text.c_str(), &end);
    if (!end || *end || !std::isfinite(v)) return false;
    out = v; return true;
}
bool SameName(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}
// Parses `value` for the property: its number (Bool 0 / 1, Int, Float, Enum index) or, for Text, nothing (the text itself).
PbzResult Parse(const Prop& p, const std::string& value, double& number) {
    switch (p.type) {
    case Type::Bool:
        if (value == "1" || value == "true" || value == "on") { number = 1; return PBZ_OK; }
        if (value == "0" || value == "false" || value == "off") { number = 0; return PBZ_OK; }
        return PBZ_E_ARG;
    case Type::Int: if (!ParseNumber(value, true, number)) return PBZ_E_ARG; break;
    case Type::Float: if (!ParseNumber(value, false, number)) return PBZ_E_ARG; break;
    case Type::Enum:
        if (ParseNumber(value, true, number)) break;
        for (size_t i = 0; i < p.options.size(); ++i) if (SameName(p.options[i], value)) { number = static_cast<double>(i); return PBZ_OK; }
        return PBZ_E_ARG;
    case Type::Text:
        if (value.size() > kTextValue) return PBZ_E_ARG;
        for (const unsigned char c : value) if (c < 0x20 && c != '\t') return PBZ_E_ARG;
        return PBZ_OK;
    }
    return number >= p.min && number <= p.max ? PBZ_OK : PBZ_E_ARG;
}

std::string Joined(const std::vector<std::string>& list) {
    std::string out; for (const std::string& s : list) out += (out.empty() ? "" : ",") + s; return out;
}
}  // namespace

const char* ThreadName(Thread thread) { return thread == Thread::Game ? "game" : thread == Thread::Any ? "any" : "worker"; }
const char* TypeName(Type type) {
    switch (type) { case Type::Bool: return "bool"; case Type::Int: return "int"; case Type::Float: return "float"; case Type::Enum: return "enum"; default: return "text"; }
}
const char* KindName(Kind kind) { return kind == Kind::Diagnostic ? "diagnostic" : kind == Kind::State ? "state" : "persist"; }

// ---- registration -------------------------------------------------------------------------------------------------
bool Register(Module module) {
    if (!Ident(module.name, 32, false)) { Log("MODULE register refused: bad name \"%s\"", module.name.c_str()); return false; }
    if (module.props.size() > kMaxMembers || module.actions.size() > kMaxMembers || module.events.size() > kMaxMembers) { Log("MODULE [%s] refused: too many members", module.name.c_str()); return false; }
    for (size_t i = 0; i < module.props.size(); ++i) {
        Prop& p = module.props[i];
        bool ok = Ident(p.name);
        for (size_t j = 0; j < i; ++j) ok = ok && module.props[j].name != p.name;
        if (p.type == Type::Text) ok = ok && p.get_text && (p.read_only || p.set_text);
        else ok = ok && p.get_num && (p.read_only || p.set_num) && p.min <= p.max;
        if (p.type == Type::Enum) ok = ok && !p.options.empty() && p.options.size() <= 64;
        if (!ok) { Log("MODULE [%s] refused: property \"%s\" is malformed", module.name.c_str(), p.name.c_str()); return false; }
        if (p.type == Type::Bool) { p.min = 0; p.max = 1; }
        if (p.type == Type::Enum) { p.min = 0; p.max = static_cast<double>(p.options.size()) - 1; }
        if (p.type != Type::Text) p.def = std::clamp(p.def, p.min, p.max);
    }
    for (size_t i = 0; i < module.actions.size(); ++i) {
        bool ok = Ident(module.actions[i].name) && module.actions[i].run;
        for (size_t j = 0; j < i; ++j) ok = ok && module.actions[j].name != module.actions[i].name;
        if (!ok) { Log("MODULE [%s] refused: action \"%s\" is malformed", module.name.c_str(), module.actions[i].name.c_str()); return false; }
    }
    for (size_t i = 0; i < module.events.size(); ++i) {
        bool ok = Ident(module.events[i].name);
        for (size_t j = 0; j < i; ++j) ok = ok && module.events[j].name != module.events[i].name;
        if (!ok) { Log("MODULE [%s] refused: event \"%s\" is malformed", module.name.c_str(), module.events[i].name.c_str()); return false; }
    }
    auto entry = std::make_shared<Entry>(); entry->module = std::move(module);
    {
        std::lock_guard<std::mutex> hold(lock_);
        if (modules_.size() >= kMaxModules) return false;
        for (const auto& e : modules_) if (e->module.name == entry->module.name) { Log("MODULE [%s] refused: registered already", entry->module.name.c_str()); return false; }
        modules_.push_back(entry);
    }
    Bump();
    return true;
}
bool Unregister(const std::string& name) {
    std::lock_guard<std::mutex> hold(lock_);
    const auto it = std::find_if(modules_.begin(), modules_.end(), [&](const auto& e) { return e->module.name == name; });
    if (it == modules_.end()) return false;
    modules_.erase(it); Bump(); return true;
}
bool Has(const std::string& module) { return FindModule(module) != nullptr; }
uint64_t Serial() { return serial_.load(); }

// ---- reading the tables -------------------------------------------------------------------------------------------
std::string ModuleList() {
    std::vector<std::shared_ptr<Entry>> all; { std::lock_guard<std::mutex> hold(lock_); all = modules_; }
    std::sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a->module.name < b->module.name; });
    std::string out;
    for (const auto& e : all) {
        const Module& m = e->module;
        out += (out.empty() ? "" : "\n") + m.name + "\t" + Cell(m.label) + "\t" + ThreadName(m.thread) + "\t" + std::to_string(m.props.size()) + "\t" +
               std::to_string(m.actions.size()) + "\t" + std::to_string(m.events.size()) + "\t" + Cell(m.description);
    }
    return out;
}
PbzResult PropList(const std::string& module, std::string& out) {
    const auto e = FindModule(module); if (!e) return PBZ_E_NOT_FOUND;
    out.clear();
    for (const Prop& p : e->module.props)
        out += (out.empty() ? "" : "\n") + p.name + "\t" + TypeName(p.type) + "\t" + (p.read_only ? "ro" : "rw") + "\t" + Limit(p.type, p.min) + "\t" + Limit(p.type, p.max) + "\t" +
               (p.type == Type::Text ? std::string() : Format(p.type, p.def)) + "\t" + Cell(p.ini) + "\t" + ThreadName(e->module.thread) + "\t" + Cell(p.label) + "\t" + Cell(Joined(p.options)) + "\t" + Cell(p.note);
    return PBZ_OK;
}
PbzResult ActionList(const std::string& module, std::string& out) {
    const auto e = FindModule(module); if (!e) return PBZ_E_NOT_FOUND;
    out.clear();
    for (const Action& a : e->module.actions)
        out += (out.empty() ? "" : "\n") + a.name + "\t" + ThreadName(e->module.thread) + "\t" + (a.persistent ? "1" : "0") + "\t" + Cell(a.args) + "\t" + Cell(a.label) + "\t" + Cell(a.note);
    return PBZ_OK;
}
PbzResult EventList(const std::string& module, std::string& out) {
    const auto e = FindModule(module); if (!e) return PBZ_E_NOT_FOUND;
    out.clear();
    for (const Event& v : e->module.events) out += (out.empty() ? "" : "\n") + v.name + "\t" + Cell(v.label) + "\t" + Cell(v.format);
    return PBZ_OK;
}
PbzResult FindProp(const std::string& module, const std::string& prop, PropInfo& out) {
    const auto e = FindModule(module); if (!e) return PBZ_E_NOT_FOUND;
    const Prop* p = FindIn(e->module, prop); if (!p) return PBZ_E_NOT_FOUND;
    out.type = p->type; out.read_only = p->read_only; out.min = p->min; out.max = p->max; out.thread = e->module.thread; out.options = p->options;
    return PBZ_OK;
}
PbzResult FindAction(const std::string& module, const std::string& action) {
    const auto e = FindModule(module); if (!e) return PBZ_E_NOT_FOUND;
    for (const Action& a : e->module.actions) if (a.name == action) return PBZ_OK;
    return PBZ_E_NOT_FOUND;
}
bool HasEvent(const std::string& module, const std::string& event) {
    const auto e = FindModule(module); if (!e) return false;
    for (const Event& v : e->module.events) if (v.name == event) return true;
    return false;
}

// ---- properties and actions ---------------------------------------------------------------------------------------
PbzResult Get(const std::string& module, const std::string& prop, std::string& out, bool game_thread) {
    const auto e = FindModule(module); if (!e) return PBZ_E_NOT_FOUND;
    const Prop* p = FindIn(e->module, prop); if (!p) return PBZ_E_NOT_FOUND;
    if (e->module.thread == Thread::Game && !game_thread) return PBZ_E_THREAD;
    if (e->faulted) return PBZ_E_STATE;
    bool ok = false; std::string text; double v = 0;
    if (!Run(e, ("get " + prop).c_str(), [&] { ok = p->type == Type::Text ? (p->get_text && p->get_text(text)) : (p->get_num && p->get_num(v)); })) return PBZ_E_STATE;
    if (!ok) return PBZ_E_STATE;
    out = p->type == Type::Text ? text.substr(0, kTextValue) : Format(p->type, v);
    return PBZ_OK;
}
PbzResult Check(const std::string& module, const std::string& prop, const std::string& value, std::string& canonical) {
    const auto e = FindModule(module); if (!e) return PBZ_E_NOT_FOUND;
    const Prop* p = FindIn(e->module, prop); if (!p) return PBZ_E_NOT_FOUND;
    if (p->read_only) return PBZ_E_ARG;
    double number = 0;
    const PbzResult r = Parse(*p, value, number);
    if (r != PBZ_OK) return r;
    canonical = p->type == Type::Text ? value : Format(p->type, number);
    return PBZ_OK;
}
PbzResult Apply(const std::string& module, const std::string& prop, const std::string& canonical, std::string& why) {
    const auto e = FindModule(module); if (!e) { why = "no such module"; return PBZ_E_NOT_FOUND; }
    const Prop* p = FindIn(e->module, prop); if (!p) { why = "no such property"; return PBZ_E_NOT_FOUND; }
    double number = 0;
    if (p->read_only || Parse(*p, canonical, number) != PBZ_OK) { why = "value refused"; return PBZ_E_ARG; }
    if (e->faulted) { why = "module switched off"; return PBZ_E_STATE; }
    bool ok = false;
    if (!Run(e, ("set " + prop).c_str(), [&] { ok = p->type == Type::Text ? p->set_text(canonical) : p->set_num(number); })) { why = "module faulted"; return PBZ_E_STATE; }
    if (!ok) { why = "module could not do it now"; return PBZ_E_STATE; }
    return PBZ_OK;
}
PbzResult Invoke(const std::string& module, const std::string& action, const std::string& arg, std::string& out) {
    const auto e = FindModule(module); if (!e) return PBZ_E_NOT_FOUND;
    const Action* a = nullptr; for (const Action& x : e->module.actions) if (x.name == action) a = &x;
    if (!a) return PBZ_E_NOT_FOUND;
    if (e->faulted) { out = "module switched off"; return PBZ_E_STATE; }
    bool ok = false; out.clear();
    if (!Run(e, ("action " + action).c_str(), [&] { ok = a->run(arg, out); })) { out = "module faulted"; return PBZ_E_STATE; }
    if (out.size() > 4000) out.resize(4000);
    return ok ? PBZ_OK : PBZ_E_STATE;
}

// ---- events -------------------------------------------------------------------------------------------------------
void Emit(const std::string& module, const std::string& event, const std::string& text) {
    if (!listening_) return;
    Emitted e; e.module = module; e.event = event; e.text = text.substr(0, kEventText); e.ms = GetTickCount64();
    std::lock_guard<std::mutex> hold(events_lock_);
    if (events_.size() < kMaxEvents) events_.push_back(std::move(e));
}
void SetListening(bool on) { listening_ = on; }
bool Listening() { return listening_; }
std::vector<Emitted> Drain() {
    std::vector<Emitted> out;
    std::lock_guard<std::mutex> hold(events_lock_);
    out.swap(events_);
    return out;
}

bool AddPoller(const std::string& module, std::function<void()> poll) {
    if (!poll || !Has(module)) return false;
    std::lock_guard<std::mutex> hold(lock_);
    if (pollers_.size() >= kMaxModules) return false;
    pollers_.push_back({module, std::move(poll), false});
    return true;
}
void RunPollers() {
    if (!listening_) return;
    std::vector<std::pair<size_t, std::function<void()>>> run;
    { std::lock_guard<std::mutex> hold(lock_); for (size_t i = 0; i < pollers_.size(); ++i) if (!pollers_[i].dead) run.emplace_back(i, pollers_[i].poll); }
    for (auto& [index, poll] : run) {
        DWORD code = 0;
        if (Guarded(poll, code)) continue;
        std::string module;
        { std::lock_guard<std::mutex> hold(lock_); if (index < pollers_.size()) { pollers_[index].dead = true; module = pollers_[index].module; } }
        Log("MODULE [%s] event poller fault 0x%08lx: its events stop for this session", module.c_str(), code);
    }
}

// ---- the command bus ----------------------------------------------------------------------------------------------
bool RegisterCommand(Command command) {
    if (!CommandName(command.name) || !command.run) return false;
    std::lock_guard<std::mutex> hold(lock_);
    if (commands_.size() >= kMaxCommands) return false;
    for (const Command& c : commands_) if (c.name == command.name) return false;
    commands_.push_back(std::move(command)); Bump();
    return true;
}
std::string CommandList() {
    std::vector<Command> all; { std::lock_guard<std::mutex> hold(lock_); all = commands_; }
    std::sort(all.begin(), all.end(), [](const Command& a, const Command& b) { return a.name < b.name; });
    std::string out;
    for (const Command& c : all) out += (out.empty() ? "" : "\n") + c.name + "\t" + ThreadName(c.thread) + "\t" + KindName(c.kind) + "\t" + Cell(c.args) + "\t" + Cell(c.note);
    return out;
}
bool FindCommand(const std::string& name, Command& out) {
    std::lock_guard<std::mutex> hold(lock_);
    for (const Command& c : commands_) if (c.name == name) { out = c; return true; }
    return false;
}
PbzResult RunCommand(const std::string& name, const std::string& args, std::string& out) {
    Command c; if (!FindCommand(name, c)) return PBZ_E_NOT_FOUND;
    bool ok = false; out.clear();
    DWORD code = 0;
    if (!Guarded([&] { ok = c.run(args, out); }, code)) { Log("COMMAND %s fault 0x%08lx", name.c_str(), code); out = "command faulted"; return PBZ_E_STATE; }
    if (out.size() > 4000) out.resize(4000);
    return ok ? PBZ_OK : PBZ_E_STATE;
}
bool PostWorker(WorkerJob job) {
    std::lock_guard<std::mutex> hold(lock_);
    if (worker_.size() >= kMaxWorker) return false;
    worker_.push_back(std::move(job)); return true;
}
size_t PumpWorker(bool (*allow)(const WorkerJob&), void (*done)(const std::string&, bool, const std::string&, const std::string&)) {
    std::deque<WorkerJob> jobs;
    { std::lock_guard<std::mutex> hold(lock_); if (worker_.empty()) return 0; jobs.swap(worker_); }
    for (const WorkerJob& job : jobs) {
        std::string out; bool ok = false;
        if (allow && !allow(job)) out = "denied";
        else ok = RunCommand(job.name, job.args, out) == PBZ_OK;
        if (done) done(job.owner, ok, job.line, out);
    }
    return jobs.size();
}

void SetLog(LogSink sink) { log_ = sink; }
void ResetForTest() {
    { std::lock_guard<std::mutex> hold(lock_); modules_.clear(); commands_.clear(); worker_.clear(); pollers_.clear(); }
    { std::lock_guard<std::mutex> hold(events_lock_); events_.clear(); }
    listening_ = false; Bump();
}

// ---- builder ------------------------------------------------------------------------------------------------------
ModuleBuilder::ModuleBuilder(std::string name, std::string label, std::string description, Thread thread) {
    module_.name = std::move(name); module_.label = std::move(label); module_.description = std::move(description); module_.thread = thread;
}
Prop ModuleBuilder::Make(const char* name, const char* label, Type type, double min, double max, const char* ini, const char* note) {
    Prop p; p.name = name ? name : ""; p.label = label ? label : ""; p.type = type; p.min = min; p.max = max; p.ini = ini ? ini : ""; p.note = note ? note : "";
    return p;
}
ModuleBuilder& ModuleBuilder::Action(const char* name, const char* label, const char* args, bool persistent, std::function<bool(const std::string&, std::string&)> run) {
    ModRegistry::Action a; a.name = name ? name : ""; a.label = label ? label : ""; a.args = args ? args : ""; a.persistent = persistent; a.run = std::move(run);
    module_.actions.push_back(std::move(a)); return *this;
}
ModuleBuilder& ModuleBuilder::Event(const char* name, const char* label, const char* format) {
    ModRegistry::Event e; e.name = name ? name : ""; e.label = label ? label : ""; e.format = format ? format : "";
    module_.events.push_back(std::move(e)); return *this;
}
}  // namespace ModRegistry
