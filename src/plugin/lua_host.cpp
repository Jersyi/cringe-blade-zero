#include "lua_host.h"
#include "combo_plugins.h"
#include "mod_registry.h"
#include "npc_combo_model.h"
#include "plugin_text.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
// Lua is built as C++ (CMakeLists.txt, PBZ_LUA_SOURCES): its headers are included as they are, without extern "C".
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"

// The UE4SS fork's copy of Lua routes lua_lock / lua_unlock (luaconf.h "Local configuration") to three functions its luauser.c defines
// with a global critical section. Scripts run on the game thread only, one Lua state each, so nothing needs locking: they do nothing.
void LuaLock(lua_State*) {}
void LuaUnlock(lua_State*) {}
void LuaLockInitial(lua_State*) {}

namespace LuaHost {
namespace {
using PluginText::Utf8;
using PluginText::Wide;

constexpr size_t kMemoryCap = 16u * 1024u * 1024u;   // per script (Lua's own allocations)
constexpr long kCallBudget = 200000;                  // VM instructions one callback may run (about a millisecond or two)
constexpr long kLoadBudget = 5000000;                 // ... and the script's top level while it loads
constexpr int kHookCount = 1000;                      // the budget is checked every this many instructions
constexpr size_t kMaxFileBytes = 1u << 20, kMaxFiles = 64;

struct Script;
struct ActionCtx { Script* owner = nullptr; std::string id; int ref = LUA_NOREF; };
struct Script {
    std::string id, stem;                       // "lua.<stem>"
    std::wstring file, path;
    lua_State* L = nullptr;
    PbzPlugin* self = nullptr; const PbzApi* api = nullptr;
    size_t used = 0;                            // bytes Lua has now
    long budget = 0; int depth = 0;
    bool exceeded = false, dead = false, loading = false, ok = false;
    ULONGLONG log_window = 0, notify_window = 0; int log_lines = 0, notify_lines = 0;   // a script may write 200 log lines and show 4 notices a second
    int on_event = LUA_NOREF, on_hero_ability = LUA_NOREF, on_tick = LUA_NOREF, on_enabled = LUA_NOREF, on_module_event = LUA_NOREF, on_ui = LUA_NOREF, on_caps = LUA_NOREF, on_start = LUA_NOREF;
    std::vector<std::unique_ptr<ActionCtx>> actions;
};
std::vector<std::unique_ptr<Script>> g_scripts;
Script* g_loading = nullptr;                    // the script whose entry runs now (the entry function has no user pointer)
DWORD g_game_thread = 0;                        // the thread Load ran on (the plugin host's frame): the only one Lua is called on

Script* Self(lua_State* L) { return *static_cast<Script**>(lua_getextraspace(L)); }
// A script's log lines and notices are rate limited (a loop that prints every tick must not fill the log): more than 200 lines or 4 notices
// in a second are dropped, with one line saying so.
bool Allowed(ULONGLONG& window, int& count, int most, Script& s, const char* what) {
    const ULONGLONG now = GetTickCount64();
    if (now - window >= 1000) { window = now; count = 0; }
    if (++count <= most) return true;
    if (count == most + 1 && s.api && s.self) s.api->log(s.self, (std::string("rate limit: more than ") + std::to_string(most) + " " + what + " a second are dropped").c_str());
    return false;
}
void Say(Script& s, const std::string& text) {
    if (s.api && s.self && Allowed(s.log_window, s.log_lines, 200, s, "log lines")) s.api->log(s.self, text.substr(0, 700).c_str());
}

// ---- the interpreter's allocator, hook and sandbox -----------------------------------------------------------------------
void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Script* s = static_cast<Script*>(ud);
    if (!ptr) osize = 0;
    if (nsize == 0) { if (ptr) { s->used = s->used >= osize ? s->used - osize : 0; std::free(ptr); } return nullptr; }
    if (nsize > osize && s->used + (nsize - osize) > kMemoryCap) return nullptr;  // "not enough memory" in the script
    void* grown = std::realloc(ptr, nsize);
    if (!grown) return nullptr;
    s->used = s->used - osize + nsize;
    return grown;
}
int Panic(lua_State* L) {  // an error outside every protected call: nothing should get here (the libraries open under pcall)
    const char* message = lua_tostring(L, -1);
    ComboPlugins::LogHost(std::string("lua panic: ") + (message ? message : "?"));
    return 0;
}
// Every kHookCount instructions: past the budget the script's call ends with an error. `exceeded` makes pcall / xpcall pass that
// error on, so a script cannot catch it and go on.
void Hook(lua_State* L, lua_Debug*) {
    Script* s = Self(L);
    s->budget -= kHookCount;
    if (s->budget < 0) { s->exceeded = true; luaL_error(L, "the script ran too long (instruction budget used up)"); }
}
int FinishPcall(lua_State* L, int status, lua_KContext extra) {
    if (status != LUA_OK && status != LUA_YIELD) {
        if (Self(L)->exceeded) return lua_error(L);
        lua_pushboolean(L, 0); lua_pushvalue(L, -2);
        return 2;
    }
    return lua_gettop(L) - static_cast<int>(extra);
}
int PcallFn(lua_State* L) {  // lbaselib's pcall, passing on a budget error
    luaL_checkany(L, 1);
    lua_pushboolean(L, 1); lua_insert(L, 1);
    const int status = lua_pcallk(L, lua_gettop(L) - 2, LUA_MULTRET, 0, 0, FinishPcall);
    return FinishPcall(L, status, 0);
}
int XpcallFn(lua_State* L) {
    const int n = lua_gettop(L);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushboolean(L, 1); lua_pushvalue(L, 1); lua_rotate(L, 3, 2);
    const int status = lua_pcallk(L, n - 2, LUA_MULTRET, 2, 2, FinishPcall);
    return FinishPcall(L, status, 2);
}
int PrintFn(lua_State* L) {  // to the log, as the script's own line
    std::string line;
    const int n = lua_gettop(L);
    for (int i = 1; i <= n; ++i) {
        size_t length = 0; const char* text = luaL_tolstring(L, i, &length);
        if (i > 1) line += '\t';
        line.append(text, length);
        lua_pop(L, 1);
    }
    Script* s = Self(L);
    if (s) Say(*s, line);
    return 0;
}
int LoadFn(lua_State* L) {  // load(text [, name [, "t" [, env]]]): text chunks only (bytecode is not checked by Lua and may crash it)
    size_t length = 0;
    const char* text = lua_tolstring(L, 1, &length);
    if (!text) { lua_pushnil(L); lua_pushstring(L, "load takes a string"); return 2; }
    const char* name = luaL_optstring(L, 2, text);
    const bool has_env = !lua_isnone(L, 4);
    if (luaL_loadbufferx(L, text, length, name, "t") != LUA_OK) { lua_pushnil(L); lua_insert(L, -2); return 2; }
    if (has_env) { lua_pushvalue(L, 4); if (!lua_setupvalue(L, -2, 1)) lua_pop(L, 1); }
    return 1;
}
int CollectGarbageFn(lua_State* L) {  // "collect" and "count" only
    const char* what = luaL_optstring(L, 1, "collect");
    if (!std::strcmp(what, "count")) { lua_pushnumber(L, static_cast<lua_Number>(lua_gc(L, LUA_GCCOUNT)) + static_cast<lua_Number>(lua_gc(L, LUA_GCCOUNTB)) / 1024.0); return 1; }
    if (!std::strcmp(what, "collect")) { lua_gc(L, LUA_GCCOLLECT); lua_pushinteger(L, 0); return 1; }
    return luaL_error(L, "collectgarbage: only \"collect\" and \"count\"");
}
int RepFn(lua_State* L) {  // string.rep, but a result of more than 1 MB is an error before it is built
    size_t length = 0; const char* text = luaL_checklstring(L, 1, &length);
    const lua_Integer count = luaL_checkinteger(L, 2);
    size_t sep_length = 0; const char* sep = luaL_optlstring(L, 3, "", &sep_length);
    if (count <= 0) { lua_pushliteral(L, ""); return 1; }
    const double total = static_cast<double>(count) * static_cast<double>(length) + static_cast<double>(count - 1) * static_cast<double>(sep_length);
    if (total > 1048576.0) return luaL_error(L, "string.rep: the result would be over 1 MB");
    std::string out; out.reserve(static_cast<size_t>(total));
    for (lua_Integer i = 0; i < count; ++i) { if (i) out.append(sep, sep_length); out.append(text, length); }
    lua_pushlstring(L, out.data(), out.size());
    return 1;
}
int OsClock(lua_State* L) { lua_pushnumber(L, static_cast<lua_Number>(std::clock()) / static_cast<lua_Number>(CLOCKS_PER_SEC)); return 1; }
int OsTime(lua_State* L) { lua_pushinteger(L, static_cast<lua_Integer>(std::time(nullptr))); return 1; }
int OsDifftime(lua_State* L) { lua_pushnumber(L, luaL_checknumber(L, 1) - luaL_optnumber(L, 2, 0)); return 1; }

void Require(lua_State* L, const char* name, lua_CFunction open) { luaL_requiref(L, name, open, 1); lua_pop(L, 1); }
void RemoveGlobal(lua_State* L, const char* name) { lua_pushnil(L); lua_setglobal(L, name); }
void SetGlobalFn(lua_State* L, const char* name, lua_CFunction fn) { lua_pushcfunction(L, fn); lua_setglobal(L, name); }
void OpenPbz(lua_State* L);  // below
int OpenLibs(lua_State* L) {  // runs under pcall: an allocation failure here is an error, not a panic
    Require(L, LUA_GNAME, luaopen_base);
    Require(L, LUA_COLIBNAME, luaopen_coroutine);
    Require(L, LUA_TABLIBNAME, luaopen_table);
    Require(L, LUA_STRLIBNAME, luaopen_string);
    Require(L, LUA_MATHLIBNAME, luaopen_math);
    Require(L, LUA_UTF8LIBNAME, luaopen_utf8);
    for (const char* name : {"dofile", "loadfile"}) RemoveGlobal(L, name);
    SetGlobalFn(L, "load", LoadFn); SetGlobalFn(L, "pcall", PcallFn); SetGlobalFn(L, "xpcall", XpcallFn);
    SetGlobalFn(L, "print", PrintFn); SetGlobalFn(L, "collectgarbage", CollectGarbageFn);
    lua_getglobal(L, LUA_STRLIBNAME);
    lua_pushnil(L); lua_setfield(L, -2, "dump");
    lua_pushcfunction(L, RepFn); lua_setfield(L, -2, "rep");
    lua_pop(L, 1);
    lua_newtable(L);  // a small os: no files, no processes, no environment
    lua_pushcfunction(L, OsClock); lua_setfield(L, -2, "clock");
    lua_pushcfunction(L, OsTime); lua_setfield(L, -2, "time");
    lua_pushcfunction(L, OsDifftime); lua_setfield(L, -2, "difftime");
    lua_setglobal(L, "os");
    OpenPbz(L);
    return 0;
}

// ---- calling into Lua -------------------------------------------------------------------------------------------------------
struct Job { int ref; const std::function<int(lua_State*)>* push; int nres; };
int JobMain(lua_State* L) {  // under pcall: the function, its arguments and the call all happen in one protected frame
    const Job* job = static_cast<const Job*>(lua_touserdata(L, 1));
    lua_settop(L, 0);
    lua_rawgeti(L, LUA_REGISTRYINDEX, job->ref);
    const int nargs = job->push ? (*job->push)(L) : 0;
    lua_call(L, nargs, job->nres);
    return job->nres;
}
struct PcallArgs { lua_State* L; int nargs; int status; };
bool GuardedPcall(PcallArgs& a, DWORD& code) {  // an access violation in the interpreter ends the call, not the game
    __try { a.status = lua_pcall(a.L, a.nargs, LUA_MULTRET, 0); return true; }
    __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void Fail(Script& s, const std::string& where, const std::string& message) {
    s.dead = true;
    ComboPlugins::ScriptFailed(s.self, where, message);
}
// Runs the function in registry slot `ref` with the arguments `push` puts on the stack. On success the `nres` results are on the stack
// (the caller reads them, then calls Done); false: the script failed (it is switched off) or has nothing to call.
bool Call(Script& s, int ref, const char* where, const std::function<int(lua_State*)>& push, int nres, int& top) {
    if (s.dead || !s.L || ref == LUA_NOREF) return false;
    if (g_game_thread && GetCurrentThreadId() != g_game_thread) return false;  // Lua runs on the game thread only
    lua_State* L = s.L;
    top = lua_gettop(L);
    if (s.depth == 0) { s.budget = kCallBudget; s.exceeded = false; }
    ++s.depth;
    Job job{ref, &push, nres};
    lua_pushcfunction(L, JobMain);
    lua_pushlightuserdata(L, &job);
    PcallArgs args{L, 1, LUA_OK};
    DWORD code = 0;
    const bool survived = GuardedPcall(args, code);
    --s.depth;
    if (!survived) {
        char text[64]{}; _snprintf_s(text, sizeof(text), _TRUNCATE, "access violation in the interpreter (0x%08lx)", code);
        Fail(s, where, text); return false;
    }
    if (args.status != LUA_OK) {
        const char* message = lua_tostring(L, -1);
        const std::string text = message ? message : "(error object is not a string)";
        lua_settop(L, top);
        Fail(s, where, text);
        return false;
    }
    return true;
}
void Done(Script& s, int top) { if (s.L) lua_settop(s.L, top); }

// ---- small Lua helpers (none of them raise except through luaL_error where a bad call is the script's bug) ---------------------
const char* ResultName(PbzResult r) {
    switch (r) {
    case PBZ_OK: return "OK"; case PBZ_E_ARG: return "ARG"; case PBZ_E_STATE: return "STATE"; case PBZ_E_THREAD: return "THREAD";
    case PBZ_E_NOT_FOUND: return "NOT_FOUND"; case PBZ_E_FULL: return "FULL"; case PBZ_E_EXISTS: return "EXISTS"; case PBZ_E_BUFFER: return "BUFFER";
    case PBZ_E_DISABLED: return "DISABLED"; case PBZ_E_VERSION: return "VERSION"; case PBZ_E_DENIED: return "DENIED";
    default: return "UNKNOWN";
    }
}
// The functions of the pbz table never raise: a failure is nil, the result code and its name (or a message).
int Fail(lua_State* L, PbzResult code, const char* message = nullptr) {
    lua_pushnil(L); lua_pushinteger(L, code); lua_pushstring(L, message ? message : ResultName(code));
    return 3;
}
int Ok(lua_State* L, PbzResult r) {
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushboolean(L, 1);
    return 1;
}
void SetInt(lua_State* L, const char* key, lua_Integer v) { lua_pushinteger(L, v); lua_setfield(L, -2, key); }
void SetNum(lua_State* L, const char* key, double v) { lua_pushnumber(L, v); lua_setfield(L, -2, key); }
void SetStr(lua_State* L, const char* key, const char* v) { lua_pushstring(L, v ? v : ""); lua_setfield(L, -2, key); }
void SetStr(lua_State* L, const char* key, const std::string& v) { lua_pushlstring(L, v.data(), v.size()); lua_setfield(L, -2, key); }
void SetBool(lua_State* L, const char* key, bool v) { lua_pushboolean(L, v ? 1 : 0); lua_setfield(L, -2, key); }
void SetList(lua_State* L, const char* key, const int32_t* values, int32_t count) {
    lua_createtable(L, count > 0 ? count : 0, 0);
    for (int32_t i = 0; i < count; ++i) { lua_pushinteger(L, values[i]); lua_rawseti(L, -2, i + 1); }
    lua_setfield(L, -2, key);
}
bool ArgStr(lua_State* L, int index, std::string& out) {
    const int t = lua_type(L, index);
    if (t != LUA_TSTRING && t != LUA_TNUMBER) return false;
    size_t length = 0; const char* text = lua_tolstring(L, index, &length);
    out.assign(text, length);
    return true;
}
bool ArgInt(lua_State* L, int index, int32_t& out) {
    if (lua_type(L, index) != LUA_TNUMBER) return false;
    const lua_Number v = lua_tonumber(L, index);
    if (!std::isfinite(v) || v < -2147483648.0 || v > 2147483647.0) return false;
    out = static_cast<int32_t>(v);
    return true;
}
bool ArgValue(lua_State* L, int index, std::string& out) {  // text, a number or a boolean: the text a property or a field takes
    if (lua_type(L, index) == LUA_TBOOLEAN) { out = lua_toboolean(L, index) ? "1" : "0"; return true; }
    if (lua_type(L, index) == LUA_TNUMBER) {
        char text[48]{};
        if (lua_isinteger(L, index)) _snprintf_s(text, sizeof(text), _TRUNCATE, "%lld", static_cast<long long>(lua_tointeger(L, index)));
        else _snprintf_s(text, sizeof(text), _TRUNCATE, "%.9g", static_cast<double>(lua_tonumber(L, index)));
        out = text; return true;
    }
    return ArgStr(L, index, out);
}
std::string FieldStr(lua_State* L, int table, const char* key) {
    lua_getfield(L, table, key);
    std::string out; ArgStr(L, -1, out);
    lua_pop(L, 1);
    return out;
}
bool FieldInt(lua_State* L, int table, const char* key, int32_t& out) {
    lua_getfield(L, table, key);
    const bool ok = ArgInt(L, -1, out);
    lua_pop(L, 1);
    return ok;
}
double FieldNum(lua_State* L, int table, const char* key, double fallback) {
    lua_getfield(L, table, key);
    double v = fallback;
    if (lua_type(L, -1) == LUA_TNUMBER) v = static_cast<double>(lua_tonumber(L, -1));
    else if (lua_type(L, -1) == LUA_TBOOLEAN) v = lua_toboolean(L, -1) ? 1 : 0;
    lua_pop(L, 1);
    return v;
}
template <class F> PbzResult ReadText(F&& read, std::string& out, bool keep_on_state = false) {
    std::string buffer(1024, '\0'); int32_t needed = 0;
    PbzResult r = read(buffer.data(), static_cast<int32_t>(buffer.size()), &needed);
    if (r == PBZ_E_BUFFER && needed > 0 && needed <= 8 * 1024 * 1024) { buffer.assign(static_cast<size_t>(needed), '\0'); r = read(buffer.data(), needed, &needed); }
    if (r == PBZ_OK || (keep_on_state && r == PBZ_E_STATE)) out.assign(buffer.c_str());
    return r;
}
void PushLines(lua_State* L, const std::string& text) {
    lua_newtable(L);
    lua_Integer n = 0; size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        const std::string line = text.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        lua_pushlstring(L, line.data(), line.size()); lua_rawseti(L, -2, ++n);
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
}
std::vector<std::string> Cells(const std::string& line) {
    std::vector<std::string> out; size_t at = 0;
    while (true) {
        const size_t tab = line.find('\t', at);
        out.push_back(line.substr(at, tab == std::string::npos ? std::string::npos : tab - at));
        if (tab == std::string::npos) break;
        at = tab + 1;
    }
    return out;
}
// A TSV list (one item per line) as an array of tables whose fields are named by `names`; a field in `numbers` is a number, in `flags` a boolean.
void PushRows(lua_State* L, const std::string& text, const std::vector<const char*>& names, const std::vector<const char*>& numbers = {}, const std::vector<const char*>& flags = {}) {
    lua_newtable(L);
    lua_Integer n = 0; size_t at = 0;
    while (at < text.size()) {
        const size_t nl = text.find('\n', at);
        const std::vector<std::string> cells = Cells(text.substr(at, nl == std::string::npos ? std::string::npos : nl - at));
        lua_newtable(L);
        for (size_t i = 0; i < names.size() && i < cells.size(); ++i) {
            const auto is = [&](const std::vector<const char*>& list) { for (const char* x : list) if (!std::strcmp(x, names[i])) return true; return false; };
            if (is(numbers)) { char* end = nullptr; const double v = std::strtod(cells[i].c_str(), &end); if (end && end != cells[i].c_str()) lua_pushnumber(L, v); else lua_pushstring(L, cells[i].c_str()); }
            else if (is(flags)) lua_pushboolean(L, cells[i] == "1");
            else lua_pushlstring(L, cells[i].data(), cells[i].size());
            lua_setfield(L, -2, names[i]);
        }
        lua_rawseti(L, -2, ++n);
        if (nl == std::string::npos) break;
        at = nl + 1;
    }
}
struct CapName { const char* name; uint32_t cap; };
constexpr CapName kCaps[] = {{"combo", PBZ_CAP_COMBO}, {"read", PBZ_CAP_READ}, {"write", PBZ_CAP_WRITE}, {"ini", PBZ_CAP_INI}, {"command", PBZ_CAP_COMMAND}, {"ui", PBZ_CAP_UI}, {"ue", PBZ_CAP_UE}};
uint32_t CapsFrom(lua_State* L, int index) {  // {"read", "write"}, "read,write" or a number
    if (lua_type(L, index) == LUA_TNUMBER) return static_cast<uint32_t>(lua_tointeger(L, index)) & PBZ_CAP_ALL;
    std::vector<std::string> names;
    if (lua_istable(L, index)) {
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, index));
        for (lua_Integer i = 1; i <= n; ++i) { lua_rawgeti(L, index, i); std::string name; if (ArgStr(L, -1, name)) names.push_back(name); lua_pop(L, 1); }
    } else {
        std::string all; if (ArgStr(L, index, all)) { size_t at = 0; while (at <= all.size()) { const size_t c = all.find(',', at); names.push_back(all.substr(at, c == std::string::npos ? std::string::npos : c - at)); if (c == std::string::npos) break; at = c + 1; } }
    }
    uint32_t caps = 0;
    for (std::string& name : names) {
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();
        for (const CapName& c : kCaps) if (name == c.name) caps |= c.cap;
    }
    return caps & PBZ_CAP_ALL;
}
void PushCaps(lua_State* L, uint32_t caps) {  // { read = true, write = true, ... }
    lua_newtable(L);
    for (const CapName& c : kCaps) SetBool(L, c.name, (caps & c.cap) != 0);
}

// ---- the callbacks the plugin host calls ----------------------------------------------------------------------------------------
void PushCall(lua_State* L, const PbzNodeCall* c) {
    lua_createtable(L, 0, 10);
    SetInt(L, "node", c->node); SetInt(L, "trigger", c->trigger); SetInt(L, "button", c->button); SetInt(L, "value", c->value);
    SetInt(L, "press_bits", c->press_bits); SetBool(L, "press_npc_key", c->press_npc_key != 0); SetInt(L, "output_count", c->output_count);
    SetStr(L, "action", c->action); SetStr(L, "table", c->table); SetInt(L, "now_ms", static_cast<lua_Integer>(c->now_ms));
}
int32_t PBZ_CALL DecideThunk(void* user, const PbzNodeCall* call) {
    ActionCtx* a = static_cast<ActionCtx*>(user);
    Script& s = *a->owner;
    int top = 0; int32_t out = -1;
    if (Call(s, a->ref, ("decide " + a->id).c_str(), [&](lua_State* L) { PushCall(L, call); return 1; }, 1, top)) {
        int32_t v = -1;
        if (ArgInt(s.L, -1, v)) out = v;
        else if (lua_type(s.L, -1) == LUA_TBOOLEAN) out = lua_toboolean(s.L, -1) ? 0 : 1;
        Done(s, top);
    }
    return out;
}
void PBZ_CALL RunThunk(void* user, const PbzNodeCall* call) {
    ActionCtx* a = static_cast<ActionCtx*>(user);
    Script& s = *a->owner;
    int top = 0;
    if (Call(s, a->ref, ("run " + a->id).c_str(), [&](lua_State* L) { PushCall(L, call); return 1; }, 0, top)) Done(s, top);
}
void PBZ_CALL EventThunk(void* user, const PbzEvent* e) {
    Script& s = *static_cast<Script*>(user);
    int top = 0;
    if (!Call(s, s.on_event, "on_event", [&](lua_State* L) {
            lua_createtable(L, 0, 12);
            SetInt(L, "type", e->type); SetInt(L, "now_ms", static_cast<lua_Integer>(e->now_ms)); SetInt(L, "trigger", e->trigger); SetInt(L, "node", e->node);
            SetInt(L, "node_type", e->node_type); SetInt(L, "button", e->button); SetBool(L, "taken", e->taken != 0); SetInt(L, "press_bits", e->press_bits);
            SetBool(L, "npc_key", e->npc_key != 0); SetStr(L, "text", e->text); SetStr(L, "ability", e->ability); SetStr(L, "boss", e->boss);
            return 1; }, 0, top)) return;
    Done(s, top);
}
int32_t PBZ_CALL AbilityThunk(void* user, const PbzAbilityEvent* a) {
    Script& s = *static_cast<Script*>(user);
    int top = 0; int32_t out = PBZ_PASS;
    if (Call(s, s.on_hero_ability, "on_hero_ability", [&](lua_State* L) {
            lua_createtable(L, 0, 5);
            SetStr(L, "name", a->name); SetInt(L, "bits", a->bits); SetInt(L, "handle", a->handle); SetBool(L, "chain_own", a->chain_own != 0); SetInt(L, "now_ms", static_cast<lua_Integer>(a->now_ms));
            return 1; }, 1, top)) {
        if (lua_type(s.L, -1) == LUA_TBOOLEAN) out = lua_toboolean(s.L, -1) ? PBZ_CONSUME : PBZ_PASS;
        else { int32_t v = 0; if (ArgInt(s.L, -1, v)) out = v == PBZ_CONSUME ? PBZ_CONSUME : PBZ_PASS; }
        Done(s, top);
    }
    return out;
}
void PBZ_CALL TickThunk(void* user, uint64_t now) {
    Script& s = *static_cast<Script*>(user);
    int top = 0;
    if (Call(s, s.on_tick, "on_tick", [&](lua_State* L) { lua_pushinteger(L, static_cast<lua_Integer>(now)); return 1; }, 0, top)) Done(s, top);
}
void PBZ_CALL EnabledThunk(void* user, int32_t enabled) {
    Script& s = *static_cast<Script*>(user);
    int top = 0;
    if (Call(s, s.on_enabled, "on_enabled", [&](lua_State* L) { lua_pushboolean(L, enabled != 0); return 1; }, 0, top)) Done(s, top);
}
void PBZ_CALL ModuleEventThunk(void* user, const PbzModuleEvent* e) {
    Script& s = *static_cast<Script*>(user);
    int top = 0;
    if (Call(s, s.on_module_event, "on_module_event", [&](lua_State* L) {
            lua_createtable(L, 0, 4); SetStr(L, "module", e->module); SetStr(L, "event", e->event); SetStr(L, "text", e->text); SetInt(L, "now_ms", static_cast<lua_Integer>(e->now_ms));
            return 1; }, 0, top)) Done(s, top);
}
void PBZ_CALL UiThunk(void* user, const PbzUiEvent* e) {
    Script& s = *static_cast<Script*>(user);
    int top = 0;
    if (Call(s, s.on_ui, "on_ui", [&](lua_State* L) {
            lua_createtable(L, 0, 5); SetStr(L, "page", e->page); SetStr(L, "item", e->item); SetInt(L, "kind", e->kind); SetNum(L, "value", e->value); SetInt(L, "now_ms", static_cast<lua_Integer>(e->now_ms));
            return 1; }, 0, top)) Done(s, top);
}
void PBZ_CALL CapsThunk(void* user, uint32_t granted) {
    Script& s = *static_cast<Script*>(user);
    int top = 0;
    if (Call(s, s.on_caps, "on_caps", [&](lua_State* L) { lua_pushinteger(L, granted); PushCaps(L, granted); return 2; }, 0, top)) Done(s, top);
}
// The callbacks the script has set, handed to the plugin host (again whenever it sets one).
void Sync(Script& s) {
    if (!s.api || !s.self || !s.api->set_callbacks) return;
    PbzCallbacks cb{}; cb.size = sizeof(cb); cb.user = &s;
    if (s.on_event != LUA_NOREF) cb.on_event = EventThunk;
    if (s.on_hero_ability != LUA_NOREF) cb.on_hero_ability = AbilityThunk;
    if (s.on_tick != LUA_NOREF) cb.on_tick = TickThunk;
    if (s.on_enabled != LUA_NOREF) cb.on_enabled = EnabledThunk;
    if (s.on_module_event != LUA_NOREF) cb.on_module_event = ModuleEventThunk;
    if (s.on_ui != LUA_NOREF) cb.on_ui = UiThunk;
    if (s.on_caps != LUA_NOREF) cb.on_caps = CapsThunk;
    s.api->set_callbacks(s.self, &cb);
}

// ---- the pbz table (mirrors the C API; names as in the UE4SS fork's PBZ table) -----------------------------------------------------
#define NEED_SCRIPT() \
    Script* S = Self(L); \
    if (!S || !S->api || !S->self || S->dead) return Fail(L, PBZ_E_STATE, "the script is not running")

// PBZ.Init{ name =, version =, author =, description =, min_revision =, caps = {"read", "write"}, caps_reason = "..." } while the script loads.
// (A script's id is "lua.<file name>": it cannot take another plugin's id and its grants. An `id` field is accepted and ignored.)
int FnInit(lua_State* L) {
    NEED_SCRIPT();
    if (!S->loading) return Fail(L, PBZ_E_STATE, "Init only works while the script loads (at its top level)");
    if (!lua_istable(L, 1)) return Fail(L, PBZ_E_ARG, "Init{ name = ..., caps = {...} }");
    const std::string name = FieldStr(L, 1, "name"), version = FieldStr(L, 1, "version"), author = FieldStr(L, 1, "author"), description = FieldStr(L, 1, "description"), reason = FieldStr(L, 1, "caps_reason");
    int32_t revision = 0; FieldInt(L, 1, "min_revision", revision);
    uint32_t caps = PBZ_CAP_COMBO;
    lua_getfield(L, 1, "caps");
    if (!lua_isnil(L, -1)) caps |= CapsFrom(L, lua_gettop(L));
    lua_pop(L, 1);
    PbzPluginInfoEx info{}; info.size = sizeof(info);
    info.id = S->id.c_str(); info.name = name.empty() ? S->stem.c_str() : name.c_str(); info.version = version.c_str(); info.author = author.c_str(); info.description = description.c_str();
    info.min_api = 1; info.min_revision = static_cast<uint32_t>(revision < 0 ? 0 : revision); info.caps = caps; info.caps_reason = reason.empty() ? nullptr : reason.c_str();
    return Ok(L, S->api->describe_ex(S->self, &info));
}
int FnId(lua_State* L) { Script* S = Self(L); lua_pushstring(L, S ? S->id.c_str() : ""); return 1; }
int FnAvailable(lua_State* L) { lua_pushboolean(L, 1); return 1; }
int FnStarted(lua_State* L) { Script* S = Self(L); lua_pushboolean(L, S && S->self && !S->dead); return 1; }
int FnVersion(lua_State* L) {  // api version, MOD version, interface revision
    NEED_SCRIPT();
    lua_pushinteger(L, S->api->version); lua_pushstring(L, S->api->mod_version ? S->api->mod_version : ""); lua_pushinteger(L, PBZ_API_REVISION_OF(S->api));
    return 3;
}
int FnRevision(lua_State* L) { NEED_SCRIPT(); lua_pushinteger(L, PBZ_API_REVISION_OF(S->api)); return 1; }
int FnHasFeature(lua_State* L) {
    NEED_SCRIPT();
    std::string name;
    if (!ArgStr(L, 1, name)) return Fail(L, PBZ_E_ARG);
    lua_pushboolean(L, PBZ_API_HAS(S->api, has_feature) && S->api->has_feature(name.c_str()) == 1);
    return 1;
}
int FnCaps(lua_State* L) {  // the grants now: a table { combo = true, read = false, ... } and the number
    NEED_SCRIPT();
    const uint32_t caps = S->api->granted_caps(S->self);
    PushCaps(L, caps); lua_pushinteger(L, caps);
    return 2;
}
int FnLog(lua_State* L) {
    NEED_SCRIPT();
    std::string text;
    if (!ArgStr(L, 1, text)) return Fail(L, PBZ_E_ARG);
    Say(*S, text);
    lua_pushboolean(L, 1);
    return 1;
}
int FnNotify(lua_State* L) {
    NEED_SCRIPT();
    std::string text;
    if (!ArgStr(L, 1, text)) return Fail(L, PBZ_E_ARG);
    if (Allowed(S->notify_window, S->notify_lines, 4, *S, "notices")) S->api->notify(S->self, text.substr(0, 200).c_str());
    lua_pushboolean(L, 1);
    return 1;
}
int FnEnabled(lua_State* L) { NEED_SCRIPT(); lua_pushboolean(L, S->api->enabled() ? 1 : 0); return 1; }
int FnNow(lua_State* L) { lua_pushinteger(L, static_cast<lua_Integer>(GetTickCount64())); return 1; }

// signals
int FnRegisterSignal(lua_State* L) {
    NEED_SCRIPT();
    if (!lua_istable(L, 1)) return Fail(L, PBZ_E_ARG, "RegisterSignal{ id =, name =, group =, description = }");
    const std::string id = FieldStr(L, 1, "id"), name = FieldStr(L, 1, "name"), group = FieldStr(L, 1, "group"), description = FieldStr(L, 1, "description");
    PbzSignalDesc d{}; d.size = sizeof(d); d.id = id.c_str(); d.name = name.c_str(); d.group = group.empty() ? nullptr : group.c_str(); d.description = description.empty() ? nullptr : description.c_str();
    int32_t code = 0;
    const PbzResult r = S->api->register_signal(S->self, &d, &code);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushinteger(L, code);
    return 1;
}
int FnUnregisterSignal(lua_State* L) {
    NEED_SCRIPT(); std::string id;
    if (!ArgStr(L, 1, id)) return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->unregister_signal(S->self, id.c_str()));
}
int FnSignalCode(lua_State* L) {  // the key code of "<this script's id>/<signal id>"
    NEED_SCRIPT(); std::string id;
    if (!ArgStr(L, 1, id)) return Fail(L, PBZ_E_ARG);
    lua_pushinteger(L, NpcCombo::SignalKeyCode(S->id + "/" + id));
    return 1;
}
int FnFireSignal(lua_State* L) {
    NEED_SCRIPT(); std::string id;
    if (!ArgStr(L, 1, id)) return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->fire_signal(S->self, id.c_str()));
}
int FnSetSignal(lua_State* L) {
    NEED_SCRIPT(); std::string id;
    if (!ArgStr(L, 1, id)) return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->set_signal(S->self, id.c_str(), lua_toboolean(L, 2) ? 1 : 0));
}
int FnSignalWanted(lua_State* L) {
    NEED_SCRIPT(); std::string id;
    if (!ArgStr(L, 1, id)) return Fail(L, PBZ_E_ARG);
    lua_pushboolean(L, S->api->signal_wanted(S->self, id.c_str()) == 1 ? 1 : 0);
    return 1;
}

// node actions: RegisterAction({ id =, name =, kind = "decide" | "run", outputs = {...}, value = { label =, unit =, min =, max =, step =, default = },
// group =, description =, decide = function(call) ... end | run = function(call) ... end } [, function])
int FnRegisterAction(lua_State* L) {
    NEED_SCRIPT();
    if (!lua_istable(L, 1)) return Fail(L, PBZ_E_ARG, "RegisterAction({ id =, name =, kind =, ... }, function(call) ... end)");
    const std::string id = FieldStr(L, 1, "id"), name = FieldStr(L, 1, "name"), group = FieldStr(L, 1, "group"), description = FieldStr(L, 1, "description");
    int32_t kind = PBZ_ACTION_RUN;
    lua_getfield(L, 1, "kind");
    if (lua_type(L, -1) == LUA_TSTRING) kind = std::strcmp(lua_tostring(L, -1), "decide") == 0 ? PBZ_ACTION_DECIDE : PBZ_ACTION_RUN; else ArgInt(L, -1, kind);
    lua_pop(L, 1);
    kind = kind == PBZ_ACTION_DECIDE ? PBZ_ACTION_DECIDE : PBZ_ACTION_RUN;
    std::vector<std::string> outputs;
    lua_getfield(L, 1, "outputs");
    if (lua_istable(L, -1)) {
        const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
        for (lua_Integer i = 1; i <= n && i <= 6; ++i) { lua_rawgeti(L, -1, i); std::string out; ArgStr(L, -1, out); outputs.push_back(out); lua_pop(L, 1); }
    }
    lua_pop(L, 1);
    // The function: the 2nd argument, else the table's decide / run field.
    int function = 0;
    if (lua_isfunction(L, 2)) function = 2;
    else { lua_getfield(L, 1, kind == PBZ_ACTION_DECIDE ? "decide" : "run"); if (lua_isfunction(L, -1)) function = lua_gettop(L); else lua_pop(L, 1); }
    if (!function) return Fail(L, PBZ_E_ARG, "the action needs its function (2nd argument, or a decide / run field)");
    auto ctx = std::make_unique<ActionCtx>(); ctx->owner = S; ctx->id = id;
    lua_pushvalue(L, function); ctx->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    std::vector<const char*> out_names; for (const std::string& o : outputs) out_names.push_back(o.c_str());
    PbzActionDesc d{}; d.size = sizeof(d); d.id = id.c_str(); d.name = name.c_str(); d.group = group.empty() ? nullptr : group.c_str(); d.description = description.empty() ? nullptr : description.c_str();
    d.kind = kind; d.output_count = static_cast<int32_t>(outputs.size()); d.outputs = out_names.empty() ? nullptr : out_names.data();
    std::string value_label, value_unit;
    lua_getfield(L, 1, "value");
    if (lua_istable(L, -1)) {
        const int v = lua_gettop(L);
        value_label = FieldStr(L, v, "label"); value_unit = FieldStr(L, v, "unit");
        d.has_value = 1; d.value_label = value_label.c_str(); d.value_unit = value_unit.empty() ? nullptr : value_unit.c_str();
        d.value_min = static_cast<int32_t>(FieldNum(L, v, "min", 0)); d.value_max = static_cast<int32_t>(FieldNum(L, v, "max", 100));
        d.value_step = static_cast<int32_t>(FieldNum(L, v, "step", 1)); d.value_default = static_cast<int32_t>(FieldNum(L, v, "default", 0));
    }
    lua_pop(L, 1);
    d.decide = kind == PBZ_ACTION_DECIDE ? DecideThunk : nullptr; d.run = kind == PBZ_ACTION_RUN ? RunThunk : nullptr; d.user = ctx.get();
    const PbzResult r = S->api->register_action(S->self, &d);
    if (r != PBZ_OK) { luaL_unref(L, LUA_REGISTRYINDEX, ctx->ref); return Fail(L, r); }
    for (auto& old : S->actions) if (old->id == id && old->ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, old->ref); old->ref = LUA_NOREF; }  // replaced: the host calls the new one
    S->actions.push_back(std::move(ctx));
    lua_pushboolean(L, 1);
    return 1;
}
int FnUnregisterAction(lua_State* L) {
    NEED_SCRIPT(); std::string id;
    if (!ArgStr(L, 1, id)) return Fail(L, PBZ_E_ARG);
    const PbzResult r = S->api->unregister_action(S->self, id.c_str());
    if (r == PBZ_OK) for (auto& a : S->actions) if (a->id == id && a->ref != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, a->ref); a->ref = LUA_NOREF; }
    return Ok(L, r);
}

// callbacks: OnEvent(fn) ... ; nil clears
int SetCallback(lua_State* L, int Script::*field) {
    NEED_SCRIPT();
    if (!lua_isfunction(L, 1) && !lua_isnoneornil(L, 1)) return Fail(L, PBZ_E_ARG, "a function or nil");
    if (S->*field != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, S->*field); S->*field = LUA_NOREF; }
    if (lua_isfunction(L, 1)) { lua_pushvalue(L, 1); S->*field = luaL_ref(L, LUA_REGISTRYINDEX); }
    Sync(*S);
    lua_pushboolean(L, 1);
    return 1;
}
int FnOnEvent(lua_State* L) { return SetCallback(L, &Script::on_event); }
int FnOnHeroAbility(lua_State* L) { return SetCallback(L, &Script::on_hero_ability); }
int FnOnTick(lua_State* L) { return SetCallback(L, &Script::on_tick); }
int FnOnEnabled(lua_State* L) { return SetCallback(L, &Script::on_enabled); }
int FnOnModuleEvent(lua_State* L) { return SetCallback(L, &Script::on_module_event); }
int FnOnUi(lua_State* L) { return SetCallback(L, &Script::on_ui); }
int FnOnCaps(lua_State* L) { return SetCallback(L, &Script::on_caps); }
int FnOnStart(lua_State* L) {  // called once, right after the script's top level has run (before the plugin is in use)
    NEED_SCRIPT();
    if (!lua_isfunction(L, 1) && !lua_isnoneornil(L, 1)) return Fail(L, PBZ_E_ARG, "a function or nil");
    if (S->on_start != LUA_NOREF) { luaL_unref(L, LUA_REGISTRYINDEX, S->on_start); S->on_start = LUA_NOREF; }
    if (lua_isfunction(L, 1)) { lua_pushvalue(L, 1); S->on_start = luaL_ref(L, LUA_REGISTRYINDEX); }
    lua_pushboolean(L, 1);
    return 1;
}

// reading the chain
void PushNode(lua_State* L, const PbzNodeInfo& n) {
    lua_createtable(L, 0, 20);
    SetInt(L, "index", n.index); SetInt(L, "type", n.type); SetBool(L, "enabled", n.enabled != 0); SetInt(L, "x", n.x); SetInt(L, "y", n.y); SetInt(L, "next", n.next);
    SetInt(L, "group", n.group); SetInt(L, "ms", n.ms); SetInt(L, "button", n.button); SetInt(L, "route_count", n.route_count);
    SetList(L, "keys", n.keys, (std::min)(n.key_count, 6)); SetInt(L, "value", n.value); SetInt(L, "action_kind", n.action_kind);
    SetStr(L, "ability", n.ability); SetStr(L, "boss", n.boss); SetStr(L, "weapon", n.weapon); SetStr(L, "action", n.action);
}
void PushRoute(lua_State* L, const PbzRouteInfo& r) {
    lua_createtable(L, 0, 8);
    SetInt(L, "button", r.button); SetInt(L, "next", r.next); SetInt(L, "pause_ms", r.pause_ms); SetInt(L, "hold_ms", r.hold_ms); SetBool(L, "chord", r.chord != 0);
    SetList(L, "keys", r.keys, (std::min)(r.key_count, 6));
}
int FnGetState(lua_State* L) {
    NEED_SCRIPT();
    PbzChainState s{}; s.size = sizeof(s);
    const PbzResult r = S->api->get_state(&s);
    if (r != PBZ_OK) return Fail(L, r);
    lua_createtable(L, 0, 16);
    SetBool(L, "ready", s.ready != 0); SetInt(L, "trigger", s.trigger); SetInt(L, "cursor", s.cursor); SetInt(L, "playing", s.playing); SetInt(L, "queued", s.queued);
    SetInt(L, "button", s.button); SetInt(L, "phase", s.phase); SetBool(L, "look_changing", s.look_changing != 0); SetInt(L, "visit", static_cast<lua_Integer>(s.visit));
    SetInt(L, "generation", static_cast<lua_Integer>(s.generation)); SetInt(L, "now_ms", static_cast<lua_Integer>(s.now_ms)); SetInt(L, "move_started_ms", static_cast<lua_Integer>(s.move_started_ms));
    SetInt(L, "reset_at_ms", static_cast<lua_Integer>(s.reset_at_ms)); SetStr(L, "table", s.table); SetStr(L, "look", s.look);
    return 1;
}
int FnNodeCount(lua_State* L) { NEED_SCRIPT(); lua_pushinteger(L, S->api->node_count()); return 1; }
int FnGetNode(lua_State* L) {
    NEED_SCRIPT(); int32_t i = 0;
    if (!ArgInt(L, 1, i)) return Fail(L, PBZ_E_ARG);
    PbzNodeInfo n{}; n.size = sizeof(n);
    const PbzResult r = S->api->get_node(i, &n);
    if (r != PBZ_OK) return Fail(L, r);
    PushNode(L, n);
    return 1;
}
int FnGetRoute(lua_State* L) {
    NEED_SCRIPT(); int32_t node = 0, row = 0;
    if (!ArgInt(L, 1, node) || !ArgInt(L, 2, row)) return Fail(L, PBZ_E_ARG);
    PbzRouteInfo ri{}; ri.size = sizeof(ri);
    const PbzResult r = S->api->get_route(node, row, &ri);
    if (r != PBZ_OK) return Fail(L, r);
    PushRoute(L, ri);
    return 1;
}
int FnGetHero(lua_State* L) {
    NEED_SCRIPT();
    PbzHeroFacts h{}; h.size = sizeof(h);
    const PbzResult r = S->api->get_hero(&h);
    if (r != PBZ_OK) return Fail(L, r);
    lua_createtable(L, 0, 16);
    SetBool(L, "valid", h.valid != 0); SetNum(L, "health", h.health); SetNum(L, "health_max", h.health_max); SetBool(L, "airborne", h.airborne != 0);
    SetNum(L, "x", h.x); SetNum(L, "y", h.y); SetNum(L, "z", h.z); SetBool(L, "has_target", h.has_target != 0); SetBool(L, "target_locked", h.target_locked != 0);
    SetNum(L, "target_distance", h.target_distance); SetNum(L, "target_health", h.target_health); SetNum(L, "target_health_max", h.target_health_max);
    SetStr(L, "weapon", h.weapon); SetStr(L, "look", h.look);
    return 1;
}
int FnWeaponMoves(lua_State* L) {
    NEED_SCRIPT();
    const int32_t count = S->api->weapon_move_count();
    lua_createtable(L, count > 0 ? count : 0, 0);
    for (int32_t i = 0; i < count; ++i) {
        PbzWeaponMove m{}; m.size = sizeof(m);
        if (S->api->get_weapon_move(i, &m) != PBZ_OK) continue;
        lua_createtable(L, 0, 5); SetStr(L, "weapon", m.weapon); SetStr(L, "ability", m.ability); SetInt(L, "kind", m.kind); SetBool(L, "shaqi", m.shaqi != 0); SetStr(L, "stage", m.stage);
        lua_rawseti(L, -2, i + 1);
    }
    return 1;
}

// controlling the chain
int FnPress(lua_State* L) { NEED_SCRIPT(); int32_t b = 0; if (!ArgInt(L, 1, b)) return Fail(L, PBZ_E_ARG); return Ok(L, S->api->press(S->self, b)); }
int FnPlayNode(lua_State* L) {
    NEED_SCRIPT(); int32_t node = 0, button = -1;
    if (!ArgInt(L, 1, node) || (!lua_isnoneornil(L, 2) && !ArgInt(L, 2, button))) return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->play_node(S->self, node, button));
}
int FnResetChain(lua_State* L) {
    NEED_SCRIPT(); std::string reason = "lua";
    if (!lua_isnoneornil(L, 1) && !ArgStr(L, 1, reason)) return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->reset_chain(S->self, reason.c_str()));
}
int FnCancelHeroAbility(lua_State* L) { NEED_SCRIPT(); int32_t h = 0; if (!ArgInt(L, 1, h)) return Fail(L, PBZ_E_ARG); return Ok(L, S->api->cancel_hero_ability(h)); }

// tables and edit sessions
int FnListTables(lua_State* L) {
    NEED_SCRIPT(); std::string text;
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->list_tables(b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    PushLines(L, text);
    return 1;
}
int FnExportTable(lua_State* L) {
    NEED_SCRIPT(); std::string table, text;
    if (!ArgStr(L, 1, table)) return Fail(L, PBZ_E_ARG);
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->export_table(table.c_str(), b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
int FnImportTable(lua_State* L) {
    NEED_SCRIPT(); std::string table, text;
    if (!ArgStr(L, 1, table) || !ArgStr(L, 2, text)) return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->import_table(S->self, table.c_str(), text.c_str()));
}
int FnEditBegin(lua_State* L) {
    NEED_SCRIPT(); std::string table;
    if (!ArgStr(L, 1, table)) return Fail(L, PBZ_E_ARG);
    PbzEdit edit = 0;
    const PbzResult r = S->api->edit_begin(S->self, table.c_str(), &edit);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushinteger(L, edit);
    return 1;
}
// edit_* with the edit handle as the first argument
bool EditArg(lua_State* L, PbzEdit& edit) { int32_t e = 0; if (!ArgInt(L, 1, e)) return false; edit = e; return true; }
#define EDIT_ARG() PbzEdit E = 0; if (!EditArg(L, E)) return Fail(L, PBZ_E_ARG, "an edit handle from EditBegin")
int FnEditCommit(lua_State* L) { NEED_SCRIPT(); EDIT_ARG(); return Ok(L, S->api->edit_commit(S->self, E)); }
int FnEditCancel(lua_State* L) { NEED_SCRIPT(); EDIT_ARG(); return Ok(L, S->api->edit_cancel(S->self, E)); }
int FnEditNodeCount(lua_State* L) { NEED_SCRIPT(); EDIT_ARG(); lua_pushinteger(L, S->api->edit_node_count(S->self, E)); return 1; }
int FnEditGetNode(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); int32_t i = 0;
    if (!ArgInt(L, 2, i)) return Fail(L, PBZ_E_ARG);
    PbzNodeInfo n{}; n.size = sizeof(n);
    const PbzResult r = S->api->edit_get_node(S->self, E, i, &n);
    if (r != PBZ_OK) return Fail(L, r);
    PushNode(L, n);
    return 1;
}
int FnEditGetRoute(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); int32_t node = 0, row = 0;
    if (!ArgInt(L, 2, node) || !ArgInt(L, 3, row)) return Fail(L, PBZ_E_ARG);
    PbzRouteInfo ri{}; ri.size = sizeof(ri);
    const PbzResult r = S->api->edit_get_route(S->self, E, node, row, &ri);
    if (r != PBZ_OK) return Fail(L, r);
    PushRoute(L, ri);
    return 1;
}
int FnEditAddNode(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); int32_t type = 0, x = 0, y = 0;
    if (!ArgInt(L, 2, type)) return Fail(L, PBZ_E_ARG);
    ArgInt(L, 3, x); ArgInt(L, 4, y);
    int32_t index = -1;
    const PbzResult r = S->api->edit_add_node(S->self, E, type, x, y, &index);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushinteger(L, index);
    return 1;
}
int FnEditRemoveNode(lua_State* L) { NEED_SCRIPT(); EDIT_ARG(); int32_t i = 0; if (!ArgInt(L, 2, i)) return Fail(L, PBZ_E_ARG); return Ok(L, S->api->edit_remove_node(S->self, E, i)); }
int FnEditGetField(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); int32_t node = 0, row = -1; std::string field, text;
    if (!ArgInt(L, 2, node) || !ArgInt(L, 3, row) || !ArgStr(L, 4, field)) return Fail(L, PBZ_E_ARG, "EditGetField(edit, node, row, field)");
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->edit_get_field(S->self, E, node, row, field.c_str(), b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
int FnEditSetField(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); int32_t node = 0, row = -1; std::string field, value;
    if (!ArgInt(L, 2, node) || !ArgInt(L, 3, row) || !ArgStr(L, 4, field) || !ArgValue(L, 5, value)) return Fail(L, PBZ_E_ARG, "EditSetField(edit, node, row, field, value)");
    return Ok(L, S->api->edit_set_field(S->self, E, node, row, field.c_str(), value.c_str()));
}
int FnEditAddRoute(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); int32_t node = 0, row = -1;
    if (!ArgInt(L, 2, node)) return Fail(L, PBZ_E_ARG);
    const PbzResult r = S->api->edit_add_route(S->self, E, node, &row);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushinteger(L, row);
    return 1;
}
int FnEditRemoveRoute(lua_State* L) { NEED_SCRIPT(); EDIT_ARG(); int32_t node = 0, row = 0; if (!ArgInt(L, 2, node) || !ArgInt(L, 3, row)) return Fail(L, PBZ_E_ARG); return Ok(L, S->api->edit_remove_route(S->self, E, node, row)); }
int FnEditLink(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); int32_t from = 0, output = 0, to = -1;
    if (!ArgInt(L, 2, from) || !ArgInt(L, 3, output) || !ArgInt(L, 4, to)) return Fail(L, PBZ_E_ARG, "EditLink(edit, from, output, to)");
    return Ok(L, S->api->edit_link(S->self, E, from, output, to));
}
int FnEditLinkModifier(lua_State* L) { NEED_SCRIPT(); EDIT_ARG(); int32_t from = 0, mod = 0; if (!ArgInt(L, 2, from) || !ArgInt(L, 3, mod)) return Fail(L, PBZ_E_ARG); return Ok(L, S->api->edit_link_modifier(S->self, E, from, mod)); }
int FnEditUnlinkModifier(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); int32_t from = 0, slot = -1;
    if (!ArgInt(L, 2, from) || (!lua_isnoneornil(L, 3) && !ArgInt(L, 3, slot))) return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->edit_unlink_modifier(S->self, E, from, slot));
}
int FnEditExport(lua_State* L) {
    NEED_SCRIPT(); EDIT_ARG(); std::string text;
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->edit_export(S->self, E, b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
int FnEditImport(lua_State* L) { NEED_SCRIPT(); EDIT_ARG(); std::string text; if (!ArgStr(L, 2, text)) return Fail(L, PBZ_E_ARG); return Ok(L, S->api->edit_import(S->self, E, text.c_str())); }
int FnFieldNames(lua_State* L) {
    NEED_SCRIPT(); std::string text; const int32_t rows = lua_toboolean(L, 1) ? 1 : 0;
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->edit_field_names(rows, b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    PushLines(L, text);
    return 1;
}

// options and settings
int FnGetOption(lua_State* L) {
    NEED_SCRIPT(); std::string name; double v = 0;
    if (!ArgStr(L, 1, name)) return Fail(L, PBZ_E_ARG);
    const PbzResult r = S->api->get_option(name.c_str(), &v);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushnumber(L, v);
    return 1;
}
int FnSetOption(lua_State* L) {
    NEED_SCRIPT(); std::string name;
    if (!ArgStr(L, 1, name) || lua_type(L, 2) != LUA_TNUMBER) return Fail(L, PBZ_E_ARG, "SetOption(name, number)");
    return Ok(L, S->api->set_option(S->self, name.c_str(), static_cast<double>(lua_tonumber(L, 2))));
}
int FnGetSetting(lua_State* L) {
    NEED_SCRIPT(); std::string section, key, text;
    if (!ArgStr(L, 1, section) || !ArgStr(L, 2, key)) return Fail(L, PBZ_E_ARG);
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->get_setting(section.c_str(), key.c_str(), b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
int FnSetSetting(lua_State* L) {
    NEED_SCRIPT(); std::string section, key, value;
    if (!ArgStr(L, 1, section) || !ArgStr(L, 2, key) || !ArgValue(L, 3, value)) return Fail(L, PBZ_E_ARG, "SetSetting(section, key, value)");
    return Ok(L, S->api->set_setting(S->self, section.c_str(), key.c_str(), value.c_str()));
}

// modules (revision 2): lists need no permission; PropGet / PropSet / ActionInvoke / Subscribe / Command follow the grants
int FnModuleList(lua_State* L) {
    NEED_SCRIPT(); std::string text;
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->module_list(b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    PushRows(L, text, {"name", "label", "thread", "props", "actions", "events", "description"}, {"props", "actions", "events"});
    return 1;
}
using MemberFn = std::function<PbzResult(const PbzApi*, const char*, char*, int32_t, int32_t*)>;
int MemberList(lua_State* L, const MemberFn& list, const std::vector<const char*>& names, const std::vector<const char*>& numbers, const std::vector<const char*>& flags) {
    NEED_SCRIPT(); std::string module, text;
    if (!ArgStr(L, 1, module)) return Fail(L, PBZ_E_ARG);
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return list(S->api, module.c_str(), b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    PushRows(L, text, names, numbers, flags);
    return 1;
}
int FnPropList(lua_State* L) {
    return MemberList(L, [](const PbzApi* a, const char* m, char* b, int32_t n, int32_t* needed) { return a->prop_list(m, b, n, needed); },
                      {"name", "type", "access", "min", "max", "default", "persist", "thread", "label", "options", "note"}, {"min", "max", "default"}, {});
}
int FnActionList(lua_State* L) {
    return MemberList(L, [](const PbzApi* a, const char* m, char* b, int32_t n, int32_t* needed) { return a->action_list(m, b, n, needed); },
                      {"name", "thread", "persistent", "args", "label", "note"}, {}, {"persistent"});
}
int FnEventList(lua_State* L) {
    return MemberList(L, [](const PbzApi* a, const char* m, char* b, int32_t n, int32_t* needed) { return a->event_list(m, b, n, needed); }, {"name", "label", "format"}, {}, {});
}
int FnCommandList(lua_State* L) {
    NEED_SCRIPT(); std::string text;
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->command_list(b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    PushRows(L, text, {"name", "thread", "kind", "args", "note"});
    return 1;
}
int FnPropGet(lua_State* L) {  // the value as bool / integer / number / string, by the property's type
    NEED_SCRIPT(); std::string module, prop, text;
    if (!ArgStr(L, 1, module) || !ArgStr(L, 2, prop)) return Fail(L, PBZ_E_ARG, "PropGet(module, prop)");
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->prop_get(S->self, module.c_str(), prop.c_str(), b, n, needed); }, text);
    if (r != PBZ_OK) return Fail(L, r);
    ModRegistry::PropInfo info;
    if (ModRegistry::FindProp(module, prop, info) != PBZ_OK) { lua_pushlstring(L, text.data(), text.size()); return 1; }
    switch (info.type) {
    case ModRegistry::Type::Bool: lua_pushboolean(L, text == "1"); break;
    case ModRegistry::Type::Int: case ModRegistry::Type::Enum: lua_pushinteger(L, static_cast<lua_Integer>(std::strtoll(text.c_str(), nullptr, 10))); break;
    case ModRegistry::Type::Float: lua_pushnumber(L, std::strtod(text.c_str(), nullptr)); break;
    default: lua_pushlstring(L, text.data(), text.size()); break;
    }
    return 1;
}
int FnPropSet(lua_State* L) {
    NEED_SCRIPT(); std::string module, prop, value;
    if (!ArgStr(L, 1, module) || !ArgStr(L, 2, prop) || !ArgValue(L, 3, value)) return Fail(L, PBZ_E_ARG, "PropSet(module, prop, value)");
    return Ok(L, S->api->prop_set(S->self, module.c_str(), prop.c_str(), value.c_str()));
}
// ActionInvoke(module, action [, argument [, later]]) -> the action's text. later (true): queued, the answer comes as the host / action_done event.
int FnActionInvoke(lua_State* L) {
    NEED_SCRIPT(); std::string module, action, arg, text;
    if (!ArgStr(L, 1, module) || !ArgStr(L, 2, action)) return Fail(L, PBZ_E_ARG, "ActionInvoke(module, action [, argument [, later]])");
    if (!lua_isnoneornil(L, 3) && !ArgValue(L, 3, arg)) return Fail(L, PBZ_E_ARG);
    const bool later = lua_toboolean(L, 4) != 0;
    PbzResult r = PBZ_OK;
    if (later) r = S->api->action_invoke(S->self, module.c_str(), action.c_str(), arg.c_str(), PBZ_CALL_LATER, nullptr, 0, nullptr);
    else r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->action_invoke(S->self, module.c_str(), action.c_str(), arg.c_str(), 0, b, n, needed); }, text, true);
    if (r != PBZ_OK) return Fail(L, r, r == PBZ_E_STATE && !text.empty() ? text.c_str() : nullptr);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}
int FnModuleSubscribe(lua_State* L) {  // Subscribe(module [, event [, on]]): nil module / event: all
    NEED_SCRIPT(); std::string module = "*", event = "*";
    if (!lua_isnoneornil(L, 1) && !ArgStr(L, 1, module)) return Fail(L, PBZ_E_ARG);
    if (!lua_isnoneornil(L, 2) && !ArgStr(L, 2, event)) return Fail(L, PBZ_E_ARG);
    const int32_t enable = lua_isnone(L, 3) || lua_toboolean(L, 3) ? 1 : 0;
    return Ok(L, S->api->module_subscribe(S->self, module.c_str(), event.c_str(), enable));
}
// Command(line [, later]) -> the command's text (a panel-thread command is always queued: its answer is the host / command_done event)
int FnCommand(lua_State* L) {
    NEED_SCRIPT(); std::string line, text;
    if (!ArgStr(L, 1, line)) return Fail(L, PBZ_E_ARG, "Command(line [, later])");
    const bool later = lua_toboolean(L, 2) != 0;
    const PbzResult r = ReadText([&](char* b, int32_t n, int32_t* needed) { return S->api->command(S->self, line.c_str(), later ? PBZ_CALL_LATER : 0, b, n, needed); }, text, true);
    if (r != PBZ_OK) return Fail(L, r, r == PBZ_E_STATE && !text.empty() ? text.c_str() : nullptr);
    lua_pushlstring(L, text.data(), text.size());
    return 1;
}

// UI pages: UiPage{ id =, title =, items = { { kind = "toggle" | "slider" | "choice" | "button" | "label" | "text", id =, label =, text =, bind =, min =, max =, step =, value = }, ... } }
int UiKind(lua_State* L, int index) {
    if (lua_type(L, index) == LUA_TNUMBER) return static_cast<int>(lua_tointeger(L, index));
    std::string name; ArgStr(L, index, name);
    static const struct { const char* name; int kind; } kNames[] = {{"label", PBZ_UI_LABEL}, {"toggle", PBZ_UI_TOGGLE}, {"slider", PBZ_UI_SLIDER}, {"button", PBZ_UI_BUTTON}, {"choice", PBZ_UI_CHOICE}, {"text", PBZ_UI_TEXT}};
    for (const auto& k : kNames) if (name == k.name) return k.kind;
    return -1;
}
int FnUiPage(lua_State* L) {
    NEED_SCRIPT();
    if (!lua_istable(L, 1)) return Fail(L, PBZ_E_ARG, "UiPage{ id =, title =, items = {...} }");
    const std::string id = FieldStr(L, 1, "id"), title = FieldStr(L, 1, "title");
    lua_getfield(L, 1, "items");
    if (!lua_istable(L, -1)) return Fail(L, PBZ_E_ARG, "items = { ... }");
    const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
    if (n < 1 || n > 64) return Fail(L, PBZ_E_ARG, "1 to 64 items");
    std::deque<std::string> keep; std::vector<PbzUiItem> items(static_cast<size_t>(n)); std::vector<const PbzUiItem*> pointers;
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, -1, i);
        if (!lua_istable(L, -1)) return Fail(L, PBZ_E_ARG, "every item is a table");
        const int t = lua_gettop(L);
        PbzUiItem& it = items[static_cast<size_t>(i - 1)]; it = PbzUiItem{}; it.size = sizeof(it);
        lua_getfield(L, t, "kind"); it.kind = UiKind(L, lua_gettop(L)); lua_pop(L, 1);
        const auto text = [&](const char* key) -> const char* { keep.push_back(FieldStr(L, t, key)); return keep.back().c_str(); };
        it.id = text("id"); it.label = text("label"); it.text = text("text"); const char* bind = text("bind"); it.bind = *bind ? bind : nullptr;
        it.min = FieldNum(L, t, "min", 0); it.max = FieldNum(L, t, "max", 0); it.step = FieldNum(L, t, "step", 0); it.value = FieldNum(L, t, "value", 0);
        pointers.push_back(&it);
        lua_pop(L, 1);
    }
    PbzUiPage page{}; page.size = sizeof(page); page.id = id.c_str(); page.title = title.c_str(); page.items = pointers.data(); page.item_count = static_cast<int32_t>(n);
    return Ok(L, S->api->ui_page(S->self, &page));
}
int FnUiRemovePage(lua_State* L) { NEED_SCRIPT(); std::string id; if (!ArgStr(L, 1, id)) return Fail(L, PBZ_E_ARG); return Ok(L, S->api->ui_remove_page(S->self, id.c_str())); }
int FnUiSetValue(lua_State* L) {
    NEED_SCRIPT(); std::string page, item;
    if (!ArgStr(L, 1, page) || !ArgStr(L, 2, item)) return Fail(L, PBZ_E_ARG);
    double v = 0;
    if (lua_type(L, 3) == LUA_TNUMBER) v = static_cast<double>(lua_tonumber(L, 3)); else if (lua_type(L, 3) == LUA_TBOOLEAN) v = lua_toboolean(L, 3) ? 1 : 0; else return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->ui_set_value(S->self, page.c_str(), item.c_str(), v));
}
int FnUiSetText(lua_State* L) {
    NEED_SCRIPT(); std::string page, item, text;
    if (!ArgStr(L, 1, page) || !ArgStr(L, 2, item) || !ArgStr(L, 3, text)) return Fail(L, PBZ_E_ARG);
    return Ok(L, S->api->ui_set_text(S->self, page.c_str(), item.c_str(), text.c_str()));
}

void SetConstants(lua_State* L, const char* name, const std::vector<std::pair<const char*, lua_Integer>>& values) {
    lua_createtable(L, 0, static_cast<int>(values.size()));
    for (const auto& [key, v] : values) SetInt(L, key, v);
    lua_setfield(L, -2, name);
}
void OpenPbz(lua_State* L) {
    static const luaL_Reg kFunctions[] = {
        {"Init", FnInit}, {"Describe", FnInit}, {"Id", FnId}, {"Available", FnAvailable}, {"Started", FnStarted}, {"Version", FnVersion}, {"Revision", FnRevision},
        {"HasFeature", FnHasFeature}, {"Caps", FnCaps}, {"Log", FnLog}, {"Notify", FnNotify}, {"Enabled", FnEnabled}, {"Now", FnNow},
        {"RegisterSignal", FnRegisterSignal}, {"UnregisterSignal", FnUnregisterSignal}, {"SignalCode", FnSignalCode}, {"FireSignal", FnFireSignal}, {"SetSignal", FnSetSignal},
        {"SignalWanted", FnSignalWanted}, {"RegisterAction", FnRegisterAction}, {"UnregisterAction", FnUnregisterAction},
        {"OnEvent", FnOnEvent}, {"OnHeroAbility", FnOnHeroAbility}, {"OnTick", FnOnTick}, {"OnEnabled", FnOnEnabled}, {"OnStart", FnOnStart},
        {"OnModuleEvent", FnOnModuleEvent}, {"OnUi", FnOnUi}, {"OnCaps", FnOnCaps},
        {"GetState", FnGetState}, {"NodeCount", FnNodeCount}, {"GetNode", FnGetNode}, {"GetRoute", FnGetRoute}, {"GetHero", FnGetHero}, {"WeaponMoves", FnWeaponMoves},
        {"Press", FnPress}, {"PlayNode", FnPlayNode}, {"ResetChain", FnResetChain}, {"CancelHeroAbility", FnCancelHeroAbility},
        {"ListTables", FnListTables}, {"ExportTable", FnExportTable}, {"ImportTable", FnImportTable},
        {"EditBegin", FnEditBegin}, {"EditCommit", FnEditCommit}, {"EditCancel", FnEditCancel}, {"EditNodeCount", FnEditNodeCount}, {"EditGetNode", FnEditGetNode},
        {"EditGetRoute", FnEditGetRoute}, {"EditAddNode", FnEditAddNode}, {"EditRemoveNode", FnEditRemoveNode}, {"EditGetField", FnEditGetField}, {"EditSetField", FnEditSetField},
        {"EditAddRoute", FnEditAddRoute}, {"EditRemoveRoute", FnEditRemoveRoute}, {"EditLink", FnEditLink}, {"EditLinkModifier", FnEditLinkModifier},
        {"EditUnlinkModifier", FnEditUnlinkModifier}, {"EditExport", FnEditExport}, {"EditImport", FnEditImport}, {"FieldNames", FnFieldNames},
        {"GetOption", FnGetOption}, {"SetOption", FnSetOption}, {"GetSetting", FnGetSetting}, {"SetSetting", FnSetSetting},
        {"ModuleList", FnModuleList}, {"PropList", FnPropList}, {"ActionList", FnActionList}, {"EventList", FnEventList}, {"CommandList", FnCommandList},
        {"PropGet", FnPropGet}, {"PropSet", FnPropSet}, {"ActionInvoke", FnActionInvoke}, {"Subscribe", FnModuleSubscribe}, {"Command", FnCommand},
        {"UiPage", FnUiPage}, {"UiRemovePage", FnUiRemovePage}, {"UiSetValue", FnUiSetValue}, {"UiSetText", FnUiSetText},
        {nullptr, nullptr}};
    lua_newtable(L);
    luaL_setfuncs(L, kFunctions, 0);
    SetInt(L, "PASS", PBZ_PASS); SetInt(L, "CONSUME", PBZ_CONSUME);
    SetConstants(L, "BUTTON", {{"NPC_KEY", PBZ_BUTTON_NPC_KEY}, {"LIGHT", PBZ_BUTTON_LIGHT}, {"HEAVY", PBZ_BUTTON_HEAVY}, {"SHACHI", PBZ_BUTTON_SHACHI}, {"SPECIAL", PBZ_BUTTON_SPECIAL},
        {"COMBAT_ART", PBZ_BUTTON_COMBAT_ART}, {"JUMP", PBZ_BUTTON_JUMP}, {"DASH", PBZ_BUTTON_DASH}, {"CONTINUOUS_DASH", PBZ_BUTTON_CONTINUOUS_DASH},
        {"PERFECT_DODGE", PBZ_BUTTON_PERFECT_DODGE}, {"DEFLECT_DASH", PBZ_BUTTON_DEFLECT_DASH}, {"SPECIAL_DASH", PBZ_BUTTON_SPECIAL_DASH}, {"ANY_DODGE", PBZ_BUTTON_ANY_DODGE},
        {"EXECUTION", PBZ_BUTTON_EXECUTION}, {"COMBO_KEYS", PBZ_BUTTON_COMBO_KEYS}, {"KEYS", PBZ_BUTTON_KEYS}, {"STANCE", PBZ_BUTTON_STANCE}, {"SIMPLE_EXECUTION", PBZ_BUTTON_SIMPLE_EXECUTION}});
    SetConstants(L, "NODE", {{"SKILL", PBZ_NODE_SKILL}, {"INTERVAL", PBZ_NODE_INTERVAL}, {"TRIGGER", PBZ_NODE_TRIGGER}, {"AUTO_DODGE", PBZ_NODE_AUTO_DODGE}, {"BRANCH", PBZ_NODE_BRANCH},
        {"SPEED", PBZ_NODE_SPEED}, {"WEAPON", PBZ_NODE_WEAPON}, {"ARMOR", PBZ_NODE_ARMOR}, {"INTERRUPT", PBZ_NODE_INTERRUPT}, {"GRAB", PBZ_NODE_GRAB}, {"BOSS_SKILL", PBZ_NODE_BOSS_SKILL},
        {"WEAPON_MOVE", PBZ_NODE_WEAPON_MOVE}, {"DAMAGE", PBZ_NODE_DAMAGE}, {"GROUP", PBZ_NODE_GROUP}, {"TELEPORT", PBZ_NODE_TELEPORT}, {"INVINCIBLE", PBZ_NODE_INVINCIBLE},
        {"LOOK", PBZ_NODE_LOOK}, {"HERO_LOOK", PBZ_NODE_HERO_LOOK}, {"KEEP_LOOK", PBZ_NODE_KEEP_LOOK}, {"FORCE_HIT", PBZ_NODE_FORCE_HIT}, {"PRE_INPUT", PBZ_NODE_PRE_INPUT},
        {"WEAPON_ACTIONS", PBZ_NODE_WEAPON_ACTIONS}, {"IGNORE_GUARD", PBZ_NODE_IGNORE_GUARD}, {"KEEP_CHAIN", PBZ_NODE_KEEP_CHAIN}, {"ASSIST", PBZ_NODE_ASSIST}, {"HOOK", PBZ_NODE_HOOK},
        {"TRIM", PBZ_NODE_TRIM}, {"AIR", PBZ_NODE_AIR}, {"AIR_DEFAULT", PBZ_NODE_AIR_DEFAULT}, {"BONE", PBZ_NODE_BONE}, {"BOSS_EFFECT", PBZ_NODE_BOSS_EFFECT}, {"PLUGIN", PBZ_NODE_PLUGIN},
        {"HERO_SIZE", PBZ_NODE_HERO_SIZE}});
    SetConstants(L, "EVENT", {{"CHAIN_START", PBZ_EVENT_CHAIN_START}, {"PRESS", PBZ_EVENT_PRESS}, {"NODE_PLAY", PBZ_EVENT_NODE_PLAY}, {"MOVE_END", PBZ_EVENT_MOVE_END},
        {"CHAIN_RESET", PBZ_EVENT_CHAIN_RESET}, {"LOOK", PBZ_EVENT_LOOK}, {"TABLES", PBZ_EVENT_TABLES}});
    SetConstants(L, "ACTION", {{"RUN", PBZ_ACTION_RUN}, {"DECIDE", PBZ_ACTION_DECIDE}});
    SetConstants(L, "PHASE", {{"IDLE", PBZ_PHASE_IDLE}, {"STARTING", PBZ_PHASE_STARTING}, {"RUNNING", PBZ_PHASE_RUNNING}});
    SetConstants(L, "RESULT", {{"OK", PBZ_OK}, {"ARG", PBZ_E_ARG}, {"STATE", PBZ_E_STATE}, {"THREAD", PBZ_E_THREAD}, {"NOT_FOUND", PBZ_E_NOT_FOUND}, {"FULL", PBZ_E_FULL},
        {"EXISTS", PBZ_E_EXISTS}, {"BUFFER", PBZ_E_BUFFER}, {"DISABLED", PBZ_E_DISABLED}, {"VERSION", PBZ_E_VERSION}, {"DENIED", PBZ_E_DENIED}});
    SetConstants(L, "CAP", {{"COMBO", PBZ_CAP_COMBO}, {"READ", PBZ_CAP_READ}, {"WRITE", PBZ_CAP_WRITE}, {"INI", PBZ_CAP_INI}, {"COMMAND", PBZ_CAP_COMMAND}, {"UI", PBZ_CAP_UI}});
    SetConstants(L, "UI", {{"LABEL", PBZ_UI_LABEL}, {"TOGGLE", PBZ_UI_TOGGLE}, {"SLIDER", PBZ_UI_SLIDER}, {"BUTTON", PBZ_UI_BUTTON}, {"CHOICE", PBZ_UI_CHOICE}, {"TEXT", PBZ_UI_TEXT}});
    lua_pushvalue(L, -1); lua_setglobal(L, "PBZ");  // the UE4SS fork's name
    lua_setglobal(L, "pbz");
}

// ---- one script ---------------------------------------------------------------------------------------------------------------
void CloseScript(Script& s) {
    if (s.L) {
        // lua_close frees everything; the plugin host has already dropped the actions that point into this script (RetireScripts / reset).
        __try { lua_close(s.L); } __except (EXCEPTION_EXECUTE_HANDLER) {}
        s.L = nullptr;
    }
    s.actions.clear();
    s.dead = true;
}
bool ReadFile(const std::wstring& path, std::string& out, std::string& error) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) { error = "cannot open the file"; return false; }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    if (size < 0 || static_cast<size_t>(size) > kMaxFileBytes) { error = "the file is over 1 MB"; return false; }
    in.seekg(0);
    out.assign(static_cast<size_t>(size), '\0');
    in.read(out.data(), size);
    if (out.size() >= 3 && static_cast<unsigned char>(out[0]) == 0xEF && static_cast<unsigned char>(out[1]) == 0xBB && static_cast<unsigned char>(out[2]) == 0xBF) out.erase(0, 3);
    return true;
}
int OpenMain(lua_State* L) { OpenLibs(L); return 0; }
bool Open(Script& s, std::string& error) {
    lua_State* L = lua_newstate(Alloc, &s);
    if (!L) { error = "not enough memory for a Lua state"; return false; }
    s.L = L;
    *static_cast<Script**>(lua_getextraspace(L)) = &s;
    lua_atpanic(L, Panic);
    lua_pushcfunction(L, OpenMain);
    if (lua_pcall(L, 0, 0, 0) != LUA_OK) { const char* m = lua_tostring(L, -1); error = m ? m : "cannot open the libraries"; return false; }
    lua_sethook(L, Hook, LUA_MASKCOUNT, kHookCount);
    return true;
}
// The plugin entry of a script (the host calls it from StartScript on the game thread; g_loading says which script).
PbzResult PBZ_CALL ScriptMain(const PbzApi* api, PbzPlugin* self) {
    Script* s = g_loading;
    if (!s) return PBZ_E_STATE;
    s->self = self; s->api = api;
    PbzPluginInfoEx info{}; info.size = sizeof(info);
    info.id = s->id.c_str(); info.name = s->stem.c_str(); info.version = ""; info.author = ""; info.description = "Lua"; info.min_api = 1; info.caps = PBZ_CAP_COMBO;
    if (api->describe_ex(self, &info) != PBZ_OK) { ComboPlugins::SetScriptStatus(self, "the plugin host refused this script's id"); return PBZ_E_STATE; }
    std::string source, error;
    if (!ReadFile(s->path, source, error)) { ComboPlugins::SetScriptStatus(self, error); return PBZ_E_STATE; }
    if (!Open(*s, error)) { ComboPlugins::SetScriptStatus(self, error); CloseScript(*s); return PBZ_E_STATE; }
    lua_State* L = s->L;
    const std::string chunk = "@" + Utf8(s->file);
    if (luaL_loadbufferx(L, source.data(), source.size(), chunk.c_str(), "t") != LUA_OK) {
        const char* m = lua_tostring(L, -1);
        error = m ? m : "syntax error";
        ComboPlugins::SetScriptStatus(self, error); ComboPlugins::LogHost("[" + s->id + "] " + error);
        CloseScript(*s); return PBZ_E_STATE;
    }
    s->loading = true; s->depth = 1; s->budget = kLoadBudget; s->exceeded = false;
    PcallArgs args{L, 0, LUA_OK};
    DWORD code = 0;
    const bool survived = GuardedPcall(args, code);  // the chunk is on the stack: the same SEH guard as every call
    s->depth = 0;
    if (!survived || args.status != LUA_OK) {
        const char* m = survived ? lua_tostring(L, -1) : nullptr;
        error = survived ? (m ? m : "(error object is not a string)") : "access violation in the interpreter";
        ComboPlugins::SetScriptStatus(self, error); ComboPlugins::LogHost("[" + s->id + "] " + error);
        s->loading = false; CloseScript(*s); return PBZ_E_STATE;
    }
    if (s->on_start != LUA_NOREF) {  // PBZ.OnStart(fn): right after the top level, still in the entry
        int top = 0;
        s->depth = 0;
        if (Call(*s, s->on_start, "on_start", [](lua_State*) { return 0; }, 0, top)) Done(*s, top);
        if (s->dead) { s->loading = false; return PBZ_E_STATE; }
    }
    s->loading = false;
    Sync(*s);
    s->ok = true;
    return PBZ_OK;
}
PbzResult PBZ_CALL RefuseMain(const PbzApi*, PbzPlugin* self) {
    ComboPlugins::SetScriptStatus(self, "the file name may only have letters, digits, _ . - (at most 44 bytes)");
    return PBZ_E_STATE;
}
bool ValidStem(const std::string& stem) { return !stem.empty() && stem.size() <= 44 && PluginText::IdText(stem, 44); }
}  // namespace

void Load() {
    g_game_thread = GetCurrentThreadId();
    const std::wstring folder = ComboPlugins::ScriptFolder();
    std::vector<std::wstring> files;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((folder + L"\\*.lua").c_str(), &found);
    if (find != INVALID_HANDLE_VALUE) {
        do { if (!(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) files.push_back(found.cFileName); } while (FindNextFileW(find, &found) && files.size() < kMaxFiles);
        FindClose(find);
    }
    std::sort(files.begin(), files.end());
    ComboPlugins::LogHost("scripts folder " + Utf8(folder) + ": " + std::to_string(files.size()) + " file(s)");
    for (const std::wstring& name : files) {
        auto script = std::make_unique<Script>();
        script->file = name; script->path = folder + L"\\" + name;
        script->stem = Utf8(name.substr(0, name.size() - 4));  // ".lua"
        const bool valid = ValidStem(script->stem);
        if (valid) script->id = "lua." + script->stem;
        g_loading = script.get();
        PbzPlugin* plugin = ComboPlugins::StartScript(valid ? ScriptMain : RefuseMain, name);
        g_loading = nullptr;
        if (plugin && script->ok) g_scripts.push_back(std::move(script));
        else CloseScript(*script);  // not in use: its state goes at once (the plugin record stays in the list with the reason)
    }
}
void Unload() {
    for (auto& s : g_scripts) CloseScript(*s);
    g_scripts.clear();
}
void Install() {
    static const ComboPlugins::ScriptHost host{&Load, &Unload};
    ComboPlugins::SetScriptHost(&host);
}
}  // namespace LuaHost
