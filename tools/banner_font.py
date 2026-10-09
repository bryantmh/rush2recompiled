"""The track banner font (tools/build_banners.py): capitals drawn from stroke skeletons in the two banner styles,
"sfrush" (heavy condensed gold letters with a bevel, a navy outline and a drop shadow, as on the SF Rush banners) and
"rush2049" (wide square letters with some corners cut off at 45 degrees, dark on top fading to a blue gradient, a lime
and a black outline, as on the Rush 2049 banners).

  python tools/banner_font.py OUT.png        renders A-Z in both styles, 4x, as a preview

Glyphs are polylines in a unit box (x 0 = left stroke's centre, 1 = right stroke's; y 0 = top stroke's centre, 1 =
bottom's), drawn 8x supersampled with square joints, cut at the corners a style lists (CORNER_CUTS) or rounded by a
morphological opening, then reduced by coverage.
"""

import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter

SS = 8

# Letter: (width factor, polylines). STYLE_GLYPHS overrides a letter for one style.
GLYPHS = {
    "A": (1.0, [[(0, 1), (0, 0), (1, 0), (1, 1)], [(0, 0.5), (1, 0.5)]]),
    "B": (1.0, [[(0, 0), (0, 1), (1, 1), (1, 0.5), (0, 0.5)], [(0, 0), (0.85, 0), (0.85, 0.5)]]),
    "C": (1.0, [[(1, 0), (0, 0), (0, 1), (1, 1)]]),
    "D": (1.0, [[(0, 0), (0.7, 0), (1, 0.3), (1, 0.7), (0.7, 1), (0, 1), (0, 0)]]),
    "E": (1.0, [[(1, 0), (0, 0), (0, 1), (1, 1)], [(0, 0.5), (0.8, 0.5)]]),
    "F": (1.0, [[(1, 0), (0, 0), (0, 1)], [(0, 0.5), (0.8, 0.5)]]),
    "G": (1.0, [[(1, 0), (0, 0), (0, 1), (1, 1), (1, 0.55), (0.5, 0.55)]]),
    "H": (1.0, [[(0, 0), (0, 1)], [(1, 0), (1, 1)], [(0, 0.5), (1, 0.5)]]),
    "I": (0.0, [[(0, 0), (0, 1)]]),
    "J": (1.0, [[(1, 0), (1, 1), (0, 1), (0, 0.65)]]),
    "K": (1.0, [[(0, 0), (0, 1)], [(1, 0), (0.1, 0.5), (1, 1)]]),
    "L": (0.9, [[(0, 0), (0, 1), (1, 1)]]),
    "M": (1.3, [[(0, 1), (0, 0), (0.5, 0.55), (1, 0), (1, 1)]]),
    "N": (1.0, [[(0, 1), (0, 0), (1, 1), (1, 0)]]),
    "O": (1.0, [[(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)]]),
    "P": (1.0, [[(0, 1), (0, 0), (1, 0), (1, 0.55), (0, 0.55)]]),
    "Q": (1.0, [[(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)], [(0.55, 0.6), (1, 1)]]),
    "R": (1.0, [[(0, 1), (0, 0), (1, 0), (1, 0.5), (0, 0.5)], [(0.45, 0.5), (1, 1)]]),
    "S": (1.0, [[(1, 0), (0, 0), (0, 0.5), (1, 0.5), (1, 1), (0, 1)]]),
    "T": (1.0, [[(0, 0), (1, 0)], [(0.5, 0), (0.5, 1)]]),
    "U": (1.0, [[(0, 0), (0, 1), (1, 1), (1, 0)]]),
    "V": (1.0, [[(0, 0), (0.5, 1), (1, 0)]]),
    "W": (1.3, [[(0, 0), (0, 1), (1, 1), (1, 0)], [(0.5, 0.4), (0.5, 1)]]),
    "X": (1.0, [[(0, 0), (1, 1)], [(1, 0), (0, 1)]]),
    "Y": (1.0, [[(0, 0), (0.5, 0.5), (1, 0)], [(0.5, 0.5), (0.5, 1)]]),
    "Z": (1.0, [[(0, 0), (1, 0), (0, 1), (1, 1)]]),
    # Digits, square like the letters (the stunt, obstacle and battle arenas' banners: STUNT 1, BATTLE 8).
    "0": (1.0, [[(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)]]),
    "1": (0.5, [[(0, 0.2), (1, 0), (1, 1)]]),
    "2": (1.0, [[(0, 0), (1, 0), (1, 0.5), (0, 0.5), (0, 1), (1, 1)]]),
    "3": (1.0, [[(0, 0), (1, 0), (1, 1), (0, 1)], [(0.35, 0.5), (1, 0.5)]]),
    "4": (1.0, [[(0, 0), (0, 0.55), (1, 0.55)], [(1, 0), (1, 1)]]),
    "5": (1.0, [[(1, 0), (0, 0), (0, 0.5), (1, 0.5), (1, 1), (0, 1)]]),
    "6": (1.0, [[(1, 0), (0, 0), (0, 1), (1, 1), (1, 0.5), (0, 0.5)]]),
    "7": (1.0, [[(0, 0), (1, 0), (1, 1)]]),
    "8": (1.0, [[(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)], [(0, 0.5), (1, 0.5)]]),
    "9": (1.0, [[(1, 0.5), (0, 0.5), (0, 0), (1, 0), (1, 1), (0, 1)]]),
}

STYLE_GLYPHS = {
    "sfrush": {
        "A": (1.1, [[(0, 1), (0.35, 0), (0.65, 0), (1, 1)], [(0.2, 0.62), (0.8, 0.62)]]),
    },
    "rush2049": {
        # The 2049 banners' M is three legs under one bar, and their D a box cut on the right.
        "M": (1.35, [[(0, 1), (0, 0), (1, 0), (1, 1)], [(0.5, 0), (0.5, 1)]]),
        "D": (1.0, [[(0, 0), (1, 0), (1, 1), (0, 1), (0, 0)]]),
        "V": (1.0, [[(0, 0), (0, 0.55), (0.5, 1), (1, 0.55), (1, 0)]]),
    },
}

# The corners of a letter's box cut off at 45 degrees, per style ("tl", "tr", "bl", "br"), as on the 2049 banners:
# METRO's E (left side), T (top left), R and M (top right), O (top left and bottom right), PRESIDIO's P (top right),
# S (top left and bottom right) and D (right side). The other letters follow the same pattern.
CORNER_CUTS = {
    "rush2049": {
        "A": "tl tr", "B": "tr br", "C": "tl bl", "D": "tr br", "E": "tl bl", "F": "tl", "G": "tl bl",
        "J": "bl", "L": "bl", "M": "tr", "N": "tr", "O": "tl br", "P": "tr", "Q": "tl br", "R": "tr",
        "S": "tl br", "T": "tl", "U": "bl br", "W": "bl br",
        "0": "tl br", "1": "tl", "2": "tr bl", "3": "tr br", "4": "tl", "5": "tl br", "6": "tl bl", "7": "tr",
        "8": "tl tr bl br", "9": "tr br",
    },
}

# height: cap height; width: a letter's width at factor 1; tv/th: vertical/horizontal stroke widths; round: every
# convex corner's radius; cut: how far a cut corner (CORNER_CUTS) is cut along each side; gap: pixels between letters'
# fills; space: a space's width (all in output pixels).
STYLES = {
    "sfrush": dict(height=17, width=13, tv=5, th=4, round=0.7, cut=0, gap=2, space=6, min_width=9),
    "rush2049": dict(height=15, width=20, tv=4, th=4, round=0, cut=5, gap=5, space=6, min_width=9),
}


def glyph_mask(ch, style, width):
    """The letter's fill as an L image (0/255) at output size, and its width."""
    s = STYLES[style]
    factor, lines = STYLE_GLYPHS[style].get(ch, GLYPHS[ch])
    tv, th, h = s["tv"], s["th"], s["height"]
    w = max(tv, round(width * factor)) if factor > 0 else tv
    pad = 2
    big = Image.new("L", ((w + 2 * pad) * SS, (h + 2 * pad) * SS), 0)
    d = ImageDraw.Draw(big)

    def at(p):
        return ((pad + tv / 2 + p[0] * (w - tv)) * SS, (pad + th / 2 + p[1] * (h - th)) * SS)

    hv, hh = tv * SS / 2, th * SS / 2
    for line in lines:
        pts = [at(p) for p in line]
        for (x0, y0), (x1, y1) in zip(pts, pts[1:]):
            if abs(y0 - y1) < 1e-6:
                d.rectangle([min(x0, x1) - hv, y0 - hh, max(x0, x1) + hv - 1, y0 + hh - 1], fill=255)
            elif abs(x0 - x1) < 1e-6:
                d.rectangle([x0 - hv, min(y0, y1) - hh, x0 + hv - 1, max(y0, y1) + hh - 1], fill=255)
            else:
                dx, dy = x1 - x0, y1 - y0
                n = (dx * dx + dy * dy) ** 0.5
                nx, ny = -dy / n * hv, dx / n * hv
                d.polygon([(x0 + nx, y0 + ny), (x1 + nx, y1 + ny), (x1 - nx, y1 - ny), (x0 - nx, y0 - ny)], fill=255)
        for x, y in pts:
            d.rectangle([x - hv, y - hh, x + hv - 1, y + hh - 1], fill=255)
    # Keep the drawing inside the letter's box with its listed corners cut, then round every convex corner by round
    # (opening = erode, then dilate; it must stay under half a stroke).
    x0, y0, x1, y1 = pad * SS, pad * SS, (pad + w) * SS, (pad + h) * SS
    c = s["cut"] * SS
    cuts = CORNER_CUTS.get(style, {}).get(ch, "").split()
    outline = []
    for corner, (cx, cy), (ax, ay), (bx, by) in (("tl", (x0, y0), (x0, y0 + c), (x0 + c, y0)),
                                                 ("tr", (x1, y0), (x1 - c, y0), (x1, y0 + c)),
                                                 ("br", (x1, y1), (x1, y1 - c), (x1 - c, y1)),
                                                 ("bl", (x0, y1), (x0 + c, y1), (x0, y1 - c))):
        outline += [(ax, ay), (bx, by)] if corner in cuts and c > 0 else [(cx, cy)]
    box = Image.new("L", big.size, 0)
    ImageDraw.Draw(box).polygon(outline, fill=255)
    big = ImageChops.multiply(big, box)
    r = int(s["round"] * SS)
    if r > 0:
        k = 2 * r + 1
        big = big.filter(ImageFilter.MinFilter(k)).filter(ImageFilter.MaxFilter(k))
    small = big.crop((pad * SS, pad * SS, (pad + w) * SS, (pad + h) * SS)).resize((w, h), Image.BOX)
    return small.point(lambda v: 255 if v >= 128 else 0), w


def layout(text, style, max_width):
    """Each letter's mask and x, fitted into max_width by narrowing the letters."""
    s = STYLES[style]
    width = s["width"]
    while True:
        x, out = 0, []
        for ch in text:
            if ch == " ":
                x += s["space"]
                continue
            m, w = glyph_mask(ch, style, width)
            out.append((m, x))
            x += w + s["gap"]
        total = x - s["gap"]
        if total <= max_width or width <= s["min_width"]:
            return out, total
        width -= 1


def dilate(img, n):
    return img.filter(ImageFilter.MaxFilter(2 * n + 1)) if n else img


def render(text, style, size=(128, 32)):
    """text in the style, centred in an RGBA image of size."""
    W, H = size
    s = STYLES[style]
    margin = 3
    glyphs, total = layout(text.upper(), style, W - 2 * margin)
    fill = Image.new("L", size, 0)
    x0 = (W - total) // 2
    top = (H - s["height"]) // 2 - (1 if style == "sfrush" else 0)
    for m, x in glyphs:
        fill.paste(255, (x0 + x, top), m)
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    px = img.load()
    f = fill.load()
    h = s["height"]

    if style == "sfrush":
        outline = dilate(fill, 1)
        shadow = ImageChops.offset(outline, 1, 1)
        shadow = ImageChops.lighter(shadow, ImageChops.offset(outline, 2, 2))
        o, sh = outline.load(), shadow.load()
        for y in range(H):
            for x in range(W):
                if f[x, y]:
                    t = (y - top) / max(1, h - 1)
                    # Gold, light on top, with a bright band a third of the way down as on the banners.
                    if t < 0.12:
                        c = (240, 226, 150)
                    elif t < 0.45:
                        c = (214, 182, 70)
                    elif t < 0.6:
                        c = (236, 218, 140)
                    else:
                        c = (192, 156, 48)
                    up = y == 0 or not f[x, y - 1]
                    left = x == 0 or not f[x - 1, y]
                    down = y == H - 1 or not f[x, y + 1]
                    right = x == W - 1 or not f[x + 1, y]
                    if up or left:
                        c = (248, 236, 170)
                    elif down or right:
                        c = (140, 104, 24)
                    px[x, y] = c + (255,)
                elif o[x, y]:
                    px[x, y] = (0, 2, 48, 255)
                elif sh[x, y]:
                    px[x, y] = (26, 6, 0, 255)
    else:
        lime = dilate(fill, 1)
        black = dilate(fill, 2)
        li, bl = lime.load(), black.load()
        for y in range(H):
            for x in range(W):
                if f[x, y]:
                    t = (y - top) / max(1, h - 1)
                    if t < 0.42:
                        c = (0, 22 + int(t * 30), 8)
                    else:
                        u = (t - 0.42) / 0.58
                        c = (int(2 + 20 * u), int(24 + 50 * u), int(80 + 150 * u))
                    if y > 0 and not f[x, y - 1] and t > 0.3:
                        c = (150, 230, 140)          # the lime edge's inner highlight under a bar
                    px[x, y] = c + (255,)
                elif li[x, y]:
                    px[x, y] = (186, 228, 92, 255)
                elif bl[x, y]:
                    px[x, y] = (0, 10, 4, 255)
    return img


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "banner_font.png"
    rows = ["ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ", "THE ROCK", "MARINA", "HAIGHT", "CIVIC", "MISSION"]
    sheet = Image.new("RGBA", (128 * 2, 32 * len(rows)), (90, 110, 140, 255))
    for i, row in enumerate(rows):
        for j, style in enumerate(("sfrush", "rush2049")):
            sheet.alpha_composite(render(row, style), (128 * j, 32 * i))
    sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(out)
