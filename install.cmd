@echo off
rem Puts Save Profiles into the co-op game's mods folder (install.cmd), or takes it out (install.cmd -Uninstall).
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1" %*
if "%~1"=="" pause
