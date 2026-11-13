#pragma once
// Lua scripts for the plugin API (interface revision 3; docs/PLUGIN_API.md "Lua 脚本"). Every *.lua in PBZPlugins\scripts\ is a plugin
// of its own in the same list as the DLLs: id "lua.<file name>", the same grants ([PluginGrants]), started and called on the game thread
// only, only while 派生插件 is on. A script sees a global table `pbz` that mirrors the C API (and `PBZ`, the same table, so scripts of
// the UE4SS fork's PBZ table port over). Lua 5.4.7 is compiled in without io / os / package / debug; a call gets an instruction budget
// and a script a memory cap; an error in a script switches that script off (one log line, its status in the plugin list).
namespace LuaHost {
// Registers the script host with the plugin host (ComboPlugins::SetScriptHost). Call once at start; the scripts are read at the first
// frame 派生插件 is on, and again on ComboPlugins::RequestScriptReload.
void Install();
// Game thread: the plugin host's own calls (Install is all a caller needs).
void Load();
void Unload();
}
