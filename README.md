<div align="center">

<img src="docs/title.jpg" alt="豪之刃 · Cringe Blade Zero" width="100%">

# 豪之刃 · Cringe Blade Zero

**《影之刃零》（Phantom Blade Zero）单机游戏内工具箱**

一个基于 `version.dll` 转发代理的深度功能扩展 MOD：游戏内自绘面板、Boss 连战、Boss 原生投技、空中战技与 JC 系统、双武器预设与挡位成长，以及 75 位 NPC / 人形 Boss 外观实操。

[English](README_EN.md) · [简体中文](README.md)

![Platform](https://img.shields.io/badge/platform-Windows%20x64-1f2328?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square)
![Engine](https://img.shields.io/badge/Unreal%20Engine-5-0e1128?style=flat-square)
![Build](https://img.shields.io/badge/build-CMake%20%2B%20MSVC-d8c292?style=flat-square)
![License](https://img.shields.io/badge/license-MIT-3c7a3c?style=flat-square)

**[⬇ 下载最新版（CringeBladeZero.zip）](https://github.com/Jersyi/cringe-blade-zero/releases/latest/download/CringeBladeZero.zip)** · [全部版本](https://github.com/Jersyi/cringe-blade-zero/releases) · [❤ Donate](#donate--支持作者)

MOD 完全免费，不打赏也可使用全部功能。喜欢本作可在[爱发电](https://afdian.com/a/Jersyi1)自愿支持作者。

</div>

---

## 目录

- [功能模块](#功能模块)
- [默认快捷键](#默认快捷键)
- [身材与骨骼系统](#身材与骨骼系统)
- [本地替身实验](#本地替身实验开发中)
- [安装与更新](#安装与更新)
- [环境检查与排错](#环境检查与排错)
- [设置文件（INI）](#设置文件ini)
- [源码构建与测试](#源码构建与测试)
- [项目结构](#项目结构)
- [开发文档](#开发文档)
- [支持作者](#donate--支持作者)
- [许可与说明](#许可与说明)

---

## 功能模块

> [!TIP]
> 完整操作指南与全部节点详解请参阅 **[用户手册（Markdown）](docs/USER_MANUAL.md)** 或下载 **[中文 PDF 手册（60页）](https://github.com/Jersyi/cringe-blade-zero/releases/latest/download/CringeBladeZero_Manual_ZH.pdf)**。PDF 内含 36 张节点实图、9 张离线渲染自绘控件图与 32 类节点逐项解析，随最新发布包附带。

| 模块 | 核心特性 | 功能详情 |
|---|---|---|
| **面板（Hub）** | 墨色金线自绘界面<br>中英双语支持 | • 无边框自绘渲染，贴合游戏原生美术风格<br>• 按 **F10** 随时呼出/收起，可配置开启时暂停游戏<br>• 支持键盘与手柄全程无障碍操作，中英双语一键切换 |
| **快捷键系统** | 键鼠 / Xbox / PS 三套独立映射<br>智能设备识别 | • 自动感知最近使用的输入设备并在 HUD 显示对应键位图标<br>• 手柄两键同时按下触发（220ms 容差判定），避免与游戏原生键冲突<br>• 面板内直观录制，数据保存于 INI |
| **Boss 连战** | 自由传送与即刻触战<br>周目难度定制 | • 传送到任意 Boss 场景并即刻触发战斗<br>• 自定义连战顺序、轮数与难度曲线<br>• 内置入场保护与存档防护机制，防止误触导致卡死或进度污染 |
| **Boss AI** | 原生参数即时调整 | • 读取当前场景 Boss 实例的原生 AI 决策数据<br>• 支持动态微调侵略度、攻击间隔与招式权重 |
| **Boss 投技** | 53 招人形 Boss 原生投技<br>武器与演出完全还原 | • 默认按键触发（键盘 `G` / Xbox `Y+B` / PS `△+○`），倒地或受击硬直中均可施放<br>• 独立骨架代理重放，自动挂载该 Boss 专属武器，保持原汁原味受击位移与镜头演出<br>• 施放瞬间角色头部亮起原生出招红光警示（可自选关闭）<br>• 可配置倍率放大伤害与无视无敌判定<br>• 可选「投技时变身为该 Boss」，演出完毕自动无缝切回主角 |
| **空中技能 / JC** | 滞空战技与闪避<br>近敌 JC 弹跳重置普攻 | • 解锁空中施放地面战技与杀气技、空中冲刺与闪避<br>• 跳跃动作打断后摇，近敌 JC 踩怪弹跳并重置空中轻攻击次数限制<br>• 提供滞空资源奖励与可控缓落状态 |
| **武器预设** | 双套装备一键秒切 | • 默认快捷键（键盘 `Z` / Xbox `LB+L3` / PS 触控板左右滑）即时切换两套装备<br>• 完美呈现游戏内武器图标与官方定名，游戏重启后保持状态 |
| **挡位系统** | 0–10 阶动态能力成长 | • 屏幕右下角 0–10 档胶囊微件（键盘 `=` / `-` 增减）<br>• 档位越高解锁能力越多（JC、投技、资源奖励、伤害倍数、破霸体/穿无敌）<br>• 满档（10 档）点燃游戏手绘火焰动态特效 |
| **角色 / NPC** | 75 款 NPC / 人形 Boss 外观<br>动态比例适配 | • 一键将主角替换为 75 款 NPC / 人形 Boss 造型，完整保留主角全部操控与能力<br>• 手臂骨骼、武器挂点、碰撞胶囊体与镜头视距自适应 NPC 体型<br>• 原生过场动画自动套用所选外观，过场结束后平滑还原 |
| **NPC 技能** | 招式判定与特效完全继承<br>全图慢放机制 | • 套用外观后按 NPC 技能键（键盘 `X` / 手柄 `L3+R3`）释放该 Boss 原生技能<br>• 技能释放期间自动换持 Boss 武器，同步挂载攻击判定盒与武器粒子拖尾<br>• 完美复刻 Boss 专有蓝图机制（龙柱、魔渊柱、追踪弹道、反弹闪光、骑马冲锋）<br>• 释放全图慢放/时停招式（如梦华圣母、虚空）时，主角保持正常攻速，仅减速周围 100m 敌方（投技目标保持同步，不被误减速）<br>• 伤害倍率 1–10 倍自由调节（默认 3 倍） |
| **招式派生** | 32 类节点可视化自由拼装<br>时间轴精准掌控 | • 支持 BOSS 技能、武器派生、投技、援护、分支、组合、加速曲线、动作裁剪与按住停留、空中派生、骨骼变换等<br>• 节点/武器双页面板，支持实时过滤搜索、招式卡片悬停预览、多表归档与 sampleCombo 示例<br>• 节点到达触发独立停顿/间隔计时，链尾收束后接受隐式循环触发 |
| **派生插件** | DLL / Lua 5.4 动态脚本扩展<br>模块总线与权限管控 | • 动态加载 `PBZPlugins` 目录插件，扩展自定义条件分支（血量/距离/空中/概率等）与信号按键<br>• 修订 2+ 接口开放全局 MOD 读写（各模块属性、动作、事件与面板专页接入），支持安全授权机制<br>• 内置 Lua 5.4.7 引擎，`PBZPlugins\scripts\*.lua` 脚本改动无需重启游戏，即改即热重载<br>• 详见 [插件接口手册](docs/PLUGIN_API.md) 与 [插件编写指南](docs/AGENT_PLUGIN_GUIDE.md) |
| **毛发 / Groom** | 真实毛发与发片动态切换 | • 头发表示自由切换，强制 Groom 模式与 GPU 包围盒实时调试<br>• 联机/替身缺失对应 Groom 时智能回退至发片模型，不干扰主角毛发表现 |
| **调试补充** | 169 项原厂调试指令重组<br>（需补充包） | • 整理为 9 大卡片分区（无敌、不死、穿墙飞行、移速、传送、DLSS、音频、物品、天赋、过场控制、性能采集等）<br>• 支持关键词检索与分类过滤；写入存档类高危项配置 3 秒防误触二次确认 |
| **其他特性** | 硬件与系统深度适配 | • 支持关闭 DualSense 手柄自适应扳机阻尼<br>• 全局配置统一保存于 `PBZDebugMenu.ini` |

---

## 默认快捷键

### 核心操作键位表

| 操作 | 键盘 | Xbox 手柄 | PlayStation 手柄 |
|---|---|---|---|
| **释放投技** | `G` | `Y` + `B` | `△` + `○` |
| **施放 NPC 技能** | `X` | `L3` + `R3` | `L3` + `R3` |
| **切换武器预设** | `Z` | `LB` + `L3` | 触控板向左 / 向右滑动 |
| **提升 / 降低挡位** | `=` / `-` | — | — |
| **呼出 / 隐藏面板** | `F10` | `View` + `Menu` | `Create` + `Options` |
| **返回 / 隐藏 · 切换分页** | `Esc` · `Ctrl` + `Tab` | `B` · `LB` / `RB` | `○` · `L1` / `R1` |
| **毛发页 · 蒙皮 / Groom** | `F7` · `F8` | — | — |
| **强制 Groom · 套装解锁** | `F6` · `Shift` + `F6` | — | — |
| **GPU 包围盒 · 自适应扳机** | `F9` · `Shift` + `F9` | — | — |

> [!NOTE]
> - **手柄双键判定**：手柄组合键须在 220 毫秒内按下，防止日常战斗误触。
> - **设备自适应**：手柄类型默认「自动」，根据最近输入即时切换按键图标（经 Steam Input 或 DS4Windows 映射的 DualSense 亦可识别为 PS 布局）。
> - **手柄操作面板**：
>   - 十字键 / 左摇杆：在控件与左侧导航列表间移动焦点；左右方向调节滑条与下拉框。
>   - `A` / `×`：确认、勾选、录制快捷键。
>   - `B` / `○`：收起下拉框、回退至导航栏、隐藏面板。
>   - `LB` / `RB`（`L1` / `R1`）：切换分页；`LT` / `RT`（`L2` / `R2`）：滚动页面。
>   - 面板打开时拦截游戏手柄信号，防止角色在背景移动；面板关闭时已按下的键释放后才会恢复传给游戏。

---

## 身材与骨骼系统

本 MOD 提供完整的骨骼变换与身材定制能力，分为**独立编辑器**与**招式派生节点**两种使用维度：

### 1. UUEPBS 身材编辑器
- 按 **F11** 开启独立的 [UUEPBS](https://github.com/XTGMods/UUEPBS) 编辑器窗口（默认关闭）。
- 提供部位滑杆、逐根骨骼参数调节、预设保存、骨骼动画与站/走/跑/冲刺分档姿态。
- 骨骼页配备自绘搜索列表（自动剔除表情骨骼）与实时全身骨架调试；新增简化体型滑杆、子骨骼模式与参考姿势镜像。
- 仅作用于主角，在变身为 Boss 期间自动暂停；预设与动画存放于游戏目录的 `UUEPBS Presets/`。

### 2. 招式派生「骨骼变换」节点
- 节点内允许对最多 32 根骨骼执行偏移、旋转与缩放，连接与时间轴规则与加速节点一致。
- 节点编辑窗口分为体型、骨骼明细与骨架调试三个分区，悬停预览实时生效。
- 与身材编辑器共用同一骨骼注入管线，保证状态连贯。

### 3. 女性预设与柔体骨骼
- 女性预设涵盖胯部扭转、低强度臀胯跟随、胸腔反向回旋及惯性运动模拟。
- 可选安装 [PBZSoftBones 补充包](docs/SOFT_BONES.md)，为主角及 75 个外观模型引入带真实权重的胸臀骨骼（含沐小葵双阶段形态）。开关默认关闭，关闭后即刻复原原生形态。
- 参数说明与调优指南请见 [骨骼变换与动画预设指南](docs/ANIMATION_PRESETS.md)。

---

## 本地替身实验（开发中）

进入任意关卡后，在「联机测试」页无需加入房间，点击「生成替身」即可开启本地镜像沙盒：

- **姿态与效果同步**：替身采用网络同步协议同款姿态编码，完整复制角色部件、显示状态与 Niagara / Cascade 粒子效果（包括火流星链条的曲线绑定与曲线点），并同步生成活动投射物轨迹。
- **本地对战判定**：点击「本地对战判定 / 重置」，替身自动转向面向主角。双方各分配 1000 点独立实验血量，武器碰撞发生接触时每次扣除最高 100 点，血量归零后终止判定。点击「结束对战判定」切回无害镜像。
- **发型回退机制**：替身头发默认应用 Groom，若缺失匹配资源则自动回退至发片网格，不干扰主角本人的头发设置。
- **技术定位**：此功能用于单机离线观测武器攻击轨迹与双向命中判定，不干扰游戏真实生命值与原生存档。

---

## 安装与更新

> Steam 版与 WeGame 版共用同一个发布包与 `version.dll`，自动识别版本。

### 方式 A：自动化安装（推荐）

1. 退出游戏，下载最新发布包 **[CringeBladeZero.zip](https://github.com/Jersyi/cringe-blade-zero/releases/latest/download/CringeBladeZero.zip)** 并完全解压。
2. 双击运行 `Install.bat`：
   - 安装器自动从注册表检索游戏安装路径并校验版本。
   - 输入 **1** 安装完整版 MOD（附带官方连招示例 `combos/sampleCombo.combo`）。
   - 输入 **2** 安装精简版（仅包含 DualSense 自适应扳机关闭功能）。
   - 输入 **3** 卸载 MOD 并还原备份。
   - 输入 **4** 运行环境检查与排错工具（`EnvCheck.exe`）。
3. 复制完成后自动执行 SHA-256 完整性核验；若原目录存在第三方 `version.dll` 会自动创建备份。

### 方式 B：手动安装

将压缩包内的 `version.dll` 复制到游戏主程序 `PBZ.exe` 同级目录下：
- **Steam 版**：`<游戏根目录>\platform\steam\version.dll`
- **WeGame 版**：`<游戏根目录>\PBZ.exe` 所在目录

### 进阶：调试补充包（可选）

将自备的补充包文件（`PBZDebugSupplement.pak`、`.utoc`、`.ucas`）置于：
`<游戏根目录>\ProjectPBZ\Content\Paks\~mods\`
启动游戏后面板将自动解锁「调试补充」功能页。

---

## 环境检查与排错

当遇到游戏无法启动、按 F10 无响应或配置无法保存时，请在 `Install.bat` 中选 **4** 或直接运行 `EnvCheck.exe`（支持拖拽 `PBZ.exe` 至其图标上排查）：

### 诊断范围

| 诊断层级 | 核心检测项 |
|---|---|
| **系统底层** | • Windows 系统版本及更新补丁<br>• VC++ 2015–2022 x64 运行库状态（游戏底层依赖，低于 14.40 提示升级）<br>• Direct3D 12 驱动支持与 XInput 手柄连接<br>• MOD 静态导入库与系统原生转发目标 `version.dll` 导出完整性 |
| **安全与拦截** | • 注册表 KnownDLLs 劫持防范<br>• Windows Exploit Protection（首选系统映像、代码完整性保护等）<br>• 映像劫持（IFEO）、智能应用控制（Smart App Control）、ASR 攻击面减少规则<br>• 提取最近 30 天 Defender / 安全软件的拦截审计日志 |
| **游戏安装目录** | • `PBZ.exe` 版本识别与 SHA-256 校验（支持 Steam / WeGame 官方版本）<br>• 已部署 `version.dll` 完整性与 Web 标记（Mark-of-the-Web）检测<br>• 错放路径检查（如放至根目录或 Binaries 下的无效 DLL）<br>• 目录写入权限、只读属性与 INI 文件健康度<br>• `combos\` 目录下全部派生文件的完整性 |
| **运行时审计** | • 游戏运行中实时挂钩进程，确认实际加载的 DLL 物理路径与加载日志 |

### 一键修复机制

发现异常时，程序将列出修复建议，支持一键处理（按 **A** 全部修复，按 **R** 重新检测）：
- 自动清除 `version.dll` 的网络下载锁定标记
- 停用并改名放错位置的残留 DLL
- 校验微软数字签名并静默升级官方 VC++ 运行库
- 移除损坏的 `.combo` 文件并归档至 `combos\broken\`
- 自动授予当前用户对游戏目录的完全修改权限
- 必要时调用系统组件核验（`sfc /scannow`）

---

## 设置文件（INI）

所有设置保存在与 `version.dll` 同级的 `PBZDebugMenu.ini` 中，仅在修改时写入磁盘。首次启动时自动生成并默认关闭所有功能。

| 配置节 | 关键字段与释义 | 默认值 |
|---|---|---|
| `[UI]` | `Language`：界面语言（`zh-CN` / `en-US`） | `zh-CN` |
| `[Hub]` | `PauseGame`：打开面板时是否暂停游戏（`1` 开启，`0` 关闭） | `0` |
| `[Hotkeys]` | 快捷键配置：支持 `<功能>`、`<功能>.Xbox`、`<功能>.PS`<br>`PadGlyphs`：手柄键位图标风格（`auto` / `xbox` / `ps`）<br>`ToggleHub`：面板唤出按键（默认 `kb:F10`） | 见快捷键表 |
| `[Gear]` | `Level`：当前生效的挡位（`0`–`10`） | `0` |
| `[Grab]` | `Enable`：开启投技；`TargetBoss`：允许抓取人形 Boss<br>`IgnoreInvulnerable`：无视敌方无敌判定；`RedFlash`：出招头部红光警示 | `0` |
| `[GrabExtras]` | `HidePlayerWeapon`：投技期间隐藏主角背部/手中武器<br>`TransformToBoss`：施展投技期间化身为对应 Boss 外形 | `0` |
| `[NpcSkill]` | `DamageMultiplier`：NPC 技能伤害放大倍率（`1`–`10`） | `3` |
| `[NpcRetarget]` | `BossLocomotion`：Boss 自带移动动画（行走/跑步时使用 Boss 原生姿态） | `0` |
| `[NpcCombo]` | `File`：当前激活的派生链文件名（位于 `combos\` 文件夹下）<br>`ViewTarget`：镜头跟随部位（`0` 骨盆，`1` 脖子，`2` 头部）<br>`ViewScale`：镜头取景基准（`0` 按身高比例，`1` 按体型比例） | `default`<br>`0`<br>`0` |
| `[NpcBody]` | `Enabled`：身材/骨骼编辑器开关；`Hook`：骨骼写入管线挂钩 | `0`<br>`1` |
| `[AirSkills]` | `EnableAirSkills`：空中战技与杀气技；`NearEnemyJC`：近敌踩怪弹跳 | `0` |
| `[WeaponPresets]` | `PresetA`、`PresetB`：两套装备配置；`ActivePreset`：当前装配索引 | — |

---

## 源码构建与测试

### 环境依赖
- **操作系统**：Windows 10 / 11 x64
- **编译器**：Visual Studio 2022（需勾选「C++ 桌面开发」与「MSVC v143 - VS 2022 C++ x64/x86 生成工具」，包含 MASM）
- **CMake**：≥ 3.24

### 编译指令

```powershell
# 1. 使用自动化脚本构建全部目标（Release）
.\build.ps1

# 2. 构建并自动打包分发 ZIP 包（包含版本号与校验）
.\build.ps1 -Package

# 3. 亦可通过原生 CMake 手动构建
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target version
```

### 构建产物

| 目标名称 | 输出路径 | 功能描述 |
|---|---|---|
| `version` | `build/Release/version.dll` | 完整版 MOD 主动态链接库 |
| `version_triggers` | `build/triggers_only/version.dll` | 精简版 DLL（仅关闭自适应扳机） |
| `EnvCheck` | `build/Release/EnvCheck.exe` | 独立运行的环境健康检查与一键修复工具 |
| `env_check_test` | `build/Release/env_check_test.exe` | PE 结构、导入导出表及环境诊断逻辑测试 |

### 自动化测试套件

本仓库包含覆盖各子系统的离线与模拟测试套件（编译选项严格遵循 `/W4 /WX /utf-8`，全静态 CRT 链接）：

| 测试体系 | 涵盖测试目标 | 验证范围与核心逻辑 |
|---|---|---|
| **核心与生命周期** | `proxy_test`<br>`game_thread_test` | DLL 导出转发正确性、游戏主线程挂钩时序、UObject 反射访问与命令管道安全性 |
| **手柄与输入屏蔽** | `pad_triggers_test`<br>`pad_gate_test`<br>`game_pad_gate_test`<br>`input_focus_test` | DualSense 报文拦截与阻尼关闭；虚假窗口线程下验证面板焦点捕获与手柄按键屏蔽；三套快捷键录制、迁移与输入设备无缝热切换 |
| **外观与姿态解算** | `npc_pose_transfer_test`<br>`hair_controls_test` | 合成骨架姿势重定向、双手武器绑定与缩放、坐骑代理；毛发 Groom 动态挂载与发片平滑回退逻辑 |
| **招式派生与存档** | `npc_combo_test`<br>`combo_file_test`<br>`combo_save_stress_test`<br>`combo_library_test` | 32 类派生节点跳转分支、时间轴停顿、霸体/无敌范围、蓄力停留；`.combo` 文本压缩校验、并发读写保护与坏档自动隔离 |
| **自绘渲染与交互** | `combo_menu_preview` | 离线无窗渲染招式派生界面，自动化鼠标/键盘脚本遍历节点拖拽、连线阻断、按键录制、曲线编辑与招式预览卡片交互 |

---

## 项目结构

```text
src/
├─ core/          DLL 转发入口（proxy.cpp、forwarders.asm）、工作线程、反射与命令总线
├─ ui/            自绘面板（墨色金线）、主题引擎、中英双语、输入焦点、快捷键、挡位胶囊、Donate 页
├─ character/     外观重定向（姿势传递、武器缩放、碰撞盒自适应、坐骑代理）、招式派生引擎、身材/骨骼
├─ plugin/        派生插件宿主、Lua 5.4.7 脚本虚拟机、全局模块注册表（mod_registry）与权限管线
├─ boss/          Boss 连战流程、Boss AI 参数热调、Boss 援护、木桩生成、招式预览、入场/存档防护
├─ grab/          Boss 原生投技：成对能力激活、状态租约、独立武器与姿态代理、伤害换算与红光警示
├─ air/           空中战技与杀气技、空中闪避、近敌 JC 弹跳重置普攻、缓落流体控制
├─ weapons/       双武器预设秒切、官方武器库图标与名称映射
├─ net/           本地替身实验：姿态帧编码、Niagara 粒子同步、投射物镜像与本地命中结算
├─ pad/           DualSense 自适应扳机拦截、面板激活时手柄信号全屏蔽网关
├─ hair/          毛发渲染模式切换、强制 Groom 状态机、GPU 边界盒调试
├─ supplement/    官方调试补充包挂载层：169 项调试蓝图卡片化重组与二次确认守卫
├─ uuepbs/        UUEPBS 骨骼与身材编辑器（第三方组件）
├─ third_party/   Dear ImGui、Lua 5.4.7、stb_image
├─ generated/     由游戏解包数据逆向生成的静态数据表（NPC、武器、骨骼映射、图标）
├─ env_check/     EnvCheck.exe 诊断与修复程序源码
└─ triggers_only/ 精简版 version.dll 独立入口

sdk/              插件开发 SDK（pbz_combo_api.h、C++ 示例插件源码、Lua 范例）
tests/            各模块单元测试与离线自绘界面渲染用例
installer/        一键安装包资源与脚本（Install.bat / install.ps1）
samples/          官方派生链示例（sampleCombo.combo）与 UUEPBS 演示身材预设
licenses/         第三方组件开源许可证全文
docs/             架构、编译、逆向更新、插件接口与图文用户手册
scripts/          EXE 版本指纹提取、空 pak 挂载生成器、PDF 手册构建工具
```

---

## 开发文档

- **[AGENTS.md](AGENTS.md)**：开发者与 AI 协作规范、核心硬规则与各任务阅读索引
- **[项目架构指南](docs/ARCHITECTURE.md)**：线程交互模型、UObject 反射原理、派生四层架构与插件机制
- **[编译测试与打包](docs/BUILD.md)**：构建依赖链、离线测试用例与安装包打包细节
- **[游戏更新适配指南](docs/GAME_UPDATES.md)**：新版 EXE 内存地址重定位、字节特征码比对与资源表更新流程
- **[独立 pak 制作指引](docs/PAK_WORKFLOW.md)**：UnrealPak 资源封包、挂载规则与解包密钥记录
- **[派生插件接口手册](docs/PLUGIN_API.md)**：C/C++ API 规范、事件总线、全局模块访问与 Lua 接口说明
- **[插件编写上手指南](docs/AGENT_PLUGIN_GUIDE.md)**：插件开发模板、热重载调试与常见问题排查
- **[骨骼与动画预设指南](docs/ANIMATION_PRESETS.md)**：参数解算、全身姿态调试与柔体骨骼调校
- **[武器派生与特殊输入](docs/WEAPON_DERIVATIONS.md)**：按住蓄力、格挡组合输入与武器动作拓展

---

## Donate · 支持作者

<img src="docs/donate.jpg" alt="爱发电 · Jersyi" width="280" align="right">

**豪之刃完全免费，不打赏也可使用全部功能**，不存在任何付费解锁或功能限制。

如果您认可本作并希望支持作者持续维护，欢迎在爱发电进行自愿赞助：

- **爱发电主页**：**<https://afdian.com/a/Jersyi1>**
- 或使用手机应用扫描右侧赞助二维码。
- 游戏内按 **F10** 呼出面板，最底部的 **Donate** 页面亦提供实时二维码与复制链接。

<br clear="right">

---

## 许可与说明

### 开源许可

本项目自主编写的源代码与文档遵循 **[MIT 许可证](LICENSE)** 发布，Copyright (c) 2026 Jersyi。

下列第三方资产与代码不属于 MIT 授权范围，分别保留其原始权利：
- `src/third_party/` 内的 Dear ImGui 与 Lua 5.4.7；`src/uuepbs/third_party/` 内的 stb_image：各组件许可证见 `licenses/` 及 [第三方说明](THIRD_PARTY_NOTICES.md)。
- UUEPBS 身材编辑器源码及示例预设：设计参考自 XTGMods，详情见 [第三方说明](THIRD_PARTY_NOTICES.md)。
- `src/generated/` 目录中的图标切片、字库与数据定义逆向抽取自游戏本体文件，著作权归原开发商所有。

### 免责声明

- 本工具仅供个人单机离线学习与技术交流使用，与游戏官方开发商无任何关联。
- 请勿将本工具用于商业牟利或破坏游戏网络公平性的环境。
