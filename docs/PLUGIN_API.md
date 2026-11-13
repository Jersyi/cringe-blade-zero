# 派生插件接口手册（PBZ Combo Plugin API v1，接口修订 3）

派生插件让别人不改 MOD 源码，也能给「招式派生」加东西：新的节点、新的“按键”、对派生链的监听和控制，以及对派生表、选项和设置的读写。**修订 2（2026-11-13）起，插件还能读写整个 MOD**：用一组通用函数读写各功能模块（外观、身材 / 骨骼、Boss 连战、空中技能、投技、武器预设……）的属性、调用它们的动作、订阅它们的事件，通过白名单运行面板命令，并在面板上放自己的一页；这些都要玩家逐个插件授权，默认只给派生。插件是一个 Windows x64 DLL，用一个 C 头文件 `pbz_combo_api.h` 编写，任何能生成 x64 DLL 的 C / C++ 编译器都能用（MSVC、clang-cl、MinGW-w64）。

| 插件能做的事 | 用到的接口 |
| --- | --- |
| 加一种**插件节点**：派生链走到它时做一件事（动作），或在按键经过时决定走哪个出口（判断） | `register_action` |
| 加一种**插件信号**：自定义按键列表里多出的一个“按键”，插件决定它什么时候按下 | `register_signal`、`fire_signal`、`set_signal` |
| 监听派生链：开始、按键、播放节点、招式结束、重置、变身、派生表变化 | `on_event` |
| 监听主角放出的每个技能，必要时取消它（让派生链的招式替代） | `on_hero_ability`、`cancel_hero_ability` |
| 读派生链状态、节点、出口、主角与目标的血量 / 距离 / 空中 | `get_state`、`get_node`、`get_route`、`get_hero` |
| 控制派生链：送一次按键、直接播放某个节点、结束派生链 | `press`、`play_node`、`reset_chain` |
| 读写派生表：增删节点、改任意字段、接线、修饰、整表导入导出 | `edit_*`、`export_table`、`import_table` |
| 读写 MOD 选项和 `PBZDebugMenu.ini` 的任意一项 | `get_option` / `set_option`、`get_setting` / `set_setting` |
| 更新、注销自己注册的节点动作和信号 | 再次 `register_*`、`unregister_*` |
| 判断这个 MOD 有什么功能、是哪一版 | `api_revision`、`has_feature`（见「接口修订与特性探测」） |
| 向玩家申请能力，查看现在被允许了什么 | `describe_ex`、`granted_caps`、`on_caps`（见「权限」） |
| 读写 MOD 各功能模块的属性、调用动作、订阅事件 | `module_list`、`prop_*`、`action_*`、`event_list`、`module_subscribe`（见「模块」） |
| 运行白名单里的面板命令（毛发开关、面板显示 / 隐藏、各模块状态） | `command`、`command_list`（见「命令总线」） |
| 在面板「派生插件」列表里放自己的一页（开关、滑条、按钮、文字），可绑定到模块属性 | `ui_page`、`ui_set_value`、`ui_set_text`（见「插件 UI 页」） |

SDK 的内容：

```
SDK/
  PLUGIN_API.md                        本手册
  include/pbz_combo_api.h              唯一的头文件（C ABI，带中文注释）
  samples/CMakeLists.txt               示例的独立构建
  samples/conditions/                  示例 1：条件判断节点（血量、距离、空中、概率）+ 两个动作节点
  samples/weapon_actions/              示例 2：把具体的武器动作（空中轻攻击等）变成插件信号
  samples/mod_panel/                   示例 3：用模块接口读写整个 MOD（申请权限、画一页、订阅事件、命令总线）
```

## 快速开始

### 1. 装上示例插件

1. 在 `version.dll` 旁边建一个文件夹 `PBZPlugins`（安装包里已经有，含三个示例 DLL）。
2. 把插件 DLL 放进去。需要配置文件的插件（例如 `pbz_weapon_actions.txt`）放在同一个文件夹。
3. 进游戏按 F10，打开「招式派生」页，点顶栏的 **派生插件：关 ▾**，在列表里选 **打开派生插件**。
4. 插件随即载入。列表里会列出每个插件和它的状态；`PBZDebugMenu.log` 里有一行 `PLUGIN [插件id] loaded: …`。

开关保存在 `PBZDebugMenu.ini` 的 `[Plugins] Enabled`。没有 ini 时默认关闭；关闭时 MOD 不载入任何 DLL。打开后再放进文件夹的插件，在下次启动游戏时载入。

### 2. 在派生链里用插件

- **插件节点**：画布右侧「节点列表 → 起端与流程 → + 插件节点」，在弹出的列表里选一个插件动作。也可以先放一个空的插件节点，之后点卡片上的选择框换动作。
- **插件信号**：给「自定义按键」起端、按键分支或武器派生修改的自定义按键行录键时，在按键列表的「插件信号」分组里选。信号可以和真实按键组合（同时按 / 依次按）。

### 3. 编译示例

```bat
cd SDK\samples
cmake -S . -B build -A x64
cmake --build build --config Release
```

生成的 `build\Release\pbz_conditions.dll`、`pbz_weapon_actions.dll` 复制到 `PBZPlugins`。不用 CMake 时：

```bat
cl /LD /O2 /EHsc /utf-8 /MT pbz_conditions.cpp /I ..\..\include
```

`/utf-8` 必须加：接口里的字符串一律是 UTF-8，源码里的中文字符串要按 UTF-8 编进 DLL。建议用静态运行库（`/MT`），玩家不必另装运行库。

### 4. 最小的插件

```cpp
// my_plugin.cpp —— 一个判断节点：主角在空中走「空中」出口，否则走「地面」出口。
#include "pbz_combo_api.h"

static const PbzApi* api;
static PbzPlugin* self;

static int32_t PBZ_CALL InAir(void*, const PbzNodeCall*) {
    PbzHeroFacts h{}; h.size = sizeof(h);
    if (api->get_hero(&h) != PBZ_OK || !h.valid) return -1;   // 读不到：不走任何出口
    return h.airborne ? 0 : 1;
}

PBZ_PLUGIN_EXPORT PbzResult PBZ_CALL PbzPluginMain(const PbzApi* a, PbzPlugin* s) {
    if (!a || a->version < 1) return PBZ_E_VERSION;
    api = a; self = s;

    PbzPluginInfo info{};
    info.size = sizeof(info);
    info.id = "me.air";  info.name = "空中判断";  info.version = "1.0.0";
    info.author = "我";  info.description = "按主角是否在空中分支";  info.min_api = 1;
    if (PbzResult r = api->describe(self, &info); r != PBZ_OK) return r;   // 必须第一个调用

    static const char* const outs[] = {"空中", "地面"};
    PbzActionDesc d{};
    d.size = sizeof(d);
    d.id = "in_air";  d.name = "空中？";  d.kind = PBZ_ACTION_DECIDE;
    d.output_count = 2;  d.outputs = outs;  d.decide = InAir;
    return api->register_action(self, &d);
}
```

插件动作在派生表里保存为 `插件id/动作id`（这里是 `me.air/in_air`），所以插件 id 和动作 id 发布后不要再改。

## 插件的生命周期

### 载入

- 「派生插件」打开后的第一帧，MOD 在游戏线程上按文件名顺序载入 `PBZPlugins\*.dll`（最多 32 个），对每个 DLL 调用导出函数 `PbzPluginMain`。
- 入口函数里**第一件事是 `describe`**；没有调用 `describe` 的插件不会被使用。入口返回非 `PBZ_OK` 时插件不被使用，已注册的信号和动作一并撤销，原因写进日志并显示在插件列表里。
- 被使用的插件的 DLL 一直留在游戏里，直到游戏退出（没有卸载回调，也不会被 `FreeLibrary`）。没有导出 `PbzPluginMain` 的 DLL 会被立即卸载。
- `min_api` 高于 MOD 的 `PBZ_COMBO_API_VERSION` 时 `describe` 返回 `PBZ_E_VERSION`，插件不被使用。

### 开关

- 页面上关闭「派生插件」时：每个插件收到 `on_enabled(0)`；信号全部松开；判断节点不走任何出口；动作节点什么都不做；`fire_signal` 等返回 `PBZ_E_DISABLED`；事件和排队的操作被丢弃。
- 再打开时收到 `on_enabled(1)`。插件不会被重新载入，也不会再调用入口函数。

### 出错隔离

MOD 用结构化异常保护每一次对插件的调用。插件在入口、回调、`decide` 或 `run` 里崩溃（访问冲突等）时，这个插件被停用（插件列表里显示「运行出错，本次游戏内已停用」，日志里有异常码和出错的回调名），画面右下角提示玩家，游戏和其他插件照常运行。被停用的插件直到下次启动游戏前都不会再被调用。

C++ 异常不要抛出插件边界：回调里用 `try / catch` 自己接住。

## 线程与时序

- **所有回调**（`on_event`、`on_hero_ability`、`on_tick`、`on_enabled`、`decide`、`run`）都在游戏线程上调用。回调里尽快返回：不要等待锁、不要读写文件或网络。需要慢操作时自己开线程，结果再用「任意线程」的函数交回来。
- 标注**任意线程**的函数可以在任何线程调用。会改变派生链的那些（`fire_signal`、`set_signal`、`press`、`reset_chain`、`play_node`、`notify`）只是排队，在下一帧开头、`on_tick` 之后按调用顺序执行。
- 其余函数只能在游戏线程（也就是回调里）调用，在别的线程调用返回 `PBZ_E_THREAD`。
- 每帧的顺序：
  1. 递送上一帧积攒的事件（`on_event`，按发生顺序）；
  2. 每个插件的 `on_tick`；
  3. 执行排队的操作；
  4. 推进信号（`fire_signal` 的“按一下”在这一步按下，下一帧松开）；
  5. 派生链处理这一帧的按键（这时才会调用 `decide` 和 `run`）。
- 事件是**延后一帧**递送的：派生链在这一帧做的事，插件在下一帧开头才看到。事件里的字符串在回调返回后失效，需要保留请复制。
- **修订 2 追加的写入**也是排队的：`prop_set` 在下一帧、`on_tick` 之后按调用顺序执行（和 `press` 等同一个队列）；`action_invoke` 和 `command`（`game` / `any` 的命令）在游戏线程上立即执行；模块事件、`host / command_done`、`on_ui`、`on_caps` 都在下一帧开头递送（`on_tick` 之前）。按 2026-11-13 的代码，过场动画期间派生插件的帧不跑（没有 `on_tick`，事件和排队的写入要等过场结束）。

## 兼容规则

- `PBZ_COMBO_API_VERSION` 只在不兼容的改动时增加。新函数只追加在 `PbzApi` 末尾，用 `PBZ_API_HAS(api, 函数名)` 判断这个 MOD 有没有：

  ```c
  if (PBZ_API_HAS(api, play_node)) api->play_node(self, node, -1);
  ```

- 每个结构体第一个字段是 `size`：调用方填 `sizeof(结构体)`。MOD 只读写这个大小以内的字段，所以用旧头文件编译的插件在新 MOD 上照常工作，用新头文件编译的插件在旧 MOD 上也只会少拿到新字段。
- 字符串一律 UTF-8、以 0 结尾。MOD 给出的字符串指针只在本次调用 / 本次回调内有效。
- 插件交给 MOD 的字符串（`PbzPluginInfo`、`PbzActionDesc`、`PbzSignalDesc` 里的）MOD 会复制一份，调用返回后可以释放。
- 节点类型 `PBZ_NODE_*` 和按键 `PBZ_BUTTON_*` 的数值就是保存在 `.combo` 文件里的值，永远不变。
- 调用约定 `PBZ_CALL` 是 `__cdecl`（x64 上只有一种调用约定，写上是为了清楚）。
- 修订 2 追加的函数都在 `PbzApi` 末尾，字段 `api_revision` 之后；`PbzCallbacks` 也只在末尾追加了 `on_module_event`、`on_ui`、`on_caps`。用修订 2 的头文件编译的插件在旧 MOD 上：新函数用 `PBZ_API_HAS` 判断，新回调旧 MOD 不会调用。反过来，旧插件在新 MOD 上不受影响。
- 新的结果码 `PBZ_E_DENIED = -10`，旧码的数值不变。

## 插件节点

插件节点是派生链里的一种节点（`PBZ_NODE_PLUGIN = 31`），它做什么由插件的**动作**决定。动作分两种：

| 类型 | 走到节点时 | 卡片 | 接线 |
| --- | --- | --- | --- |
| **动作** `PBZ_ACTION_RUN` | 像一招一样“播放”：调用一次 `run`，派生链停在这里等下一次按键 | 一个出线 | 和招式一样：前一招接它，它接下一招 |
| **判断** `PBZ_ACTION_DECIDE` | 按键经过时调用 `decide`，返回值决定走哪个出口；本身不播放、不占时间 | 每个出口一根出线 | 像按键分支：每个出口接一个节点 |

两种都可以带一个**数值**（`has_value`）：卡片上出现一个可以加减、键入的数，插件在 `PbzNodeCall.value` 里读到。数值的名字、单位、范围、步长和新节点的默认值由插件给出。

### 动作（RUN）

- 派生链走到动作节点时：MOD 停下正在播放的招式（等它到派生点后），不在主角或 BOSS 身上放任何东西，调用一次 `run`。
- 之后派生链停在这个节点，像放完一个 BOSS 援护一样：下一次按键立刻可以接到它的出线（没有派生点要等），等待超时后派生链照常重置。
- 想“不等按键，直接接下一招”：在 `run` 里读出这个节点的出线，再 `play_node`：

  ```c
  static void PBZ_CALL Chain(void*, const PbzNodeCall* call) {
      PbzNodeInfo n = { sizeof(n) };
      if (api->get_node(call->node, &n) == PBZ_OK && n.next >= 0) api->play_node(self, n.next, -1);
  }
  ```

- `run` 里可以 `press`、`play_node`、`reset_chain`、`fire_signal`、`notify`……它们都在下一帧执行。

### 判断（DECIDE）

- 一次按键沿连线走到判断节点时，MOD 调用 `decide`，返回 `0 ~ output_count-1` 走对应出口，返回 `-1`（或任何超出范围的数）表示不走任何出口——这次按键就像没有接上一样（起端处的按键交还给游戏）。
- 出口没有接线时同样不走。
- 判断节点可以连续串联（判断 → 判断 → 招式），也可以放在起端后面作为第一个节点。
- **只做判断，不要有副作用。** 派生链处理一次按键时会沿连线查找不止一次（先看这次按键会不会被接住，再真正走过去），所以 `decide` 可能被问到多次。MOD 在同一帧里对相同的「节点、按键、数值」只调用一次，之后用缓存的结果——随机判断因此前后一致。下一帧会重新问。
- 在游戏线程以外的地方（例如页面预览路径）需要答案时，MOD 不调用插件，直接用这个节点上一次的答案。

### 节点上的信息

`PbzNodeCall`（`decide` / `run` 的参数）：

| 字段 | 含义 |
| --- | --- |
| `node` | 节点序号（在当前派生表里） |
| `trigger` | 派生链的起端节点；`-1` 表示这次按键会开始一条新的派生链 |
| `button` | 派生链此刻的按键（`PBZ_BUTTON_*`） |
| `value` | 节点上设置的数值（没有数值的动作为 0） |
| `press_bits` | 带来这次经过的按键的主角按键位 `1u << PBZ_BUTTON_*`；自定义按键为 0 |
| `press_npc_key` | 1：这次按键是 NPC 技能键 |
| `output_count` | 判断节点的出口数 |
| `action` | 动作 id（不含插件 id） |
| `table` | 派生表（`"Hero"` 为主角，其余是 NPC 外观 id） |
| `now_ms` | `GetTickCount64()` |

### 注册动作

`PbzActionDesc`：

| 字段 | 说明 |
| --- | --- |
| `id` | 插件内唯一，字母、数字、`_ . -`，1~48 字节；保存为 `插件id/动作id` |
| `name` | 卡片标题、选择列表里的名字 |
| `group` | 选择列表里的分组（可为 NULL） |
| `description` | 卡片上的说明行（可为 NULL） |
| `kind` | `PBZ_ACTION_RUN` 或 `PBZ_ACTION_DECIDE` |
| `output_count`、`outputs` | 判断：出口数 1~6 和每个出口的名字 |
| `has_value` | 1：卡片上有一个数值 |
| `value_label`、`value_unit` | 数值名（例如「血量低于」）、单位（例如 `%`、`厘米`，可为 NULL） |
| `value_min`、`value_max`、`value_step`、`value_default` | 范围、加减步长、新节点的默认值（范围 ±10 亿以内） |
| `decide` / `run` | 判断必须给 `decide`，动作必须给 `run` |
| `user` | 原样传回 `decide` / `run` 的第一个参数 |

每个插件最多 256 个动作。同一个 id 再注册一次就是**更新**：名字、分组、出口、数值范围、回调都换成新的，派生表里已有的节点立即用新的定义。`unregister_action` 之后，派生表里用到它的节点原样保留（卡片标题后显示「（插件未载入）」），只是不再起作用；重新注册即恢复。

判断节点的出口数变化时，已有节点的出线按序号保留：出口变少，多出的出线不再被走到；出口变多，新出口没有接线。

## 插件信号

信号是一个虚拟按键。注册后它出现在自定义按键的按键列表里（「插件信号」分组，插件可以再给分组名），玩家可以把它用作：

- 「自定义按键」起端的按键（单独一个信号，或和真实按键组合：同时按 / 依次按）；
- 按键分支的自定义按键行；
- 武器派生修改（含空中派生）的自定义按键行。

插件决定信号什么时候按下：

| 函数 | 作用 |
| --- | --- |
| `fire_signal(self, id)` | 按一下：下一帧按下，再下一帧松开。连续调用两次就是按两下（中间会松开一帧）；最多排 8 下 |
| `set_signal(self, id, 1 / 0)` | 按住 / 松开，直到再次调用 |
| `signal_wanted(self, id)` | 1：派生链此刻有起端或下一行在等这个信号（游戏线程） |

信号的键码是 `0x10000 + "插件id/信号id" 的 16 位散列`，保存在 `.combo` 文件里；同一个完整 id 在任何一次游戏里都是同一个键码。`register_signal` 的第三个参数给出键码，可以用来在 `get_node` / `get_route` 读到的 `keys[]` 里认出自己的信号。两个信号的散列撞在一起时，后注册的返回 `PBZ_E_EXISTS`，换一个信号 id 即可。

每个插件最多 256 个信号。再注册同一个 id 是更新（名字、分组、说明），键码不变；`unregister_signal` 后用到它的按键原样保留，只是不会再按下。

### 例子：只在空中轻攻击时接派生

示例 `pbz_weapon_actions` 就是这样做的：

```c
static int32_t PBZ_CALL OnHeroAbility(void*, const PbzAbilityEvent* e) {
    if (e->chain_own) return PBZ_PASS;                         /* 派生链自己放的技能 */
    if (!strstr(e->name, "_Air_A")) return PBZ_PASS;
    int wanted = api->signal_wanted(self, "air_light");        /* 派生链此刻在等它吗 */
    api->fire_signal(self, "air_light");                       /* 按一下信号 */
    return wanted ? PBZ_CONSUME : PBZ_PASS;                    /* 在等：取消游戏原本的空中轻攻击 */
}
```

玩家在派生链里用「空中轻攻击」信号做自定义按键起端，接上自己的招式：空中按轻攻击时，游戏原本的动作被取消，派生链的招式替代它；地面轻攻击不受影响。

## 事件与回调

`set_callbacks` 登记回调，不需要的填 NULL。可以多次调用，后一次替换前一次。

| 回调 | 时机 | 返回 |
| --- | --- | --- |
| `on_event(user, event)` | 派生链发生的事，下一帧开头递送 | — |
| `on_hero_ability(user, ability)` | 主角开始一个技能（当帧） | `PBZ_PASS` 或 `PBZ_CONSUME`（取消这个技能） |
| `on_tick(user, now_ms)` | 每帧一次 | — |
| `on_enabled(user, enabled)` | 页面上开 / 关派生插件 | — |

### 事件类型

`PbzEvent`：

| `type` | 什么时候 | 有意义的字段 |
| --- | --- | --- |
| `PBZ_EVENT_CHAIN_START` 1 | 一条派生链开始 | `trigger` 起端，`button` 按键，`text` 来源 |
| `PBZ_EVENT_PRESS` 2 | 派生链收到一次按键 | `taken` 是否被接住，`press_bits` / `npc_key`，`button`，`text` 来源 |
| `PBZ_EVENT_NODE_PLAY` 3 | 开始播放一个节点 | `node`、`node_type`、`ability`、`boss` |
| `PBZ_EVENT_MOVE_END` 4 | 正在播放的招式结束，派生链开始等下一次按键 | `node` |
| `PBZ_EVENT_CHAIN_RESET` 5 | 派生链结束 / 重置 | `text` 原因：`timeout`、`unmatched`、`other-trigger`、`edited`、插件给的原因等 |
| `PBZ_EVENT_LOOK` 6 | 外观换好 | `text` 现在的外观 id（`hero` 为主角） |
| `PBZ_EVENT_TABLES` 7 | 派生表被编辑或重新读取 | 节点序号可能变了：缓存的节点请重新读 |

所有事件都带 `now_ms`、`trigger`（-1：没有派生链）和 `button`（没有特别指出时是派生链当前的按键）。没有相关节点时 `node`、`node_type` 为 -1；有节点时 `ability`、`boss` 是这个节点的招式名和外观 id。`taken`、`press_bits`、`npc_key` 只在 `PRESS` 里有意义，其余事件为 0。字符串字段不为 NULL（没有时是空字符串）。

### 主角技能

`PbzAbilityEvent`：

| 字段 | 含义 |
| --- | --- |
| `name` | 技能类名，去掉了 `_C`，例如 `GA_Hun_Young_Air_A` |
| `bits` | 它带的主角按键位 `1u << PBZ_BUTTON_*`（0：不是主角按键动作） |
| `handle` | 技能句柄，`cancel_hero_ability` 用 |
| `chain_own` | 1：派生链自己放的技能（通常应忽略） |
| `now_ms` | 时间 |

返回 `PBZ_CONSUME` 时 MOD 立即取消这个技能。只要有一个插件返回 `PBZ_CONSUME` 就取消；所有插件都会被问到。之后想取消也可以在游戏线程调用 `cancel_hero_ability(handle)`。没有任何插件登记 `on_hero_ability` 时，MOD 不扫描主角的技能。

## 读取派生链

以下函数只能在游戏线程调用，读的是**正在使用的派生表**（主角现在的外观对应的那张）。

| 函数 | 说明 |
| --- | --- |
| `get_state(&state)` | 派生链运行状态 `PbzChainState` |
| `node_count()` | 节点数 |
| `get_node(i, &node)` | 节点 `PbzNodeInfo` |
| `get_route(node, row, &route)` | 按键分支 / 武器派生修改的一行，或判断节点的一个出口 `PbzRouteInfo` |
| `get_hero(&facts)` | 主角与目标 `PbzHeroFacts`（每帧最多读一次游戏数据，同一帧里多次调用很便宜） |

`PbzChainState`：

| 字段 | 含义 |
| --- | --- |
| `ready` | 1：当前角色的派生链可用（有主角，派生链开关开着） |
| `trigger` | 当前派生链的起端节点（-1：没有派生链在进行） |
| `cursor` | 派生链所在节点（刚放过的招式、组合或起端） |
| `playing` | 正在播放的节点（-1：没有） |
| `queued` | 已记住的下一招（-1：没有） |
| `button` | 派生链当前的按键 |
| `phase` | `PBZ_PHASE_IDLE` / `STARTING` / `RUNNING` |
| `look_changing` | 1：正在变身 / 变回 |
| `visit` | 每播放一个节点就变（循环回到同一节点也会变） |
| `generation` | 派生表版本：编辑或重新读取后会变 |
| `now_ms`、`move_started_ms`、`reset_at_ms` | 现在、当前招式开始的时间、等待中超时重置的时间（0：不适用） |
| `table`、`look` | 正在使用的派生表、主角现在的外观 id |

`PbzNodeInfo` 和 `PbzRouteInfo` 的字段见头文件注释；想读写任意字段请用下面的编辑会话（它能读到每一个字段，包括这两个结构体里没有的）。

`PbzHeroFacts`：`valid`（0：读图、过场等主角不可用）、`health` / `health_max`、`airborne`、位置 `x y z`（厘米）、`has_target`（有锁定目标或最近的敌人）、`target_locked`、`target_distance`（厘米，胶囊中心之间）、`target_health` / `target_health_max`、`weapon`（主武器 id）、`look`。

## 控制派生链

| 函数 | 线程 | 说明 |
| --- | --- | --- |
| `press(self, button)` | 任意 | 向派生链送一次按键（`PBZ_BUTTON_*`），像玩家按下那样走派生，但**不会**放出主角自己的动作。`ANY_DODGE`、`COMBO_KEYS`、`KEYS` 不能送（返回 `PBZ_E_ARG`）；自定义按键请用信号 |
| `play_node(self, node, button)` | 任意 | 下一帧直接播放一个节点：立即打断正在播放的招式，不等派生点。没有派生链在进行时，从连到这个节点的起端开始一条。`button` 是之后派生链所在的按键（-1：沿用当前的） |
| `reset_chain(self, reason)` | 任意 | 结束当前派生链。`reason` 出现在 `CHAIN_RESET` 事件里（前面加上 `plugin:插件id:`） |
| `cancel_hero_ability(handle)` | 游戏 | 取消主角正在放的一个技能（句柄来自 `on_hero_ability`） |

排队的操作在下一帧按调用顺序执行，最多排 1024 个。

## 编辑派生表

### 编辑会话

派生表的所有内容都可以通过编辑会话修改。会话是一份副本：

```c
PbzEdit ed;
if (api->edit_begin(self, "Hero", &ed) == PBZ_OK) {          /* 把主角的派生表读进副本 */
    int32_t trig, skill;
    api->edit_add_node(self, ed, PBZ_NODE_TRIGGER, 100, 100, &trig);
    api->edit_set_field(self, ed, trig, -1, "trigger", "2");          /* 重攻击起端 */
    api->edit_add_node(self, ed, PBZ_NODE_BOSS_SKILL, 400, 100, &skill);
    api->edit_set_field(self, ed, skill, -1, "boss", "MuXiaoKui");
    api->edit_set_field(self, ed, skill, -1, "ability", "GA_MuXiaoKui_Skill_01");
    api->edit_link(self, ed, trig, 0, skill);                          /* 起端 → 招式 */
    api->edit_commit(self, ed);                                        /* 写回文件并生效 */
}
```

- `edit_begin(self, 表, &ed)`：表名用 `list_tables` 列出（`"Hero"` 为主角，其余是 NPC 外观 id）。不存在的表返回 `PBZ_E_NOT_FOUND`。
- 之后的 `edit_*` 只改副本，游戏里什么都不变。
- `edit_commit` 把副本一次写回当前 `.combo` 文件并生效：正在使用这张表的派生链会重置（原因 `edited`），页面重新读取，插件收到 `PBZ_EVENT_TABLES`。写回前按保存格式整理一遍：不合法的连线、超出范围的数值会像页面保存时一样被去掉或改到范围内。
- `edit_cancel` 放弃副本。
- 会话可以在任意线程使用，但同一个会话不要同时在两个线程里用。每个插件最多同时开 8 个会话。
- 读副本：`edit_node_count`、`edit_get_node`、`edit_get_route`，以及 `edit_get_field` 读任意字段。

### 节点与接线

| 函数 | 说明 |
| --- | --- |
| `edit_add_node(self, ed, type, x, y, &index)` | 加一个节点，默认值和页面上新建时一样；`index` 是它的序号（排在最后） |
| `edit_remove_node(self, ed, index)` | 删除节点：连进它的线改接到它第一个出口原来连的节点；之后的序号减 1 |
| `edit_link(self, ed, from, output, to)` | 出线：普通节点只有 `output = 0`；按键分支、武器派生修改、判断节点的 `output` 是行号 / 出口号（从 0 起）。`to = -1` 断开。节点没有这个出口返回 `PBZ_E_NOT_FOUND`，这根线不允许（例如连到修饰节点）返回 `PBZ_E_ARG` |
| `edit_link_modifier(self, ed, from, modifier)` | 箭头：招式、起端或组合指向一个修饰节点（加速、霸体、裁剪……），放进修饰节点对应的槽 |
| `edit_unlink_modifier(self, ed, from, slot)` | 断开箭头：`slot` 是槽号（见下表 `mod.*` 的顺序），-1 断开全部 |
| `edit_add_route(self, ed, node, &row)` | 给按键分支、武器派生修改或判断节点加一行（判断节点的行就是出口，出口数由插件动作决定，多出的行不会被走到）。插件节点要先把 `action_kind` 设为 1，否则返回 `PBZ_E_STATE` |
| `edit_remove_route(self, ed, node, row)` | 删一行 |
| `edit_export(self, ed, buf, size, &needed)` | 副本的保存文本 |
| `edit_import(self, ed, text)` | 用保存文本替换整个副本（格式错误返回 `PBZ_E_ARG`，副本不变） |

### 字段

`edit_get_field(self, ed, node, row, 字段, buf, size, &needed)` / `edit_set_field(self, ed, node, row, 字段, 值)`：

- `node = -1`：派生表本身，只有字段 `enabled`（0 / 1，整张表是否启用）。
- `row = -1`：节点的字段；`row >= 0`：节点那一行的字段（按键分支、武器派生修改的行，判断节点的出口）。
- 值一律是文本：整数写十进制，开关写 `0` / `1`（也接受 `true` / `false`、`on` / `off`），字符串原样（最长 200 字节），列表用逗号分隔。
- 设置时只检查格式；数值范围在 `edit_commit` 整理时套用（和页面一样）。
- 字段名可以用 `edit_field_names(0 或 1, …)` 列出（0：节点字段，1：行字段），每行一个。

节点字段：

| 字段 | 类型 | 用于 | 含义 |
| --- | --- | --- | --- |
| `type` | 整数 | 全部 | 节点类型 `PBZ_NODE_*`（改类型后其余字段保持原值，提交时按新类型整理） |
| `x`、`y` | 整数 | 全部 | 画布位置 |
| `next` | 整数 | 有出线的节点 | 出线指向的节点（-1：没有） |
| `ability` | 文本 | 招式、BOSS 技能、武器派生、投技等 | 技能 / 招式名 |
| `boss` | 文本 | BOSS 技能、援护、变身；BOSS 特效 | 外观 id；BOSS 特效的特效目录 |
| `weapon` | 文本 | 武器派生、武器派生修改、处决起端 | 武器 id（处决起端：只在这把武器时，空 = 任意） |
| `action` | 文本 | 插件节点 | 动作 id `插件id/动作id` |
| `action_kind` | 整数 | 插件节点 | 0 动作，1 判断（选动作时 MOD 按动作类型设置） |
| `value` | 整数 | 插件节点 | 节点上的数值 |
| `ms` | 整数 | 起端、武器派生修改：无按键多久后重置；间隔：等待时间；招式类：多久后可接下一招（0：放完）；预输入、保持派生：各自的时长 | 毫秒 |
| `trigger` | 整数 | 起端、按键 | 按键 `PBZ_BUTTON_*`（14 自定义组合键，15 自定义按键） |
| `on` | 开关 | 起端；空中释放 | 起端是否启用；空中释放是否启用 |
| `hold_ms` | 整数 | 起端 | 按住多久才触发（0：点按） |
| `keys` | 列表 | 自定义组合键 / 自定义按键起端 | 按键码（自定义按键：虚拟键码 1~254，手柄 0x100 起，插件信号 0x10000 起） |
| `chord` | 开关 | 同上 | 1 同时按，0 依次按 |
| `window` | 整数 | 同上 | 依次按时两键之间最多隔多少毫秒 |
| `swallow` | 开关 | 自定义按键 | 吞键：这些键原本放出的主角动作被取消 |
| `then` | 整数 | 自定义组合键 / 自定义按键 | 继续派生用的键 |
| `own_look` | 开关 | BOSS 技能、投技；起端、武器派生修改 | 不变身 / 整体不变身 |
| `air_set` | 开关 | 武器派生修改 | 1：空中派生 |
| `speed` | 整数 | 加速 | 倍率（百分比，50~1000） |
| `curve`、`points` | 开关、`t:速度,…` | 加速 | 曲线调节；`points` 例如 `0:100,50:300,100:100`（t 为招式长度的百分比） |
| `hit`、`launch` | 整数 | 强制受击 | 受击类型（0 击飞，1 击倒，2 挑空，3 击退，4 破防，5 硬直）；击飞高度（厘米） |
| `air_lift`、`air_hang`、`air_foe_lift`、`air_foe_hang` | 整数 | 滞空、空中释放 | 自身升高（厘米）、自身滞空（%）、敌人升高、敌人滞空 |
| `group`、`w`、`h` | 整数 | 组合内的节点；组合 | 所在组合节点（-1：没有）；组合框的大小 |
| `dist`、`lift` | 整数 | 瞬移 | 距离、抬高（厘米） |
| `hook_self`、`hook_auto`、`hook_dist`、`hook_speed` | 开关 / 整数 | 钩子 | 勾过去、自动、最远距离（厘米）、速度（%） |
| `fx_place`、`fx_damage`、`fx_radius`、`fx_hits` | 整数 | BOSS 特效 | 位置（0 武器上，1 主角脚下，2 主角前方，3 锁定目标）、每段伤害（%）、半径（厘米）、段数 |
| `trim_from`、`trim_to`、`trim_holds` | 整数 / 列表 | 裁剪 | 从、到（毫秒，动画自身时间）、停留点列表 |
| `mod.speed` … `mod.hero_size` | 整数 | 招式、起端、组合 | 箭头指向的修饰节点序号（-1：没有），15 个槽依次是 `speed armor weapon interrupt damage invincible keep_look force_hit ignore_guard keep_chain trim air bone effect hero_size`（`hero_size`：主角体型，`PBZ_NODE_HERO_SIZE = 32`，2026-11-13 起） |

骨骼节点的骨骼编辑数据没有单独的字段，需要时用 `edit_export` / `edit_import` 处理整张表的保存文本。

行字段：

| 字段 | 类型 | 含义 |
| --- | --- | --- |
| `button` | 整数 | 按键 `PBZ_BUTTON_*`（判断节点的出口为 -1） |
| `next` | 整数 | 这一行连向的节点 |
| `pause_ms` | 整数 | 按键分支：停顿后再按 |
| `hold_ms` | 整数 | 按住多久 |
| `keys`、`chord`、`window`、`swallow` | 列表 / 开关 / 整数 / 开关 | 自定义按键行的按键、同时按、间隔、吞键 |

### 整表导入导出（任意线程）

| 函数 | 说明 |
| --- | --- |
| `list_tables(buf, size, &needed)` | 当前 `.combo` 文件里的派生表 id，换行分隔 |
| `export_table(表, buf, size, &needed)` | 一张表的保存文本（和 `.combo` 文件里的一样） |
| `import_table(self, 表, 文本)` | 写入并保存，**覆盖**整张表（表必须已存在，否则 `PBZ_E_NOT_FOUND`；「派生插件」关着时 `PBZ_E_DISABLED`） |

需要缓冲区的函数：缓冲区太小时返回 `PBZ_E_BUFFER`，`needed` 给出需要的字节数（含结尾 0）。先用 `size = 0` 问一次长度也可以。

## 选项与设置（任意线程）

`get_option(名字, &值)` / `set_option(self, 名字, 值)`，立即生效并保存：

| 名字 | 含义 |
| --- | --- |
| `skill_damage` | 派生页「技能伤害」：0 跟随 NPC 技能页，否则 0.1~10 倍 |
| `grab_damage_mode` | 派生页「投技伤害」：-1 跟随投技页，0 原生，1 倍率，2 每段至少最大生命的百分比 |
| `grab_damage_value` | 投技伤害的数值（倍率或百分比） |
| `chain_hud` | 派生显示：0 关，1 详细，2 简略 |
| `view_target` | 视角跟随盯的骨骼：0 骨盆，1 脖子，2 头部 |
| `show_toast` | 派生提示：0 / 1 |
| `plugins` | 派生插件开关（只读） |

`get_setting(段, 键, buf, size, &needed)` / `set_setting(self, 段, 键, 值)` 读写 `PBZDebugMenu.ini`。读的是 MOD 缓存的值（不存在返回 `PBZ_E_NOT_FOUND`），任何插件都可以读；写入立即保存，读取这一项的功能在下次读取时生效（有的要重新打开对应页面或下次启动）。**修订 2 起写入要看权限**：只有 `[NpcCombo]` 和插件自己的 `[Plugin.插件id]` 段是默认可写的，其他段需要玩家授予 `PBZ_CAP_INI`（否则 `PBZ_E_DENIED`）；`[Plugins]`（派生插件开关）和 `[PluginGrants]`（授权）只有玩家能改（`PBZ_E_ARG`）。想立刻生效的功能设置，请用「模块」的 `prop_set`（见下）：直接改 ini 的值，模块可能在启动时才读。

`view_scale`（镜头取景：0 按身高、1 按体型比例）只在模块 `combo` 里，`get_option` / `set_option` 保持原来的 7 个。

## 接口修订与特性探测

`PBZ_COMBO_API_VERSION` 一直是 1（二进制兼容）。功能的增减用**接口修订** `api_revision` 表示：单调递增，只增不减。

| 修订 | 日期 | 内容 |
| --- | --- | --- |
| 1 | 2026-11-12 | 派生插件接口首版（函数表里没有 `api_revision` 字段）。2026-11-13 加入的 `PBZ_NODE_HERO_SIZE` 和修饰槽 `hero_size` 没有改版本号，**无法**从修订 1 判断有没有 |
| 2 | 2026-11-13 | `api_revision`、`has_feature`；权限（`describe_ex`、`granted_caps`、`PBZ_E_DENIED`）；模块注册表（`module_list` … `module_subscribe`）；命令总线；插件 UI 页；`on_module_event` / `on_ui` / `on_caps` 三个回调。`PBZ_NODE_HERO_SIZE` 可用 `has_feature("hero_size_node")` 判断 |
| 3 | 2026-11-13 | Lua 脚本（`PBZPlugins\scripts\*.lua`，全局表 `pbz`，见「Lua 脚本」）。C 接口没有新函数：只新增 `has_feature("lua_scripts")`，宿主的 `host` 模块新增 `scripts_loaded`、`scripts_enabled`、动作 `reload_scripts`，命令 `plugins.reload_scripts` |

```c
// 这个 MOD 的修订（旧 MOD 的函数表里没有这个字段，宏给出 1）：
uint32_t rev = PBZ_API_REVISION_OF(api);
if (rev >= 2 && api->has_feature("modules")) { /* 用模块接口 */ }
if (api->has_feature && api->has_feature("hero_size_node")) { /* 主角体型节点可用 */ }
```

`has_feature(名字)`（任意线程，返回 1 / 0）：

| 名字 | 含义 |
| --- | --- |
| `hero_size_node` | 节点类型 `PBZ_NODE_HERO_SIZE`、修饰槽 `mod.hero_size` |
| `permissions` | `describe_ex`、`granted_caps`、`PBZ_E_DENIED` |
| `modules` | 模块注册表：`module_list`、`prop_*`、`action_*`、`event_list` |
| `module_events` | `module_subscribe`、`on_module_event` |
| `commands` | `command`、`command_list` |
| `ui_pages` | `ui_page` 等 |
| `lua_scripts` | 这个 MOD 会读 `PBZPlugins\scripts\*.lua`（且玩家没有在 `[Plugins] Scripts` 里关掉）；修订 3 |

未知的名字返回 0。`mod_version` 现在是 `"20261113"`，仅供显示。`describe_ex` 的 `min_revision` 高于 MOD 的修订时返回 `PBZ_E_VERSION`，玩家在插件列表里看到「需要更新版本的豪之刃（接口修订 n）」。

## 权限

玩家在「招式派生」页顶栏 **派生插件 ▾** 的列表里，逐个插件、逐项打开它申请的能力（保存在 `PBZDebugMenu.ini` 的 `[PluginGrants] 插件id = read,write,…`）。插件在 `describe_ex` 里申请：

```c
PbzPluginInfoEx info = {0};
info.size = sizeof(info);
info.id = "my.plugin"; info.name = "我的插件"; info.version = "1.0"; info.min_api = 1;
info.min_revision = 2;                                  // 需要模块接口
info.caps = PBZ_CAP_READ | PBZ_CAP_WRITE;               // 申请的能力
info.caps_reason = "显示并调整身材预设";                  // 列表里显示给玩家看的一句话
if (api->describe_ex(self, &info) != PBZ_OK) return PBZ_E_STATE;
```

| 能力 | 管什么 |
| --- | --- |
| `PBZ_CAP_COMBO` | 派生的一切：节点、信号、事件、控制派生链、编辑派生表、7 个选项、写 `[NpcCombo]` 和 `[Plugin.插件id]` 两段设置。**总是授予**，旧的 `describe` 就等于只要它 |
| `PBZ_CAP_READ` | 读模块属性（`prop_get`）、订阅模块事件（`module_subscribe`）、让 UI 页的控件显示模块属性的当前值 |
| `PBZ_CAP_WRITE` | 写模块属性（`prop_set`）、调用模块动作（`action_invoke`）、UI 页上绑定了属性 / 动作的控件 |
| `PBZ_CAP_INI` | 写 `PBZDebugMenu.ini` 里除上面两段和 `[Plugins]`、`[PluginGrants]` 之外的任意一项；运行会写 ini 或文件的命令 |
| `PBZ_CAP_COMMAND` | 命令总线 `command` |
| `PBZ_CAP_UI` | 声明 UI 页 `ui_page` |
| `PBZ_CAP_UE` | 预留，没有对应的函数（见「引擎反射」） |

- 没授权的能力：对应的调用返回 `PBZ_E_DENIED`（-10），日志里记一行 `PLUGIN [id] denied: 调用名 needs 能力`（同一个插件同一个调用只记一次）。**插件仍然被载入和使用**，只是这些调用不起作用；插件可以用 `granted_caps(self)` 看现在有什么，并在 `on_caps(user, granted)` 里得知玩家改了授权（游戏线程，下一帧）。
- 授权随时可以收回，**立即生效**：已经排队但还没执行的写入、命令，到执行时再查一次，没有权限就丢弃（日志里 `skipped: no longer allowed`）。
- 玩家只能给插件**申请过**的能力。没申请的能力存了也不起作用。
- 列表函数（`module_list`、`prop_list`、`action_list`、`event_list`、`command_list`）不需要授权，插件可以先看有什么，再向玩家说明为什么需要权限。
- 所有模块写入、动作、命令都写日志：`PLUGIN [id] prop_set 模块.属性=值`、`PLUGIN [id] action_invoke 模块.动作(参数) -> ok`、`PLUGIN [id] command 命令行 -> ok`。
- **这不是沙箱。** 插件是游戏进程里的原生代码，权限只管通过这套接口能做什么，拦不住插件自己去写内存、调系统函数。只装你信任的插件。

`set_setting` 的权限：写 `[NpcCombo]` 和 `[Plugin.<插件id>]`（插件自己的设置段）只要 `PBZ_CAP_COMBO`；其他段要 `PBZ_CAP_INI`；`[Plugins]`（派生插件开关）和 `[PluginGrants]`（授权）永远只有玩家能改（`PBZ_E_ARG`）。2026-11-12 的版本允许写任意段，这是收紧。读（`get_setting`）不受限。

## 模块：读写整个 MOD

MOD 的每个功能模块（外观、身材、毛发、Boss 连战……）各自在自己的文件里注册一份**描述表**：属性、动作、事件。插件用一组通用函数读写它们，不为每个功能单独加函数。

```c
char text[4096]; int32_t needed;
api->module_list(text, sizeof(text), &needed);       // 有哪些模块
api->prop_list("npc_look", text, sizeof(text), &needed);   // 这个模块有哪些属性
api->prop_get(self, "npc_look", "skill_damage", text, sizeof(text), &needed);   // 读（PBZ_CAP_READ）
api->prop_set(self, "npc_look", "skill_damage", "5");                          // 写（PBZ_CAP_WRITE）
api->action_invoke(self, "npc_look", "restore", "", 0, text, sizeof(text), &needed);   // 动作
```

### 列表的格式

列表函数把结果写成文本：一行一项（`\n` 分隔），列用制表符 `\t`。以后可能在行尾追加列，请忽略多出来的列。文字里的制表符、换行已换成空格。

| 函数 | 列 |
| --- | --- |
| `module_list` | 名字 \| 显示名 \| 线程 \| 属性数 \| 动作数 \| 事件数 \| 说明 |
| `prop_list(模块)` | 名字 \| 类型 \| 读写 \| 最小 \| 最大 \| 默认 \| 持久化 \| 线程 \| 显示名 \| 选项 \| 说明 |
| `action_list(模块)` | 名字 \| 线程 \| 持久 \| 参数说明 \| 显示名 \| 说明 |
| `event_list(模块)` | 名字 \| 显示名 \| 附带文本的格式 |

属性的类型：`bool`（0 / 1）、`int`、`float`、`enum`（序号，选项列给出名字，逗号分隔）、`text`。读写都用文本：写 `enum` 时序号和名字都可以；`bool` 接受 `0 1 true false on off`。**范围外的值不会被截断，而是 `PBZ_E_ARG`**（整数属性不接受小数）。文本属性最长 512 字节、不含换行。「持久化」列是 `段.键`：模块自己把这个值存在 ini 的这一项里（空：不存）；写属性时模块自己存，插件不用再 `set_setting`。

### 线程

| 线程列 | 意思 |
| --- | --- |
| `game` | 状态在游戏线程上：只能在游戏线程（回调里）读，别的线程 `PBZ_E_THREAD` |
| `any` | 模块自己线程安全（原子量、自己的锁，或「请求 → 游戏线程执行 → 快照」）：任意线程读 |
| `worker` | 状态属于面板线程，读写经过模块自己的请求队列；读任意线程，生效可能晚一两个面板周期 |

**写入不在调用线程上执行**：`prop_set` 检查名字、类型、范围和权限后**排队**，下一帧在游戏线程上调模块自己的设置函数（所以紧接着 `prop_get` 读到的还是旧值）。设置函数返回失败（模块当时做不了，例如不在场景里）只写日志，`prop_set` 已经返回了 `PBZ_OK`——要知道结果就订阅对应的模块事件或再读一次属性。`action_invoke` 在游戏线程上**立即**执行并返回文本；别的线程加 `PBZ_CALL_LATER` 排队，结果以 `host / action_done` 事件回来（见下）。

每次进模块的调用都有结构化异常保护：模块在设置函数、动作里崩溃时，这次调用返回 `PBZ_E_STATE`，这个模块本次游戏内不再被插件调用（日志 `MODULE [名字] fault …`），游戏和其他模块照常。

### 模块一览

下面是 2026-11-13 注册的模块。**都没有在游戏里验证过**（只有注册表机制和假模块有自动测试）。「写」列的属性任意插件线程都可以 `prop_set`；读写的意义以对应功能页上的同名选项为准。

**`host`**（宿主，`any`）：`api_revision`（int，只读）、`mod_version`（text，只读）、`plugins_enabled`（bool，只读）、`plugins_loaded`（int，只读）。事件 `command_done`、`action_done`（只回给发起的插件，见下）。

**`combo`**（招式派生的选项，`any`）：和 `get_option` / `set_option` 同一批，外加镜头取景。

| 属性 | 类型 | 含义 |
| --- | --- | --- |
| `skill_damage` | float 0~10 | 0 跟随 NPC 技能页，否则 0.1~10 倍 |
| `grab_damage_mode` | int -1~2 | -1 跟随投技页，0 原生，1 倍率，2 每段至少最大生命的百分比 |
| `grab_damage_value` | float 0~1000 | 倍率或百分比 |
| `chain_hud` | enum off / detailed / simple | 派生显示 |
| `view_target` | enum pelvis / neck / head | 视角跟随盯的骨骼 |
| `view_scale` | enum height / ratio | 镜头取景：按身高 / 按体型比例 |
| `show_toast` | bool | 派生提示 |

**`npc_look`**（NPC 外观套用，`any`）：请求由游戏线程执行，读到的是它最近发布的状态。

| 成员 | 说明 |
| --- | --- |
| 属性 `skill_damage` | float 1~10，NPC 技能伤害倍数（`[NpcSkill] DamageMultiplier`） |
| 属性 `boss_locomotion` | bool，BOSS 自带移动动画（`[NpcRetarget] BossLocomotion`） |
| 属性 `experimental` | bool，实验性外观；关闭时正常还原 |
| 只读 | `ready` `active` `busy` `cleanup_pending`（bool）、`selected` `status`（text）、`skin` `parts`（int） |
| 动作 | `apply`（外观 id 或序号）、`restore`、`inspect`（写日志）、`skill`（释放外观的 BOSS 技能）、`list`（`序号\tid\t名字` 每行一个） |
| 事件 `look_changed` | 文本：现在套着的外观名；还原成主角本身时为空 |

**`character`**（角色替换，`any`）：只读 `ready` `active` `busy` `original` `selected` `status`；动作 `replace`（模型 id 或序号）、`restore`、`list`；事件 `swap_changed`（现在替换成的模型，还原时为空）。

**`npc_body`**（身材 / 骨骼，`any`）：UUEPBS 编辑器的数据，窗口和这里改的是同一份；改动在下一次姿势计算时生效；主角变身成 BOSS 期间编辑暂停。

| 成员 | 说明 |
| --- | --- |
| 属性 `enabled` | bool，`[NpcBody] Enabled`；关闭时不碰主角的身体。打开编辑器会自动打开 |
| 属性 `edits_on` | bool，编辑是否生效（只是这次游戏的开关，不保存） |
| 只读 | `editor_started` `editor_visible`（bool）、`bones_edited` `morphs_set` `rigs`（int）、`hook`（text） |
| 动作 `bone` | 参数 `骨骼名 长 宽 深 [转X 转Y 转Z [移X 移Y 移Z]]`：沿骨骼自己的 X（长）、Y（宽）、Z（深）缩放，可选转角（度）和位移（厘米）；超过编辑器自己的上下限会被截断 |
| 动作 `clear_bone` `clear_bones` `bones` | 清一根 / 全部；`bones` 每行 `骨骼名 长 宽 深 转X 转Y 转Z 移X 移Y 移Z` |
| 动作 `morph` `clear_morph` `clear_morphs` `morphs` | 形变权重（-2 到 2）；`morph 形变名 权重` |
| 动作 `preset` | `save\|load\|delete 预设名`（预设文件在 `UUEPBS Presets\`，和编辑器同一份；会写文件，动作列标「持久」） |
| 动作 `presets` `editor` | 预设名列表；`editor show\|hide\|toggle` 显示 / 隐藏编辑器窗口 |

**`bones`**（骨骼变换运行时，`worker`）：属性 `supplement`（bool，补充骨骼，`[BoneMotion] Supplement`）、`debug`（bool，骨骼调试显示，不保存）；只读 `listening` `hooks` `matched` `listen_note` `mesh`；事件 `pose_finalized`（引擎每算完一次姿势后，合并成**每帧最多一个**；文本是累计次数）。命令 `bones.status`、`bones.collisionprobe`。

**`boss_rush`**（Boss 连战，`any`）：

| 成员 | 说明 |
| --- | --- |
| 属性 | `override_rank` `rank` `override_cycle` `cycle_number`（从 1 数）`override_difficulty` `difficulty`（游戏原生枚举值，不是下拉框序号，可用值见动作 `difficulties`）`auto_next` `heal_on_start`；写进去的值模块不接受（例如超出 Rank 范围）时只写日志 |
| 只读 | `ready` `running` `busy` `transitioning`（bool）、`current` `completed` `total` `rank_min` `rank_max`（int）、`status`（text） |
| 动作 | `teleport` `direct` `start`（Boss id 或序号，省略为当前）、`next` `retry` `stop` `refresh`、`list`、`difficulties` |
| 事件 `rush_changed` | `running=0/1 current=序号 completed=已完成/总数` |
| 命令 | `boss_rush.status` |

**`boss_ai`**（Boss AI 参数，`any`）：只读 `ready` `target` `status` `parameters`；动作 `list`（`序号 key 显示名 当前 基准 最小 最大 改过`）、`set`（`key或序号 值`，值在最小到最大之间）、`refresh`、`restore`。

**`air_skills`**（空中技能 / JC，`any`）：属性 `air_skills` `stomp_jump` `slow_fall` `reward_shachi` `reward_pure_shachi` `reward_spirit_shadow` `reward_skills` `reward_weapon_switch` `reward_guard_dodge`（bool）、`bounce_speed`（float 100~3000）、`enemy_radius`（float 10~1500）；只读 `ready` `active` `restore_pending` `bounces` `jump_cancels` `status` `jc_status`；命令 `air_skills.status`、`air_skills.tags`。改动由游戏线程在下一帧应用并保存。

**`grab`**（投技，`any`）：属性 `enabled` `allow_bosses` `position_correction` `tang_execution`（`[Grab] TangExecution`）`transform`（bool）、`variant`（int，招式序号，见动作 `variants`）、`range`（float 0~1000）；只读 `ready` `active` `paired` `cleanup_pending` `disabled` `attempts` `activations` `status`；动作 `trigger`、`cancel`、`variants`；命令 `grab.status`。

**`weapons`**（武器预设，`any`）：属性 `enabled`（bool，第二套预设）；只读 `ready` `busy` `can_save` `current_set`（1 或 2）`swaps` `failures` `status` `read_status` `temporary`；动作 `swap` `copy_current_to_other` `read_current` `save_to_set` `switch_to_set`（`1` 或 `2`）`take_temporary`（`武器id [shaqi]`）`restore_temporary`；事件 `weapon_changed`（文本：装备的主武器 id）；命令 `weapons.status`。装备变动由游戏线程按原生流程做（先校验、失败回滚）。

**`pad`**（手柄，`any`）：属性 `triggers_forced_off`（bool，`[Controller] ForceAdaptiveTriggersOff`）；只读 `triggers_status`。

**没有模块接口的功能**：派生表本身（用 `edit_*`）、联机测试、调试补充页、Boss 援护 / 测试木桩 / 招式预览、快捷键绑定、标题画面控制（含退出游戏，故意不开放）、毛发 / 面板的开关（见命令总线）。

### 模块事件

```c
api->module_subscribe(self, "npc_look", "look_changed", 1);     // 需要 PBZ_CAP_READ
// set_callbacks 里的 on_module_event(user, const PbzModuleEvent* e)：e->module、e->event、e->text
```

- 事件在模块发出的**下一帧**、在游戏线程上递送（宿主最多积攒 512 条，多的丢弃）。字符串只在回调里有效。
- `module` / `event` 为 `NULL` 或 `"*"` 表示全部。每个插件最多 64 条订阅。玩家收回读权限后不再递送。
- 没有自己事件钩子的模块（外观、角色替换、武器、Boss 连战……）的事件是**轮询快照**得到的：宿主每帧在游戏线程上比较一次，所以只在有插件订阅时才有这个开销，状态在一帧内来回变化会看不到。
- `host / command_done`（命令的答案）和 `host / action_done`（排队的动作的答案）不用订阅，**只回给发起的插件**。文本：`ok` 或 `fail`，TAB，命令行（动作是 `模块.动作 参数`），TAB，输出文本。

骨骼姿势事件 `bones / pose_finalized`：引擎每算完一次姿势（在姿势 finalize 的线程上）宿主只做一个原子计数，轮询再合并成每帧最多一个事件。内部的姿势监听从单槽改成了多槽（`BoneMotionRuntime::AddListener`，最多 3 个，加上身材 / 骨骼编辑器自己的 1 个），后装的监听不再覆盖先装的。插件**不能**在姿势上写东西，只能得知「姿势算完了」。

## 命令总线

`PollCommand`（面板的命令文件）和各模块 `HandleCommand` 里确认安全的命令，登记在一张**白名单**里，插件通过 `command(self, 命令行, flags, buf, size, &needed)` 运行。命令行写法同命令文件：`命令名 参数`，命令名形如 `bones.status`。不在白名单里的一律 `PBZ_E_NOT_FOUND`——不能用它跑任意命令，也包括命令文件里的 `title quit` 这类。

```c
api->command_list(text, sizeof(text), &needed);   // 命令名 | 线程 | 种类 | 参数说明 | 说明
api->command(self, "boss_rush.status", 0, text, sizeof(text), &needed);   // 需要 PBZ_CAP_COMMAND
```

| 种类 | 含义 | 权限 |
| --- | --- | --- |
| `diagnostic` | 只输出文字或写日志，不改任何跨游戏保留的东西 | `PBZ_CAP_COMMAND` |
| `state` | 改当前状态（不保存） | `PBZ_CAP_COMMAND` |
| `persist` | 会写 ini 或文件 | `PBZ_CAP_COMMAND` + `PBZ_CAP_INI` |

线程：属于 `game` / `any` 的命令在游戏线程上**立即**运行，输出文本写进 `buf`，失败返回 `PBZ_E_STATE`（`buf` 里是原因）。属于 `worker`（面板线程）的命令，以及加了 `PBZ_CALL_LATER` 的调用（任意线程），**排队**：面板线程每个周期取一次（`ComboPlugins::PumpWorker`），答案以 `host / command_done` 事件回来；调用本身返回 `PBZ_OK`，`buf` 为空。没有缓冲区也没关系：`buf = NULL`、`size = 0` 表示不要输出文本（命令照样运行）。

已登记的命令（都在游戏里**未验证**）：`boss_rush.status`、`air_skills.status`、`air_skills.tags`、`grab.status`、`weapons.status`、`bones.status`、`bones.collisionprobe`（诊断，前面几个同时写日志）；面板线程的 `hair.toggle_mesh_groom` `hair.toggle_bounds` `hair.toggle_force` `hair.toggle_outfit` `hair.restore`（状态）、`hair.toggle_triggers`（持久）、`hair.status`（诊断）；`hub.show 页名` `hub.toggle` `hub.hide`（状态）、`hub.status`（诊断；页名：`boss_rush boss_ai character npc combo air grab groom weapons hotkeys supplement online donate body`）。

## 插件 UI 页

插件可以在「派生插件」列表里放自己的一页：一组 label / toggle / slider / choice / button / text 控件。**由宿主用列表自己的行绘制**（不暴露窗口句柄）；玩家点一行就操作这个控件。需要 `PBZ_CAP_UI`。

```c
PbzUiItem toast = {0}; toast.size = sizeof(toast);
toast.kind = PBZ_UI_TOGGLE; toast.id = "toast"; toast.label = "派生提示";
toast.bind = "combo.show_toast";                 // 绑定到模块属性：宿主自己读写，不用写事件代码
PbzUiItem hello = {0}; hello.size = sizeof(hello);
hello.kind = PBZ_UI_BUTTON; hello.id = "hello"; hello.label = "说一句";   // 没有 bind：按下时 on_ui
const PbzUiItem* items[] = { &toast, &hello };
PbzUiPage page = {0}; page.size = sizeof(page);
page.id = "main"; page.title = "我的页"; page.items = items; page.item_count = 2;
api->ui_page(self, &page);                       // 游戏线程；同 id 再调一次＝整页替换
```

| 控件 | 说明 | 绑定（`bind`） |
| --- | --- | --- |
| `PBZ_UI_LABEL` | 一行文字（`text`）；`label` 是前面的小标题 | `模块.属性`：显示属性的当前值 |
| `PBZ_UI_TOGGLE` | 开关，`value` 0 / 1 | `模块.属性`（bool） |
| `PBZ_UI_SLIDER` | `min` `max` `step` `value`；`text` 是单位；列表里是「加 / 减」两行（每点一次一个 `step`） | `模块.属性`（int / float）；不填范围时取属性自己的 |
| `PBZ_UI_CHOICE` | `text` 是逗号分隔的选项名，`value` 是序号；点一下换下一个 | `模块.属性`（enum）；选项取属性的 |
| `PBZ_UI_BUTTON` | `label` 是按钮上的字 | `模块:动作`：按下时以 `text` 为参数运行这个动作 |
| `PBZ_UI_TEXT` | 多行只读文字（`\n` 换行），用来显示状态、日志 | `模块.属性`：显示属性的当前值 |

- 每个插件最多 8 页，每页 1~64 个控件。控件 id、页 id：字母、数字、`_ . -`，1~48 字节。绑定的模块、属性、动作必须已经注册，类型要对得上，只读属性只能给 label / text 用，否则 `ui_page` 返回 `PBZ_E_ARG` / `PBZ_E_NOT_FOUND`。
- 玩家操作一个**绑定**的控件：宿主按插件的权限把值写进模块属性（需要 `PBZ_CAP_WRITE`，范围外的值丢弃），或运行动作；控件显示的值游戏线程每秒几次从模块读回（需要 `PBZ_CAP_READ`）。**没有绑定**的控件：下一帧在游戏线程上调 `on_ui(user, const PbzUiEvent*)`（页、控件、类型、新值；按钮的值是 1）。
- 插件用 `ui_set_value` / `ui_set_text`（任意线程）更新控件显示的值 / 文字。
- 插件出错被停用时，它的页一起消失。

## 引擎反射（没有实现）

计划里的最后一项是薄薄地包一层宿主的 `ue::` 层（找对象、调函数、读写属性），只在游戏线程、有异常保护、默认关、要玩家另外打开 `[Plugins] AllowUe`。评估后**没有实现**，理由：

1. **类型信息不够。** `ue::` 层只给得出属性名、偏移和大小，没有属性类别和位域掩码。`bool`（位域）、`float` / `int`、枚举、对象指针都是 4 或 8 字节，让插件说明类型就等于让它自己保证内存布局；写错类型就是往游戏对象里写错误的字节，异常保护拦不住（不触发访问冲突）。
2. **对象句柄会悬空。** 宿主只能存裸 `UObject*`，`ue::Alive` 只能判断「还在对象数组里」，对象被回收后同一个槽可能换成别的对象，写进去的是另一个对象。需要序号校验，而 `ue::` 层没有暴露它。
3. **函数调用的参数打包没有边界。** `ProcessEvent` 能跑任何 UFunction，包括退出游戏、切图、改存档；字符串、数组、结构体参数要分配引擎内存。任何一个都不能靠「默认关」解决。
4. **无法在这里验证。** 它只在固定版本的游戏里有意义，没有无窗口的测试办法，而写错一次就是游戏崩溃或存档损坏。

要让插件做某件具体的事，请在对应模块的 `*_api.cpp` 里加一个**具体的属性或动作**（有类型、有范围、可测试），而不是开一个通用入口。`PBZ_CAP_UE` 和 `[Plugins] AllowUe` 这个键名保留，`has_feature("ue")` 永远是 0。

## Lua 脚本（接口修订 3）

不想编 DLL 的话，可以直接写 Lua 脚本。MOD 内置 Lua 5.4.7，读取 `version.dll` 旁 `PBZPlugins\scripts\` 里的所有 `*.lua`，每个脚本是**一个插件**，和 DLL 插件在同一个插件列表里，用同一套权限（`[PluginGrants]`），回调也都在游戏线程上。脚本里有一个全局表 `pbz`（同一个表也叫 `PBZ`，名字沿用 UE4SS 适配版的 `PBZ` 表，所以那边的脚本基本能直接搬过来），函数和上面的 C 接口一一对应。

```lua
pbz.Init{ name = "我的脚本", caps = { "read", "write" }, caps_reason = "调整派生提示" }

pbz.RegisterAction({
    id = "hero_low", name = "主角血量低", kind = "decide", outputs = { "低于", "不低于" },
    value = { label = "血量低于", unit = "%", min = 1, max = 100, step = 5, default = 30 },
}, function(call)
    local hero = pbz.GetHero()
    return (hero.valid and hero.health * 100 / hero.health_max < call.value) and 0 or 1
end)
```

样例在 `sdk/samples/lua/`：`condition.lua`（判断节点）、`signal.lua`（插件信号）、`panel.lua`（读写模块、订阅事件、画一页）。随包的 `PBZPlugins\scripts\` 里也有这三个。

### 什么时候读、怎么重新读

- 「派生插件」打开后的第一帧读一次文件夹（按文件名排序，最多 64 个文件、32 个脚本，每个文件不超过 1 MB）。「派生插件」关着时不读、不跑。`PBZDebugMenu.ini` 的 `[Plugins] Scripts = 0` 可以单独关掉 Lua（缺省是 1，只在「派生插件」打开时才起作用）。
- **热重载**：把所有脚本停掉（它们注册的信号、动作、UI 页、订阅都撤掉，Lua 状态关闭），再读一遍文件夹；DLL 插件不动。下一帧在游戏线程上做。三种触发方式：插件列表里的「重新读取脚本」，命令 `plugins.reload_scripts`（命令总线，需要 `PBZ_CAP_COMMAND`；也是 `host` 模块的动作 `reload_scripts`），命令文件里的 `plugins reload`（`plugins status` 写一行状态到日志）。
- 脚本的 id 是 **`lua.<文件名（不带 .lua）>`**，由文件名决定，脚本不能自己取名字（`Init` 里的 `id` 字段被忽略），所以脚本不会继承别的插件的授权；授权按文件名保存，改文件内容不影响。文件名只能用字母、数字、`_ . -`，最长 44 字节，否则脚本在列表里显示原因，不载入。保存在派生表里的节点动作 id 是 `lua.<文件名>/<动作 id>`。

### `pbz` 表

约定同 UE4SS 适配版：**函数不抛错**，失败时返回 `nil, 结果码, 名字`（例如 `nil, -10, "DENIED"`），成功返回值或 `true`。参数是表时按字段名取。

| 分类 | Lua 函数 | 对应的 C 函数 / 说明 |
| --- | --- | --- |
| 自身 | `Init{ name, version, author, description, min_revision, caps = {"read", ...}, caps_reason }` | `describe_ex`，只在脚本载入时（顶层）有效；也叫 `Describe` |
| | `Id()` `Version()` `Revision()` `HasFeature(名)` `Caps()` `Enabled()` `Now()` `Available()` `Started()` | `Version()` 返回接口版本、MOD 版本、接口修订；`Caps()` 返回 `{combo=true, read=false…}` 和位掩码 |
| | `Log(文字)` `Notify(文字)` | `log`、`notify`；脚本的 `print` 也写日志。日志每秒最多 200 行、提示最多 4 条，多的丢掉并写一行说明 |
| 信号 | `RegisterSignal{ id, name, group, description }` → 键码；`UnregisterSignal` `SignalCode` `FireSignal` `SetSignal(id, 按下)` `SignalWanted` | 对应 `register_signal` 等 |
| 节点动作 | `RegisterAction({ id, name, group, description, kind = "decide"\|"run", outputs = {...}, value = { label, unit, min, max, step, default } }, 函数)`；`UnregisterAction` | `register_action`。函数也可以写在表里（`decide =` / `run =` 字段）。`decide(call)` 返回出口序号（从 0 数，也可以返回 `true` = 0、`false` = 1，其他或 `nil` 不走）；`run(call)` 没有返回值。`call` 是 `{node, trigger, button, value, press_bits, press_npc_key, output_count, action, table, now_ms}` |
| 回调 | `OnEvent` `OnHeroAbility` `OnTick` `OnEnabled` `OnModuleEvent` `OnUi` `OnCaps` `OnStart`（`nil` 清除） | 对应 `set_callbacks` 的各个回调。`OnHeroAbility` 的函数返回 `true`（或 `PBZ.CONSUME`）取消这个技能；`OnCaps(mask, caps)`；`OnStart` 在脚本顶层跑完后调用一次 |
| 读派生链 | `GetState()` `NodeCount()` `GetNode(i)` `GetRoute(节点, 行)` `GetHero()` `WeaponMoves()` | `get_state` 等，返回表 |
| 控制 | `Press(按键)` `PlayNode(节点 [, 按键])` `ResetChain([原因])` `CancelHeroAbility(句柄)` | `press` 等（排队，下一帧执行） |
| 派生表 | `ListTables()` `ExportTable(表)` `ImportTable(表, 文本)`；`EditBegin(表)` → 句柄；`EditCommit` `EditCancel` `EditNodeCount` `EditGetNode` `EditGetRoute` `EditAddNode(句柄, 类型 [, x, y])` `EditRemoveNode` `EditGetField(句柄, 节点, 行, 字段)` `EditSetField(句柄, 节点, 行, 字段, 值)` `EditAddRoute` `EditRemoveRoute` `EditLink(句柄, 从, 出口, 到)` `EditLinkModifier` `EditUnlinkModifier` `EditExport` `EditImport` `FieldNames([行字段])` | `edit_*`；值可以是文本、数字、布尔 |
| 选项与设置 | `GetOption(名)` `SetOption(名, 数)` `GetSetting(段, 键)` `SetSetting(段, 键, 值)` | `get_option` 等；`SetSetting` 的权限同 C 接口（自己的段 `Plugin.lua.<文件名>` 默认可写） |
| 模块（修订 2） | `ModuleList()` `PropList(模块)` `ActionList(模块)` `EventList(模块)` `CommandList()` | 返回表的数组，字段名同列表的列（`name`、`type`、`min`……；数字列是数字，`persistent` 是布尔）。不需要授权 |
| | `PropGet(模块, 属性)` | 按属性类型返回布尔 / 整数 / 小数 / 文本；需要 `read` |
| | `PropSet(模块, 属性, 值)` | 值可以是布尔、数字、文本；排队写入，需要 `write` |
| | `ActionInvoke(模块, 动作 [, 参数 [, later]])` | 返回动作的文本；失败时第三个返回值是动作给的原因；`later = true` 排队，答案是 `host / action_done` 事件。需要 `write` |
| | `Subscribe(模块 [, 事件 [, 开关]])` | `module_subscribe`，需要 `read`；事件到 `OnModuleEvent` 的函数，参数 `{module, event, text, now_ms}` |
| | `Command(命令行 [, later])` | 命令总线，需要 `command`（`persist` 命令另需 `ini`） |
| UI 页 | `UiPage{ id, title, items = { {kind = "label"\|"toggle"\|"slider"\|"button"\|"choice"\|"text", id, label, text, bind, min, max, step, value}, ... } }` `UiRemovePage(id)` `UiSetValue(页, 控件, 值)` `UiSetText(页, 控件, 文字)` | 需要 `ui`；`bind` 绑定模块属性 `"模块.属性"` 或动作 `"模块:动作"`；没绑定的控件被玩家操作时调 `OnUi`，参数 `{page, item, kind, value, now_ms}` |
| 常量 | `pbz.BUTTON.*` `pbz.NODE.*` `pbz.EVENT.*` `pbz.ACTION.*` `pbz.PHASE.*` `pbz.RESULT.*` `pbz.CAP.*` `pbz.UI.*`，`pbz.PASS` `pbz.CONSUME` | 同 C 头文件里的 `PBZ_*` |

事件、动作、命令的线程和时序同 C 接口：回调都在游戏线程，写入排队到下一帧，模块事件延后一帧递送。

### 限制和安全

脚本是不受信任的文本，所以和 DLL 不同：

- **标准库**：只有 `base`、`coroutine`、`table`、`string`、`math`、`utf8`。**没有** `io`、`package`（`require`、`loadlib`、`cpath`）、`debug`，`os` 只剩 `os.time`、`os.clock`、`os.difftime`（没有 `os.execute`、`os.remove`、`os.getenv`、`os.date`）。`dofile`、`loadfile`、`string.dump` 没有；`load` 只读文本，不读字节码；`collectgarbage` 只认 `"collect"` 和 `"count"`；`string.rep` 的结果不能超过 1 MB。这些库是**编译时就没有放进 `version.dll`**的（见 `THIRD_PARTY_NOTICES.md`），不是运行时删掉的。
- **指令预算**：每次进脚本的回调（`decide`、`run`、事件、`OnTick`……）最多跑 20 万条虚拟机指令（大约一两毫秒），脚本的顶层载入 500 万条。用完了这次调用以错误结束，脚本被停用。`pcall` / `xpcall` 会把这个错误继续往外抛，所以脚本拦不住它；协程里的死循环也一样。**注意**：预算只数 Lua 指令，不数宿主 C 函数里花的时间，一个很糟的字符串模式（`string.find` 的回溯爆炸）仍然会卡住游戏线程，请不要在脚本里做。
- **内存上限**：每个脚本 16 MB（Lua 自己的分配），超了脚本里是 `not enough memory` 错误。
- **错误**：回调里出错（包括预算用完），这个脚本被停用：写**一行**日志 `PLUGIN [lua.名字] script error in 回调名: 信息 (switched off for this session)`，右下角提示一次，插件列表里这个脚本显示「脚本出错，本次游戏内已停用：信息」；别的脚本和 DLL 照常。载入时出错（语法错误、顶层出错、文件名不合法）：脚本不被使用，列表里显示原因（Lua 的信息带文件名和行号）。要恢复，改好文件后「重新读取脚本」。
- **线程**：脚本只在游戏线程上被调用。
- Lua 的字符串是字节串，中文直接写 UTF-8（把 `.lua` 存成 UTF-8，带不带 BOM 都行）。

### 给面板用的函数

面板（`combo_menu.cpp` 的插件列表）用到的接口：`ComboPlugins::Plugins()` 里每个 `PluginView` 的 `script`（是 Lua 脚本，列表上可以加 Lua 标记）、`file`（脚本文件名）、`loaded` / `faulted` / `error`（状态；`error` 是为什么没在用）；`ComboPlugins::RequestScriptReload()`（任意线程，「重新读取脚本」按钮用）、`ScriptCount()`、`ScriptsLoaded()`、`ScriptFolder()`、`ScriptsEnabled()`。权限行和插件页同 DLL 插件。

**未在游戏中验证**：宿主、沙箱、预算、热重载和 `pbz` 表用 `lua_host_test`（假链、假模块、示例脚本）测过；在游戏里跑真实的派生链、模块和面板，没有试过。

## 由别的加载器载入的插件（PbzAttachPlugin）

不放在 `PBZPlugins` 里、由别的加载器载入游戏的 DLL（例如 UE4SS 的 C++ mod）也可以成为派生插件：MOD 的 `version.dll` 导出了 `PbzAttachPlugin`（序号 1001）。

```cpp
#include "pbz_combo_api.h"
#include <windows.h>
#include <psapi.h>

// 在游戏进程的模块里找导出了 PbzAttachPlugin 的那个（豪之刃的 version.dll）。
static PbzAttachPluginFn FindAttach() {
    HMODULE modules[1024]; DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return nullptr;
    for (DWORD i = 0; i < bytes / sizeof(HMODULE); ++i)
        if (auto fn = reinterpret_cast<PbzAttachPluginFn>(GetProcAddress(modules[i], "PbzAttachPlugin"))) return fn;
    return nullptr;
}

PbzResult PBZ_CALL MyMain(const PbzApi* api, PbzPlugin* self);   // 写法和 PbzPluginMain 完全一样，不用导出

void Start() {
    if (PbzAttachPluginFn attach = FindAttach()) attach(&MyMain, L"我的 UE4SS 插件");
}
```

- `PbzAttachPlugin(entry, name)` 可以在任意线程调用。入口函数在「派生插件」打开后的下一帧、在游戏线程上调用，之后和 `PBZPlugins` 里的插件完全一样（插件列表里名字前面有「[外部]」）。
- 「派生插件」关着时入口函数等到它打开才调用；返回 `PBZ_OK` 只表示已登记。
- 和 `PBZPlugins` 里的插件合计最多 32 个；`entry` 为空返回 `PBZ_E_ARG`。

UE4SS 的 Lua mod 不能直接调用 C 接口，用下一节的豪之刃适配版 UE4SS。（MOD 自己的 Lua 脚本见上面的「Lua 脚本」：它不需要 UE4SS，也是插件，两者可以并存。）（适配版是单独发布的，**它的 `PBZ` 表还没有修订 2 的函数**：模块、权限、命令、UI 页现在只有 C 接口；适配版更新后再补。）

## UE4SS Lua

豪之刃适配版 UE4SS（单独发布，不在本 MOD 的安装包里）做了两件事：

- **能在《影之刃零》上启动**。原版 UE4SS 在这个游戏（UE 5.7）上找不到 GUObjectArray 和 GNatives，扫描超时后不工作。适配版附带这两个签名（`UE4SS_Signatures\GUObjectArray.lua`、`GNatives.lua`）和对象数组的布局（`MemberVariableLayout.ini`：这个版本的 ObjObjects 在 GUObjectArray +0x18，按 5.8 的字段顺序）。签名针对 2026-11-04 的游戏版本，游戏更新后需要重新找。
- **每个 Lua mod 都有一个全局表 `PBZ`**，就是本手册 C 接口的 Lua 版：一个调用了 `PBZ.Init` 的 Lua mod 就是一个派生插件，在插件列表里显示为「Lua: 名字」。

### 安装

把适配版的 `dwmapi.dll` 和 `ue4ss` 文件夹放到 `PBZ.exe` 旁边（和豪之刃的 `version.dll` 同一个文件夹，两者互不影响），Lua mod 放进 `ue4ss\Mods\`，在 `ue4ss\Mods\mods.txt` 里写一行 `你的mod名 : 1`。示例 mod `PBZComboExample` 已经装好。

### 写法

```lua
-- ue4ss\Mods\MyCombo\scripts\main.lua
local ok, code, err = PBZ.Init{ id = "me.combo", name = "我的派生插件", version = "1.0", author = "我" }
if not ok then print("PBZ.Init: " .. tostring(err) .. "\n") return end

-- 判断节点：返回出口序号（从 0 开始），-1 不走任何出口
PBZ.RegisterAction({ id = "low_hp", name = "残血？", kind = "decide", outputs = { "残血", "健康" },
                     value = { label = "血量低于", unit = "%", min = 1, max = 100, step = 5, default = 30 } },
    function(call)
        local h = PBZ.GetHero()
        if not h or not h.valid or h.health_max <= 0 then return -1 end
        return (h.health * 100 < call.value * h.health_max) and 0 or 1
    end)

-- 信号：UE4SS 的按键绑定按下它
PBZ.RegisterSignal{ id = "f6", name = "F6" }
RegisterKeyBind(Key.F6, function() PBZ.FireSignal("f6") end)

-- 事件
PBZ.OnEvent(function(e)
    if e.type == PBZ.EVENT.CHAIN_RESET then print("chain reset: " .. e.text .. "\n") end
end)
```

### 规则

- **启动**：`PBZ.Init` 只是登记。「派生插件」打开后的下一帧，豪之刃在游戏线程上启动这个插件：这时才把注册的信号、动作和回调交给 MOD，并调用 `PBZ.OnStart` 的函数。`PBZ.Started()` 返回是否已启动。在那之前，读取和控制派生链的函数返回 `nil, PBZ.RESULT.STATE, 说明`。
- **注册**：`RegisterSignal`、`RegisterAction`、`On*` 在 `Init` 之后随时可以调用（任意线程），在下一帧的游戏线程上生效；同一个 id 再注册就是更新。
- **回调**：判断、动作、事件、技能、每帧、开关的 Lua 函数都在游戏线程上调用，和 UE4SS 自己的游戏线程钩子用同一把锁，所以不会和 UE4SS 的其他 Lua 回调同时运行。Lua 出错只写一行日志（`[PBZ] [插件id] decide 动作id: 错误`），这次判断不走任何出口，插件不会被停用。
- **失败**：`PBZ` 的函数从不抛出 Lua 错误；失败时返回 `nil, 结果码, 说明`，结果码见 `PBZ.RESULT`。
- **序号**：节点、行、出口、按键的序号和 C 接口一样从 0 开始（不是 Lua 习惯的 1）；`ListTables`、`FieldNames`、`WeaponMoves` 返回的数组从 1 开始。
- **重载**：UE4SS 重新载入这个 Lua mod 时（热重载、RestartMod），旧的 Lua 状态关闭前插件的动作、信号和回调被撤下；新状态再次 `PBZ.Init` 同一个 id 时接回同一个插件。插件本身不会从游戏里卸载。最多 16 个 Lua 插件。

### 函数

| 函数 | 返回 | 对应的 C 函数 / 说明 |
| --- | --- | --- |
| `PBZ.Init{ id, name, version, author, description }` | `true` | 登记插件（`PbzAttachPlugin`）。找不到豪之刃时 `NOT_FOUND` |
| `PBZ.Available()` | 布尔 | 游戏里有没有豪之刃的 `version.dll` |
| `PBZ.Started()` | 布尔 | 插件是否已被启动 |
| `PBZ.Version()` | 接口版本, MOD 版本 | `api->version`、`api->mod_version` |
| `PBZ.Log(text)` / `PBZ.Notify(text)` | `true` | `log`（未启动时写 UE4SS 控制台）/ `notify` |
| `PBZ.Enabled()` / `PBZ.Now()` | 布尔 / 毫秒 | `enabled` / `GetTickCount64` |
| `PBZ.RegisterSignal{ id, name, group, description }` | 键码 | `register_signal`（键码按完整 id 算出） |
| `PBZ.UnregisterSignal(id)` / `PBZ.SignalCode(id)` | `true` / 键码 | `unregister_signal` / 注册时的键码 |
| `PBZ.FireSignal(id)` / `PBZ.SetSignal(id, down)` | `true` | `fire_signal` / `set_signal` |
| `PBZ.SignalWanted(id)` | 布尔 | `signal_wanted` |
| `PBZ.RegisterAction(desc, fn)` | `true` | `register_action`；desc：`id`、`name`、`kind`（`"decide"` / `"run"`）、`outputs`（数组，1~6）、`value`（`{ label, unit, min, max, step, default }`）、`group`、`description`；`fn(call)` 见下 |
| `PBZ.UnregisterAction(id)` | `true` | `unregister_action` |
| `PBZ.OnEvent(fn)` / `PBZ.OnHeroAbility(fn)` / `PBZ.OnTick(fn)` / `PBZ.OnEnabled(fn)` / `PBZ.OnStart(fn)` | `true` | 回调；`nil` 清除。`OnHeroAbility` 的函数返回 `true`（或 `PBZ.CONSUME`）取消这个技能 |
| `PBZ.GetState()` / `PBZ.NodeCount()` / `PBZ.GetNode(i)` / `PBZ.GetRoute(node, row)` / `PBZ.GetHero()` | 表 / 数 | 读取，字段名同 C 结构体 |
| `PBZ.Press(button)` / `PBZ.PlayNode(node[, button])` / `PBZ.ResetChain([reason])` / `PBZ.CancelHeroAbility(handle)` | `true` | 控制 |
| `PBZ.ListTables()` / `PBZ.ExportTable(name)` / `PBZ.ImportTable(name, text)` | 数组 / 文本 / `true` | 整表 |
| `PBZ.WeaponMoves()` | 数组 | `{ weapon, ability, kind, shaqi, stage }` |
| `PBZ.EditBegin(table)` | 会话 | `edit_begin`；之后的 `Edit*` 第一个参数都是会话 |
| `PBZ.EditCommit(e)` / `PBZ.EditCancel(e)` / `PBZ.EditNodeCount(e)` / `PBZ.EditGetNode(e, i)` / `PBZ.EditGetRoute(e, node, row)` | | 同 C |
| `PBZ.EditAddNode(e, type, x, y)` / `PBZ.EditRemoveNode(e, i)` / `PBZ.EditAddRoute(e, node)` / `PBZ.EditRemoveRoute(e, node, row)` | 序号 / `true` | 同 C |
| `PBZ.EditGetField(e, node, row, field)` / `PBZ.EditSetField(e, node, row, field, value)` | 文本 / `true` | 值可以是文本、整数或布尔 |
| `PBZ.EditLink(e, from, output, to)` / `PBZ.EditLinkModifier(e, from, mod)` / `PBZ.EditUnlinkModifier(e, from[, slot])` | `true` | 同 C |
| `PBZ.EditExport(e)` / `PBZ.EditImport(e, text)` / `PBZ.FieldNames([rows])` | 文本 / `true` / 数组 | 同 C |
| `PBZ.GetOption(name)` / `PBZ.SetOption(name, number)` / `PBZ.GetSetting(section, key)` / `PBZ.SetSetting(section, key, value)` | | 同 C |

`fn(call)` 的 `call`：`node`、`trigger`、`button`、`value`、`press_bits`、`press_npc_key`、`output_count`、`action`、`table`、`now_ms`。判断函数返回出口序号；动作函数的返回值不用。

常量：`PBZ.BUTTON.LIGHT` 等（同 `PBZ_BUTTON_*`）、`PBZ.NODE.*`、`PBZ.EVENT.*`、`PBZ.ACTION.RUN` / `DECIDE`、`PBZ.PHASE.*`、`PBZ.RESULT.*`、`PBZ.PASS`、`PBZ.CONSUME`。


## 函数参考

| 函数 | 线程 | 返回 / 说明 |
| --- | --- | --- |
| `describe(self, info)` | 入口 | 入口里第一个调用。id 不合法 `PBZ_E_ARG`，id 已被别的插件用 `PBZ_E_EXISTS`，`min_api` 太高 `PBZ_E_VERSION` |
| `set_callbacks(self, callbacks)` | 游戏 | 登记 / 替换回调 |
| `log(self, text)` | 任意 | 写入 `PBZDebugMenu.log`（前面加 `PLUGIN [插件id]`） |
| `notify(self, text)` | 任意 | 画面右下角提示（下一帧显示） |
| `now_ms()` | 任意 | `GetTickCount64()` |
| `enabled()` | 任意 | 1：派生插件开着 |
| `register_signal(self, desc, &code)` | 游戏 | 注册 / 更新信号，`code` 为键码 |
| `unregister_signal(self, id)` | 游戏 | 注销信号 |
| `fire_signal(self, id)` | 任意 | 按一下 |
| `set_signal(self, id, down)` | 任意 | 按住 / 松开 |
| `signal_wanted(self, id)` | 游戏 | 1：派生链在等这个信号 |
| `register_action(self, desc)` | 游戏 | 注册 / 更新插件节点动作 |
| `unregister_action(self, id)` | 游戏 | 注销动作 |
| `get_state(state)` | 游戏 | 派生链状态 |
| `node_count()` | 游戏 | 节点数（不可用时 0） |
| `get_node(i, node)` | 游戏 | 节点 |
| `get_route(node, row, route)` | 游戏 | 行 / 出口 |
| `get_hero(facts)` | 游戏 | 主角与目标 |
| `press(self, button)` | 任意 | 送按键 |
| `play_node(self, node, button)` | 任意 | 直接播放节点 |
| `reset_chain(self, reason)` | 任意 | 结束派生链 |
| `cancel_hero_ability(handle)` | 游戏 | 取消主角技能（找不到 `PBZ_E_NOT_FOUND`） |
| `list_tables` / `export_table` / `import_table` | 任意 | 整表文本 |
| `weapon_move_count()` / `get_weapon_move(i, move)` | 任意 | 主角武器的招式表（技能名、轻重类型、段数），可以用来列出技能名 |
| `edit_*` | 任意 | 编辑会话，见上 |
| `get_option` / `set_option` | 任意 | MOD 选项 |
| `get_setting` / `set_setting` | 任意 | ini 设置；写要看权限（见「权限」） |
| **以下是修订 2 追加的** | | |
| `api_revision`（字段） | — | MOD 的接口修订；用 `PBZ_API_REVISION_OF(api)` 读（旧 MOD 给 1） |
| `has_feature(名字)` | 任意 | 1 / 0，见「接口修订与特性探测」 |
| `describe_ex(self, info)` | 入口 | 带 `min_revision`、`caps`、`caps_reason` 的 `describe`；和 `describe` 二选一（也可以先后各调一次，id 必须一致）。修订太高 `PBZ_E_VERSION` |
| `granted_caps(self)` | 任意 | 玩家现在给的能力（总含 `PBZ_CAP_COMBO`；没 describe 过为 0） |
| `module_list` / `prop_list` / `action_list` / `event_list` | 任意 | 列表文本，不需要授权；模块不存在 `PBZ_E_NOT_FOUND`，缓冲区太小 `PBZ_E_BUFFER` |
| `prop_get(self, 模块, 属性, buf, size, &needed)` | 看模块的线程 | 需要 READ；`game` 模块只能在游戏线程读（`PBZ_E_THREAD`）；模块出过错 `PBZ_E_STATE` |
| `prop_set(self, 模块, 属性, 值)` | 任意 | 需要 WRITE；检查通过就排队（`PBZ_E_ARG` 值不合法或属性只读，`PBZ_E_NOT_FOUND`，`PBZ_E_DISABLED`，`PBZ_E_FULL`），下一帧在游戏线程上写 |
| `action_invoke(self, 模块, 动作, 参数, flags, buf, size, &needed)` | 游戏（`PBZ_CALL_LATER`：任意） | 需要 WRITE；立即执行并返回文本，动作失败 `PBZ_E_STATE`；`buf = NULL` 且 `size = 0` 不要输出；`PBZ_CALL_LATER` 排队，结果是 `host / action_done` |
| `module_subscribe(self, 模块, 事件, 开关)` | 任意 | 需要 READ；每个插件最多 64 条；模块 / 事件不存在 `PBZ_E_NOT_FOUND` |
| `command_list(buf, size, &needed)` | 任意 | 白名单命令表，不需要授权 |
| `command(self, 命令行, flags, buf, size, &needed)` | 游戏（`worker` 命令和 `PBZ_CALL_LATER`：任意） | 需要 COMMAND（`persist` 命令另需 INI）；不在白名单 `PBZ_E_NOT_FOUND` |
| `ui_page(self, page)` | 游戏 | 需要 UI；注册 / 替换一页 |
| `ui_remove_page(self, 页)` | 游戏 | 删掉一页 |
| `ui_set_value(self, 页, 控件, 值)` / `ui_set_text(self, 页, 控件, 文字)` | 任意 | 需要 UI；更新控件显示的值 / 文字 |
| 回调 `on_module_event` `on_ui` `on_caps` | 游戏 | 见各章 |

结果码：`PBZ_OK 0`、`PBZ_E_ARG -1` 参数不对、`PBZ_E_STATE -2` 当前做不了、`PBZ_E_THREAD -3` 线程不对、`PBZ_E_NOT_FOUND -4` 不存在、`PBZ_E_FULL -5` 数量到上限、`PBZ_E_EXISTS -6` 已存在 / 键码冲突、`PBZ_E_BUFFER -7` 缓冲区太小、`PBZ_E_DISABLED -8` 派生插件关着、`PBZ_E_VERSION -9` 版本太低、`PBZ_E_DENIED -10` 玩家没有授权（修订 2 起）。

## 示例插件

### pbz_conditions（条件判断）

| 动作 | 类型 | 数值 | 出口 |
| --- | --- | --- | --- |
| 主角血量 `hero_hp` | 判断 | 主角血量低于 %（默认 30） | 低于 / 不低于 |
| 目标血量 `target_hp` | 判断 | 目标血量低于 %（默认 30） | 低于 / 不低于 / 没有目标 |
| 目标距离 `target_distance` | 判断 | 距离（厘米，默认 400） | 在距离内 / 更远 / 没有目标 |
| 空中 `airborne` | 判断 | — | 空中 / 地面 |
| 概率 `chance` | 判断 | 命中概率 %（默认 50） | 命中 / 未命中 |
| 提示 `notice` | 动作 | 数值 | 在画面右下角显示数值 |
| 结束派生链 `end_chain` | 动作 | — | 走到这里时结束派生链 |

例：起端 → 目标距离 400 → 「在距离内」接近身招式，「更远」接瞬移 + 招式，「没有目标」不接（按键交还游戏）。

### pbz_weapon_actions（武器动作信号）

把主角具体的武器动作变成信号。规则写在 `PBZPlugins\pbz_weapon_actions.txt`：

```
# 信号id | 显示名 | 技能名规则;规则2 | 派生链在等它时取消原动作(1/0)
air_light | 空中轻攻击 | _Air_A | 1
air_heavy | 空中重攻击 | _Air_B | 1
dash_attack | 冲刺攻击 | _Sprint;_Dash_Attack | 0
```

规则是技能名里包含的一段文字，以 `=` 开头表示必须完全相同。主角每放出一种新技能，插件在日志里写一行 `ability seen: 技能名`，照着写规则即可。改完重启游戏。

这个示例也是「武器派生修改里指定具体武器动作」这类改法的插件版：不用改 MOD 源码，用信号就能只在某个具体动作时接派生，并在派生链接住时取消原动作。

### pbz_mod_panel（模块面板，修订 2）

示例 3 演示「读写整个 MOD」的完整写法：

1. 先查 `api_revision` / `has_feature`：旧 MOD 上只用 `describe` 写一行日志就返回，不出错；
2. 用 `describe_ex` 申请读、写、命令、UI 页，并写一句理由；
3. 玩家授权前 `ui_page` 会 `PBZ_E_DENIED`，所以在 `on_caps` 里（授权变化时）再画页；
4. 页上只放这个 MOD 里**真有**的模块属性（先用 `prop_list` / `action_list` 看过）：一行显示 MOD 版本（绑定 `host.mod_version`）、「派生提示」开关（绑定 `combo.show_toast`）、「派生显示」选项（绑定 `combo.chain_hud`）、「还原主角」按钮（绑定 `npc_look:restore`）——这些都是宿主自己读写，插件没写事件代码；另有「列出模块」和「面板状态」两个按钮和一块状态文字由插件自己处理（`on_ui`）；
5. 订阅 `npc_look / look_changed`、`character / swap_changed`、`weapons / weapon_changed`，最近一次事件写在状态文字里；
6. 「面板状态」按钮用命令总线（`PBZ_CALL_LATER`）问面板线程的 `hub.status`，答案以 `host / command_done` 事件回来。

玩家要在「派生插件」列表里把它申请的四项权限逐项点开，页才会出现。没有授权时它什么都不做。

## 排错

| 现象 | 看哪里 |
| --- | --- |
| 列表里没有插件 | 「派生插件」开着吗？DLL 在 `version.dll` 旁的 `PBZPlugins` 里吗？日志里 `PLUGIN folder … : N DLL(s)` |
| 「无法载入（缺少依赖或不是 x64 DLL）」 | DLL 必须是 x64；依赖的运行库没装时改用静态运行库（`/MT`） |
| 「入口函数没有调用 describe」/「入口函数返回失败（n）」 | 入口里先调用 `describe` 并检查返回值；n 是入口返回的结果码 |
| 「运行出错，本次游戏内已停用」 | 插件崩溃了，日志里有异常码和出错的回调名；改好后重启游戏 |
| 判断节点总是不走 | `decide` 返回了 -1 或超出范围；出口没接线；「派生插件」关着 |
| 信号按了没反应 | 派生链里用到这个信号了吗（`signal_wanted`）？信号是“按一下”，按住请用 `set_signal` |
| 中文显示成乱码 | 编译时加 `/utf-8`（字符串必须是 UTF-8） |
| Lua 脚本在列表里显示「脚本出错，本次游戏内已停用」 | 后面是 Lua 的信息（脚本名、行号）；`ran too long` 是指令预算用完了（回调太慢或死循环）；`not enough memory` 是超了 16 MB。改好文件后在插件列表里点「重新读取脚本」（或命令 `plugins reload`） |
| 脚本没出现在列表里 | 「派生插件」开着吗？文件在 `PBZPlugins\scripts\`、扩展名是 `.lua` 吗？`[Plugins] Scripts` 不是 0？文件名只能用字母、数字、`_ . -`（最长 44 字节）；日志里有 `PLUGIN scripts folder …: N file(s)` |
| 脚本里调用返回 `nil, -10, "DENIED"` | 同 DLL 插件：要在 `pbz.Init{ caps = {...} }` 里申请，玩家在列表里逐项点开；授权后 `OnCaps` 会被调用 |
| 调用返回 `PBZ_E_DENIED` | 玩家没有给这个插件授权：在「派生插件」列表里点开对应的权限行；插件应在 `describe_ex` 里申请，并在 `on_caps` 里等授权 |
| 申请了权限，列表里没有权限行 | 要用 `describe_ex`（旧的 `describe` 只要派生能力）；插件要已经载入，没出过错 |
| `prop_set` 返回 `PBZ_OK` 但值没变 | 写入排队到下一帧；模块当时做不了（不在场景里、正在切换）会写日志 `PLUGIN [id] prop_set … failed: …`；范围外的值一开始就是 `PBZ_E_ARG` |
| UI 页没出现 | 要 `PBZ_CAP_UI`；绑定的模块 / 属性 / 动作要先注册（`ui_page` 返回 `PBZ_E_NOT_FOUND`）；页在列表的最下面，插件列表之后 |

MOD 的日志里与插件有关的行都以 `PLUGIN` 开头；插件节点每次被走到写一行 `NPC_COMBO plugin node=…`。
