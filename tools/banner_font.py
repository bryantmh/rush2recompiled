"""The track banner font (tools/build_banners.py): the letters of the real banners in tools/rush1/banners and
tools/rush2049/banners, redrawn so a track without a banner image gets its name in its game's lettering.

"sfrush" (SF Rush: DOWNTOWN, THE HEIGHTS, EMBARCADERO): 18-row bitmaps (GLYPHS) on DOWNTOWN's grid, 3-row bars, 4 px
stems, round corners cut at 45 degrees, letters touching as on the real logos (a long name loses flat middle columns
until it fits). The word is laid down as one flat shape and lit as a whole the way SUNSET is: one light over
the middle of the banner near the top, so the gold is pale cream under it and deep gold towards the ends, faces turned
up and the word's bottom edge are bright, right faces and the undersides of bars are dark brown, and every bar casts a
shadow two rows down onto the plate behind the word, which a black and a navy line outline.

"rush2049" (Rush 2049: METRO, PRESIDIO): wide blocks with 1 px lime lines. A black top bar, a lime line across the
letter, then the lower part: navy at the top shading to bright blue, dark green under every inner lime line. Counters
are holes or notches in the block with the lime line drawn round them, some letters are just the block with a lime
slit (PRESIDIO's E and S), and listed corners are cut at 45 degrees. The lime runs from yellow-green at the top to mint
at the bottom. Letters are templates (BLOCKS): each template character stands for a band of rows and a column of a
fixed or flexible width, so one template draws METRO's 16 px letters and PRESIDIO's 13 px ones.

  python tools/banner_font.py OUT.png [--compare]     renders the alphabet in both styles, 4x, as a preview; with
                                                      --compare, every real banner next to its name in the font
"""

import math
import os
import sys

from PIL import Image

SS = 8
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# ---------------------------------------------------------------------------------------------------------- SF Rush

# Letters as 18-row bitmaps drawn on DOWNTOWN's grid: 3-row bars, 4 px stems, round corners cut 3 px at 45 degrees and
# counters' corners 1-2 px.
GLYPHS = {
    "A": [
        "...########...",
        "...########...",
        "...########...",
        "..####..####..",
        "..####..####..",
        "..####..####..",
        "..####..####..",
        "..####..####..",
        "..####..####..",
        ".############.",
        ".############.",
        ".############.",
        ".####....####.",
        ".####....####.",
        ".####....####.",
        "####......####",
        "####......####",
        "####......####",
    ],
    "B": [
        "###########...",
        "############..",
        "#############.",
        "####.....#####",
        "####......####",
        "####......####",
        "####.....####.",
        "#############.",
        "#############.",
        "#############.",
        "####.....####.",
        "####......####",
        "####......####",
        "####......####",
        "####.....#####",
        "#############.",
        "############..",
        "###########...",
    ],
    # EMBARCADERO's C: square with rounded corners and short terminals.
    "C": [
        "...###########.",
        ".##############",
        ".##############",
        "#####......####",
        "####.......####",
        "####...........",
        "####...........",
        "####...........",
        "####...........",
        "####...........",
        "####...........",
        "####...........",
        "####...........",
        "####.......####",
        "#####......####",
        ".##############",
        ".##############",
        "...###########.",
    ],
    "D": [
        "###########....",
        "############...",
        "#############..",
        "######...#####.",
        "#####.....#####",
        "####.......####",
        "####.......####",
        "####.......####",
        "####.......####",
        "####.......####",
        "####.......####",
        "####.......####",
        "####.......####",
        "#####.....#####",
        "######...#####.",
        "#############..",
        "############...",
        "###########....",
    ],
    "E": [
        "#############",
        "#############",
        "#############",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
        "###########..",
        "###########..",
        "###########..",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
        "#############",
        "#############",
        "#############",
    ],
    "F": [
        "#############",
        "#############",
        "#############",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
        "###########..",
        "###########..",
        "###########..",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
        "####.........",
    ],
    "G": [
        ".....#####.....",
        "....#######....",
        "...#########...",
        "..#####....####",
        ".#####.....####",
        "#####......####",
        "####...........",
        "####...........",
        "####...########",
        "####...########",
        "####...########",
        "####.......####",
        "#####.....#####",
        ".#####...######",
        "..#####.#######",
        "...#########...",
        "....#######....",
        ".....#####.....",
    ],
    "H": [
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "##############",
        "##############",
        "##############",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
    ],
    "I": [
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
        "####",
    ],
    "J": [
        "........####",
        "........####",
        "........####",
        "........####",
        "........####",
        "........####",
        "........####",
        "........####",
        "........####",
        "........####",
        "........####",
        "####....####",
        "####....####",
        "####...#####",
        "####..######",
        ".##########.",
        "..########..",
        "...######...",
    ],
    "K": [
        "####.....#####",
        "####....#####.",
        "####....#####.",
        "####...#####..",
        "####..#####...",
        "####..#####...",
        "####.#####....",
        "####.#####....",
        "#########.....",
        "####.#####....",
        "####.#####....",
        "####..#####...",
        "####..#####...",
        "####...#####..",
        "####...#####..",
        "####....#####.",
        "####....#####.",
        "####.....#####",
    ],
    "L": [
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "####........",
        "############",
        "############",
        "############",
    ],
    "M": [
        "####.........#####",
        "#####.......######",
        "#####.......######",
        "######.....#######",
        "#######....#######",
        "########..########",
        "########.####.####",
        "####.########.####",
        "####..######..####",
        "####..######..####",
        "####...####...####",
        "####...####...####",
        "####..........####",
        "####..........####",
        "####..........####",
        "####..........####",
        "####..........####",
        "####..........####",
    ],
    "N": [
        "#####.....####",
        "#####.....####",
        "######....####",
        "######....####",
        "#######...####",
        "#######...####",
        "########..####",
        "########..####",
        "#########.####",
        "#########.####",
        "####.#########",
        "####.#########",
        "####..########",
        "####..########",
        "####...#######",
        "####...#######",
        "####....######",
        "####....######",
    ],
    "O": [
        ".....######.....",
        "....########....",
        "...##########...",
        "..#####..#####..",
        ".#####....#####.",
        "#####......#####",
        "####........####",
        "####........####",
        "####........####",
        "####........####",
        "####........####",
        "####........####",
        "#####......#####",
        ".#####....#####.",
        "..#####..#####..",
        "...##########...",
        "....########....",
        ".....######.....",
    ],
    "P": [
        "###########...",
        "############..",
        "#############.",
        "####.....#####",
        "####......####",
        "####......####",
        "####......####",
        "####.....#####",
        "##############",
        "#############.",
        "############..",
        "####..........",
        "####..........",
        "####..........",
        "####..........",
        "####..........",
        "####..........",
        "####..........",
    ],
    "Q": [
        ".....######.....",
        "....########....",
        "...##########...",
        "..#####..#####..",
        ".#####....#####.",
        "#####......#####",
        "####........####",
        "####........####",
        "####........####",
        "####........####",
        "####........####",
        "####....###.####",
        "#####...########",
        ".#####....######",
        "..#####..#######",
        "...#############",
        "....############",
        ".....###########",
    ],
    # MARKET's R: a closed bowl with a small counter, a waist, and a wide leg leaning slightly right.
    "R": [
        "############...",
        "#############..",
        "##############.",
        "####.....#####.",
        "####.....#####.",
        "####.....#####.",
        "####.....#####.",
        "##############.",
        "#############..",
        "############...",
        "####...#####...",
        "####...#####...",
        "####....#####..",
        "####....#####..",
        "####....#####..",
        "####.....#####.",
        "####.....#####.",
        "####.....######",
    ],
    "S": [
        "...########...",
        "..##########..",
        ".############.",
        "####......####",
        "####......####",
        "####..........",
        "####..........",
        "##############",
        "##############",
        "##############",
        "..........####",
        "..........####",
        "..........####",
        "####......####",
        "####......####",
        ".############.",
        "..##########..",
        "...########...",
    ],
    "T": [
        "##############",
        "##############",
        "##############",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
    ],
    "U": [
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "#####....#####",
        "######..######",
        ".############.",
        "..##########..",
        "...########...",
    ],
    "V": [
        "####......####",
        "####......####",
        "####......####",
        ".####....####.",
        ".####....####.",
        ".####....####.",
        ".####....####.",
        "..####..####..",
        "..####..####..",
        "..####..####..",
        "..####..####..",
        "...########...",
        "...########...",
        "...########...",
        "...########...",
        "....######....",
        "....######....",
        "....######....",
    ],
    "W": [
        "####..............####",
        "####..............####",
        "####..............####",
        ".####............####.",
        ".####....####....####.",
        ".####....####....####.",
        ".####....####....####.",
        ".####...######...####.",
        ".####...######...####.",
        "..####..######..####..",
        "..####..######..####..",
        "..####.########.####..",
        "..####.########.####..",
        "..####.########.####..",
        "..####.########.####..",
        "...#######..#######...",
        "...#######..#######...",
        "...#######..#######...",
    ],
    "X": [
        "####......####",
        ".####....####.",
        ".####....####.",
        "..####..####..",
        "..####..####..",
        "...########...",
        "....######....",
        "....######....",
        ".....####.....",
        ".....####.....",
        "....######....",
        "....######....",
        "...########...",
        "..####..####..",
        "..####..####..",
        ".####....####.",
        ".####....####.",
        "####......####",
    ],
    "Y": [
        "####......####",
        ".####....####.",
        ".####....####.",
        "..####..####..",
        "..####..####..",
        "...########...",
        "....######....",
        "....######....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
        ".....####.....",
    ],
    "Z": [
        "##############",
        "##############",
        "##############",
        ".........#####",
        "........#####.",
        ".......#####..",
        ".......#####..",
        "......#####...",
        ".....#####....",
        "....#####.....",
        "...#####......",
        "..#####.......",
        "..#####.......",
        ".#####........",
        "#####.........",
        "##############",
        "##############",
        "##############",
    ],
    "0": [
        "...########...",
        "..##########..",
        ".############.",
        "######..######",
        "#####....#####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "#####....#####",
        "######..######",
        ".############.",
        "..##########..",
        "...########...",
    ],
    "1": [
        "..######",
        ".#######",
        "########",
        "########",
        "########",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
        "....####",
    ],
    "2": [
        "...########...",
        "..##########..",
        ".############.",
        "####......####",
        "####......####",
        "..........####",
        "..........####",
        ".#############",
        "##############",
        "#############.",
        "####..........",
        "####..........",
        "####..........",
        "####......####",
        "####......####",
        "##############",
        "##############",
        "##############",
    ],
    "3": [
        "...########...",
        "..##########..",
        ".############.",
        "####......####",
        "####......####",
        "..........####",
        "..........####",
        "#############.",
        "##############",
        "#############.",
        "..........####",
        "..........####",
        "..........####",
        "####......####",
        "####......####",
        ".############.",
        "..##########..",
        "...########...",
    ],
    "4": [
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "####......####",
        "##############",
        "##############",
        "##############",
        "..........####",
        "..........####",
        "..........####",
        "..........####",
        "..........####",
        "..........####",
        "..........####",
    ],
    "5": [
        "##############",
        "##############",
        "##############",
        "####..........",
        "####..........",
        "####..........",
        "####..........",
        "####......####",
        "####......####",
        ".###......####",
        "..........####",
        "..........####",
        "..........####",
        "####......####",
        "####......####",
        ".############.",
        "..##########..",
        "...########...",
    ],
    "6": [
        "...########...",
        "..##########..",
        ".############.",
        "####......####",
        "####......####",
        "####..........",
        "####..........",
        "####......###.",
        "####......####",
        "####......####",
        "######..######",
        "#####....#####",
        "####......####",
        "#####....#####",
        "######..######",
        ".############.",
        "..##########..",
        "...########...",
    ],
    "7": [
        "#############",
        "#############",
        "#############",
        ".........####",
        ".........####",
        "........####.",
        "........####.",
        "........####.",
        "........####.",
        ".......####..",
        ".......####..",
        ".......####..",
        "......####...",
        "......####...",
        "......####...",
        "......####...",
        ".....####....",
        ".....####....",
    ],
    "8": [
        "...########...",
        "..##########..",
        ".############.",
        "#####....#####",
        "####......####",
        "####......####",
        "#####....#####",
        ".###......###.",
        "####......####",
        ".###......###.",
        "#####....#####",
        "####......####",
        "####......####",
        "####......####",
        "#####....#####",
        ".############.",
        "..##########..",
        "...########...",
    ],
    "9": [
        "...########...",
        "..##########..",
        ".############.",
        "######..######",
        "#####....#####",
        "#####....#####",
        "######..######",
        "####......####",
        "####......####",
        ".###......####",
        "..........####",
        "..........####",
        "..........####",
        "####......####",
        "####......####",
        ".############.",
        "..##########..",
        "...########...",
    ],
}

# SUNSET's lighting: one light over the middle of the banner near the top. The gold is pale cream under it (x near the
# centre) and deep gold towards the ends; faces turned up catch it, the word's bottom edge is lit, and right faces and
# the undersides of bars are in shadow. Every bar casts a dark shadow two rows down onto the plate behind the word,
# which a navy line outlines.
SF = dict(height=18, gap=0, space=6)
SF_KERN = {"HE": -2}   # pairs pulled together: THE HEIGHTS' E overlaps the H's right stem
SF_PALE, SF_GOLD = (222, 226, 160), (174, 140, 40)   # under the light / far from it (SUNSET's N and its S and T)
SF_SPREAD = 12          # how far the pale spreads from the centre, in pixels (the falloff's standard deviation)
SF_TOP = (252, 244, 196)        # a face turned up, and the left faces
SF_BOTTOM = (218, 204, 142)     # the word's bottom edge
SF_UNDER = (104, 66, 18)        # the underside of a bar, over a counter
SF_RIGHT = (122, 86, 28)        # a face turned right
SF_SHADOW, SF_BLACK, SF_NAVY = (14, 6, 2), (8, 2, 2), (0, 0, 60)


def _mix(a, b, t):
    return tuple(round(p + (q - p) * t) for p, q in zip(a, b))


def _sf_shade(word, x, y, centre):
    """The gold at (x, y) of the word (rows of booleans, the letters' 18 rows) under SUNSET's light."""
    h, w = len(word), len(word[0])

    def g(i, j):
        return 0 <= i < w and 0 <= j < h and word[j][i]
    base = _mix(SF_GOLD, SF_PALE, math.exp(-((x - centre) / SF_SPREAD) ** 2))
    if not g(x, y - 1):
        return _mix(base, SF_TOP, 0.85)
    if not g(x, y + 1):
        return _mix(base, SF_BOTTOM, 0.8) if y >= h - 1 else _mix(base, SF_UNDER, 0.85)
    if not g(x - 1, y):
        return _mix(base, SF_TOP, 0.75)
    if not g(x + 1, y):
        return _mix(base, SF_RIGHT, 0.6)
    if not g(x, y - 2):
        return _mix(base, SF_UNDER, 0.12)      # the row under a lit top face
    if y >= h - 2 or not g(x, y + 2):
        return _mix(base, SF_UNDER, 0.18 if y < h - 2 else 0.1)
    return base


def _narrow(m):
    """The bitmap one column narrower: drops the middle-most column equal to its neighbour."""
    w = len(m[0])
    cols = [tuple(r[x] for r in m) for x in range(w)]
    best = [x for x in range(1, w - 1) if cols[x] == cols[x - 1] or cols[x] == cols[x + 1]]
    if not best:
        return m
    x = min(best, key=lambda i: abs(i - (w - 1) / 2))
    return [r[:x] + r[x + 1:] for r in m]


def render_sfrush(text, size):
    W, H = size
    s = SF
    glyphs = [[[c == "#" for c in row] for row in GLYPHS[ch]] if ch != " " else None for ch in text]
    kern = [SF_KERN.get(text[i - 1:i + 1], 0) if i else 0 for i in range(len(text))]

    def total():
        return sum(s["space"] if g is None else len(g[0]) + s["gap"] for g in glyphs) - s["gap"] + sum(kern)
    while total() > W - 4:
        for i in sorted((i for i, g in enumerate(glyphs) if g), key=lambda i: -len(glyphs[i][0])):
            n = _narrow(glyphs[i])
            if n is not glyphs[i]:
                glyphs[i] = n
                break
        else:
            break
    h = s["height"]
    x0 = (W - total()) // 2
    top = (H - h) // 2 - 1
    img = Image.new("RGBA", size, (0, 0, 0, 0))
    px = img.load()
    gold = [[False] * W for _ in range(H)]
    x = x0
    for g, k in zip(glyphs, kern):
        if g is None:
            x += s["space"]
            continue
        x += k
        for y in range(h):
            for gx in range(len(g[0])):
                if g[y][gx] and 0 <= x + gx < W:
                    gold[top + y][x + gx] = True
        x += len(g[0]) + s["gap"]
    # The word is one flat shape first and is lit as a whole, as on the real logos: touching letters merge and only
    # the word's own faces catch the light or fall in shadow.
    word = gold[top:top + h]
    for y in range(h):
        for x in range(W):
            if word[y][x]:
                px[x, top + y] = _sf_shade(word, x, y, W / 2) + (255,)

    def near(x, y, r):
        return any(0 <= x + i < W and 0 <= y + j < H and gold[y + j][x + i]
                   for i in range(-r, r + 1) for j in range(-r, r + 1))
    for y in range(H):
        for x in range(W):
            if not gold[y][x]:
                if (y >= 1 and gold[y - 1][x]) or (y >= 2 and gold[y - 2][x]):
                    px[x, y] = SF_SHADOW + (255,)
                elif near(x, y, 1):
                    px[x, y] = SF_BLACK + (255,)
                elif near(x, y, 2) or (y >= 3 and gold[y - 3][x]):
                    px[x, y] = SF_NAVY + (255,)
    return img


# ------------------------------------------------------------------------------------------------------- Rush 2049

# Template rows, top to bottom: o outline, T top bar, s the lime line under it, a/b the upper half of the lower part
# (q a 1 px line between), p a 1 px line, c/d the lower half (r between), o outline. Template columns: o outline, p a
# 1 px line, S a stem, g a gap, digits flexible widths by weight. Characters: '#' the letter, ' ' outside it (a hole
# or notch; the lime line is drawn round it on the letter's side), 'L' a lime line.
ROWS = "oTsaqbpcrdo"

F7 = "#######"


def _b(cols, rows, cuts="", width=1.0):
    assert len(rows) == len(ROWS), cols
    for r in rows:
        assert len(r) == len(cols), (cols, r)
    return dict(cols=cols, rows=rows, cuts=cuts, width=width)


def _lower(top, upper, line, lower, bottom=None):
    """Rows from a top-bar row pattern, the upper half (a q b), the p line, the lower half (c r d) and the bottom."""
    return [top, top, "L" * len(top), upper, upper, upper, line, lower, lower, lower, bottom or lower]


BLOCKS = {
    "A": _b("oSp1pSo", _lower(F7, "### ###", F7, "### ###"), "tl6 tr6"),
    # Two closed bowls, the waist notched from the right between them.
    "B": _b("oSp1pSo", [F7, F7, "LLLLLLL", "### ###", "### ###", F7, "###### ", "### ###", "### ###", F7, F7],
            "tr br2"),
    # A stem with the opening on the right (unlike METRO's e, whose notch is cut from a wide block).
    "C": _b("oSp1o", _lower("#####", "###  ", "#####", "#####"), "tl bl", 0.8),
    "D": _b("oSp1pSo", _lower(F7, "### ###", F7, F7), "tr br"),
    # METRO's e: the notch on the right and the lime line under it running back to the stem.
    "E": _b("oS1pSo", _lower("######", "####  ", "##LLLL", "######"), "tl bl"),
    "F": _b("oSp1pSo", _lower(F7, F7, F7, "###    "), "tl"),
    "G": _b("oSp1pSo", [F7, F7, "LLLLLLL", "###    ", "###    ", "###  ##", F7, F7, F7, F7, F7], "tl bl"),
    "H": _b("oSp1pSo", ["### ###", "### ###", "LLL LLL", F7, F7, F7, F7, "### ###", "### ###", "### ###",
                        "### ###"]),
    "I": _b("oSo", _lower("###", "###", "###", "###")),
    "J": _b("o1pSo", _lower("#####", "  ###", "#####", "#####"), "bl"),
    "K": _b("oSp1pSo", ["### ###", "### ###", "LLL LLL", F7, F7, "#####  ", F7, "### ###", "### ###", "### ###",
                        "### ###"]),
    "L": _b("oSp1pSo", ["###    ", "###    ", "LLL    ", "###    ", "###    ", "###    ", "###    ", F7, F7, F7, F7],
            "bl", 0.9),
    # METRO's m: three legs under the bar.
    "M": _b("oSpgpSpgpSo", _lower("###########", "### ### ###", "### ### ###", "### ### ###"), "tr6"),
    "N": _b("oSp1pSo", _lower(F7, "### ###", "### ###", "### ###"), "tr"),
    "O": _b("oSp1pSo", _lower(F7, "### ###", F7, F7), "tl br"),
    "P": _b("oSp1pSo", _lower(F7, "### ###", F7, "###    "), "tr"),
    "Q": _b("oSp1pSo", [F7, F7, "LLLLLLL", "### ###", "### ###", "### ###", F7, F7, "####L##", F7, F7], "tl"),
    # METRO's R: a short counter, the bowl, and a notch between the stem and the leg.
    "R": _b("oSp1pSo", [F7, F7, "LLLLLLL", "### ###", F7, F7, F7, F7, "### ###", "### ###", "### ###"], "tr br"),
    # PRESIDIO's S: the block with a lime slit from the left.
    "S": _b("oSp1pSo", _lower(F7, F7, "LLLLL##", F7), "tl br"),
    # METRO's t: the stem left of centre under the bar.
    "T": _b("o1pSp3o", _lower(F7, "  ###  ", "  ###  ", "  ###  "), "tl", 0.75),
    "U": _b("oSp1pSo", ["### ###", "### ###", "LLL LLL", "### ###", "### ###", "### ###", "### ###", F7, F7, F7, F7],
            "bl br"),
    "V": _b("oSp1pSo", ["### ###", "### ###", "LLL LLL", "### ###", "### ###", "### ###", "### ###", F7, F7, F7, F7],
            "bl6 br6"),
    "W": _b("oSpgpSpgpSo", ["### ### ###", "### ### ###", "LLL LLL LLL", "### ### ###", "### ### ###",
                            "### ### ###", "### ### ###", "###########", "###########", "###########",
                            "###########"], "bl"),
    "X": _b("oSp1pSo", ["### ###", "### ###", "LLL LLL", "### ###", "### ###", F7, F7, F7, "### ###", "### ###",
                        "### ###"], "tl tr bl br"),
    "Y": _b("oSpgpSpgpSo", ["###     ###", "###     ###", "LLL     LLL", "###     ###", "###     ###", "###     ###",
                            "###########", "    ###    ", "    ###    ", "    ###    ", "    ###    "], "", 1.0),
    "Z": _b("oSp1pSo", _lower(F7, "    ###", F7, "###    ", F7), "tl br"),
    "0": _b("oSp1pSo", _lower(F7, "### ###", F7, "### ###", F7), "tl tr bl br"),
    "1": _b("o1pSo", ["#####", "#####", "LLLLL", "  ###", "  ###", "  ###", "  ###", "  ###", "  ###", "  ###",
                      "  ###"], "tl", 0.55),
    "2": _b("oSp1pSo", [F7, F7, "LLLLLLL", F7, "LLLLL##", F7, F7, F7, "##LLLLL", F7, F7], "tr bl"),
    "3": _b("oSp1pSo", [F7, F7, "LLLLLLL", F7, "LLLLL##", F7, F7, F7, "LLLLL##", F7, F7], "tr br"),
    "4": _b("oSp1pSo", ["### ###", "### ###", "LLL LLL", "### ###", "### ###", "### ###", F7, "    ###", "    ###",
                        "    ###", "    ###"], "tl"),
    "5": _b("oSp1pSo", [F7, F7, "LLLLLLL", F7, "##LLLLL", F7, F7, F7, "LLLLL##", F7, F7], "tl br"),
    "6": _b("oSp1pSo", [F7, F7, "LLLLLLL", F7, "##LLLLL", F7, F7, "### ###", "### ###", "### ###", F7], "tl bl br"),
    "7": _b("o1pSo", ["#####", "#####", "LLLLL", "  ###", "  ###", "  ###", "  ###", "  ###", "  ###", "  ###",
                      "  ###"], "tr", 0.85),
    "8": _b("oSp1pSo", [F7, F7, "LLLLLLL", "### ###", "### ###", F7, F7, "### ###", "### ###", F7, F7],
            "tl tr bl br"),
    "9": _b("oSp1pSo", [F7, F7, "LLLLLLL", "### ###", "### ###", F7, F7, F7, "LLLLL##", F7, F7], "tl tr br"),
}

# Digits 0 and 2-9 as METRO-size bitmaps (17 rows, 18 wide; '#' the digit with its lime ring, row 5 the top line):
# the top bar, the upper notch or counter two rows tall, a middle bar of a lime line, a row of blue and a lime line, the
# lower one three rows, and the bottom bar. PRESIDIO's size drops rows 2, 7, 11 and 12 (one off the top bar, the rest
# off the notches) and four middle columns.
_F, _L5, _R5, _LR = "#" * 18, "#" * 5 + "." * 13, "." * 13 + "#" * 5, "#" * 5 + "." * 8 + "#" * 5
DIGITS_2049 = {
    "0": ([_F] * 6 + [_LR] * 7 + [_F] * 4, "tl3 br2"),
    "2": ([_F] * 6 + [_R5] * 2 + [_F] * 3 + [_L5] * 3 + [_F] * 3, "tr3 bl2"),
    "3": ([_F] * 6 + [_R5] * 2 + ["...." + "#" * 14] * 3 + [_R5] * 3 + [_F] * 3, "tr3 br2"),
    "4": ([_LR] * 8 + [_F] * 3 + [_R5] * 6, ""),
    "5": ([_F] * 6 + [_L5] * 2 + [_F] * 3 + [_R5] * 3 + [_F] * 3, "tl3 br2"),
    "6": ([_F] * 6 + [_L5] * 2 + [_F] * 3 + [_LR] * 3 + [_F] * 3, "tl3 bl2 br2"),
    "7": ([_F] * 6 + [_R5] * 11, "tr3"),
    "8": ([_F] * 6 + [_LR] * 2 + [_F] * 3 + [_LR] * 3 + [_F] * 3, "tl3 tr3 bl2 br2"),
    "9": ([_F] * 6 + [_LR] * 2 + [_F] * 3 + [_R5] * 3 + [_F] * 3, "tl3 tr3 br2"),
}

# PRESIDIO's E at its size: the full block with a slit from the right.
BLOCKS_SMALL = {
    "E": _b("oS1pSo", _lower("######", "######", "##LLLL", "######"), "tl bl"),
}

# METRO (16 px tall, letters 24 px wide, no gap) and PRESIDIO (13 px, 18 px, 1 px gaps): row heights by template row,
# stem and gap widths, the corner cut, a letter's width at factor 1, the gap between letters and a space.
SIZES_2049 = [
    dict(name="metro", T=4, a=1, b=2, c=2, d=2, S=4, g=3, cut=4, width=24, gap=0, space=6, min_width=16),
    dict(name="presidio", T=3, a=1, b=1, c=1, d=1, S=4, g=2, cut=3, width=18, gap=1, space=5, min_width=12),
]

# The lower part's body colors, row by row from under the top line (METRO's legs, PRESIDIO's stems).
LOWER_2049 = {
    9: [(0, 22, 78), (2, 26, 80), (0, 42, 128), (2, 42, 132), (4, 44, 184), (4, 46, 186), (6, 60, 184), (6, 60, 186),
        (18, 86, 58)],
    7: [(0, 46, 0), (2, 38, 128), (4, 46, 132), (2, 40, 196), (6, 52, 206), (14, 56, 172), (20, 64, 184)],
}
TOP_2049 = [(0, 30, 0), (0, 10, 18), (0, 10, 20), (0, 32, 0)]
DARK_GREEN_2049 = (4, 76, 20)
LIME_TOP, LIME_BOTTOM = (186, 228, 92), (156, 222, 160)
LIME_RIGHT = (168, 250, 40)


def _heights(size):
    return dict(o=1, s=1, q=1, p=1, r=1, T=size["T"], a=size["a"], b=size["b"], c=size["c"], d=size["d"])


def _ring(grid, area, cuts, size):
    """Cuts the listed corners off the area, then rows of 'L' (its edge and the grid's lines), '#' and ' '."""
    h, w = len(area), len(area[0])
    for cut in cuts.split():
        corner, n = cut[:2], int(cut[2:]) if len(cut) > 2 else size["cut"]
        if len(cut) > 2 and size["name"] != "metro":
            n = max(1, round(n * size["cut"] / 4))
        for y in range(h):
            for x in range(w):
                dx = x if corner[1] == "l" else w - 1 - x
                dy = y if corner[0] == "t" else h - 1 - y
                if dx + dy < n:
                    area[y][x] = False
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            if not area[y][x]:
                row.append(" ")
                continue
            edge = any(not (0 <= x + i < w and 0 <= y + j < h and area[y + j][x + i])
                       for i in (-1, 0, 1) for j in (-1, 0, 1))
            row.append("L" if edge or grid[y][x] == "L" else "#")
        out.append(row)
    return out


def digit_glyph(ch, size):
    """A digit from its METRO bitmap (DIGITS_2049), cut down for PRESIDIO's size."""
    rows, cuts = DIGITS_2049[ch]
    if size["name"] != "metro":
        rows = [r[:7] + r[11:] for i, r in enumerate(rows) if i not in (2, 7, 11, 12)]
    split = 1 + size["T"]
    grid = [["L" if (y == split and c == "#") else c for c in r] for y, r in enumerate(rows)]
    area = [[c != "." for c in r] for r in grid]
    return _ring(grid, area, cuts, size), len(rows[0])


def block_glyph(ch, size, width):
    """The letter as rows of '#', 'L' and ' ' at pixel size, and its width."""
    if ch in DIGITS_2049:
        return digit_glyph(ch, size)
    spec = (BLOCKS_SMALL if size["name"] != "metro" else {}).get(ch) or BLOCKS[ch]
    cols = spec["cols"]
    fixed = {"o": 1, "p": 1, "S": size["S"], "g": size["g"]}
    w_total = max(sum(fixed.get(k, 1) for k in cols), round(width * spec["width"]))
    flex = w_total - sum(fixed.get(k, 0) for k in cols)
    weights = [int(k) for k in cols if k.isdigit()]
    widths, left, seen = [], flex, 0
    for k in cols:
        if k.isdigit():
            seen += 1
            n = left if seen == len(weights) else min(left, round(flex * int(k) / sum(weights)))
            left -= n
            widths.append(max(1, n))
        else:
            widths.append(fixed[k])
    w = sum(widths)
    hts = _heights(size)
    grid = []
    for ri, kind in enumerate(ROWS):
        for _ in range(hts[kind]):
            row = []
            for ci, cw in enumerate(widths):
                row += [spec["rows"][ri][ci]] * cw
            grid.append(row)
    h = len(grid)
    area = [[grid[y][x] != " " for x in range(w)] for y in range(h)]
    # Corner cuts, "tr" or with their length ("tr6", at METRO's size; scaled with the size).
    for cut in spec["cuts"].split():
        corner, n = cut[:2], int(cut[2:]) if len(cut) > 2 else size["cut"]
        if len(cut) > 2 and size["name"] != "metro":
            n = max(1, round(n * size["cut"] / 4))
        for y in range(h):
            for x in range(w):
                dx = x if corner[1] == "l" else w - 1 - x
                dy = y if corner[0] == "t" else h - 1 - y
                if dx + dy < n:
                    area[y][x] = False
    out = []
    for y in range(h):
        row = []
        for x in range(w):
            if not area[y][x]:
                row.append(" ")
                continue
            edge = any(not (0 <= x + i < w and 0 <= y + j < h and area[y + j][x + i])
                       for i in (-1, 0, 1) for j in (-1, 0, 1))
            row.append("L" if edge or grid[y][x] == "L" else "#")
        out.append(row)
    return out, w


def render_2049(text, size_px):
    W, H = size_px
    for size in SIZES_2049:
        width = size["width"]
        while True:
            x, out = 0, []
            for ch in text:
                if ch == " ":
                    x += size["space"]
                    continue
                g, w = block_glyph(ch, size, width)
                out.append((g, w, x))
                x += w + size["gap"]
            total = x - size["gap"]
            if total <= W - 2 or width <= size["min_width"]:
                break
            width -= 1
        if total <= W - 2:
            break
    hts = _heights(size)
    h = len(out[0][0]) if out else 0
    split = hts["o"] + hts["T"]                 # the top line's row
    lower_h = h - split - 2
    palette = LOWER_2049.get(lower_h) or LOWER_2049[9]
    x0 = (W - total) // 2
    top = (H - h) // 2
    img = Image.new("RGBA", size_px, (0, 0, 0, 0))
    px = img.load()
    for g, w, gx in out:
        for y in range(h):
            for x in range(w):
                v = g[y][x]
                X, Y = x0 + gx + x, top + y
                if v == " " or not (0 <= X < W):
                    continue
                if v == "L":
                    t = y / max(1, h - 1)
                    c = tuple(round(a + (b - a) * t) for a, b in zip(LIME_TOP, LIME_BOTTOM))
                    if 0 < y < h - 1 and x > 0 and g[y][x - 1] == "#" and (x == w - 1 or g[y][x + 1] == " "):
                        c = LIME_RIGHT
                elif y < split:
                    k = y - 1
                    c = TOP_2049[0] if k == 0 else TOP_2049[-1] if y == split - 1 else TOP_2049[1 + k % 2]
                else:
                    k = y - split - 1
                    c = palette[min(k, len(palette) - 1)]
                    if k > 0 and g[y - 1][x] == "L":
                        c = DARK_GREEN_2049
                px[X, Y] = c + (255,)
    return img


# ------------------------------------------------------------------------------------------------------------------


def render(text, style, size=(128, 32)):
    """text in the style, centred in an RGBA image of size."""
    text = "".join(ch for ch in text.upper() if ch == " " or ch in GLYPHS)
    return render_sfrush(text, size) if style == "sfrush" else render_2049(text, size)


def compare_sheet():
    """Each real banner beside its name drawn in the font, on a slate background."""
    import glob
    files = sorted(glob.glob(os.path.join(ROOT, "tools/rush1/banners/*.png")))
    files += sorted(glob.glob(os.path.join(ROOT, "tools/rush2049/banners/*.png")))
    sheet = Image.new("RGBA", (256, 32 * len(files)), (90, 110, 140, 255))
    for i, f in enumerate(files):
        sheet.alpha_composite(Image.open(f).convert("RGBA"), (0, 32 * i))
        name = os.path.basename(f)[:-4].split("_", 1)[1].replace("_", " ")
        sheet.alpha_composite(render(name, "sfrush" if "rush1" in f else "rush2049"), (128, 32 * i))
    return sheet


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "banner_font.png"
    if "--compare" in sys.argv:
        sheet = compare_sheet()
    else:
        rows = ["ABCDEFGHI", "JKLMNOPQR", "STUVWXYZ", "0123456789", "THE ROCK", "MARINA", "HAIGHT", "CIVIC",
                "MISSION", "OBSTACLE", "BATTLE 8"]
        sheet = Image.new("RGBA", (128 * 2, 32 * len(rows)), (90, 110, 140, 255))
        for i, row in enumerate(rows):
            for j, style in enumerate(("sfrush", "rush2049")):
                sheet.alpha_composite(render(row, style), (128 * j, 32 * i))
    sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(out)
