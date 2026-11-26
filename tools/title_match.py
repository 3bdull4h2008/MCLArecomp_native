#!/usr/bin/env python3
"""Quantify current-title vs reference structural similarity (edge/structure based).
Usage: python tools/title_match.py <frame.bmp> <reference.jpg> [out.png]
"""
import sys
from PIL import Image, ImageChops
import numpy as np

frame, ref = sys.argv[1], sys.argv[2]
out = sys.argv[3] if len(sys.argv) > 3 else None
a = Image.open(frame).convert('RGB').resize((320, 180))
b = Image.open(ref).convert('RGB').resize((320, 180))
ga = np.asarray(a.convert('L'), dtype=np.float32)
gb = np.asarray(b.convert('L'), dtype=np.float32)

# normalized cross-correlation of luminance (structure, tolerant of exposure)
def ncc(x, y):
    x = x - x.mean(); y = y - y.mean()
    d = np.sqrt((x*x).sum() * (y*y).sum())
    return float((x*y).sum() / d) if d else 0.0

# gradient-magnitude correlation (edges = geometry/skyline/roads)
def grad(x):
    gx = np.abs(np.diff(x, axis=1)).mean()
    gy = np.abs(np.diff(x, axis=0)).mean()
    return gx, gy

corr = ncc(ga, gb)
print('luminance NCC      = %.3f' % corr)
print('mean lum  cur/ref  = %.1f / %.1f' % (ga.mean(), gb.mean()))
# masked MAD excluding the central logo band (x 90..230, y 60..110 in 320x180)
mask = np.ones((180, 320), bool); mask[58:112, 88:232] = False
mad = np.abs(ga - gb)[mask].mean()
print('scene MAD (no logo)= %.1f / 255' % mad)

if out:
    # 50/50 overlay to show alignment
    ov = Image.blend(a, b, 0.5).resize((1280, 720))
    ov.save(out)
    print('overlay', out)
