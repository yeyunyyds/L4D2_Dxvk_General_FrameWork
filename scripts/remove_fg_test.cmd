@echo off
setlocal
if "%~1"=="" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage_fg_test.ps1" -Action Restore
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage_fg_test.ps1" -Action Restore -Game "%~1"
)
if errorlevel 1 pause
