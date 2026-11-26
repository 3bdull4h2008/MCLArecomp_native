@echo off
rem Build the primary tree (current working location; E: retired 2026-10-27 by
rem owner decision - the tree moved to its present path and CMake is configured
rem here). Location-independent: cds to the directory this script sits in.
rem rc 2=vcvars, 3=configure, 4=stale/missing exe or missing build.ninja.
rem Git Bash: caller must guard cmd.exe (SKILL.md:29, PROGRAM_GUIDE:51).
rem The MSYS vars below are also exported so children inherit the guard.
rem Live logs stay at repo root build_cfg.log/build_ninja.log (SKILL.md:47-48);
rem the previous run is preserved as *.prev.log, never moved/missing.
rem Exe freshness: ninja rc=0 is not proof the exe moved (rule 12). The script
rem snapshots build\mcla.exe before ninja and fails with rc=4 if ninja reports
rem Linking but the exe time/size are unchanged. A no-work rc=0 stays rc=0.
setlocal
set "MSYS_NO_PATHCONV=1"
set "MSYS2_ARG_CONV_EXCL=*"
set "PATH=C:\Program Files\Python314;C:\Program Files\Python314\Scripts;C:\Program Files\LLVM\bin;%PATH%"
call "C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul 2>&1
if errorlevel 1 exit /b 2
cd /d "%~dp0"
if /i "%1"=="build" goto build
if exist build_cfg.log copy /y build_cfg.log build_cfg.prev.log >nul 2>&1
cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_BUILD_TYPE=RelWithDebInfo > build_cfg.log 2>&1
if errorlevel 1 exit /b 3
:build
if not exist "build\build.ninja" goto missingcfg
if exist build_ninja.log copy /y build_ninja.log build_ninja.prev.log >nul 2>&1
set "BEFORE="
if exist "build\mcla.exe" for %%A in ("build\mcla.exe") do set "BEFORE=%%~tA|%%~zA"
ninja -C build mcla.exe > build_ninja.log 2>&1
set "RC=%errorlevel%"
if not "%RC%"=="0" exit /b %RC%
if not exist "build\mcla.exe" goto missingexe
set "AFTER="
if exist "build\mcla.exe" for %%A in ("build\mcla.exe") do set "AFTER=%%~tA|%%~zA"
if "%BEFORE%"=="" exit /b 0
if not "%BEFORE%"=="%AFTER%" exit /b 0
findstr /c:"Linking CXX executable mcla.exe" build_ninja.log >nul 2>&1
if errorlevel 1 exit /b 0
echo [build_on_e] Linking reported but build\mcla.exe unchanged (stale link) 1>&2
exit /b 4
:missingcfg
echo [build_on_e] build\build.ninja missing - run bare build_on_e.bat (configure+build) first 1>&2
exit /b 4
:missingexe
echo [build_on_e] build\mcla.exe missing after ninja rc=0 1>&2
exit /b 4
