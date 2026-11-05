"""Builds the high-resolution font texture pack (assets/rush2_hires_fonts.rtz).

The game's fonts are 4-bit intensity (I4) glyph sheets, and the race HUD's numbers and "MPH" label are color (CI)
images. All of them are redrawn as SVGs once and kept in tools/font_pack/svg and tools/font_pack/hud_svg (one per
texture, named by its RT64 texture hash), so the pack can be rebuilt or hand-edited without the ROM. None of the
game's own pixels are packed.

  python tools/build_font_pack.py trace <font_tables.json>        Redraw every font sheet as an SVG.
  python tools/build_font_pack.py trace-2049                      Redraw the Rush 2049 font sheets the game draws
                                                                  (ROM2049_FONTS; RUSH2049_ROM = the 2049 ROM).
  python tools/build_font_pack.py trace-2049-menus <menu_fonts.json>
                                                                  Redraw the Rush 2049 Menus font sheets
                                                                  (src/rush2049/menus2049.cpp) from their dump.
  python tools/build_font_pack.py trace-hud <texture dump dir>...  Redraw the HUD images listed in hud.json.
  python tools/build_font_pack.py pack                            Rasterize the SVGs and write the .rtz.
  python tools/build_font_pack.py preview <font_tables.json> <out.png> [font id...]
                                                                  Each sheet's original above its redraw.
  python tools/build_font_pack.py images <image dump> <out dir>   Write every dumped 2D image as <RT64 hash>.png.
  python tools/build_font_pack.py check <svg>                     List glyphs whose outline is cut off by their cell
                                                                  (overhang left, right, top, bottom in texels).
  python tools/build_font_pack.py fit <svg> <clip id> [y0 y1] [--stroke W] [--scale S]
                                                                  Squeeze a glyph to fit its cell; with y0 y1, set the
                                                                  rows its ink spans (e.g. a "/" to the digits'); W
                                                                  thins its outline so a narrow cell squeezes it less.

font_tables.json comes from running the game with RUSH2_DUMP_FONT_TABLES=<path> (src/fonts.cpp). It holds each font's
pages: the sheet's pixels and the rectangle of every character in it. The Rush 2049 fonts the game draws (the Rush 2049
Stunt HUD's, src/rush2049/stunt2049.cpp) are read from the 2049 ROM instead (rom2049_tables) and kept in
tools/font_pack/svg_2049; their ids in fonts.json are "rush2049_<font>". The HUD images come from RT64 texture dumps
(<hash>.v5.tmem and .tile.json, from the RT64 debugger's texture dumper in developer mode).

Fonts with "style": "drawn" are hand-drawn outlines (tools/font_pack/<"design">.py) fitted to each font's texels (see
Drawn fonts): Rush 2's own lettering (rush2_glyphs.py: Small, Medium, Large and Large digits) and Rush 2049's AERO menu
lettering (aero_glyphs.py). A drawing's hard shapes (Rush 2's lowercase, digits and rounder symbols) can come from a
reference font instead (tools/font_pack/reference/Arimo.ttf, SIL Open Font License, the metric twin of Helvetica, whose
bold Rush 2's lettering is), fitted the same way. The sheets are plain anti-aliased coverage, no outline. Text is drawn
in copy mode, whose rectangles include their lower right texel, so a glyph shows one column and row past the cell its
table lists: each redraw is clipped to that full region, or round bottoms and right edges are cut flat.

Fonts with "style": "vector" (the Rush 2049 Stunt HUD's, read from the 2049 ROM): every glyph is a full-intensity core
with a dim one-texel halo around it. Each character is replaced by the same character from Inter
(assets/InterVariable.ttf, SIL Open Font License), drawn as an SVG path with the core as its fill and the halo as an
outer stroke, clipped to the character's cell. Per font, the Inter weight, how far the outlines sit inside each glyph's
ink box, and the halo's intensity and width are fitted by least squares so the redrawn sheet, averaged back down to
texels, matches the original. (The Rush 2 fonts were drawn this way once; their dim "halo" was really anti-aliasing,
and keeping it inside the ink box made every letter a texel short.)

Squared fonts (Tiny and Pixel) are too small to trace or match to a vector font, so they're drawn from a hand-made
squared font (BLOCK_GLYPHS): each character's strokes swept by a rectangular pen measured from the original, with the
halo fitted the same way. Their characters BLOCK_GLYPHS lacks ((c), (R), TM) or a font lists under "trace" are the pen
swept along their own texels (texel_sweep_polygons), straight through diagonal steps.

The Block font (5, style "segment") is a segment display: 1-texel segments apart at the corners with a dim glow. Its
characters are drawn on one segment layout (SEGMENTS: an 8's seven, two center verticals, half and full crosses, V's
point), each segment the same tapered bar (display_segment), the lit ones read from the texels (segment_layout) or
set by SEGMENT_OVERRIDES ($, *, @, ', !, ^, Z); the glow is each segment widened on its own (offset_polygon), not a
halo round the glyph, because highlighted text draws the glow at full color and a halo ran the segments together. Characters off the layout (parentheses, braces, =)
keep their texel runs as pointed segments. Pixel's sheet carries a copy of Block's texels below its characters, read at
Pixel's width (its "segment_rows"), redrawn the same way.

Ink on a sheet that no character uses is traced: bicubic upsampling, a contour between the core and halo levels,
simplification, and sharp corners kept while gentle turns become curves (all corners for squared fonts).

HUD images: see the HUD images section. Menu images (the blinking selection arrows): see the Image dumps section.

Packing renders the SVGs at SCALE times the original size with supersampling. Font sheets are stored as RGBA with every
channel equal to the intensity, which is how RT64 samples I4 textures; HUD images as RGBA. Font sheet hashes are
computed the way RT64's TMEMHasher does for a T-clamped I4 tile loaded with LoadBlock (src/fonts.cpp clamps the tiles
so the hashes are stable).
"""

import glob
import io
import json
import os
import re
import struct
import sys
import zipfile

import cv2
import numpy as np
import xxhash
from fontTools.pens.basePen import BasePen
from fontTools.pens.boundsPen import BoundsPen
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PACK_DIR = os.path.join(ROOT, 'tools', 'font_pack')
SVG_DIR = os.path.join(PACK_DIR, 'svg')
FONT_LIST = os.path.join(PACK_DIR, 'fonts.json')
HUD_LIST = os.path.join(PACK_DIR, 'hud.json')
HUD_SVG_DIR = os.path.join(PACK_DIR, 'hud_svg')
# Menu images, drawn by tools/font_pack/draw_arrows.py (never cleared here). The selection arrows are drawn flipped
# vertically starting at their last row, which RT64 samples from texel centers, so the bottom stored row never shows and
# the clamped edge past row 0 shows below them: their drawings fit between y = 0.25 and 12, leaving that edge clear.
MENU_SVG_DIR = os.path.join(PACK_DIR, 'menu_svg')
# The battle HUD's digits (src/hud.cpp draw_number), drawn by tools/font_pack/draw_battle_digits.py: white, with the
# figure in the alpha, as RT64 samples an intensity image.
BATTLE_SVG_DIR = os.path.join(PACK_DIR, 'battle_svg')
# Rush 2049's font sheets (trace-2049), drawn by src/hud.cpp draw_i4_part as I4 sheets like the game's own.
SVG_2049_DIR = os.path.join(PACK_DIR, 'svg_2049')
# The Rush 2049 Menus option's font sheets (trace-2049-menus): Rush 2049's menu fonts repacked into Rush 2 font
# sheets by src/rush2049/menus2049.cpp, which writes them in the font table format with RUSH2_DUMP_MENU_FONTS=<path>
# (ids rush2049_menu_<Rush 2 font> in fonts.json).
SVG_2049_MENU_DIR = os.path.join(PACK_DIR, 'svg_2049_menu')
# Rush 2049 fonts the game draws: 4BAERO-PLAIN12 and AERO-PLAIN7 (include/font2049.h).
ROM2049_FONTS = (10, 12)
VECTOR_FONT = os.path.join(ROOT, 'assets', 'InterVariable.ttf')
OUTPUT = os.path.join(ROOT, 'assets', 'rush2_hires_fonts.rtz')

SCALE = 8          # Replacement resolution, per source texel.
SUPERSAMPLE = 4    # Antialiasing samples per output pixel, per axis.
FIT_RES = 8        # Rendering resolution while fitting, per source texel.
UPSAMPLE = 16      # Contouring resolution for traced shapes, per source texel.
SIMPLIFY = 0.2     # Traced polygon simplification tolerance, in texels. Straightens the 4-bit edge wobble.
CORNER_ANGLE = 55  # Traced turns sharper than this (degrees) stay corners; gentler ones become curves.
CURVE_REACH = 0.6  # How far along each edge (in texels) a curve can start before a smooth vertex.
INK_FLOOR = 4      # Intensities below this are background noise, not ink.

FIT_WEIGHTS = range(300, 901, 100)
FIT_INSETS = (0.0, 0.25, 0.5, 0.75, 1.0)
FIT_HALO_WIDTHS = (0.5, 0.75, 1.0, 1.25, 1.5)

# Game character codes outside ASCII and the Unicode characters they show.
SPECIAL_CHARACTERS = {25: '™', 26: '©', 27: '®', 29: 'Ç', 30: '¿'}

MANIFEST = {
    'game_id': 'rush2',
    'id': 'rush2_hires_fonts',
    'display_name': 'High-Resolution Fonts',
    'description': 'Redrawn versions of the game\'s fonts that stay sharp at any resolution. '
                   'Turn them on or off in Settings > Graphics > Fonts.',
    'short_description': 'Sharp fonts at any resolution.',
    'version': '1.0.0',
    'authors': ['Rush 2: Recompiled'],
    'minimum_recomp_version': '0.0.0',
}


# ---------------------------------------------------------------------------------------------------------------------
# Sheets


def decode_sheet(page):
    """The page's I4 pixels as intensities 0-15."""
    raw = np.frombuffer(bytes.fromhex(page['pixels']), np.uint8).reshape(page['h'], page['w'] // 2)
    sheet = np.zeros((page['h'], page['w']), np.float32)
    sheet[:, 0::2] = raw >> 4
    sheet[:, 1::2] = raw & 0xF
    return sheet


def rt64_hash(page):
    """RT64's TMEMHasher (version 5) for a clamped I4 sheet loaded into TMEM address 0 with LoadBlock."""
    width, height = page['w'], page['h']
    row_bytes = width // 2
    line = row_bytes // 8
    pixels = bytes.fromhex(page['pixels'])
    tmem = bytearray()
    for y in range(height):
        row = pixels[y * row_bytes:(y + 1) * row_bytes]
        if y & 1:  # LoadBlock swaps the 32-bit words of odd rows.
            row = b''.join(row[i + 4:i + 8] + row[i:i + 4] for i in range(0, len(row), 8))
        tmem += row
    tlut, siz, fmt = 0, 0, 4
    data = bytes(tmem) + struct.pack('<HHIHBB', width, height, tlut, line, siz, fmt)
    return f'{xxhash.xxh3_64_intdigest(data):016x}'


def glyph_cells(page, font=None):
    """(character code, x0, y0, x1, y1) for every character drawn from the page. Fonts marked inclusive have
    rectangles whose right and bottom edges are part of the glyph (the tiny font's 3x5 letters are listed as 2x4)."""
    extra = 1 if font and font.get('inclusive') else 0
    for i, (x0, y0, x1, y1) in enumerate(page['glyphs']):
        # Inclusive rectangles of one-texel-wide characters ('.', ':', '|') have x1 == x0.
        if y1 != 0 and 0 <= x0 < x1 + extra and x1 <= page['w'] and 0 <= y0 < y1 + extra and y1 <= page['h']:
            yield page['first'] + i, x0, y0, min(x1 + extra, page['w']), min(y1 + extra, page['h'])


def ink_box(cell):
    """Bounds of a cell's ink (halo included), or None for an empty cell."""
    ys, xs = np.nonzero(cell >= INK_FLOOR)
    if len(xs) == 0:
        return None
    return float(xs.min()), float(ys.min()), float(xs.max() + 1), float(ys.max() + 1)


# ---------------------------------------------------------------------------------------------------------------------
# Paths


def fmt(p):
    return f'{p[0]:.3f},{p[1]:.3f}'


def trace_smooth(cell, threshold):
    """Contours of a cell at an intensity threshold as simplified polygons, in texel coordinates."""
    pad = 2
    padded = np.pad(cell, pad)
    h, w = padded.shape
    field = cv2.resize(padded, (w * UPSAMPLE, h * UPSAMPLE), interpolation=cv2.INTER_CUBIC)
    mask = (field >= threshold).astype(np.uint8)
    contours, _ = cv2.findContours(mask, cv2.RETR_CCOMP, cv2.CHAIN_APPROX_NONE)
    polygons = []
    for contour in contours:
        if cv2.contourArea(contour) < (0.15 * UPSAMPLE) ** 2:
            continue  # Specks from faint noise.
        simplified = cv2.approxPolyDP(contour, SIMPLIFY * UPSAMPLE, True).reshape(-1, 2).astype(np.float64)
        if len(simplified) >= 3:
            # Upsampled pixel centers back to texel coordinates (texel edges at integers).
            polygons.append((simplified + 0.5) / UPSAMPLE - pad)
    return polygons


def smooth_path(polygon):
    """SVG path data for a polygon: sharp turns stay corners, gentle turns become quadratic curves."""
    n = len(polygon)
    commands = []
    for i in range(n):
        prev, cur, nxt = polygon[i - 1], polygon[i], polygon[(i + 1) % n]
        a, b = cur - prev, nxt - cur
        la, lb = np.hypot(*a), np.hypot(*b)
        if la == 0 or lb == 0:
            continue
        turn = np.degrees(np.arccos(np.clip(np.dot(a, b) / (la * lb), -1, 1)))
        if turn > CORNER_ANGLE:
            commands.append(f'L{fmt(cur)}')
        else:
            commands.append(f'L{fmt(cur - a * min(0.5, CURVE_REACH / la))}')
            commands.append(f'Q{fmt(cur)} {fmt(cur + b * min(0.5, CURVE_REACH / lb))}')
    commands[0] = 'M' + commands[0][1:] if commands[0][0] == 'L' else commands[0]
    return ' '.join(commands) + ' Z'


def parse_path(d):
    """Polygons from SVG path data of M/L/Q/C/Z commands, flattening the curves."""
    polygons = []
    points = []
    for cmd, args in re.findall(r'([MLQCZ])([^MLQCZ]*)', d):
        coords = np.array([float(v) for v in re.findall(r'-?[\d.]+', args)]).reshape(-1, 2)
        if cmd == 'M':
            points = [coords[0]]
        elif cmd == 'L':
            points.append(coords[0])
        elif cmd in 'QC':
            controls = [points[-1], *coords]
            for t in np.linspace(0, 1, 9)[1:]:
                level = controls
                while len(level) > 1:  # De Casteljau.
                    level = [(1 - t) * a + t * b for a, b in zip(level, level[1:])]
                points.append(level[0])
        elif cmd == 'Z' and len(points) >= 3:
            polygons.append(np.array(points))
    return polygons


# ---------------------------------------------------------------------------------------------------------------------
# Drawn fonts
#
# Fonts with "style": "drawn" are hand-drawn outlines (tools/font_pack/<"design">.py, GLYPHS) fitted to each font's
# texels. A drawing's numbers are its edges: every x or y written the same in a glyph is one edge, every rounded corner
# has its radii along both edges, every stroke its width, and the corners' curvature (the cubic handles' length) is one
# number per glyph. The fit first places the drawing on the cell (scale and offset), then moves those numbers by least
# squares until the drawing's exact area coverage of each texel, times the font's full intensity, matches the original
# texel. A pull back toward the placed drawing keeps texels that say little (tiny fonts) from bending the shape.

DRAWN_K = 0.55          # Cubic handle length (fraction of the corner radius) a corner starts from; 0.55 is circular.
DRAWN_STEPS = 8         # Line segments per corner curve while fitting.
DRAWN_PULL = 0.05       # Default pull toward the placed drawing, per texel moved, against one texel's intensity error.


def _drawn_kernels():
    import numba

    @numba.njit(cache=True)
    def line(acc, w, h, x0, y0, x1, y1):
        # Exact signed area accumulation of one edge (font-rs's rasterizer).
        if y0 == y1:
            return
        d = 1.0
        if y0 > y1:
            d = -1.0
            x0, y0, x1, y1 = x1, y1, x0, y0
        dxdy = (x1 - x0) / (y1 - y0)
        x = x0
        if y0 < 0.0:
            x -= y0 * dxdy
        start = max(int(np.floor(y0)), 0)
        end = min(h, int(np.ceil(y1)))
        for y in range(start, end):
            row = y * w
            dy = min(y + 1.0, y1) - max(float(y), y0)
            xnext = x + dxdy * dy
            dd = dy * d
            if x < xnext:
                xa, xb = x, xnext
            else:
                xa, xb = xnext, x
            xa = min(max(xa, 0.0), w - 2.001)
            xb = min(max(xb, 0.0), w - 2.001)
            x0f = np.floor(xa)
            x0i = int(x0f)
            x1c = np.ceil(xb)
            x1i = int(x1c)
            if x1i <= x0i + 1:
                xmf = 0.5 * (xa + xb) - x0f
                acc[row + x0i] += dd - dd * xmf
                acc[row + x0i + 1] += dd * xmf
            else:
                s = 1.0 / (xb - xa)
                xf = xa - x0f
                a0 = 0.5 * s * (1.0 - xf) ** 2
                x1f = xb - x1c + 1.0
                am = 0.5 * s * x1f ** 2
                acc[row + x0i] += dd * a0
                if x1i == x0i + 2:
                    acc[row + x0i + 1] += dd * (1.0 - a0 - am)
                else:
                    a1 = s * (1.5 - xf)
                    acc[row + x0i + 1] += dd * (a1 - a0)
                    for xi in range(x0i + 2, x1i - 1):
                        acc[row + xi] += dd * s
                    a2 = a1 + (x1i - x0i - 3) * s
                    acc[row + x1i - 1] += dd * (1.0 - a2 - am)
                acc[row + x1i] += dd * am
            x = xnext

    @numba.njit(cache=True)
    def coverage(points, starts, w, h):
        acc = np.zeros(w * h + w + 2)
        for c in range(len(starts) - 1):
            a, b = starts[c], starts[c + 1]
            for i in range(a, b):
                j = i + 1 if i + 1 < b else a
                line(acc, w, h, points[i, 0], points[i, 1], points[j, 0], points[j, 1])
        out = np.empty(w * h)
        total = 0.0
        for i in range(w * h):
            total += acc[i]
            # Filled contours turn positive (their sum is -1 inside), counters negative: a counter cuts only ink.
            out[i] = min(1.0, max(0.0, -total))
        return out.reshape(h, w)

    return coverage


_DRAWN_COVERAGE = []


def drawn_coverage(contours, width, height, pad=4):
    """Exact coverage (0-1) of each texel of a width x height cell by contours in cell coordinates (nonzero winding;
    holes run the other way)."""
    if not _DRAWN_COVERAGE:
        _DRAWN_COVERAGE.append(_drawn_kernels())
    if not contours:
        return np.zeros((height, width))
    points = np.ascontiguousarray(np.concatenate(contours) + pad, dtype=np.float64)
    starts = np.cumsum([0] + [len(c) for c in contours]).astype(np.int64)
    full = _DRAWN_COVERAGE[0](points, starts, width + 2 * pad + 2, height + 2 * pad)
    return full[pad:pad + height, pad:pad + width]


def signed_area(points):
    x, y = points[:, 0], points[:, 1]
    return 0.5 * float(np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1)))


class DrawnGlyph:
    """One hand-drawn glyph (see "Drawn fonts") and the numbers the fit moves: its x edges, y edges, corner radii,
    stroke widths and corner curvature, in that order in the parameter vector."""

    def __init__(self, shapes):
        self.shapes = shapes
        xs, ys = {}, {}

        def key(table, v):
            return table.setdefault(round(float(v), 6), len(table))

        self.refs = []      # Per shape: per point (x index, y index, radius indices or None); strokes add a width.
        radii, widths = [], []
        for shape in shapes:
            if shape[0] in 'ph':
                refs = []
                for x, y, r in shape[1]:
                    ra, rb = r if isinstance(r, tuple) else (r, r)
                    rr = None
                    if ra or rb:
                        rr = (len(radii), len(radii) + 1)
                        radii += [ra, rb]
                    refs.append((key(xs, x), key(ys, y), rr))
            elif shape[0] == 'f':
                # A reference font's glyph: its boldness (0.2-1, weight 460-700) is fitted like a stroke's width.
                refs = ('f', len(widths))
                widths.append(shape[2] if len(shape) > 2 else 1.0)
            else:
                refs = [(key(xs, x), key(ys, y), None) for x, y in shape[1]]
                refs.append(len(widths))
                widths.append(shape[2])
            self.refs.append(refs)
        self.nx, self.ny, self.nr, self.nw = len(xs), len(ys), len(radii), len(widths)
        self.initial = np.array(list(xs) + list(ys) + radii + widths + [DRAWN_K], float)
        # Each contour's turning direction as drawn. A fit that shrinks a counter the cell doesn't have (the 7-pixel
        # A) can turn it inside out, which would fill instead of cut: a contour turning the other way is dropped.
        self.turns = None
        self.turns = self.contour_turns(self.initial)

    def contour_turns(self, p):
        return [np.sign(signed_area(pts)) for pts in self.outline(p, (1.0, 0.0, 1.0, 0.0), raw=True)]

    def stroke_edges(self):
        """The x and y edges (indices) strokes use."""
        xs, ys = set(), set()
        for shape, refs in zip(self.shapes, self.refs):
            if shape[0] == 's':
                xs |= {r[0] for r in refs[:-1]}
                ys |= {r[1] for r in refs[:-1]}
        return sorted(xs), sorted(ys)

    def own_edges(self):
        """Per shape, the x and y edges (indices) no other shape uses."""
        def used(refs):
            if refs[0] == 'f':
                return set(), set()
            pts = refs[:-1] if isinstance(refs[-1], int) else refs
            return {r[0] for r in pts}, {r[1] for r in pts}
        sets = [used(refs) for refs in self.refs]
        out = []
        for i, (xs, ys) in enumerate(sets):
            other_x = set().union(*(s[0] for j, s in enumerate(sets) if j != i))
            other_y = set().union(*(s[1] for j, s in enumerate(sets) if j != i))
            out.append((sorted(xs - other_x), sorted(ys - other_y)))
        return out

    def split(self, p):
        i = self.nx
        j = i + self.ny
        k = j + self.nr
        m = k + self.nw
        return p[:i], p[i:j], p[j:k], p[k:m], p[m]

    def outline(self, p, transform, steps=DRAWN_STEPS, raw=False):
        """Contours for parameters p, each (points, segments): the points flattened, the segments the exact path
        (('L', end) and ('C', control, control, end)), oriented so holes cancel. transform = (sx, ox, sy, oy) maps
        the drawing's coordinates to the cell's."""
        px, py, pr, pw, k = self.split(p)
        sx, ox, sy, oy = transform

        def place(pt):
            return np.array([ox + sx * pt[0], oy + sy * pt[1]])

        contours = []
        index = -1
        for shape, refs in zip(self.shapes, self.refs):
            if shape[0] == 'f':
                if not raw:
                    contours += reference_outline(shape[1], pw[refs[1]], place)
                continue
            if shape[0] not in 'ph':
                if raw:
                    continue
                contours += stroke_outline([np.array([px[xi], py[yi]]) for xi, yi, _ in refs[:-1]],
                                           pw[refs[-1]], shape[3], place)
                continue
            corners = [np.array([px[xi], py[yi]]) for xi, yi, _ in refs]
            n = len(corners)
            pts, segs = [], []
            for i, (_, _, rr) in enumerate(refs):
                v = corners[i]
                if rr is None:
                    pts.append(place(v))
                    segs.append(('L', place(v)))
                    continue
                prev, nxt = corners[i - 1], corners[(i + 1) % n]
                lin, lout = np.hypot(*(prev - v)), np.hypot(*(nxt - v))
                # Two rounded corners share an edge half each.
                ra = min(max(pr[rr[0]], 0.0), 0.5 * lin if refs[i - 1][2] else lin)
                rb = min(max(pr[rr[1]], 0.0), 0.5 * lout if refs[(i + 1) % n][2] else lout)
                a = v + (prev - v) / max(lin, 1e-9) * ra
                b = v + (nxt - v) / max(lout, 1e-9) * rb
                c1, c2 = a + (v - a) * k, b + (v - b) * k
                ctrl = [place(q) for q in (a, c1, c2, b)]
                for t in np.linspace(0, 1, steps + 1):
                    pts.append((1 - t) ** 3 * ctrl[0] + 3 * (1 - t) ** 2 * t * ctrl[1] +
                               3 * (1 - t) * t ** 2 * ctrl[2] + t ** 3 * ctrl[3])
                segs.append(('L', ctrl[0]))
                segs.append(('C', ctrl[1], ctrl[2], ctrl[3]))
            pts = np.array(pts)
            index += 1
            if raw:
                contours.append(pts)
                continue
            if self.turns is not None and np.sign(signed_area(pts)) != self.turns[index]:
                continue
            want = 1 if shape[0] == 'p' else -1
            if np.sign(signed_area(pts)) != want:
                pts, segs = pts[::-1], reverse_segments(segs)
            contours.append((pts, segs))
        return contours

    def svg(self, p, transform):
        contours = self.outline(p, transform)
        filled = [pts.astype(np.float32) for pts, _ in contours if signed_area(pts) > 0]
        commands = []
        for pts, segs in contours:
            # A counter outside every filled contour would fill under the nonzero rule: left out.
            if signed_area(pts) < 0:
                center = tuple(float(v) for v in pts.mean(axis=0))
                if not any(cv2.pointPolygonTest(f, center, False) >= 0 for f in filled):
                    continue
            for i, seg in enumerate(segs):
                commands.append(('M' if i == 0 else seg[0]) + ' '.join(fmt(q) for q in seg[1:]))
            commands.append('Z')
        return ' '.join(commands)


def reverse_segments(segs):
    """The same closed path of L/C segments traversed the other way."""
    ends = [seg[-1] for seg in segs]
    out = []
    for i in range(len(segs) - 1, -1, -1):
        start = ends[i - 1]
        if segs[i][0] == 'C':
            out.append(('C', segs[i][2], segs[i][1], start))
        else:
            out.append(('L', start))
    # The reversed path starts where the original's last segment ended.
    return [('L', ends[-1])] + out


def smooth_polyline(points, closed, steps=6):
    """A Catmull-Rom curve through points, as a polyline of steps segments per span."""
    pts = np.array(points, float)
    n = len(pts)
    if n < 3:
        return pts
    ext = np.vstack([pts[-1:], pts, pts[:2]]) if closed else np.vstack([2 * pts[0] - pts[1], pts, 2 * pts[-1] - pts[-2]])
    spans = n if closed else n - 1
    out = []
    for i in range(spans):
        p0, p1, p2, p3 = ext[i], ext[i + 1], ext[i + 2], ext[i + 3]
        for t in np.linspace(0, 1, steps, endpoint=False):
            t2, t3 = t * t, t * t * t
            out.append(0.5 * (2 * p1 + (p2 - p0) * t + (2 * p0 - 5 * p1 + 4 * p2 - p3) * t2 +
                              (3 * p1 - p0 - 3 * p2 + p3) * t3))
    if not closed:
        out.append(pts[-1])
    return np.array(out)


def stroke_outline(points, width, cap, place):
    """Contours of a stroke of a width along a polyline (closed when it ends where it starts): the polyline offset by
    half the width on each side, joined with miters, its ends cut across the stroke (cap 'p') or level (cap 'h'). A cap
    ending in 's' makes the polyline a smooth curve through its points."""
    closed = np.allclose(points[0], points[-1])
    pts = np.array(points[:-1] if closed else points, float)
    if cap.endswith('s'):
        pts = smooth_polyline(pts, closed)
    n = len(pts)
    half = max(width, 0.05) / 2

    def normal(a, b):
        d = b - a
        length = max(np.hypot(*d), 1e-9)
        return np.array([-d[1], d[0]]) / length, d / length

    left, right = [], []
    for i in range(n):
        if closed or 0 < i < n - 1:
            n1, _ = normal(pts[i - 1], pts[i])
            n2, _ = normal(pts[i], pts[(i + 1) % n])
            m = n1 + n2
            m /= max(np.hypot(*m), 1e-9)
            reach = min(half / max(np.dot(m, n1), 0.25), 4 * half)
            left.append(pts[i] + m * reach)
            right.append(pts[i] - m * reach)
            continue
        nn, d = normal(pts[0], pts[1]) if i == 0 else normal(pts[i - 1], pts[i])
        if cap[0] == 'h' and abs(d[1]) > 0.2:
            # Along each offset line to the end point's level.
            lq, rq = pts[i] + nn * half, pts[i] - nn * half
            left.append(lq + d * (pts[i][1] - lq[1]) / d[1])
            right.append(rq + d * (pts[i][1] - rq[1]) / d[1])
        else:
            left.append(pts[i] + nn * half)
            right.append(pts[i] - nn * half)
    left = [place(q) for q in left]
    right = [place(q) for q in right]
    rings = [np.array(left), np.array(right)] if closed else [np.array(left + right[::-1])]
    rings.sort(key=lambda r: -abs(signed_area(r)))
    out = []
    for ring, want in zip(rings, (1, -1)):
        if np.sign(signed_area(ring)) != want:
            ring = ring[::-1]
        out.append((ring, [('L', q) for q in ring]))
    return out


REFERENCE_FONT = os.path.join(PACK_DIR, 'reference', 'Arimo.ttf')
DRAWN_CAP = [(-0.15, 11.15)]    # The design being fitted's cap top and bottom (drawn_elements sets it).
_REFERENCE = {}


class FlattenPen(BasePen):
    """A glyph's contours as polylines (curves cut into DRAWN_STEPS lines)."""

    def __init__(self, glyph_set):
        super().__init__(glyph_set)
        self.contours = []

    def _moveTo(self, pt):
        self.contours.append([pt])

    def _lineTo(self, pt):
        self.contours[-1].append(pt)

    def _qCurveToOne(self, p1, p2):
        p0 = self.contours[-1][-1]
        for t in np.linspace(0, 1, DRAWN_STEPS + 1)[1:]:
            self.contours[-1].append(tuple((1 - t) ** 2 * np.array(p0) + 2 * (1 - t) * t * np.array(p1) +
                                           t * t * np.array(p2)))

    def _curveToOne(self, p1, p2, p3):
        p0 = self.contours[-1][-1]
        for t in np.linspace(0, 1, DRAWN_STEPS + 1)[1:]:
            self.contours[-1].append(tuple((1 - t) ** 3 * np.array(p0) + 3 * (1 - t) ** 2 * t * np.array(p1) +
                                           3 * (1 - t) * t * t * np.array(p2) + t ** 3 * np.array(p3)))


def reference_glyph(char, weight):
    """The reference font's glyph for char at a weight, as contours in font units (y up), cached."""
    key = (char, round(weight, 1))
    if key not in _REFERENCE:
        if 'font' not in _REFERENCE:
            _REFERENCE['font'] = TTFont(REFERENCE_FONT)
            font = _REFERENCE['font']
            _REFERENCE['cap'] = font['OS/2'].sCapHeight
        font = _REFERENCE['font']
        glyphs = font.getGlyphSet(location={'wght': weight})
        pen = FlattenPen(glyphs)
        glyphs[font.getBestCmap()[ord(char)]].draw(pen)
        _REFERENCE[key] = [np.array(c, float) for c in pen.contours if len(c) >= 3]
    return _REFERENCE[key]


def reference_outline(char, boldness, place):
    """Contours of the reference font's char (Arimo, the metric twin of Helvetica, whose bold Rush 2's lettering
    is): its cap height made the design's caps, baseline on the caps' bottom, at weight 400 + 300 x boldness."""
    weight = float(np.clip(400 + 300 * boldness, 400, 700))
    contours = reference_glyph(char, weight)
    top, bottom = DRAWN_CAP[0]
    k = (bottom - top) / _REFERENCE['cap']
    placed = [np.array([place(np.array([x * k, bottom - y * k])) for x, y in c]) for c in contours]
    # Filled contours turn positive: the biggest decides which way the font's run.
    if signed_area(max(placed, key=lambda c: abs(signed_area(c)))) < 0:
        placed = [c[::-1] for c in placed]
    return [(c, [('L', q) for q in c]) for c in placed]


def ink_edges(profile):
    """Where ink starts and ends along a coverage profile (0-1 per texel): the first and last inked texels, less the
    part of each the ink doesn't cover."""
    inked = np.nonzero(profile > 0.04)[0]
    if len(inked) == 0:
        return None
    a, b = inked[0], inked[-1]
    return a + 1 - profile[a], b + profile[b]


def fit_drawn_glyph(glyph, cell, level_y, pull, native=False):
    """Fits a drawn glyph to a cell's coverage (0-1 per texel). level_y = (sy, oy): the font's cap scale and the
    cell row its cap top sits on. A native drawing is in the cell's own texels already. Returns (parameters,
    transform, rms error in intensity fractions)."""
    from scipy.optimize import least_squares
    height, width = cell.shape
    sy, oy = level_y
    p0 = glyph.initial.copy()
    design = np.concatenate([pts for pts, _ in glyph.outline(p0, (1.0, 0.0, 1.0, 0.0))])
    edges = ink_edges(cell.max(axis=0))
    dl, dr = design[:, 0].min(), design[:, 0].max()
    sx = (edges[1] - edges[0]) / max(dr - dl, 0.5) if edges else sy
    transform = np.array([sx, (edges[0] if edges else 0.0) - sx * dl, sy, oy])
    if native:
        transform = np.array([1.0, 0.0, 1.0, 0.0])
    # A reference font's glyph keeps close to its proportions: its place, size (within a tenth of the font's caps),
    # weight and a narrowing to 0.7 (Rush 2's @ is narrower than Helvetica's) are fitted, nothing that bends it.
    reference = any(shape[0] == 'f' for shape in glyph.shapes)
    t_bounds = (-np.inf, np.inf)
    margin = 3

    def coverage_of(p, t):
        return drawn_coverage([pts for pts, _ in glyph.outline(p, t)], width, height)

    def residual(p, t):
        """Texel errors; for a reference glyph also its ink past the drawn region, which the game cuts off, so it
        fits inside rather than being clipped."""
        if not reference:
            return (coverage_of(p, t) - cell).ravel()
        wide = drawn_coverage([pts + margin for pts, _ in glyph.outline(p, t)], width + 2 * margin,
                              height + 2 * margin)
        inside = wide[margin:margin + height, margin:margin + width]
        outside = wide.copy()
        outside[margin:margin + height, margin:margin + width] = 0
        return np.concatenate([(inside - cell).ravel(), 0.7 * outside.ravel()])

    def placed_error(p, t):
        return float(np.sum(residual(p, t) ** 2))

    if reference and not native:
        t_bounds = ([0.7 * sy, -np.inf, 0.9 * sy, -np.inf], [1.15 * sy, np.inf, 1.1 * sy, np.inf])
        # A reference glyph taller than the cell at the caps' size (Arimo's @ rises past the caps and drops below
        # the baseline; Rush 2's is cap high) shrinks to fit the cell, centered in it, rather than being clipped.
        dt, db = design[:, 1].min(), design[:, 1].max()
        if (db - dt) * 0.9 * sy > height:
            sy = 0.97 * height / (db - dt)
            oy = height / 2 - sy * (dt + db) / 2
            t_bounds = ([0.7 * sy, -np.inf, 0.9 * sy, -np.inf], [1.15 * sy, np.inf, sy, np.inf])
        # The narrowing by a coarse search first, centered on the original's ink.
        middle = (edges[0] + edges[1]) / 2 if edges else width / 2
        transform = min((np.array([f * sy, middle - f * sy * (dl + dr) / 2, sy, oy])
                         for f in (0.7, 0.78, 0.86, 0.94, 1.02, 1.1)), key=lambda t: placed_error(p0, t))

    # Coarse searches first: a shape lying on blank texels has no gradient to follow. The whole glyph by half texels,
    # then each separate piece (a colon's dots, a ? and its dot) by its own edges, the ones no other piece shares.
    shifts = np.arange(-2.0, 2.01, 0.5)
    transform = min((transform + (0, dx, 0, dy) for dx in shifts for dy in shifts),
                    key=lambda t: placed_error(p0, t))
    if reference and not native:
        transform = np.clip(transform, np.array(t_bounds[0]) + 1e-9, np.array(t_bounds[1]) - 1e-9)
    transform = least_squares(lambda t: residual(p0, t), transform, diff_step=1e-3, bounds=t_bounds).x
    if len(glyph.shapes) > 1:
        for own_x, own_y in glyph.own_edges():
            best, best_error = p0, placed_error(p0, transform)
            for dx in np.arange(-3.0, 3.01, 0.5) if own_x else (0.0,):
                for dy in np.arange(-3.0, 3.01, 0.5) if own_y else (0.0,):
                    p = p0.copy()
                    p[own_x] += dx / transform[0]
                    p[[glyph.nx + i for i in own_y]] += dy / transform[2]
                    e = placed_error(p, transform)
                    if e < best_error - 1e-9:
                        best, best_error = p, e
            p0 = best
    scale = np.concatenate([np.full(glyph.nx, transform[0]), np.full(glyph.ny, transform[2]),
                            np.full(glyph.nr + glyph.nw, 0.5 * (transform[0] + transform[2])), [4.0]])
    # A stroke's points are many and close together: held firmer, or a small cell bends the stroke into lumps.
    stroke_x, stroke_y = glyph.stroke_edges()
    scale[stroke_x] *= 4
    scale[[glyph.nx + i for i in stroke_y]] *= 4
    lower = np.concatenate([np.full(glyph.nx + glyph.ny, -np.inf), np.zeros(glyph.nr), np.full(glyph.nw, 0.2),
                            [0.2]])
    upper = np.concatenate([np.full(glyph.nx + glyph.ny, np.inf), np.full(glyph.nr + glyph.nw, 20.0), [1.0]])

    def error(p):
        return np.concatenate([residual(p, transform), pull * (p - p0) * scale])

    p = least_squares(error, np.clip(p0, lower + 1e-6, upper - 1e-6), bounds=(lower, upper), diff_step=1e-4).x
    return p, transform, float(np.sqrt(np.mean((coverage_of(p, transform) - cell) ** 2)))


def drawn_levels(font, cells, full, cap):
    """The font's cap scale and its cap top's row in a glyph rectangle, from its H (or I, or 1) with the free texels past its
    cell (drawn_region): cap is where the drawing's caps run, (top, bottom) in its own units."""
    for char in 'HI1':                                # a digits-only font measures its 1
        if char in cells:
            (x0, y0, x1, y1), sheet, (rx1, ry1) = cells[char]
            edges = ink_edges((sheet[y0:ry1, x0:rx1] / full).max(axis=1))
            if edges:
                sy = (edges[1] - edges[0]) / (cap[1] - cap[0])
                return sy, edges[0] - cap[0] * sy
    sys.exit(f'{font["name"]}: no H, I or 1 to measure the caps from')


def drawn_region(page, font, rect):
    """A glyph cell grown by the texel column right of it and the row below it where no other character's cell takes
    them. The game draws them (text is drawn in copy mode, whose rectangles include their lower right texel, so a
    cell listed exclusive shows one more column and row), and they hold the glyph's anti-aliasing there. Returns (x1,
    y1) of the grown region."""
    x0, y0, x1, y1 = rect
    others = [r for _, *r in glyph_cells(page, font) if tuple(r) != tuple(rect)]

    def free(a0, b0, a1, b1):
        if a1 > page['w'] or b1 > page['h']:
            return False
        return not any(a0 < r[2] and r[0] < a1 and b0 < r[3] and r[1] < b1 for r in others)

    rx1 = x1 + 1 if free(x1, y0, x1 + 1, y1) else x1
    ry1 = y1 + 1 if free(x0, y1, rx1, y1 + 1) else y1
    return rx1, ry1


def traced_dim(sheet, rects):
    """Traced shapes for a drawn font's shapes no drawing covers, each at its own peak intensity (a dim frame stays
    dim)."""
    elements = []
    for x0, y0, x1, y1 in rects:
        level = float(sheet[y0:y1, x0:x1].max())
        if level > 0:
            elements += traced_elements({'fill': level, 'stroke': 0.0, 'halo': 0.0}, sheet, [(x0, y0, x1, y1)])
    return elements


def drawn_design(name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, os.path.join(PACK_DIR, name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


# Rush 2049's characters past ASCII (its font tables' codes, the same in every font that has them).
ROM2049_CHARACTERS = {0x80: 'Ä', 0x82: 'Ç', 0x83: 'É', 0x84: 'Ñ', 0x85: 'Ö', 0x86: 'Ü', 0xa1: '°', 0xa8: '®', 0xa9: '©',
                      0xaa: '™', 0xc0: '¿', 0xc1: '¡', 0xe6: 'Ê', 0xe7: 'Á', 0xe8: 'Ë', 0xe9: 'È', 0xee: 'Ó', 0xef: 'Ô',
                      0xf2: 'Ú'}


def best_shift(base, cell):
    """The whole-texel move (dx, dy) that lays a base letter's texels over an accented letter's: the ROM's accented
    letters are the plain letter's texels exactly, with the mark added."""
    h, w = cell.shape
    ys, xs = np.nonzero(base > 0.02)

    def error(dx, dy):
        y, x = ys + dy, xs + dx
        ok = (y >= 0) & (y < h) & (x >= 0) & (x < w)
        return (float(np.sum((cell[y[ok], x[ok]] - base[ys[ok], xs[ok]]) ** 2)) +
                4 * float(np.sum(base[ys[~ok], xs[~ok]] ** 2)))
    return min(((dx, dy) for dx in range(-3, 4) for dy in range(-4, 5)), key=lambda s: error(*s))


def accent_shapes(mark, rest):
    """Native shapes (cell texels) for a combining mark, started on the ink the base letter leaves (rest)."""
    from scipy import ndimage
    labels, count = ndimage.label(rest > 0.25)
    parts = sorted((s for s in ndimage.find_objects(labels) if s and rest[s].max() >= 0.5), key=lambda s: s[1].start)
    if not parts:
        return []
    if mark == '̈' and len(parts) == 1 and parts[0][1].stop - parts[0][1].start >= 3:
        # The dots' anti-aliasing joins them: split at the faintest column between them.
        ys, xs = parts[0]
        columns = rest[ys, xs].sum(axis=0)
        cut = xs.start + 1 + int(np.argmin(columns[1:-1]))
        parts = [(ys, slice(xs.start, cut)), (ys, slice(cut + 1, xs.stop))]
    if mark == '̈' or len(parts) > 1 and mark != '̂':
        # Diaeresis (and anything in pieces): a square dot per piece, on the piece's ink center with its ink's area.
        # Placed, not fitted: the small fonts' dots are a texel apart, and a fit fills the faint gap into a bar.
        dots = []
        for ys, xs in parts:
            ink = rest[ys, xs]
            area = max(float((ink >= 0.5).sum()), 1.0)      # its solid texels; the anti-aliasing would merge them
            gy, gx = np.mgrid[ys, xs]
            cx, cy = float((gx * ink).sum() / ink.sum()) + 0.5, float((gy * ink).sum() / ink.sum()) + 0.5
            half = min(np.sqrt(area), xs.stop - xs.start + 0.5) / 2
            dots.append(('p', [(cx - half, cy - half, 0), (cx + half, cy - half, 0), (cx + half, cy + half, 0),
                               (cx - half, cy + half, 0)]))
        return dots
    y0, y1 = min(s[0].start for s in parts), max(s[0].stop for s in parts)
    x0, x1 = min(s[1].start for s in parts), max(s[1].stop for s in parts)
    width = float(np.clip(rest[y0:y1, x0:x1].sum() / max(np.hypot(x1 - x0, y1 - y0), 1.0), 0.6, 2.0))
    hw = width / 2
    if mark == '́':        # acute
        return [('s', [(x0 + hw, y1 - hw * 0.5), (x1 - hw, y0 + hw * 0.5)], width, 'h')]
    if mark == '̀':        # grave
        return [('s', [(x0 + hw, y0 + hw * 0.5), (x1 - hw, y1 - hw * 0.5)], width, 'h')]
    if mark == '̂':        # circumflex
        mid = (x0 + x1) / 2
        return [('s', [(x0 + hw, y1 - hw * 0.5), (mid, y0 + hw), (x1 - hw, y1 - hw * 0.5)], width, 'h')]
    if mark == '̃':        # tilde
        top, bottom = y0 + hw, y1 - hw
        return [('s', [(x0 + hw, bottom), (x0 + (x1 - x0) * 0.33, top), (x0 + (x1 - x0) * 0.67, bottom),
                       (x1 - hw, top)], width, 'ps')]
    if mark == '̧':        # cedilla: a short tail down and to the left, as 2049 draws it
        return [('s', [(x1 - hw, y0 + hw * 0.5), (x0 + hw, y1 - hw * 0.5)], max(width, 1.2), 'h')]
    return []


def symbol_shapes(letter, cols, rows):
    """(R) or (C) as native shapes: a ring over the ink's extent (cols, rows: its left/right and top/bottom edges)
    round the design's letter scaled to half the ring's height."""
    (left, right), (top, bottom) = cols, rows
    cx, cy, rx, ry = (left + right) / 2, (top + bottom) / 2, (right - left) / 2, (bottom - top) / 2
    width = max(0.6, 0.12 * (rx + ry))
    angles = np.linspace(0, 2 * np.pi, 9)
    ring = [(cx + (rx - width / 2) * np.cos(a), cy + (ry - width / 2) * np.sin(a)) for a in angles]
    cap_top, cap_bottom = DRAWN_CAP[0]
    k = 0.5 * 2 * ry / (cap_bottom - cap_top)
    xs = [pt[0] for shape in letter if shape[0] != 'f' for pt in shape[1]]
    ox = cx - k * (min(xs) + max(xs)) / 2
    oy = cy - k * (cap_top + cap_bottom) / 2
    return [('s', ring, width, 'ps')] + map_shapes(letter, k, ox, k, oy)


def map_shapes(shapes, kx, ox, ky, oy):
    """Drawn shapes moved by x' = ox + kx x, y' = oy + ky y (both k negative turns them over)."""
    k = (abs(kx) + abs(ky)) / 2

    def one(shape):
        if shape[0] in 'ph':
            return (shape[0], [(ox + kx * x, oy + ky * y, tuple(k * v for v in r) if isinstance(r, tuple) else k * r)
                               for x, y, r in shape[1]])
        return (shape[0], [(ox + kx * x, oy + ky * y) for x, y in shape[1]], k * shape[2], shape[3])
    return [one(s) for s in shapes if s[0] != 'f']


def turned_shapes(letter, cols, rows):
    """A design letter turned upside down onto the ink's extent (cols, rows: left/right, top/bottom edges)."""
    cap_top, cap_bottom = DRAWN_CAP[0]
    xs = [pt[0] for shape in letter if shape[0] != 'f' for pt in shape[1]]
    ky = -(rows[1] - rows[0]) / (cap_bottom - cap_top)
    kx = ky * 1.0
    # The ink's middle on the design's middle, turned.
    ox = (cols[0] + cols[1]) / 2 - kx * (min(xs) + max(xs)) / 2
    oy = (rows[0] + rows[1]) / 2 - ky * (cap_top + cap_bottom) / 2
    return map_shapes(letter, kx, ox, ky, oy)


def fit_rom2049_character(char, cell, fitted, levels, pull, design_glyphs):
    """(SVG path in cell coordinates, rms error) for one of 2049's characters past ASCII. An accented letter is the
    base letter's own fitted drawing, moved to where the ROM's texels put it, and its mark fitted to the texels the
    letter leaves; the symbols (and the base letter when it has no fit) are the reference font's."""
    import unicodedata
    parts = unicodedata.normalize('NFD', char)
    if len(parts) == 2 and parts[0] in fitted:
        glyph, p, transform, base = fitted[parts[0]]
        dx, dy = best_shift(base, cell)
        moved = transform + (0, dx, 0, dy)
        placed = np.zeros_like(cell)
        h, w = cell.shape
        ys, xs = np.nonzero(base)
        y, x = ys + dy, xs + dx
        ok = (y >= 0) & (y < h) & (x >= 0) & (x < w)
        placed[y[ok], x[ok]] = base[ys[ok], xs[ok]]
        # The mark's ink: texels the letter doesn't touch (its edges' anti-aliasing differs a little).
        rest = np.where(placed < 0.1, cell, 0.0)
        # Marks sit above or below the letter: ink beside it (a grown edge column's texel) is no mark.
        letter_rows = np.nonzero(placed.max(axis=1) >= 0.25)[0]
        if len(letter_rows):
            rest[letter_rows[0] + 1:letter_rows[-1]] = 0
        contours = [pts for pts, _ in glyph.outline(p, moved)]
        d = glyph.svg(p, moved)
        shapes = accent_shapes(parts[1], rest)
        if shapes:
            mark = DrawnGlyph(shapes)
            # Placed on the mark's ink, not fitted: a mark is a few texels, and a fit shrinks or slides it off.
            q, t = mark.initial, (1.0, 0.0, 1.0, 0.0)
            contours += [pts for pts, _ in mark.outline(q, t)]
            d += ' ' + mark.svg(q, t)
        rms = float(np.sqrt(np.mean((np.clip(drawn_coverage(contours, w, h), 0, 1) - cell) ** 2)))
        return d, rms
    if char in '®©' and ('R' if char == '®' else 'C') in design_glyphs:
        # A ring round the font's own letter, both in the cell's texels.
        cols, rows = ink_edges(cell.max(axis=0)), ink_edges(cell.max(axis=1))
        if cols and rows:
            glyph = DrawnGlyph(symbol_shapes(design_glyphs['R' if char == '®' else 'C'], cols, rows))
            p, transform, rms = fit_drawn_glyph(glyph, cell, levels, pull, native=True)
            return glyph.svg(p, transform), rms
    if char == '™' and 'T' in design_glyphs and 'M' in design_glyphs:
        # The font's own T and M, small, over the ink's extent.
        cols, rows = ink_edges(cell.max(axis=0)), ink_edges(cell.max(axis=1))
        if cols and rows:
            cap_top, cap_bottom = DRAWN_CAP[0]
            k = (rows[1] - rows[0]) / (cap_bottom - cap_top)
            shapes, x = [], cols[0]
            for letter in (design_glyphs['T'], design_glyphs['M']):
                xs = [pt[0] for shape in letter if shape[0] != 'f' for pt in shape[1]]
                shapes += map_shapes(letter, k, x - k * min(xs), k, rows[0] - k * cap_top)
                x += k * (max(xs) - min(xs)) + 0.5
            # Squeezed or spread to the ink's width.
            sx = (cols[1] - cols[0]) / max(x - 0.5 - cols[0], 0.5)
            glyph = DrawnGlyph(map_shapes(shapes, sx, cols[0] * (1 - sx), 1.0, 0.0))
            p, transform, rms = fit_drawn_glyph(glyph, cell, levels, pull, native=True)
            return glyph.svg(p, transform), rms
    if char in '¿¡' and ('?' if char == '¿' else '!') in design_glyphs:
        # The font's own ? and ! turned over.
        cols, rows = ink_edges(cell.max(axis=0)), ink_edges(cell.max(axis=1))
        if cols and rows:
            glyph = DrawnGlyph(turned_shapes(design_glyphs['?' if char == '¿' else '!'], cols, rows))
            p, transform, rms = fit_drawn_glyph(glyph, cell, levels, pull, native=True)
            return glyph.svg(p, transform), rms
    glyph = DrawnGlyph([('f', char)])
    p, transform, rms = fit_drawn_glyph(glyph, cell, levels, pull)
    return glyph.svg(p, transform), rms


def drawn_elements(font_id, font, sheet_list, only=None):
    """Each sheet's glyph elements for a drawn font, fitted to its texels, and a summary of the fit. The design's
    NATIVE drawings for the font (by its id in fonts.json) replace its shared ones."""
    design = drawn_design(font['design'])
    glyphs = design.GLYPHS
    DRAWN_CAP[0] = getattr(design, 'CAP', (-0.15, 11.15))
    natives = getattr(design, 'NATIVE', {}).get(font_id, {})
    full = max(float(sheet.max()) for _, sheet in sheet_list)
    cells = {}
    for page, sheet in sheet_list:
        for code, x0, y0, x1, y1 in glyph_cells(page, font):
            if 32 < code < 127:
                cells[chr(code)] = ((x0, y0, x1, y1), sheet, drawn_region(page, font, (x0, y0, x1, y1)))
    levels = drawn_levels(font, cells, full, DRAWN_CAP[0])
    pull = font.get('pull', DRAWN_PULL)
    per_sheet, errors = [], []
    fitted = {}     # Each letter's fit (glyph, parameters, transform, texels), for its accented forms.
    rom2049 = font_id.startswith('rush2049')
    for page, sheet in sheet_list:
        elements = []
        # 2049's accented letters after the plain ones, which they're built from.
        for code, x0, y0, x1, y1 in sorted(glyph_cells(page, font), key=lambda c: rom2049 and c[0] in ROM2049_CHARACTERS):
            if rom2049 and code in ROM2049_CHARACTERS:
                char = ROM2049_CHARACTERS[code]
                if only and char not in only:
                    continue
                rx1, ry1 = drawn_region(page, font, (x0, y0, x1, y1))
                cell = sheet[y0:ry1, x0:rx1] / full
                d, rms = fit_rom2049_character(char, cell, fitted, levels, pull, glyphs)
                if rms * full > 2.0 and char in '°®©™¿¡':
                    # No drawing fits the symbol (font 12's (R) and (C) are solid boxes with the letter cut out):
                    # traced. Letters are always drawn: their base is the letter's own fit.
                    elements += traced_dim(sheet, [(x0, y0, rx1, ry1)])
                    print(f'  {font["name"]} {char}: drawn rms {rms * full:.2f}, traced instead')
                    continue
                errors.append((rms, char))
                d = re.sub(r'(-?[\d.]+),(-?[\d.]+)', lambda m: fmt((float(m.group(1)) + x0, float(m.group(2)) + y0)), d)
                elements.append({'d': d, 'rule': 'nonzero', 'fill': full, 'stroke': 0.0, 'halo': 0.0,
                                 'clip': (x0, y0, rx1, ry1)})
                continue
            char = chr(code) if code != 32 and code < 127 else None  # with the special characters below 32
            if only and (char is None or char not in only):
                continue
            if char not in glyphs and char not in natives:
                # Shapes no drawing covers (the cursor, (c), (r), TM): traced from their own texels.
                rx1, ry1 = drawn_region(page, font, (x0, y0, x1, y1))
                elements += traced_dim(sheet, [(x0, y0, rx1, ry1)])
                continue
            rx1, ry1 = drawn_region(page, font, (x0, y0, x1, y1))
            cell = sheet[y0:ry1, x0:rx1] / full
            if not cell.any():
                continue
            glyph = DrawnGlyph(natives.get(char) or glyphs[char])
            p, transform, rms = fit_drawn_glyph(glyph, cell, levels, pull, char in natives)
            errors.append((rms, char))
            fitted[char] = (glyph, p, transform, cell)
            d = re.sub(r'(-?[\d.]+),(-?[\d.]+)', lambda m: fmt((float(m.group(1)) + x0, float(m.group(2)) + y0)),
                       glyph.svg(p, transform))
            # Clipped to what the game draws: text is drawn in copy mode, whose rectangles include their lower right
            # texel, so a glyph shows one column and row past its listed cell (free sheet texels, drawn_region).
            elements.append({'d': d, 'rule': 'nonzero', 'fill': full, 'stroke': 0.0, 'halo': 0.0,
                             'clip': (x0, y0, rx1, ry1)})
        # Ink no character's cell holds (other code might draw it): traced, as the other styles do, except marks a
        # font names under "extra" ({sheet hash: {char: [x0, y0, x1, y1]}}: Medium's free-standing TM), which are
        # drawn as the design draws that character (or fit_rom2049_character's symbols when it doesn't).
        if not only:
            extra = font.get('extra', {}).get(rt64_hash(page), {})
            for char, (ex0, ey0, ex1, ey1) in extra.items():
                cell = sheet[ey0:ey1, ex0:ex1] / full
                code = {v: k for k, v in SPECIAL_CHARACTERS.items()}.get(char, ord(char))
                shapes = natives.get(chr(code)) or glyphs.get(chr(code))
                if shapes:      # the design's own drawing of it (Rush 2's TM: the reference font's)
                    glyph = DrawnGlyph(shapes)
                    p, transform, rms = fit_drawn_glyph(glyph, cell, levels, pull, chr(code) in natives)
                    d = glyph.svg(p, transform)
                else:
                    d, rms = fit_rom2049_character(char, cell, {}, levels, pull, glyphs)
                errors.append((rms, char))
                d = re.sub(r'(-?[\d.]+),(-?[\d.]+)', lambda m: fmt((float(m.group(1)) + ex0, float(m.group(2)) + ey0)), d)
                elements.append({'d': d, 'rule': 'nonzero', 'fill': full, 'stroke': 0.0, 'halo': 0.0,
                                 'clip': (ex0, ey0, ex1, ey1)})
            elements += traced_dim(sheet, [r for r in uncovered_regions(page, sheet, font)
                                           if not any(r[0] < e[2] and e[0] < r[2] and r[1] < e[3] and e[1] < r[3]
                                                      for e in extra.values())])
        per_sheet.append(elements)
    errors.sort(reverse=True)
    worst = ' '.join(f'{c}:{e * full:.2f}' for e, c in errors[:8])
    mean = float(np.sqrt(np.mean([e * e for e, _ in errors]))) * full if errors else 0.0
    return per_sheet, f'drawn ({font["design"]}), rms error {mean:.2f} of {full:.0f} levels, worst {worst}'


# ---------------------------------------------------------------------------------------------------------------------
# Synthesized fonts
#
# Rush 2049 Menus shows menu text at the size its buttons set and in the lettering its Fonts switch sets
# (src/rush2049/menus2049.cpp). Two of those combinations have no font of their own, so they're drawn here: one game's
# lettering (its drawn design) at the other game's font sizes, each glyph's capitals on the target font's cap rows
# (measured on its H), in uniform scale, its left ink edge on the cell's left texel. `synth` writes each glyph's
# coverage (assets/menu_fonts_synth.json, which the game packs into its sheets like the 2049 fonts) and outline
# (tools/font_pack/synth.json, for the high-resolution sheets: style "synth" draws them in the packed cells exactly).

SYNTH_ASSET = os.path.join(ROOT, 'assets', 'menu_fonts_synth.json')
SYNTH_OUTLINES = os.path.join(PACK_DIR, 'synth.json')
# (id, design, target (tables, font id), header (line height, space, gap, ascent, descent)). The 2049 headers are its
# glyph tables' bytes 5, 7, 8, 2 and 3 (fonts 12, 11, 10, 13); Rush 2's space is its font record's byte 5, its gap
# the record's own spacing (byte 6), which the swap keeps.
SYNTH_FONTS = [
    ('rush2_at_aero_8', 'rush2_glyphs', ('menu', 'rush2049_menu_8'), (9, 2, 1, 7, 2)),
    ('rush2_at_aero_9', 'rush2_glyphs', ('menu', 'rush2049_menu_9'), (12, 3, 1, 10, 2)),
    ('rush2_at_aero_10', 'rush2_glyphs', ('menu', 'rush2049_menu_10'), (15, 3, 1, 12, 3)),
    ('rush2_at_aero_title', 'rush2_glyphs', ('menu', 'rush2049_menu_title'), (19, 4, 1, 15, 4)),
    ('aero_at_rush2_8', 'aero_glyphs', ('rush2', '8'), (8, 6, 1, 8, 0)),
    ('aero_at_rush2_9', 'aero_glyphs', ('rush2', '9'), (10, 6, 1, 10, 0)),
    ('aero_at_rush2_10', 'aero_glyphs', ('rush2', '10'), (12, 7, 1, 12, 0)),
]


def target_caps(tables, font_id):
    """A font's capitals' top and bottom rows (sub-texel, from its line's top), from a font tables dump: its H's
    rows, as drawn_levels measures them; and its H's cell height, which the menus center rows by (cap_of in
    src/rush2049/menus2049.cpp)."""
    fonts = json.load(open(FONT_LIST, encoding='utf-8'))
    font = fonts[font_id]
    table = next(t for t in tables if str(t['id']) == font_id)
    for page in table['pages']:
        sheet = decode_sheet(page)
        full = float(sheet.max()) or 1.0
        for code, x0, y0, x1, y1 in glyph_cells(page, font):
            if code == ord('H'):
                return ink_edges((sheet[y0:y1, x0:x1] / full).max(axis=1)), y1 - y0
    sys.exit(f'{font_id}: no H to measure the caps from')


SYNTH_INK_RES = 16  # Samples a texel measuring a synthesized glyph's ink width.


def synth_font(design_name, caps, cell):
    """Each character the design draws (0x16-0x7E; the game shows lower case as capitals), its capitals on rows caps:
    {code: (width, height, coverage 0-255 row major, SVG path in cell coordinates)}. Cells are at least `cell` tall
    (the target font's H cell), so a row is centered on the same cell as in the target font."""
    design = drawn_design(design_name)
    cap = getattr(design, 'CAP', (-0.15, 11.15))
    DRAWN_CAP[0] = cap
    sy = (caps[1] - caps[0]) / (cap[1] - cap[0])
    oy = caps[0] - cap[0] * sy
    out = {}
    for code in range(0x16, 0x7F):
        char = chr(code)
        if char.islower() or char == ' ' or char not in design.GLYPHS:
            continue
        glyph = DrawnGlyph(design.GLYPHS[char])
        p = glyph.initial
        pts = np.concatenate([c for c, _ in glyph.outline(p, (sy, 0.0, sy, oy))])
        left, right, bottom = float(pts[:, 0].min()), float(pts[:, 0].max()), float(pts[:, 1].max())
        h = max(cell, int(np.ceil(bottom - 1e-6)))
        # The cell spans the ink as drawn (columns at least half covered at SYNTH_INK_RES samples a texel), rounded to
        # whole texels: rounding the outline's box up left letters ending just past a texel line (T, A) almost a
        # texel of extra space, and R's box reached a column its ink never does. The overhang (under half a texel)
        # falls in the gap column, which the game draws with the glyph (copy mode includes the cell's x1).
        n = SYNTH_INK_RES
        fine = drawn_coverage([c * n for c, _ in glyph.outline(p, (sy, -left, sy, oy))],
                              int(np.ceil(right - left)) * n + n, h * n)
        cols = np.nonzero(fine.max(axis=0) >= 0.5)[0]
        if len(cols):
            left += cols[0] / n
            right = left + (cols[-1] + 1 - cols[0]) / n
        transform = (sy, -left, sy, oy)
        w = max(1, int(round(right - left)))
        cov = np.clip(drawn_coverage([c for c, _ in glyph.outline(p, transform)], w, h), 0.0, 1.0)
        out[code] = (w, h, [int(round(v * 255)) for v in cov.ravel()], glyph.svg(p, transform))
    return out


def synth(r2_tables_path, menu_tables_path):
    tables = {'rush2': json.load(open(r2_tables_path)), 'menu': json.load(open(menu_tables_path))}
    asset, outlines = {}, {}
    for font_id, design, (source, target), (line, space, gap, ascent, descent) in SYNTH_FONTS:
        caps, cell = target_caps(tables[source], target)
        glyphs = synth_font(design, caps, cell)
        asset[font_id] = {'line_height': line, 'space': space, 'gap': gap, 'ascent': ascent, 'descent': descent,
                          'glyphs': {str(c): {'w': w, 'h': h, 'coverage': bytes(cov).hex()}
                                     for c, (w, h, cov, _) in glyphs.items()}}
        outlines[font_id] = {str(c): d for c, (_, _, _, d) in glyphs.items()}
        print(f'{font_id}: {design} on {target}\'s caps {caps[0]:.2f}-{caps[1]:.2f}, {len(glyphs)} glyphs')
    with open(SYNTH_ASSET, 'w', newline='\n') as f:
        json.dump(asset, f, separators=(',', ':'))
    with open(SYNTH_OUTLINES, 'w', newline='\n') as f:
        json.dump(outlines, f, indent=1)


def synth_elements(font_id, font, sheet_list):
    """A synthesized font's sheets: each packed cell gets its glyph's own outline (synth.json), clipped to the cell
    and the free texels past it the game draws (drawn_region)."""
    outlines = json.load(open(SYNTH_OUTLINES))[font['synth']]
    full = max(float(sheet.max()) for _, sheet in sheet_list) or 15.0
    per_sheet = []
    for page, sheet in sheet_list:
        elements = []
        for code, x0, y0, x1, y1 in glyph_cells(page, font):
            d = outlines.get(str(code))
            if d is None:
                continue
            rx1, ry1 = drawn_region(page, font, (x0, y0, x1, y1))
            d = re.sub(r'(-?[\d.]+),(-?[\d.]+)', lambda m: fmt((float(m.group(1)) + x0, float(m.group(2)) + y0)), d)
            elements.append({'d': d, 'rule': 'nonzero', 'fill': full, 'stroke': 0.0, 'halo': 0.0,
                             'clip': (x0, y0, rx1, ry1)})
        per_sheet.append(elements)
    return per_sheet, f'synthesized ({len(outlines)} glyphs)'


# ---------------------------------------------------------------------------------------------------------------------
# Rendering
#
# An element is one SVG path: {'d', 'rule', 'fill', 'stroke', 'halo', 'clip'} with fill and stroke as intensities
# (0-15), halo as the visible stroke width outside the fill (half the SVG stroke-width), and clip as a cell rectangle.


def fill(polygons, rule, rows, cols):
    """Scanline fill of polygons (in sample units) with the nonzero or even-odd rule, sampling at sample centers.

    Vector glyphs need a real nonzero fill: Inter draws some letters as one self-overlapping contour.
    """
    covered = np.zeros((rows, cols), bool)
    segments = [np.stack([p, np.roll(p, -1, axis=0)], axis=1) for p in polygons]
    if not segments:
        return covered
    seg = np.concatenate(segments)
    seg = seg[seg[:, 0, 1] != seg[:, 1, 1]]  # Horizontal edges never cross a scanline.
    x0, y0, x1, y1 = seg[:, 0, 0], seg[:, 0, 1], seg[:, 1, 0], seg[:, 1, 1]
    direction = np.where(y1 > y0, 1, -1)
    top, bottom = np.minimum(y0, y1), np.maximum(y0, y1)
    first, last = max(int(top.min()) - 1, 0), min(int(bottom.max()) + 1, rows)
    for row in range(first, last):
        y = row + 0.5
        active = (top <= y) & (y < bottom)
        if not active.any():
            continue
        xs = x0[active] + (y - y0[active]) * (x1[active] - x0[active]) / (y1[active] - y0[active])
        order = np.argsort(xs)
        xs, dirs = xs[order], direction[active][order]
        winding = np.cumsum(dirs) if rule == 'nonzero' else np.cumsum(np.ones_like(dirs))
        inside = (winding != 0) if rule == 'nonzero' else (winding % 2 == 1)
        for i in np.nonzero(inside)[0]:
            start, end = int(np.ceil(xs[i] - 0.5)), int(np.ceil(xs[i + 1] - 0.5))
            covered[row, max(start, 0):min(end, cols)] = True
    return covered


def shape(element, width, height, res):
    """An element's fill mask and each sample's distance outside it (in texels) over its clip rectangle, at res
    samples per texel, and that rectangle. The distance accounts for the shape beyond the clip rectangle."""
    x0, y0, x1, y1 = element['clip'] or (0, 0, width, height)
    margin = int(np.ceil(max(FIT_HALO_WIDTHS))) + 1
    ox, oy = x0 - margin, y0 - margin
    rows, cols = (y1 - y0 + 2 * margin) * res, (x1 - x0 + 2 * margin) * res
    inside = fill([(p - (ox, oy)) * res for p in parse_path(element['d'])], element['rule'], rows, cols)
    distance = cv2.distanceTransform((~inside).astype(np.uint8), cv2.DIST_L2, 5) / res
    crop = (slice(margin * res, rows - margin * res), slice(margin * res, cols - margin * res))
    return inside[crop], distance[crop], (x0, y0, x1, y1)


def render(width, height, elements, res):
    """Paints the elements in order and returns intensities (0-15) at res samples per texel."""
    image = np.zeros((height * res, width * res), np.float32)
    for element in elements:
        inside, distance, (x0, y0, x1, y1) = shape(element, width, height, res)
        target = image[y0 * res:y1 * res, x0 * res:x1 * res]
        if element['stroke']:
            target[~inside & (distance <= element['halo'])] = element['stroke']
        target[inside] = element['fill']
    return image


def texel_average(image, res):
    h, w = image.shape
    return image.reshape(h // res, res, w // res, res).mean(axis=(1, 3))


# ---------------------------------------------------------------------------------------------------------------------
# Vector glyphs


class SvgPen(BasePen):
    """Collects a glyph's outline as SVG path data, mapping font units through (sx, sy, dx, dy) (y flipped), or an
    affine (a, b, c, d, e, f) giving (a x + b y + e, c x + d y + f)."""

    def __init__(self, glyph_set, transform):
        super().__init__(glyph_set)
        if len(transform) == 4:
            sx, sy, dx, dy = transform
            transform = (sx, 0, 0, -sy, dx, dy)
        self.transform = transform
        self.commands = []

    def map(self, p):
        a, b, c, d, e, f = self.transform
        return fmt((a * p[0] + b * p[1] + e, c * p[0] + d * p[1] + f))

    def _moveTo(self, p):
        self.commands.append('M' + self.map(p))

    def _lineTo(self, p):
        self.commands.append('L' + self.map(p))

    def _qCurveToOne(self, c, p):
        self.commands.append(f'Q{self.map(c)} {self.map(p)}')

    def _curveToOne(self, c1, c2, p):
        self.commands.append(f'C{self.map(c1)} {self.map(c2)} {self.map(p)}')

    def _closePath(self):
        self.commands.append('Z')


class VectorFont:
    _cache = {}

    def __init__(self, weight, optical_size):
        if 'font' not in VectorFont._cache:
            VectorFont._cache['font'] = TTFont(VECTOR_FONT)
        font = VectorFont._cache['font']
        self.cmap = font.getBestCmap()
        self.glyphs = font.getGlyphSet(location={'wght': weight, 'opsz': optical_size})
        self.bounds_cache = {}

    def name(self, char):
        return self.cmap.get(ord(char))

    def bounds(self, char):
        if char not in self.bounds_cache:
            pen = BoundsPen(self.glyphs)
            self.glyphs[self.name(char)].draw(pen)
            self.bounds_cache[char] = pen.bounds
        return self.bounds_cache[char]

    def path(self, char, transform):
        pen = SvgPen(self.glyphs, transform)
        self.glyphs[self.name(char)].draw(pen)
        return ' '.join(pen.commands)


def vector_char(code, font):
    """The Unicode character to draw for a game character code, or None to trace it."""
    char = SPECIAL_CHARACTERS.get(code, chr(code) if 33 <= code <= 126 else None)
    if char is None or code in font.get('trace', []):
        return None
    return char


def vector_paths(vector, cells, inset):
    """Path data placing each vector glyph over its original's ink box shrunk by inset texels.

    One scale per font keeps glyphs consistent; each glyph may stretch a little to match its original's proportions.
    """
    fits = []
    for char, rect, (x0, y0, x1, y1) in cells:
        box = (x0 + inset, y0 + inset, max(x1 - inset, x0 + inset + 0.25), max(y1 - inset, y0 + inset + 0.25))
        bx0, by0, bx1, by1 = vector.bounds(char)
        fits.append((char, rect, box, (bx0, by0, bx1, by1), (box[3] - box[1]) / max(by1 - by0, 1)))
    letters = [f[4] for f in fits if f[0].isalnum()]
    scale = float(np.median(letters if letters else [f[4] for f in fits]))
    paths = []
    for char, rect, (x0, y0, x1, y1), (bx0, by0, bx1, by1), _ in fits:
        sx = np.clip((x1 - x0) / max(bx1 - bx0, 1), scale * 0.75, scale * 1.3)
        sy = np.clip((y1 - y0) / max(by1 - by0, 1), scale * 0.85, scale * 1.15)
        # Center the glyph's bounds on the box (font y points up, SVG y points down).
        dx = (x0 + x1) / 2 - (bx0 + bx1) / 2 * sx
        dy = (y0 + y1) / 2 + (by0 + by1) / 2 * sy
        paths.append((rect, vector.path(char, (sx, sy, dx, dy))))
    return paths


def collect_cells(font, sheets, char_for):
    """Cells drawn from glyphs as (char, rect, ink box) and traced cells as rect, per sheet. char_for maps a game
    character code to the glyph to draw, or None to trace the cell."""
    collected = []
    for page, sheet in sheets:
        vector_cells, traced_cells = [], []
        for code, x0, y0, x1, y1 in glyph_cells(page, font):
            box = ink_box(sheet[y0:y1, x0:x1])
            if box is None:
                continue
            rect = (x0, y0, x1, y1)
            char = char_for(code, font)
            if char is not None:
                vector_cells.append((char, rect, (box[0] + x0, box[1] + y0, box[2] + x0, box[3] + y0)))
            else:
                traced_cells.append(rect)
        collected.append((page, sheet, vector_cells, traced_cells, uncovered_regions(page, sheet, font)))
    return collected


def uncovered_regions(page, sheet, font):
    """Rectangles around ink no character uses. Other code might still draw it, so it's traced rather than dropped.

    Ink within two texels of a character's cell is that character's own halo spilling past its rectangle, which the
    redrawn glyph replaces, so it's left out: traced, it showed as ghost edges beside the letters.
    """
    ink = (sheet >= INK_FLOOR).astype(np.uint8)
    for _, x0, y0, x1, y1 in glyph_cells(page, font):
        ink[max(y0 - 2, 0):y1 + 2, max(x0 - 2, 0):x1 + 2] = 0
    count, _, stats, _ = cv2.connectedComponentsWithStats(ink, connectivity=8)
    regions = []
    for x, y, w, h, area in stats[1:count]:
        if area >= 2:  # Single texels are noise.
            regions.append((max(x - 1, 0), max(y - 1, 0), min(x + w + 1, page['w']), min(y + h + 1, page['h'])))
    return regions


def fit_style(font, collected, weight, inset):
    """Least-squares (error, halo width, fill, stroke) for the font's vector glyphs at a weight and inset.

    Averaged down to texels, a glyph is fill * (fill coverage) + stroke * (halo coverage), so for each halo width the
    two intensities are a linear fit against the original texels.
    """
    vector = VectorFont(weight, font['optical_size'])
    originals, core_cover, halo_cover = [], [], {w: [] for w in FIT_HALO_WIDTHS}
    for page, sheet, vector_cells, _, _ in collected:
        for rect, d in vector_paths(vector, vector_cells, inset):
            x0, y0, x1, y1 = rect
            originals.append(sheet[y0:y1, x0:x1].ravel())
            inside, distance, _ = shape({'d': d, 'rule': 'nonzero', 'clip': rect}, page['w'], page['h'], FIT_RES)
            core_cover.append(texel_average(inside.astype(np.float32), FIT_RES).ravel())
            for w in FIT_HALO_WIDTHS:
                halo = (~inside & (distance <= w)).astype(np.float32)
                halo_cover[w].append(texel_average(halo, FIT_RES).ravel())
    original = np.concatenate(originals)
    core = np.concatenate(core_cover)
    # The core keeps the font's full intensity (fitting it too would dim the text to make up for shape differences).
    fill_level = float(np.percentile(original[original >= INK_FLOOR], 95))
    best = None
    for w in FIT_HALO_WIDTHS:
        ring = np.concatenate(halo_cover[w])
        residual = original - fill_level * core
        stroke_level = float(np.clip(np.dot(ring, residual) / max(np.dot(ring, ring), 1e-9), 0, fill_level))
        error = float(np.mean((residual - stroke_level * ring) ** 2))
        if best is None or error < best[0]:
            best = (error, w, fill_level, stroke_level)
    return best


def choose_style(font, collected):
    """Searches weights and insets for the redraw that best reproduces the font's sheets."""
    results = {}

    def evaluate(weight, inset):
        if (weight, inset) not in results:
            results[(weight, inset)] = fit_style(font, collected, weight, inset)

    for weight in FIT_WEIGHTS:
        for inset in FIT_INSETS:
            evaluate(weight, inset)
    _, weight, inset = min((r[0], w, i) for (w, i), r in results.items())
    for step in (50, 25):  # Refine the weight around the best one.
        for candidate in (weight - step, weight + step):
            if 100 <= candidate <= 900:
                evaluate(candidate, inset)
        _, weight, inset = min((r[0], w, i) for (w, i), r in results.items() if i == inset)
    error, halo, fill_level, stroke_level = results[(weight, inset)]

    # The best match usually has the halo reaching past the ink box, where the cell cuts it off flat (the original's
    # outermost texels mix the core's edge with the halo). Move the outlines in until the halo fits, keeping the fitted
    # weight and halo: refitting either at the smaller size thins the letters well below the originals' boldness.
    return {'weight': weight, 'inset': max(inset, halo), 'halo': halo, 'fill': fill_level, 'stroke': stroke_level,
            'error': error}


def vector_elements(font, style, sheet, vector_cells, traced_cells, uncovered):
    vector = VectorFont(style['weight'], font['optical_size'])
    look = {'fill': style['fill'], 'stroke': style['stroke'], 'halo': style['halo']}
    elements = traced_elements(style, sheet, uncovered)
    elements += traced_elements(style, sheet, traced_cells, clear=True)
    for rect, d in vector_paths(vector, vector_cells, style['inset']):
        elements += [clear_element(rect), dict(look, d=d, rule='nonzero', clip=rect)]
    return elements


def clear_element(rect):
    """Black over a character's whole cell, so traced ink around other characters never shows inside it."""
    x0, y0, x1, y1 = rect
    return {'d': f'M{x0},{y0} L{x1},{y0} L{x1},{y1} L{x0},{y1} Z', 'rule': 'nonzero', 'fill': 0, 'stroke': 0,
            'halo': 0, 'clip': rect}


def traced_elements(style, sheet, rects, squared=False, clear=False):
    """Traced shapes in rects, contoured between the halo and core intensities, with the font's fill and halo.
    Squared fonts keep every corner sharp. clear blanks each rect first (for character cells)."""
    look = {'fill': style['fill'], 'stroke': style['stroke'], 'halo': style['halo']}
    threshold = (style['fill'] + style['stroke']) / 2
    elements = []
    for x0, y0, x1, y1 in rects:
        polygons = trace_smooth(sheet[y0:y1, x0:x1], threshold)
        if polygons:
            to_path = polygons_path if squared else lambda ps: ' '.join(smooth_path(p) for p in ps)
            d = to_path([p + (x0, y0) for p in polygons])
            if clear:
                elements.append(clear_element((x0, y0, x1, y1)))
            elements.append(dict(look, d=d, rule='evenodd', clip=(x0, y0, x1, y1)))
    return elements


# ---------------------------------------------------------------------------------------------------------------------
# Block glyphs
#
# The squared fonts are too small to trace or match to a vector font (some glyphs are 2x4 texels), so they're drawn
# from this hand-made squared font instead: each character is polylines on a grid WIDTH units wide and 4 tall (0 top,
# 2 middle, 4 bottom), separated by ';'. A width of 0 centers the glyph. Lowercase uses the capitals, as the game's
# squared fonts do.

BLOCK_GLYPHS = {
    '0': (2, '0,0 2,0 2,4 0,4 0,0; 2,0 0,4'),
    '1': (1, '0,1 1,0 1,4'),
    '2': (2, '0,0 2,0 2,2 0,2 0,4 2,4'),
    '3': (2, '0,0 2,0 2,4 0,4; 0,2 2,2'),
    '4': (2, '0,0 0,2 2,2; 2,0 2,4'),
    '5': (2, '2,0 0,0 0,2 2,2 2,4 0,4'),
    '6': (2, '2,0 0,0 0,4 2,4 2,2 0,2'),
    '7': (2, '0,0 2,0 2,4'),
    '8': (2, '0,0 2,0 2,4 0,4 0,0; 0,2 2,2'),
    '9': (2, '2,2 0,2 0,0 2,0 2,4 0,4'),
    'A': (2, '0,4 0,0 2,0 2,4; 0,2 2,2'),
    'B': (2, '0,0 0,4 2,4 2,2 0,2; 0,0 1.5,0 1.5,2'),
    'C': (2, '2,0 0,0 0,4 2,4'),
    'D': (2, '0,0 0,4 1.5,4 2,3.5 2,0.5 1.5,0 0,0'),
    'E': (2, '2,0 0,0 0,4 2,4; 0,2 1.5,2'),
    'F': (2, '2,0 0,0 0,4; 0,2 1.5,2'),
    'G': (2, '2,0 0,0 0,4 2,4 2,2 1,2'),
    'H': (2, '0,0 0,4; 2,0 2,4; 0,2 2,2'),
    'I': (2, '0,0 2,0; 1,0 1,4; 0,4 2,4'),
    'J': (2, '2,0 2,4 0,4 0,3'),
    'K': (2, '0,0 0,4; 2,0 0,2 2,4'),
    'L': (2, '0,0 0,4 2,4'),
    'M': (4, '0,4 0,0 2,2 4,0 4,4'),
    'N': (2, '0,4 0,0 2,4 2,0'),
    'O': (2, '0,0 2,0 2,4 0,4 0,0'),
    'P': (2, '0,4 0,0 2,0 2,2 0,2'),
    'Q': (2, '0,0 2,0 2,4 0,4 0,0; 1,3 2,4'),
    'R': (2, '0,4 0,0 2,0 2,2 0,2; 1,2 2,4'),
    'S': (2, '2,0 0,0 0,2 2,2 2,4 0,4'),
    'T': (2, '0,0 2,0; 1,0 1,4'),
    'U': (2, '0,0 0,4 2,4 2,0'),
    'V': (2, '0,0 1,4 2,0'),
    'W': (4, '0,0 0,4 2,2 4,4 4,0'),
    'X': (2, '0,0 2,4; 2,0 0,4'),
    'Y': (2, '0,0 1,2 2,0; 1,2 1,4'),
    'Z': (2, '0,0 2,0 0,4 2,4'),
    '!': (0, '0,0 0,2.5; 0,4 0,4'),
    '"': (1, '0,0 0,1; 1,0 1,1'),
    "'": (0, '0,0 0,1'),
    '$': (2, '2,0.5 0,0.5 0,2 2,2 2,3.5 0,3.5; 1,0 1,4'),
    '%': (2, '0,4 2,0; 0,0 0,0; 2,4 2,4'),
    '(': (1, '1,0 0,1 0,3 1,4'),
    ')': (1, '0,0 1,1 1,3 0,4'),
    '*': (2, '1,1 1,3; 0,2 2,2; 0,1 2,3; 2,1 0,3'),
    '+': (2, '1,1 1,3; 0,2 2,2'),
    '-': (2, '0,2 2,2'),
    '.': (0, '0,4 0,4'),
    '/': (2, '0,4 2,0'),
    ':': (0, '0,1 0,1; 0,3 0,3'),
    '<': (1, '1,0 0,2 1,4'),
    '=': (2, '0,1.25 2,1.25; 0,2.75 2,2.75'),
    '>': (1, '0,0 1,2 0,4'),
    '^': (2, '0,1 1,0 2,1'),
    '_': (2, '0,4 2,4'),
    '{': (1, '1,0 0.5,0 0.5,1.5 0,2 0.5,2.5 0.5,4 1,4'),
    '|': (0, '0,0 0,4'),
    '}': (1, '0,0 0.5,0 0.5,1.5 1,2 0.5,2.5 0.5,4 0,4'),
}



def block_char(code, font):
    if not 33 <= code <= 126 or code in font.get('trace', []):
        return None
    char = chr(code).upper()
    return char if char in BLOCK_GLYPHS else None


def block_polygons(char, box, pen, gap):
    """Polygons for a block glyph over box (x0, y0, x1, y1, the vertical extent being the font's letter height).

    Each segment is swept by a rectangular pen of pen = (width, height) texels, which keeps the letters squared even
    where the font is drawn stretched. gap shortens segments at both ends, for the segmented style.
    """
    width, strokes = BLOCK_GLYPHS[char]
    pw, ph = pen
    x0, y0, x1, y1 = box
    lo_x, hi_x = x0 + pw / 2, x1 - pw / 2
    if width == 0 or hi_x < lo_x:
        lo_x = hi_x = (x0 + x1) / 2
    lo_y, hi_y = y0 + ph / 2, y1 - ph / 2
    def to_texels(u, v):
        return np.array([lo_x + (u / width if width else 0) * (hi_x - lo_x), lo_y + v / 4 * (hi_y - lo_y)])
    polygons = []
    for stroke in strokes.split(';'):
        points = [to_texels(*map(float, p.split(','))) for p in stroke.split()]
        for a, b in zip(points, points[1:]) if len(points) > 1 else [(points[0], points[0])]:
            length = np.hypot(*(b - a))
            if gap and length > 0:
                step = (b - a) / length * min(gap, length / 2)
                a, b = a + step, b - step
            polygons.append(pen_sweep(a, b, pen))
    return polygons


def pen_sweep(a, b, pen):
    """The area a rectangular pen of pen = (width, height) texels covers moving from a to b, wound one way so
    overlapping sweeps unite under the nonzero rule."""
    pw, ph = pen
    corners = np.array([[-pw / 2, -ph / 2], [pw / 2, -ph / 2], [pw / 2, ph / 2], [-pw / 2, ph / 2]])
    hull = cv2.convexHull(np.concatenate([a + corners, b + corners]).astype(np.float32)).reshape(-1, 2)
    x, y = hull[:, 0], hull[:, 1]
    if np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1)) < 0:
        hull = hull[::-1]
    return hull.astype(np.float64)


def texel_sweep_polygons(lit, origin, pen):
    """A pixel glyph (lit: its stroke texels) as the font's pen swept along them: through each horizontal and vertical
    run, and from texel to texel where two touch only at a corner (a diagonal step), so slants come out straight
    instead of stepped. origin is the cell's top left on the sheet."""
    ox, oy = origin
    h, w = lit.shape
    center = lambda x, y: np.array([ox + x + 0.5, oy + y + 0.5])
    polygons, covered = [], np.zeros_like(lit)
    for y in range(h):
        for x in range(w):
            if not lit[y, x]:
                continue
            if x + 1 < w and lit[y, x + 1] and (x == 0 or not lit[y, x - 1]):    # a row's run starts here
                end = x
                while end + 1 < w and lit[y, end + 1]:
                    end += 1
                polygons.append(pen_sweep(center(x, y), center(end, y), pen))
                covered[y, x:end + 1] = True
            if y + 1 < h and lit[y + 1, x] and (y == 0 or not lit[y - 1, x]):    # a column's run starts here
                end = y
                while end + 1 < h and lit[end + 1, x]:
                    end += 1
                polygons.append(pen_sweep(center(x, y), center(x, end), pen))
                covered[y:end + 1, x] = True
            for dx in (-1, 1):
                if y + 1 < h and 0 <= x + dx < w and lit[y + 1, x + dx] and not lit[y, x + dx] and not lit[y + 1, x]:
                    polygons.append(pen_sweep(center(x, y), center(x + dx, y + 1), pen))
                    covered[y, x] = covered[y + 1, x + dx] = True
    for y, x in zip(*np.nonzero(lit & ~covered)):
        polygons.append(pen_sweep(center(x, y), center(x, y), pen))
    return polygons


def polygons_path(polygons):
    return ' '.join('M' + ' L'.join(fmt(p) for p in polygon) + ' Z' for polygon in polygons)


def block_layout(font, collected):
    """The font's letter top and bottom within a cell, from the capitals' ink."""
    tops, bottoms = [], []
    for _, sheet, cells, _, _ in collected:
        for char, (x0, y0, x1, y1), box in cells:
            if char.isalpha():
                tops.append(box[1] - y0)
                bottoms.append(box[3] - y0)
    return float(np.median(tops)), float(np.median(bottoms))


def block_boxes(cells, layout, inset):
    """Each block glyph's box: its ink's horizontal extent and the font's letter height, shrunk by inset texels."""
    top, bottom = layout
    boxes = []
    for char, rect, box in cells:
        x0, y0, x1, y1 = box[0] + inset, rect[1] + top + inset, box[2] - inset, rect[1] + bottom - inset
        cx, cy = (box[0] + box[2]) / 2, rect[1] + (top + bottom) / 2
        boxes.append((char, rect, (min(x0, cx), min(y0, cy), max(x1, cx), max(y1, cy))))
    return boxes


def stroke_thickness(cells, fill_level, axis):
    """Median ink of the font's strokes across an axis (runs of bright texels along rows for axis 1), in texels."""
    thicknesses = []
    for cell in cells:
        lines = cell if axis == 1 else cell.T
        for line in lines:
            bright = line >= fill_level / 2
            start = None
            for i, on in enumerate(list(bright) + [False]):
                if on and start is None:
                    start = i
                elif not on and start is not None:
                    thicknesses.append(line[start:i].sum() / fill_level)
                    start = None
    return float(np.median(thicknesses))


def fit_block_style(font, collected):
    """The pen measured from the font's strokes, and the halo that best reproduces the font.

    Only the halo is fitted by least squares: where a hand-made glyph differs from the original, thinner strokes (or
    bigger gaps) always cost less, which would thin every glyph. The gap is a per-font style setting.
    """
    layout = block_layout(font, collected)
    cells = [(sheet, cell) for _, sheet, block_cells, _, _ in collected for cell in block_cells]
    original = np.concatenate([sheet[r[1]:r[3], r[0]:r[2]].ravel() for sheet, (_, r, _) in cells])
    fill_level = float(np.percentile(original[original >= INK_FLOOR], 95))
    letters = [sheet[r[1]:r[3], r[0]:r[2]] for sheet, (char, r, _) in cells if char.isalnum()]
    # Fonts the game draws stretched can set their pen, since their anti-aliasing hides the stroke width.
    pw, ph = font.get('pen') or (stroke_thickness(letters, fill_level, axis=1),
                                 stroke_thickness(letters, fill_level, axis=0))
    gap = font.get('gap', 0.0)
    best = None
    # The ink box includes the halo, so each halo width draws the strokes in the box shrunk by that width, keeping the
    # halo inside the cell (the game draws only the cell, so anything past it is cut off). Width 0 is for fonts
    # without a halo, whose ink is all stroke.
    for w in (0.0, *FIT_HALO_WIDTHS):
        core, ring = [], []
        for sheet, (char, rect, box) in cells:
            _, _, glyph_box = block_boxes([(char, rect, box)], layout, w)[0]
            x0, y0, x1, y1 = rect
            margin = 2
            rows, cols = (y1 - y0 + 2 * margin) * FIT_RES, (x1 - x0 + 2 * margin) * FIT_RES
            mask = np.zeros((rows, cols), np.uint8)
            for polygon in block_polygons(char, glyph_box, (pw, ph), gap):
                local = (polygon - (x0 - margin, y0 - margin)) * FIT_RES * 16  # 4 fractional bits.
                cv2.fillConvexPoly(mask, np.round(local).astype(np.int32), 1, cv2.LINE_8, 4)
            inside = mask.astype(bool)
            distance = cv2.distanceTransform((~inside).astype(np.uint8), cv2.DIST_L2, 5) / FIT_RES
            crop = (slice(margin * FIT_RES, rows - margin * FIT_RES),
                    slice(margin * FIT_RES, cols - margin * FIT_RES))
            core.append(texel_average(inside[crop].astype(np.float32), FIT_RES).ravel())
            ring.append(texel_average((~inside & (distance <= w))[crop].astype(np.float32), FIT_RES).ravel())
        residual = original - fill_level * np.concatenate(core)
        ring = np.concatenate(ring)
        stroke = float(np.clip(np.dot(ring, residual) / max(np.dot(ring, ring), 1e-9), 0, fill_level))
        error = float(np.mean((residual - stroke * ring) ** 2))
        if best is None or error < best['error']:
            best = {'pen': (pw, ph), 'gap': gap, 'halo': w, 'fill': fill_level, 'stroke': stroke, 'error': error,
                    'layout': layout}
    return best


# The Block font's segment layout, in a glyph's texels (x 0-4, y 0-8 from the cell's top left plus one: its glow ring
# takes the cell's outer texels). Segments run between nodes at x 0, 2, 4 and y 0, 4, 8: the seven of an 8 (its
# middle split, as G lights only the right half), two center verticals, the half crosses (corner to center: K, M, X,
# Y, W), the full crosses (corner to the far middle: N) and V's point (middle to bottom center). Each is
# (node, node, the texels it lights in the original).
SEGMENTS = {
    'a': ((0, 0), (4, 0), [(1, 0), (2, 0), (3, 0)]),
    'd': ((0, 8), (4, 8), [(1, 8), (2, 8), (3, 8)]),
    'g1': ((0, 4), (2, 4), [(1, 4)]),
    'g2': ((2, 4), (4, 4), [(3, 4)]),
    'f': ((0, 0), (0, 4), [(0, 1), (0, 2), (0, 3)]),
    'b': ((4, 0), (4, 4), [(4, 1), (4, 2), (4, 3)]),
    'e': ((0, 4), (0, 8), [(0, 5), (0, 6), (0, 7)]),
    'c': ((4, 4), (4, 8), [(4, 5), (4, 6), (4, 7)]),
    'h': ((2, 0), (2, 4), [(2, 1), (2, 2), (2, 3)]),
    'j': ((2, 4), (2, 8), [(2, 5), (2, 6), (2, 7)]),
    'ul': ((0, 0), (2, 4), [(1, 2), (2, 3)]),
    'ur': ((4, 0), (2, 4), [(3, 2), (2, 3)]),
    'll': ((2, 4), (0, 8), [(2, 5), (1, 6)]),
    'lr': ((2, 4), (4, 8), [(2, 5), (3, 6)]),
    'ux': ((0, 0), (4, 4), [(1, 1), (2, 2), (3, 3)]),
    'uy': ((4, 0), (0, 4), [(3, 1), (2, 2), (1, 3)]),
    'lx': ((0, 4), (4, 8), [(1, 5), (2, 6), (3, 7)]),
    'ly': ((4, 4), (0, 8), [(3, 5), (2, 6), (1, 7)]),
    'vl': ((0, 4), (2, 8), [(1, 6), (2, 7)]),
    'vr': ((4, 4), (2, 8), [(3, 6), (2, 7)]),
}


def segment_layout(lit):
    """The SEGMENTS a glyph's lit texels (9 x 5 from its top left) light, and how many lit texels they leave
    unexplained (a glyph off the layout: parentheses and the like)."""
    on = [name for name, (_, _, texels) in SEGMENTS.items() if texels and all(lit[y, x] for x, y in texels)]
    explained = np.zeros_like(lit)
    for name in on:
        a, b, texels = SEGMENTS[name]
        for x, y in texels + [a, b]:
            explained[y, x] = True
    return on, int((lit & ~explained).sum())


# Characters whose texels don't follow the layout, drawn on it the way segment displays draw them ('dotX,Y' is a
# square dot on that texel). The rest off the layout (parentheses, braces, =) keep their texel runs.
SEGMENT_OVERRIDES = {
    '$': ['a', 'f', 'g1', 'g2', 'c', 'd', 'h', 'j'],
    '*': ['g1', 'g2', 'h', 'j', 'ul', 'ur', 'll', 'lr'],
    '@': ['a', 'b', 'd', 'e', 'f', 'g2', 'hl'],
    'Z': ['a', 'ur', 'll', 'd'],     # its lower left a diagonal, not the original's vertical
    "'": ['h'],
    '!': ['h', 'dot2,8'],
    '^': ['al', 'ar'],
}
# Drawn only by override or merge: ^'s two strokes, and the whole middle (g1 and g2 both lit).
SEGMENTS.update({'al': ((2, 0), (0, 4), []), 'ar': ((2, 0), (4, 4), []), 'g': ((0, 4), (4, 4), []),
                 'hl': ((2, 2), (2, 4), [])})      # and @'s stub: the upper center vertical's lower half


def display_segment(a, b, width=1.0, gap=0.12):
    """A display segment from node a to node b (texel units): a bar of the width with its ends pointed at 45 degrees,
    the tips a gap short of the nodes, so segments meeting at a node touch only at their points. Diagonals stop
    further short, clear of the bars' points."""
    d = b - a
    length = np.hypot(*d)
    u = d / length
    n = np.array([-u[1], u[0]])
    hw = width / 2
    if abs(u[0]) > 1e-6 and abs(u[1]) > 1e-6:
        gap = 0.55
    ta, tb = a + u * gap, b - u * gap
    sa, sb = ta + u * hw, tb - u * hw
    return np.array([ta, sa + n * hw, sb + n * hw, tb, sb - n * hw, sa - n * hw])


def segment_elements(sheet, rect, cells=None):
    """A segment-display font's redraw over rect (x0, y0, x1, y1) of its sheet, from its texels: Rush 2's Block font
    (5) lights 1-texel segments, apart at the corners, at full intensity with a dim 1-texel glow round them. Vertical
    runs of lit texels become vertical segments and horizontal runs (of what's left) horizontal ones, both pointed at
    the ends as display segments are; texels left over are diagonal segments (K, M, N, X...), stroked through their
    centers, or dots. The glow is the element's halo at the sheet's dim level."""
    x0, y0, x1, y1 = rect
    area = sheet[y0:y1, x0:x1]
    full = float(area.max())
    if full <= 0:
        return []
    lit = area >= 0.75 * full
    dim = area[(area > 0) & ~lit]
    glow = float(np.median(dim)) if dim.size else 0.0
    polygons = []
    # Characters on the segment layout: its segments, all one shape. Their texels are then taken out of what the
    # texel runs below redraw.
    for code, cx, cy in cells or []:
        gx, gy = cx + 1 - x0, cy + 1 - y0
        glyph = lit[gy:gy + 9, gx:gx + 5]
        if glyph.shape != (9, 5) or not glyph.any():
            continue
        char = chr(code) if 32 < code < 127 else None
        on, unexplained = segment_layout(glyph)
        if char in SEGMENT_OVERRIDES:
            on = SEGMENT_OVERRIDES[char]
        elif not on or unexplained > 4:
            continue
        if 'g1' in on and 'g2' in on:       # the whole middle: one segment, as an 8's; the halves are for G
            on = [n for n in on if n not in ('g1', 'g2')] + ['g']
        for name in on:
            if name.startswith('dot'):
                x, y = (int(v) for v in name[3:].split(','))
                polygons.append(np.array([(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)], float) + (gx, gy))
            else:
                a, b, _ = SEGMENTS[name]
                polygons.append(display_segment(np.array(a) + (gx + 0.5, gy + 0.5), np.array(b) + (gx + 0.5, gy + 0.5)))
        lit[cy - y0:cy - y0 + 11, cx - x0:cx - x0 + 7] = False

    def bar(ax, ay, bx, by):
        """A segment over texels (ax, ay)..(bx, by) inclusive, along x or y, pointed at both ends."""
        if ay == by:
            return np.array([(ax, ay + 0.5), (ax + 0.5, ay), (bx + 0.5, ay), (bx + 1, ay + 0.5), (bx + 0.5, ay + 1),
                             (ax + 0.5, ay + 1)], float)
        return np.array([(ax + 0.5, ay), (ax + 1, ay + 0.5), (ax + 1, by + 0.5), (ax + 0.5, by + 1), (ax, by + 0.5),
                         (ax, ay + 0.5)], float)

    used = np.zeros_like(lit)
    for axis in (0, 1):                          # vertical runs first: a diagonal's foot is no horizontal segment
        free = lit & ~used
        lines = free.T if axis == 0 else free
        for i, line in enumerate(lines):
            j = 0
            while j < len(line):
                if not line[j]:
                    j += 1
                    continue
                k = j
                while k + 1 < len(line) and line[k + 1]:
                    k += 1
                if k > j:
                    polygons.append(bar(i, j, i, k) if axis == 0 else bar(j, i, k, i))
                    if axis == 0:
                        used[j:k + 1, i] = True
                    else:
                        used[i, j:k + 1] = True
                j = k + 1
    rest = (lit & ~used).astype(np.uint8)
    count, labels = cv2.connectedComponents(rest, connectivity=8)
    for n in range(1, count):
        ys, xs = np.nonzero(labels == n)
        points = list(zip(xs, ys))
        if len(points) == 1:
            x, y = points[0]
            polygons.append(np.array([(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)], float))
            continue
        # A chain: walked from an end, each step to a diagonal neighbor.
        left = set(points)
        ends = [p for p in points if sum((p[0] + dx, p[1] + dy) in left for dx in (-1, 0, 1) for dy in (-1, 0, 1)
                                         if dx or dy) == 1]
        path = [ends[0] if ends else points[0]]
        left.discard(path[0])
        while left:
            p = path[-1]
            near = [q for q in left if max(abs(q[0] - p[0]), abs(q[1] - p[1])) == 1]
            if not near:
                break
            path.append(near[0])
            left.discard(near[0])
        centers = [np.array([x + 0.5, y + 0.5]) for x, y in path]
        # Out to the end texels' far corners.
        for i, j in ((0, 1), (-1, -2)):
            d = centers[i] - centers[j]
            centers[i] = centers[i] + d / np.hypot(*d) * 0.35
        polygons += [ring for ring, _ in stroke_outline(centers, 1.0, 'p', lambda q: q)]
        for x, y in left:
            polygons.append(np.array([(x, y), (x + 1, y), (x + 1, y + 1), (x, y + 1)], float))
    polygons = [(p if signed_area(p) > 0 else p[::-1]) + (x0, y0) for p in polygons]
    core = {'d': polygons_path(polygons), 'rule': 'nonzero', 'fill': full, 'stroke': 0.0, 'halo': 0.0, 'clip': rect}
    if not glow:
        return [clear_element(rect), core]
    # The glow: each segment widened on its own (mitred, so its points stay points), not a halo round the whole
    # glyph. Highlighted text (the flashing lap record, the player's name) draws every texel that isn't 0 at full
    # color, glow included: a round halo there filled the gaps between segments and the holes of 0, 8 and B, and the
    # letters ran together into blobs. Widened segments keep those gaps, so the highlight reads as bold segments.
    widened = [offset_polygon(p, SEGMENT_GLOW) for p in polygons]
    return [clear_element(rect), dict(core, d=polygons_path(widened), fill=glow), core]


SEGMENT_GLOW = 0.35     # How far a segment font's glow reaches past each segment, in texels.


def offset_polygon(points, d, miter_limit=2.0):
    """A counter-clockwise polygon pushed out by d along each edge's normal, corners mitred (a mitre longer than
    miter_limit x d is cut to that length along the corner's bisector)."""
    n = len(points)
    out = []
    for i in range(n):
        p, a, b = points[i], points[i - 1], points[(i + 1) % n]
        e0, e1 = p - a, b - p
        if np.hypot(*e0) < 1e-9 or np.hypot(*e1) < 1e-9:
            continue
        e0, e1 = e0 / np.hypot(*e0), e1 / np.hypot(*e1)
        n0, n1 = np.array([e0[1], -e0[0]]), np.array([e1[1], -e1[0]])   # outward for counter-clockwise (y down)
        bis = n0 + n1
        length = np.hypot(*bis)
        if length < 1e-9:
            out.append(p + n0 * d)
            continue
        bis /= length
        miter = d / max(float(np.dot(bis, n0)), 1e-9)
        out.append(p + bis * min(miter, miter_limit * d))
    return np.array(out)


def block_elements(style, sheet, block_cells, traced_cells, uncovered):
    look = {'fill': style['fill'], 'stroke': style['stroke'], 'halo': style['halo']}
    elements = traced_elements(style, sheet, uncovered, squared=True)
    # Cells no skeleton draws (the (R), (C) and TM, and characters a font lists under "trace"): the pen swept along
    # their own texels, the font's strokes, where a trace rounded them into blobs.
    for rect in traced_cells:
        x0, y0, x1, y1 = rect
        lit = sheet[y0:y1, x0:x1] >= style['fill'] / 2
        elements.append(clear_element(rect))
        if lit.any():
            polygons = texel_sweep_polygons(lit, (x0, y0), style['pen'])
            elements.append(dict(look, d=polygons_path(polygons), rule='nonzero', clip=rect))
    for char, rect, box in block_boxes(block_cells, style['layout'], style['halo']):
        polygons = block_polygons(char, box, style['pen'], style['gap'])
        elements += [clear_element(rect), dict(look, d=polygons_path(polygons), rule='nonzero', clip=rect)]
    return elements


# ---------------------------------------------------------------------------------------------------------------------
# SVG


def gray(level):
    v = round(level * 17)
    return f'rgb({v},{v},{v})'


def format_svg(title, width, height, elements):
    lines = [
        f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {width} {height}" '
        f'width="{width * SCALE}" height="{height * SCALE}">',
        f'  <title>{title}</title>',
        '  <defs>',
    ]
    for i, element in enumerate(elements):
        if element['clip']:
            x0, y0, x1, y1 = element['clip']
            lines.append(f'    <clipPath id="c{i}"><rect x="{x0}" y="{y0}" width="{x1 - x0}" height="{y1 - y0}"/>'
                         '</clipPath>')
    lines += ['  </defs>', f'  <rect width="{width}" height="{height}" fill="#000"/>']
    for i, element in enumerate(elements):
        attributes = f'fill="{gray(element["fill"])}" fill-rule="{element["rule"]}"'
        if element['stroke']:
            # paint-order puts the stroke under the fill, so only its outer half (the halo) shows.
            attributes += (f' stroke="{gray(element["stroke"])}" stroke-width="{element["halo"] * 2:g}"'
                           ' stroke-linejoin="round" paint-order="stroke"')
        if element['clip']:
            attributes += f' clip-path="url(#c{i})"'
        lines.append(f'  <path {attributes} d="{element["d"]}"/>')
    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


def parse_svg(text):
    """Reads the SVGs this script writes."""
    width, height = map(float, re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', text).groups())
    clips = {}
    for cid, x, y, w, h in re.findall(
            r'<clipPath id="(\w+)"><rect x="([\d.]+)" y="([\d.]+)" width="([\d.]+)" height="([\d.]+)"/>', text):
        x, y, w, h = (int(float(v)) for v in (x, y, w, h))
        clips[cid] = (x, y, x + w, y + h)

    def level(rgb):
        return int(re.match(r'rgb\((\d+)', rgb).group(1)) / 17

    elements = []
    for attributes in re.findall(r'<path ([^>]*)/>', text):
        attr = dict(re.findall(r'([\w-]+)="([^"]*)"', attributes))
        clip = clips[re.match(r'url\(#(\w+)\)', attr['clip-path']).group(1)] if 'clip-path' in attr else None
        elements.append({
            'd': attr['d'], 'rule': attr['fill-rule'], 'fill': level(attr['fill']), 'clip': clip,
            'stroke': level(attr['stroke']) if 'stroke' in attr else 0,
            'halo': float(attr.get('stroke-width', 0)) / 2,
        })
    return int(width), int(height), elements


# ---------------------------------------------------------------------------------------------------------------------
# HUD images
#
# The race HUD's numbers and "MPH" label are color (CI) images rather than font sheets. They're redrawn the same way:
# digits from Inter with a fitted weight and slant, filled with a vertical gradient fitted from the original's colors
# over a black drop shadow at the fitted offset; the label from the block glyphs with an outline. The game stores them
# upside down (it draws them flipped), so they're fitted upright and the SVGs keep the stored orientation.

DIGIT_WEIGHTS = range(500, 901, 100)
DIGIT_SLANTS = (0, 6, 10, 14, 18)  # Degrees.
SHADOW_OFFSETS = np.arange(0, 3.01, 0.5)
BODY_BRIGHTNESS = 250  # r + g + b above this is the digit's body, below it (opaque) the shadow and outline.


def decode_ci(tmem, info):
    """RGBA of a CI4 or CI8 tile with an RGBA16 palette, from an RT64 TMEM dump (see RT64's TextureDecoder.hlsli)."""
    tile, width, height = info['tile'], info['width'], info['height']
    start, stride = tile['tmem'] << 3, tile['line'] << 3
    out = np.zeros((height, width, 4), np.uint8)
    for y in range(height):
        for x in range(width):
            rel = y * stride + ((x << tile['siz']) >> 1)
            if y & 1:  # Odd rows have their 32-bit words swapped.
                row = (rel // stride) * stride
                rel = row + (((rel - row) // 4) ^ 1) * 4 + (rel & 3)
            value = tmem[(start + rel) & 0x7FF]
            if tile['siz'] == 0:
                index = (tile['palette'] << 4) | ((value >> (0 if x & 1 else 4)) & 0xF)
            else:
                index = value
            entry = (tmem[0x800 + index * 8] << 8) | tmem[0x800 + index * 8 + 1]
            expand = lambda v: (v << 3) | (v >> 2)
            out[y, x] = (expand(entry >> 11 & 0x1F), expand(entry >> 6 & 0x1F), expand(entry >> 1 & 0x1F),
                         255 if entry & 1 else 0)
    return out


def load_hud_image(dump_dirs, hash_name):
    base = find_dump(dump_dirs, hash_name)
    if base is None:
        return None
    info = json.load(open(base + '.tile.json'))
    return decode_ci(open(base + '.tmem', 'rb').read(), info)


def find_dump(dump_dirs, hash_name):
    for d in dump_dirs:
        base = os.path.join(d, hash_name + '.v5')
        if os.path.exists(base + '.tile.json'):
            return base
    return None


def body_and_shadow(rgba):
    opaque = rgba[..., 3] > 0
    bright = rgba[..., :3].astype(int).sum(axis=2) > BODY_BRIGHTNESS
    return (opaque & bright).astype(np.float32), (opaque & ~bright).astype(np.float32)


def bounds(mask):
    ys, xs = np.nonzero(mask)
    return float(xs.min()), float(ys.min()), float(xs.max() + 1), float(ys.max() + 1)


def slanted_glyph(vector, char, slant, box):
    """Affine placing the glyph, slanted by slant degrees, exactly over box (x0, y0, x1, y1), upright SVG space."""
    k = np.tan(np.radians(slant))
    shear = (1, k, 0, -1, 0, 0)  # Font units to SVG orientation, leaning right as the glyph rises.
    points = np.concatenate(parse_path(vector.path(char, shear)))
    gx0, gy0 = points.min(axis=0)
    gx1, gy1 = points.max(axis=0)
    x0, y0, x1, y1 = box
    sx, sy = (x1 - x0) / (gx1 - gx0), (y1 - y0) / (gy1 - gy0)
    return (sx, sx * k, 0, -sy, x0 - gx0 * sx, y0 - gy0 * sy)


def flip_affine(affine, height):
    a, b, c, d, e, f = affine
    return (a, b, -c, -d, e, height - f)


def coverage(d, width, height, offset=(0, 0)):
    """Texel coverage of path data, optionally offset (in texels)."""
    polygons = [(p + offset) * FIT_RES for p in parse_path(d)]
    return texel_average(fill(polygons, 'nonzero', height * FIT_RES, width * FIT_RES).astype(np.float32), FIT_RES)


def visible_box(body, hidden_top_rows):
    """The body's bounds, below the rows the game never shows.

    The game draws the digits flipped starting exactly at the image's last stored row (the upright top row), so that
    row falls just outside the rectangle and only a bilinear sliver of it shows. Glyphs fitted into it lose their tops.
    """
    x0, y0, x1, y1 = bounds(body)
    return x0, max(y0, float(hidden_top_rows)), x1, y1


def fit_digits(images, hidden_top_rows):
    """Weight, slant and shadow offset shared by a digit set, and each digit's upright path data."""
    best = None
    for weight in DIGIT_WEIGHTS:
        vector = VectorFont(weight, 32)
        for slant in DIGIT_SLANTS:
            error, paths = 0.0, []
            for char, rgba in images:
                body, _ = body_and_shadow(rgba)
                d = vector.path(char, slanted_glyph(vector, char, slant, visible_box(body, hidden_top_rows)))
                error += float(np.sum((coverage(d, rgba.shape[1], rgba.shape[0]) - body) ** 2))
                paths.append(d)
            if best is None or error < best[0]:
                best = (error, weight, slant, paths)
    _, weight, slant, paths = best

    # The shadow is the glyph moved down and right, showing where the body doesn't cover it.
    best_offset = None
    for dx in SHADOW_OFFSETS:
        for dy in SHADOW_OFFSETS:
            error = 0.0
            for (char, rgba), d in zip(images, paths):
                body, shadow = body_and_shadow(rgba)
                h, w = body.shape
                predicted = coverage(d, w, h, (dx, dy)) * (1 - coverage(d, w, h))
                error += float(np.sum((predicted - shadow) ** 2))
            if best_offset is None or error < best_offset[0]:
                best_offset = (error, (float(dx), float(dy)))
    return weight, slant, best_offset[1], paths


def fit_gradient(rgba, box):
    """Colors at the top, middle and bottom of the box that best match the body's colors, linearly interpolated."""
    body, _ = body_and_shadow(rgba)
    ys, xs = np.nonzero(body)
    t = np.clip((ys + 0.5 - box[1]) / (box[3] - box[1]), 0, 1)
    basis = np.stack([np.clip(1 - 2 * t, 0, 1), 1 - np.abs(2 * t - 1), np.clip(2 * t - 1, 0, 1)], axis=1)
    colors, *_ = np.linalg.lstsq(basis, rgba[ys, xs, :3].astype(np.float64), rcond=None)
    return np.clip(colors, 0, 255)


def label_elements(rgba):
    """Block glyph elements for a label ("MPH"): one per connected run of body texels, in reading order."""
    body, shadow = body_and_shadow(rgba)
    count, _, stats, _ = cv2.connectedComponentsWithStats(body.astype(np.uint8), connectivity=8)
    boxes = sorted((x, y, x + w, y + h) for x, y, w, h, _ in stats[1:count])
    top, bottom = min(b[1] for b in boxes), max(b[3] for b in boxes)
    color = np.median(rgba[body > 0][:, :3], axis=0)
    outline = np.median(rgba[shadow > 0][:, :3], axis=0) if shadow.any() else np.zeros(3)
    return boxes, top, bottom, color, outline


def rgb(color):
    r, g, b = (int(round(v)) for v in color)
    return f'rgb({r},{g},{b})'


def trace_hud(dump_dirs):
    hud = json.load(open(HUD_LIST))
    os.makedirs(HUD_SVG_DIR, exist_ok=True)
    for old in glob.glob(os.path.join(HUD_SVG_DIR, '*.svg')):
        os.remove(old)
    digit_paths = {}  # (set, char) -> (font, char, stored-orientation affines of the shadow and body), for alpha tiles.
    for set_name, settings in hud['sets'].items():
        entries = [e for e in hud['textures'] if e['set'] == set_name]
        images = []
        for entry in entries:
            rgba = load_hud_image(dump_dirs, entry['hash'])
            if rgba is None:
                sys.exit(f'{entry["hash"]} ({set_name} {entry["char"]}) is not in the dumps')
            images.append((entry['char'], rgba[::-1] if settings['flipped'] else rgba))
        if settings['style'] == 'alpha tile':
            # The digits are drawn in two-cycle mode with a second tile that reads the same bytes as CI4 at double S
            # (32 texels per row, so each sample lands on the high nibble of a CI8 texel), and the draw's alpha is
            # two thirds the digit's and one third this tile's. So it becomes the redrawn digit's shape (body and
            # shadow) stretched to twice the width, opaque white.
            for entry, (char, rgba) in zip(entries, images):
                h, w = rgba.shape[:2]
                vector, digit_char, affines = digit_paths[(entry['of'], char)]
                body = ''.join(f'\n  <path fill="rgb(255,255,255)" fill-rule="nonzero" '
                               f'd="{vector.path(digit_char, (2 * a, 2 * b, c, d, 2 * e, f))}"/>'
                               for a, b, c, d, e, f in affines)
                with open(os.path.join(HUD_SVG_DIR, entry['hash'] + '.svg'), 'w', newline='\n') as out:
                    out.write(f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w * SCALE}" '
                              f'height="{h * SCALE}">\n  <title>{set_name} {char} ({entry["hash"]}), the alpha tile '
                              f'of {entry["of"]} {char}</title>{body}\n</svg>\n')
            print(f'{set_name}: {len(entries)} images')
            continue
        if settings['style'] == 'digits':
            weight, slant, (dx, dy), paths = fit_digits(images, settings.get('hidden_top_rows', 0))
            print(f'{set_name}: Inter weight {weight}, slant {slant}, shadow offset ({dx}, {dy})')
            vector = VectorFont(weight, 32)
        for i, (entry, (char, rgba)) in enumerate(zip(entries, images)):
            h, w = rgba.shape[:2]
            body, _ = body_and_shadow(rgba)
            box = visible_box(body, settings.get('hidden_top_rows', 0))
            flip = (lambda y: h - y) if settings['flipped'] else (lambda y: y)
            lines = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {w} {h}" width="{w * SCALE}" '
                     f'height="{h * SCALE}">',
                     f'  <title>{set_name} {char} ({entry["hash"]}), stored upside down as the game draws it flipped'
                     f'</title>' if settings['flipped'] else f'  <title>{set_name} {char} ({entry["hash"]})</title>']
            if settings['style'] == 'digits':
                affine = slanted_glyph(vector, char, slant, box)
                stored = flip_affine(affine, h) if settings['flipped'] else affine
                shadow = flip_affine((*affine[:4], affine[4] + dx, affine[5] + dy), h) if settings['flipped'] \
                    else (*affine[:4], affine[4] + dx, affine[5] + dy)
                stops = fit_gradient(rgba, box)
                lines += [
                    '  <defs>',
                    f'    <linearGradient id="body" gradientUnits="userSpaceOnUse" x1="0" y1="{flip(box[1]):g}" '
                    f'x2="0" y2="{flip(box[3]):g}">',
                    *(f'      <stop offset="{o}" stop-color="{rgb(c)}"/>' for o, c in zip((0, 0.5, 1), stops)),
                    '    </linearGradient>',
                    '  </defs>',
                    f'  <path fill="rgb(0,0,0)" fill-rule="nonzero" d="{vector.path(char, shadow)}"/>',
                    f'  <path fill="url(#body)" fill-rule="nonzero" d="{vector.path(char, stored)}"/>',
                ]
                digit_paths[(set_name, char)] = (vector, char, (shadow, stored))
            else:
                boxes, top, bottom, color, outline = label_elements(rgba)
                polygons = []
                for char_box, letter in zip(boxes, char):
                    x0, _, x1, _ = char_box
                    glyph = block_polygons(letter, (x0, top, x1, bottom), (1.0, 1.0), 0)
                    polygons += [np.stack([p[:, 0], flip(p[:, 1])], axis=1) for p in glyph]
                lines.append(f'  <path fill="{rgb(color)}" fill-rule="nonzero" stroke="{rgb(outline)}" '
                             f'stroke-width="2" stroke-linejoin="round" paint-order="stroke" '
                             f'd="{polygons_path(polygons)}"/>')
            lines.append('</svg>')
            with open(os.path.join(HUD_SVG_DIR, entry['hash'] + '.svg'), 'w', newline='\n') as f:
                f.write('\n'.join(lines) + '\n')
        print(f'{set_name}: {len(entries)} images')


def render_color_svg(text):
    """RGBA (straight alpha) at SCALE for the HUD SVGs: solid or vertical-gradient fills, optional outline stroke."""
    width, height = map(float, re.search(r'viewBox="0 0 ([\d.]+) ([\d.]+)"', text).groups())
    width, height = int(width), int(height)
    res = SCALE * SUPERSAMPLE
    gradients = {}
    for gid, y1, y2, body in re.findall(r'<linearGradient id="(\w+)"[^>]* y1="([\d.-]+)" x2="0" y2="([\d.-]+)">'
                                        r'(.*?)</linearGradient>', text, re.S):
        stops = [(float(o), [int(v) for v in c]) for o, *c in
                 re.findall(r'offset="([\d.]+)" stop-color="rgb\((\d+),(\d+),(\d+)\)"', body)]
        gradients[gid] = (float(y1), float(y2), stops)
    premultiplied = np.zeros((height * res, width * res, 4), np.float32)
    ys = (np.arange(height * res) + 0.5) / res
    for attributes in re.findall(r'<path ([^>]*)/>', text):
        attr = dict(re.findall(r'([\w-]+)="([^"]*)"', attributes))
        inside = fill([p * res for p in parse_path(attr['d'])], attr['fill-rule'], height * res, width * res)
        layers = []
        if 'stroke' in attr:
            distance = cv2.distanceTransform((~inside).astype(np.uint8), cv2.DIST_L2, 5) / res
            outline = ~inside & (distance <= float(attr['stroke-width']) / 2)
            layers.append((outline, np.array([int(v) for v in re.findall(r'\d+', attr['stroke'])], np.float32)))
        if attr['fill'].startswith('url'):
            y1, y2, stops = gradients[re.match(r'url\(#(\w+)\)', attr['fill']).group(1)]
            t = np.clip((ys - y1) / (y2 - y1), 0, 1)
            offsets = [o for o, _ in stops]
            column = np.stack([np.interp(t, offsets, [c[i] for _, c in stops]) for i in range(3)], axis=1)
            color = np.broadcast_to(column[:, None, :], (height * res, width * res, 3))
        else:
            color = np.array([int(v) for v in re.findall(r'\d+', attr['fill'])], np.float32)
        layers.append((inside, color))
        for mask, layer_color in layers:
            source = np.broadcast_to(layer_color, (height * res, width * res, 3))
            premultiplied[mask, :3] = source[mask]
            premultiplied[mask, 3] = 255
    premultiplied[..., :3] *= premultiplied[..., 3:] / 255
    small = cv2.resize(premultiplied, (width * SCALE, height * SCALE), interpolation=cv2.INTER_AREA)
    alpha = small[..., 3:]
    rgb_out = np.where(alpha > 0, small[..., :3] * 255 / np.maximum(alpha, 1e-6), 0)
    return np.clip(np.round(np.concatenate([rgb_out, alpha], axis=2)), 0, 255).astype(np.uint8)


# ---------------------------------------------------------------------------------------------------------------------
# Image dumps
#
# Menu images (tools/font_pack/menu_svg) are drawn by hand, named by their RT64 hash. To find an image's hash, run the
# game with RUSH2_DUMP_IMAGES=<path> (src/fonts.cpp) and pass the dump to the images command: it replays each load
# into TMEM the way RT64 does (rt64_rdp.cpp) and writes every distinct image as <hash>.png.

TMEM_MASK = 0xFFF


class DumpTile:
    def __init__(self):
        self.fmt = self.siz = self.line = self.tmem = self.palette = 0
        self.cms = self.cmt = self.masks = self.maskt = 0
        self.uls = self.ult = self.lrs = self.lrt = 0


def replay(cmds, images, tmem, tiles, state):
    """Runs a dumped command list's tile setup and texture loads into tmem."""
    for w0, w1 in cmds:
        op = w0 >> 24
        if op == 0xFD:  # SetTImg
            state['timg'] = ((w0 >> 19) & 3, (w0 & 0xFFF) + 1, images[str(w1)])
            continue
        tile = tiles[(w1 >> 24) & 7]
        if op == 0xF5:  # SetTile
            tile.fmt, tile.siz, tile.line, tile.tmem = (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 >> 9) & 0x1FF, w0 & 0x1FF
            tile.palette, tile.cmt, tile.maskt = (w1 >> 20) & 0xF, (w1 >> 18) & 3, (w1 >> 14) & 0xF
            tile.cms, tile.masks = (w1 >> 8) & 3, (w1 >> 4) & 0xF
        elif op == 0xF2:  # SetTileSize
            tile.uls, tile.ult, tile.lrs, tile.lrt = (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF
        elif op in (0xF3, 0xF0, 0xF4):
            siz, width, src = state['timg']
            uls, ult, lrs, lrt = (w0 >> 12) & 0xFFF, w0 & 0xFFF, (w1 >> 12) & 0xFFF, w1 & 0xFFF
            row_bytes = width << siz >> 1

            def put(address, value):
                tmem[address & TMEM_MASK] = value

            def byte(offset):
                return src[offset] if offset < len(src) else 0

            if op == 0xF3:  # LoadBlock: one run of words, odd lines (by dxt) word-swapped.
                source, address, swap, counter = (uls << siz >> 1) + row_bytes * ult, tile.tmem << 3, 0, 0
                for _ in range(((lrs - uls) >> (4 - tile.siz)) + 1):
                    for i in range(8):
                        put((address + i) ^ swap, byte(source + i))
                    counter += lrt
                    while counter >= 0x800:
                        address += tile.line << 3
                        counter -= 0x800
                        swap ^= 4
                    source += 8
                    address += 8
            elif op == 0xF0:  # LoadTLUT: each 16-bit entry fills a 64-bit word.
                source = ((uls >> 2) << siz >> 1) + row_bytes * (ult >> 2)
                for entry in range((lrs >> 2) - (uls >> 2) + 1):
                    for i in range(8):
                        put((tile.tmem << 3) + entry * 8 + i, byte(source + entry * 2 + (i & 1)))
            else:  # LoadTile: rows at the tile's line stride, odd rows word-swapped.
                source = ((uls >> 2) << siz >> 1) + row_bytes * (ult >> 2)
                words = (((lrs >> 2) - (uls >> 2)) >> (4 - tile.siz)) + 1
                for row in range((lrt >> 2) - (ult >> 2) + 1):
                    for i in range(words * 8):
                        put((tile.tmem << 3) + row * (tile.line << 3) + (i ^ (4 if row & 1 else 0)),
                            byte(source + row * row_bytes + i))


def tmem_hash(tmem, tile, width, height, tlut):
    """RT64's TMEMHasher (version 5), with its TLUT bitset."""
    rgba32 = tile.siz == 3 and tile.fmt == 0
    size = 2048 if rgba32 or tlut else 4096
    row_bytes = max(width << (2 if rgba32 else tile.siz) >> 1, 1)
    start = (tile.tmem << 3) & (size - 1)
    h = xxhash.xxh3_64()
    used = [0, 0, 0, 0]

    def update(data):
        h.update(bytes(data))
        for v in data if tlut else ():
            if tile.siz == 0:
                used[0] |= (1 << (v & 0xF)) | (1 << (v >> 4))
            else:
                used[v >> 6] |= 1 << (v & 0x3F)

    def hash_range(base, count, odd_row):
        if base + count > size:
            update(tmem[base:size])
            count = min(count - (size - base), base)
            base = 0
        if odd_row:
            words = count // 8 * 8
            update(tmem[base:base + words])
            base, count = base + words, count - words
            if count > 4:
                update(tmem[base:base + count - 4])
                update(tmem[base + 4:base + 8])
            elif count > 0:
                update(tmem[base + 4:base + 4 + count])
        else:
            update(tmem[base:base + count])

    if (tile.line << 3) > row_bytes:
        for y in range(height):
            hash_range((start + y * (tile.line << 3)) & (size - 1), row_bytes, y & 1)
    else:
        hash_range(start, (tile.line << 3) * (height - 1) + row_bytes, False)
    if tlut:
        ci4 = tile.siz == 0
        palette = 2048 + (tile.palette << 7 if ci4 else 0)
        if ci4 and used[0] == 0xFFFF:
            h.update(bytes(tmem[palette:palette + 0x80]))
        else:
            for i in range(1 if ci4 else 4):
                if used[i] == (1 << 64) - 1:
                    h.update(bytes(tmem[palette + i * 0x200:palette + (i + 1) * 0x200]))
                else:
                    for bit in range(64):
                        if used[i] >> bit & 1:
                            entry = palette + (i * 64 + bit) * 8
                            h.update(bytes(tmem[entry:entry + 8]))
    h.update(struct.pack('<HHIHBB', width, height, tlut, tile.line, tile.siz, tile.fmt))
    return f'{h.intdigest():016x}'


def decode_tmem(tmem, tile, width, height):
    """RGBA of a CI, I4 or RGBA16 tile in tmem."""
    out = np.zeros((height, width, 4), np.uint8)

    def rgba16(e):
        expand = lambda v: (v << 3) | (v >> 2)
        return expand(e >> 11 & 31), expand(e >> 6 & 31), expand(e >> 1 & 31), 255 if e & 1 else 0

    for y in range(height):
        for x in range(width):
            rel = y * (tile.line << 3) + ((x << tile.siz) >> 1)
            if y & 1:
                rel ^= 4
            address = (tile.tmem << 3) + rel
            value = tmem[address & (0x7FF if tile.fmt == 2 else TMEM_MASK)]
            nibble = (value >> (0 if x & 1 else 4)) & 0xF
            if tile.fmt == 2:
                index = (tile.palette << 4) | nibble if tile.siz == 0 else value
                out[y, x] = rgba16((tmem[0x800 + index * 8] << 8) | tmem[0x801 + index * 8])
            elif tile.fmt == 4 and tile.siz == 0:
                out[y, x] = (nibble * 17,) * 4
            elif tile.fmt == 0 and tile.siz == 2:
                out[y, x] = rgba16((value << 8) | tmem[(address + 1) & TMEM_MASK])
    return out


def dumped_image(load):
    """(hash, RGBA) of a dumped image load as RT64 sees it when drawn with tile 0."""
    images = {k: bytes.fromhex(v) for k, v in load['images'].items()}
    tmem, tiles, state = bytearray(4096), [DumpTile() for _ in range(8)], {}
    replay(load.get('tlut', []) + load['cmds'], images, tmem, tiles, state)
    tile = tiles[0]
    # src/fonts.cpp's clamps: font sheets on both axes, other images on T when loaded shorter than their mask.
    if tile.fmt == 4:
        tile.cms |= 2
        tile.cmt |= 2
    elif tile.maskt and (tile.lrt - tile.ult) // 4 + 1 < (1 << tile.maskt):
        tile.cmt |= 2
    # RT64's sample size: the tile size where clamped, capped by the mask.
    width = min(max((tile.lrs - tile.uls + 4) // 4, 1) if tile.masks == 0 or tile.cms & 2 else 0xFFFF,
                (1 << tile.masks) if tile.masks else 0xFFFF)
    height = min(max((tile.lrt - tile.ult + 4) // 4, 1) if tile.maskt == 0 or tile.cmt & 2 else 0xFFFF,
                 (1 << tile.maskt) if tile.maskt else 0xFFFF)
    if width > 1024 or height > 1024:
        return None, None
    # The game sets the RGBA16 palette mode outside the loads, for every CI image.
    tlut = 0x8000 if tile.fmt == 2 else 0
    return tmem_hash(tmem, tile, width, height, tlut), decode_tmem(tmem, tile, width, height)


def dump_images(dump_path, out_dir):
    os.makedirs(out_dir, exist_ok=True)
    written = set()
    for line in open(dump_path):
        hash_name, rgba = dumped_image(json.loads(line))
        if hash_name is None or hash_name in written:
            continue
        written.add(hash_name)
        cv2.imwrite(os.path.join(out_dir, hash_name + '.png'), cv2.cvtColor(rgba, cv2.COLOR_RGBA2BGRA))
    print(f'Wrote {len(written)} images to {out_dir}')


# ---------------------------------------------------------------------------------------------------------------------
# Commands


def rom2049_tables():
    """Font tables (the RUSH2_DUMP_FONT_TABLES format) for ROM2049_FONTS, from the Rush 2049 ROM: each page's coverage
    as I4 (IA8 pages keep their low nibble, as src/rush2049/font2049.cpp loads them) and its glyphs' inclusive cells."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'rush2049'))
    import font49
    import roms
    rom = roms.Rush2049()
    tables = []
    for n in ROM2049_FONTS:
        k = font49.FIRST + n
        _, images = font49.atlas(rom, k)
        cells = font49.glyphs(rom, k)
        pages = []
        for index, image in enumerate(images):
            values = np.array(image, np.uint8) // 17
            packed = (values[:, 0::2] << 4) | values[:, 1::2]
            # Every character, the accented ones past 127 too: they're packed between the others, and a sheet whose
            # table left them out would show their texels as free space (drawn_region).
            last = max([ord(c) for c in cells] + [127])
            glyphs = [[0, 0, 0, 0] for _ in range(last + 1)]
            for char, (page, x0, y0, x1, y1) in cells.items():
                if page == index:
                    glyphs[ord(char)] = [x0, y0, x1, y1]
            pages.append({'first': 0, 'last': last, 'w': image.width, 'h': image.height, 'glyphs': glyphs,
                          'pixels': packed.tobytes().hex()})
        tables.append({'id': f'rush2049_{n}', 'pages': pages})
    return tables


def trace(table_path, tables=None, svg_dir=SVG_DIR):
    fonts = json.load(open(FONT_LIST, encoding='utf-8'))
    if tables is None:
        tables = json.load(open(table_path))

    # Group pages by sheet. Sheets shared by several pages (a font's digits and W-Z) get every page's characters.
    sheets = {}
    for table in tables:
        font = fonts.get(str(table['id']))
        if font is None:
            print(f'Font {table["id"]} is not listed in {FONT_LIST}, skipping')
            continue
        for page in table['pages']:
            hash_name = rt64_hash(page)
            if hash_name not in sheets:
                sheets[hash_name] = (font, dict(page, first=0, glyphs=[]))
            merged = sheets[hash_name][1]
            for code, *rect in ((page['first'] + i, *r) for i, r in enumerate(page['glyphs']) if r[3] != 0):
                merged['glyphs'] += [[0, 0, 0, 0]] * (code + 1 - len(merged['glyphs']))
                merged['glyphs'][code] = rect

    os.makedirs(svg_dir, exist_ok=True)
    for old in glob.glob(os.path.join(svg_dir, '*.svg')):
        os.remove(old)

    for font_id, font in fonts.items():
        font_sheets = [(h, page) for h, (f, page) in sheets.items() if f is font]
        if not font_sheets:
            continue
        sheet_list = [(page, decode_sheet(page)) for _, page in font_sheets]
        if font['style'] == 'synth':
            elements, detail = synth_elements(font_id, font, sheet_list)
            per_sheet = [(h, page, e) for (h, _), (page, _), e in zip(font_sheets, sheet_list, elements)]
        elif font['style'] == 'drawn':
            # Hand-drawn outlines (Rush 2049's AERO menu lettering), fitted to each font's texels.
            elements, detail = drawn_elements(font_id, font, sheet_list)
            per_sheet = [(h, page, e) for (h, _), (page, _), e in zip(font_sheets, sheet_list, elements)]
        elif font['style'] == 'segment':
            # A segment display font: redrawn from its lit texels over the whole sheet (segment_elements).
            per_sheet = [(h, page, segment_elements(sheet, (0, 0, page['w'], page['h']),
                                                    [(c, x, y) for c, x, y, _, _ in glyph_cells(page, font)]))
                         for (h, _), (page, sheet) in zip(font_sheets, sheet_list)]
            detail = 'segments'
        elif font['style'] == 'block':
            collected = collect_cells(font, sheet_list, block_char)
            style = fit_block_style(font, collected)
            # "segment_rows": a band of the sheet below the characters holding segment-display letters (Pixel's
            # sheet carries a copy of the Block font's), redrawn as segments rather than traced.
            band = font.get('segment_rows')

            def outside(r):
                return not band or r[3] <= band[0] or r[1] >= band[1]
            per_sheet = [(h, page, block_elements(style, sheet, block_cells, traced_cells,
                                                  [r for r in uncovered if outside(r)]) +
                          (segment_elements(sheet, (0, band[0], page['w'], min(band[1], page['h'])))
                           if band and band[0] < page['h'] else []))
                         for (h, _), (page, sheet, block_cells, traced_cells, uncovered)
                         in zip(font_sheets, collected)]
            detail = (f'block pen {style["pen"]}, gap {style["gap"]}, '
                      f'fill {style["fill"]:.1f}, halo {style["stroke"]:.1f} x {style["halo"]}, '
                      f'rms error {style["error"] ** 0.5:.2f}')
        else:
            collected = collect_cells(font, sheet_list, vector_char)
            style = choose_style(font, collected)
            per_sheet = [(h, page, vector_elements(font, style, sheet, vector_cells, traced_cells, uncovered))
                         for (h, _), (page, sheet, vector_cells, traced_cells, uncovered)
                         in zip(font_sheets, collected)]
            detail = (f'Inter weight {style["weight"]}, inset {style["inset"]}, fill {style["fill"]:.1f}, '
                      f'halo {style["stroke"]:.1f} x {style["halo"]}, rms error {style["error"] ** 0.5:.2f}')
        for hash_name, page, elements in per_sheet:
            chars = ''.join(chr(c) for c, *_ in glyph_cells(page) if 33 <= c <= 126)
            title = f'{font["name"]}: {chars[:1]}-{chars[-1:]} ({hash_name})'
            with open(os.path.join(svg_dir, hash_name + '.svg'), 'w', newline='\n') as f:
                f.write(format_svg(title, page['w'], page['h'], elements))
            print(f'{hash_name} {font["name"]} {page["w"]}x{page["h"]}: {detail}')


def preview(tables_path, out_path, ids):
    """Each sheet of the given fonts (all when none are named) from a font tables dump, its original texels above its
    redraw (the SVG rasterized as the pack does), into one PNG: to compare a redraw with the original by eye."""
    tables = json.load(open(tables_path))
    rows = []
    for table in tables:
        if ids and str(table['id']) not in ids:
            continue
        for page in table['pages']:
            path = None
            for d in (SVG_DIR, SVG_2049_DIR, SVG_2049_MENU_DIR):
                if os.path.exists(os.path.join(d, rt64_hash(page) + '.svg')):
                    path = os.path.join(d, rt64_hash(page) + '.svg')
            if path is None:
                continue
            width, height, elements = parse_svg(open(path).read())
            image = cv2.resize(render(width, height, elements, SCALE * SUPERSAMPLE), (width * SCALE, height * SCALE),
                               interpolation=cv2.INTER_AREA)
            original = cv2.resize(decode_sheet(page), (width * SCALE, height * SCALE), interpolation=cv2.INTER_NEAREST)
            rows += [np.clip(original * 17, 0, 255).astype(np.uint8), np.clip(np.round(image * 17), 0, 255).astype(np.uint8)]
    if not rows:
        sys.exit('No redrawn sheets for ' + ' '.join(ids))
    width = max(r.shape[1] for r in rows)   # sheets differ in width: padded, with a gap under each pair
    rows = [np.pad(r, ((0, 16 if i % 2 else 2), (0, width - r.shape[1])), constant_values=40)
            for i, r in enumerate(rows)]
    cv2.imwrite(out_path, np.vstack(rows))


def pack():
    svgs = sorted(glob.glob(os.path.join(SVG_DIR, '*.svg')) + glob.glob(os.path.join(SVG_2049_DIR, '*.svg')) +
                  glob.glob(os.path.join(SVG_2049_MENU_DIR, '*.svg')))
    if not svgs:
        sys.exit('No SVGs in ' + SVG_DIR)
    textures = []
    stamp = (2000, 1, 1, 0, 0, 0)  # Fixed timestamps keep the archive identical between builds of the same SVGs.
    buffer = io.BytesIO()
    with zipfile.ZipFile(buffer, 'w', zipfile.ZIP_DEFLATED) as rtz:
        for path in svgs:
            hash_name = os.path.splitext(os.path.basename(path))[0]
            width, height, elements = parse_svg(open(path).read())
            image = render(width, height, elements, SCALE * SUPERSAMPLE)
            image = cv2.resize(image, (width * SCALE, height * SCALE), interpolation=cv2.INTER_AREA)
            value = np.clip(np.round(image * 17), 0, 255).astype(np.uint8)
            ok, png = cv2.imencode('.png', np.dstack([value] * 4))
            assert ok
            rtz.writestr(zipfile.ZipInfo(f'fonts/{hash_name}.png', stamp), png.tobytes(), zipfile.ZIP_DEFLATED)
            # No shift: the game draws its text point-sampled, each texel over [i, i + 1) of its cell, which is where
            # each redraw block sits. Measured in game against the original sheets (an 8x block upscale lands on them
            # exactly); "center", right for filtered 3D textures, moved every glyph about 0.35 texel up and left,
            # clipping its first row and column.
            textures.append({'path': f'fonts/{hash_name}', 'hashes': {'rt64': hash_name}, 'operation': 'preload',
                             'shift': 'none'})
        for folder, svg_dir in (('hud', HUD_SVG_DIR), ('menu', MENU_SVG_DIR), ('battle', BATTLE_SVG_DIR)):
            for path in sorted(glob.glob(os.path.join(svg_dir, '*.svg'))):
                hash_name = os.path.splitext(os.path.basename(path))[0]
                ok, png = cv2.imencode('.png', cv2.cvtColor(render_color_svg(open(path).read()), cv2.COLOR_RGBA2BGRA))
                assert ok
                rtz.writestr(zipfile.ZipInfo(f'{folder}/{hash_name}.png', stamp), png.tobytes(), zipfile.ZIP_DEFLATED)
                textures.append({'path': f'{folder}/{hash_name}', 'hashes': {'rt64': hash_name},
                                 'operation': 'preload', 'shift': 'none'})
        database = {
            'configuration': {'autoPath': 'rt64', 'configurationVersion': 3, 'hashVersion': 5},
            'textures': textures,
        }
        for name, data in (('rt64.json', database), ('mod.json', MANIFEST)):
            rtz.writestr(zipfile.ZipInfo(name, stamp), json.dumps(data, indent=4), zipfile.ZIP_DEFLATED)
        # Mod menu thumbnail, drawn by tools/build_icons.py.
        with open(os.path.join(PACK_DIR, 'thumb.png'), 'rb') as thumb:
            rtz.writestr(zipfile.ZipInfo('thumb.png', stamp), thumb.read(), zipfile.ZIP_STORED)
    with open(OUTPUT, 'wb') as f:
        f.write(buffer.getvalue())
    print(f'Wrote {len(textures)} sheets to {OUTPUT} ({len(buffer.getvalue()) // 1024} KiB)')


GLYPH_PATH = re.compile(r'<path fill="rgb\(255,255,255\)"[^>]*?stroke-width="([\d.]+)"[^>]*?clip-path="url\(#(c\d+)\)" d="([^"]*)"')
CLIP_RECT = re.compile(r'<clipPath id="(c\d+)"><rect x="([\d.]+)" y="([\d.]+)" width="([\d.]+)" height="([\d.]+)"/>')
POINT = re.compile(r'(-?[\d.]+),(-?[\d.]+)')


def glyph_overhangs(svg_path):
    """Yields (clip id, overhang left, right, top, bottom) for each glyph whose outline (fill plus half its stroke)
    reaches past its cell, where the game cuts it off flat. Bounds come from the path's points, curve controls
    included."""
    with open(svg_path, encoding='utf-8') as f:
        text = f.read()
    cells = {m[1]: tuple(float(v) for v in m.groups()[1:]) for m in CLIP_RECT.finditer(text)}
    for m in GLYPH_PATH.finditer(text):
        half = float(m[1]) / 2
        x, y, w, h = cells[m[2]]
        pts = np.array([[float(a), float(b)] for a, b in POINT.findall(m[3])])
        yield (m[2], x - (pts[:, 0].min() - half), pts[:, 0].max() + half - (x + w),
               y - (pts[:, 1].min() - half), pts[:, 1].max() + half - (y + h))


def fit_glyph(svg_path, clip_id, y0=None, y1=None, stroke=None, scale=None):
    """Squeezes one glyph horizontally about its center so its outline fits its cell, and, given y0 and y1, moves its
    ink to span exactly those rows (to line a symbol up with the digits). A thinner outline (stroke, in texels) leaves
    a glyph in a narrow cell more of its width; scale overrides the squeeze when a trace of outline past the cell
    looks better than a narrow glyph. Edits the SVG in place.

    The game lines characters up by their cells' tops, so y0 and y1 are in the sheet's rows offset by the difference
    between this cell's top and the reference glyph's (a "/" cell starting a row above the digits' needs its ink a
    row higher than theirs)."""
    with open(svg_path, encoding='utf-8', newline='') as f:
        text = f.read()
    cells = {m[1]: tuple(float(v) for v in m.groups()[1:]) for m in CLIP_RECT.finditer(text)}
    m = next(m for m in GLYPH_PATH.finditer(text) if m[2] == clip_id)
    if stroke is not None:
        text = text[:m.start(1)] + f'{stroke:g}' + text[m.end(1):]
        m = next(m for m in GLYPH_PATH.finditer(text) if m[2] == clip_id)
    half = float(m[1]) / 2
    cx, _, cw, _ = cells[clip_id]
    pts = np.array([[float(a), float(b)] for a, b in POINT.findall(m[3])])
    lo, hi = pts.min(axis=0), pts.max(axis=0)
    mid = (lo[0] + hi[0]) / 2
    # Keep the ink centered where it was, or on the cell's center if that's the only place it fits.
    room = min(mid - cx, cx + cw - mid) - half - 0.02
    if room <= 0:
        mid, room = cx + cw / 2, cw / 2 - half - 0.02
    sx = scale if scale is not None else min(1.0, room / max((hi[0] - lo[0]) / 2, 1e-6))
    sy, ty = 1.0, 0.0
    if y0 is not None:
        sy = (y1 - y0) / (hi[1] - lo[1])
        ty = y0 - lo[1] * sy

    def move(p):
        return f'{mid + (float(p[1]) - mid) * sx:.3f},{float(p[2]) * sy + ty:.3f}'

    d = POINT.sub(move, m[3])
    text = text[:m.start(3)] + d + text[m.end(3):]
    with open(svg_path, 'w', encoding='utf-8', newline='') as f:
        f.write(text)
    print(f'{clip_id}: x scale {sx:.3f}, y {lo[1]:.2f}-{hi[1]:.2f} -> {lo[1] * sy + ty:.2f}-{hi[1] * sy + ty:.2f}')


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'check':
        for clip_id, *over in glyph_overhangs(sys.argv[2]):
            if max(over) > 0.25:
                print(clip_id, ' '.join(f'{v:+.2f}' for v in over))
    elif len(sys.argv) >= 4 and sys.argv[1] == 'fit':
        args = sys.argv[4:]
        opts = {}
        for flag in ('--stroke', '--scale'):
            if flag in args:
                i = args.index(flag)
                opts[flag[2:]] = float(args[i + 1])
                del args[i:i + 2]
        fit_glyph(sys.argv[2], sys.argv[3], *(float(v) for v in args), **opts)
    elif len(sys.argv) == 3 and sys.argv[1] == 'trace':
        trace(sys.argv[2])
    elif len(sys.argv) == 2 and sys.argv[1] == 'trace-2049':
        trace(None, rom2049_tables(), SVG_2049_DIR)
    elif len(sys.argv) >= 4 and sys.argv[1] == 'preview':
        preview(sys.argv[2], sys.argv[3], sys.argv[4:])
    elif len(sys.argv) == 3 and sys.argv[1] == 'trace-2049-menus':
        trace(sys.argv[2], None, SVG_2049_MENU_DIR)
    elif len(sys.argv) >= 3 and sys.argv[1] == 'trace-hud':
        trace_hud(sys.argv[2:])
    elif len(sys.argv) == 4 and sys.argv[1] == 'synth':
        synth(sys.argv[2], sys.argv[3])
    elif len(sys.argv) == 2 and sys.argv[1] == 'pack':
        pack()
    elif len(sys.argv) == 4 and sys.argv[1] == 'images':
        dump_images(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
