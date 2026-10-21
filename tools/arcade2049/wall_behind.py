# Finds road a car can drive on that sits behind a wall within Rush 2's body probe reach. The probe
# (car_wall_collision_query_8006FC94) takes a wall from 5 ft behind its plane to 3.75 ft in front and pushes the car
# out along the wall's normal, so a body point on road behind a wall is pushed through the wall to its front: a wall
# that should hold from that side lets the car through (a thin rail with road on both sides, or a wall facing off the
# road). Points on every road polygon (type <= 2, facing up), lifted 1 and 2.5 ft, are looked up in a converted
# collision.bin with Rush 2's cell rule and tested against the walls of their leaf.
# python -I tools/arcade2049/wall_behind.py <collision.bin> [--list N] [--box x0 x1 z0 z1]
import collections, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'rush2049'))
import collision as C
c = C.Collision.parse(open(sys.argv[1], 'rb').read(), C.R2)
show = int(sys.argv[sys.argv.index('--list') + 1]) if '--list' in sys.argv else 20
box = [float(v) for v in sys.argv[sys.argv.index('--box') + 1:sys.argv.index('--box') + 5]] if '--box' in sys.argv else None
sub = lambda a, b: [a[i] - b[i] for i in range(3)]
dot = lambda a, b: a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
cross = lambda a, b: [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]
polys = []
for p in c.polys:
    polys.append((p['flags'] & 0xF, C.world_polygon(c, p), [x / 16384 for x in p['matrix'][3:6]]))
def inside(q, w, n):
    # q projects inside the (convex) polygon along n: every edge sees it on the same side, either winding
    s = [dot(cross(sub(w[(i + 1) % len(w)], w[i]), sub(q, w[i])), n) for i in range(len(w))]
    return min(s) >= 0 or max(s) <= 0
hits = collections.defaultdict(list)     # wall -> [(road polygon, point, distance)]
checked = 0
for ri, (t, w, n) in enumerate(polys):
    if t > 2 or n[1] < 0.7:
        continue
    a = [sum(v[k] for v in w) / len(w) for k in range(3)]
    if box and not (box[0] < a[0] < box[1] and box[2] < a[2] < box[3]):
        continue
    samples = [a] + [[a[k] + (v[k] - a[k]) * 0.85 for k in range(3)] for v in w]
    for s in samples:
        for lift in (1.0, 2.5):
            q = [s[0], s[1] + lift, s[2]]
            checked += 1
            for wi in C.leaf_for_cell(c, C.cell(q[0], C.R2), C.cell(q[2], C.R2), C.R2) or []:
                t2, w2, n2 = polys[wi]
                if t2 not in (5, 6):
                    continue
                d = dot(sub(q, w2[0]), n2)
                if -5 < d < 0 and inside(q, w2, n2):
                    hits[wi].append((ri, q, d))
print('road points checked: %d; walls with road behind them in reach: %d' % (checked, len(hits)))
for wi, l in sorted(hits.items(), key=lambda kv: -len(kv[1]))[:show]:
    t2, w2, n2 = polys[wi]
    ys = [v[1] for v in w2]
    ri, q, d = l[0]
    print('  wall %d (n %.2f %.2f %.2f, y %.1f..%.1f): %d points, e.g. road %d at (%.0f, %.0f, %.0f) %.2f ft behind'
          % (wi, *n2, min(ys), max(ys), len(l), ri, *q, -d))
