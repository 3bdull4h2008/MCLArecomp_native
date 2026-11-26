#!/usr/bin/env bash
# One command for the whole measure cycle: (build) -> boot -> stage sheet -> CPU-replay overlay -> summary.
# Usage (Git Bash, repo root):  tools/cycle.sh <tag> [seconds=75] [build]
#   tools/cycle.sh ed80            boot only
#   tools/cycle.sh ed80 100 build  rebuild first, 100 s run
# Output: build/<tag>.log, build/<tag>_out/{sheet.png,scene.png,overlay.png,final.png,summary.txt}
set -u
cd "$(dirname "$0")/.." || exit 1
tag=${1:?tag}; secs=${2:-75}; out="build/${tag}_out"
# Several agents share this tree: one build+boot at a time. The lock is a directory (atomic mkdir);
# a stale lock (>15 min, crashed holder) is broken.
until mkdir build/.cycle_lock 2>/dev/null; do
  [ -n "$(find build/.cycle_lock -maxdepth 0 -mmin +15 2>/dev/null)" ] && rmdir build/.cycle_lock 2>/dev/null
  sleep 10; done
LOCK="$PWD/build/.cycle_lock"; trap 'rmdir "$LOCK" 2>/dev/null' EXIT
# Never kill by image name: the owner may be playing his own mcla.exe. Only the PID this script starts is stopped.
if [ "${3:-}" = build ]; then
  taskkill //F //IM ninja.exe >/dev/null 2>&1
  touch /tmp/cycle_stamp; sleep 1
  cmd //c ".\build_on_e.bat build" 2>&1 | tail -2
  # Stale-exe trap: the exe must be newer than every source file. (Comparing with the build start time
  # false-tripped when another agent had already built the same tree and ninja had nothing to do.)
  newest=$(ls -t src/*.cpp src/*.h src/*/*.cpp src/*/*.h 2>/dev/null | head -1)
  [ build/mcla.exe -nt "$newest" ] || { echo "BUILD DID NOT PRODUCE A NEW EXE (stale-exe trap: $newest is newer)"; exit 2; }
  if grep -q " error" build_ninja.log; then grep -m5 " error" build_ninja.log | cut -c1-240; exit 2; fi
fi
cd build || exit 1
rm -rf replay vs_hlsl stages/*.bmp d3d12_frame_*.bmp; mkdir -p "../$out" stages
# MCLA_NO_HOSTSERVE / MCLA_MEMSTREAM_NATIVE are default-on in the exe.
export MCLA_LOG_LEVEL=${MCLA_LOG_LEVEL:-2} MCLA_TEMP_EDRAM_FRAME=24 MCLA_TEMP_DUMP_VSALL=1 MCLA_TEMP_STAGES=1
# Optional: BMP=<presents to skip> dumps 24 presented frames, one every 25 presents (d3d12_frame_NN.bmp).
[ -n "${BMP:-}" ] && export MCLA_BMP_DUMP=1 MCLA_BMP_SKIP="$BMP" MCLA_BMP_EVERY="${EVERY:-25}"  # EVERY=1: consecutive frames
# Optional: REC=<frames> records that many presented frames (from BMP=<skip>, every EVERY presents, default 1)
# into build/<tag>_out/rec.mp4 -- the game's own frames only, never a desktop capture.
[ -n "${REC:-}" ] && export MCLA_BMP_DUMP=1 MCLA_BMP_SKIP="${BMP:-0}" MCLA_BMP_EVERY="${EVERY:-1}" MCLA_BMP_MAX="$REC"
# Optional: ENTER="35,50,62" presses Enter INSIDE the game at those seconds (MCLA_TEMP_AUTOENTER,
# src/kernel/xam.cpp). No OS keystrokes: an unfocused SendKeys would type into the user's windows.
[ -n "${ENTER:-}" ] && export MCLA_TEMP_AUTOENTER="$ENTER"
if [ -n "${AUTO:-}" ]; then export MCLA_TEMP_CMDFILE="$PWD/cmd_$tag.txt"; rm -f "$MCLA_TEMP_CMDFILE"
  python ../tools/autodrive.py "$MCLA_TEMP_CMDFILE" "$secs" . > /dev/null 2>&1 & fi
./mcla.exe > "$tag.log" 2> "${tag}_err.log" & MPID=$!
WPID=$(cat /proc/$MPID/winpid 2>/dev/null)
sleep "$secs"
[ -n "$WPID" ] && taskkill //F //PID "$WPID" >/dev/null 2>&1; kill -9 "$MPID" 2>/dev/null; wait "$MPID" 2>/dev/null
if ls d3d12_frame_*.bmp >/dev/null 2>&1; then python - "../$out" <<'E'
import sys, glob
from PIL import Image, ImageDraw
fs = sorted(glob.glob('d3d12_frame_*.bmp')); fs = fs[::max(1, len(fs) // 24)][:24]; W, H = 320, 180; c = 4
sheet = Image.new('RGB', (W * c, H * ((len(fs) + c - 1) // c)))
for i, f in enumerate(fs):
    im = Image.open(f).convert('RGB').resize((W, H)); ImageDraw.Draw(im).text((4, 4), f[12:16], fill=(255, 255, 0))
    sheet.paste(im, ((i % c) * W, (i // c) * H))
sheet.save(sys.argv[1] + '/frames.png')
E
fi
[ -n "${REC:-}" ] && ls d3d12_frame_*.bmp >/dev/null 2>&1 && ffmpeg -y -loglevel error -framerate 30 -i d3d12_frame_%04d.bmp -c:v libx264 -pix_fmt yuv420p -vf "scale=1280:-2" "../$out/rec.mp4"
S="../$out/summary.txt"
{
  echo "== $tag ($secs s) =="
  grep -a -m1 "FLOW-FIRST world" "$tag.log" | cut -c1-70
  grep -a -m1 "TEMP-EDRAM WORLD0" "$tag.log" | cut -c1-70
  for m in PC-PSO-CREATE-FAILED "TEMP-EDRAM REFUSE" POOL16-OOM ARENA-OOM "upload arena region exhausted" 887A0005 D3D12-DEBUGLAYER; do
    printf '%-32s %s\n' "$m" "$(grep -ac "$m" "$tag.log")"; done
  printf '%-32s %s\n' "BUDDY76-ALLOC ret=NULL" "$(grep -a BUDDY76-ALLOC "$tag.log" | grep -ac 'ret=00000000')"
  printf '%-32s %s\n' "Not owned by any known heap" "$(cat "$tag.log" "${tag}_err.log" | grep -ac 'Not owned by any known heap')"
  printf '%-32s %s\n' "REFUSING (err log)" "$(grep -ac REFUSING "${tag}_err.log")"
  grep -a "TEMP-RENDER-1S" "$tag.log" | tail -1 | cut -c1-260
  echo "-- input / save / movie / dialogs --"
  grep -a "KEYSTROKE-ENQ\|XAM-CENSUS\|XamShowMessageBoxUI\|SAVE-MOUNT\|SAVE-UNMOUNT\|SAVE-DEVICE\|DeviceSelector\|\.bik\|BINK\|Bink" "$tag.log" | cut -c1-200 | awk '{k=$3" "$4; if (c[k]++ < 3) print}' | head -40
  grep -a "MEM-CACHES" "$tag.log" | tail -1 | cut -c30-330
  grep -a "PHYS-FREE-SUM\|XTL-FREE-SUM" "$tag.log" | tail -2 | cut -c30-260
} > "$S" 2>&1
python ../tools/stage_sheet.py "$tag.log" stages >/dev/null 2>&1 && cp stages/sheet.png "../$out/sheet.png"
scene=$(ls stages/*1280x720_src1024*.bmp 2>/dev/null | grep -i "17EC0000\|18258000" | tail -1)
final=$(ls stages/*src720*.bmp 2>/dev/null | tail -1)
if [ -n "$scene" ]; then
  python ../tools/replay_overlay.py "$scene" "../$out/overlay.png" >> "$S" 2>&1
  python - "$scene" "${final:-$scene}" "../$out" <<'E'
import sys
from PIL import Image
Image.open(sys.argv[1]).convert('RGB').resize((1024, 576)).save(sys.argv[3] + '/scene.png')
Image.open(sys.argv[2]).convert('RGB').resize((1024, 576)).save(sys.argv[3] + '/final.png')
E
else echo "no scene stage dumped" >> "$S"; fi
cat "$S"
