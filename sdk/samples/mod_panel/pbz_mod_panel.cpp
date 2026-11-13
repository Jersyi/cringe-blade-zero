/*
 * pbz_mod_panel — 示例 3：用「模块」接口读写整个 MOD（接口修订 2）
 *
 * 做什么：
 *   1. 先看 MOD 够不够新（api_revision、has_feature），不够新就只写一行日志，不出错；
 *   2. 用 describe_ex 向玩家申请能力：读、写、命令、UI 页（玩家在「派生插件」里逐项打开，没打开前对应的调用返回 PBZ_E_DENIED）；
 *   3. 玩家授权后（on_caps）画一页：一行显示 MOD 版本，「派生提示」开关、「派生显示」选项、一个「还原主角」按钮
 *      ——都是绑定到模块属性 / 动作的，宿主自己读写，插件不写一行事件代码；另有一个「列出模块」按钮和一块状态文字
 *      由插件自己处理（on_ui）；
 *   4. 订阅外观套用、角色替换、武器切换的模块事件，把最近一次写在状态文字里；
 *   5. 按钮「面板状态」用命令总线问面板线程（hub.status），答案以 host / command_done 事件回来。
 *
 * 页上只放这个 MOD 里真有的模块属性：先用 prop_list 看过再放（旧 MOD、精简版没有的就不放）。
 */
#include "pbz_combo_api.h"
#include <cstdio>
#include <cstring>
#include <string>

namespace {
const PbzApi* api = nullptr;
PbzPlugin* self = nullptr;
bool page_up = false;
std::string last_event = "(还没有事件)";

void Say(const std::string& text) { if (api && self) api->log(self, text.c_str()); }

bool HasProp(const char* module, const char* prop) {
    char text[4096]{}; int32_t needed = 0;
    if (!PBZ_API_HAS(api, prop_list) || api->prop_list(module, text, sizeof(text), &needed) != PBZ_OK) return false;
    const std::string rows = std::string("\n") + text;  // 每行第一列是属性名
    return rows.find(std::string("\n") + prop + "\t") != std::string::npos;
}
bool HasAction(const char* module, const char* action) {
    char text[4096]{}; int32_t needed = 0;
    if (!PBZ_API_HAS(api, action_list) || api->action_list(module, text, sizeof(text), &needed) != PBZ_OK) return false;
    return (std::string("\n") + text).find(std::string("\n") + action + "\t") != std::string::npos;
}

// 把模块和它们的属性写进日志（「列出模块」按钮）。
void DumpModules() {
    char modules[8192]{}; int32_t needed = 0;
    if (api->module_list(modules, sizeof(modules), &needed) != PBZ_OK) { Say("module_list: buffer too small"); return; }
    std::string all = modules; size_t at = 0;
    while (at < all.size()) {
        const size_t nl = all.find('\n', at); const std::string row = all.substr(at, nl == std::string::npos ? std::string::npos : nl - at);
        at = nl == std::string::npos ? all.size() : nl + 1;
        const size_t tab = row.find('\t'); const std::string name = row.substr(0, tab);
        Say("module " + name + ": " + row);
        char props[16384]{};
        if (api->prop_list(name.c_str(), props, sizeof(props), &needed) == PBZ_OK) Say(std::string("  props:\n") + props);
    }
}

void ShowText(const char* item, const std::string& text) { api->ui_set_text(self, "main", item, text.c_str()); }

void TryPage() {
    if (page_up || !PBZ_API_HAS(api, ui_page)) return;
    if (!(api->granted_caps(self) & PBZ_CAP_UI)) return;  // 玩家还没开 UI 页：等 on_caps
    static PbzUiItem items[8]; static const PbzUiItem* list[8]; int n = 0;
    const auto add = [&](int kind, const char* id, const char* label) -> PbzUiItem& {
        items[n] = PbzUiItem{}; items[n].size = sizeof(PbzUiItem); items[n].kind = kind; items[n].id = id; items[n].label = label; list[n] = &items[n]; return items[n++];
    };
    if (HasProp("host", "mod_version")) add(PBZ_UI_LABEL, "version", "MOD 版本").bind = "host.mod_version";
    if (HasProp("combo", "show_toast")) add(PBZ_UI_TOGGLE, "toast", "派生提示").bind = "combo.show_toast";
    if (HasProp("combo", "chain_hud")) add(PBZ_UI_CHOICE, "hud", "派生显示").bind = "combo.chain_hud";
    if (HasAction("npc_look", "restore")) add(PBZ_UI_BUTTON, "restore", "还原主角").bind = "npc_look:restore";
    add(PBZ_UI_BUTTON, "dump", "列出模块（写日志）");
    if (PBZ_API_HAS(api, command)) add(PBZ_UI_BUTTON, "hub", "面板状态");
    add(PBZ_UI_TEXT, "info", "").text = last_event.c_str();
    PbzUiPage page{}; page.size = sizeof(page); page.id = "main"; page.title = "模块面板（示例）"; page.items = list; page.item_count = n;
    const PbzResult r = api->ui_page(self, &page);
    if (r == PBZ_OK) page_up = true;
    else Say("ui_page refused: " + std::to_string(r));
}

// 订阅模块事件：需要读权限（没有时返回 PBZ_E_DENIED）；重复订阅没有害处，所以入口和授权变化时都调。
void Subscribe() {
    api->module_subscribe(self, "npc_look", "look_changed", 1);
    api->module_subscribe(self, "character", "swap_changed", 1);
    api->module_subscribe(self, "weapons", "weapon_changed", 1);
}
void PBZ_CALL OnCaps(void*, uint32_t granted) {
    char text[64]{}; std::snprintf(text, sizeof(text), "the player now allows caps 0x%x", static_cast<unsigned>(granted)); Say(text);
    Subscribe();
    TryPage();  // 授权可能是刚打开的
}
void PBZ_CALL OnUi(void*, const PbzUiEvent* e) {
    if (!std::strcmp(e->item, "dump")) DumpModules();
    else if (!std::strcmp(e->item, "hub")) {
        // 面板的命令属于面板线程：排队，答案以 host / command_done 回来
        char reply[64]{}; int32_t needed = 0;
        const PbzResult r = api->command(self, "hub.status", PBZ_CALL_LATER, reply, sizeof(reply), &needed);
        if (r != PBZ_OK) ShowText("info", r == PBZ_E_DENIED ? "没有「命令」权限" : "命令失败 " + std::to_string(r));
    }
}
void PBZ_CALL OnModuleEvent(void*, const PbzModuleEvent* e) {
    last_event = std::string(e->module) + "/" + e->event + ": " + e->text;
    if (page_up) ShowText("info", last_event);
}
}  // namespace

PBZ_PLUGIN_EXPORT PbzResult PBZ_CALL PbzPluginMain(const PbzApi* a, PbzPlugin* s) {
    api = a; self = s;
    if (!a || a->version < 1) return PBZ_E_VERSION;
    // 修订 1 的 MOD 没有 describe_ex：用 describe 只做能做的（这里就是什么都不做）。
    if (!PBZ_API_HAS(a, describe_ex) || PBZ_API_REVISION_OF(a) < 2) {
        PbzPluginInfo old{}; old.size = sizeof(old); old.id = "pbz.mod_panel"; old.name = "模块面板（示例）"; old.version = "1.0"; old.author = "豪之刃"; old.min_api = 1;
        old.description = "需要更新版本的 MOD 才有模块接口";
        if (a->describe(s, &old) != PBZ_OK) return PBZ_E_STATE;
        a->log(s, "this MOD has no module API (api_revision < 2): nothing to do");
        return PBZ_OK;
    }
    PbzPluginInfoEx info{}; info.size = sizeof(info);
    info.id = "pbz.mod_panel"; info.name = "模块面板（示例）"; info.version = "1.0"; info.author = "豪之刃";
    info.description = "示例：用模块接口读写整个 MOD，并在面板上画一页"; info.min_api = 1; info.min_revision = 2;
    info.caps = PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_COMMAND | PBZ_CAP_UI;
    info.caps_reason = "读写模块属性、订阅事件、在面板上显示一页";
    if (a->describe_ex(s, &info) != PBZ_OK) return PBZ_E_STATE;
    PbzCallbacks cb{}; cb.size = sizeof(cb); cb.on_caps = OnCaps; cb.on_ui = OnUi; cb.on_module_event = OnModuleEvent;
    if (a->set_callbacks(s, &cb) != PBZ_OK) return PBZ_E_STATE;
    Subscribe();
    TryPage();
    return PBZ_OK;
}
