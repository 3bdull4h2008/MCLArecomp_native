@echo off
rem Native 16:10 fullscreen launcher (2560x1600 monitor).
rem Guest aspect constant 0x8201E7EC is rewritten to 1152/720 = 1.6 so the
rem game's UI/HUD uses a 16:10 frame; host aspect correction uses mcla_aspect_ratio=16:10.
setlocal
cd /d "%~dp0..\..\build"
set MCLA_DISPLAY_MODE=borderless_fullscreen
set MCLA_WINDOW_W=2560
set MCLA_WINDOW_H=1600
set MCLA_RENDER_SCALE=2
set MCLA_ASPECT_FIX=1
set MCLA_ASPECT_WIDTH=1152
set MCLA_LOG_LEVEL=4
echo Launching 16:10 fullscreen 2560x1600 (guest width 1152)...
"%~dp0..\..\build\mcla.exe"
endlocal
