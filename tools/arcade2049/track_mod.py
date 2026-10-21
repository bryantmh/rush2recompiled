#!/usr/bin/env python3
"""Converts a San Francisco Rush 2049 arcade (SE) track into a custom-track mod (docs/custom_tracks.md), at the
arcade's full model detail (LOD 0) and texture size. Formats and findings: docs/arcade2049.md.

    python tools/arcade2049/track_mod.py TRACK4 OUT_MOD_DIR [--id alcatraz] [--name Alcatraz] [--mod-id ID]
    python tools/arcade2049/track_mod.py --ship-previews

--ship-previews cuts each tools/arcade2049/previews/<id>.png round (round_preview) into the game's shipped
assets/arcade2049/<id>_preview.png, which its own build (src/arcade2049) puts in the mod.

Reads the arcade data ProjectR unpacks (ARCADE2049_DATA, default %LOCALAPPDATA%\\ProjectR\\Data\\sf2049se) and
writes OUT_MOD_DIR/mod.json and OUT_MOD_DIR/tracks/<id>/track.glb + track.toml + logo.png (the name
or --logo-text, in Rush 2049's banner lettering by tools/banner_font.py, as the 2049 tracks' own names are drawn):
  - scenery: worlds.rom instances and dobs.rom objects (flipbooks draw their first frame), vertex colors, one
    material per arcade texture (AYIQ8422 alpha -> MASK, or BLEND where it is soft)
  - SKY: the sky objects (sky textures, or placed past SKY_RADIUS)
  - COL_*: roaddat.rom polygons by surface type
  - FINISH at the finish sign, CHECKPOINT_n at the checkpoint banners
  - AIPATH_main over the drones' centre line, AIPATH_lane1-4 the drones' racing lines, each point with the arcade's
    speed and steering behaviour (_SPEED, _AI_MODE)
  - BILLBOARD_ the cards the arcade turns to the camera (frontface); PROP_KNOCK_ the knock-over cones and
    construction horses (bounce); MOVE_ the spinning objects (spin<axis>, degrees per second [I]) and each flipbook
    frame (flipbook <frames per second>)
  - [track] gravity = "rush2049": the arcade's tracks race on Rush 2049's gravity
  - [track] smooth_collision = false: the arcade's road is built for flat-faced collision (src/arcade2049/arcade_track.cpp
    does the same)
Left out (reported): the coin pickups, sounds, event scripts (the finish sign's swap, the hidden ship).

The game builds this mod itself (Games tab, src/arcade2049/arcade_track.cpp, a C++ port of this file for TRACK4):
change both together and compare them with tools/arcade2049/cpp_test/build.bat (docs/arcade2049.md, "Building it
in the game").
"""
import argparse, io, json, math, os, sys
from collections import defaultdict

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.dirname(HERE))
import arcade  # noqa: E402
from stock_track_glb import Glb, to_gltf, gate_matrix, FEET_PER_METER  # noqa: E402

SKY_TEXTURES = ('GS_SKY', 'GS_APANS', 'AL_MOON')
LANE_MAP = 1.2              # a Rush 2049 lane speed / 1.2 is the Rush 2 lane speed (docs/rush2049_research/cars.md)
SKY_RADIUS = 9000.0          # ft: objects reaching past this from the origin are sky (the track spans about 2,500)
SURFACE = {0: 'ROAD', 1: 'DIRT', 2: 'GRASS', 3: 'WATER', 5: 'WALL', 6: 'WALL', 7: 'KILL'}
# The checkpoint banners (TE_CHKPNT on Mission and Presidio), the finish sign when no FINISHSIGN dob marks it, and
# how far apart along the lap two banners must be to be two checkpoints (a banner's pieces and stacked banners are one).
GATE_TEXTURES = {'MG_CHECK5', 'MG_W_CHECK6', 'TE_CHKPNT'}
FINISH_TEXTURES = {'TE_FINISH'}
GATE_MERGE = 250.0
# The arcade's lap length per track (ProjectR's table 0x1407A9FE4, by track index), which its `when progress <feet>`
# counts laps with: progress = laps x this + feet into the lap (0x140233840; docs/arcade2049.md#dobsrom-dynamic-objects).
# Mission and Presidio have 0 there (no progress events).
ARCADE_LAP_FT = {'TRACK1': 12600.0, 'TRACK2': 18000.0, 'TRACK3': 17000.0, 'TRACK4': 15900.0, 'TRACK5': 19700.0}
GATE_REACH = 120.0   # ft: banner pieces farther from the lap (signs off the course, on Mission) aren't checkpoints
WATER_TEXTURES = {'WATER2'}      # the bay's sheets (A_WATER_RIBS and the water tower's aren't water)
WATER_EDGE = 2500.0          # ft: water collision triangles are split as scenery is (MAX_EDGE)
P_SWAP = np.array([[1, 0, 0], [0, 0, 1], [0, 1, 0]], float)


def png(rgba):
    from PIL import Image
    b = io.BytesIO()
    Image.fromarray(rgba, 'RGBA').save(b, 'PNG', optimize=False)
    return b.getvalue()


def instances(t, report):
    """(object, 3x3 world matrix applied as v @ M, world position, dobs script) for every placed object."""
    out = []
    for name, flags, m, p in t.worlds():
        o = t.object_named(name)
        if o is None:
            report.append('worlds.rom: %s names no object, left out' % name)
            continue
        out.append((o, np.array(m, float).reshape(3, 3), np.array(p, float), []))
    for name, script, m, p in t.dobs():
        o = t.object_named(name)
        if o is None:
            report.append('dobs.rom: %s names no object, left out' % name)
            continue
        # dobs placements are (x, z, y) of world; the objects are modelled in world axes. The matrix's rows are
        # where the object's axes go (points are row vectors, p M): the three GS_PLANEARC flyby arms only keep
        # their planes in the air (140-2,000 ft up) this way; transposed, their circles go 550 ft underground.
        M = P_SWAP @ np.array(m, float).reshape(3, 3) @ P_SWAP
        out.append((o, M, np.array(arcade.to_world_dobs(p), float), script))
    return out


def road_half_width(tris2d, point, tangent, height, reach=25.0):
    """Half width (ft) of the drivable surface across `point`, from the road/dirt collision seen from above."""
    lat = np.array([-tangent[1], tangent[0]])
    def on_road(q):
        for (a, b, c), y in tris2d:
            if abs(y - height) > reach:
                continue
            v0, v1, v2 = c - a, b - a, q - a
            d00, d01, d11, d20, d21 = v0 @ v0, v0 @ v1, v1 @ v1, v2 @ v0, v2 @ v1
            den = d00 * d11 - d01 * d01
            if abs(den) < 1e-9:
                continue
            u = (d11 * d20 - d01 * d21) / den
            v = (d00 * d21 - d01 * d20) / den
            if u >= -1e-6 and v >= -1e-6 and u + v <= 1 + 1e-6:
                return True
        return False
    sides = []
    for s in (1, -1):
        d = 0.0
        while d < 200 and on_road(point + s * (d + 2) * lat):
            d += 2
        sides.append(d)
    return sides


MAX_EDGE = 2500.0            # ft: longer scenery triangle edges are split (the converter draws none over 4000 ft)
MAX_LEAF = 255               # the game's collision queries hold 255 polygons per index cell (include/collision_polys.h;
                             # 36 until 2026-10-20, when relieve_crowding and cut_tips moved corners to fit)
NEAR_FULL = 240              # cells past this get their exactly flat neighbours joined (relieve_crowding)
FLAT = 0.001                 # ft: neighbours this flat are joined without moving anything
CELL = 4.0                   # ft: the converter's smallest cell


def split_large(tri, limit):
    """tri: 3 corners (world position, uv, rgba). Halves the longest edge (uv and color interpolated) until every edge
    is under `limit` ft (0: never)."""
    out, todo = [], [tri]
    while todo:
        t = todo.pop()
        lens = [np.linalg.norm(t[(k + 1) % 3][0] - t[k][0]) for k in range(3)]
        k = int(np.argmax(lens))
        if not limit or lens[k] < limit:
            out.append(t)
            continue
        a, b, c = t[k], t[(k + 1) % 3], t[(k + 2) % 3]
        m = tuple((np.asarray(x) + np.asarray(y)) / 2 for x, y in zip(a, b))
        todo += [[a, m, c], [m, b, c]]
    return out


def crowded_cells(polys, limit=MAX_LEAF):
    """Index cells (CELL ft, as the converter's quadtree ends) where more than MAX_LEAF polygons' outlines (walls
    widened by 6 ft, the rest by 1, as the converter lists them) meet (more than `limit`). Returns {cell: [polygon
    indices]}."""
    from shapely.geometry import Polygon, box
    boxes = defaultdict(list)    # quick pass: polygons by the cells their widened boxes cover
    shapes = []
    for i, (flags, pts, normal) in enumerate(polys):
        m = 6.0 if flags & 0xF in (5, 6) else 1.0
        xs, zs = [p[0] for p in pts], [p[2] for p in pts]
        x0, x1 = math.floor((min(xs) - m) / CELL), math.floor((max(xs) + m) / CELL)
        z0, z1 = math.floor((min(zs) - m) / CELL), math.floor((max(zs) + m) / CELL)
        shapes.append(None)
        if (x1 - x0 + 1) * (z1 - z0 + 1) > 20000:
            continue                 # a huge polygon: counted by its outline only where the quick pass flags a cell
        for cx in range(x0, x1 + 1):
            for cz in range(z0, z1 + 1):
                boxes[(cx, cz)].append(i)
    out = {}
    for (cx, cz), ids in boxes.items():
        if len(ids) <= limit:
            continue
        c = box(cx * CELL, cz * CELL, (cx + 1) * CELL, (cz + 1) * CELL)
        hit = []
        for i in ids:
            if shapes[i] is None:
                f, pts, n = polys[i]
                g = Polygon([(p[0], p[2]) for p in pts])
                if not g.is_valid or g.area < 1e-3:
                    g = g.buffer(0.5)
                shapes[i] = g.buffer(6.0 if f & 0xF in (5, 6) else 1.0)
            if shapes[i].intersects(c):
                hit.append(i)
        if len(hit) > limit:
            out[(cx, cz)] = hit
    return out


def weld(polys, reach=0.1):
    """Snaps corners within `reach` ft to one position: arcade polygons rebuild shared corners from their own origin
    and axes, so neighbours' copies differ by a few hundredths of a foot."""
    from scipy.spatial import cKDTree
    flat = [p for _, pts, _ in polys for p in pts]
    tree = cKDTree(np.array(flat))
    parent = list(range(len(flat)))
    def root(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for a, b in tree.query_pairs(reach):
        ra, rb = root(a), root(b)
        if ra != rb:
            parent[max(ra, rb)] = min(ra, rb)
    out, k = [], 0
    for f, pts, n in polys:
        out.append((f, [flat[root(k + q)] for q in range(len(pts))], n))
        k += len(pts)
    return out


def merge_pair(pa, pb):
    """The polygon of pa and pb joined across their shared edge (corner lists in order), or None."""
    for k in range(len(pa)):
        u, v = pa[k], pa[(k + 1) % len(pa)]
        if u in pb and v in pb:
            m = pb.index(v)
            if pb[(m + 1) % len(pb)] != u:
                continue
            rest = [pb[(m + 2 + q) % len(pb)] for q in range(len(pb) - 2)]   # pb from after u round to before v
            return pa[:k + 1] + rest + pa[k + 1:]
    return None


def convex(poly, nrm):
    P = np.array(poly)
    sign = 0
    for k in range(len(P)):
        c = np.cross(P[(k + 1) % len(P)] - P[k], P[(k + 2) % len(P)] - P[(k + 1) % len(P)]) @ nrm
        if abs(c) < 1e-6:
            continue
        if sign and np.sign(c) != sign:
            return False
        sign = np.sign(c)
    return True


def drop_spline_twins(polys, report):
    """The arcade lists some spline road polygons twice, once with their segment and once flat (the same corners and
    facing; 74 in Presidio's tunnel, a few on Civic and Metro). The spline copy becomes the smooth road (spline_road),
    so the flat one is a second road on it: it is dropped. Besides doubling the road's polygons, the pairs crowd past
    the game's 36 per cell where Presidio's tunnel sections meet. polys: t.collision(with_segment=True).
    arcade_track.cpp drop_spline_twins is the same."""
    key = lambda pts: tuple(sorted(tuple(math.floor(v * 100 + 0.5) for v in p) for p in pts))
    spline = defaultdict(list)
    for f, pts, n, k in polys:
        if k is not None:
            spline[key(pts)].append(np.asarray(n, dtype=float))
    out, dropped = [], 0
    for f, pts, n, k in polys:
        if k is None and any(float(m @ np.asarray(n, dtype=float)) > 0 for m in spline.get(key(pts), ())):
            dropped += 1
            continue
        out.append((f, pts, n, k))
    if dropped:
        report.append('collision: %d flat copies of spline road polygons dropped' % dropped)
    return out


def drop_copies(polys, report):
    """The arcade's collision repeats some polygons exactly (same surface, corners and facing; 105 walls on Presidio).
    The copies change nothing, but share every edge four ways, which keeps relieve_crowding from joining them: a
    corner fan of wall pieces repeated stays past the game's 36 per cell. Keeps the first of each. arcade_track.cpp
    drop_copies is the same."""
    seen, out, n = {}, [], 0
    for f, pts, nrm in polys:
        key = (f, tuple(sorted(pts)))
        if key in seen and float(np.dot(seen[key], nrm)) > 0:
            n += 1
            continue
        seen.setdefault(key, np.asarray(nrm, dtype=float))
        out.append((f, pts, nrm))
    if n:
        report.append('collision: %d exact copies of other polygons dropped' % n)
    return out


APEX_POLYS = 240            # a corner shared by this many polygons is a cone tip (cut_tips; 24 under the old 36)
TIP_CUT = 12.0               # ft across x/z: where a cone tip is cut off


def col_class(f):
    """What keeps two roaddat polygons apart when they are joined: the surface type and the covered bits (flags bits
    30-31, Rush 2's POLY info 0x4000 / 0x8000: covered levels 1 and 2, the cars drawn darker as in its tunnels)."""
    return (f & 0xF) | (f & 0xC0000000)


def covered_level(f):
    return 2 if f & 0x80000000 else 1 if f & 0x40000000 else 0


def cut_tips(polys, report):
    """A corner shared by APEX_POLYS or more polygons of one surface all the way round (the tip of a cone of steep
    wedges, as Haight's pit at 2653, 76, -3305 with 31) puts every one of them in the index cells there, past the
    game's 36, and wedges at an angle can't be joined flat. Each polygon's corner there is replaced by the points TIP_CUT
    ft out (across x/z) along its two edges, the same point for the neighbours sharing an edge (no cracks), and the hole
    closed with a cap of convex pieces of at most 8 corners over the ring of cuts. A fan with a gap of more than 60
    degrees (a corner at the surface's edge) is left alone. arcade_track.cpp cut_tips is the same."""
    at = defaultdict(list)
    for i, (f, pts, n) in enumerate(polys):
        for p in pts:
            if not at[p] or at[p][-1] != i:
                at[p].append(i)
    xz_len = lambda p, c: math.hypot(p[0] - c[0], p[2] - c[2])
    polys = list(polys)
    touched, caps, tips = set(), [], 0
    for c in sorted(at):
        ids = at[c]
        if len(ids) < APEX_POLYS:
            continue
        if any(i in touched or col_class(polys[i][0]) != col_class(polys[ids[0]][0]) or len(polys[i][1]) >= 8 for i in ids):
            continue
        r = TIP_CUT
        for i in ids:
            P = polys[i][1]
            k = P.index(c)
            r = min(r, 0.5 * xz_len(P[(k + 1) % len(P)], c), 0.5 * xz_len(P[(k + len(P) - 1) % len(P)], c))
        if r < 4.0:
            continue
        def cut(q):
            s = r / xz_len(q, c)
            return tuple(c[j] + (q[j] - c[j]) * s for j in range(3))
        ring, cut_pts = [], []
        nsum = np.zeros(3)
        for i in ids:
            f, P, n = polys[i]
            k = P.index(c)
            a, b = cut(P[(k + len(P) - 1) % len(P)]), cut(P[(k + 1) % len(P)])
            cut_pts.append(P[:k] + [a, b] + P[k + 1:])
            for p in (a, b):
                if all(e[1] != p for e in ring):
                    ring.append((math.atan2(p[2] - c[2], p[0] - c[0]), p))
            nsum = nsum + np.asarray(n, dtype=float)
        ring.sort()
        gap = max((ring[k + 1][0] if k + 1 < len(ring) else ring[0][0] + 2 * math.pi) - ring[k][0]
                  for k in range(len(ring))) if ring else 0
        if len(ring) < 3 or gap > math.pi / 3:
            continue
        for n_, i in enumerate(ids):
            polys[i] = (polys[i][0], cut_pts[n_], polys[i][2])
        touched |= set(ids)
        nrm = nsum / (np.linalg.norm(nsum) or 1.0)
        for s in range(1, len(ring) - 1, 6):
            caps.append((polys[ids[0]][0], [ring[0][1]] + [ring[k][1] for k in range(s, min(s + 6, len(ring) - 1) + 1)],
                         nrm))
        tips += 1
    if tips:
        report.append('collision: %d cone tips where %d or more polygons meet cut off and capped' % (tips, APEX_POLYS))
    return polys + caps


def relieve_crowding(polys, report, tolerance=1.0):
    """Where a spot has more collision polygons than the game's index cell holds (a finely tessellated curved tunnel
    wall), joins neighbouring polygons of a surface into one flat convex polygon of at most 8 corners (the converter's
    own rule): the second's other corners move onto the first's plane (everywhere they are used, so no cracks open)
    when that is under `tolerance` ft, then twice and three times that while spots stay crowded. Every face is kept.
    First, where a cell is near full (NEAR_FULL), exactly flat neighbours are joined, nothing moved: the converter's
    cells don't line up with ours and split its polygons its own way, so it can count a few more than we do (Presidio
    Arcade's tunnel portal, 13 pieces of one flat ring wall). Returns the polygons (corners welded) with those merges."""
    polys = cut_tips(drop_copies(weld([(f, [tuple(p) for p in pts], n) for f, pts, n in polys]), report), report)
    total, worst, flat_joins = 0, 0.0, 0
    steps = [(FLAT, NEAR_FULL)] * 6 + [(tolerance, MAX_LEAF)] * 6 + [(tolerance * 2, MAX_LEAF)] * 3 +         [(tolerance * 3, MAX_LEAF)] * 3
    for tol, limit in steps:
        cells = crowded_cells(polys, limit)
        if not cells:
            if limit < MAX_LEAF:
                continue
            break
        crowd = {i for ids in cells.values() for i in ids}
        edges = defaultdict(list)
        for i in range(len(polys)):
            pts = polys[i][1]
            for k in range(len(pts)):
                edges[frozenset((pts[k], pts[(k + 1) % len(pts)]))].append(i)
        cands = []
        for e, ps in edges.items():
            if len(ps) != 2 or col_class(polys[ps[0]][0]) != col_class(polys[ps[1]][0]) or not (set(ps) & crowd):
                continue
            a, b = ps
            A = np.array(polys[a][1])
            nrm = np.cross(A[1] - A[0], A[2] - A[0])
            nrm /= np.linalg.norm(nrm) or 1.0
            devs = {p: float((np.array(p) - A[0]) @ nrm) for p in polys[b][1] if p not in e}
            d = max([abs(v) for v in devs.values()] or [0.0])
            if d < tol:
                cands.append((d, a, b, devs, nrm))
        moved, locked, joined, gone = {}, set(), {}, set()
        for d, a, b, devs, nrm in sorted(cands, key=lambda c: c[0]):
            if a in locked or b in locked or any(p in moved for p in devs):
                continue
            new_pos = {p: tuple(np.array(p) - dv * nrm) for p, dv in devs.items()}
            m = merge_pair(polys[a][1], polys[b][1])
            if m is None or len(m) > 8 or not convex([new_pos.get(p, p) for p in m], nrm):
                continue
            moved.update(new_pos)
            locked |= {a, b}
            joined[a] = m
            gone.add(b)
            worst = max(worst, d)
        if not joined:
            continue
        if limit < MAX_LEAF:
            flat_joins += len(joined)
        else:
            total += len(joined)
        polys = [(f, [moved.get(p, p) for p in joined.get(i, pts)], n) for i, (f, pts, n) in enumerate(polys)
                 if i not in gone]
    if flat_joins:
        report.append('collision: %d flat polygon pairs joined where more than %d crowd a cell' % (flat_joins, NEAR_FULL))
    if total:
        report.append("collision: %d polygon pairs in spots over the game's %d polygons per cell joined flat (corners "
                      "moved %.2f ft at most)" % (total, MAX_LEAF, worst))
    left = crowded_cells(polys)
    if left:
        report.append('collision: still %d spots over %d polygons (worst %d)' % (
            len(left), MAX_LEAF, max(len(v) for v in left.values())))
    return polys


SPLINE_WINDOW = 30           # segments either side of a polygon's own that can hold part of it
SPLINE_FIT = 1.0             # ft: a polygon piece is on a segment's surface when its facet is this close to it


def spline_road(t, polys, report):
    """The arcade's spline road as the game's (ROAD_SPLINES, docs/custom_tracks.md): Rush 2 and the N64 take a
    spline polygon's height from its segment (collision_seg_surface_8006BDA8), not from the polygon, which only
    marks where. The arcade's spline polygons are long facets (a crease every 11 ft of lap, 2.5 degrees typical: a
    stream of bumps when driven flat) and many reach well past their own segment, which the arcade copes with and
    Rush 2 can't (it extrapolates the one segment named). So each segment gets the part of the spline road over its
    stretch (between its start line and its next segment's), joined into one flat footprint polygon per segment,
    and takes its height from it there. polys: t.collision(with_segment=True). Returns (the segments' 33-float
    records, [(flags, [world xyz...], segment)] footprint triangles, the polygons left flat), the rest of a spline
    polygon no segment's stretch covers (chain ends) staying flat."""
    from shapely.geometry import Polygon
    from shapely.ops import unary_union
    import shapely
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'rush2049'))
    import collision as C
    records = t.segments()
    segs = [C.seg_fields(r) for r in records]
    ns = len(segs)
    nxt = lambda k: segs[(k + 1) % ns]

    def band(k):
        # Segment k's stretch from above: its start line to its next one's, well past the road's edges (u -1.5..2.5).
        S, T = segs[k], nxt(k)
        h, tt, u = C.seg_surface(S, T, [T['O']])
        if not (abs(tt[0] - 1) < 0.05 and abs(u[0] - 0.5) < 0.05):
            return None         # the next record isn't this one's continuation (a chain's end)
        line = lambda G, u: (G['O'] + G['30'] * (u - 0.5) * G['54'])[[0, 2]]
        b = Polygon([line(S, -1.5), line(S, 2.5), line(T, 2.5), line(T, -1.5)])
        return b if b.is_valid and b.area > 0.01 else None
    bands = [band(k) for k in range(ns)]

    def facet_y(pts, normal, x, z):
        return pts[0][1] - (normal[0] * (x - pts[0][0]) + normal[2] * (z - pts[0][2])) / normal[1]

    pieces = defaultdict(list)      # (segment, flags) -> footprint parts
    flat = []
    for flags, pts, normal, seg in polys:
        if seg is None or not flags & 0x1000 or flags & 0xF not in (0, 1, 2) or normal[1] < 0.2:
            flat.append((flags, pts, normal))
            continue
        rest = Polygon([(p[0], p[2]) for p in pts])
        if not rest.is_valid:
            rest = rest.buffer(0)
        for k in sorted(range(max(0, seg - SPLINE_WINDOW), min(ns, seg + SPLINE_WINDOW + 1)), key=lambda k: abs(k - seg)):
            if bands[k] is None or rest.is_empty:
                continue
            part = rest.intersection(bands[k])
            if part.area < 0.01:
                continue
            c = part.representative_point()
            h, tt, u = C.seg_surface(segs[k], nxt(k), [(c.x, facet_y(pts, normal, c.x, c.y), c.y)])
            if abs(h[0]) < SPLINE_FIT and -0.01 <= tt[0] <= 1.01:
                pieces[(k, (flags & 0xFF) | (flags & 0xC0000000))].append(part)
                rest = rest.difference(bands[k])
        for left in getattr(rest, 'geoms', [rest]):
            if left.geom_type == 'Polygon' and left.area > 0.05:
                for tri in shapely.constrained_delaunay_triangles(left).geoms:
                    xz = list(tri.exterior.coords)[:3]
                    flat.append((flags, [(x, facet_y(pts, normal, x, z), z) for x, z in xz], normal))

    def surface_y(k, x, z, y):
        # The height of segment k's surface over (x, z): the lift along the normal, solved for y.
        for _ in range(4):
            h0 = C.seg_surface(segs[k], nxt(k), [(x, y, z)])[0][0]
            h1 = C.seg_surface(segs[k], nxt(k), [(x, y + 1.0, z)])[0][0]
            if abs(h1 - h0) < 1e-6:
                break
            y -= h0 / (h1 - h0)
        return y

    tris = []
    off = []
    for (k, flags), parts in sorted(pieces.items()):
        # On the file's 1/32 ft grid: corners a hair apart become one (no slivers), and the border two footprints
        # share snaps the same on both sides (no cracks).
        joined = shapely.set_precision(unary_union(parts), 1.0 / 32)
        for part in getattr(joined, 'geoms', [joined]):
            if part.geom_type != 'Polygon' or part.area < 0.01:
                continue
            # One plane per footprint (fitted to the surface at its corners) so its triangles join back into the
            # game's convex polygons; the height comes from the segment, the plane only has to be near it.
            xz = np.array(part.exterior.coords[:-1])
            ys = np.array([surface_y(k, x, z, segs[k]['O'][1]) for x, z in xz])
            A = np.c_[xz, np.ones(len(xz))]
            coef = np.linalg.lstsq(A, ys, rcond=None)[0]
            off.append(np.abs(A @ coef - ys).max())
            for tri in shapely.constrained_delaunay_triangles(part).geoms:
                if tri.area < 0.01:
                    continue
                tri_xz = list(tri.exterior.coords)[:3]
                tris.append((flags | 0x1000, [(x, coef[0] * x + coef[1] * z + coef[2], z) for x, z in tri_xz], k))
    report.append('spline road: %d segments, %d footprints from %d spline polygons (planes within %.2f ft of the '
                  'surface), %d flat leftovers' % (ns, len(off), sum(1 for p in polys if p[3] is not None and p[0] & 0x1000),
                                                   max(off) if off else 0, sum(1 for f in flat if f[0] & 0x1000)))
    return records, tris, flat


def build(track, out_dir, track_id, name, mod_id, finish_at=None, logo_text=None, preview=None, after='rush2049'):
    t = arcade.Track(track)
    report = []
    g = Glb()
    g.js['asset']['generator'] = 'rush2-recomp tools/arcade2049/track_mod.py'

    # Materials: one per arcade texture, made on first use.
    mats = {}

    def material(ti):
        if ti in mats:
            return mats[ti]
        tex = t.textures[ti]
        rgba = tex.rgba(t.texels)
        mode = 'OPAQUE'
        if tex.fmt == arcade.FMT_AYIQ8422:
            a = rgba[..., 3]
            soft = np.mean((a > 16) & (a < 240))
            mode = 'BLEND' if soft > 0.05 else 'MASK'
        wrap = (33071 if tex.clamp_s else 10497, 33071 if tex.clamp_t else 10497)
        mats[ti] = g.material(tex.name, g.image(tex.name, png(rgba)), mode, True, wrap)
        return mats[ti]

    scenery = defaultdict(lambda: ([], [], [], [], []))
    sky = defaultdict(lambda: ([], [], [], [], []))
    sky_names = {o.name for o in t.objects
                 if any(t.textures[tr[3]].name.startswith(SKY_TEXTURES) for tr in o.lods[0].tris)}
    # Most arcade skies are a ring of wall panels under a flat ceiling, centered off the origin, so a few near-side
    # panels reach less than SKY_RADIUS; as scenery the draw distance cut them off and left a gap in the sky. A texture
    # that at least two far objects use, and that far objects make up at least half the users of, is a sky texture,
    # and every object using it is sky (as src/arcade2049/arcade_track.cpp).
    users = defaultdict(lambda: [0, 0])
    placed = list(instances(t, []))
    for o, M, pos, _ in placed:
        lod = o.lods[0]
        if not lod.verts:
            continue
        V = np.array([v[0] for v in lod.verts], float) @ M + pos
        water = all(ti < len(t.textures) and t.textures[ti].name in WATER_TEXTURES for *_, ti in lod.tris)
        far = np.max(np.linalg.norm(V[:, [0, 2]], axis=1)) > SKY_RADIUS and not water
        for ti in {tr[3] for tr in lod.tris if tr[3] < len(t.textures)}:
            users[ti][0] += 1
            users[ti][1] += far
    sky_tex = {ti for ti, (n, f) in users.items() if f >= 2 and f * 2 >= n}
    sky_names |= {o.name for o, *_ in placed if any(tr[3] in sky_tex for tr in o.lods[0].tris)}
    tri_count = 0

    def add_tris(target, lod, V, limit):
        """lod's triangles at the world points V into target (material -> primitive lists); returns the count."""
        count = 0
        for a, b, c, ti in lod.tris:
            if ti >= len(t.textures):
                continue
            verts, nrms, uvs, cols, tris = target[material(ti)]
            su, sv = t.textures[ti].uv_scale()
            # Mirroring x (to_gltf) flips the winding, so b and c swap to keep each face's front.
            corner = [(V[k], (lod.verts[k][1][0] / su, lod.verts[k][1][1] / sv), np.array(lod.verts[k][3]) / 255.0)
                      for k in (a, c, b)]
            for tri in split_large(corner, limit):
                gl = [to_gltf(p) for p, _, _ in tri]
                n = np.cross(np.subtract(gl[1], gl[0]), np.subtract(gl[2], gl[0]))
                n = n / (np.linalg.norm(n) or 1.0)
                base = len(verts)
                for k, (p, uv, col) in enumerate(tri):
                    verts.append(gl[k])
                    nrms.append(tuple(n))
                    uvs.append(tuple(uv))
                    cols.append(tuple(col))
                tris.append((base, base + 1, base + 2))
                count += 1
        return count

    billboards = 0

    def object_collision(lod, P, road, wall):
        """An object's collision (dobs `collision`): its model's triangles at the points P (world, or a mover's own
        frame), up-facing ones road and the rest walls, each kept on its drawn side (glTF space)."""
        for a, b, c, _ in lod.tris:
            if max(a, b, c) >= len(P):
                continue
            gl = [np.array(to_gltf(P[k]), float) for k in (a, c, b)]     # the drawn winding
            n = np.cross(gl[1] - gl[0], gl[2] - gl[0])
            if np.linalg.norm(n) < 1e-9:
                continue
            n = n / np.linalg.norm(n)
            verts, nrms, tris = road if n[1] > 0.7 else wall
            base = len(verts)
            verts += [tuple(x) for x in gl]
            nrms += [tuple(n)] * 3
            tris.append((base, base + 1, base + 2))

    def box_mesh(corner, top, sides=None):
        """A box (8 glTF-space corners, bit 0/1/2 = the high end of axis 0/1/2) as 12 outward triangles: up-facing faces
        into top, the rest into sides (into top when there is none)."""
        mid = np.mean(corner, axis=0)
        for f in ((2, 6, 7, 3), (0, 1, 5, 4), (0, 4, 6, 2), (1, 3, 7, 5), (0, 2, 3, 1), (4, 5, 7, 6)):
            q = [np.array(corner[k], float) for k in f]
            n = np.cross(q[1] - q[0], q[2] - q[0])
            if np.linalg.norm(n) < 1e-12:
                continue
            if np.dot(n, (q[0] + q[2]) * 0.5 - mid) < 0:
                q[1], q[3], n = q[3], q[1], -n
            n = n / np.linalg.norm(n)
            verts, nrms, tris = top if n[1] > 0.7 or sides is None else sides
            base = len(verts)
            verts += [tuple(x) for x in q]
            nrms += [tuple(n)] * 4
            tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)]

    def aabb_corners(lo, hi):
        return [(hi[0] if k & 1 else lo[0], hi[1] if k & 2 else lo[1], hi[2] if k & 4 else lo[2]) for k in range(8)]

    object_road, object_wall = ([], [], []), ([], [], [])

    # Trigger boxes: hidden dobs objects with a `name` (UNITCUBE mostly), which other objects' `when <name>` wait for a
    # car to drive into.
    trigger_boxes = {}
    lap_pts = np.array(t.lap(), float)
    lap_at = np.concatenate([[0.0], np.cumsum(np.linalg.norm(np.diff(np.vstack([lap_pts, lap_pts[:1]]), axis=0), axis=1))])
    lap_length = float(lap_at[-1])
    placed_all = list(instances(t, report))
    for o, M, pos, script in placed_all:
        words = [s.split() for s in script if s.split()]
        tname = next((w[1].upper() for w in words if w[0].lower() == 'name' and len(w) > 1), None)
        if tname and any(w[0].lower() == 'hide' for w in words) and o.lods[0].verts:
            V = np.array([v[0] for v in o.lods[0].verts], float) @ M + pos
            trigger_boxes[tname] = (V.min(axis=0), V.max(axis=0))
    unknown_events, used_triggers = set(), set()

    def mod_event(w):
        """An arcade event (mg1, progress 14000, 3, oddlap, finishswap) as a track.toml event, or None."""
        if not w:
            return None
        a = w[0].lower()
        if a == 'progress' and len(w) > 1:
            # The leader that far into the race: laps x the arcade's lap length (ARCADE_LAP_FT) + feet into the lap
            # [V]; the same lap and point on the converted lap, whose length differs a little.
            arcade_lap = ARCADE_LAP_FT.get(os.path.basename(os.path.normpath(t.folder)).upper(), 0)
            d = float(w[1])
            if arcade_lap <= 0 or lap_length <= 0:
                unknown_events.add(' '.join(w))
                return None
            laps = math.floor(d / arcade_lap)
            return 'progress %g' % round(laps * lap_length + (d - laps * arcade_lap), 1)
        if a and all(ch.isdigit() or ch == '.' for ch in a):
            return 'time ' + w[0]                 # seconds into the race [V]
        if a in ('oddlap', 'evenlap', 'finishswap', 'lastlap'):
            # finishswap: player 1 past the race's last checkpoint before the finish [V]; lastlap: the last lap started.
            return {'oddlap': 'odd_lap', 'evenlap': 'even_lap', 'finishswap': 'last_checkpoint', 'lastlap': 'final_lap'}[a]
        if w[0].upper() in trigger_boxes:
            used_triggers.add(w[0].upper())
            return 'trigger ' + w[0].upper()
        unknown_events.add(w[0])
        return None

    def toml_list(v):
        return '[' + ', '.join('"%s"' % e for e in v) + ']'

    def toml_number(v):
        s = '%g' % v
        return s if ('.' in s or 'e' in s) else s + '.0'

    # (name, [(lod, local points)], world position, key times, rotations or None, scales or None, translations or None,
    #  extras: card, collision, M, settings)
    movers = []
    still = []
    placed_props = defaultdict(int)
    for o, M, pos, script in placed_all:
        words = {s.split()[0].lower() for s in script}
        lod = o.lods[0]
        L = np.array([v[0] for v in lod.verts], float)
        if 'coin' in words or o.name.upper().startswith('GOLDCOIN'):
            # The arcade's coin pickups: left out (mods have no collectibles; Rush 2049's coins are its hidden
            # collectibles, a different thing).
            still.append('%s (a coin pickup: left out)' % o.name)
            continue
        # The script's motion and events (docs/arcade2049.md#dobsrom-dynamic-objects): follow<axis> <speed> <distance>
        # [<acceleration>] moves it along its own axis; start / open / close take a `when <event>` on the same line or
        # the next `when` line.
        follow = None
        start_ev, open_ev, close_ev = [], [], []
        # `collision` anywhere in the script, as the arcade reads it word by word ('followy 60 1400 22 collision').
        collision = any(x.lower() == 'collision' for ln in script for x in ln.split())
        first_close, seen_door = False, False
        open_delay = close_delay = -1.0
        pending = None
        for s in script:
            w = s.split()
            if not w:
                continue
            a = w[0].lower()
            if a.startswith('follow') and len(a) >= 7 and len(w) >= 3:
                axis = np.array({'x': (1, 0, 0), 'y': (0, 0, 1), 'z': (0, 1, 0)}[a[-1]], float) @ M
                if np.linalg.norm(axis) > 0:
                    axis = axis / np.linalg.norm(axis) * (-1.0 if '-' in a else 1.0)
                    speed, dist = float(w[1]), float(w[2])
                    if speed and dist > 0:
                        follow = (axis, speed, dist, float(w[3]) if len(w) > 3 else 0.0)
                continue
            if a == 'start':
                low = [x.lower() for x in w]
                if 'when' in low:
                    e = mod_event(w[low.index('when') + 1:])
                    if e:
                        start_ev.append(e)
                    pending = None
                else:
                    pending = start_ev
            elif a in ('open', 'close') and len(w) > 1:
                if w[1].lower() == 'delay' and len(w) > 2:
                    if a == 'open':
                        open_delay = float(w[2])
                    else:
                        close_delay = float(w[2])
                elif w[1].lower() == 'when':
                    # The first event line says where it starts: a close first means it starts open.
                    if not seen_door:
                        first_close, seen_door = a == 'close', True
                    e = mod_event(w[2:])
                    if e:
                        (open_ev if a == 'open' else close_ev).append(e)
            elif a == 'when' and pending is not None:
                e = mod_event(w[1:])
                if e:
                    pending.append(e)
                pending = None
        door = bool(open_ev or close_ev)
        # bounce: knocked flying (PROP_KNOCK_, the construction horses and cones). Each in its own frame at its
        # placement.
        kind = 'KNOCK' if 'bounce' in words else None
        if kind or ('frontface' in words and follow is None):
            local = L @ M
            prims = defaultdict(lambda: ([], [], [], [], []))
            add_tris(prims, lod, local, 0.0)
            prims = [(m,) + v for m, v in prims.items()]
            card = 'frontface' in words
            if card:
                # A card the arcade turns to the camera: the game keeps it upright, turned about y,
                # its scale kept, its face (normal) toward glTF +z = Blender's front view, the side the converter shows.
                nz = sum(n[2] for _, _, nrms, _, _, _ in prims for n in nrms)
                if nz < 0:      # half a turn about y
                    prims = [(m, [(-x, y, -z) for x, y, z in vs], [(-x, y, -z) for x, y, z in ns], uv, col, tr)
                             for m, vs, ns, uv, col, tr in prims]
            node_name = ('PROP_%s_%s' % (kind, o.name) if kind else 'BILLBOARD_' + o.name) + '.%03d' % billboards
            node = g.textured_mesh(node_name, prims, trs=(list(to_gltf(pos)), [0, 0, 0, 1], [1, 1, 1]))
            if kind and card:
                g.js['nodes'][node]['extras'] = {'billboard': True}
            billboards += 1
            tri_count += len(lod.tris)
            if 'flipbook' in words:
                still.append(o.name + ' (a flipbook billboard: its first frame)')
            if kind:
                placed_props[kind] += 1
            continue
        if 'hide' in words:
            # Hidden: a trigger box (above) or a sound's position; never drawn.
            if not any(s.lower().startswith('name ') for s in script):
                still.append(o.name + " (hidden: a sound's position; the arcade's sounds aren't converted)")
            continue
        if o.name.upper() == 'UNITCUBE':
            # The unit cube marks volumes (soundbox: where a sound plays); it is never drawn (on Marina a 495 ft blue
            # block over the road).
            still.append("UNITCUBE (a sound volume: not drawn; the arcade's sounds aren't converted)")
            continue
        V = L @ M + pos
        # The bay's water sheets reach 10,700 ft out, past what the game draws (2,000 ft): scenery for the near part
        # (depth tested against the island) and sky too, where the far part meets the distant land (GS_APANS, 9,750
        # ft) as in the arcade. The sky sits at (1 - its scale) x the camera's height (src/rush2049/track2049.cpp
        # rush2_track49_sky_height), so both keep their true angles below the horizon.
        is_water = all(ti < len(t.textures) and t.textures[ti].name in WATER_TEXTURES for *_, ti in lod.tris)
        far = np.max(np.linalg.norm(V[:, [0, 2]], axis=1)) > SKY_RADIUS
        is_sky = o.name in sky_names or 'CLOUDS' in o.name or (far and not is_water)
        if is_water and far:
            add_tris(sky, lod, V, 0.0)
        spin = [s.split() for s in script if s.lower().startswith(('spinx', 'spiny', 'spinz'))]
        flip = [s.split() for s in script if s.lower().startswith('flipbook')]

        def settings(free_mode):
            """How a moving object plays: open / close events make a door (it starts open when its first event is a
            close), a start event waits for it, the rest go back and forth (follow) or loop (spin)."""
            if door:
                s = 'mode = "door"\n'
                if open_ev:
                    s += 'open = %s\n' % toml_list(open_ev)
                if close_ev:
                    s += 'close = %s\n' % toml_list(close_ev)
                if open_delay >= 0:
                    s += 'open_delay = %s\n' % toml_number(open_delay)
                if close_delay >= 0:
                    s += 'close_delay = %s\n' % toml_number(close_delay)
                if first_close:
                    s += 'open_at_start = true\n'
                return s
            s = 'mode = "%s"\n' % free_mode
            if start_ev:
                s += 'start = %s\n' % toml_list(start_ev)
            return s

        if not is_sky and follow is not None:
            # follow: along the axis at the speed (ft/s), the distance (ft) from its placement, speeding up and slowing
            # down at the acceleration (ft/s², [I]: none = a steady speed).
            axis, speed, D, acc = follow
            v, direction = abs(speed), axis * (-1.0 if speed < 0 else 1.0)
            times, offsets = [], []

            def key(tt, s):
                times.append(tt)
                offsets.append(direction * s)
            if acc <= 0:
                key(0.0, 0.0)
                key(D / v, D)
            else:
                ta = v / acc
                da = 0.5 * acc * ta * ta
                if 2 * da > D:
                    ta = math.sqrt(D / acc)
                    da, v = D / 2, acc * ta
                tc = (D - 2 * da) / v
                for k in range(7):
                    tt = ta * k / 6
                    key(tt, 0.5 * acc * tt * tt)
                if tc > 1e-6:
                    key(ta + tc, da + v * tc)
                for k in range(1, 7):
                    tt = ta * k / 6
                    key(ta + tc + tt, D - da + v * tt - 0.5 * acc * tt * tt)
            movers.append(('%s_%d' % (o.name, len(movers)), [(lod, L @ M)], pos, times, None, None,
                           [tuple(to_gltf(pos + d)) for d in offsets],
                           dict(card='frontface' in words, collision=(2 if 'frontface' in words else 1) if collision else 0,
                                M=M, settings=settings('pingpong'))))
            tri_count += len(lod.tris)
            continue
        if not is_sky and spin:
            # spin<axis> <degrees per second> [I]: a looping MOVE_ turn about the object's origin, about the
            # object's own axis as its placement matrix turns it (GS_PLANEARC: planes 2,500 ft out on a tilted arm
            # spin about its x; about world x they only rolled in place). The script's x, y, z are world x, z, y
            # (to_world_dobs), and the x mirror (to_gltf) flips the axis's x and reverses the turn.
            axis = np.array({'x': (1, 0, 0), 'y': (0, 0, 1), 'z': (0, 1, 0)}[spin[0][0][-1].lower()], float) @ M
            axis = (-axis[0], axis[1], axis[2]) / np.linalg.norm(axis)
            rate = float(spin[0][1]) if len(spin[0]) > 1 else 0.0
            if rate:
                period = 360.0 / abs(rate)
                times, quats = [], []
                for k in range(5):
                    a = -math.copysign(1, rate) * math.pi * k / 2   # quarter turns
                    times.append(period * k / 4)
                    quats.append([axis[0] * math.sin(a / 2), axis[1] * math.sin(a / 2), axis[2] * math.sin(a / 2),
                                  math.cos(a / 2)])
                movers.append(('%s_%d' % (o.name, len(movers)), [(lod, L @ M)], pos, times, quats, None, None,
                               dict(card=False, collision=1 if collision else 0, M=M,
                                    settings=settings('loop') if start_ev else '')))
                tri_count += len(lod.tris)
                continue
        fps = float(flip[0][1]) if flip and len(flip[0]) > 1 else 0.0
        # An event flipbook (negative rate: breaking glass and signs, the finish sign) plays once when its start event
        # comes, then keeps its last frame.
        event_book = fps < 0 and bool(start_ev)
        if not is_sky and (fps > 0 or event_book):
            # flipbook <frames per second>: frames <name>_1..n, each a MOVE_ object at full size only in its slot
            # (0.001 otherwise: a rest pose of 0 can't be undone), one after the other, looping.
            base = o.name.rsplit('_', 1)[0] if o.name.rsplit('_', 1)[-1].isdigit() else o.name
            frames = []
            while base + '_%d' % (len(frames) + 1) in t.by_name:
                frames.append(t.by_name[base + '_%d' % (len(frames) + 1)])
            if len(frames) > 1:
                rate = abs(fps)
                period, eps = len(frames) / rate, 0.002
                last = len(frames) - 1
                # One pivot for the whole book, its frames' geometry center on a whole foot: their vertices then round
                # to the scenery's 1/16 ft grid and their edges meet the walls around them (BLINK3's placement is
                # the world origin, 1,000 ft away, and left hairline cracks). Shared, the frame shown has the matrix
                # of the one it replaces, and the game draws a book's frames under one interpolation ID
                # (src/rush2049/track2049_movers.cpp): with a pivot each, every frame started unblended and shook.
                pivot = np.round(np.concatenate([np.array([v[0] for v in fr.lods[0].verts], float) @ M
                                                 for fr in frames]).mean(axis=0) + pos)
                for j, fr in enumerate(frames):
                    s0, s1 = j / rate, (j + 1) / rate
                    keys = [(0.0, j == 0)]
                    if j:
                        keys += [(s0 - eps, False), (s0, True)]
                    if event_book:
                        # Played once: the last frame stays.
                        keys += [(period, True)] if j == last else [(s1 - eps, True), (s1, False), (period, False)]
                    else:
                        keys += [(s1 - eps, True), (s1, False)]
                        # Every frame's keys end on key 0's pose so the loop closes in exactly `period` (an open loop
                        # adds 0.001 s back to key 0; the first frame drifted out of step and every frame of the
                        # blinkers, which carry their walls, was hidden at times).
                        if j == 0:
                            keys += [(period - eps, False), (period, True)]
                        elif s1 < period - eps:
                            keys.append((period, False))
                    keys = [k for i, k in enumerate(keys) if i == 0 or k[0] > keys[i - 1][0]]
                    frv = np.array([v[0] for v in fr.lods[0].verts], float) @ M
                    movers.append(('%s_%d' % (fr.name, len(movers)), [(fr.lods[0], frv + pos - pivot)], pivot,
                                   [k[0] for k in keys], None, [[1.0] * 3 if k[1] else [0.001] * 3 for k in keys], None,
                                   dict(card=False, collision=0, M=M,
                                        settings='mode = "once"\nstart = %s\n' % toml_list(start_ev) if event_book else '')))
                    tri_count += len(fr.lods[0].tris)
                continue
        if flip or spin:
            still.append(o.name + (' (sky: it stays still)' if is_sky else ' (a flipbook with no event: its first frame)'))
        if collision and not is_sky:
            object_collision(lod, V, object_road, object_wall)
        n = add_tris(sky if is_sky else scenery, lod, V, 0.0 if is_sky else MAX_EDGE)
        tri_count += 0 if is_sky else n
    g.textured_mesh('TRACK', [(m,) + v for m, v in scenery.items()])
    mover_toml = ''
    moving_collision = 0
    for mover_name, parts, pos, times, quats, scales, translations, ex in movers:
        prims = defaultdict(lambda: ([], [], [], [], []))
        for lod, local in parts:
            add_tris(prims, lod, local, 0.0)
        prims = [(m,) + v for m, v in prims.items()]
        if ex['card']:
            # A moving card: drawn turned to the camera (billboard = true), facing glTF +z as BILLBOARD_ cards do.
            nz = sum(n[2] for _, _, nrms, _, _, _ in prims for n in nrms)
            if nz < 0:
                prims = [(m, [(-x, y, -z) for x, y, z in vs], [(-x, y, -z) for x, y, z in ns], uv, col, tr)
                         for m, vs, ns, uv, col, tr in prims]
        node = g.textured_mesh('MOVE_' + mover_name, prims, trs=(list(to_gltf(pos)), [0, 0, 0, 1], [1, 1, 1]))
        if ex['card']:
            g.js['nodes'][node]['extras'] = {'billboard': True}
        g.animate(mover_name, node, times, translation=translations, rotation=quats, scale=scales)
        if ex['collision']:
            # Its collision moves with it: COL_ meshes under MOVE_, in its frame.
            road, wall = ([], [], []), ([], [], [])
            lod, local = parts[0]
            if ex['collision'] == 2:
                q = np.array([to_gltf(p) for p in local], float)
                lo, hi = q.min(axis=0), q.max(axis=0)
                half = max(hi[0] - lo[0], hi[2] - lo[2]) / 2
                cx, cz = (lo[0] + hi[0]) / 2, (lo[2] + hi[2]) / 2
                box_mesh(aabb_corners((cx - half, lo[1], cz - half), (cx + half, hi[1], cz + half)), road, wall)
            else:
                object_collision(lod, np.array([v[0] for v in lod.verts], float) @ ex['M'], road, wall)
            for part, surf in ((road, 'ROAD'), (wall, 'WALL')):
                if part[2]:
                    g.mesh('COL_%s_%s' % (surf, mover_name), part[0], part[1], part[2], parent=node)
            moving_collision += 1
        if ex['settings']:
            mover_toml += '\n[movers.%s]\n%s' % (mover_name, ex['settings'])
    if movers:
        report.append('%d moving objects (MOVE_): spinning and sliding objects, trains, doors and flipbook frames; %d '
                      'with collision' % (len(movers), moving_collision))
    if moving_collision > 31:
        report.append("more than 31 moving objects with collision: the game keeps the first 31's")
    for name in sorted(trigger_boxes):
        if name not in used_triggers:
            continue
        lo, hi = trigger_boxes[name]
        a, b = to_gltf(lo), to_gltf(hi)
        m = ([], [], [])
        box_mesh(aabb_corners((min(a[0], b[0]), a[1], a[2]), (max(a[0], b[0]), b[1], b[2])), m)
        g.mesh('TRIGGER_' + name, m[0], m[1], m[2])
    # Objects that don't move but have collision (a parked trolley, a fire truck).
    if object_road[2]:
        g.mesh('COL_ROAD_objects', *object_road)
    if object_wall[2]:
        g.mesh('COL_WALL_objects', *object_wall)
    for e in sorted(unknown_events):
        report.append('event "%s" isn\'t known: left out' % e)
    for s in sorted(set(still)):
        report.append('still: ' + s)
    if sky:
        # The sky is drawn without depth in material order: the dome, the far water over it, the land on the water,
        # then the clouds.
        def sky_layer(m):
            name = g.js['materials'][m]['name']
            return (3 if 'CLOUD' in name.upper() else 2 if name.startswith('GS_APANS') else
                    1 if name in WATER_TEXTURES else 0)
        g.textured_mesh('SKY', [(m,) + sky[m] for m in sorted(sky, key=sky_layer)])

    # Collision.
    seg_records, spline_tris, flat = spline_road(t, drop_spline_twins(t.collision(with_segment=True), report), report)
    col_polys = [(f, p, n, -1) for f, p, n in relieve_crowding(flat, report)]
    for flags, tri, k in spline_tris:
        up = np.cross(np.subtract(tri[1], tri[0]), np.subtract(tri[2], tri[0]))
        col_polys.append((flags, tri, up if up[1] > 0 else -up, k))     # footprints face up
    by_type = defaultdict(lambda: ([], [], [], []))
    road2d = []
    up_road = []
    for flags, pts, normal, seg in col_polys:
        normal = np.asarray(normal, float) / (np.linalg.norm(normal) or 1)
        ty = flags & 0xF
        surface = SURFACE.get(ty)
        if surface is None:
            report.append('collision type %d: made WALL' % ty)
            surface = 'WALL'
        if ty == 0:
            up_road.append(normal[1])
        for k in range(1, len(pts) - 1):
            tri = [pts[0], pts[k], pts[k + 1]]
            if ty in (0, 1, 2):
                road2d.append(((np.array(tri[0])[[0, 2]], np.array(tri[1])[[0, 2]], np.array(tri[2])[[0, 2]]),
                               np.mean([p[1] for p in tri])))
            # The converter's front (game space) is -(v1-v0)x(v2-v0); keep it on the polygon's normal side.
            if np.dot(np.cross(np.subtract(tri[1], tri[0]), np.subtract(tri[2], tri[0])), normal) > 0:
                tri[1], tri[2] = tri[2], tri[1]
            cover = covered_level(flags)
            verts, nrms, tris, segv = by_type['%s_covered%d' % (surface, cover) if cover else surface]
            base = len(verts)
            verts += [to_gltf(p) for p in tri]
            nrms += [(-normal[0], normal[1], normal[2])] * 3
            segv += [seg] * 3
            tris.append((base, base + 1, base + 2))
    # The bay: roaddat has no water polygons (a car that leaves the road sinks to the sea floor, which is road and
    # wall), so the arcade must drown cars some other way. The water sheets themselves become COL_WATER, which wrecks
    # and resets the car as Rush 2's own Alcatraz water does. No road polygon or lap point is under them.
    water = [k for k, tx in enumerate(t.textures) if tx.name in WATER_TEXTURES]
    sheets = 0
    for inst_name, _, m, pos in t.worlds():
        o = t.object_named(inst_name)
        if o is None:
            continue
        L = o.lods[0]
        for tri in L.tris:
            if tri[3] not in water:
                continue
            corners = [(np.add(L.verts[k][0], pos), (0.0, 0.0), (0.0, 0.0, 0.0, 0.0)) for k in tri[:3]]
            for part in split_large(corners, WATER_EDGE):
                p = [tuple(c[0]) for c in part]
                normal = np.cross(np.subtract(p[1], p[0]), np.subtract(p[2], p[0]))
                normal = normal / (np.linalg.norm(normal) or 1)
                if normal[1] < 0:
                    normal = -normal
                if np.dot(np.cross(np.subtract(p[1], p[0]), np.subtract(p[2], p[0])), normal) > 0:
                    p[1], p[2] = p[2], p[1]
                verts, nrms, tris, segv = by_type['WATER']
                base = len(verts)
                verts += [to_gltf(q) for q in p]
                nrms += [(-normal[0], normal[1], normal[2])] * 3
                segv += [-1] * 3
                tris.append((base, base + 1, base + 2))
                sheets += 1
    if sheets:
        report.append('water: %d collision triangles from the water sheets' % sheets)
    for surface, (verts, nrms, tris, segv) in sorted(by_type.items()):
        node = g.mesh('COL_%s_arcade' % surface, verts, nrms, tris, attrs={'_SEGMENT': segv} if max(segv) >= 0 else None)
        # Covered polygons (roaddat flags 0x40000000 / 0x80000000) keep Rush 2's covered level: covered = 1 or 2.
        if '_covered' in surface:
            g.js['nodes'][node]['extras'] = {'covered': int(surface.rsplit('_covered', 1)[1])}
    # The spline road's segments, as the game reads them (unused fields of some records are NaN in the arcade data).
    g.empty('ROAD_SPLINES')
    g.js['nodes'][-1]['extras'] = {'segments': [[v if math.isfinite(v) else 0.0 for v in r] for r in seg_records]}
    report.append('road polygon normals: %d of %d point up' % (sum(y > 0.3 for y in up_road), len(up_road)))

    # The drones' lap and the gates.
    lap = np.array(t.lap(), float)
    n = len(lap)
    tangent = lambda i: (lambda d: d / (np.linalg.norm(d) or 1.0))((lap[(i + 1) % n] - lap[i - 1])[[0, 2]])
    from scipy.spatial import cKDTree
    centers = cKDTree(np.array([np.mean(tr, 0) for tr, _ in road2d]))
    def near_tris(i):
        idx = centers.query_ball_point(lap[i][[0, 2]], 260)
        return [road2d[k] for k in idx]
    half = []
    for i in range(n):
        l, r = road_half_width(near_tris(i), lap[i][[0, 2]], tangent(i), lap[i][1])
        if l + r == 0:   # over a jump: the road below
            l, r = road_half_width(near_tris(i), lap[i][[0, 2]], tangent(i), lap[i][1], 90.0)
        half.append((l, r))
    gates = []
    finish = [p for nm, sc, m, p in t.dobs() if nm == 'FINISHSIGN']
    if finish:
        fp = np.array(arcade.to_world_dobs(finish[0]))
        gates.append(('FINISH', int(np.argmin(np.linalg.norm(lap - fp, axis=1)))))
    else:
        # Mission's finish is a sign in worlds.rom (TE_FINISH).
        signs = []
        for nm, flags, m, p in t.worlds():
            o = t.by_name.get(nm)
            if o:
                L = o.lods[0]
                pts = [L.verts[k][0] for tr in L.tris if t.textures[tr[3]].name in FINISH_TEXTURES for k in tr[:3]]
                if pts:
                    signs.append(np.mean(pts, 0) + p)
        if signs:
            gates.append(('FINISH', int(np.argmin(np.linalg.norm(lap - signs[0], axis=1)))))
        else:
            report.append('no FINISHSIGN: the finish is the lap\'s first point')
            gates.append(('FINISH', 0))
    if finish_at is not None:
        # Test aid: the finish moved finish_at ft along the lap, to start a race next to something far into it.
        i, run = gates[0][1], 0.0
        while run < finish_at:
            run += float(np.linalg.norm(lap[(i + 1) % n] - lap[i]))
            i = (i + 1) % n
        gates[0] = ('FINISH', i)
        report.append('test: FINISH moved %.0f ft along the lap, to lap point %d' % (run, i))
    banners = []
    for nm, flags, m, p in t.worlds():
        o = t.by_name.get(nm)
        if not o:
            continue
        L = o.lods[0]
        pts = [L.verts[k][0] for tr in L.tris if t.textures[tr[3]].name in GATE_TEXTURES for k in tr[:3]]
        if pts:
            c = np.mean(pts, 0) + p
            i = int(np.argmin(np.linalg.norm(lap - c, axis=1)))
            if np.linalg.norm((lap[i] - c)[[0, 2]]) <= GATE_REACH:
                banners.append(i)
    f0 = gates[0][1]
    # Distance along the lap from the finish; banners within GATE_MERGE of the last one kept (or of the finish, either
    # side) are pieces of it.
    step = np.linalg.norm(np.roll(lap, -1, axis=0) - lap, axis=1)
    run = np.concatenate([[0.0], np.cumsum(np.roll(step, -f0))])
    total = run[-1]
    kept = []
    for i in sorted(set(banners), key=lambda i: (i - f0) % n):
        at = run[(i - f0) % n]
        if at < GATE_MERGE or total - at < GATE_MERGE or (kept and at - kept[-1][1] < GATE_MERGE):
            continue
        kept.append((i, at))
    for k, (i, at) in enumerate(kept):
        gates.append(('CHECKPOINT_%d' % (k + 1), i))
    for gname, i in gates:
        l, r = half[i]
        g.empty(gname, gate_matrix(tuple(lap[i]), (tangent(i)[0], 0.0, tangent(i)[1]), max(l, r, 30.0) + 10.0))
    # AI: every racing line the arcade gives the drones. Table 0's lap is AIPATH_main (as wide as the road there: the
    # converter spreads unpainted lanes over 0.6 of its half width, but every lane is painted here); the racing lines
    # (tables 1-7 that run on this course) are AIPATH_lane1-4 left to right, each with the arcade's speed and steering
    # behaviour per point (_SPEED in Rush 2 mph = ft/s x 60/88 / 1.2, the 2049 lane map; _AI_MODE).
    tables = t.path_tables()
    def ai_attrs(recs):
        return {'_SPEED': [r[1] * 60.0 / 88.0 / LANE_MAP for r in recs], '_AI_MODE': [float(r[2]) for r in recs]}
    def ribbon(name, pts, widths, attrs):
        verts, nrms, tris = [], [], []
        m = len(pts)
        at = {k: [] for k in attrs}
        for i in range(m):
            d = (pts[(i + 1) % m] - pts[i - 1])[[0, 2]]
            d = d / (np.linalg.norm(d) or 1.0)
            lat = np.array([-d[1], 0.0, d[0]])
            verts += [to_gltf(pts[i] - lat * widths[i]), to_gltf(pts[i] + lat * widths[i])]
            nrms += [(0.0, 1.0, 0.0)] * 2
            for k, v in attrs.items():
                at[k] += [v[i], v[i]]
        for i in range(m):
            j = (i + 1) % m
            tris += [(2 * i, 2 * i + 1, 2 * j), (2 * i + 1, 2 * j + 1, 2 * j)]
        g.mesh(name, verts, nrms, tris, attrs=at)
    widths = [max(15.0, min(l, r, 80.0)) * 0.9 for l, r in half]
    ribbon('AIPATH_main', lap, widths, ai_attrs(tables[0][1][:n]))
    # The arcade lap is at road height except over its jumps, where it is the cars' arc: keep it there.
    g.js['nodes'][-1]['extras'] = {'keep_height': True}
    lap_tree = cKDTree(lap)
    lines = [(0, lap, tables[0][1][:n])]
    for k, (m, recs) in enumerate(tables[1:], 1):
        pts = np.array([r[0] for r in recs[:m]])
        if len(pts) < 10:
            continue
        d, _ = lap_tree.query(pts)
        if np.median(d) > 60.0:
            report.append('dpath table %d: %d points, %.0f ft from the course (not a racing line here), left out' % (
                k, m, np.median(d)))
            continue
        lines.append((k, pts, recs[:m]))
    extra = len(tables[0][1]) - n
    if extra:
        report.append('dpath table 0: %d records past the lap (routes off the course, not raced), left out' % extra)
    # Left to right in the race direction: the mean offset along the lap's left-hand lateral.
    def side(pts):
        _, j = lap_tree.query(pts)
        off = []
        for p, i in zip(pts, j):
            tg = tangent(i)
            off.append((p - lap[i])[[0, 2]] @ np.array([tg[1], -tg[0]]))
        return float(np.mean(off))
    lines.sort(key=lambda L: -side(L[1]))
    # Four slots: the lines spread over them, the middle ones repeated when there are fewer (stock tracks repeat
    # lanes too), never a computed one.
    picks = [lines[round(s * (len(lines) - 1) / 3)] for s in range(4)]
    if len(lines) > 4:
        report.append('%d racing lines: the game has 4 lane slots, so %d are left out' % (len(lines), len(lines) - 4))
    for slot, (k, pts, recs) in enumerate(picks):
        ribbon('AIPATH_lane%d' % (slot + 1), np.array(pts), [3.0] * len(pts), ai_attrs(recs))
        print('  AIPATH_lane%d: dpath table %d (%d points)' % (slot + 1, k, len(pts)))

    folder = os.path.join(out_dir, 'tracks', track_id)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'track.glb'), 'wb') as f:
        f.write(g.bytes())
    with open(os.path.join(folder, 'track.toml'), 'w', newline='\n') as f:
        f.write('[track]\nname = "%s"\nauthor = "Rush 2049 arcade (converted)"\ndescription = "%s from San Francisco '
                'Rush 2049 Special Edition (arcade)"\ngravity = "rush2049"\nsmooth_collision = false\n\n%s[menu]\nlogo = "logo.png"\nafter = "%s"\n'
                % (name, name,
                   # Alcatraz is at night; the other arcade tracks keep the custom tracks' default fog.
                   '[look]\nfog_color = "#0c1424"\n\n' if os.path.basename(os.path.normpath(t.folder)) == 'TRACK4' else '',
                   after))
        if preview is None:
            here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'previews', track_id + '.png')
            preview = here if os.path.exists(here) else None
        if preview:
            round_preview(preview).save(os.path.join(folder, 'preview.png'))
            f.write('preview = "preview.png"\n')
        # How the moving objects play: their dobs scripts' events (docs/arcade2049.md#dobsrom-dynamic-objects).
        f.write(mover_toml)
    # The name banner in Rush 2049's lettering, as the 2049 tracks' own (tools/build_banners.py).
    import banner_font
    banner_font.render_fit(logo_text or name, 'rush2049').convert('RGBA').save(
        os.path.join(folder, 'logo.png'))
    # The arcade's CHECKPOINT banner (the same on arcade tracks 1-5), which the game's Arcade Checkpoints option hangs
    # over the Rush 2049 tracks' checkpoints (src/arcade2049/arcade_checkpoints.cpp).
    for tex in t.textures:
        if tex.name == 'MG_CHECK5':
            from PIL import Image
            Image.fromarray(tex.rgba(t.texels)).save(os.path.join(out_dir, 'checkpoint.png'))
            break
    mod_json = os.path.join(out_dir, 'mod.json')
    if not os.path.exists(mod_json):
        with open(mod_json, 'w', newline='\n') as f:
            json.dump({'game_id': 'rush2', 'id': mod_id, 'display_name': 'Rush 2049 Arcade Tracks',
                       'description': 'Tracks from San Francisco Rush 2049 Special Edition (arcade), converted with '
                                      'tools/arcade2049/track_mod.py.',
                       'short_description': 'Rush 2049 arcade tracks', 'version': '1.0.0',
                       'authors': ['rush2-recomp'], 'minimum_recomp_version': '0.0.0',
                       'hide_track_warnings': True}, f, indent=4)
    print('wrote %s: %d scenery triangles, %d textures, %d collision polygons, %d gates, lap of %d points' % (
        folder, tri_count, len(mats), sum(len(v[2]) for v in by_type.values()), len(gates), n))
    for gname, i in gates:
        print('  %s at lap point %d, road half widths %.0f / %.0f ft' % (gname, i, *half[i]))
    if placed_props:
        print('props:', dict(placed_props))
    for r in report:
        print(r)
    return folder


def round_preview(path):
    """A track select picture cut like Rush 2049's own (its TPIC thumbnails): the middle square of the image at
    128 x 128, inside a disc of radius 62 centered on (63.5, 64.5) with hard edges, clear outside. That disc matches
    every 2049 TPIC's alpha exactly."""
    from PIL import Image
    im = Image.open(path).convert('RGBA')
    s = min(im.size)
    x0, y0 = (im.width - s) // 2, (im.height - s) // 2
    im = im.crop((x0, y0, x0 + s, y0 + s)).resize((128, 128), Image.LANCZOS)
    y, x = np.mgrid[0:128, 0:128]
    a = np.array(im)
    a[..., 3] = np.where((x - 63.5) ** 2 + (y - 64.5) ** 2 <= 62 ** 2, 255, 0)
    return Image.fromarray(a)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('track', help='arcade track folder, e.g. TRACK4 (Alcatraz)')
    ap.add_argument('out', help='the mod folder to write')
    ap.add_argument('--id', default='alcatraz', help='track folder name (its records and ghosts key)')
    ap.add_argument('--name', default='Alcatraz')
    ap.add_argument('--after', default='rush2049', help='track.toml [menu] after: the game, or the custom track folder '
                    'it follows in the track select (the game\'s own build chains them, include/arcade2049.h)')
    ap.add_argument('--mod-id', default='rush2049_arcade')
    ap.add_argument('--logo-text', help='the track select name banner (default: the name)')
    ap.add_argument('--finish-at', type=float, help='test aid: move FINISH this many feet along the lap (a race then '
                    'starts next to what is there)')
    ap.add_argument('--preview', help='a picture for the track select, cut round like the 2049 tracks\' (default: '
                    'tools/arcade2049/previews/<id>.png when there is one)')
    a = ap.parse_args()
    build(a.track, a.out, a.id, a.name, a.mod_id, a.finish_at, a.logo_text, a.preview, a.after)


def ship_previews():
    here = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'previews')
    assets = os.path.join(os.path.dirname(os.path.dirname(HERE)), 'assets', 'arcade2049')
    for name in sorted(os.listdir(here)):
        if name.endswith('.png'):
            out = os.path.join(assets, name[:-4] + '_preview.png')
            round_preview(os.path.join(here, name)).save(out)
            print('wrote', os.path.relpath(out))


if __name__ == '__main__':
    if '--ship-previews' in sys.argv:
        ship_previews()
    else:
        main()
