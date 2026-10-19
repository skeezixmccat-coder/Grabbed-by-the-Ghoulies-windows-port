@echo off
cd /d "%~dp0"
if not exist "Ghoulies.exe" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\build_launcher.ps1"
  if errorlevel 1 exit /b 1
)
start "" "%~dp0Ghoulies.exe"
