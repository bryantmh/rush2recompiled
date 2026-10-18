"""Renders text in one of Rush 2049's ROM fonts (research aid: previews of in-game lettering).

Usage:  python font49.py list                                  every font file, its texture and glyph count
        python font49.py OUT.png TEXT [--font N] [--scale S] [--color RRGGBB]
                                                               TEXT in font file N (default 33, AERO-PLAIN10, the menu font)

2049's fonts are files 22-37: font n (0-15) is atlas file 22 + n and glyph table file 38 + n, chosen by
func_800B4200 (font number in $s2; current font index 0x80149780, font header pointer 0x801497F0).
    atlas: intensity texture(s), I4, or IA8 (coverage in the low nibble) when the TXHD idx word's top byte is 1 (QUIVER12, AERO-PLAIN7);
           AERO-PLAIN16 has two pages
    glyph table header (20 bytes): bytes 2/3 = ascent/descent, byte 5 = line height, byte 12 = glyph count
    glyph records (12 bytes from offset 20): u8 char, u8 page, u8 x0, u8 y0, u8 x1, u8 y1 (inclusive atlas
    box), 6 bytes (0 in every record seen)
The menus use the AERO family: in main, func_800B4200 is called with 11 (AERO-PLAIN10) 62 times, 10
(4BAERO-PLAIN12) and 13 (AERO-PLAIN16) 28 times each; 14 (GENEVA10B) and 0 (MONACO9K) never. The unlocked-cars
list (func_80103D28, names from 0x80110D70) draws in font 11.
The boxes are top-aligned cells, so glyphs are placed with their tops on one line and advanced by box width + 1.
QUIVER*, HUD_A-P* hold digits and a few signs only; GENEVA10B, MONACO9K and the AERO-PLAIN sizes have letters.
"""
import os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roms, model, ui49
from PIL import Image

FIRST, LAST = 22, 37


def glyphs(q, k):
    d = q.file(k + 16)
    out = {}
    for o in range(20, len(d) - 11, 12):
        c, page, x0, y0, x1, y1 = d[o], d[o + 1], d[o + 2], d[o + 3], d[o + 4], d[o + 5]
        if c and x1 >= x0 and y1 >= y0:
            out[chr(c)] = (page, x0, y0, x1, y1)
    return out


def atlas(q, k):
    """(name, [page images]) for font file k."""
    m = model.M49(q.file(k))
    pages = []
    for t in m.textures:
        bpp = 8 if t['idx'] >> 24 == 1 else 4
        idx = ui49.texel_indices(m.d, m.imag + t['texels'], t['w'], t['h'], bpp)
        img = Image.new('L', (t['w'], t['h']))
        # 8-bit atlases are IA8: the low nibble is the glyph's coverage, the high one is near solid
        img.putdata([(v & 15) * 17 if bpp == 8 else v * 17 for v in idx])
        pages.append(img)
    return m.textures[0]['name'], pages


def render(q, k, text, color=(255, 255, 255)):
    _, pages = atlas(q, k)
    g = glyphs(q, k)
    space = max(2, (g['N'][3] - g['N'][1] + 1) // 2) if 'N' in g else 3
    width = sum((g[c][3] - g[c][1] + 2) if c in g else space for c in text)
    height = max(y1 - y0 + 1 for _, _, y0, _, y1 in g.values())
    out = Image.new('RGBA', (max(width, 1), height), (0, 0, 0, 0))
    x = 0
    for c in text:
        if c not in g:
            x += space
            continue
        page, x0, y0, x1, y1 = g[c]
        cell = pages[min(page, len(pages) - 1)].crop((x0, y0, x1 + 1, y1 + 1))
        ink = Image.new('RGBA', cell.size, color + (255,))
        ink.putalpha(cell)
        out.alpha_composite(ink, (x, 0))
        x += x1 - x0 + 2
    return out


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__); sys.exit(1)
    q = roms.Rush2049()
    if a[0] == 'list':
        for k in range(FIRST, LAST + 1):
            name, pages = atlas(q, k)
            print('%d  font %-2d %-18s %d page(s) %3d glyphs' % (k, k - FIRST, name, len(pages), len(glyphs(q, k))))
        return
    out, text = a[0], a[1]
    opt = lambda f, d: a[a.index(f) + 1] if f in a else d
    k, scale = int(opt('--font', 33)), int(opt('--scale', 1))
    color = tuple(bytes.fromhex(opt('--color', 'FFFFFF')))
    img = render(q, k, text, color)
    img.resize((img.width * scale, img.height * scale), Image.NEAREST).save(out)


if __name__ == '__main__':
    main()
