# Checks that Rush 2's body probe can see each wall from where it should: points on a wall's face pushed out along its
# normal by 0.25..3.75 ft (the probe takes a wall from 5 ft behind to 3.75 ft in front, docs/calspeed_research.md
# "Walls: California Speed's body collision") are looked up in a converted collision.bin with Rush 2's cell rule and
# tree walk; a point whose leaf doesn't list the wall is a spot where a car's body point passes it unseen.
# python -I tools/arcade2049/wall_reach.py <collision.bin> [x0 x1 z0 z1 y0 y1]
import collections, os, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'rush2049'))
import collision as C
c = C.Collision.parse(open(sys.argv[1], 'rb').read(), C.R2)
box = [float(v) for v in sys.argv[2:8]] if len(sys.argv) >= 8 else None
miss = collections.Counter(); total = collections.Counter(); bad = {}
for pi, p in enumerate(c.polys):
    if (p['flags'] & 0xF) not in (5, 6):
        continue
    w = C.world_polygon(c, p)
    a = [sum(v[k] for v in w) / len(w) for k in range(3)]
    if box and not (box[0] < a[0] < box[1] and box[2] < a[2] < box[3] and box[4] < a[1] < box[5]):
        continue
    n = [x / 16384 for x in p['matrix'][3:6]]
    # sample the face: the corners pulled 10% toward the centroid, and the centroid
    samples = [a] + [[a[k] + (v[k] - a[k]) * 0.9 for k in range(3)] for v in w]
    for s in samples:
        for d in (0.25, 1.0, 2.0, 3.0, 3.75):
            q = [s[k] + n[k] * d for k in range(3)]
            leaf = C.leaf_for_cell(c, C.cell(q[0], C.R2), C.cell(q[2], C.R2), C.R2)
            total[d] += 1
            if not leaf or pi not in leaf:
                miss[d] += 1; bad.setdefault(pi, []).append((d, q))
print('points missed / checked by distance in front:', {d: '%d/%d' % (miss[d], total[d]) for d in sorted(total)})
print('walls with a miss:', len(bad))
for pi, l in list(bad.items())[:15]:
    print('  wall %d: %s' % (pi, ', '.join('%.2f ft at (%.0f,%.0f,%.0f)' % (d, *q) for d, q in l[:3])))
