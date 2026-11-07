@echo off
setlocal
if "%~1"=="" (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage_fg_test.ps1" -Action Install
) else (
  powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0manage_fg_test.ps1" -Action Install -Game "%~1"
)
if errorlevel 1 pause
