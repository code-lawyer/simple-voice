@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" -Action Check
if errorlevel 1 (
  echo Check failed. Please read the message above.
  pause
  exit /b 1
)
pause
