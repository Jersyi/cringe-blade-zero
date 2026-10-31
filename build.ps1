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
$name = 'CringeBladeZero_' + (Get-Date -Format 'yyyyMMdd') + $(if ($commit) { '_' + $commit } else { '' })
$readme = @"
豪之刃 · Cringe Blade Zero
《影之刃零》（Phantom Blade Zero）游戏内工具箱 MOD

安装
  把 version.dll 复制到游戏目录的 platform\steam 文件夹：
    <PBZ 安装目录>\platform\steam\version.dll
  启动游戏，进入游戏后按 F10 打开面板。

精简版
  triggers_only\version.dll 只关闭 DualSense 自适应扳机，没有面板和其他功能。
  两个文件同名，只装其中一个。

卸载
  删除 platform\steam 下的 version.dll，以及同目录的 PBZDebugMenu.ini、PBZDebugMenu.log。

说明
  MOD 只在匹配的 PBZ.exe 版本上启用（启动时校验 SHA-256），版本不符时只转发系统 version.dll。
  设置保存在 DLL 旁的 PBZDebugMenu.ini，日志写在 PBZDebugMenu.log。
  仅用于单机游戏，与游戏开发商无关。

构建 $name
"@
$files = [ordered]@{
    "$name/version.dll"               = Join-Path $buildRoot 'Release\version.dll'
    "$name/triggers_only/version.dll" = Join-Path $buildRoot 'triggers_only\version.dll'
}
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
    $writer = New-Object System.IO.StreamWriter($archive.CreateEntry("$name/README.txt").Open(), (New-Object System.Text.UTF8Encoding($true)))
    try { $writer.Write($readme.Replace("`r`n", "`n").Replace("`n", "`r`n")) } finally { $writer.Dispose() }
} finally { $archive.Dispose() }
Write-Output $zipPath
