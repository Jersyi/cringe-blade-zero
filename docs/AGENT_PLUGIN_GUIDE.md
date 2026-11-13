# 豪之刃插件与 Lua 脚本：给 Agent 的使用指南

适用于接口修订 3（2026-11-13）。这份文档讲**怎么做**：写一个插件或脚本、装上、打开、授权、确认它在工作。每个函数的完整说明在 [PLUGIN_API.md](PLUGIN_API.md)，类型和注释在 `sdk/include/pbz_combo_api.h`（安装包里是 `SDK/include/`）。两者有出入时，以头文件为准。

如果你的任务是**修改宿主**（给 MOD 加模块属性、加 API 函数），先看第 9 节。

## 1. 动手前的硬规则

1. **默认全关。** 没有 `PBZDebugMenu.ini` 时 `[Plugins] Enabled = 0`，MOD 不载入任何 DLL，也不运行任何脚本。要在面板（F10）「招式派生」页顶栏点「派生插件：关 ▾」，选「打开派生插件」。
2. **回调都在游戏线程上。** 回调里不要等锁、不要读写文件或网络，尽快返回。
3. **写入是排队的。** `prop_set`、`press`、`play_node`、`reset_chain`、`fire_signal` 都在下一帧执行，紧接着读到的还是旧值。
4. **只有派生能力是默认的。** 读写其他模块、命令、UI 页、写 ini，都要玩家给这个插件逐项授权。没授权的调用返回 `PBZ_E_DENIED`（-10），插件仍然载入和运行。
5. **字符串一律 UTF-8。** C 源码要用 `/utf-8` 编译。节点、行、出口、按键的序号从 0 开始，Lua 里也是。
6. **C 插件不是沙箱。** 它是游戏进程里的原生代码，权限只管它通过接口能做什么。Lua 脚本在沙箱里：没有 `io`、`package`、`debug`，`os` 只有几个无害的函数，而且有指令预算和内存上限。
7. **插件 id 和动作 id 发布后不要改。** 派生表把插件节点存成 `插件id/动作id`，信号的键码由 `插件id/信号id` 算出。

## 2. 选哪种写法

| 写法 | 放在哪 | 适合 | 能用的接口 |
| --- | --- | --- | --- |
| C / C++ DLL | `PBZPlugins\*.dll` | 要性能、要调系统函数、要自带线程 | 全部（修订 1~3） |
| Lua 脚本（内置，修订 3） | `PBZPlugins\scripts\*.lua` | 判断节点、信号、监听、模块读写、UI 页，不想编译 | 全局表 `pbz`，和 C 接口一一对应（见第 5 节） |
| UE4SS Lua（豪之刃适配版，单独发布） | `ue4ss\Mods\<名字>\scripts\main.lua` | 已经在用 UE4SS，要和 UE4SS 的游戏对象操作结合 | 全局表 `PBZ`，**只有修订 1 的函数** |
| 别的加载器载入的 DLL | 任意 | UE4SS 的 C++ mod 等 | 用 `PbzAttachPlugin`（`version.dll` 序号 1001）登记，之后同 C 插件 |

DLL 最多 32 个，Lua 脚本另算、最多 32 个；它们出现在同一个插件列表里，用同一套授权。

## 3. 游戏目录里的文件

```
<PBZ.exe 所在文件夹>\
  version.dll                 豪之刃本体（插件宿主）
  PBZDebugMenu.ini            设置；[Plugins] Enabled 开关，[PluginGrants] 授权，[Plugin.<id>] 插件自己的段
  PBZDebugMenu.log            日志；插件相关的行以 PLUGIN 开头
  PBZPlugins\
    pbz_conditions.dll        示例 1：条件判断节点
    pbz_weapon_actions.dll    示例 2：武器动作信号（规则在 pbz_weapon_actions.txt）
    pbz_mod_panel.dll         示例 3：模块面板（权限、UI 页、事件、命令）
    scripts\*.lua             Lua 脚本（修订 3）
```

Steam 版的 `PBZ.exe` 在 `<安装目录>\platform\steam\`。

## 4. C 插件：从零到能用

### 4.1 模板

下面这个插件申请读写权限，注册一个判断节点，授权后在插件列表里画一页，并订阅外观事件。它能直接编译。

```cpp
// my_plugin.cpp
#include "pbz_combo_api.h"
#include <cstdio>

static const PbzApi* api;
static PbzPlugin* self;

// 判断节点：目标在「数值」厘米以内走出口 0，更远走 1，没有目标走 2。
static int32_t PBZ_CALL Near(void*, const PbzNodeCall* call) {
    PbzHeroFacts h{}; h.size = sizeof(h);
    if (api->get_hero(&h) != PBZ_OK || !h.valid) return -1;      // -1：不走任何出口，按键交还游戏
    if (!h.has_target) return 2;
    return h.target_distance <= call->value ? 0 : 1;
}

static void DrawPage() {
    PbzUiItem toast{}; toast.size = sizeof(toast);
    toast.kind = PBZ_UI_TOGGLE; toast.id = "toast"; toast.label = "派生提示";
    toast.bind = "combo.show_toast";                              // 宿主自己读写这个模块属性
    PbzUiItem status{}; status.size = sizeof(status);
    status.kind = PBZ_UI_TEXT; status.id = "status"; status.label = "最近的事件"; status.text = "（还没有）";
    const PbzUiItem* items[] = {&toast, &status};
    PbzUiPage page{}; page.size = sizeof(page);
    page.id = "main"; page.title = "我的插件"; page.items = items; page.item_count = 2;
    api->ui_page(self, &page);                                    // 没有 PBZ_CAP_UI 时 PBZ_E_DENIED
}

static void PBZ_CALL OnCaps(void*, uint32_t granted) {           // 玩家改了授权（游戏线程，下一帧）
    if (granted & PBZ_CAP_UI) DrawPage();
    if (granted & PBZ_CAP_READ) api->module_subscribe(self, "npc_look", "look_changed", 1);
}

static void PBZ_CALL OnModuleEvent(void*, const PbzModuleEvent* e) {
    char line[256];
    std::snprintf(line, sizeof(line), "%s / %s：%s", e->module, e->event, e->text);
    api->ui_set_text(self, "main", "status", line);
}

PBZ_PLUGIN_EXPORT PbzResult PBZ_CALL PbzPluginMain(const PbzApi* a, PbzPlugin* s) {
    if (!a || a->version < 1) return PBZ_E_VERSION;
    api = a; self = s;
    if (PBZ_API_REVISION_OF(api) < 2) return PBZ_E_VERSION;      // 需要 describe_ex 和模块接口

    PbzPluginInfoEx info{};                                       // 入口里第一件事
    info.size = sizeof(info);
    info.id = "me.near"; info.name = "距离判断"; info.version = "1.0.0"; info.author = "me";
    info.description = "按目标距离分支，并显示外观事件"; info.min_api = 1; info.min_revision = 2;
    info.caps = PBZ_CAP_READ | PBZ_CAP_WRITE | PBZ_CAP_UI;
    info.caps_reason = "在自己的页上显示外观事件，并切换派生提示";
    if (PbzResult r = api->describe_ex(self, &info); r != PBZ_OK) return r;

    PbzCallbacks cb{}; cb.size = sizeof(cb);
    cb.on_caps = OnCaps; cb.on_module_event = OnModuleEvent;
    api->set_callbacks(self, &cb);

    static const char* const outs[] = {"近", "远", "没有目标"};
    PbzActionDesc d{}; d.size = sizeof(d);
    d.id = "near"; d.name = "目标在附近？"; d.kind = PBZ_ACTION_DECIDE;
    d.output_count = 3; d.outputs = outs; d.decide = Near;
    d.has_value = 1; d.value_label = "距离"; d.value_unit = "厘米";
    d.value_min = 50; d.value_max = 3000; d.value_step = 50; d.value_default = 400;
    if (PbzResult r = api->register_action(self, &d); r != PBZ_OK) return r;

    OnCaps(nullptr, api->granted_caps(self));                     // 已经授权过（ini 里有）就直接画页
    return PBZ_OK;
}
```

要点：

- `describe` / `describe_ex` 必须是入口里的第一个调用，否则插件不被使用。
- `decide` 只做判断，不要有副作用。派生链处理一次按键时可能问它好几次；同一帧里相同的「节点、按键、数值」只问一次。
- 回调里的字符串指针只在这次回调内有效，要留就复制。
- C++ 异常不要抛出插件边界。插件崩溃时宿主停用它（本次游戏内不再调用），游戏照常。

### 4.2 编译

```bat
cl /LD /O2 /EHsc /utf-8 /MT /std:c++17 my_plugin.cpp /I <SDK>\include /Fe:my_plugin.dll
```

或者照 `SDK\samples\CMakeLists.txt` 加一个目标：

```bat
cd SDK\samples
cmake -S . -B build -A x64
cmake --build build --config Release
```

必须是 x64 DLL。用 `/MT`（静态运行库），玩家不必另装运行库。

### 4.3 安装、打开、授权

1. 退出游戏，把 DLL 复制到 `PBZPlugins\`。插件只在「派生插件」打开后的第一帧载入；游戏运行中放进去的要下次启动才载入。
2. 进游戏，F10 →「招式派生」→ 顶栏「派生插件：关 ▾」→「打开派生插件」。开关存在 `[Plugins] Enabled`。
3. 同一个列表里，插件下面有它申请的每一项权限。逐项点开（存在 `[PluginGrants] me.near = read,write,ui`）。授权立即生效，插件收到 `on_caps`。
4. 插件的 UI 页在列表最下面，插件列表之后。

Agent 不能替玩家点授权。需要测试时，可以在游戏关着时把授权写进 ini（`[PluginGrants]` 段，值是逗号分隔的 `read write ini command ui`），这和玩家点出来的效果一样；先告诉用户你写了什么。

### 4.4 确认它在工作

看 `PBZDebugMenu.log`：

| 日志 | 意思 |
| --- | --- |
| `PLUGIN folder <路径>: N DLL(s)` | 找到了几个 DLL |
| `PLUGIN [me.near] loaded: 名字 版本 by 作者 (…) signals=0 actions=1` | 载入成功 |
| `PLUGIN <文件> not loaded: 原因 (result n)` / `LoadLibrary failed` / `has no PbzPluginMain` | 没载入：入口失败、不是 x64 DLL 或缺依赖、没有导出入口 |
| `PLUGIN [me.near] denied: ui_page needs ui (the player has not allowed it)` | 缺权限（同一个调用只记一次） |
| `PLUGIN [me.near] prop_set combo.show_toast=1` | 模块写入（失败时行尾有 `failed: 原因`） |
| `PLUGIN [me.near] fault 0x… in 回调名: switched off for this session` | 插件崩溃，本次游戏内停用（列表里显示「运行出错，本次游戏内已停用」） |
| `NPC_COMBO plugin node=…` | 派生链走到了插件节点 |

## 5. Lua 脚本（修订 3）

MOD 内置 Lua 5.4.7。`PBZPlugins\scripts\` 里的每个 `*.lua` 是一个插件，和 DLL 在同一个插件列表里，用同一套授权，回调也都在游戏线程上。脚本里的全局表是 `pbz`（同一个表也叫 `PBZ`，函数名沿用 UE4SS 适配版，那边的脚本基本能搬过来）。

### 5.1 模板

```lua
-- PBZPlugins\scripts\near.lua（存成 UTF-8）
pbz.Init{
    name = "距离判断（Lua）", version = "1.0", author = "me",
    description = "按目标距离分支，并显示外观事件",
    min_revision = 3,
    caps = { "read", "write", "ui" },                 -- 申请的能力；combo 总是有
    caps_reason = "在自己的页上显示外观事件，并切换派生提示",
}

-- 判断节点：返回出口序号（从 0 数）；true = 0，false = 1；nil 或超范围 = 不走
pbz.RegisterAction({
    id = "near", name = "目标在附近？", kind = "decide", outputs = { "近", "远", "没有目标" },
    value = { label = "距离", unit = "厘米", min = 50, max = 3000, step = 50, default = 400 },
}, function(call)
    local h = pbz.GetHero()
    if not h or not h.valid then return nil end
    if not h.has_target then return 2 end
    return (h.target_distance <= call.value) and 0 or 1
end)

local page_up = false
local function try_page()
    if page_up then return end
    local ok, code = pbz.UiPage{ id = "main", title = "距离判断", items = {
        { kind = "toggle", id = "toast", label = "派生提示", bind = "combo.show_toast" },
        { kind = "text", id = "status", text = "（还没有事件）" },
    } }
    if ok then page_up = true elseif code ~= pbz.RESULT.DENIED then pbz.Log("UiPage: " .. tostring(code)) end
end

pbz.OnCaps(function(mask, caps)                       -- 玩家改了授权
    if caps.read then pbz.Subscribe("npc_look", "look_changed") end
    if caps.ui then try_page() end
end)

pbz.OnModuleEvent(function(e)
    if page_up then pbz.UiSetText("main", "status", e.module .. "/" .. e.event .. "：" .. e.text) end
end)

pbz.OnStart(function()                                -- 顶层跑完后调一次：已经授权过就直接画页
    local caps = pbz.Caps()                            -- 第一个返回值是表，第二个是位掩码
    if caps and caps.read then pbz.Subscribe("npc_look", "look_changed") end
    if caps and caps.ui then try_page() end
end)
```

### 5.2 和 C 插件不同的地方

| 项目 | Lua 脚本 |
| --- | --- |
| 插件 id | **`lua.<文件名>`**，由文件名决定（`Init` 里的 `id` 被忽略）。文件名只能用字母、数字、`_ . -`，最长 44 字节。授权按这个 id 存（`[PluginGrants] lua.near = read,write,ui`），节点动作存成 `lua.near/near`，自己的设置段是 `[Plugin.lua.near]`。改文件名等于换了一个插件 |
| 声明 | 顶层调一次 `pbz.Init{...}`，没有清单文件或文件头。`caps` 可以写成名字表 `{"read","write"}`、字符串 `"read,write"` 或位掩码数字。顶层跑完后再调 `Init` 返回 `nil, -2, "STATE"`。不调 `Init`：插件名就是文件名，只有派生能力 |
| 失败 | 函数**不抛错**：失败返回 `nil, 结果码, 名字`，例如 `nil, -10, "DENIED"`。成功返回值或 `true` |
| 序号 | 节点、行、出口、按键从 0 数（和 C 一样，不是 Lua 习惯的 1）；返回的数组从 1 数 |
| 标准库 | 只有 `base` `coroutine` `table` `string` `math` `utf8`；**没有** `io`、`package`（`require`）、`debug`；`os` 只有 `time` `clock` `difftime`；没有 `dofile` `loadfile` `string.dump`；`load` 只收文本 |
| 预算 | 每次回调最多 20 万条 Lua 指令（约一两毫秒），顶层载入 500 万条；`pcall` 拦不住预算错误。预算不数 C 函数里的时间：别写会回溯爆炸的 `string.find` 模式 |
| 内存 | 每个脚本 16 MB |
| 日志、提示 | `pbz.Log` 和 `print` 写日志，每秒最多 200 行；`pbz.Notify` 每秒最多 4 条 |
| 出错 | 回调出错或超预算：这个脚本本次停用，日志一行 `PLUGIN [lua.名字] script error in 回调名: 信息 (switched off for this session)`，列表里显示原因。载入出错（语法、顶层出错、文件名不合法）：不载入，列表里显示 Lua 的信息（带文件名、行号）。别的脚本和 DLL 照常 |
| 数量 | 一次最多读 64 个文件、32 个脚本，每个文件不超过 1 MB |

### 5.3 载入和热重载

- 「派生插件」打开后的第一帧读一次文件夹（按文件名排序）。`[Plugins] Scripts = 0` 单独关掉 Lua（缺省 1）。
- 改完脚本**不用重启游戏**：插件列表里点「重新读取脚本」。所有脚本先停下（撤掉它们的信号、动作、UI 页、订阅，关闭 Lua 状态），下一帧重读整个文件夹；DLL 不动。也可以用命令总线的 `plugins.reload_scripts`（需要 `command`）、`host` 模块的动作 `reload_scripts`，或者命令文件里写 `plugins reload`（`plugins status` 写一行状态到日志）。
- 授权按文件名保存，改文件内容不影响已给的授权。

### 5.4 `pbz` 表速查

| 分类 | 函数 |
| --- | --- |
| 自身 | `Init{ name, version, author, description, min_revision, caps = {...}, caps_reason }`（只在顶层有效，也叫 `Describe`）`Id()` `Version()`（接口版本、MOD 版本、修订）`Revision()` `HasFeature(名)` `Caps()`（返回 `{combo=true, read=false, …}` 和位掩码）`Enabled()` `Now()` `Log(文字)` `Notify(文字)` |
| 信号 | `RegisterSignal{ id, name, group, description }` → 键码；`UnregisterSignal(id)` `SignalCode(id)` `FireSignal(id)` `SetSignal(id, 按下)` `SignalWanted(id)` |
| 节点动作 | `RegisterAction(desc, fn)`：desc 里 `id name group description kind("decide"/"run") outputs value{label,unit,min,max,step,default}`，函数也可以写成 desc 的 `decide =` / `run =`；`UnregisterAction(id)`。`call` = `{node, trigger, button, value, press_bits, press_npc_key, output_count, action, table, now_ms}` |
| 回调 | `OnEvent(fn(e))` `OnHeroAbility(fn(a))`（返回 `true` 或 `pbz.CONSUME` 取消这个技能）`OnTick(fn(now_ms))` `OnEnabled(fn(on))` `OnModuleEvent(fn(e))` `OnUi(fn(e))` `OnCaps(fn(mask, caps))` `OnStart(fn())`（顶层跑完后调一次）；传 `nil` 清除。参数字段：`OnEvent` 的 `e` = `{type, now_ms, trigger, node, node_type, button, taken, press_bits, npc_key, text, ability, boss}`；`a` = `{name, bits, handle, chain_own, now_ms}`；`OnModuleEvent` 的 `e` = `{module, event, text, now_ms}`；`OnUi` 的 `e` = `{page, item, kind, value, now_ms}`；`caps` = `{combo, read, write, ini, command, ui, ue}` 布尔 |
| 读派生链 | `GetState()` `NodeCount()` `GetNode(i)` `GetRoute(节点, 行)` `GetHero()` `WeaponMoves()`，字段名同 C 结构体 |
| 控制 | `Press(按键)` `PlayNode(节点 [, 按键])` `ResetChain([原因])` `CancelHeroAbility(句柄)`，排队到下一帧 |
| 派生表 | `ListTables()` `ExportTable(表)` `ImportTable(表, 文本)`；`EditBegin(表)` → 句柄，之后 `EditAddNode(句柄, 类型 [, x, y])` `EditSetField(句柄, 节点, 行, 字段, 值)` `EditLink(句柄, 从, 出口, 到)` `EditCommit(句柄)` 等，同 C 的 `edit_*` |
| 选项与设置 | `GetOption(名)` `SetOption(名, 数)` `GetSetting(段, 键)` `SetSetting(段, 键, 值)` |
| 模块 | `ModuleList()` `PropList(模块)` `ActionList(模块)` `EventList(模块)` `CommandList()`（返回表的数组，不需要授权）；`PropGet(模块, 属性)`（`read`）`PropSet(模块, 属性, 值)`（`write`，排队）`ActionInvoke(模块, 动作 [, 参数 [, later]])`（`write`）`Subscribe(模块 [, 事件 [, 开关]])`（`read`）`Command(命令行 [, later])`（`command`） |
| UI 页 | `UiPage{ id, title, items = { {kind = "label"/"toggle"/"slider"/"button"/"choice"/"text", id, label, text, bind, min, max, step, value}, ... } }`（`ui`）`UiRemovePage(id)` `UiSetValue(页, 控件, 值)` `UiSetText(页, 控件, 文字)` |
| 常量 | `pbz.BUTTON.*` `pbz.NODE.*` `pbz.EVENT.*` `pbz.ACTION.*` `pbz.PHASE.*` `pbz.RESULT.*` `pbz.CAP.*` `pbz.UI.*` `pbz.PASS` `pbz.CONSUME` |

示例在 `sdk/samples/lua/`（安装包 `SDK\samples\lua\`，同时已放进 `PBZPlugins\scripts\`）：`condition.lua`（判断和动作节点，只用派生能力）、`signal.lua`（把空中轻攻击变成信号）、`panel.lua`（申请四项权限、绑定模块属性画一页、订阅事件）。

### 5.5 确认它在工作

- 插件列表里脚本前面有「Lua ·」，后面是信号数和节点动作数，或者没载入的原因。
- 日志：`PLUGIN [lua.near] loaded: …`；出错时 `script error in …`；缺权限时 `denied: …`。
- 不开游戏的测试：`build\Release\lua_host_test.exe`（任意工作目录，示例脚本的路径编进了程序）用假链、假模块和三个示例脚本测宿主、沙箱、预算和热重载，68 项，全过时打印 `ALL PASSED`、退出码 0。它运行前会清空自己旁边的 `PBZPlugins\*.dll` 和 `PBZPlugins\scripts\`，别把要留的文件放在那里。
- 测试或安装脚本里预先授权：游戏关着时在 ini 写 `[PluginGrants]` 下的 `lua.near=read,write,ui`。

## 6. 速查

### 6.1 线程

| 函数 | 线程 |
| --- | --- |
| `log` `notify` `now_ms` `enabled` `has_feature` `granted_caps` 列表函数 `export_table` `import_table` `list_tables` `edit_*` `get_option` `set_option` `get_setting` `set_setting` `ui_set_value` `ui_set_text` | 任意 |
| `fire_signal` `set_signal` `press` `play_node` `reset_chain` `prop_set` | 任意，排队到下一帧 |
| `set_callbacks` `register_*` `unregister_*` `signal_wanted` `get_state` `node_count` `get_node` `get_route` `get_hero` `cancel_hero_ability` `ui_page` `ui_remove_page` | 只能在游戏线程（回调里） |
| `prop_get` | 看模块的线程列：`game` 只能在游戏线程，`any` / `worker` 任意 |
| `action_invoke` `command` | 游戏线程上立即执行；加 `PBZ_CALL_LATER` 可在任意线程排队，结果以 `host / action_done`、`host / command_done` 事件回来 |

每帧的顺序：递送上一帧的事件 → 各插件 `on_tick` → 执行排队的操作 → 推进信号 → 派生链处理按键（这时才调 `decide` / `run`）。过场动画期间这一帧不跑。

### 6.2 结果码

`PBZ_OK 0`，`E_ARG -1` 参数不对 / 值超范围，`E_STATE -2` 现在做不了，`E_THREAD -3` 线程不对，`E_NOT_FOUND -4`，`E_FULL -5` 到上限，`E_EXISTS -6` 已存在或键码冲突，`E_BUFFER -7` 缓冲区太小（`needed` 给出字节数，含结尾 0），`E_DISABLED -8` 派生插件关着，`E_VERSION -9`，`E_DENIED -10` 没授权。

### 6.3 能力（`PBZ_CAP_*`，ini 里的名字）

| 位 | ini 名 | 管什么 |
| --- | --- | --- |
| `COMBO` 1 | （总是授予） | 节点、信号、派生链事件和控制、编辑派生表、7 个派生选项、写 `[NpcCombo]` 和 `[Plugin.<id>]` |
| `READ` 2 | `read` | `prop_get`、`module_subscribe`、UI 控件显示属性值 |
| `WRITE` 4 | `write` | `prop_set`、`action_invoke`、UI 上绑定的控件 |
| `INI` 8 | `ini` | 写其他 ini 段；`persist` 类命令 |
| `COMMAND` 16 | `command` | `command` |
| `UI` 32 | `ui` | `ui_page` |

玩家只能授予插件申请过的能力。`[Plugins]` 和 `[PluginGrants]` 两段插件永远写不了。

### 6.4 模块

用 `module_list`、`prop_list(模块)`、`action_list(模块)`、`event_list(模块)` 列出（不需要授权，结果是一行一项、制表符分列的文本，行尾可能多出列）。2026-11-13 注册的模块：

| 模块 | 常用成员 |
| --- | --- |
| `host` | `api_revision` `mod_version` `plugins_enabled` `plugins_loaded`（只读）；事件 `command_done` `action_done` |
| `combo` | `skill_damage` `grab_damage_mode` `grab_damage_value` `chain_hud`（off/detailed/simple）`view_target`（pelvis/neck/head）`view_scale`（height/ratio）`show_toast` |
| `npc_look` | `skill_damage` `boss_locomotion` `experimental`；动作 `apply` `restore` `inspect` `skill` `list`；事件 `look_changed` |
| `character` | 动作 `replace` `restore` `list`；事件 `swap_changed` |
| `npc_body` | `enabled` `edits_on`；动作 `bone` `clear_bone(s)` `bones` `morph` `clear_morph(s)` `morphs` `preset` `presets` `editor` |
| `bones` | `supplement` `debug`；事件 `pose_finalized`（每帧最多一个） |
| `boss_rush` | `override_rank` `rank` `override_cycle` `cycle_number` `override_difficulty` `difficulty` `auto_next` `heal_on_start`；动作 `teleport` `direct` `start` `next` `retry` `stop` `refresh` `list` `difficulties`；事件 `rush_changed` |
| `boss_ai` | 动作 `list` `set` `refresh` `restore` |
| `air_skills` | 一组开关和 `bounce_speed` `enemy_radius` |
| `grab` | `enabled` `allow_bosses` `position_correction` `tang_execution` `transform` `variant` `range`；动作 `trigger` `cancel` `variants` |
| `weapons` | `enabled`；动作 `swap` `switch_to_set` `take_temporary` 等；事件 `weapon_changed` |
| `pad` | `triggers_forced_off` |

属性值读写都是文本：`bool` 收 `0 1 true false on off`；`enum` 收序号或名字；超出范围是 `PBZ_E_ARG`（不截断）；整数属性不收小数。模块属性由模块自己存进 ini，插件不用再 `set_setting`。**所有模块都还没在游戏里验证过。**

### 6.5 命令（`command_list` 列出）

诊断：`boss_rush.status` `air_skills.status` `air_skills.tags` `grab.status` `weapons.status` `bones.status` `bones.collisionprobe` `hair.status` `hub.status`。状态：`hair.toggle_mesh_groom` `hair.toggle_bounds` `hair.toggle_force` `hair.toggle_outfit` `hair.restore` `hub.show <页名>` `hub.toggle` `hub.hide`。持久（另需 `ini`）：`hair.toggle_triggers`。不在白名单里的一律 `PBZ_E_NOT_FOUND`。

### 6.6 UI 控件

`LABEL`（`text`）、`TOGGLE`（`value` 0/1）、`SLIDER`（`min max step value`，列表里是加 / 减两行）、`CHOICE`（`text` 逗号分隔，`value` 序号）、`BUTTON`（`label`）、`TEXT`（多行只读）。`bind` 写 `模块.属性`（按钮写 `模块:动作`，`text` 是参数）：宿主自己读写，不调 `on_ui`。没有 `bind` 的控件被操作时调 `on_ui`。每个插件最多 8 页，每页 1~64 个控件；同一个页 id 再 `ui_page` 是整页替换。

### 6.7 常用编号

- 按键 `PBZ_BUTTON_*`：0 NPC 技能键，1 轻攻击，2 重攻击，3 影之武装，4 特殊攻击，5 战技，6 跳跃，7 闪避，8 连续闪避，9 完美闪避，10 偏斜闪避，11 特殊闪避，12 任意闪避（只用于匹配），13 处决，14 自定义组合键，15 自定义按键，16 格挡，17 简易处决。`press` 不能送 12、14、15。
- 事件 `PBZ_EVENT_*`：1 派生链开始，2 按键，3 播放节点，4 招式结束，5 重置（`text` 是原因），6 外观换好，7 派生表变了（缓存的节点序号作废）。
- 节点类型的数值就是 `.combo` 文件里存的值，永远不变；插件节点 31，主角体型 32。完整列表见头文件 `PBZ_NODE_*`。

## 7. 配方

### 7.1 用信号替换一个具体的武器动作

```c
static int32_t PBZ_CALL OnHeroAbility(void*, const PbzAbilityEvent* e) {
    if (e->chain_own || !strstr(e->name, "_Air_A")) return PBZ_PASS;   // 只管空中轻攻击
    int wanted = api->signal_wanted(self, "air_light");
    api->fire_signal(self, "air_light");
    return wanted ? PBZ_CONSUME : PBZ_PASS;                            // 派生链在等它：取消原动作
}
```

先在入口里 `register_signal`，并在 `set_callbacks` 里登记 `on_hero_ability`。玩家把「空中轻攻击」信号用作自定义按键起端。

### 7.2 改派生表：加一个重攻击起端接一个 BOSS 技能

```c
PbzEdit ed;
if (api->edit_begin(self, "Hero", &ed) == PBZ_OK) {
    int32_t trig, skill;
    api->edit_add_node(self, ed, PBZ_NODE_TRIGGER, 100, 100, &trig);
    api->edit_set_field(self, ed, trig, -1, "trigger", "2");
    api->edit_add_node(self, ed, PBZ_NODE_BOSS_SKILL, 400, 100, &skill);
    api->edit_set_field(self, ed, skill, -1, "boss", "MuXiaoKui");
    api->edit_set_field(self, ed, skill, -1, "ability", "GA_MuXiaoKui_Skill_01");
    api->edit_link(self, ed, trig, 0, skill);
    api->edit_commit(self, ed);   // 写回当前 .combo 文件；正在用这张表的派生链重置（原因 edited）
}
```

`edit_commit` 会**改玩家的 .combo 文件**。测试时先另存一个文件，别在玩家的 `default.combo` 上试。字段名用 `edit_field_names` 列出，或查 PLUGIN_API.md「字段」。

### 7.3 从别的线程问面板状态

```c
api->command(self, "hub.status", PBZ_CALL_LATER, NULL, 0, NULL);   // 需要 command 权限
// 答案在 on_module_event：module = "host"，event = "command_done"，
// text = "ok\thub.status\t输出"（失败时是 fail）
```

## 8. 测试

### 8.1 不开游戏能测的

在仓库根目录跑（有的测试按相对路径找样例文件，从 `build\Release` 里跑会失败）：

```bat
build\Release\combo_plugins_test.exe
build\Release\mod_registry_test.exe
build\Release\lua_host_test.exe
```

它们用真宿主和一张假的派生链，不开窗口、不碰游戏。

### 8.2 要开游戏才能测的

只有在用户要求时才启动游戏或操作窗口。测试前确认：

- 「派生插件」开着，插件在列表里显示「已载入」；
- 需要的权限已授予；
- 看日志里的 `PLUGIN` 行，而不是只看画面。

### 8.3 常见问题

| 现象 | 原因 |
| --- | --- |
| 列表里没有插件 | 派生插件没开；DLL 不在 `PBZPlugins\`；不是 x64 DLL；缺运行库（改用 `/MT`） |
| 「入口函数没有调用 describe」 | 入口里第一个调用必须是 `describe` / `describe_ex` |
| 调用返回 `PBZ_E_DENIED` | 没授权；在 `on_caps` 里等授权再做 |
| 申请了权限，列表里没有权限行 | 用的是 `describe` 而不是 `describe_ex` |
| `prop_set` 返回 OK 但值没变 | 排队到下一帧；模块当时做不了会写日志 `prop_set … failed` |
| UI 页没出现 | 缺 `ui` 权限；绑定的模块 / 属性不存在（`ui_page` 返回 `PBZ_E_NOT_FOUND`） |
| 判断节点总不走 | `decide` 返回 -1 或超范围；出口没接线 |
| 中文乱码 | 没加 `/utf-8` |

## 9. 修改宿主（本仓库）

| 想做的事 | 改哪里 |
| --- | --- |
| 给一个功能加属性 / 动作 / 事件 | 那个功能的 `*_api.cpp`（例如 `src/character/combo_api.cpp`），用 `ModRegistry::ModuleBuilder(...).Bool/Int/Float/Enum/Text(...).Commit()`；新模块在 `src/plugin/module_apis.cpp` 里调用注册函数 |
| 加一个白名单命令 | 模块的描述表；种类按「只读 / 改状态 / 写文件」选 `diagnostic` / `state` / `persist` |
| 加 C 接口函数 | `sdk/include/pbz_combo_api.h` 的 `PbzApi` **末尾**追加，`PBZ_API_REVISION` 加 1，`has_feature` 加一个名字；实现在 `src/plugin/combo_plugins.cpp` |
| Lua 一侧 | `src/plugin/lua_host.cpp`；新函数要同时进 `pbz` 表和 PLUGIN_API.md 的 Lua 函数表 |
| 插件列表、权限行、插件页的绘制 | `src/character/combo_menu.cpp`（「派生插件」列表，search mode 7） |

规则：

- 二进制兼容：不改已有字段和数值，只在末尾追加；每个结构体第一个字段是 `size`。
- 写入一律走排队，回调一律在游戏线程；模块调用要有异常保护（`ModRegistry` 已经做了）。
- 加了东西要同时更新 `docs/PLUGIN_API.md`、`tests/mod_registry_test.cpp` 或 `tests/combo_plugins_test.cpp`，必要时更新示例。
- 构建、安装、提交的规矩（低优先级构建、关游戏再装、`/W4 /WX`、行尾与 BOM）见仓库根目录的 `CODEX_HANDOFF.md`。
