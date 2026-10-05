"""Draws the menu selection arrows (tools/font_pack/menu_svg): the rounded triangle that lights up, its slanted sides
bowing out slightly, with a dark margin and a metal rim of constant width around it (their corners curve around the
triangle's), and the connector stub.

  python tools/font_pack/draw_arrows.py
"""

import os

import numpy as np

# Stored coordinates: the game draws the arrow flipped and only rows 0..12 show (see build_font_pack.py MENU_SVG_DIR).
TOP, BOTTOM = 0.25, 12.0
CY = (TOP + BOTTOM) / 2
BACK, TIP = 4.0, 20.5  # The plate's back and the front of its rounded point.
GAP, RIM = 0.7, 1.0    # Dark margin around the triangle, and the metal rim around that.
BACK_ROUND = 0.6       # Corner radius of the triangle's back corners; each layer out adds its distance to it.
POINT_ROUND = 0.3      # Corner radius of the triangle's point.
BOW = 0.35             # How far the slanted sides bow out at their middles.
STUB_HALF = 2.25       # Half the connector stub's height.
# The stub's shading top to bottom (stored, so the game shows it the other way up): the original's five stripes as one
# smooth bar.
STUB_SHADES = [(0.0, (104, 96, 60)), (0.12, (118, 110, 68)), (0.32, (82, 74, 54)), (0.5, (41, 49, 49)),
               (0.7, (76, 101, 101)), (0.88, (44, 54, 54)), (1.0, (34, 40, 40))]
LIT_EDGE = 0.55        # Width of the shaded edge around the face.


def clip(poly, normal, offset):
    """The part of a convex polygon where dot(normal, point) >= offset."""
    normal = np.array(normal, float)
    out = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        da, db = normal @ a - offset, normal @ b - offset
        if da >= 0:
            out.append(a)
        if da * db < 0:
            out.append(a + (b - a) * da / (da - db))
    return np.array(out)


def outward(a, b, center):
    """The unit normal of the edge a-b pointing away from center."""
    edge = (b - a) / np.hypot(*(b - a))
    normal = np.array([-edge[1], edge[0]])
    return normal if normal @ (a - center) > 0 else -normal


def side(a, b, center):
    """Points along a slanted side from a to b, bowing out by BOW at its middle."""
    control = (a + b) / 2 + outward(a, b, center) * 2 * BOW
    t = np.linspace(0, 1, 17)[:, None]
    return (1 - t) ** 2 * a + 2 * (1 - t) * t * control + t ** 2 * b


def grown(triangle, d):
    """The rounded, bowed triangle grown by d (shrunk for negative d): its sides moved out by d, and each corner an arc
    around the same center as the triangle's own corner, so every layer stays evenly spaced around the corners too."""
    back_top, point, back_bottom = triangle
    center = triangle.mean(axis=0)
    shape = np.array([(-100.0, -100.0), (100.0, -100.0), (100.0, 100.0), (-100.0, 100.0)])

    def within(normal, anchor, distance):
        # The points no further than distance past anchor along normal.
        return clip(shape, -normal, -(normal @ anchor + distance))

    # Each side as segments, with the normals at its ends for the corners.
    sides = [side(back_top, point, center), side(point, back_bottom, center), np.array([back_bottom, back_top])]
    ends = []
    for points in sides:
        normals = [outward(a, b, center) for a, b in zip(points, points[1:])]
        for a, normal in zip(points, normals):
            shape = within(normal, a, d)
        ends.append((normals[0], normals[-1]))
    # Corner i joins the end of side i - 1 and the start of side i; the back corners are 0 and 2, the point 1.
    for i, round_radius in enumerate((BACK_ROUND, POINT_ROUND, BACK_ROUND)):
        n1, n2 = ends[i - 1][1], ends[i][0]
        bisector = (n1 + n2) / np.hypot(*(n1 + n2))
        # The circle of round_radius touching both sides inside the corner.
        arc_center = triangle[i] - bisector * round_radius / (n1 @ bisector)
        if round_radius + d > 0:
            for t in np.linspace(0, 1, 24):
                direction = n1 * (1 - t) + n2 * t
                shape = within(direction / np.hypot(*direction), arc_center, round_radius + d)
    return shape


def triangle_for(half_back, point_x):
    """The triangle with its back a margin and rim inside BACK, the given half height there, and its point at point_x."""
    return np.array([(BACK + GAP + RIM, CY - half_back), (point_x, CY), (BACK + GAP + RIM, CY + half_back)])


def fitted(half_back):
    """The triangle with the given half back height whose rim's front reaches TIP."""
    point_x = TIP - GAP - RIM
    for _ in range(8):
        point_x += TIP - grown(triangle_for(half_back, point_x), GAP + RIM)[:, 0].max()
    return triangle_for(half_back, point_x)


# The tallest triangle whose rim stays within the rows that show.
low, high = 1.0, CY - TOP
for _ in range(30):
    mid = (low + high) / 2
    if grown(fitted(mid), GAP + RIM)[:, 1].min() >= TOP:
        low = mid
    else:
        high = mid
triangle = fitted(low)
plate = grown(triangle, GAP + RIM)
margin = grown(triangle, GAP)


# Shading, as on the original: colors by the direction each stretch of outline faces (degrees, stored coordinates with
# y down, so -90 faces the stored top, which the game shows at the bottom), blended in between.
RIM_SHADES = [(-180, (64, 60, 46)), (-135, (110, 100, 64)), (-90, (123, 115, 66)), (-70, (120, 110, 66)),
              (-30, (80, 72, 50)), (0, (34, 34, 30)), (30, (56, 76, 76)), (70, (80, 106, 106)), (90, (66, 92, 92)),
              (135, (52, 66, 70)), (180, (64, 60, 46))]
TIP_DARKEN = 0.2   # How much darker the rim gets toward the point.
# Along the back, which faces away from the light: by height, from the gold side (stored top) through the shadowed
# middle to the teal side, blended in as the rim turns to face back.
BACK_SHADES = [(0.0, (104, 94, 60)), (0.35, (58, 52, 40)), (0.5, (38, 36, 32)), (0.65, (44, 54, 56)),
               (1.0, (58, 78, 80))]
LIT_SHADES = [(-180, (0, 222, 0)), (-110, (0, 222, 0)), (-70, (0, 165, 0)), (0, (0, 105, 0)), (70, (0, 185, 0)),
              (110, (0, 222, 0)), (180, (0, 222, 0))]
UNLIT_SHADES = [(-180, (50, 50, 50)), (-110, (50, 50, 50)), (-70, (52, 42, 36)), (0, (34, 25, 23)),
                (70, (54, 46, 40)), (110, (50, 50, 50)), (180, (50, 50, 50))]


def shade(shades, angle):
    for (a0, c0), (a1, c1) in zip(shades, shades[1:]):
        if a0 <= angle <= a1:
            t = (angle - a0) / (a1 - a0)
            return np.array(c0) * (1 - t) + np.array(c1) * t
    return np.array(shades[0][1])


def shaded(poly, shades, darken=0.0, back_shades=None):
    """The polygon as slices from its middle, one per short stretch of outline, each colored by the way that stretch
    faces; darken dims them toward the point, and back_shades shades the back by height."""
    center = triangle.mean(axis=0)
    x0, x1 = poly[:, 0].min(), poly[:, 0].max()
    y0, y1 = poly[:, 1].min(), poly[:, 1].max()
    slices = []
    for i in range(len(poly)):
        a, b = poly[i], poly[(i + 1) % len(poly)]
        length = np.hypot(*(b - a))
        if length < 1e-6:
            continue
        normal = outward(a, b, center)
        facing = shade(shades, np.degrees(np.arctan2(normal[1], normal[0])))
        # Short stretches, so the shading by position stays smooth along long straight sides.
        steps = int(np.ceil(length / 0.25))
        for k in range(steps):
            p, q = a + (b - a) * k / steps, a + (b - a) * (k + 1) / steps
            mid = (p + q) / 2
            color = facing
            if back_shades is not None and normal[0] < 0:
                weight = min(1.0, -normal[0]) ** 2
                color = color * (1 - weight) + shade(back_shades, (mid[1] - y0) / (y1 - y0)) * weight
            color = color * (1 - darken * (mid[0] - x0) / (x1 - x0))
            slices.append((','.join(str(int(round(c))) for c in color), path(np.array([center, p, q]))))
    return slices


def path(poly):
    return 'M' + ' L'.join(f'{x:.3f},{y:.3f}' for x, y in poly) + ' Z'


def rect(x0, y0, x1, y1):
    return path(np.array([(x0, y0), (x1, y0), (x1, y1), (x0, y1)]))


def svg(hash_name, title, faces):
    # Connector stub, under the plate: a round bar, shaded top to bottom through the original's stripe colors.
    stub_top, stub_bottom = CY - STUB_HALF, CY + STUB_HALF
    elements = [('url(#stub)', rect(0, stub_top, BACK + 0.5, stub_bottom))]
    # Rim shaded by the way it faces, then the dark margin, then the face: its shaded edge and its flat middle.
    elements += shaded(plate, RIM_SHADES, TIP_DARKEN, BACK_SHADES)
    elements.append(('22,18,16', path(margin)))
    edge_shades, middle = faces
    elements += shaded(grown(triangle, 0), edge_shades)
    elements.append((middle, path(grown(triangle, -LIT_EDGE))))
    lines = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 13" width="256" height="104">',
             f'  <title>{title} ({hash_name}), stored upside down as the game draws it flipped</title>',
             '  <defs>',
             f'    <linearGradient id="stub" gradientUnits="userSpaceOnUse" x1="0" y1="{stub_top:g}" x2="0" '
             f'y2="{stub_bottom:g}">',
             *(f'      <stop offset="{offset:g}" stop-color="rgb({r},{g},{b})"/>' for offset, (r, g, b) in STUB_SHADES),
             '    </linearGradient>',
             '  </defs>']
    lines += [f'  <path fill="{c if c.startswith("url") else f"rgb({c})"}" fill-rule="nonzero" d="{d}"/>'
              for c, d in elements]
    lines.append('</svg>')
    return '\n'.join(lines) + '\n'


out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'menu_svg')
open(f'{out_dir}/60c730289fea9208.svg', 'w', newline='\n').write(
    svg('60c730289fea9208', 'Menu arrow, unlit', (UNLIT_SHADES, '50,50,50')))
open(f'{out_dir}/87f40df409c34f72.svg', 'w', newline='\n').write(
    svg('87f40df409c34f72', 'Menu arrow, lit', (LIT_SHADES, '0,222,0')))
