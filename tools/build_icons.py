"""Draws the app icon and the high-resolution font pack's thumbnail.

  python tools/build_icons.py

Writes:
  icons/512.png               App icon, full size.
  icons/app.ico               App icon for the Windows executable (16 to 256 px), used by icons/app.rc.
  assets/icon_128.rgba        128x128 RGBA pixels for the window icon (src/main.cpp sets it with SDL).
  tools/font_pack/thumb.png   Mod menu thumbnail for the font pack (tools/build_font_pack.py packs it).

Everything is drawn at 4x and downsampled. Text uses Inter (assets/InterVariable.ttf, SIL Open Font License).
"""

import os

from PIL import Image, ImageDraw, ImageFilter, ImageFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONT = os.path.join(ROOT, 'assets', 'InterVariable.ttf')
SS = 4


def font(size, weight):
    f = ImageFont.truetype(FONT, size)
    try:
        f.set_variation_by_axes([32, weight])  # Optical size, weight.
    except OSError:
        pass
    return f


def gradient(size, top, bottom):
    w, h = size
    img = Image.new('RGBA', size)
    px = img.load()
    for y in range(h):
        t = y / (h - 1)
        c = tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)) + (255,)
        for x in range(w):
            px[x, y] = c
    return img


def rounded_mask(size, radius):
    mask = Image.new('L', size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, size[0] - 1, size[1] - 1), radius, fill=255)
    return mask


def italic_text(text, fnt, fill, shear=0.22, stroke=0, stroke_fill=None):
    """Renders text on its own layer and slants it like an italic."""
    bbox = fnt.getbbox(text, stroke_width=stroke)
    w, h = bbox[2] - bbox[0], bbox[3] - bbox[1]
    pad = int(h * shear) + 8
    layer = Image.new('RGBA', (w + pad * 2, h + 16), (0, 0, 0, 0))
    ImageDraw.Draw(layer).text((pad - bbox[0], 8 - bbox[1]), text, font=fnt, fill=fill,
                               stroke_width=stroke, stroke_fill=stroke_fill)
    lw, lh = layer.size
    # x' = x + shear * (y - lh), so the bottom stays put and the top leans right.
    return layer.transform(layer.size, Image.AFFINE, (1, shear, -shear * lh, 0, 1, 0), Image.BICUBIC)


def paste_center(base, layer, cx, cy):
    base.alpha_composite(layer, (int(cx - layer.width / 2), int(cy - layer.height / 2)))


def shadow_of(layer, offset, blur, alpha):
    sh = Image.new('RGBA', layer.size, (0, 0, 0, 0))
    sh.putalpha(layer.getchannel('A').point(lambda a: a * alpha // 255))
    sh = sh.filter(ImageFilter.GaussianBlur(blur))
    out = Image.new('RGBA', (layer.width + abs(offset[0]) * 2, layer.height + abs(offset[1]) * 2), (0, 0, 0, 0))
    out.alpha_composite(sh, (abs(offset[0]) + offset[0], abs(offset[1]) + offset[1]))
    return out


def draw_road(img, horizon_y):
    """Dark road in perspective from the bottom edge to the horizon, with a dashed center line."""
    w, h = img.size
    d = ImageDraw.Draw(img)
    vx = w * 0.5
    d.polygon([(vx - w * 0.02, horizon_y), (vx + w * 0.02, horizon_y), (w * 1.05, h), (-w * 0.05, h)],
              fill=(28, 24, 38, 255))
    # Shoulder stripes.
    for side in (-1, 1):
        d.line([(vx + side * w * 0.02, horizon_y), (vx + side * w * 0.47, h)], fill=(255, 255, 255, 200),
               width=int(w * 0.012))
    # Dashes shrink toward the horizon.
    y = h
    length = h * 0.16
    while y - length > horizon_y + 4:
        t0 = (y - horizon_y) / (h - horizon_y)
        t1 = (y - length - horizon_y) / (h - horizon_y)
        hw0, hw1 = w * 0.022 * t0, w * 0.022 * t1
        d.polygon([(vx - hw0, y), (vx + hw0, y), (vx + hw1, y - length), (vx - hw1, y - length)],
                  fill=(255, 214, 64, 255))
        y -= length * 1.8
        length *= 0.55


def app_icon(size=512):
    S = size * SS
    sky = gradient((S, S), (255, 64, 46), (255, 168, 40))
    horizon = int(S * 0.6)
    # Sun on the horizon.
    d = ImageDraw.Draw(sky)
    r = S * 0.25
    d.ellipse((S / 2 - r, horizon - r, S / 2 + r, horizon + r), fill=(255, 230, 120, 255))
    draw_road(sky, horizon)

    # "2" with a dark outline and drop shadow, leaning forward, with speed lines trailing left.
    two = italic_text('2', font(int(S * 0.72), 900), (255, 255, 255, 255), shear=0.2,
                      stroke=int(S * 0.028), stroke_fill=(24, 16, 40, 255))
    lines = ImageDraw.Draw(sky)
    for yf, lf in ((0.33, 0.20), (0.44, 0.14), (0.55, 0.17)):
        y = S * yf
        x1 = S * 0.34
        lines.rounded_rectangle((x1 - S * lf, y, x1, y + S * 0.035), S * 0.017, fill=(255, 255, 255, 230))
    sh = shadow_of(two, (int(S * 0.02), int(S * 0.025)), S * 0.012, 160)
    paste_center(sky, sh, S * 0.57, S * 0.46)
    paste_center(sky, two, S * 0.57, S * 0.46)

    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    out.paste(sky, (0, 0), rounded_mask((S, S), int(S * 0.2)))
    return out.resize((size, size), Image.LANCZOS)


def font_pack_thumb(size=256):
    S = size * SS
    img = gradient((S, S), (34, 28, 52), (14, 12, 22))
    d = ImageDraw.Draw(img)

    # Left: a blocky low-res "A" drawn as chunky pixels. Right: the same letter in smooth vector type.
    pixel_a = [
        '..###..',
        '.##.##.',
        '##...##',
        '##...##',
        '#######',
        '##...##',
        '##...##',
    ]
    cell = S * 0.046
    ox, oy = S * 0.07, S * 0.34
    for row, line in enumerate(pixel_a):
        for col, ch in enumerate(line):
            if ch == '#':
                x, y = ox + col * cell, oy + row * cell
                d.rectangle((x, y, x + cell - 1, y + cell - 1), fill=(150, 146, 170, 255))

    # Arrow between them.
    ax, ay = S * 0.455, S * 0.515
    d.polygon([(ax - S * 0.03, ay - S * 0.06), (ax + S * 0.04, ay), (ax - S * 0.03, ay + S * 0.06)],
              fill=(255, 168, 40, 255))

    sharp = italic_text('A', font(int(S * 0.5), 800), (255, 255, 255, 255), shear=0.0)
    glow = shadow_of(sharp, (0, 0), S * 0.03, 200)
    glow_tinted = Image.new('RGBA', glow.size, (255, 120, 40, 0))
    glow_tinted.putalpha(glow.getchannel('A'))
    paste_center(img, glow_tinted, S * 0.72, S * 0.51)
    paste_center(img, sharp, S * 0.72, S * 0.51)

    out = Image.new('RGBA', (S, S), (0, 0, 0, 0))
    out.paste(img, (0, 0), rounded_mask((S, S), int(S * 0.12)))
    return out.resize((size, size), Image.LANCZOS)


def main():
    icons_dir = os.path.join(ROOT, 'icons')
    os.makedirs(icons_dir, exist_ok=True)

    icon = app_icon(512)
    icon.save(os.path.join(icons_dir, '512.png'))
    # Small sizes are drawn from the full-size icon; Windows picks the closest one.
    icon.save(os.path.join(icons_dir, 'app.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64),
                                                          (128, 128), (256, 256)])
    with open(os.path.join(ROOT, 'assets', 'icon_128.rgba'), 'wb') as f:
        f.write(icon.resize((128, 128), Image.LANCZOS).tobytes())

    font_pack_thumb(256).save(os.path.join(ROOT, 'tools', 'font_pack', 'thumb.png'))


if __name__ == '__main__':
    main()
