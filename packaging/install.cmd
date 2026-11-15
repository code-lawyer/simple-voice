@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" -Action Install
if errorlevel 1 (
  echo Installation did not finish. Please read the message above.
  pause
  exit /b 1
)
pause
