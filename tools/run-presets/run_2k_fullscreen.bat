@echo off
rem Fullscreen 2K launcher (owner request 2026-11-20).
rem Host desktop is 2560x1600; guest output is 1280x720, scaled x2 at present.
setlocal
cd /d "%~dp0..\..\build"
set MCLA_DISPLAY_MODE=fullscreen
set MCLA_WINDOW_W=2560
set MCLA_WINDOW_H=1440
set MCLA_RENDER_SCALE=2
set MCLA_LOG_LEVEL=4
echo Launching fullscreen 2560x1440 (MCLA_RENDER_SCALE=2)...
"%~dp0..\..\build\mcla.exe"
endlocal
