#!/usr/bin/env python3
"""Aspect-aware title/reference comparison (fixes title_match.py flaws).
- Letterboxes/crops to 16:9 instead of stretching 4:3 refs.
- Applies the SAME logo mask to NCC and MAD.
- Pinned resampling. Removes dead grad().
"""
import sys
from PIL import Image
import numpy as np

def load_169(path):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    target = 16/9
    cur = w/h
    if abs(cur-target) < 0.02:
        return im
    if cur < target:  # too tall (4:3) -> crop vertically to 16:9
        newh = int(round(w/target))
        top = (h-newh)//2
        return im.crop((0, top, w, top+newh))
    else:  # too wide -> crop horizontally
        neww = int(round(h*target))
        left = (w-neww)//2
        return im.crop((left, 0, left+neww, h))

def ncc(a, b):
    x=a.astype(float).ravel(); y=b.astype(float).ravel()
    x-=x.mean(); y-=y.mean()
    d=np.sqrt((x*x).sum()*(y*y).sum())
    return float((x*y).sum()/d) if d else 0.0

def main(frame, ref):
    a=load_169(frame).resize((320,180), Image.Resampling.BILINEAR)
    b=load_169(ref).resize((320,180), Image.Resampling.BILINEAR)
    ga=np.asarray(a.convert('L'),dtype=float)
    gb=np.asarray(b.convert('L'),dtype=float)
    # logo region (measured bbox x76-244 y61-99 in 320x180) + margin
    mask=np.ones((180,320),bool)
    mask[55:105, 70:250]=False
    print('frame',frame,'ref',ref)
    print('sizes',Image.open(frame).size,Image.open(ref).size,'-> cropped 320x180')
    print('NCC full   = %.3f'%ncc(ga,gb))
    print('NCC masked = %.3f'%ncc(ga[mask],gb[mask]))
    print('MAD masked = %.2f / 255'%np.abs(ga[mask]-gb[mask]).mean())
    # sky band (top strip, outside logo)
    sa=ga[8:40,20:300]; sb=gb[8:40,20:300]
    print('sky mean   = %.1f / %.1f'%(sa.mean(),sb.mean()))

if __name__=='__main__':
    main(sys.argv[1],sys.argv[2])
