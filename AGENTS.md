# AGENTS.md

给在这个仓库里工作的 AI 助手和新开发者。先读这一页，再按任务读下面列出的文档。

## 这是什么

《影之刃零》（`PBZ.exe`）的游戏内工具箱 MOD「豪之刃 · Cringe Blade Zero」：一个 `version.dll` 代理，在游戏线程里改外观、招式派生、投技、空中技能、身材等，并画一个 F10 面板。C++17（UUEPBS 编辑器部分为 C++20），Visual Studio 2022 x64，CMake。只用于单机游戏。

## 按任务读

| 要做的事 | 先读 |
|---|---|
| 理解代码结构、线程、某个模块在哪 | [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) |
| 编译、测试、打包、安装 | [docs/BUILD.md](docs/BUILD.md) |
| 改招式派生（节点、存档、运行端、页面） | ARCHITECTURE.md 第 6 节，然后 `src/character/npc_combo_model.h` 和 `tests/npc_combo_test.cpp` |
| 写派生插件或 Lua 脚本 | [docs/AGENT_PLUGIN_GUIDE.md](docs/AGENT_PLUGIN_GUIDE.md)，完整接口见 [docs/PLUGIN_API.md](docs/PLUGIN_API.md) |
| 改插件宿主或插件接口 | ARCHITECTURE.md 第 7 节、PLUGIN_API.md「兼容规则」、`sdk/include/pbz_combo_api.h` |
| 游戏更新后重新适配 | [docs/GAME_UPDATES.md](docs/GAME_UPDATES.md) |
| 资源包 / pak | [docs/PAK_WORKFLOW.md](docs/PAK_WORKFLOW.md) |
| 玩家看到的功能说明 | [docs/USER_MANUAL.md](docs/USER_MANUAL.md)、[README.md](README.md) |

## 硬规则

1. **线程**：UObject 读写和引擎调用只在游戏线程（各模块的 `OnGameThreadGuarded`）。面板线程只提交请求、读快照。头文件顶部写着每个函数属于哪个线程，改之前先看。
2. **存档兼容**：`.combo` 文本的新节点写成旧版本认识的类型加一个小写字母标记，旧版本读到时下标不变。加标记前 grep 已用过的字母，测试里要有旧版本读法。
3. **默认关闭**：没有 `PBZDebugMenu.ini` 时，任何新功能都必须是关的。
4. **警告即错误**：`/W4 /WX`，包括 C4456（内层变量和外层同名）。第三方代码（`src/third_party/`、`src/uuepbs/`）保持原样，用自己的警告级别。
5. **不要手改生成表**：`src/generated/*.generated.h` 由脚本从游戏文件生成。
6. **换行符和 BOM 保持原样**：仓库里 LF / CRLF 混用，部分文件带 BOM。以文件实际字节为准，Python 读写用 `newline=''`。
7. **双语**：新的界面中文字符串加进 `src/ui/ui_i18n_strings.h`（`{L"中文", L"English"}`）。
8. **用词**：`README.md`、`CHANGELOG.md`、`README.txt`、`更新内容.txt` 里不写「菜单」两个字，用「面板」「页」「列表」。
9. **白名单 `.gitignore`**：新加的非 `.cpp` / `.h` 文件要加一行 `!路径`，否则不会进提交。提交前跑 `git status --short --ignored` 看 `!!` 行。
10. **不进仓库**：游戏 EXE、游戏资源包、补充包 `PBZDebugSupplement.*`、UE4SS、玩家存档（`combos\*.combo`）、本地分析缓存。
11. **游戏和玩家的东西**：测试不要读写玩家的 `combos\default.combo`；不要关闭正在被玩的游戏；编译结束时系统可能把前台交给游戏，所以先关游戏再编译。

## 命令

```powershell
.\build.ps1            # 配置并编译全部目标（Release）
.\build.ps1 -Package   # 另外打包 dist\CringeBladeZero_<日期>_<提交>.zip
```

```bash
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --target version          # 只编主 DLL
cmake --build build --config Release --target npc_combo_test   # 编一个测试
./build/Release/npc_combo_test.exe                              # 看 failures=0 / 退出码 0
```

- 可以无人值守跑的测试、要参数的测试、会操作窗口和输入的测试，分别列在 ARCHITECTURE.md 第 11 节。**`input_focus_test`、`combo_menu_preview`、`manual_ui_export`、`combo_canvas_perf_test`、`ui_paint_buffer_test`、`supplement_sequence_test` 会动窗口、焦点或输入，不要在别人用电脑时跑。**
- 游戏里的行为离线测不到。游戏内测试可以写 `PBZDebugMenu.command`（游戏目录，每秒读一次），结果看 `PBZDebugMenu.log` 里的 `COMMAND <verb> ok=…` 和模块标签。

## 改完以后

- 开发记录写进 `CHANGELOG.md` 顶部当天的小节：改了什么、怎么验证的（离线 / 游戏里）、旧版本读到新存档会怎样。
- 玩家能看到的变化写进 `更新内容.txt`，用【新增】/【修复】。
- 功能有变化时同步 `docs/USER_MANUAL.md` 和 README 里对应的那一行。
- 新文件在 `.gitignore` 里放行；要进安装包的文件加进 `build.ps1` 的 `$files` 表。

## 许可

本项目自己的代码以 [MIT](LICENSE) 发布。第三方代码（Dear ImGui、Lua、stb_image、UUEPBS）保留各自的条款，`src/generated/` 里取自游戏的数据归原权利人所有，见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。
