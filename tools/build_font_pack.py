"""Builds the high-resolution font texture pack (assets/rush2_hires_fonts.rtz).

The game's fonts are 4-bit intensity (I4) glyph sheets, and the race HUD's numbers and "MPH" label are color (CI)
images. All of them are redrawn as SVGs once and kept in tools/font_pack/svg and tools/font_pack/hud_svg (one per
texture, named by its RT64 texture hash), so the pack can be rebuilt or hand-edited without the ROM. None of the
game's own pixels are packed.

  python tools/build_font_pack.py trace <font_tables.json>        Redraw every font sheet as an SVG.
  python tools/build_font_pack.py trace-hud <texture dump dir>...  Redraw the HUD images listed in hud.json.
  python tools/build_font_pack.py pack                            Rasterize the SVGs and write the .rtz.

font_tables.json comes from running the game with RUSH2_DUMP_FONT_TABLES=<path> (src/fonts.cpp). It holds each font's
pages: the sheet's pixels and the rectangle of every character in it. The HUD images come from RT64 texture dumps
(<hash>.v5.tmem and .tile.json, from the RT64 debugger's texture dumper in developer mode).

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

HUD images: see the HUD images section.

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
        if y1 != 0 and 0 <= x0 < x1 <= page['w'] and 0 <= y0 < y1 <= page['h']:
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
# Commands


def trace(table_path):
    fonts = json.load(open(FONT_LIST))
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
            for code, *rect in glyph_cells(page):
                merged['glyphs'] += [[0, 0, 0, 0]] * (code + 1 - len(merged['glyphs']))
                merged['glyphs'][code] = rect

    os.makedirs(SVG_DIR, exist_ok=True)
    for old in glob.glob(os.path.join(SVG_DIR, '*.svg')):
        os.remove(old)

    for font in fonts.values():
        font_sheets = [(h, page) for h, (f, page) in sheets.items() if f is font]
        if not font_sheets:
            continue
        sheet_list = [(page, decode_sheet(page)) for _, page in font_sheets]
        if font['style'] == 'block':
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
            with open(os.path.join(SVG_DIR, hash_name + '.svg'), 'w', newline='\n') as f:
                f.write(format_svg(title, page['w'], page['h'], elements))
            print(f'{hash_name} {font["name"]} {page["w"]}x{page["h"]}: {detail}')


def pack():
    svgs = sorted(glob.glob(os.path.join(SVG_DIR, '*.svg')))
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
            # No half-texel shift: the redraw is aligned to the cells, and shifting would sample the neighbors.
            textures.append({'path': f'fonts/{hash_name}', 'hashes': {'rt64': hash_name}, 'operation': 'preload',
                             'shift': 'none'})
        for path in sorted(glob.glob(os.path.join(HUD_SVG_DIR, '*.svg'))):
            hash_name = os.path.splitext(os.path.basename(path))[0]
            ok, png = cv2.imencode('.png', cv2.cvtColor(render_color_svg(open(path).read()), cv2.COLOR_RGBA2BGRA))
            assert ok
            rtz.writestr(zipfile.ZipInfo(f'hud/{hash_name}.png', stamp), png.tobytes(), zipfile.ZIP_DEFLATED)
            textures.append({'path': f'hud/{hash_name}', 'hashes': {'rt64': hash_name}, 'operation': 'preload',
                             'shift': 'none'})
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


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'trace':
        trace(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == 'trace-hud':
        trace_hud(sys.argv[2:])
    elif len(sys.argv) == 2 and sys.argv[1] == 'pack':
        pack()
    else:
        sys.exit(__doc__)
