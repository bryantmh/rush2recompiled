"""PS1 SF Rush collision (COLLDAT<n>/COLLMAP.DAT + .SIZ, FLAGS.DAT, BOUNDS.DAT; docs/sfrush_ps1.md#collision).

Loader func_8004FBFC; ground query func_8004D12C. Units 0.1 ft (like the AI paths), axes (x, -height, y) of the track frame (track.py).
A chunk: +0x04 origin (1/256 ft), +0x10 u32 vertex count, +0x14 u8 polygon count, +0x24 u8 wall count (A),
+0x25 u8 B count, +0x28 vertices (3 x s32), then A records (0x18), B records (0x20), then ground records: 0x20 bytes
when the polygon's FLAGS byte & 7 == 0, else 0x28. FLAGS.DAT has one byte per polygon, chunks in order.
  A wall (0x18): s32 d, s16 nx, s16 nz (16384 = 1), s32 x min, x max, z min, z max: a vertical line with no height,
  the diagonal of that box that lies on the plane n . p = d.
  B (0x20): s32, s32, then x0, x1, z0, z1, s32 ya, yb (a wall with heights?).
  ground (0x20): s32 d, s32 xmin, xmax, zmin, zmax, u8 idx[4], s16 n[3] (s16 pad).
  ground (0x28): s32 d0, d1, xmin, xmax, zmin, zmax, u8 idx[4], s16 n0[3], s16 n1[3] (a bent quad: two planes).

  python collision.py stats TRACK
  python collision.py png TRACK OUT.png        top view, ground colored by flags, walls in red
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps1 import Disc


def read(disc, track):
    flags = disc.read('COLLDAT%d/FLAGS.DAT' % track)
    chunks = []; fo = 0
    for c in disc.chunks('COLLDAT%d/COLLMAP' % track):
        nv = struct.unpack_from('<I', c, 0x10)[0]; npol = c[0x14]; na = c[0x24]; nb = c[0x25]
        verts = [tuple(v / 10 for v in struct.unpack_from('<3i', c, 0x28 + 12 * i)) for i in range(nv)]
        o = 0x28 + 12 * nv
        F = flags[fo:fo + npol]; fo += npol
        walls, bs, ground = [], [], []
        for i in range(na):
            d, nx, nz, x0, x1, z0, z1 = struct.unpack_from('<ihh4i', c, o); o += 0x18
            # A bounding box: the segment is the diagonal lying on the plane.
            if abs(nx * x0 + nz * z1 - d) + abs(nx * x1 + nz * z0 - d) < abs(nx * x0 + nz * z0 - d) + abs(nx * x1 + nz * z1 - d):
                z0, z1 = z1, z0
            walls.append(dict(d=d, n=(nx / 16384, nz / 16384), a=(x0 / 10, z0 / 10), b=(x1 / 10, z1 / 10), flags=F[i]))
        for i in range(nb):
            v = struct.unpack_from('<8i', c, o); o += 0x20
            bs.append(dict(raw=v, flags=F[na + i]))
        for i in range(na + nb, npol):
            f = F[i]
            if f & 7:
                d0, d1, x0, x1, z0, z1 = struct.unpack_from('<6i', c, o); idx = c[o + 0x18:o + 0x1C]
                n = struct.unpack_from('<6h', c, o + 0x1C); o += 0x28
                ground.append(dict(idx=tuple(idx), flags=f, normals=(n[:3], n[3:]), d=(d0, d1)))
            else:
                d, x0, x1, z0, z1 = struct.unpack_from('<5i', c, o); idx = c[o + 0x14:o + 0x18]
                n = struct.unpack_from('<3h', c, o + 0x18); o += 0x20
                ground.append(dict(idx=tuple(idx), flags=f, normals=(n,), d=(d,)))
        assert o == len(c), 'collision chunk size'
        chunks.append(dict(verts=verts, walls=walls, b=bs, ground=ground))
    return chunks


if __name__ == '__main__':
    t = int(sys.argv[2]); ch = read(Disc(), t)
    if sys.argv[1] == 'stats':
        from collections import Counter
        print(len(ch), 'chunks', sum(len(c['ground']) for c in ch), 'ground', sum(len(c['walls']) for c in ch), 'walls',
              sum(len(c['b']) for c in ch), 'B')
        print(Counter(g['flags'] for c in ch for g in c['ground']))
        for c in ch:
            for b in c['b']:
                print('B', b['flags'], b['raw'])
    elif sys.argv[1] == 'png':
        import matplotlib; matplotlib.use('Agg')
        import matplotlib.pyplot as plt
        from matplotlib.patches import Polygon
        fig, ax = plt.subplots(figsize=(16, 16))
        cols = {0: '#888888', 0x10: '#33aa33', 0x08: '#3366ff', 0x80: '#ffaa00', 0x18: '#00dddd', 0x90: '#aa00aa'}
        for c in ch:
            V = c['verts']
            for g in c['ground']:
                pts = [(V[i][0], V[i][2]) for i in g['idx']]
                ax.add_patch(Polygon(pts, closed=True, fc=cols.get(g['flags'] & 0xF8, '#000000'), ec='none'))
            for w in c['walls']:
                ax.plot([w['a'][0], w['b'][0]], [w['a'][1], w['b'][1]], 'r-', lw=0.4)
            for b in c['b']:
                r = b['raw']; ax.plot([r[2] / 10, r[3] / 10], [r[4] / 10, r[5] / 10], 'm-', lw=2)
        ax.autoscale(); ax.set_aspect('equal'); plt.savefig(sys.argv[3], dpi=70)
