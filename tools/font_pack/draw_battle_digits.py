"""Draws the high-resolution digits of the battle HUD's numbers (tools/font_pack/battle_svg): the ammo on the weapon and
the kills on the coin, which src/hud.cpp (rush2::hud::draw_number) draws from its own 8 x 10 digit images.

  python tools/font_pack/draw_battle_digits.py

Each digit is the same heavy rounded figure as its 8 x 10 image, drawn as strokes of a round pen. The SVGs are named
by the RT64 hash of the image they replace, which is worked out here from the digit rows in src/hud.cpp and the texture
load draw_number emits, so they follow any change to those images. tools/build_font_pack.py pack puts them in the pack.
"""

import os
import re
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.dirname(HERE))
import build_font_pack as pack  # noqa: E402

OUT_DIR = os.path.join(HERE, 'battle_svg')
WIDTH, HEIGHT = 8, 10
PEN = 2.2                         # Stroke width in texels: the images' strokes are 2 to 3 texels.
L, R, T, M, B = 1.2, 6.8, 1.15, 5.0, 8.85   # Stroke center lines: left, right, top, middle, bottom.
ROUND = 1.6                       # Corner radius of the bowls.

# Per digit: strokes of (x, y, corner radius) points; a stroke that ends where it starts is a loop.
DIGITS = {
    '0': [[(L, M, 0), (L, T, ROUND), (R, T, ROUND), (R, B, ROUND), (L, B, ROUND), (L, M, 0)]],
    '1': [[(2.3, 3.1, 0), (4.3, T, 0.2), (4.3, B, 0)], [(2.2, B, 0), (6.4, B, 0)]],
    '2': [[(L, 3.0, 0), (L, T, ROUND), (R, T, ROUND), (R, 4.3, 1.2), (L, B, 0.2), (R, B, 0)]],
    '3': [[(L, 2.8, 0), (L, T, ROUND), (R, T, ROUND), (R, M, 1.3), (3.6, M, 0)],
          [(3.6, M, 0), (R, M, 1.3), (R, B, ROUND), (L, B, ROUND), (L, 7.2, 0)]],
    '4': [[(5.6, T, 0), (L, 6.3, 0.2), (R, 6.3, 0)], [(5.6, T, 0), (5.6, B, 0)]],
    '5': [[(R, T, 0), (L, T, 0.2), (L, 4.2, 0.2), (R, 4.2, ROUND), (R, B, ROUND), (L, B, ROUND), (L, 7.3, 0)]],
    '6': [[(R, 2.8, 0), (R, T, ROUND), (L, T, ROUND), (L, B, ROUND), (R, B, ROUND), (R, 4.6, ROUND), (L, 4.6, 0)]],
    '7': [[(L, T, 0), (R, T, 0.3), (3.3, B, 0)]],
    '8': [[(L, 3.1, 0), (L, T, 1.4), (R, T, 1.4), (R, M, 1.4), (L, M, 1.4), (L, 3.1, 0)],
          [(L, 6.9, 0), (L, M, 1.4), (R, M, 1.4), (R, B, 1.4), (L, B, 1.4), (L, 6.9, 0)]],
    '9': [[(L, 7.2, 0), (L, B, ROUND), (R, B, ROUND), (R, T, ROUND), (L, T, ROUND), (L, 5.4, ROUND), (R, 5.4, 0)]],
}


def digit_rows():
    """The ten 8 x 10 digit images of src/hud.cpp, as bytes (0xFF ink, 0 clear)."""
    source = open(os.path.join(ROOT, 'src', 'hud.cpp')).read()
    table = source[source.index('digit_rows[10][digit_h]'):]
    rows = re.findall(r'"([.#]{8})"', table[:table.index('};')])
    assert len(rows) == 10 * HEIGHT, len(rows)
    return [bytes(0xFF if c == '#' else 0 for row in rows[d * HEIGHT:(d + 1) * HEIGHT] for c in row) for d in range(10)]


def image_hash(image):
    """RT64's hash of a digit image as draw_number loads it: an 8-bit intensity tile, clamped, by LoadBlock."""
    fmt_i, siz_8b, siz_16b, clamp = 4, 1, 2, 2
    tile_clamp = (clamp << 18) | (clamp << 8)
    cmds = [(0xFD000000 | (fmt_i << 21) | (siz_16b << 19), 1),
            (0xF5000000 | (fmt_i << 21) | (siz_16b << 19), (7 << 24) | tile_clamp),
            (0xF3000000, (7 << 24) | (((WIDTH * HEIGHT + 1) // 2 - 1) << 12) | 2048),
            (0xF5000000 | (fmt_i << 21) | (siz_8b << 19) | (1 << 9), tile_clamp),
            (0xF2000000, (((WIDTH - 1) << 2) << 12) | ((HEIGHT - 1) << 2))]
    tmem, tiles, state = bytearray(4096), [pack.DumpTile() for _ in range(8)], {}
    pack.replay(cmds, {'1': image}, tmem, tiles, state)
    return pack.tmem_hash(tmem, tiles[0], WIDTH, HEIGHT, 0)


def rounded(points):
    """The stroke's center line with its corners rounded: each corner is cut by its radius and bridged by a curve."""
    out = [np.array(points[0][:2], float)]
    for before, corner, after in zip(points, points[1:], points[2:]):
        a, c, b = np.array(before[:2], float), np.array(corner[:2], float), np.array(after[:2], float)
        radius = min(corner[2], np.hypot(*(a - c)) / 2, np.hypot(*(b - c)) / 2)
        if radius <= 0:
            out.append(c)
            continue
        start = c + (a - c) / np.hypot(*(a - c)) * radius
        end = c + (b - c) / np.hypot(*(b - c)) * radius
        # A quadratic curve weighted toward a circular arc.
        for t in np.linspace(0, 1, 9):
            w = 0.7071
            den = (1 - t) ** 2 + 2 * w * t * (1 - t) + t ** 2
            out.append(((1 - t) ** 2 * start + 2 * w * t * (1 - t) * c + t ** 2 * end) / den)
    out.append(np.array(points[-1][:2], float))
    return out


def capsule(a, b, steps=10):
    """The outline of the pen swept from a to b."""
    direction = b - a
    length = np.hypot(*direction)
    angle = np.arctan2(direction[1], direction[0]) if length > 1e-9 else 0.0
    half = PEN / 2
    points = []
    for center, base in ((b, angle - np.pi / 2), (a, angle + np.pi / 2)):
        for k in range(steps + 1):
            t = base + np.pi * k / steps
            points.append(center + half * np.array([np.cos(t), np.sin(t)]))
    return points


def svg(hash_name, char):
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {WIDTH} {HEIGHT}" width="{WIDTH * 8}" '
             f'height="{HEIGHT * 8}">',
             f'  <title>Battle HUD digit {char} ({hash_name})</title>']
    for stroke in DIGITS[char]:
        line = rounded(stroke)
        for a, b in zip(line, line[1:]):
            d = 'M' + ' L'.join(f'{x:.3f},{y:.3f}' for x, y in capsule(a, b)) + ' Z'
            lines.append(f'  <path fill="rgb(255,255,255)" fill-rule="nonzero" d="{d}"/>')
    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    os.makedirs(OUT_DIR, exist_ok=True)
    for old in os.listdir(OUT_DIR):
        if old.endswith('.svg'):
            os.remove(os.path.join(OUT_DIR, old))
    for d, image in enumerate(digit_rows()):
        name = image_hash(image)
        open(os.path.join(OUT_DIR, name + '.svg'), 'w', newline='\n').write(svg(name, str(d)))
        print(d, name)
