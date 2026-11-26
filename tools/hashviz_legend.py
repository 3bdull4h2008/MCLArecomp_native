"""Map HASHVIZ frame pixels back to hashes.
  hashviz_legend.py <log> <frame.bmp> x y            nearest legend colour at a pixel
  hashviz_legend.py <log> <frame.bmp> x0 y0 x1 y1    top-10 legend colours inside a bbox
"""
import re, sys
from collections import Counter
from PIL import Image

RX = re.compile(r"HASHVIZ mode=(\d) key=([0-9A-Fa-f]+) rgb=([0-9A-Fa-f]{6}) (.*)")

def load(log):
    leg = {}
    for line in open(log, "rb").read().decode("utf-8", "replace").splitlines():
        m = RX.search(line)
        if m:
            rgb = tuple(int(m.group(3)[i:i + 2], 16) for i in (0, 2, 4))
            leg.setdefault(rgb, (m.group(2), m.group(4)))
    return leg

def nearest(leg, px):
    k = min(leg, key=lambda c: sum((a - b) ** 2 for a, b in zip(c, px)))
    return k, sum((a - b) ** 2 for a, b in zip(k, px)) ** 0.5

def main():
    if len(sys.argv) not in (5, 7):
        sys.exit(__doc__)
    leg = load(sys.argv[1])
    if not leg:
        sys.exit("no HASHVIZ lines in log")
    img = Image.open(sys.argv[2]).convert("RGB")
    n = [int(v) for v in sys.argv[3:]]
    if len(n) == 2:
        px = img.getpixel((n[0], n[1]))
        k, d = nearest(leg, px)
        h, info = leg[k]
        print(f"pixel={px} nearest={k[0]:02X}{k[1]:02X}{k[2]:02X} dist={d:.1f} key={h} {info}")
    else:
        cnt = Counter()
        for y in range(n[1], n[3]):
            for x in range(n[0], n[2]):
                px = img.getpixel((x, y))
                k, d = nearest(leg, px)
                cnt[k if d < 12 else None] += 1
        tot = sum(cnt.values())
        for k, c in cnt.most_common(10):
            if k is None:
                print(f"{100 * c / tot:5.1f}%  (no legend match)")
            else:
                h, info = leg[k]
                print(f"{100 * c / tot:5.1f}%  {k[0]:02X}{k[1]:02X}{k[2]:02X} key={h} {info}")

main()
