"""Zooms a region of a banner PNG (or any image) with a pixel grid and coordinate ticks, for reading letters pixel by
pixel when matching the banner font (tools/banner_font.py) to the real banners.

  python tools/banner_zoom.py IN.png OUT.png X0 Y0 X1 Y1 [--scale N]     (X1/Y1 exclusive; transparent shows magenta)
"""

import argparse

from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("src")
    ap.add_argument("out")
    ap.add_argument("box", type=int, nargs=4)
    ap.add_argument("--scale", type=int, default=16)
    a = ap.parse_args()
    x0, y0, x1, y1 = a.box
    s = a.scale
    im = Image.open(a.src).convert("RGBA").crop((x0, y0, x1, y1))
    bg = Image.new("RGBA", im.size, (255, 0, 255, 255))
    bg.alpha_composite(im)
    m = 20
    big = Image.new("RGB", (im.width * s + m, im.height * s + m), (255, 255, 255))
    big.paste(bg.convert("RGB").resize((im.width * s, im.height * s), Image.NEAREST), (m, m))
    d = ImageDraw.Draw(big)
    for i in range(im.width + 1):
        d.line([(m + i * s, m), (m + i * s, big.height)], fill=(128, 128, 128) if (x0 + i) % 5 else (255, 255, 255))
        if i < im.width and (x0 + i) % 2 == 0:
            d.text((m + i * s + 1, 2), str(x0 + i), fill=(0, 0, 0))
    for j in range(im.height + 1):
        d.line([(m, m + j * s), (big.width, m + j * s)], fill=(128, 128, 128) if (y0 + j) % 5 else (255, 255, 255))
        if j < im.height:
            d.text((1, m + j * s + 2), str(y0 + j), fill=(0, 0, 0))
    big.save(a.out)


if __name__ == "__main__":
    main()
