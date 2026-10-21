# Finds walls in a collision.bin (Rush 2 layout) that face away from the road beside them. Rush 2's body probe takes
# a wall from 5 ft behind its plane to 3.75 ft in front and pushes along its normal, so a one-sided wall facing off
# the road pushes a car that touches it from the road out through it. For each wall the road polygons (normal y >
# 0.7) within 6 ft of its face's centroid, over its lower half, vote for the side they are on.
# python -I tools/arcade2049/wall_facing.py <collision.bin> [--list N] [--box x0 x1 z0 z1]
import collections, math, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'rush2049'))
import collision as C
c = C.Collision.parse(open(sys.argv[1], 'rb').read(), C.R2)
show = int(sys.argv[sys.argv.index('--list') + 1]) if '--list' in sys.argv else 10
box = [float(v) for v in sys.argv[sys.argv.index('--box') + 1:sys.argv.index('--box') + 5]] if '--box' in sys.argv else None
polys = []
for i, p in enumerate(c.polys):
    w = C.world_polygon(c, p); n = [x / 16384 for x in p['matrix'][3:6]]
    polys.append((i, p['flags'] & 0xF, w, n, [sum(v[k] for v in w) / len(w) for k in range(3)]))
grid = collections.defaultdict(list)
for k, (i, t, w, n, a) in enumerate(polys):
    if t <= 2 and n[1] > 0.7:
        for v in w + [a]:
            grid[(int(v[0] // 8), int(v[2] // 8))].append(k)
stats = collections.Counter(); away = []
for i, t, w, n, a in polys:
    if t not in (5, 6) or abs(n[1]) > 0.7:
        continue
    if box and not (box[0] < a[0] < box[1] and box[2] < a[2] < box[3]):
        continue
    lo = min(v[1] for v in w)
    front = back = 0
    seen = set()
    for gx in range(int(a[0] // 8) - 1, int(a[0] // 8) + 2):
        for gz in range(int(a[2] // 8) - 1, int(a[2] // 8) + 2):
            for k in grid[(gx, gz)]:
                if k in seen: continue
                seen.add(k)
                j, t2, w2, n2, b = polys[k]
                for v in w2 + [b]:
                    d = [v[q] - a[q] for q in range(3)]
                    if math.hypot(d[0], d[2]) > 6 or not (lo - 2 < v[1] < a[1]):
                        continue
                    s = sum(d[q] * n[q] for q in range(3))
                    if s > 0.5: front += 1
                    elif s < -0.5: back += 1
    if not front and not back: stats['no road beside'] += 1
    elif front and back: stats['road both sides'] += 1
    elif front: stats['faces the road'] += 1
    else:
        stats['faces AWAY from the road'] += 1; away.append((i, a))
print(dict(stats))
for i, a in away[:show]:
    print('  wall %d at (%.0f, %.0f, %.0f)' % (i, *a))
