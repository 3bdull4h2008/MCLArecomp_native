"""State-driven input for unattended runs: watches the game's own dumped frames (build/d3d12_frame_*.bmp,
MCLA_BMP_DUMP) and writes one button word to the MCLA_TEMP_CMDFILE file. No timing assumptions, no OS keys.
Policy: if the picture has not changed for STILL polls -> press A (advance); every 3rd stuck press -> START;
if still stuck after 8 presses -> DOWN then A (menu move). Logs each decision with the frame it saw.
Usage: python tools/autodrive.py <cmdfile> <seconds> [framedir]"""
import sys, time, glob, os
from PIL import Image, ImageChops

cmd, secs = sys.argv[1], float(sys.argv[2])
fdir = sys.argv[3] if len(sys.argv) > 3 else "build"
STILL = 3
prev, still, presses, seen = None, 0, 0, None
end = time.time() + secs
log = open(os.path.join(fdir, "autodrive.log"), "w")
def press(w):
    with open(cmd, "w") as f: f.write(w)
while time.time() < end:
    fs = sorted(glob.glob(os.path.join(fdir, "d3d12_frame_*.bmp")), key=os.path.getmtime)
    if not fs: time.sleep(1); continue
    if fs[-1] == seen: time.sleep(1); continue
    seen = fs[-1]
    try: im = Image.open(seen).convert("L").resize((64, 36))
    except Exception: continue
    mean = sum(im.getdata()) / (64 * 36)
    if prev is not None:
        d = sum(ImageChops.difference(im, prev).getdata()) / (64 * 36)
        still = still + 1 if d < 1.5 else 0
    prev = im
    act = ""
    if still >= STILL and mean >= 4 and not os.path.exists(cmd):  # never press on a black frame: a movie/loading may be about to start
        presses += 1
        act = "START" if presses % 3 == 0 else ("DOWN" if presses % 8 == 0 and not os.environ.get("NODOWN") else "A")
        press(act); still = 0
    log.write(f"{time.time():.0f} {os.path.basename(seen)} mean={mean:.0f} still={still} act={act}\n"); log.flush()
    time.sleep(1)
