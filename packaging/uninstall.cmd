@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" -Action Uninstall
if errorlevel 1 (
  echo Uninstall did not finish. Please read the message above.
  pause
  exit /b 1
)
pause
