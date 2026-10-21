# Compares an arcade track's walls (roaddat.rom, type 5/6) with the N64 port's collision file: each arcade wall is
# matched to the N64 wall nearest in x/z (centroid within --near ft, normals within ~25 degrees) and the two tops and
# bottoms compared. Shows where the N64 port changed the walls (docs/arcade2049.md, "Wall heights").
# RUSH2049_ROM=<rom> python -I tools/arcade2049/wall_compare.py TRACK1 139 [--near 2] [--list N] [--box x0 x1 z0 z1]
import collections, math, os, struct, sys
sys.path.insert(0, os.path.dirname(__file__)); sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'rush2049'))
import arcade, roms, collision as C

def arg(name, n, default):
    return [float(v) for v in sys.argv[sys.argv.index(name) + 1:sys.argv.index(name) + 1 + n]] if name in sys.argv else default

track, fileno = sys.argv[1], int(sys.argv[2])
near = arg('--near', 1, [2.0])[0]
show = int(arg('--list', 1, [0])[0])
box = arg('--box', 4, None)
cen = lambda pts: tuple(sum(p[k] for p in pts) / len(pts) for k in range(3))
arc = [(pts, n) for f, pts, n in arcade.Track(track).collision() if (f & 0xF) in (5, 6)]
c = C.Collision.parse(roms.Rush2049().file(fileno), C.R49)
n64 = []
for p in c.polys:
    if (p['flags'] & 0xF) in (5, 6):
        n64.append((C.world_polygon(c, p), [v / 16384 for v in p['matrix'][3:6]]))
grid = collections.defaultdict(list)
for k, (w, m) in enumerate(n64):
    a = cen(w); grid[(int(a[0] // 8), int(a[2] // 8))].append(k)
stats = collections.Counter(); raised = []
for pts, n in arc:
    a = cen(pts)
    if box and not (box[0] < a[0] < box[1] and box[2] < a[2] < box[3]):
        continue
    best = None
    for gx in range(int(a[0] // 8) - 1, int(a[0] // 8) + 2):
        for gz in range(int(a[2] // 8) - 1, int(a[2] // 8) + 2):
            for k in grid[(gx, gz)]:
                w, m = n64[k]; b = cen(w)
                d = math.dist((a[0], a[2]), (b[0], b[2]))
                if d < near and sum(x * y for x, y in zip(n, m)) > 0.9 and (best is None or d < best[0]):
                    best = (d, k)
    if best is None:
        stats['no N64 wall'] += 1; continue
    w, m = n64[best[1]]
    top = max(p[1] for p in w) - max(p[1] for p in pts)
    bot = min(p[1] for p in w) - min(p[1] for p in pts)
    key = 'N64 top higher' if top > 1 else 'N64 top lower' if top < -1 else 'same top'
    stats[key] += 1
    if abs(bot) > 1: stats['bottom differs'] += 1
    if top > 1: raised.append((top, a))
print(track, 'file', fileno, 'arcade walls', len(arc), 'N64 walls', len(n64), dict(stats))
if raised:
    tops = sorted(t for t, _ in raised)
    print('  raised by: median %.1f ft, max %.1f' % (tops[len(tops) // 2], tops[-1]))
for t, a in sorted(raised, reverse=True)[:show]:
    print('  +%.1f ft at (%.0f, %.0f, %.0f)' % (t, *a))
