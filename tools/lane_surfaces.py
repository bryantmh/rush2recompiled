"""The ground surface under a track's AI lanes: for each lane, the runs off plain road (dirt, grass, water...), where
they fall in the lap (percent of the lane's length, and the lane point's position) and how long they are. Compares a
converted course with its original, e.g. the PlayStation SF Rush's Golden Gate (a converter output folder) against
the N64's (docs/sfrush_ps1.md#collision).

    python tools/lane_surfaces.py --n64 0                     N64 SF Rush track 0-6 (RUSH1_ROM, tools/rush1/r1.py)
    python tools/lane_surfaces.py --rush2 tmp/gg_out3_night   a converted track: collision.bin + path.bin
    [--plot X Z RADIUS PNG [--trail motion.jsonl]]          a top-down map of the ground, walls and lanes round a spot
    [--walls 6]  instead: where each lane passes within 6 ft of a wall at its height (or crosses one)
    [--min 40]  shortest run printed, ft

Surface types: Rush 2's polygon type (flags & 0xF: 0 road, 1 dirt, 2 grass, 3 water, 5/6 wall, 7 kill); the N64's
polygons carry the same field. A lane point with no ground polygon under it is '-'.
"""
import argparse, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'rush1'))
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
NAMES = {0: 'road', 1: 'dirt', 2: 'grass', 3: 'water', 5: 'wall', 6: 'wall', 7: 'kill', None: '-'}


def inside(pt, poly):
    """Is (x, z) inside the polygon's horizontal outline (a list of (x, z))?"""
    x, z = pt
    c = False
    n = len(poly)
    for i in range(n):
        x1, z1 = poly[i]
        x2, z2 = poly[(i + 1) % n]
        if (z1 > z) != (z2 > z) and x < (x2 - x1) * (z - z1) / (z2 - z1) + x1:
            c = not c
    return c


class Grid:
    """Ground polygons (type, [(x, y, z)...]) bucketed by 64 ft cells for the point queries."""
    def __init__(self, polys):
        self.cells = {}
        for t, pts in polys:
            xs = [p[0] for p in pts]; zs = [p[2] for p in pts]
            for cx in range(int(min(xs) // 64), int(max(xs) // 64) + 1):
                for cz in range(int(min(zs) // 64), int(max(zs) // 64) + 1):
                    self.cells.setdefault((cx, cz), []).append((t, pts))

    def surface(self, p):
        """The type of the ground polygon under p (x, y, z) nearest its height, or None."""
        best = None
        for t, pts in self.cells.get((int(p[0] // 64), int(p[2] // 64)), []):
            if t in (5, 6) or not inside((p[0], p[2]), [(q[0], q[2]) for q in pts]):
                continue
            h = sum(q[1] for q in pts) / len(pts)
            if best is None or abs(h - p[1]) < best[0]:
                best = (abs(h - p[1]), t)
        return best[1] if best else None


def n64(track):
    """(polygons, lanes) of N64 SF Rush track 0-6, in collision space (render z, render x, -render y): x, z across the
    ground, y height (tools/rush1/collision1.py, docs/rush1_research.md#5-collision-v)."""
    from r1 import Rush1
    import collision1, paths1
    rom = Rush1()
    c = collision1.parse(rom.asset(44 + track))
    polys = []
    for p in c['polys']:
        fl, info = struct.unpack('>HH', p[:4])
        m = [v / 16384 for v in struct.unpack('>9h', p[6:24])]
        idx, _, _ = collision1.decode_vlist(c['vlist'], struct.unpack('>H', p[24:26])[0], info & 0xF)
        if len(idx) < 3:
            continue
        v0 = collision1.vval(c['verts'][idx[0]])
        pts = [v0]
        for k in idx[1:]:
            l = collision1.vval(c['verts'][k])
            # world = origin + M local (Rush 1 stores the transpose of Rush 2's matrix).
            pts.append(tuple(v0[a] + m[3 * a] * l[0] + m[3 * a + 1] * l[1] + m[3 * a + 2] * l[2] for a in range(3)))
        # Collision space (cz, cx, -cy) -> x across, y height, z across.
        polys.append((fl & 0xF, [(q[1], -q[2], q[0]) for q in pts]))
    lanes = []
    for _, pts in paths1.lanes(rom.asset(58 + track)):
        # Lane points are render (x, y up, z): collision (z, x, -y) -> (x, y, z) in the frame above.
        lanes.append([(q[0][0], q[0][1], q[0][2]) for q in pts])
    return polys, lanes


def rush2(folder):
    import collision, paths
    sys.path.insert(0, HERE)
    from stock_track_glb import polygons
    c = collision.Collision.parse(open(os.path.join(folder, 'collision.bin'), 'rb').read(), 'rush2')
    polys = [(t, pts) for t, pts, _ in polygons(c)]
    p = paths.parse(open(os.path.join(folder, 'path.bin'), 'rb').read())
    lanes = [[(q[0], q[1], q[2], q[3]) for q in lane['points']] for lane in p['lanes'] if lane['count']]
    return polys, lanes


def plot(polys, lanes, cx, cz, radius, out, trail=None, size=1000, steep=12.0):
    """Top-down map of the ground round (cx, cz): road grey (lighter = higher), dirt brown, grass green, water blue,
    kill purple; slopes over `steep` degrees (12) yellow (ramps), walls red; each lane its own color with a dot per point; a
    motion.jsonl trail (tools/test_race.py --motion) white. x to the right, z down; 1 grid line per 50 ft."""
    from PIL import Image, ImageDraw
    k = size / (2 * radius)
    to = lambda x, z: ((x - cx) * k + size / 2, (z - cz) * k + size / 2)
    img = Image.new('RGB', (size, size), (20, 20, 30))
    d = ImageDraw.Draw(img)
    # Polygons whose outline box overlaps the view (a big one can have no corner in it).
    near = [(t, pts) for t, pts in polys
            if min(p[0] for p in pts) < cx + radius * 1.2 and max(p[0] for p in pts) > cx - radius * 1.2 and
            min(p[2] for p in pts) < cz + radius * 1.2 and max(p[2] for p in pts) > cz - radius * 1.2]
    ys = [p[1] for t, pts in near if t in (0, 1, 2, 3) for p in pts] or [0, 1]
    lo, hi = min(ys), max(ys)
    walls = []
    for t, pts in sorted(near, key=lambda tp: sum(p[1] for p in tp[1]) / len(tp[1])):
        if t in (5, 6):
            walls.append(pts)
            continue
        # Slope from the polygon's normal (first three corners).
        a, b, c = pts[0], pts[1], pts[2]
        u = [b[i] - a[i] for i in range(3)]; v = [c[i] - a[i] for i in range(3)]
        n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
        ln = math.sqrt(sum(x * x for x in n)) or 1
        slope = math.degrees(math.acos(min(1, abs(n[1]) / ln)))
        h = (sum(p[1] for p in pts) / len(pts) - lo) / max(1e-6, hi - lo)
        base = {0: (90, 90, 90), 1: (110, 80, 50), 2: (50, 100, 50), 3: (40, 60, 140), 7: (120, 40, 120)}.get(t, (70, 70, 70))
        col = tuple(int(x * (0.6 + 0.8 * h)) for x in base)
        if slope > steep and slope < 80:
            col = (200, 180, 40)
        d.polygon([to(p[0], p[2]) for p in pts], fill=col, outline=tuple(max(0, x - 25) for x in col))
    for pts in walls:
        d.line([to(p[0], p[2]) for p in pts] + [to(pts[0][0], pts[0][2])], fill=(230, 40, 40), width=2)
    g = 50 * math.floor((cx - radius) / 50)
    while g < cx + radius:
        d.line([to(g, cz - radius), to(g, cz + radius)], fill=(255, 255, 255, 40), width=1) if g % 250 == 0 else None
        g += 50
    colors = [(0, 220, 255), (255, 120, 0), (0, 255, 120), (255, 0, 200), (255, 255, 255)]
    for li, lane in enumerate(lanes):
        c = colors[li % len(colors)]
        pts = [to(p[0], p[2]) for p in lane]
        for i in range(len(pts) - 1):
            if math.dist(lane[i][::2], (cx, cz)) < radius * 1.5:
                d.line([pts[i], pts[i + 1]], fill=c, width=2)
        for p in pts:
            d.ellipse([p[0] - 2, p[1] - 2, p[0] + 2, p[1] + 2], fill=c)
    if trail:
        import json
        tp = [json.loads(l)['pos'] for l in open(trail)]
        d.line([to(p[0], p[2]) for p in tp], fill=(255, 255, 255), width=1)
    d.text((5, 5), '(%.0f, %.0f) +-%.0f ft, heights %.0f..%.0f' % (cx, cz, radius, lo, hi), fill=(255, 255, 255))
    img.save(out)


def wall_report(polys, lanes, clear, step=2.0):
    """Where each lane runs within `clear` ft of a wall polygon at the lane's height (a wall spanning the car's body, lane y + 1..3 ft), or
    crosses one: lap percent, position, the nearest distance and the lane's speed there. A car is about 6 ft wide, so
    a lane under 3 ft from a wall scrapes it."""
    cells = {}
    for t, pts in polys:
        if t not in (5, 6):
            continue
        ys = [q[1] for q in pts]
        edges = [((pts[i][0], pts[i][2]), (pts[(i + 1) % len(pts)][0], pts[(i + 1) % len(pts)][2])) for i in range(len(pts))]
        w = (min(ys), max(ys), edges)
        xs = [q[0] for q in pts]; zs = [q[2] for q in pts]
        for cx in range(int((min(xs) - clear) // 32), int((max(xs) + clear) // 32) + 1):
            for cz in range(int((min(zs) - clear) // 32), int((max(zs) + clear) // 32) + 1):
                cells.setdefault((cx, cz), []).append(w)

    def seg_dist(p, a, b):
        dx, dz = b[0] - a[0], b[1] - a[1]
        l2 = dx * dx + dz * dz
        t = 0 if l2 == 0 else max(0, min(1, ((p[0] - a[0]) * dx + (p[1] - a[1]) * dz) / l2))
        return math.hypot(p[0] - a[0] - t * dx, p[1] - a[1] - t * dz)

    def nearest(p):
        best = (1e9, None)
        seen = set()
        for w in cells.get((int(p[0] // 32), int(p[2] // 32)), []):
            if id(w) in seen or not (w[0] <= p[1] + 3 and w[1] >= p[1] + 1):
                continue
            seen.add(id(w))
            for a, b in w[2]:
                d = seg_dist((p[0], p[2]), a, b)
                if d < best[0]:
                    best = (d, w)
        return best

    for li, lane in enumerate(lanes):
        n = len(lane)
        seg = [math.dist(lane[i][:3], lane[(i + 1) % n][:3]) for i in range(n)]
        total = sum(seg)
        hits = []
        at = 0.0
        for i in range(n):
            a, b = lane[i], lane[(i + 1) % n]
            if seg[i] > 100:   # a run-in course's end to its start: not driven
                at += seg[i]
                continue
            k = max(1, int(seg[i] / step))
            for s in range(k):
                f = s / k
                p = tuple(a[j] + (b[j] - a[j]) * f for j in range(3))
                d, w = nearest(p)
                if d < clear:
                    hits.append((at + seg[i] * f, d, p, a[3] if len(a) > 3 else 0, w))
            at += seg[i]
        runs = []
        for h in hits:
            if runs and h[0] - runs[-1][-1][0] < 3 * step:
                runs[-1].append(h)
            else:
                runs.append([h])
        print('lane %d: %.0f ft, %d spots under %.0f ft' % (li, total, len(runs), clear))
        for r in runs:
            m = min(r, key=lambda h: h[1])
            print('  %5.1f%%  %4.0f ft long  nearest %4.1f ft at (%.0f, %.0f, %.0f)  %3.0f mph  wall y %.0f..%.0f' %
                  (100 * r[0][0] / total, r[-1][0] - r[0][0] + step, m[1], m[2][0], m[2][1], m[2][2], m[3], m[4][0], m[4][1]))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n64', type=int)
    ap.add_argument('--rush2')
    ap.add_argument('--min', type=float, default=40)
    ap.add_argument('--plot', nargs=4, metavar=('X', 'Z', 'RADIUS', 'PNG'),
                    help='a top-down map of the ground, walls and lanes round (X, Z) instead of the report')
    ap.add_argument('--trail', help='with --plot: a motion.jsonl (tools/test_race.py --motion) drawn over it')
    ap.add_argument('--steep', type=float, default=12.0, help='with --plot: slopes over this many degrees are yellow')
    ap.add_argument('--walls', type=float, help='report where lanes pass within this many ft of a wall')
    a = ap.parse_args()
    polys, lanes = n64(a.n64) if a.n64 is not None else rush2(a.rush2)
    if a.plot:
        plot(polys, lanes, float(a.plot[0]), float(a.plot[1]), float(a.plot[2]), a.plot[3], a.trail, steep=a.steep)
        return
    if a.walls:
        wall_report(polys, lanes, a.walls)
        return
    grid = Grid(polys)
    from collections import Counter
    print('%d polygons by type: %s' % (len(polys), dict(Counter(NAMES.get(t, t) for t, _ in polys))))
    for li, lane in enumerate(lanes):
        seg = [math.dist(lane[i], lane[(i + 1) % len(lane)]) for i in range(len(lane))]
        total = sum(seg)
        surf = [grid.surface(p) for p in lane]
        print('lane %d: %d points, %.0f ft, %s' % (li, len(lane), total, dict(Counter(NAMES.get(s, s) for s in surf))))
        at = 0.0
        i = 0
        while i < len(lane):
            j = i
            run = 0.0
            while j < len(lane) and surf[j] == surf[i]:
                run += seg[j]
                j += 1
            if surf[i] != 0 and run >= a.min:
                p = lane[i]
                print('  %5.1f%%-%5.1f%%  %6.0f ft  %-5s  from (%.0f, %.0f, %.0f)' %
                      (100 * at / total, 100 * (at + run) / total, run, NAMES.get(surf[i], surf[i]), p[0], p[1], p[2]))
            at += run
            i = j


if __name__ == '__main__':
    main()
