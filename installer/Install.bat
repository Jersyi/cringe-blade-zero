@echo off
rem Cringe Blade Zero installer: runs install.ps1, in Windows Terminal when available (true-colour logo).
chcp 65001 >nul
if defined WT_SESSION goto run
where wt.exe >nul 2>nul || goto run
start "" wt.exe -w new powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
exit /b
:run
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
