# Finds back-to-back walls in a collision.bin (Rush 2 layout, as the converter writes it): pairs of wall polygons on one
# plane (within --tol ft) facing opposite ways whose faces overlap. Rush 2's body probe takes a wall from 5 ft behind
# its plane, so a car reaching such a pair is pushed out by the far face's normal: through the wall
# (docs/custom_tracks.md, Collision surfaces). python -I tools/arcade2049/wall_twins.py <collision.bin> [--tol 0.3] [--list N]
import collections, math, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'rush2049'))
import collision as C
c = C.Collision.parse(open(sys.argv[1], 'rb').read(), C.R2)
tol = float(sys.argv[sys.argv.index('--tol') + 1]) if '--tol' in sys.argv else 0.3
show = int(sys.argv[sys.argv.index('--list') + 1]) if '--list' in sys.argv else 10
walls = []
for i, p in enumerate(c.polys):
    if (p['flags'] & 0xF) in (5, 6):
        w = C.world_polygon(c, p); n = [x / 16384 for x in p['matrix'][3:6]]
        walls.append((i, w, n, [sum(v[k] for v in w) / len(w) for k in range(3)]))
grid = collections.defaultdict(list)
for k, (i, w, n, a) in enumerate(walls):
    grid[(int(a[0] // 16), int(a[2] // 16))].append(k)
def inside(q, w, n):
    # q projected on w's plane lies inside w (convex), with a small margin
    for a, b in zip(w, w[1:] + w[:1]):
        e = [b[k] - a[k] for k in range(3)]; d = [q[k] - a[k] for k in range(3)]
        cr = [e[1] * d[2] - e[2] * d[1], e[2] * d[0] - e[0] * d[2], e[0] * d[1] - e[1] * d[0]]
        if sum(cr[k] * n[k] for k in range(3)) < -0.05 * math.hypot(*e):
            return False
    return True
pairs = []
for k, (i, w, n, a) in enumerate(walls):
    for gx in range(int(a[0] // 16) - 1, int(a[0] // 16) + 2):
        for gz in range(int(a[2] // 16) - 1, int(a[2] // 16) + 2):
            for m in grid[(gx, gz)]:
                j, w2, n2, b = walls[m]
                if j <= i or sum(x * y for x, y in zip(n, n2)) > -0.98:
                    continue
                if max(abs(sum((v[t] - a[t]) * n[t] for t in range(3))) for v in w2) > tol:
                    continue
                pts = [b] + [[b[t] + (v[t] - b[t]) * 0.8 for t in range(3)] for v in w2]
                s = sum(inside(q, w, n) for q in pts)
                if s >= 2:
                    pairs.append((i, j, a))
print('walls', len(walls), 'back-to-back pairs', len(pairs))
for i, j, a in pairs[:show]:
    print('  %d / %d at (%.0f, %.0f, %.0f)' % (i, j, *a))
