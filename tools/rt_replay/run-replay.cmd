@echo off
setlocal
cd /d "%~dp0"
"%~dp0python\python.exe" -I "%~dp0tools\rt_replay\replay.py" %*
set "REPLAY_EXIT=%errorlevel%"
echo.
echo The UPLOAD lines above list all result ZIP parts.
pause
exit /b %REPLAY_EXIT%
