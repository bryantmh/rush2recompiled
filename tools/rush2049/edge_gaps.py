#!/usr/bin/env python3
"""Finds open road edges on a collision file: places beside an AI lane where a car driving sideways off the road
runs out of floor before any wall stops it (research aid for GitHub issue 37, docs/rush2049_research/collision.md).

From every lane point the probe steps outward to the left and right, square to the lane, --step ft at a time up to
--reach ft. At each step it looks for a floor under the probe (any polygon but walls 5/6, plane within --drop ft
under the last floor height to --climb ft over it) and for a wall polygon crossed between the last step and this one
at --height ft over the floor. The side ends at the first wall ("wall"), or at the first step with no floor ("open":
the car leaves the collision there). Prints runs of lane points whose side is open, with where the floor ends.

    python edge_gaps.py K [--file 2049|converted] [--out DIR] [--lanes 0,1,2,3] [--from I --to J] [--reach FT]
        K: 2049 track 1-6 (Marina, Haight, Civic, Metro, Mission, Presidio). --file converted reads
        DIR/collision.bin (default cpp_test/out/nostatic/track<K>); the 2049 file comes from RUSH2049_ROM. The lane
        points always come from DIR/path.bin.
"""
import argparse, math, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import collision as col
import paths
import roms
from floor_gaps import Floors, NAMES


class Walls:
    """Wall polygons (types 5/6) as world-space vertex arrays with their planes, for segment crossing tests."""

    def __init__(self, c):
        self.polys = []
        for p in c.polys:
            if (p['flags'] & 0xF) not in col.TYPE_WALL or len(p['verts']) < 3:
                continue
            w = np.array(col.world_polygon(c, p), float)
            n = np.cross(w[1] - w[0], w[2] - w[0])
            if np.linalg.norm(n) < 1e-9:
                continue
            self.polys.append((w, n / np.linalg.norm(n)))
        self.lo = np.array([w.min(0) for w, _ in self.polys])
        self.hi = np.array([w.max(0) for w, _ in self.polys])

    def crossed(self, a, b):
        """True when segment a-b (3D) passes through a wall polygon."""
        lo, hi = np.minimum(a, b) - 0.01, np.maximum(a, b) + 0.01
        m = np.all(self.lo <= hi, 1) & np.all(self.hi >= lo, 1)
        for k in np.nonzero(m)[0]:
            w, n = self.polys[k]
            da, db = np.dot(a - w[0], n), np.dot(b - w[0], n)
            if da * db > 0 or da == db:
                continue
            p = a + (b - a) * (da / (da - db))
            # Inside test in the polygon's plane: drop the largest normal axis.
            ax = int(np.argmax(np.abs(n)))
            keep = [i for i in range(3) if i != ax]
            q = w[:, keep]
            pp = p[keep]
            sign = 0
            inside = True
            for s, e in zip(q, np.roll(q, -1, 0)):
                cr = (e[0] - s[0]) * (pp[1] - s[1]) - (e[1] - s[1]) * (pp[0] - s[0])
                if abs(cr) < 1e-9:
                    continue
                if sign == 0:
                    sign = 1 if cr > 0 else -1
                elif (cr > 0) != (sign > 0):
                    inside = False
                    break
            if inside:
                return True
        return False


def floor_height(floors, x, y, z, drop, climb):
    """Height of the floor nearest y under (x, z), within drop below and climb above, or None."""
    best = None
    for i in floors.under(x, y, z, drop, climb):
        w, n = floors.polys[i]
        h = w[0][1] - (n[0] * (x - w[0][0]) + n[2] * (z - w[0][2])) / n[1]
        if best is None or abs(h - y) < abs(best - y):
            best = h
    return best


def probe(floors, walls, pt, side, a):
    """Walk from pt along side (unit x/z): ('wall', d) or ('open', d) or ('far', reach)."""
    x, y, z = pt
    h = floor_height(floors, x, y, z, a.drop, a.climb)
    if h is None:
        return 'nofloor', 0.0
    prev = np.array([x, h + a.height, z])
    d = 0.0
    while d < a.reach:
        d += a.step
        nx, nz = x + side[0] * d, z + side[1] * d
        cur = np.array([nx, prev[1], nz])
        if walls.crossed(prev, cur):
            return 'wall', d
        nh = floor_height(floors, nx, prev[1] - a.height, nz, a.drop, a.climb)
        if nh is None:
            return 'open', d
        prev = np.array([nx, nh + a.height, nz])
    return 'far', a.reach


def draw_map(c, path, spec, out_png):
    """Top-down PNG of the area spec = 'X,Z,R[,Y0,Y1]' (centre, half-size in ft, optional height band): floors grey
    by height (only those inside the band), wall footprints red (tops under the band's floor dark red), lane points
    in lane colours (0 blue, 1 green, 2 yellow, 3 magenta) with every 10th point numbered."""
    from PIL import Image, ImageDraw
    v = [float(s) for s in spec.split(',')]
    cx, cz, r = v[:3]
    y0, y1 = (v[3], v[4]) if len(v) >= 5 else (-1e9, 1e9)
    size = 1000
    s = size / (2 * r)
    img = Image.new('RGB', (size, size), (0, 0, 0))
    g = ImageDraw.Draw(img)

    def px(x, z):
        return ((x - cx + r) * s, (z - cz + r) * s)

    walls = []
    for p in c.polys:
        if len(p['verts']) < 3:
            continue
        w = col.world_polygon(c, p)
        xs, zs = [q[0] for q in w], [q[2] for q in w]
        if max(xs) < cx - r or min(xs) > cx + r or max(zs) < cz - r or min(zs) > cz + r:
            continue
        ys = [q[1] for q in w]
        if max(ys) < y0 or min(ys) > y1:
            continue
        if (p['flags'] & 0xF) in col.TYPE_WALL:
            walls.append((w, max(ys)))
            continue
        k = int(80 + 120 * ((sum(ys) / len(ys) - y0) / max(1.0, y1 - y0) if y1 < 1e8 else 0.5))
        k = max(40, min(220, k))
        t = p['flags'] & 0xF
        g.polygon([px(q[0], q[2]) for q in w], fill=(k, k, k) if t != 7 else (k, k // 2, 0), outline=(60, 60, 90))
    for w, top in walls:
        pts = [px(q[0], q[2]) for q in w]
        g.line(pts + pts[:1], fill=(255, 40, 40) if top >= y0 + 2 or y0 < -1e8 else (120, 0, 0), width=2)
    colours = [(60, 120, 255), (40, 220, 60), (240, 220, 0), (240, 0, 240)]
    for li, lane in enumerate(path['lanes']):
        for pi, q in enumerate(lane['points']):
            if abs(q[0] - cx) > r or abs(q[2] - cz) > r:
                continue
            a, b = px(q[0], q[2])
            g.ellipse((a - 2, b - 2, a + 2, b + 2), fill=colours[li % 4])
            if pi % 10 == 0:
                g.text((a + 3, b + 3), '%d:%d' % (li, pi), fill=colours[li % 4])
    img.save(out_png)
    print('map written to', out_png)


def raster_floors(c, box, res):
    """{(ix, iz): [heights]}: every floor polygon's height at each res-ft cell centre inside box (x0, z0, x1, z1)."""
    x0, z0, x1, z1 = box
    out = {}
    for p in c.polys:
        if (p['flags'] & 0xF) in col.TYPE_WALL or len(p['verts']) < 3:
            continue
        w = np.array(col.world_polygon(c, p), float)
        n = np.cross(w[1] - w[0], w[2] - w[0])
        if abs(n[1]) < 1e-6 * max(1.0, np.linalg.norm(n)):
            continue
        lo, hi = w.min(0), w.max(0)
        if hi[0] < x0 or lo[0] > x1 or hi[2] < z0 or lo[2] > z1:
            continue
        ia = np.arange(math.floor(max(lo[0], x0) / res), math.floor(min(hi[0], x1) / res) + 1)
        ka = np.arange(math.floor(max(lo[2], z0) / res), math.floor(min(hi[2], z1) / res) + 1)
        if not len(ia) or not len(ka):
            continue
        I, K = np.meshgrid(ia, ka, indexing='ij')
        X, Z = (I + 0.5) * res, (K + 0.5) * res
        sign = np.zeros(X.shape)
        inside = np.ones(X.shape, bool)
        for a, b in zip(w, np.roll(w, -1, 0)):
            cr = (b[0] - a[0]) * (Z - a[2]) - (b[2] - a[2]) * (X - a[0])
            if abs(b[0] - a[0]) + abs(b[2] - a[2]) < 1e-9:
                continue
            s = np.sign(cr)
            set_now = (sign == 0) & (s != 0)
            inside &= ~((sign != 0) & (s != 0) & (s != sign))
            sign = np.where(set_now, s, sign)
        H = w[0][1] - (n[0] * (X - w[0][0]) + n[2] * (Z - w[0][2])) / n[1]
        for i, k, h in zip(I[inside], K[inside], H[inside]):
            out.setdefault((int(i), int(k)), []).append(float(h))
    return out


def flood(c, path, a):
    """Spreads from the lane points over the floor in res-ft cells, stepping only where the step at --height over the
    floor crosses no wall; reports reached cells whose neighbour has no floor within --drop / --climb, clustered."""
    x0, z0, x1, z1 = [float(s) for s in a.flood.split(',')]
    res = a.res
    floors = raster_floors(c, (x0, z0, x1, z1), res)
    walls = Walls(c)
    B = 8.0
    buckets = {}
    for k, (lo, hi) in enumerate(zip(walls.lo, walls.hi)):
        for bx in range(math.floor(lo[0] / B), math.floor(hi[0] / B) + 1):
            for bz in range(math.floor(lo[2] / B), math.floor(hi[2] / B) + 1):
                buckets.setdefault((bx, bz), []).append(k)
    saved = walls.polys, walls.lo, walls.hi

    def crossed(p, q):
        ks = set()
        for bx in {math.floor(p[0] / B), math.floor(q[0] / B)}:
            for bz in {math.floor(p[2] / B), math.floor(q[2] / B)}:
                ks.update(buckets.get((bx, bz), ()))
        if not ks:
            return False
        ks = sorted(ks)
        walls.polys = [saved[0][k] for k in ks]
        walls.lo, walls.hi = saved[1][ks], saved[2][ks]
        # Three heights 0.5 ft apart, so a hairline seam between two wall polygons doesn't count as an opening.
        hit = any(walls.crossed(p + [0, d, 0], q + [0, d, 0]) for d in (-0.5, 0.0, 0.5))
        walls.polys, walls.lo, walls.hi = saved
        return hit

    def pick(cell, h, drop, climb):
        best = None
        for nh in floors.get(cell, ()):
            if -drop <= nh - h <= climb and (best is None or abs(nh - h) < abs(best - h)):
                best = nh
        return best

    from collections import deque
    seen, q = set(), deque()
    parent = {}
    flood.parent = parent
    lanes = [int(s) for s in a.lanes.split(',')]
    for li in lanes:
        for x, y, z, _, _ in path['lanes'][li]['points']:
            if x0 <= x <= x1 and z0 <= z <= z1:
                cell = (math.floor(x / res), math.floor(z / res))
                h = pick(cell, y, 8, 8)
                if h is not None and (cell, round(h * 2)) not in seen:
                    seen.add((cell, round(h * 2)))
                    q.append((cell, h))
    edges = []
    while q:
        (i, k), h = q.popleft()
        for di, dk in ((1, 0), (-1, 0), (0, 1), (0, -1)):
            nc = (i + di, k + dk)
            nx, nz = (nc[0] + 0.5) * res, (nc[1] + 0.5) * res
            if not (x0 <= nx <= x1 and z0 <= nz <= z1):
                continue
            nh = pick(nc, h, a.drop, a.climb)
            p = np.array([(i + 0.5) * res, h + a.height, (k + 0.5) * res])
            if nh is None:
                # Off the edge of the floor: blocked only if a wall stands at the edge.
                if not crossed(p, np.array([nx, h + a.height, nz])):
                    edges.append((i, k, h, nc))
                continue
            if (nc, round(nh * 2)) in seen:
                continue
            if crossed(p, np.array([nx, nh + a.height, nz])):
                continue
            seen.add((nc, round(nh * 2)))
            parent[(nc, round(nh * 2))] = ((i, k), h)
            q.append((nc, nh))
    print('flood over (%g, %g)-(%g, %g): %d cells reached, %d open edge steps' % (x0, z0, x1, z1, len(seen),
                                                                                  len(edges)))
    # Cluster edge steps that lie within 3 cells of each other.
    groups = []
    left = set(range(len(edges)))
    index = {}
    for n, (i, k, h, _) in enumerate(edges):
        index.setdefault((i // 3, k // 3), []).append(n)
    while left:
        n = left.pop()
        grp, todo = [n], [n]
        while todo:
            m = todo.pop()
            i, k = edges[m][0], edges[m][1]
            for bi in range(i // 3 - 1, i // 3 + 2):
                for bk in range(k // 3 - 1, k // 3 + 2):
                    for o in index.get((bi, bk), ()):
                        if o in left and abs(edges[o][0] - i) <= 3 and abs(edges[o][1] - k) <= 3:
                            left.discard(o)
                            grp.append(o)
                            todo.append(o)
        groups.append(grp)
    pts = [(li, pi, q_) for li in lanes for pi, q_ in enumerate(path['lanes'][li]['points'])]
    for grp in sorted(groups, key=len, reverse=True):
        xs = [(edges[m][0] + 0.5) * res for m in grp]
        zs = [(edges[m][1] + 0.5) * res for m in grp]
        hs = [edges[m][2] for m in grp]
        cx, cz = sum(xs) / len(xs), sum(zs) / len(zs)
        li, pi, lp = min(pts, key=lambda t: (t[2][0] - cx) ** 2 + (t[2][2] - cz) ** 2)
        print('  %4d steps x %d..%d z %d..%d floor y %d..%d; nearest lane %d point %d (%.0f ft)' % (
            len(grp), min(xs), max(xs), min(zs), max(zs), min(hs), max(hs), li, pi,
            math.hypot(lp[0] - cx, lp[2] - cz)))
    return seen, edges


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--flood', help='X0,Z0,X1,Z1: flood the floor over that box from the lanes instead of probing')
    ap.add_argument('--res', type=float, default=1.0, help='flood cell size (ft)')
    ap.add_argument('--map', help="X,Z,R[,Y0,Y1]: draw that area to --png instead of probing")
    ap.add_argument('--png', default='edge_map.png')
    ap.add_argument('k', type=int)
    ap.add_argument('--file', choices=('2049', 'converted'), default='2049')
    ap.add_argument('--raw', help='read the 2049-format collision file from this path instead (e.g. the Dreamcast '
                    "source's file 138+K from cpp_test/out/dc_test.exe)")
    ap.add_argument('--out', help='converted track folder (default cpp_test/out/nostatic/track<K>)')
    ap.add_argument('--lanes', default='0,1,2,3')
    ap.add_argument('--from', dest='first', type=int, default=0)
    ap.add_argument('--to', dest='last', type=int, default=1 << 30)
    ap.add_argument('--reach', type=float, default=80.0)
    ap.add_argument('--step', type=float, default=1.0)
    ap.add_argument('--height', type=float, default=2.0, help='probe height over the floor (ft)')
    ap.add_argument('--drop', type=float, default=3.0, help='floor may fall this much per step (ft)')
    ap.add_argument('--climb', type=float, default=2.0, help='floor may rise this much per step (ft)')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    out = a.out or os.path.join(here, 'cpp_test', 'out', 'nostatic', 'track%d' % a.k)
    if a.raw:
        c = col.Collision.parse(open(a.raw, 'rb').read(), col.R49)
        a.file = os.path.basename(a.raw)
    elif a.file == '2049':
        c = col.Collision.parse(roms.Rush2049().file(138 + a.k), col.R49)
    else:
        c = col.Collision.parse(open(os.path.join(out, 'collision.bin'), 'rb').read(), col.R2)
    path = paths.parse(open(os.path.join(out, 'path.bin'), 'rb').read())
    if a.map:
        draw_map(c, path, a.map, a.png)
        return
    if a.flood:
        seen, edges = flood(c, path, a)
        if a.png != 'edge_map.png':
            from PIL import Image
            x0, z0, x1, z1 = [float(s) for s in a.flood.split(',')]
            s = 1000 / max(x1 - x0, z1 - z0)
            img = Image.new('RGB', (int((x1 - x0) * s) + 1, int((z1 - z0) * s) + 1))
            px = img.load()

            def put(i, k, col_):
                u, v = int(((i + 0.5) * a.res - x0) * s), int(((k + 0.5) * a.res - z0) * s)
                for du in range(max(1, int(s * a.res))):
                    for dv in range(max(1, int(s * a.res))):
                        if 0 <= u + du < img.width and 0 <= v + dv < img.height:
                            px[u + du, v + dv] = col_
            for (i, k), _ in seen:
                put(i, k, (70, 110, 70))
            for i, k, _, _ in edges:
                put(i, k, (255, 255, 0))
            from PIL import ImageDraw
            g = ImageDraw.Draw(img)
            for w, _ in Walls(c).polys:
                pts = [((v[0] - x0) * s, (v[2] - z0) * s) for v in w]
                g.line(pts + pts[:1], fill=(255, 40, 40), width=1)
            img.save(a.png)
            print('flood map written to', a.png)
        return
    floors, walls = Floors(c), Walls(c)
    print('%s (%s file): %d polygons, %d floors, %d walls' % (NAMES[a.k - 1], a.file, len(c.polys),
                                                              len(floors.polys), len(walls.polys)))
    for li in [int(s) for s in a.lanes.split(',')]:
        pts = path['lanes'][li]['points']
        rows = []
        for pi in range(max(0, a.first), min(len(pts), a.last + 1)):
            x, y, z = pts[pi][:3]
            nxt = pts[(pi + 1) % len(pts)]
            dx, dz = nxt[0] - x, nxt[2] - z
            ln = math.hypot(dx, dz)
            if ln < 1e-6:
                continue
            dx, dz = dx / ln, dz / ln
            res = []
            for name, side in (('left', (dz, -dx)), ('right', (-dz, dx))):
                kind, d = probe(floors, walls, (x, y, z), side, a)
                res.append((name, kind, d, (x + side[0] * d, z + side[1] * d)))
            rows.append((pi, (x, y, z), res))
        for si, name in enumerate(('left', 'right')):
            i = 0
            while i < len(rows):
                if rows[i][2][si][1] != 'open':
                    i += 1
                    continue
                j = i
                while j + 1 < len(rows) and rows[j + 1][2][si][1] == 'open':
                    j += 1
                ds = [r[2][si][2] for r in rows[i:j + 1]]
                mid = rows[(i + j) // 2]
                ex, ez = mid[2][si][3]
                print('  lane %d points %d-%d %s: floor ends %.0f-%.0f ft out with no wall; lane (%d, %d, %d), '
                      'edge (%d, %d)' % (li, rows[i][0], rows[j][0], name, min(ds), max(ds), *mid[1], ex, ez))
                i = j + 1


if __name__ == '__main__':
    main()
