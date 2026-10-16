@echo off
setlocal
cd /d "%~dp0"
"%~dp0python\python.exe" -I "%~dp0scripts\rt_draw_suite.py" %*
pause
