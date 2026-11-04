# Builds every target (Release). -Package also writes the player ZIP to dist\.
param([switch]$Package)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$buildRoot = Join-Path $projectRoot 'build'
& cmake -S $projectRoot -B $buildRoot -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& cmake --build $buildRoot --config Release --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
if (-not $Package) { return }

$commit = (& git -C $projectRoot rev-parse --short HEAD 2>$null)
# Uncommitted changes to tracked files: the archive is not that commit's build.
if ($commit -and (& git -C $projectRoot status --porcelain --untracked-files=no 2>$null)) { $commit += '-modified' }
$name = 'CringeBladeZero_' + (Get-Date -Format 'yyyyMMdd') + $(if ($commit) { '_' + $commit } else { '' })
$readme = @"
豪之刃 · Cringe Blade Zero
《影之刃零》（Phantom Blade Zero）游戏内工具箱 MOD

安装
  一键安装：先退出游戏，双击 Install.bat，按 1 安装完整版、2 安装精简版、3 卸载。
    安装器从注册表找到游戏目录（找不到时可手动输入），校验游戏版本，复制后再校验 DLL；
    目录里已有其他 MOD 的 version.dll 时先备份，卸载时还原。界面语言随系统显示语言（中文 / 英文）。
  手动安装：把 version.dll 复制到游戏目录的 platform\steam 文件夹：
    <PBZ 安装目录>\platform\steam\version.dll
  启动游戏，进入游戏后按 F10 打开面板。

默认快捷键（键盘 / Xbox / PlayStation；按最近使用的设备自动识别）
  释放投技          G       Y + B      三角 + 圈
  施放 NPC 技能     X       L3 + R3    L3 + R3
  切换武器预设      Z       LB + L3    触控板左右滑
  提升 / 降低挡位   = / -
  都可在面板“快捷键”页或 PBZDebugMenu.ini 的 [Hotkeys] 修改。

精简版
  triggers_only\version.dll 只关闭 DualSense 自适应扳机，没有面板和其他功能。
  两个文件同名，只装其中一个。

卸载
  运行 Install.bat 选 3；或手动删除 platform\steam 下的 version.dll，以及同目录的 PBZDebugMenu.ini、PBZDebugMenu.log。

说明
  MOD 只在匹配的 PBZ.exe 版本上启用（启动时校验 SHA-256），版本不符时只转发系统 version.dll。
  设置保存在 DLL 旁的 PBZDebugMenu.ini，日志写在 PBZDebugMenu.log。
  更新内容见同目录的 CHANGELOG.md。
  仅用于单机游戏，与游戏开发商无关。

构建 $name
"@
$files = [ordered]@{
    "$name/version.dll"               = Join-Path $buildRoot 'Release\version.dll'
    "$name/triggers_only/version.dll" = Join-Path $buildRoot 'triggers_only\version.dll'
    "$name/CHANGELOG.md"              = Join-Path $projectRoot 'CHANGELOG.md'
    "$name/Install.bat"               = Join-Path $projectRoot 'installer\Install.bat'
}
# The installer checks the game against the build proxy.cpp accepts.
$proxy = Get-Content -LiteralPath (Join-Path $projectRoot 'src\core\proxy.cpp') -Raw
$exeBytes = [regex]::Match($proxy, 'kExeBytes = (\d+);').Groups[1].Value
$exeSha256 = [regex]::Match($proxy, 'kSha256 = "([0-9a-f]{64})"').Groups[1].Value
if (-not $exeBytes -or -not $exeSha256) { throw 'kExeBytes / kSha256 not found in proxy.cpp' }
$installer = [IO.File]::ReadAllText((Join-Path $projectRoot 'installer\install.ps1')).Replace('__EXE_BYTES__', $exeBytes).Replace('__EXE_SHA256__', $exeSha256)
$distRoot = Join-Path $projectRoot 'dist'
New-Item -ItemType Directory -Path $distRoot -Force | Out-Null
$zipPath = Join-Path $distRoot "$name.zip"
Remove-Item -LiteralPath $zipPath -ErrorAction SilentlyContinue
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
# Entries are named explicitly so the archive always uses '/' separators.
$archive = [System.IO.Compression.ZipFile]::Open($zipPath, [System.IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($entry in $files.GetEnumerator()) {
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive, $entry.Value, $entry.Key, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
    # Text entries: UTF-8 with BOM (Windows PowerShell 5.1 reads a script without one as ANSI) and CRLF.
    foreach ($text in @(@("$name/README.txt", $readme), @("$name/install.ps1", $installer))) {
        $writer = New-Object System.IO.StreamWriter($archive.CreateEntry($text[0]).Open(), (New-Object System.Text.UTF8Encoding($true)))
        try { $writer.Write($text[1].Replace("`r`n", "`n").Replace("`n", "`r`n")) } finally { $writer.Dispose() }
    }
} finally { $archive.Dispose() }
Write-Output $zipPath
