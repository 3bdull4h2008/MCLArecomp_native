"""Generate the original MCLA-PC release icon set (PNG + ICO).

Original artwork: midnight-navy rounded tile, bold white "M", gold accent
bar. No Rockstar / Take-Two logos, trademarks, or game assets are used.
Single-letter "M" motif only. All sizes drawn natively (not downscaled
from one bitmap) so the 16 px frame stays crisp.

Usage (from repo root):
    python src/icon/make_icons.py

Outputs: src/icon/mcla-release-{16,32,48,64,128,256}.png (license tEXt
embedded) and src/icon/mcla-release.ico (16/32/48/64/128/256, 32bpp).
"""
import os

from PIL import Image, ImageDraw, ImageFont
from PIL.PngImagePlugin import PngInfo

HERE = os.path.dirname(os.path.abspath(__file__))
FONT_PATH = r"C:\Windows\Fonts\arialbd.ttf"
SIZES = [16, 32, 48, 64, 128, 256]

LICENSE_TEXT = (
    "Original artwork (c) 2026 MCLA-PC project contributors. "
    "Midnight-navy tile with a bold white M and a gold accent bar. "
    "No Rockstar Games / Take-Two Interactive logos, trademarks, or "
    "game assets are reproduced. Single-letter motif only. "
    "Not affiliated with or endorsed by Rockstar Games. "
    "Licensed for shipping with the MCLA-PC emulator release."
)

# Eye-dropper constants (midnight palette; navy top matches the boot clear
# colour 16,26,56 measured in docs/HANDOFF_3D_ASSETS.md:4).
NAVY_TOP = (16, 26, 56)
NAVY_BOT = (4, 6, 18)
GOLD = (255, 201, 60)
WHITE = (245, 247, 255)


def gradient_tile(size: int) -> Image.Image:
    tile = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    grad = Image.new("RGBA", (1, size))
    for y in range(size):
        t = y / max(size - 1, 1)
        grad.putpixel(
            (0, y),
            tuple(int(NAVY_TOP[i] + (NAVY_BOT[i] - NAVY_TOP[i]) * t) for i in range(3))
            + (255,),
        )
    grad = grad.resize((size, size))
    mask = Image.new("L", (size, size), 0)
    mask_draw = ImageDraw.Draw(mask)
    radius = int(size * 0.20)
    mask_draw.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=255)
    tile.paste(grad, (0, 0), mask)
    return tile


def draw_size(size: int) -> Image.Image:
    img = gradient_tile(size)
    d = ImageDraw.Draw(img)
    # Center a bold "M" occupying ~62% of the tile width.
    font_size = int(size * 0.78)
    font = ImageFont.truetype(FONT_PATH, font_size)
    bbox = d.textbbox((0, 0), "M", font=font)
    tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
    x = (size - tw) / 2 - bbox[0]
    # Sit the M a touch high to leave room for the accent bar.
    y = size * 0.44 - th / 2 - bbox[1]
    d.text((x, y), "M", font=font, fill=WHITE + (255,))
    # Gold accent bar below the M.
    bar_w = int(size * 0.52)
    bar_h = max(1, int(size * 0.055))
    bar_x = (size - bar_w) // 2
    bar_y = int(size * 0.76)
    d.rounded_rectangle(
        [bar_x, bar_y, bar_x + bar_w - 1, bar_y + bar_h - 1],
        radius=max(1, bar_h // 2),
        fill=GOLD + (255,),
    )
    return img


def main() -> None:
    png_paths = []
    for size in SIZES:
        img = draw_size(size)
        info = PngInfo()
        info.add_text("License", LICENSE_TEXT)
        png_path = os.path.join(HERE, f"mcla-release-{size}.png")
        img.save(png_path, pnginfo=info)
        png_paths.append(png_path)
        print(f"wrote {png_path} {img.size}")
    # ICO: Pillow embeds the largest bitmaps; pass every size explicitly.
    largest = Image.open(png_paths[-1])
    ico_path = os.path.join(HERE, "mcla-release.ico")
    largest.save(
        ico_path,
        sizes=[(s, s) for s in SIZES],
        append_images=[Image.open(p) for p in png_paths[:-1]],
    )
    print(f"wrote {ico_path}")


if __name__ == "__main__":
    main()
