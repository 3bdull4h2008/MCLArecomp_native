#!/usr/bin/env python
"""Build a labelled contact sheet of MCLA frames sampled at ~1 second intervals.

Use this for EVERY visual verification. One image per second, not a cherry-picked
single frame, so a broken/intermittent 3D scene cannot hide.

Usage:
  python tools/frame_sheet.py <frames_dir> <out_png> [--interval 1.0] [--cols 5]

frames_dir: directory holding d3d12_frame_*.bmp (e.g. build/reg_<tag>/ or build/)
The sheet is labelled with frame index and seconds since the first captured frame.
"""
import sys, os, glob, argparse
from PIL import Image, ImageDraw, ImageFont

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("frames_dir")
    ap.add_argument("out_png")
    ap.add_argument("--interval", type=float, default=1.0)
    ap.add_argument("--cols", type=int, default=5)
    ap.add_argument("--thumb", type=int, default=320)
    args = ap.parse_args()

    files = sorted(glob.glob(os.path.join(args.frames_dir, "d3d12_frame_*.bmp")))
    if not files:
        print("NO FRAMES in", args.frames_dir)
        return 1
    entries = []
    for f in files:
        try:
            mt = os.path.getmtime(f)
            im = Image.open(f)
            entries.append((mt, f, im.size))
        except Exception:
            pass
    if not entries:
        print("NO READABLE FRAMES")
        return 1
    t0 = entries[0][0]
    # sample ~one frame per interval second
    picked = []
    for mt, f, size in entries:
        rel = mt - t0
        if not picked or rel - picked[-1][0] >= args.interval:
            picked.append((rel, f, size))

    tw, th = args.thumb, args.thumb * 9 // 16
    label_h = 22
    rows = (len(picked) + args.cols - 1) // args.cols
    sheet = Image.new("RGB", (args.cols * tw, rows * (th + label_h)), (16, 16, 16))
    d = ImageDraw.Draw(sheet)
    try:
        font = ImageFont.truetype("arial.ttf", 14)
    except Exception:
        font = ImageFont.load_default()
    for i, (rel, f, size) in enumerate(picked):
        x = (i % args.cols) * tw
        y = (i // args.cols) * (th + label_h)
        try:
            im = Image.open(f).convert("RGB")
            im.thumbnail((tw, th))
            sheet.paste(im, (x, y))
        except Exception:
            pass
        d.text((x + 4, y + th + 2),
               "%s  +%.1fs  %dx%d" % (os.path.basename(f)[12:16], rel, size[0], size[1]),
               fill=(230, 230, 230), font=font)
    sheet.save(args.out_png)
    print("SHEET", args.out_png, "frames", len(picked), "of", len(entries),
          "interval", args.interval, "size", sheet.size)
    return 0

if __name__ == "__main__":
    sys.exit(main())
