#!/usr/bin/env python3
"""Finds places on a Rush 2049 track where a car could fall through: AI lane points with a floor under them in the
original 2049 collision file but none in the converted one as the game looks it up (research aid for GitHub issue 37,
docs/rush2049_research/collision.md).

For each lane point of the converted path, the floor polygons (any type but walls 5/6) whose x/z footprint holds the
point and whose plane is within --below ft under to --above ft over it are found three ways: every polygon of the
2049 file, the 2049 file's own leaf for the point's cell (2049's walk), and the converted file's leaf (the recomp's
walk of a tagged file). Prints the lane stretches where the converted lookup finds nothing but the 2049 file has a
floor, and those where the 2049 file has none either (holes in the original, or jumps).

    python floor_gaps.py K [--out DIR] [--below FT] [--above FT]
        K: 2049 track 1-6 (Marina, Haight, Civic, Metro, Mission, Presidio); DIR: the converter test's output
        folder (cpp_test/out/static/track<K>, from cpp_test/build.bat); the 2049 file is read from RUSH2049_ROM.
"""
import argparse, math, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import collision as col
import paths
import roms

NAMES = ['Marina', 'Haight', 'Civic', 'Metro', 'Mission', 'Presidio']


class Floors:
    """Floor polygons of a collision file as planes and x/z outlines, for point queries."""

    def __init__(self, c):
        self.c = c
        self.polys = {}
        for i, p in enumerate(c.polys):
            if (p['flags'] & 0xF) in col.TYPE_WALL or len(p['verts']) < 3:
                continue
            w = np.array(col.world_polygon(c, p), float)
            n = np.cross(w[1] - w[0], w[2] - w[0])
            if abs(n[1]) < 1e-6:
                continue
            self.polys[i] = (w, n)
        ids = list(self.polys)
        self.ids = np.array(ids)
        self.lo = np.array([self.polys[i][0][:, [0, 2]].min(0) for i in ids])
        self.hi = np.array([self.polys[i][0][:, [0, 2]].max(0) for i in ids])

    def holds(self, i, x, y, z, below, above):
        w, n = self.polys[i]
        pts = w[:, [0, 2]]
        sign = 0
        for a, b in zip(pts, np.roll(pts, -1, 0)):
            s = (b[0] - a[0]) * (z - a[1]) - (b[1] - a[1]) * (x - a[0])
            if abs(s) < 1e-9:
                continue
            if sign == 0:
                sign = 1 if s > 0 else -1
            elif (s > 0) != (sign > 0):
                return False
        h = w[0][1] - (n[0] * (x - w[0][0]) + n[2] * (z - w[0][2])) / n[1]
        return -below <= h - y <= above

    def under(self, x, y, z, below, above, candidates=None):
        if candidates is None:
            m = (self.lo[:, 0] <= x) & (x <= self.hi[:, 0]) & (self.lo[:, 1] <= z) & (z <= self.hi[:, 1])
            candidates = self.ids[m]
        return [i for i in candidates if i in self.polys and self.holds(i, x, y, z, below, above)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('k', type=int)
    ap.add_argument('--out', help='converted track folder (default cpp_test/out/static/track<K>)')
    ap.add_argument('--below', type=float, default=12.0)
    ap.add_argument('--above', type=float, default=6.0)
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    out = a.out or os.path.join(here, 'cpp_test', 'out', 'static', 'track%d' % a.k)
    src = col.Collision.parse(roms.Rush2049().file(138 + a.k), col.R49)
    conv = col.Collision.parse(open(os.path.join(out, 'collision.bin'), 'rb').read(), col.R2)
    path = paths.parse(open(os.path.join(out, 'path.bin'), 'rb').read())
    fs, fc = Floors(src), Floors(conv)
    print('%s: 2049 file %d polygons (%d floors), converted %d (%d floors)' %
          (NAMES[a.k - 1], len(src.polys), len(fs.polys), len(conv.polys), len(fc.polys)))
    for li, lane in enumerate(path['lanes']):
        rows = []
        for pi, (x, y, z, _, _) in enumerate(lane['points']):
            cx, cz = col.cell(x, col.R49), col.cell(z, col.R49)
            orig = fs.under(x, y, z, a.below, a.above)
            orig_leaf = fs.under(x, y, z, a.below, a.above, col.leaf_for_cell(src, cx, cz, col.R49) or [])
            conv_leaf = fc.under(x, y, z, a.below, a.above, col.leaf_for_cell(conv, cx, cz, col.R49) or [])
            conv_all = fc.under(x, y, z, a.below, a.above)
            kind = None
            if orig and not conv_leaf:
                kind = 'converted lookup misses (converted has %s, 2049 leaf has %s)' % (
                    'floor' if conv_all else 'no floor', 'floor' if orig_leaf else 'none')
            elif not orig:
                kind = 'no floor in 2049 either'
            rows.append((pi, (x, y, z), kind))
        i = 0
        while i < len(rows):
            if rows[i][2] is None:
                i += 1
                continue
            j = i
            while j + 1 < len(rows) and rows[j + 1][2] == rows[i][2]:
                j += 1
            p = rows[(i + j) // 2][1]
            print('  lane %d points %d-%d: %s, middle (%d, %d, %d)' % (li, i, j, rows[i][2], *p))
            i = j + 1


if __name__ == '__main__':
    main()
