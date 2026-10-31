$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
& cmake -S $projectRoot -B (Join-Path $projectRoot 'build') -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'CMake configure failed' }
& cmake --build (Join-Path $projectRoot 'build') --config Release --target version proxy_test hair_controls_test game_thread_test --parallel 2
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
