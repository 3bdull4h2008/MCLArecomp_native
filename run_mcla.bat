@echo off
rem ============================================================================
rem run_mcla.bat - FOREGROUND single-instance launcher for long soaks (20+ min).
rem location-independent: cds to the build\ dir THIS script sits next to
rem (AGENTS.md rule 6 / build_on_e.bat:9 pattern). Sits at the repo root, so
rem the exe/log/cache paths below are all relative to build\ by construction.
rem
rem Safety contract (each line exists because of a measured failure mode):
rem  [window focus]  ONE mcla.exe only: a second process collides on the
rem                   single window / D3D12 device / VFS cache (F-572; AGENTS.md
rem                   build/soak_parallel.py row: "one window, one D3D12
rem                   device, one audio endpoint, one VFS cache"). Refused here
rem                   via tasklist BEFORE launch (rule 11). Launched FOREGROUND
rem                   and synchronously: never `start /b`, `/min`, or detached,
rem                   which would background the D3D12/SDL window and change
rem                   present timing. Keep this console + game window visible
rem                   and focused for the whole soak; closing the console
rem                   kills the soak with no survival signal.
rem  [env vars]      setlocal: caller env is never mutated past exit. Console
rem                   quiet (MCLA_QUIET=1, src/common/logging.h:28-30) but the
rem                   logger LEVEL is recorded: unset MCLA_LOG_LEVEL defaults
rem                   to Error (src/app.cpp:52-55), which would starve a
rem                   census-style gate comparison (rule-19 blind zeros). So
rem                   default MCLA_LOG_LEVEL=2 (Info, full census) ONLY when
rem                   unset; a caller-set value is honoured and echoed.
rem                   Debug/dump knobs (MCLA_FORCE_*, MCLA_*DUMP*, MCLA_NO_*,
rem                   MCLA_HALO_*, MCLA_DIRECT_BACKBUFFER, MCLA_UNSKIP,
rem                   MCLA_VB_DUMP, MCLA_SKIP_PRIM8, MCLA_BOOT_CENSUS,
rem                   MCLA_PARK_SAMPLE, MCLA_SCAN_VTABLE, MCLA_BMP_DUMP,
rem                   MCLA_PROTECT_DRAW_BUFFER and cp_* traces) are cleared
rem                   for a reproducible baseline, logged in the header, and
rem                   kept when MCLA_KEEP_ENV=1 is set. Path knobs that may be
rem                   REQUIRED (MCLA_DXC_DIR src/renderer/dxc_runtime.cpp:266,
rem                   MCLA_ASSET_CACHE src/asset_cache.h:47, MCLA_MAINWAIT_BOUND_MS
rem                   src/kernel/imports.cpp:2815, MCLA_KEYSTROKE_QUEUE
rem                   src/kernel/xam.cpp:895) are NEVER cleared, only logged.
rem  [log naming]    build\cache\mcla.log is a ROTATING sink (10 MB x5,
rem                   src/common/logging.h:38-39), NOT a soak log: it cannot
rem                   serve as a census baseline. This launcher mints a new
rem                   immutable pair build\wYYYYMMDD-HHMMSS.log + _err.log per
rem                   run (rule 5: one new log per experiment; boot-cycle
rem                   skill: "Never reuse or overwrite an existing soak log")
rem                   and archives cache\mcla.log* to w*-mcla.log BEFORE launch
rem                   (launch truncates/rotates the sink) and AFTER exit (the
rem                   gate evidence). Header records exe identity (size/mtime),
rem                   effective env, and cache\mcla.toml content so a later
rem                   gate comparison can prove the binary/config were the
rem                   ones claimed (rules 12/13). cmd.exe `^>` here is ANSI,
rem                   never PowerShell UTF-16LE (MEMORY.md trap: never write
rem                   build/*.log with PowerShell `^>`; boot-cycle skill:85-92).
rem
rem No timeout is imposed by design: an unbounded foreground wait records a
rem caller-determined duration in the footer; use a Python subprocess watchdog
rem (boot-cycle skill:88-89) for a BOUNDED soak - never `cmd /c timeout ...`.
rem
rem CLI OPTIONS (readers change these WITHOUT editing code; see --help).
rem Rule: this block exposes ONLY levers proven in src/. Everything else is
rem either (a) accepted+validated but logged as NO-OP with the src/ reason, or
rem (b) left to the caller env via --keep-env (NOT one flag per debug knob:
rem ~30 MCLA_FORCE_*/DUMP/PROBE env vars exist; a flag each would bloat this
rem file while adding zero coverage past `MCLA_KEEP_ENV=1` passthrough).
rem
rem  SUPPORTED (maps to a real src/ lever, applied before launch):
rem   --logLevel 0..6   MCLA_LOG_LEVEL (src/app.cpp:52-55); --quiet 0|1
rem                      MCLA_QUIET (src/common/logging.h:28-30).
rem   --usegpu native|legacy|capture (alias --renderer)
rem                      renderer_mode CVar (src/renderer_mode.cpp:8) via
rem                      cache\mcla.toml LoadConfig (src/app.cpp:64).
rem   --resolution 720p|1080p|1440p|4k|800p|WxH -- startup size is HARDCODED
rem                      1280x720 (src/app.h:84-85); the only bat-possible lever
rem                      is post-launch resize down the supported path
rem                      SDL_EVENT_WINDOW_RESIZED -> App::OnResize
rem                      (src/app.cpp:307-310,343-358) -> D3D12Backend::Resize
rem                      (src/d3d12_backend.cpp:7572) by a helper that waits for
rem                      the window and moves it. Default 1280x720 = no helper.
rem   --icon normal|minimized|hidden, --topmost 0|1 -- HOST window control by
rem                      the same helper (ShowWindow/SetWindowPos); window flags
rem                      are fixed in CreateSDLWindow (src/app.cpp:232), so these
rem                      are host niceties, NOT exe flags. Defaults normal/0.
rem   --probes|--no-probes -- MCLA_PROBE_READBACK_BB
rem                      (src/d3d12_backend.cpp:6330)+MCLA_PROBE_FLUSH_RED
rem                      (src/d3d12_backend.cpp:8273); default no-probes. Covers
rem                      ONLY these env-gated probes, not in-code X901 probes.
rem   --psocache|--no-psocache -- MCLA_NO_PSOCACHE
rem                      (src/renderer/pipeline_cache.cpp:3235). Default on.
rem   --dxc-dir PATH     MCLA_DXC_DIR (src/renderer/dxc_runtime.cpp:360).
rem   --asset-cache PATH MCLA_ASSET_CACHE (src/asset_cache.h:47).
rem   --mainwait-ms N    MCLA_MAINWAIT_BOUND_MS (src/kernel/imports.cpp:2815).
rem   --keep-env 0|1     MCLA_KEEP_ENV for the clearing block (lines below).
rem  VALIDATED NO-OP (accepted, range-checked, logged, IGNORED by the exe):
rem   --windowed|--fullscreen -- fullscreen CVar is UNDECLARED so the read
rem                      always returns false (src/app.cpp:234-236); main()
rem                      ignores argv (src/main.cpp:7-9). Always windowed.
rem   --gamma 0.4..4.0   VdGetCurrentDisplayGamma is a STUB
rem                      (src/kernel/imports.cpp:7337) and the gamma LUT is
rem                      UNMODELED (src/gpu_cp.cpp:894-895,1096). Logged only.
rem   --asynccompile on|off -- no toggle exists; the async worker always starts
rem                      (src/render_thread.cpp:150; queue
rem                      src/renderer/pipeline_cache.h:824). Use --no-psocache
rem                      for the cache control that does exist.
rem  UTILITY: --help (usage, exit 0, no exe needed); --dry-run|--print-config
rem   (mint the same w*.log header + OPTIONS line, exit 0 BEFORE launch;
rem   skips exe/single-instance checks so it proves log generation).
rem Use space form: --logLevel 2 (NOT --logLevel=2; `=` needs quoting tricks
rem this file deliberately avoids -- see the eq_hint below). Single-dash
rem aliases (-logLevel) are accepted for the task's requested names.
rem ============================================================================
setlocal EnableExtensions EnableDelayedExpansion

rem --- launcher option defaults (CLI wins over caller env) -------------------
rem Empty = untouched (fall through to the deterministic defaults below).
set "OPT_LOGLEVEL="
set "OPT_QUIET="
set "OPT_RENDERER="
set "OPT_RESOLUTION="
set "OPT_WINDOW="
set "OPT_GAMMA="
set "OPT_ICON=normal"
set "OPT_TOPMOST=0"
set "OPT_PROBES=off"
set "OPT_PSOCACHE=on"
set "OPT_ASYNC=on"
set "OPT_DXC_DIR="
set "OPT_ASSET_CACHE="
set "OPT_MAINWAIT="
set "OPT_KEEPENV="
set "OPT_DRYRUN=0"
set "RES_W=1280"
set "RES_H=720"

rem --- parse args (space form; flags take no value) ---------------------------
:parse_args
if "%~1"=="" goto parse_done
echo %~1| findstr /c:"=" >nul
if not errorlevel 1 goto eq_hint
if /i "%~1"=="--help" goto do_help
if /i "%~1"=="-h" goto do_help
if /i "%~1"=="-help" goto do_help
if /i "%~1"=="--dry-run" set "OPT_DRYRUN=1"& shift& goto parse_args
if /i "%~1"=="--print-config" set "OPT_DRYRUN=1"& shift& goto parse_args
if /i "%~1"=="--logLevel" goto parse_logLevel
if /i "%~1"=="-logLevel" goto parse_logLevel
if /i "%~1"=="--quiet" goto parse_quiet
if /i "%~1"=="-quiet" goto parse_quiet
if /i "%~1"=="--usegpu" goto parse_renderer
if /i "%~1"=="-usegpu" goto parse_renderer
if /i "%~1"=="--renderer" goto parse_renderer
if /i "%~1"=="-renderer" goto parse_renderer
if /i "%~1"=="--resolution" goto parse_resolution
if /i "%~1"=="-resolution" goto parse_resolution
if /i "%~1"=="--windowed" set "OPT_WINDOW=windowed"& shift& goto parse_args
if /i "%~1"=="-windowed" set "OPT_WINDOW=windowed"& shift& goto parse_args
if /i "%~1"=="--fullscreen" set "OPT_WINDOW=fullscreen"& shift& goto parse_args
if /i "%~1"=="-fullscreen" set "OPT_WINDOW=fullscreen"& shift& goto parse_args
if /i "%~1"=="--gamma" goto parse_gamma
if /i "%~1"=="-gamma" goto parse_gamma
if /i "%~1"=="--icon" goto parse_icon
if /i "%~1"=="-icon" goto parse_icon
if /i "%~1"=="--topmost" goto parse_topmost
if /i "%~1"=="-topmost" goto parse_topmost
if /i "%~1"=="--probes" set "OPT_PROBES=on"& shift& goto parse_args
if /i "%~1"=="-probes" set "OPT_PROBES=on"& shift& goto parse_args
if /i "%~1"=="--no-probes" set "OPT_PROBES=off"& shift& goto parse_args
if /i "%~1"=="-no-probes" set "OPT_PROBES=off"& shift& goto parse_args
if /i "%~1"=="--psocache" set "OPT_PSOCACHE=on"& shift& goto parse_args
if /i "%~1"=="-psocache" set "OPT_PSOCACHE=on"& shift& goto parse_args
if /i "%~1"=="--no-psocache" set "OPT_PSOCACHE=off"& shift& goto parse_args
if /i "%~1"=="-no-psocache" set "OPT_PSOCACHE=off"& shift& goto parse_args
if /i "%~1"=="--asynccompile" goto parse_async
if /i "%~1"=="-asynccompile" goto parse_async
if /i "%~1"=="--dxc-dir" goto parse_dxc_dir
if /i "%~1"=="-dxc-dir" goto parse_dxc_dir
if /i "%~1"=="--asset-cache" goto parse_asset_cache
if /i "%~1"=="-asset-cache" goto parse_asset_cache
if /i "%~1"=="--mainwait-ms" goto parse_mainwait
if /i "%~1"=="-mainwait-ms" goto parse_mainwait
if /i "%~1"=="--mainwait" goto parse_mainwait
if /i "%~1"=="-mainwait" goto parse_mainwait
if /i "%~1"=="--keep-env" goto parse_keepenv
if /i "%~1"=="-keep-env" goto parse_keepenv
echo [run_mcla] ERROR: unknown option "%~1" 1>&2
echo [run_mcla] Run "run_mcla.bat --help" for supported options. 1>&2
exit /b 2
:parse_logLevel
if "%~2"=="" goto need_value
set "OPT_LOGLEVEL=%~2"
shift&shift&goto parse_args
:parse_quiet
if "%~2"=="" goto need_value
set "OPT_QUIET=%~2"
shift&shift&goto parse_args
:parse_renderer
if "%~2"=="" goto need_value
set "OPT_RENDERER=%~2"
shift&shift&goto parse_args
:parse_resolution
if "%~2"=="" goto need_value
set "OPT_RESOLUTION=%~2"
shift&shift&goto parse_args
:parse_gamma
if "%~2"=="" goto need_value
set "OPT_GAMMA=%~2"
shift&shift&goto parse_args
:parse_icon
if "%~2"=="" goto need_value
set "OPT_ICON=%~2"
shift&shift&goto parse_args
:parse_topmost
if "%~2"=="" goto need_value
set "OPT_TOPMOST=%~2"
shift&shift&goto parse_args
:parse_async
if "%~2"=="" goto need_value
set "OPT_ASYNC=%~2"
shift&shift&goto parse_args
:parse_dxc_dir
if "%~2"=="" goto need_value
set "OPT_DXC_DIR=%~2"
shift&shift&goto parse_args
:parse_asset_cache
if "%~2"=="" goto need_value
set "OPT_ASSET_CACHE=%~2"
shift&shift&goto parse_args
:parse_mainwait
if "%~2"=="" goto need_value
set "OPT_MAINWAIT=%~2"
shift&shift&goto parse_args
:parse_keepenv
if "%~2"=="" goto need_value
set "OPT_KEEPENV=%~2"
shift&shift&goto parse_args
:need_value
echo [run_mcla] ERROR: option needs a value - use space form, e.g. "--logLevel 2" 1>&2
echo [run_mcla] Run "run_mcla.bat --help" for supported options. 1>&2
exit /b 2
:eq_hint
echo [run_mcla] ERROR: "%~1" uses "=" - this launcher takes space form, e.g. "--logLevel 2" 1>&2
echo [run_mcla] Run "run_mcla.bat --help" for supported options. 1>&2
exit /b 2
:parse_done

rem --- validate every option against the current code level ------------------
if defined OPT_LOGLEVEL (
  echo !OPT_LOGLEVEL!| findstr /r "^[0-6]$" >nul
  if errorlevel 1 goto bad_loglevel
)
if defined OPT_QUIET (
  echo !OPT_QUIET!| findstr /r "^[01]$" >nul
  if errorlevel 1 goto bad_quiet
)
if not defined OPT_RENDERER goto renderer_done
if /i "!OPT_RENDERER!"=="native" goto renderer_done
if /i "!OPT_RENDERER!"=="legacy" goto renderer_done
if /i "!OPT_RENDERER!"=="capture" goto renderer_done
goto bad_renderer
:renderer_done
if not defined OPT_RESOLUTION goto res_skip
if /i "!OPT_RESOLUTION!"=="720p" goto res_720p
if /i "!OPT_RESOLUTION!"=="1080p" goto res_1080p
if /i "!OPT_RESOLUTION!"=="1440p" goto res_1440p
if /i "!OPT_RESOLUTION!"=="4k" goto res_4k
if /i "!OPT_RESOLUTION!"=="800p" goto res_800p
echo !OPT_RESOLUTION!| findstr /r "^[0-9][0-9]*[xX][0-9][0-9]*$" >nul
if errorlevel 1 goto bad_resolution
for /f "tokens=1,2 delims=xX" %%W in ("!OPT_RESOLUTION!") do (set "RES_W=%%W"& set "RES_H=%%X")
if "!RES_W!"=="" goto bad_resolution
if "!RES_H!"=="" goto bad_resolution
if !RES_W! LSS 640 goto bad_resolution
if !RES_W! GTR 7680 goto bad_resolution
if !RES_H! LSS 480 goto bad_resolution
if !RES_H! GTR 4320 goto bad_resolution
goto res_skip
:res_720p
set "RES_W=1280"& set "RES_H=720"& goto res_skip
:res_1080p
set "RES_W=1920"& set "RES_H=1080"& goto res_skip
:res_1440p
set "RES_W=2560"& set "RES_H=1440"& goto res_skip
:res_4k
set "RES_W=3840"& set "RES_H=2160"& goto res_skip
:res_800p
set "RES_W=1280"& set "RES_H=800"& goto res_skip
:res_skip
if not defined OPT_WINDOW goto window_done
if /i "!OPT_WINDOW!"=="windowed" goto window_done
if /i "!OPT_WINDOW!"=="fullscreen" goto window_done
goto bad_window
:window_done
rem NO-OP by design (see header): fullscreen is never read by the exe.
if not defined OPT_GAMMA goto gamma_done
echo !OPT_GAMMA!| findstr /r "^[0-9][0-9]*\.[0-9][0-9]*$" >nul
if not errorlevel 1 goto gamma_range
echo !OPT_GAMMA!| findstr /r "^[0-9][0-9]*$" >nul
if errorlevel 1 goto bad_gamma
:gamma_range
powershell -NoProfile -Command "$v=[double]'!OPT_GAMMA!'; if ($v -lt 0.4 -or $v -gt 4.0) {exit 1}" >nul 2>&1
if errorlevel 1 goto bad_gamma
:gamma_done
rem NO-OP by design (see header): gamma is never read by the exe.
if not defined OPT_ICON goto icon_done
if /i "!OPT_ICON!"=="normal" goto icon_done
if /i "!OPT_ICON!"=="minimized" goto icon_done
if /i "!OPT_ICON!"=="hidden" goto icon_done
goto bad_icon
:icon_done
echo !OPT_TOPMOST!| findstr /r "^[01]$" >nul
if errorlevel 1 goto bad_topmost
if /i "!OPT_PROBES!"=="on" goto probes_ok
if /i "!OPT_PROBES!"=="off" goto probes_ok
goto bad_probes
:probes_ok
if /i "!OPT_PSOCACHE!"=="on" goto pso_ok
if /i "!OPT_PSOCACHE!"=="off" goto pso_ok
goto bad_pso
:pso_ok
if /i "!OPT_ASYNC!"=="on" goto async_ok
if /i "!OPT_ASYNC!"=="off" goto async_ok
goto bad_async
:async_ok
rem NO-OP by design (see header): no async toggle exists in src/.
if defined OPT_MAINWAIT (
  echo !OPT_MAINWAIT!| findstr /r "^[0-9][0-9]*$" >nul
  if errorlevel 1 goto bad_mainwait
)
if defined OPT_KEEPENV (
  echo !OPT_KEEPENV!| findstr /r "^[01]$" >nul
  if errorlevel 1 goto bad_keepenv
)
goto validate_done
:bad_loglevel
echo [run_mcla] ERROR: --logLevel must be a single digit 0..6 (trace..off, src/app.cpp:52-55) 1>&2
exit /b 2
:bad_quiet
echo [run_mcla] ERROR: --quiet must be 0 or 1 (src/common/logging.h:28-30) 1>&2
exit /b 2
:bad_renderer
echo [run_mcla] ERROR: --usegpu must be native, legacy or capture (src/renderer_mode.cpp:8,22-30) 1>&2
exit /b 2
:bad_resolution
echo [run_mcla] ERROR: --resolution takes 720p,1080p,1440p,4k,800p or WxH (W 640..7680, H 480..4320) 1>&2
exit /b 2
:bad_window
echo [run_mcla] ERROR: --windowed/--fullscreen takes no value 1>&2
exit /b 2
:bad_gamma
echo [run_mcla] ERROR: --gamma must be a number 0.4..4.0 1>&2
exit /b 2
:bad_icon
echo [run_mcla] ERROR: --icon must be normal, minimized or hidden 1>&2
exit /b 2
:bad_topmost
echo [run_mcla] ERROR: --topmost must be 0 or 1 1>&2
exit /b 2
:bad_probes
echo [run_mcla] ERROR: --probes/--no-probes takes no value 1>&2
exit /b 2
:bad_pso
echo [run_mcla] ERROR: --psocache/--no-psocache takes no value 1>&2
exit /b 2
:bad_async
echo [run_mcla] ERROR: --asynccompile must be on or off 1>&2
exit /b 2
:bad_mainwait
echo [run_mcla] ERROR: --mainwait-ms must be a non-negative integer (src/kernel/imports.cpp:2815) 1>&2
exit /b 2
:bad_keepenv
echo [run_mcla] ERROR: --keep-env must be 0 or 1 1>&2
exit /b 2
:validate_done
rem Defaults for the NO-OPs (logged in every header so a later gate can tell).
if not defined OPT_WINDOW set "OPT_WINDOW=windowed"
if not defined OPT_GAMMA set "OPT_GAMMA=1.0"

rem --- exe / dirs, location-independent -------------------------------------
cd /d "%~dp0build"
if errorlevel 1 (
  echo [run_mcla] ERROR: cannot cd to "%~dp0build" 1>&2
  exit /b 2
)
if "!OPT_DRYRUN!"=="1" goto skip_exe_checks
if not exist "mcla.exe" (
  echo [run_mcla] ERROR: mcla.exe not found in "%CD%" - build it first ^(build_on_e.bat build^) 1>&2
  exit /b 2
)
if not exist "cache" mkdir "cache"

rem --- single-instance guard (rule 11 / F-572) -------------------------------
tasklist /fi "IMAGENAME eq mcla.exe" 2>nul | findstr /i /c:"mcla.exe" >nul
if not errorlevel 1 (
  echo [run_mcla] REFUSED: mcla.exe already running - second window/D3D12/VFS collision. Kill it first ^(taskkill /F /IM mcla.exe^). 1>&2
  exit /b 3
)
goto guard_done
:skip_exe_checks
if not exist "cache" mkdir "cache"
:guard_done

rem --- deterministic env ------------------------------------------------------
set "MCLA_QUIET=1"
if not defined MCLA_LOG_LEVEL set "MCLA_LOG_LEVEL=4"
rem Debug/dump/experiment knobs: cleared for a reproducible baseline.
rem (All are getenv'd in src/; see grep evidence in the header comment.)
if not "%MCLA_KEEP_ENV%"=="1" (
  set "MCLA_FORCE_RED="& set "MCLA_RED_STAR="& set "MCLA_FORCE_TEXONLY="
  set "MCLA_FORCE_TEXCENTER="& set "MCLA_FORCE_SATURATE="
  set "MCLA_FORCE_UVVIS="& set "MCLA_FORCE_OPAQUE="& set "MCLA_FORCE_VSUV="
  set "MCLA_FORCE_VSPOS="& set "MCLA_FORCE_BLEND="& set "MCLA_BLEND_TEXT="
  set "MCLA_NO_PSOCACHE="& set "MCLA_HALO_FIX="& set "MCLA_ATLAS_TESTPAT="
  set "MCLA_DIRECT_BACKBUFFER="& set "MCLA_NO_DSV="& set "MCLA_NO_EXPAND="
  set "MCLA_UNSKIP="& set "MCLA_F930="& set "MCLA_SKIP_PRIM8="
  set "MCLA_VB_DUMP="& set "MCLA_USE_PRECOMPILED_PSO="& set "MCLA_BOOT_CENSUS="
  set "MCLA_PARK_SAMPLE="& set "MCLA_SCAN_VTABLE="& set "MCLA_BMP_DUMP="
  set "MCLA_PROTECT_DRAW_BUFFER="& set "MCLA_PROBE_READBACK_BB="
  set "MCLA_PROBE_FLUSH_RED="& set "MCLA_T43_FORCE_INV_GATE="
  set "cp_deferred_consume_experiment="& set "cp_wait_regmem_ms="
  set "cp_wait_regmem_nowriter_ms="& set "cp_ib_trace="
  set "mcla_xtl_title_alloc="& set "mcla_phys_subpage="
)

rem --- apply CLI overrides AFTER the clearing block (so they survive) ---------
if defined OPT_LOGLEVEL set "MCLA_LOG_LEVEL=!OPT_LOGLEVEL!"
if defined OPT_QUIET set "MCLA_QUIET=!OPT_QUIET!"
if defined OPT_KEEPENV set "MCLA_KEEP_ENV=!OPT_KEEPENV!"
if "!OPT_PSOCACHE!"=="off" set "MCLA_NO_PSOCACHE=1"
if "!OPT_PROBES!"=="on" set "MCLA_PROBE_READBACK_BB=1"& set "MCLA_PROBE_FLUSH_RED=1"
if defined OPT_DXC_DIR set "MCLA_DXC_DIR=!OPT_DXC_DIR!"
if defined OPT_ASSET_CACHE set "MCLA_ASSET_CACHE=!OPT_ASSET_CACHE!"
if defined OPT_MAINWAIT set "MCLA_MAINWAIT_BOUND_MS=!OPT_MAINWAIT!"

rem --- timestamped immutable soak-log pair ------------------------------------
set "TS="
for /f "usebackq delims=" %%T in (`powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss" 2^>nul`) do set "TS=%%T"
if not defined TS (
  rem Locale-safe fallback: date+time sanitized, never overwritten (checked below).
  set "TS=%date%_%time%"
  set "TS=!TS:/=-!"
  set "TS=!TS::=-!"
  set "TS=!TS:.=-!"
  set "TS=!TS: =0!"
)
set "LOGOUT=w%TS%.log"
set "LOGERR=w%TS%_err.log"
set "LOGMCLA=w%TS%-mcla.log"
set "HELPERLOG=w%TS%_helper.log"
if exist "%LOGOUT%" (
  echo [run_mcla] REFUSED: "%LOGOUT%" already exists - will not overwrite evidence. 1>&2
  exit /b 4
)
title "MCLA SOAK %TS%"

rem --- archive PREVIOUS cache log before this launch truncates it -------------
rem Dry-run never launches, so it must not rotate the live sink.
if "!OPT_DRYRUN!"=="1" goto no_archive
if exist "cache\mcla.log" copy /y "cache\mcla.log" "%LOGMCLA%.prev" >nul 2>&1
for %%R in (1 2 3 4 5) do (
  if exist "cache\mcla.log.%%R" copy /y "cache\mcla.log.%%R" "%LOGMCLA%.prev.%%R" >nul 2>&1
)
goto arch_done
:no_archive
:arch_done

rem --- renderer config: serve only if missing, never clobber ------------------
if not exist "cache\mcla.toml" (
  echo renderer_mode = native> "cache\mcla.toml"
)
rem Apply --usegpu BEFORE the header so the header shows the effective mode.
rem The file is git-ignored (cache/ in .gitignore), so this cannot dirty the tree.
if "!OPT_DRYRUN!"=="1" goto dry_no_toml_write
if defined OPT_RENDERER GOTO toml_write
goto toml_done
:toml_write
findstr /v /c:"renderer_mode" "cache\mcla.toml" > "cache\mcla.toml.tmp" 2>nul
echo renderer_mode = !OPT_RENDERER!>> "cache\mcla.toml.tmp"
move /y "cache\mcla.toml.tmp" "cache\mcla.toml" >nul
goto toml_done
:dry_no_toml_write
:toml_done

rem --- soak header (self-describing log for later gate comparison) ------------
(
echo ===== run_mcla soak header =====
echo TS=%TS%
echo DATE=%date%_%time%
echo CWD=%CD%
for %%F in ("mcla.exe") do echo EXE=mcla.exe SIZE=%%~zF MTIME=%%~tF
echo MCLA_QUIET=%MCLA_QUIET% MCLA_LOG_LEVEL=%MCLA_LOG_LEVEL% MCLA_KEEP_ENV=%MCLA_KEEP_ENV%
echo OPTIONS logLevel=!OPT_LOGLEVEL! quiet=!OPT_QUIET! usegpu=!OPT_RENDERER! resolution=!OPT_RESOLUTION! resWH=!RES_W!x!RES_H! window=!OPT_WINDOW! gamma=!OPT_GAMMA! icon=!OPT_ICON! topmost=!OPT_TOPMOST! probes=!OPT_PROBES! psocache=!OPT_PSOCACHE! asynccompile=!OPT_ASYNC! dxcDir=!OPT_DXC_DIR! assetCache=!OPT_ASSET_CACHE! mainwaitMs=!OPT_MAINWAIT! keepEnv=!OPT_KEEPENV! dryRun=!OPT_DRYRUN!
if "!OPT_WINDOW!"=="fullscreen" echo WARN fullscreen requested but UNSUPPORTED: fullscreen CVar undeclared ^(src/app.cpp:234-236^); continuing windowed
if "!OPT_ASYNC!"=="off" echo WARN asynccompile=off requested but UNSUPPORTED: no toggle in src/ ^(render_thread.cpp:150^); async worker still runs; use --no-psocache for the cache control
echo WARN gamma=!OPT_GAMMA! is LOGGED ONLY: VdGetCurrentDisplayGamma is a stub ^(imports.cpp:7337^), gamma LUT unmodeled ^(gpu_cp.cpp:894-895^); exe ignores it
echo RESOLUTION note: startup is hardcoded 1280x720 ^(src/app.h:84-85^); requested !RES_W!x!RES_H! is applied post-launch via SDL resize ^(app.cpp:307-310,343-358^) helper when other than 1280x720
echo --- effective MCLA_/cp_ env ^(gate-comparison inputs^) ---
set | findstr /i "MCLA_ cp_ mcla_"
if defined OPT_RENDERER echo RENDERER_WOULD_SET=!OPT_RENDERER! ^(dry-run: cache\mcla.toml NOT written^)
echo --- cache\mcla.toml ---
type "cache\mcla.toml"
echo --- previous cache log archived to "%LOGMCLA%.prev" ---
) > "%LOGOUT%" 2> "%LOGERR%"
if "!OPT_DRYRUN!"=="1" goto dryrun_footer
echo Starting MCLA ^(native D3D12^). OPTIONS-logLevel=!OPT_LOGLEVEL! usegpu=!OPT_RENDERER! resolution=!RES_W!x!RES_H! window=!OPT_WINDOW! gamma=!OPT_GAMMA! icon=!OPT_ICON! topmost=!OPT_TOPMOST! probes=!OPT_PROBES! psocache=!OPT_PSOCACHE! asynccompile=!OPT_ASYNC!. Soak log: build\%LOGOUT% ^+ %LOGERR%
echo Keep this console + the game window visible/foreground for the whole soak. Close the window, Ctrl+C, or kill mcla.exe to quit.

rem --- host window helper: post-launch resize + icon/topmost only -------------
rem NEVER for mcla.exe itself (foreground contract above). This helper only
rem moves the already-created SDL window down the supported resize path.
set "NEED_HELPER=0"
if not "!RES_W!x!RES_H!"=="1280x720" set "NEED_HELPER=1"
if not "!OPT_ICON!"=="normal" set "NEED_HELPER=1"
if not "!OPT_TOPMOST!"=="0" set "NEED_HELPER=1"
if "!NEED_HELPER!"=="0" goto helper_done
echo $ErrorActionPreference='SilentlyContinue' > "cache\run_mcla_helper.ps1"
echo function logmsg($m){ Add-Content -Path '!HELPERLOG!' -Value ((Get-Date -Format 'HH:mm:ss')+' '+$m) } >> "cache\run_mcla_helper.ps1"
echo logmsg 'helper start res=!RES_W!x!RES_H! icon=!OPT_ICON! topmost=!OPT_TOPMOST!' >> "cache\run_mcla_helper.ps1"
echo Add-Type -TypeDefinition 'using System; using System.Runtime.InteropServices; public class W { [DllImport("user32.dll")] public static extern bool MoveWindow(IntPtr h,int x,int y,int w,int hh,bool r); [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h,int c); [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr a,int x,int y,int w,int hh,uint f); [DllImport("user32.dll")] public static extern int GetSystemMetrics(int n); }' >> "cache\run_mcla_helper.ps1"
echo $hwnd=[IntPtr]::Zero >> "cache\run_mcla_helper.ps1"
echo $deadline=(Get-Date).AddSeconds(120) >> "cache\run_mcla_helper.ps1"
echo while ((Get-Date) -lt $deadline) { $p=Get-Process -Name 'mcla' -ErrorAction SilentlyContinue; if($p){ foreach($proc in $p){ if($proc.MainWindowHandle -ne 0){ $hwnd=$proc.MainWindowHandle; break } } }; if($hwnd -ne [IntPtr]::Zero){ break }; Start-Sleep -Milliseconds 500 } >> "cache\run_mcla_helper.ps1"
echo if($hwnd -eq [IntPtr]::Zero){ logmsg 'no window found'; exit 0 } >> "cache\run_mcla_helper.ps1"
echo Start-Sleep -Milliseconds 1500 >> "cache\run_mcla_helper.ps1"
echo $sw=[W]::GetSystemMetrics(0) >> "cache\run_mcla_helper.ps1"
echo $sh=[W]::GetSystemMetrics(1) >> "cache\run_mcla_helper.ps1"
echo $x=0 >> "cache\run_mcla_helper.ps1"
echo if($sw -gt !RES_W!){ $x=[int](($sw - !RES_W!)/2) } >> "cache\run_mcla_helper.ps1"
echo $y=0 >> "cache\run_mcla_helper.ps1"
echo if($sh -gt !RES_H!){ $y=[int](($sh - !RES_H!)/2) } >> "cache\run_mcla_helper.ps1"
echo $ok=[W]::MoveWindow($hwnd,$x,$y,!RES_W!,!RES_H!,$false) >> "cache\run_mcla_helper.ps1"
echo logmsg ('MoveWindow ok='+$ok+' '+$x+' '+$y+' !RES_W!x!RES_H!') >> "cache\run_mcla_helper.ps1"
echo if('!OPT_TOPMOST!' -eq '1'){ $ok2=[W]::SetWindowPos($hwnd,[IntPtr](-1),0,0,0,0,3); logmsg ('SetWindowPos topmost ok='+$ok2) } >> "cache\run_mcla_helper.ps1"
echo $code=9 >> "cache\run_mcla_helper.ps1"
echo if('!OPT_ICON!' -eq 'minimized'){ $code=6 } >> "cache\run_mcla_helper.ps1"
echo if('!OPT_ICON!' -eq 'hidden'){ $code=0 } >> "cache\run_mcla_helper.ps1"
echo if('!OPT_ICON!' -ne 'normal'){ $ok3=[W]::ShowWindow($hwnd,$code); logmsg ('ShowWindow code='+$code+' ok='+$ok3) } >> "cache\run_mcla_helper.ps1"
echo logmsg 'helper done' >> "cache\run_mcla_helper.ps1"
start "" /min powershell -NoProfile -ExecutionPolicy Bypass -File "cache\run_mcla_helper.ps1"
echo Helper started (resize/icon/topmost): build\%HELPERLOG% ^(host window control only; mcla.exe stays foreground^)
goto helper_launch_done
:helper_done
:helper_launch_done

rem --- foreground synchronous launch: append output to the new pair (never truncate header) ---
.\mcla.exe >> "%LOGOUT%" 2>> "%LOGERR%"
set "RC=%ERRORLEVEL%"

rem --- archive THIS run's cache log (gate evidence) + footer -------------------
if exist "cache\mcla.log" copy /y "cache\mcla.log" "%LOGMCLA%" >nul 2>&1
for %%R in (1 2 3 4 5) do (
  if exist "cache\mcla.log.%%R" copy /y "cache\mcla.log.%%R" "%LOGMCLA%.%%R" >nul 2>&1
)
(
echo.
echo ===== run_mcla soak footer =====
echo TS=%TS% END=%date%_%time% EXIT=%RC%
echo LOGOUT=%LOGOUT% LOGERR=%LOGERR% LOGMCLA=%LOGMCLA%
echo OPTIONS logLevel=!OPT_LOGLEVEL! usegpu=!OPT_RENDERER! resolution=!RES_W!x!RES_H! window=!OPT_WINDOW! gamma=!OPT_GAMMA! icon=!OPT_ICON! topmost=!OPT_TOPMOST! probes=!OPT_PROBES! psocache=!OPT_PSOCACHE! asynccompile=!OPT_ASYNC!
if "!OPT_WINDOW!"=="fullscreen" echo WARN fullscreen no-op ^(see header^)
if "!OPT_ASYNC!"=="off" echo WARN asynccompile no-op ^(see header^)
) >> "%LOGOUT%" 2>> "%LOGERR%"
echo Finished. EXIT=%RC% Log: build\%LOGOUT% (+ %LOGMCLA%)
if defined NEED_HELPER echo Helper log: build\%HELPERLOG%

endlocal & exit /b %RC%

:dryrun_footer
(
echo.
echo ===== run_mcla soak footer =====
echo TS=%TS% END=%date%_%time% EXIT=0 DRYRUN=1
echo LOGOUT=%LOGOUT% LOGERR=%LOGERR% LOGMCLA=%LOGMCLA% ^(no exe launched; cache\mcla.log NOT archived; cache\mcla.toml NOT written^)
echo OPTIONS logLevel=!OPT_LOGLEVEL! usegpu=!OPT_RENDERER! resolution=!RES_W!x!RES_H! window=!OPT_WINDOW! gamma=!OPT_GAMMA! icon=!OPT_ICON! topmost=!OPT_TOPMOST! probes=!OPT_PROBES! psocache=!OPT_PSOCACHE! asynccompile=!OPT_ASYNC! dxcDir=!OPT_DXC_DIR! assetCache=!OPT_ASSET_CACHE! mainwaitMs=!OPT_MAINWAIT! keepEnv=!OPT_KEEPENV!
echo DRYRUN-OK: header above is what a real launch would write; no mcla.exe was started.
) >> "%LOGOUT%" 2>> "%LOGERR%"
echo [run_mcla] DRY-RUN OK. Sample header+OPTIONS written to build\%LOGOUT% (no exe launched).
endlocal & exit /b 0

:do_help
echo Usage: run_mcla.bat [options]
echo.
echo Long-running foreground launcher (20+ min soaks). With no options the
echo behaviour is unchanged: logLevel Info, renderer from cache\mcla.toml,
echo 1280x720 windowed, no env-gated probes, PSO cache on, fresh w*.log pair.
echo.
echo SUPPORTED (applied before launch; invalid values exit 2, no log minted):
echo   --logLevel 0..6        MCLA_LOG_LEVEL (src/app.cpp:52-55). Default 2.
echo   --quiet 0^|1             MCLA_QUIET (src/common/logging.h:28-30). Default 1.
echo   --usegpu native^|legacy^|capture (alias --renderer)
echo                            renderer_mode CVar (src/renderer_mode.cpp:8)
echo                            via cache\mcla.toml. Default: file value.
echo   --resolution PRESET    720p=1280x720 1080p=1920x1080 1440p=2560x1440
echo                            4k=3840x2160 800p=1280x800, or WxH e.g. 2560x1440
echo                            (W 640..7680, H 480..4320). Startup size is
echo                            hardcoded 1280x720 (src/app.h:84-85); the helper
echo                            resizes post-launch via the SDL resize path
echo                            (src/app.cpp:307-310,343-358). Default 1280x720.
echo   --icon normal^|minimized^|hidden  Host window ShowWindow (no exe lever:
echo                            flags fixed at src/app.cpp:232). Default normal.
echo   --topmost 0^|1           Host SetWindowPos always-on-top. Default 0.
echo   --probes ^| --no-probes  MCLA_PROBE_READBACK_BB/FLUSH_RED (d3d12_back-
echo                            end.cpp:6330,8273). Only these env-gated probes.
echo                            Default --no-probes.
echo   --psocache ^| --no-psocache  MCLA_NO_PSOCACHE (pipeline_cache.cpp:3235).
echo                            Default on.
echo   --dxc-dir PATH         MCLA_DXC_DIR (dxc_runtime.cpp:360).
echo   --asset-cache PATH     MCLA_ASSET_CACHE (asset_cache.h:47).
echo   --mainwait-ms N        MCLA_MAINWAIT_BOUND_MS (imports.cpp:2815).
echo   --keep-env 0^|1         MCLA_KEEP_ENV passthrough for all other debug
echo                            env vars (NOT one flag each - see header).
echo.
echo VALIDATED NO-OP (accepted + logged, IGNORED by the exe - do not cite as
echo working):
echo   --windowed ^| --fullscreen  fullscreen CVar undeclared (src/app.cpp:234-236),
echo                            argv ignored (src/main.cpp:7-9). Always windowed.
echo   --gamma 0.4..4.0       Gamma stub (imports.cpp:7337), LUT unmodeled
echo                            (gpu_cp.cpp:894-895). Logged only.
echo   --asynccompile on^|off  No toggle; worker always on (render_thread.cpp:150).
echo                            Use --no-psocache for cache control.
echo.
echo UTILITY:
echo   --help                 This text (exit 0, no exe needed).
echo   --dry-run (--print-config)  Write header+OPTIONS to a fresh w*.log and
echo                            exit 0 WITHOUT launching (proves logging).
echo.
echo Use space form (--logLevel 2), not --logLevel=2. Single-dash aliases
echo (-logLevel, -resolution, -windowed, -fullscreen, -no-probes, -usegpu,
echo -asynccompile) are accepted. Example:
echo   run_mcla.bat --logLevel 2 --usegpu native --resolution 2560x1440
echo   run_mcla.bat --dry-run --resolution 2560x1440 --topmost 1
endlocal & exit /b 0
