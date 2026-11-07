"""Drives every AI lane point of a converted track through the game's ground pick and reports where a wheel would get
the wrong height (research aid; docs/custom_tracks.md#spline-road, docs/rush2049_research/collision.md).

    python tools/collision_probe.py OUTDIR [--near X,Z,R] [--tol FT] [--gap FT] [--why]

OUTDIR is tools/track_mod_test.bat output (collision.bin, path.bin). For each lane point (dropped onto the road by the
converter): the ground faces of its quadtree leaf whose local x/z footprint holds it (func_8006C670's test), of those
the highest whose flat plane is at most 5 ft above the point (the wheel probe's window), and that face's height there:
its spline segment's surface for a spline face (collision_seg_surface_8006BDA8), else its plane. Reports points where
no face is taken (a hole) or the height taken is more than --tol ft (default 3) from the point, with the face, its
segment and how far its plane is from the point. --near limits the report to points within R ft of (X, Z). --gap FT
mimics the crack fill of smooth collision (src/smooth_collision.cpp gap_tol, 1 ft; on unless a track sets
smooth_collision = false): when no face holds a point, a face whose footprint is within FT of it holds it.
"""
import argparse, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
sys.path.insert(0, HERE)
import numpy as np  # noqa: E402
import collision, paths  # noqa: E402
from track_mod_check import leaf_for  # noqa: E402

WINDOW = 5.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('outdir')
    ap.add_argument('--near')
    ap.add_argument('--tol', type=float, default=3.0)
    ap.add_argument('--gap', type=float, default=0.0, help='crack fill: a face within this many ft of a point holds it '
                    'when none does (smooth collision: 1)')
    ap.add_argument('--why', action='store_true', help='with each bad point, list every ground face holding it')
    a = ap.parse_args()
    c = collision.Collision.parse(open(os.path.join(a.outdir, 'collision.bin'), 'rb').read(), collision.R2)
    p = paths.parse(open(os.path.join(a.outdir, 'path.bin'), 'rb').read())
    segs = [collision.seg_fields(struct.unpack('>33f', r[:0x84])) for r in c.segs]
    near = tuple(float(v) for v in a.near.split(',')) if a.near else None

    faces = {}

    def face(pi):
        if pi not in faces:
            poly = c.polys[pi]
            vs = [collision.vert_xyz(c.verts[v]) for v in poly['verts']]
            M = np.array(poly['matrix'], float).reshape(3, 3) / 16384.0
            local = np.array([[0.0, 0.0, 0.0]] + [list(v) for v in vs[1:]])
            faces[pi] = (np.array(vs[0], float), M, local[:, [0, 2]], poly['seg'], poly['flags'] & 0xF)
        return faces[pi]

    def outside_by(q, poly2d):
        # How far q lies outside the convex footprint (0 inside): the largest distance past an edge line.
        worst, sign = 0.0, 0.0
        for i in range(len(poly2d)):
            a0, b0 = poly2d[i], poly2d[(i + 1) % len(poly2d)]
            cr = (b0[0] - a0[0]) * (q[1] - a0[1]) - (b0[1] - a0[1]) * (q[0] - a0[0])
            ln = np.hypot(b0[0] - a0[0], b0[1] - a0[1])
            if ln < 1e-9:
                continue
            if sign == 0.0 and abs(cr) > 1e-9:
                sign = 1.0 if cr > 0 else -1.0
            if sign != 0.0:
                worst = max(worst, -sign * cr / ln)
        return worst

    def inside(q, poly2d):
        sign = 0.0
        for i in range(len(poly2d)):
            a0, b0 = poly2d[i], poly2d[(i + 1) % len(poly2d)]
            cr = (b0[0] - a0[0]) * (q[1] - a0[1]) - (b0[1] - a0[1]) * (q[0] - a0[0])
            if abs(cr) < 1e-9:
                continue
            if sign == 0.0:
                sign = cr
            elif (cr > 0) != (sign > 0):
                return False
        return True

    def surface_y(k, x, z, y):
        S, T = segs[k], segs[(k + 1) % len(segs)]
        for _ in range(6):
            h0 = collision.seg_surface(S, T, [(x, y, z)])[0][0]
            h1 = collision.seg_surface(S, T, [(x, y + 1.0, z)])[0][0]
            if not np.isfinite(h0) or abs(h1 - h0) < 1e-6:
                break
            y -= h0 / (h1 - h0)
        return y

    bad, total = [], 0
    for li, ln in enumerate(p['lanes']):
        for pt in ln['points']:
            P = np.array(pt[:3], float)
            if near and np.hypot(P[0] - near[0], P[2] - near[1]) > near[2]:
                continue
            total += 1
            best, held = None, []
            for gap in ([0.0, a.gap] if a.gap > 0 else [0.0]):
                if best is not None:
                    break
                for pi in leaf_for(c, P[0], P[2]):
                    O, M, fp, seg, ty = face(pi)
                    if ty in (5, 6):
                        continue
                    d = M @ (P - O)
                    if (outside_by((d[0], d[2]), fp) > gap) if gap > 0 else not inside((d[0], d[2]), fp):
                        continue
                    n = M[1]
                    if abs(n[1]) < 1e-6:
                        continue
                    plane_y = O[1] - (n[0] * (P[0] - O[0]) + n[2] * (P[2] - O[2])) / n[1]
                    held.append((pi, seg, plane_y - P[1]))
                    if plane_y - P[1] > WINDOW:
                        continue        # over the window: the probe skips it
                    if best is None or plane_y > best[0]:
                        best = (plane_y, pi, seg)
            if best is None:
                bad.append((li, P, 'no face taken', None, None, None, held))
                continue
            plane_y, pi, seg = best
            y = surface_y(seg, P[0], P[2], P[1]) if seg is not None and seg < len(segs) else plane_y
            if abs(y - P[1]) > a.tol:
                bad.append((li, P, 'height %.1f ft off' % (y - P[1]), pi, seg, plane_y - P[1], held))
    print('%d lane points probed, %d bad' % (total, len(bad)))
    for li, P, what, pi, seg, off, held in bad[:40]:
        print('  lane %d at (%.0f, %.0f, %.0f): %s%s' % (li, P[0], P[1], P[2], what,
              '' if pi is None else ' (face %d, segment %s, its plane %+.1f ft)' % (pi, seg, off)))
        if a.why:
            print('    faces holding it: ' + (', '.join('%d seg %s plane %+.1f' % h for h in held) or 'none'))


if __name__ == '__main__':
    main()
