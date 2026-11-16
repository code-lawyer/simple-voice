@echo off
setlocal
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\setup.ps1" -Action Install
if "%errorlevel%"=="3010" (
  echo Registration completed. Save your work and sign out of Windows, then sign in again.
  pause
  exit /b 3010
)
if errorlevel 1 (
  echo Installation did not finish. Please read the message above.
  pause
  exit /b 1
)
pause
