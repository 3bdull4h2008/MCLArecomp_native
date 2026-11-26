#!/usr/bin/env python
"""One-command visual verification: boot -> 1-second frame sheet.

Usage:
  python tools/visual_check.py <tag> [seconds] [ENV=k=v;...]

Runs build/regress.ps1 (frame dump, MCLA_LOG_LEVEL=4), then tools/frame_sheet.py
on the resulting build/reg_<tag>/ frames. Always produces a contact sheet so an
intermittent/broken 3D scene cannot hide behind a single cherry-picked frame.
"""
import sys, os, subprocess

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tag = sys.argv[1] if len(sys.argv) > 1 else "vc"
secs = sys.argv[2] if len(sys.argv) > 2 else "110"
extra = sys.argv[3] if len(sys.argv) > 3 else ""
cmd = ["powershell", "-ExecutionPolicy", "Bypass", "-File",
       os.path.join(root, "build", "regress.ps1"), "-Tag", tag]
if extra:
    cmd += ["-Env", extra]
print("BOOT:", " ".join(cmd), flush=True)
r = subprocess.run(cmd, cwd=root, timeout=int(secs) + 180)
print("BOOT rc", r.returncode, flush=True)
frames = os.path.join(root, "build", "reg_" + tag)
out = os.path.join(root, "build", tag + "_sheet.png")
r2 = subprocess.run([sys.executable, os.path.join(root, "tools", "frame_sheet.py"),
                     frames, out, "--interval", "1.0", "--cols", "5"], cwd=root)
print("SHEET rc", r2.returncode, "->", out, flush=True)
