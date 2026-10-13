@echo off
if "%LDB_CAPTURE_TRIGGER%"=="" (
  echo LDB_CAPTURE_TRIGGER is not set. Open this script from the configured CMD.
  pause
  exit /b 1
)
echo Return to the game now. Capture starts in 5 seconds.
timeout /t 5 /nobreak >nul
type nul > "%LDB_CAPTURE_TRIGGER%"
if errorlevel 1 exit /b 1
echo Trigger created.
