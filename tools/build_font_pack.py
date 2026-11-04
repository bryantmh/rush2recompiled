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
Drawn fonts): Rush 2049's AERO menu lettering, whose rounded and sharp corners no vector font matches and whose 7-16
pixel glyphs are too small to trace.

Regular fonts: every glyph is a full-intensity core with a dim one-texel halo around it, which the game's color
combiner turns into the text's outline. Each character is replaced by the same character from Inter
(assets/InterVariable.ttf, SIL Open Font License), close to the Helvetica-style originals, drawn as an SVG path with
the core as its fill and the halo as an outer stroke, clipped to the character's cell so nothing spills into its
neighbors. Per font, the Inter weight, how far the outlines sit inside each glyph's ink box, and the halo's intensity
and width are fitted by least squares so the redrawn sheet, averaged back down to texels, matches the original. The
halo always stays inside the ink box: the game draws exactly each character's cell, which the ink often fills, so a
halo reaching past it is cut off flat.

Squared fonts (the tiny, pixel and segmented ones) are too small to trace or match to a vector font, so they're drawn
from a hand-made squared font (BLOCK_GLYPHS): each character's strokes swept by a rectangular pen measured from the
original, with the halo fitted the same way.

Characters neither set covers (the game's cursor, block, diamond and (c)/(r) shapes), and ink on a sheet that no
character uses, are traced instead: bicubic upsampling, a contour between the core and halo levels, simplification,
and sharp corners kept while gentle turns become curves (all corners for squared fonts).

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


def stroke_outline(points, width, cap, place):
    """Contours of a stroke of a width along a polyline (closed when it ends where it starts): the polyline offset by
    half the width on each side, joined with miters, its ends cut across the stroke (cap 'p') or level (cap 'h')."""
    closed = np.allclose(points[0], points[-1])
    pts = np.array(points[:-1] if closed else points, float)
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
        if cap == 'h' and abs(d[1]) > 0.2:
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

    def coverage_of(p, t):
        return drawn_coverage([pts for pts, _ in glyph.outline(p, t)], width, height)

    def placed_error(p, t):
        return float(np.sum((coverage_of(p, t) - cell) ** 2))

    # Coarse searches first: a shape lying on blank texels has no gradient to follow. The whole glyph by half texels,
    # then each separate piece (a colon's dots, a ? and its dot) by its own edges, the ones no other piece shares.
    shifts = np.arange(-2.0, 2.01, 0.5)
    transform = min((transform + (0, dx, 0, dy) for dx in shifts for dy in shifts),
                    key=lambda t: placed_error(p0, t))
    transform = least_squares(lambda t: (coverage_of(p0, t) - cell).ravel(), transform, diff_step=1e-3).x
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
        return np.concatenate([(coverage_of(p, transform) - cell).ravel(), pull * (p - p0) * scale])

    p = least_squares(error, np.clip(p0, lower + 1e-6, upper - 1e-6), bounds=(lower, upper), diff_step=1e-4).x
    return p, transform, float(np.sqrt(np.mean((coverage_of(p, transform) - cell) ** 2)))


def drawn_levels(font, cells, full):
    """The font's cap scale and its cap top's row in a glyph rectangle, from its H (or I): the drawings' caps run from
    y = 0 to 11, the true edges 0.15 beyond."""
    for char in 'HI':
        if char in cells:
            (x0, y0, x1, y1), sheet = cells[char]
            edges = ink_edges((sheet[y0:y1, x0:x1] / full).max(axis=1))
            if edges:
                sy = (edges[1] - edges[0]) / 11.3
                return sy, edges[0] + 0.15 * sy
    sys.exit(f'{font["name"]}: no H or I to measure the caps from')


def drawn_design(name):
    import importlib.util
    spec = importlib.util.spec_from_file_location(name, os.path.join(PACK_DIR, name + '.py'))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def drawn_elements(font_id, font, sheet_list, only=None):
    """Each sheet's glyph elements for a drawn font, fitted to its texels, and a summary of the fit. The design's
    NATIVE drawings for the font (by its id in fonts.json) replace its shared ones."""
    design = drawn_design(font['design'])
    glyphs = design.GLYPHS
    natives = getattr(design, 'NATIVE', {}).get(font_id, {})
    full = max(float(sheet.max()) for _, sheet in sheet_list)
    cells = {}
    for page, sheet in sheet_list:
        for code, x0, y0, x1, y1 in glyph_cells(page, font):
            if 32 < code < 127:
                cells[chr(code)] = ((x0, y0, x1, y1), sheet)
    levels = drawn_levels(font, cells, full)
    pull = font.get('pull', DRAWN_PULL)
    per_sheet, errors = [], []
    for page, sheet in sheet_list:
        elements = []
        for code, x0, y0, x1, y1 in glyph_cells(page, font):
            char = chr(code) if 32 < code < 127 else None
            if (char not in glyphs and char not in natives) or (only and char not in only):
                continue
            cell = sheet[y0:y1, x0:x1] / full
            if not cell.any():
                continue
            glyph = DrawnGlyph(natives.get(char) or glyphs[char])
            p, transform, rms = fit_drawn_glyph(glyph, cell, levels, pull, char in natives)
            errors.append((rms, char))
            d = re.sub(r'(-?[\d.]+),(-?[\d.]+)', lambda m: fmt((float(m.group(1)) + x0, float(m.group(2)) + y0)),
                       glyph.svg(p, transform))
            elements.append({'d': d, 'rule': 'nonzero', 'fill': full, 'stroke': 0.0, 'halo': 0.0,
                             'clip': (x0, y0, x1, y1)})
        per_sheet.append(elements)
    errors.sort(reverse=True)
    worst = ' '.join(f'{c}:{e * full:.2f}' for e, c in errors[:8])
    mean = float(np.sqrt(np.mean([e * e for e, _ in errors]))) * full if errors else 0.0
    return per_sheet, f'drawn ({font["design"]}), rms error {mean:.2f} of {full:.0f} levels, worst {worst}'


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
    corners = np.array([[-pw / 2, -ph / 2], [pw / 2, -ph / 2], [pw / 2, ph / 2], [-pw / 2, ph / 2]])
    polygons = []
    for stroke in strokes.split(';'):
        points = [to_texels(*map(float, p.split(','))) for p in stroke.split()]
        for a, b in zip(points, points[1:]) if len(points) > 1 else [(points[0], points[0])]:
            length = np.hypot(*(b - a))
            if gap and length > 0:
                step = (b - a) / length * min(gap, length / 2)
                a, b = a + step, b - step
            hull = cv2.convexHull(np.concatenate([a + corners, b + corners]).astype(np.float32)).reshape(-1, 2)
            x, y = hull[:, 0], hull[:, 1]
            if np.dot(x, np.roll(y, -1)) - np.dot(y, np.roll(x, -1)) < 0:
                hull = hull[::-1]  # One winding direction, so overlapping strokes unite under the nonzero rule.
            polygons.append(hull.astype(np.float64))
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


def block_elements(style, sheet, block_cells, traced_cells, uncovered):
    look = {'fill': style['fill'], 'stroke': style['stroke'], 'halo': style['halo']}
    elements = traced_elements(style, sheet, uncovered, squared=True)
    elements += traced_elements(style, sheet, traced_cells, squared=True, clear=True)
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
            glyphs = [[0, 0, 0, 0] for _ in range(128)]
            for char, (page, x0, y0, x1, y1) in cells.items():
                if page == index and ord(char) < 128:
                    glyphs[ord(char)] = [x0, y0, x1, y1]
            pages.append({'first': 0, 'last': 127, 'w': image.width, 'h': image.height, 'glyphs': glyphs,
                          'pixels': packed.tobytes().hex()})
        tables.append({'id': f'rush2049_{n}', 'pages': pages})
    return tables


def trace(table_path, tables=None, svg_dir=SVG_DIR):
    fonts = json.load(open(FONT_LIST))
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
        if font['style'] == 'drawn':
            # Hand-drawn outlines (Rush 2049's AERO menu lettering), fitted to each font's texels.
            elements, detail = drawn_elements(font_id, font, sheet_list)
            per_sheet = [(h, page, e) for (h, _), (page, _), e in zip(font_sheets, sheet_list, elements)]
        elif font['style'] == 'block':
            collected = collect_cells(font, sheet_list, block_char)
            style = fit_block_style(font, collected)
            per_sheet = [(h, page, block_elements(style, sheet, block_cells, traced_cells, uncovered))
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
        if ids and table['id'] not in ids:
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
    elif len(sys.argv) == 2 and sys.argv[1] == 'pack':
        pack()
    elif len(sys.argv) == 4 and sys.argv[1] == 'images':
        dump_images(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
