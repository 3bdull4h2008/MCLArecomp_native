@echo off
set "PATH=C:\Program Files\Python314;C:\Program Files\Python314\Scripts;C:\Program Files\LLVM\bin;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 2
cd /d "%~dp0"
ninja -v -C build mcla.exe 2>&1