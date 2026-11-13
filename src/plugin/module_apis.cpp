#include "module_apis.h"
#include "lua_host.h"
#include "mod_registry.h"

namespace ModuleApis {
void RegisterAll(Logger logger, HairControls* hair) {
    if (logger) ModRegistry::SetLog(logger);
    RegisterCombo(); RegisterNpcLook(); RegisterCharacter(); RegisterBody();
    RegisterBossRush(); RegisterBossAi(); RegisterAir(); RegisterGrab(); RegisterWeapons();
    RegisterPad(); RegisterHair(hair); RegisterHub();
    LuaHost::Install();  // Lua scripts (the PBZPlugins\scripts folder): read once 派生插件 is on
}
}  // namespace ModuleApis
