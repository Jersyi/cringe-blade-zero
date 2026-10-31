<div align="center">

<img src="docs/title.jpg" alt="豪之刃 · Cringe Blade Zero" width="100%">

# 豪之刃 · Cringe Blade Zero

**《影之刃零》（Phantom Blade Zero）单机游戏内工具箱**

一个 `version.dll` 代理 MOD：游戏内自绘面板、Boss 连战、Boss 投技、空中技能、武器预设与挡位系统。

![Platform](https://img.shields.io/badge/platform-Windows%20x64-1f2328?style=flat-square)
![Language](https://img.shields.io/badge/C%2B%2B-17-00599C?style=flat-square)
![Engine](https://img.shields.io/badge/Unreal%20Engine-5-0e1128?style=flat-square)
![Build](https://img.shields.io/badge/build-CMake%20%2B%20MSVC-d8c292?style=flat-square)

</div>

---

## 功能

| 模块 | 内容 |
|---|---|
| **面板（Hub）** | 无边框自绘界面，仿游戏 UI 的墨色与金线风格；中英双语；打开时可暂停游戏；键盘 / Xbox / PlayStation 按键图标 |
| **Boss 连战** | 传送到任意 Boss、直接触战、连战流程，附难度与周目设置；入场保护与存档保护 |
| **Boss AI** | 读取并调整当前 Boss 实例的原生 AI 参数 |
| **投技** | 53 招人形 Boss 原生投技，三角 + 圈触发；施放时换成该 Boss 的武器，并由不可见的 Boss 骨架代理重放动画，武器演出与 Boss 一致；可放大伤害、无视无敌；受击、倒地时也能触发 |
| **空中技能 / JC** | 空中施放战技与杀气技、空中闪避、跳跃打断、近敌 JC 弹跳与资源奖励、缓落 |
| **武器预设** | 两套装备一键或滑动触控板切换，显示游戏内武器图标与官方名称，重启后保留 |
| **挡位** | 右下角 0–10 档胶囊：挡位越高开启越多（JC、投技、奖励、伤害倍数、无视无敌），10 档滑条燃起游戏手绘火焰 |
| **角色 / NPC** | 主角兼容装束替换、以 NPC 外观操作主角（实验） |
| **毛发 / Groom** | 头发表示切换、强制 Groom、GPU 包围盒调试 |
| **其他** | DualSense 自适应扳机可关闭；全部设置写入 `PBZDebugMenu.ini` |

## 安装

1. 构建或取得 `version.dll`（见下文）。
2. 复制到游戏目录：

   ```text
   <PBZ 安装目录>\platform\steam\version.dll
   ```

3. 启动游戏。进入游戏时屏幕上方出现“豪之刃”横幅，按 **F10** 打开面板。

> MOD 只在匹配的 `PBZ.exe` 版本上启用（启动时校验 SHA-256），版本不符时只转发系统 `version.dll` 调用，不写入任何游戏数据。日志写在 DLL 旁的 `PBZDebugMenu.log`。

## 构建

需要 **Visual Studio 2022**（含 C++ 桌面开发与 MASM）和 **CMake ≥ 3.24**。

```powershell
.\build.ps1            # 构建全部目标（Release）
.\build.ps1 -Package   # 另外打包 dist\CringeBladeZero_<日期>_<提交>.zip
```

或手动：

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target version
```

发布包内含完整版与精简版 `version.dll`，以及一份安装说明 `README.txt`。

| 目标 | 产物 |
|---|---|
| `version` | `build/Release/version.dll`，完整 MOD |
| `version_triggers` | `build/triggers_only/version.dll`，只关闭自适应扳机的精简版 |
| `proxy_test` · `hair_controls_test` · `game_thread_test` · `pad_triggers_test` · `input_focus_test` | 测试（`input_focus_test` 用假 UnrealWindow 线程验证面板焦点、鼠标捕获与按键投递） |

编译选项：`/W4 /WX /utf-8`，静态 CRT。

## 默认快捷键

| 操作 | 按键 |
|---|---|
| 显示 / 隐藏面板 | `F10` |
| 隐藏面板 · 切换页面 | `Esc` · `Ctrl` + `Tab` |
| 毛发页 · 蒙皮 / Groom 切换 | `F7` · `F8` |
| 强制 Groom · 套装解锁 | `F6` · `Shift` + `F6` |
| GPU 包围盒 · 自适应扳机 | `F9` · `Shift` + `F9` |
| 释放投技 | PlayStation `△` + `○` |
| 切换武器预设 | 触控板左右滑动 |

所有快捷键都可在面板“快捷键”页重新绑定（键盘、Xbox、PlayStation 组合键）。

## 设置文件

`PBZDebugMenu.ini` 与 DLL 放在一起，修改时才写盘：

| 节 | 内容 |
|---|---|
| `[UI]` | 界面语言 |
| `[Hub]` | 打开面板时暂停游戏 |
| `[Hotkeys]` | 自定义快捷键 |
| `[Gear]` | 当前挡位 |
| `[Grab]` · `[GrabExtras]` · `[GrabDamage]` | 投技招式与开关、换武器 / 伤害模式 / 无视无敌、各招实测伤害 |
| `[AirSkills]` | 空中施放、近敌 JC、缓落与奖励 |
| `[WeaponPresets]` | 两套武器预设与当前套 |

## 项目结构

```text
src/
├─ core/          DLL 入口与转发（proxy.cpp、forwarders.asm、version.def）、游戏线程钩子、UObject 反射、设置文件
├─ ui/            面板、主题、动画、输入焦点与鼠标捕获、双语、快捷键、横幅、挡位胶囊、面板暂停
├─ boss/          Boss 连战、Boss AI、入场保护与存档保护
├─ grab/          投技：原生成对激活、状态租约、Boss 武器与姿态代理、伤害
├─ air/           空中技能、空中闪避、JC 与奖励
├─ weapons/       武器预设与游戏内武器图标
├─ character/     角色替换与 NPC 外观重定向
├─ hair/          毛发显示与 Groom
├─ pad/           DualSense 自适应扳机
├─ generated/     从游戏数据生成的表（投技目录、武器、骨骼、UI 资源、导出表）
└─ triggers_only/ 精简版 version.dll
tests/            proxy / 毛发 / 游戏线程 / 扳机 / 输入焦点测试
docs/             README 图片
```

每个 `src/` 子目录都是头文件搜索路径，源码里直接写 `#include "ue_runtime.h"`。

## 说明

- 仅用于单机游戏，与游戏开发商无关。
- `src/generated/` 内的图标、字体子集与数据表取自游戏文件，版权归原权利人所有。
