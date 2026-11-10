# 编译、测试与打包

这份指南面向需要从源码构建豪之刃的开发者。普通玩家下载发布 ZIP 即可；Steam 和 WeGame 使用同一个完整版 `version.dll`。

## 准备构建环境

玩家手册源文件为 `docs/USER_MANUAL.md`。可在安装 ReportLab 的 Python 环境中运行 `python scripts/build_manual.py`，默认使用 Windows 微软雅黑字体，输出 `output/pdf/CringeBladeZero_Manual_ZH.pdf` 和覆盖检查 JSON；生成器会核对全部 30 类节点及调试补充卡片。正式打包前渲染 PDF 检查分页和中文显示。`build.ps1 -Package` 会在该 PDF 存在时将其纳入 ZIP；Markdown 和明确公开的 `samples/sampleCombo.combo` 始终纳入，其他玩家文件不会打包。

安装器隔离测试运行 `powershell -NoProfile -ExecutionPolicy Bypass -File tests/installer_sample_test.ps1`；发布示例解包检查运行 `build/Release/sample_combo_test.exe samples/sampleCombo.combo`。前者使用独立临时目录及假 DLL，不操作游戏安装。

安装 Visual Studio 2022 或对应 Build Tools，选择“使用 C++ 的桌面开发”、MSVC x64 工具、Windows SDK 和 MASM；安装 CMake 3.24 或以上、Git。命令在仓库根目录的 PowerShell 中运行。

DLL 使用 C++17、静态 MSVC 运行库和 Windows 系统库。正常编译不需要 Unreal Editor、UE4SS、游戏资源包或 Python；已经提交的 `src/generated/` 数据头直接参与编译。更新资源表时才需要相应的本地分析数据和生成器，不要手工修改生成表。

先退出游戏，再构建或更换 DLL。不要在游戏运行时覆盖文件，也不要为了编译强制结束别人的游戏进程。

界面图片在 `docs/manual_images/`。如 UI 有改动，先编译并运行专用离线导出器，再重建 PDF：

```powershell
cmake --build build --config Release --target manual_ui_export
if ($LASTEXITCODE -ne 0) { throw 'UI export build failed' }
.\build\Release\manual_ui_export.exe docs/manual_images
if ($LASTEXITCODE -ne 0) { throw 'UI export failed' }
python scripts/build_manual.py
```

导出器只在自己的隐藏窗口内渲染实际控件，不启动游戏、不发游戏输入；示例配置写入独立临时目录。输出 34 张节点图片、6 张 UI 图片和 `nodes.json`，生成器验证 30 类节点的图片覆盖。PDF 完成后应检查全部页面缩略图、关键页面原图和嵌入中文字体。

## 编译完整版

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
cmake --build build --config Release --parallel 2 --target version
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
```

产物为 `build/Release/version.dll`。[CMakeLists.txt](../CMakeLists.txt) 是目标和依赖的唯一来源：`version` 是完整版，`version_triggers` 是仅关闭自适应扳机的精简版，`EnvCheck` 是独立环境检查器。

编译全部目标可直接执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1
if ($LASTEXITCODE -ne 0) { throw 'Full build failed' }
```

这会编译测试程序，但不会自动运行测试、启动游戏或安装 DLL。

如果需要降低编译对桌面的影响，使用以下低优先级方式。日志必须检查，`cmd start` 的返回值不能单独证明构建成功。

```powershell
New-Item -ItemType Directory -Force local\validation | Out-Null
$env:MSBUILDDISABLENODEREUSE = '1'
cmd /c 'start "" /low /b /wait powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1 > local\validation\build.log 2>&1'
Get-Content local\validation\build.log -Tail 40
```

## 运行离线回归

完整构建后运行以下测试；它们不启动游戏。存档测试使用自己的临时目录，不把玩家的 `combos/default.combo` 当成测试输入或输出。

```powershell
$tests = @(
    'game_build_test', 'env_check_test', 'npc_combo_test', 'combo_file_test',
    'combo_save_stress_test', 'combo_library_test', 'combo_playback_rules_test',
    'air_attack_reset_test', 'npc_skill_support_test', 'npc_skill_colliders_test',
    'npc_pose_transfer_test', 'npc_cutscene_policy_test', 'npc_source_budget_test',
    'supplement_layout_test'
)
foreach ($test in $tests) {
    & ".\build\Release\$test.exe"
    if ($LASTEXITCODE -ne 0) { throw "$test failed" }
}
```

`combo_library_test` 使用隐藏窗口，不注入鼠标键盘输入。`supplement_sequence_test` 也使用隐藏窗口，但会写固定位置的预览 PNG；单独运行前建立 `local/validation/supplement_sequence_20261109/`，检查图片后自行清理。不要把 `input_focus_test` 或 `combo_menu_preview` 加入无人值守测试：它们会操作焦点或输入。

2026-11-09 的功能发布通过完整 Release 和 15 组离线回归；隔离压力测试包含 310 次保存、113 次重读、单链 10000 节点。离线通过不代表所有技能、过场和视觉效果已实机通过。变更后的游戏内验证应另行记录版本、场景和结果。

## 打一个两端通用的发布包

先提交要发布的源码，再执行：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\build.ps1 -Package
if ($LASTEXITCODE -ne 0) { throw 'Package failed' }
```

[build.ps1](../build.ps1) 重新配置、编译并输出 `dist/CringeBladeZero_<日期>_<短提交号>.zip`。存在未提交的已跟踪文件修改时，包名增加 `-modified`。正式发布还应检查 `git status --short`，因为这个标记不检查未跟踪文件。

ZIP 使用同一套 Steam / WeGame DLL，包含以下内容：

| 文件 | 用途 |
|---|---|
| `version.dll` | Steam / WeGame 通用完整版 |
| `triggers_only/version.dll` | 另一种功能版本：仅关闭自适应扳机 |
| `EnvCheck.exe` | 环境检查与修复 |
| `Install.bat`、`install.ps1` | 安装、卸载和检查入口 |
| `README.txt`、`CHANGELOG.md` | 玩家说明和更新记录 |
| `samples/sampleCombo.combo` | 作者授权公开的示例；完整安装或安装器选项 5 安装 |
| `CringeBladeZero_Manual_ZH.pdf`、`docs/` | 图文手册、构建/适配/资源说明及界面图片 |
| `THIRD_PARTY_NOTICES.md`、`licenses/` | 署名与许可证状态说明 |

完整版和精简版二选一安装。安装器的两端 EXE 大小和 SHA256 从 `src/core/proxy.cpp` 自动填入；`EnvCheck` 的版本表也由 CMake 从同一文件生成，不要另抄一套版本常量。

发布前核对 ZIP 的 CRC、文件清单、内部 DLL 与构建产物的 SHA256，以及安装器中没有残留 `__EXE_BUILDS__`。标准包不含补充 pak、UE4SS、配置、日志、游戏 EXE 或玩家的 default；只包含明确公开的 sampleCombo。女性化主角包不再发行。

## 安装与回滚

退出游戏后，使用 ZIP 的 `Install.bat` 安装；手动安装则先备份现有代理 DLL，再复制完整版到实际 `PBZ.exe` 旁。

| 平台 | DLL 位置 |
|---|---|
| Steam | `<游戏根目录>/platform/steam/version.dll` |
| WeGame | 实际 `PBZ.exe` 所在目录的 `version.dll` |

核对源 DLL 与目标 DLL 的 SHA256。出现问题时退出游戏并还原备份；不要通过删除玩家的 `.combo` 文件来“重置测试”。`Install.bat` 的卸载流程会按自身备份规则恢复原代理文件。

游戏更新后先阅读[版本适配流程](GAME_UPDATES.md)。需要制作或接入额外资源时阅读[独立 pak 流程](PAK_WORKFLOW.md)。
