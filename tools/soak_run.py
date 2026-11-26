#!/usr/bin/env python3
"""Single-instance bounded soak runner (rules 5, 6, 11, 19).

Why this exists: AGENTS.md has told every agent to soak through
`build/soak_run.py` since 2026-10, but `build/` is regenerable and the file is
gone. The hand recipe (`cd build && timeout 90 ./mcla.exe > X.log`) then fails
three ways that each cost a soak in the 2026-11-23 session:

  * logging is Off by default (`src/app.cpp:121-126`), so the log came out
    256 bytes and the census read all-zero -- `MCLA_LOG_LEVEL=2` is required;
  * a leaked `mcla.exe` from the previous run holds the D3D12 device (rule 6);
  * `timeout`/`>` semantics differ between Git Bash, PowerShell and cmd, and
    `cmd /c "timeout 300 mcla.exe"` is a syntax error (skill, 2026-09-28).

This runner fixes all three by doing the run in Python: kill stale, fresh log
name (rule 5), `MCLA_LOG_LEVEL` in the child env, `wait(secs)` + kill, and a
fatal-token gate over the log it just wrote.

Semantics: rc=124 ("timeout fired") means the process SURVIVED, which is the
normal good result. An early exit is a FAIL.

Usage:
  python tools/soak_run.py --tag md10a --secs 90
  python tools/soak_run.py --tag md10b --secs 90 --scale 2 --env MCLA_NO_CLIPEXT=1
  python tools/soak_run.py --tag md10c --baseline build/w193.log
  python tools/soak_run.py --dry-run --tag plan-only

Exit codes (same contract as tools/soak60.ps1):
  0 PASS  - survived --secs AND zero fatal-gate lines
  1 FAIL  - early exit and/or fatal-gate lines
  2 SETUP - missing exe / build dir / refused (ninja running, or an mcla.exe
            already running -- never killed; only the PID this runner started
            is ever terminated)
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"

# soak60.ps1 / soak_census token set -- but counted on the MESSAGE's own label, not
# anywhere in the line. Rule 19 corollary (a second printer can emit your token):
# 2026-11-23, lv_on_120s.log had four VERTEX-V0 hex dumps whose guest vertex bytes
# contained the nibble run C0000005; the naive substring gate reported a crash that
# never happened. Real printers: imports.cpp:11369 ("Fatal error dispatcher..."),
# imports.cpp:11431/11454 (FATAL-SOFT*), boot_host.cpp:2854/2867/3198/3204
# (code=0x{:08X}). Hex dumps cannot produce any of these shapes.
GATE_TOKENS = (b"fatal error", b"fatal-soft", b"c0000005", b"assert", b"skip fatal")
_MSG_PREFIX = re.compile(rb"^\[[^\]]*\] \[[^\]]*\] (.*)$")
GATE_RULES = (
    re.compile(rb"^fatal error", re.I),
    re.compile(rb"^fatal-soft", re.I),
    re.compile(rb"\bcode=0x0*c0000005\b", re.I),
    re.compile(rb"(^assert\b|assertion failed)", re.I),
    re.compile(rb"\bskip fatal\b", re.I),
)


def _tasklist(image: str) -> list[str]:
    try:
        out = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {image}", "/NH"],
            capture_output=True, text=True, timeout=30,
        ).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [ln for ln in out.splitlines() if image.lower() in ln.lower()]


def _kill_pid_cmd(pid: int) -> list[str]:
    """Kill only this PID and its children. Never by image name (other sessions' mcla.exe)."""
    return ["taskkill", "/PID", str(pid), "/T", "/F"]


def _kill_pid(pid: int) -> None:
    subprocess.run(_kill_pid_cmd(pid), capture_output=True, text=True, timeout=30)


def _selftest() -> int:
    assert _kill_pid_cmd(1234) == ["taskkill", "/PID", "1234", "/T", "/F"]
    assert _kill_pid_cmd(7)[1] == "/PID"
    print("SELFTEST OK")
    return 0


def _fresh_log(tag: str, secs: int) -> Path:
    """Rule 5: never reuse or overwrite a soak log; they are the evidence base."""
    log = BUILD / f"{tag}_{secs}s.log"
    n = 2
    while log.exists():
        log = BUILD / f"{tag}_{secs}s_{n}.log"
        n += 1
    return log


def _scan_gate(log: Path) -> tuple[list[tuple[bytes, int]], list[tuple[bytes, str]]]:
    """Line counts per gate token + first matching line for each (rule 19: show it firing).

    Tokens are matched against the log MESSAGE (after `[time] [level] `), so a hex
    dump of guest bytes can never fake a fault. Lines without that prefix (raw
    stderr, e.g. a CRT assert) are matched whole.
    """
    counts = {t: 0 for t in GATE_TOKENS}
    first: dict[bytes, str] = {}
    with log.open("rb") as fh:
        for raw in fh:
            m = _MSG_PREFIX.match(raw)
            msg = m.group(1) if m else raw
            for t, rule in zip(GATE_TOKENS, GATE_RULES):
                if rule.search(msg):
                    counts[t] += 1
                    if t not in first:
                        first[t] = raw.decode("utf-8", "replace").strip()[:200]
    return [(t, counts[t]) for t in GATE_TOKENS], list(first.items())


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--tag", default=None,
                    help="log name prefix (default soak_YYYYMMDD_HHMM); a suffix is added if taken")
    ap.add_argument("--secs", type=int, default=60, help="wall-clock bound (default 60)")
    ap.add_argument("--exe", default=str(BUILD / "mcla.exe"),
                    help="executable to run (default build/mcla.exe); cwd is always build/")
    ap.add_argument("--arg", action="append", default=[], help="extra child argument (repeatable)")
    ap.add_argument("--env", action="append", default=[], metavar="K=V",
                    help="extra child env var (repeatable), e.g. MCLA_RENDER_SCALE=2")
    ap.add_argument("--scale", type=int, default=None,
                    help="shorthand for --env MCLA_RENDER_SCALE=N")
    ap.add_argument("--log-level", default="2",
                    help="MCLA_LOG_LEVEL for the child (default 2 = census; 6 = off)")
    ap.add_argument("--baseline", default=None,
                    help="run tools/soak_census.py <log> <baseline> afterwards")
    ap.add_argument("--no-census", action="store_true", help="skip the baseline census")
    ap.add_argument("--dry-run", action="store_true", help="print the plan, run nothing")
    ap.add_argument("--force", action="store_true", help="soak even if ninja.exe is running")
    args = ap.parse_args(argv)

    tag = args.tag or "soak_" + _dt.datetime.now().strftime("%Y%m%d_%H%M")
    exe = Path(args.exe)
    if not exe.is_absolute():
        exe = (ROOT / exe).resolve()
    log = _fresh_log(tag, args.secs)

    env = dict(os.environ)
    env["MCLA_LOG_LEVEL"] = args.log_level
    if args.scale is not None:
        env["MCLA_RENDER_SCALE"] = str(args.scale)
    deltas = {"MCLA_LOG_LEVEL": args.log_level}
    if args.scale is not None:
        deltas["MCLA_RENDER_SCALE"] = str(args.scale)
    for kv in args.env:
        if "=" not in kv:
            print(f"SETUP: --env needs K=V, got {kv!r}", file=sys.stderr)
            return 2
        k, v = kv.split("=", 1)
        env[k] = v
        deltas[k] = v

    print(f"EXE={exe}")
    print(f"CWD={BUILD}")
    print(f"LOG={log}")
    print("ENV=" + " ".join(f"{k}={v}" for k, v in sorted(deltas.items())))
    if args.arg:
        print("ARGV=" + " ".join(args.arg))

    if args.dry_run:
        print("DRY-RUN: nothing launched")
        return 0

    if not BUILD.is_dir():
        print("SETUP: build/ missing -- configure first (build_on_e.bat)", file=sys.stderr)
        return 2
    if not exe.exists():
        print(f"SETUP: {exe} missing -- build first (build_on_e.bat build)", file=sys.stderr)
        return 2

    # Rule 11: a concurrent ninja means the exe is being rewritten under us.
    if not args.force and _tasklist("ninja.exe"):
        print("REFUSED: ninja.exe is running (rule 11 concurrent-session check). "
              "Wait for the build, or pass --force.", file=sys.stderr)
        return 2

    # Never kill an mcla.exe we did not start (another session may own it): refuse instead.
    running = _tasklist("mcla.exe")
    if running:
        print(f"REFUSED: {len(running)} mcla.exe already running (not ours; not killed). "
              "Wait for it to exit or stop it yourself.", file=sys.stderr)
        return 2

    started = time.time()
    with log.open("wb") as out:
        proc = subprocess.Popen([str(exe), *args.arg], cwd=str(BUILD), env=env,
                                stdout=out, stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL)
        try:
            proc.wait(timeout=args.secs)
            rc = proc.returncode
            survived = False
        except subprocess.TimeoutExpired:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)
            rc = 124
            survived = True
        elapsed = time.time() - started

    if proc.poll() is None:  # rule 6: never leave our soaked instance behind (our PID only)
        _kill_pid(proc.pid)
    size = log.stat().st_size
    print(f"BOOT_RC={rc} survived={int(survived)} secs={elapsed:.1f} bytes={size}")

    if size < 1024:
        # The 2026-11-23 trap: a tiny log means the child never logged at all.
        print(f"WARN log-empty bytes={size} -- check MCLA_LOG_LEVEL and the exe's own stderr")

    rows, first = _scan_gate(log)
    print("GATE " + " ".join(f"{t.decode()}={n}" for t, n in rows))
    for t, line in first:
        print(f"FIRST[{t.decode()}] {line}")
    fatal = sum(n for _, n in rows)

    if args.baseline and not args.no_census:
        census = ROOT / "tools" / "soak_census.py"
        baseline = Path(args.baseline)
        if not baseline.is_absolute():
            baseline = (ROOT / baseline).resolve()
        if census.exists() and baseline.exists():
            print(f"CENSUS {log.name} vs {baseline.name}")
            subprocess.run([sys.executable, str(census), str(log), str(baseline)],
                           cwd=str(ROOT))
        else:
            print(f"CENSUS skipped (missing {census if not census.exists() else baseline})")

    ok = survived and fatal == 0
    print(f"GATE {'PASS' if ok else 'FAIL'}")
    return 0 if ok else 1


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(_selftest())
    sys.exit(main(sys.argv[1:]))
