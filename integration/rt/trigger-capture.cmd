@echo off
setlocal
cd /d "%~dp0"
"%~dp0python\python.exe" -I "%~dp0scripts\trigger_rt_capture.py"
pause
