"""Rush 2 track sections and their visibility table, from the ROM (no game run).

A track's top-level placement records are its sections; region i (the camera inside section i's box) has a 128-bit
mask of the sections drawn from there (func_8007C27C). A section its own region hides is a low-detail stand-in the
game draws only from far away: either a copy of real scenery it swaps in (hiding the real section) or a backdrop
nothing else draws. src/draw_distance.cpp explains how the extended draw distance treats them.

    python tools/rush2_sections.py list TRACK                 sections: name, position, box, regions that show it
    python tools/rush2_sections.py region TRACK R[,R...]      sections region R shows
    python tools/rush2_sections.py copies [TRACK...]          stand-ins whose 3D box overlaps a real section the
                                                              game hides wherever the stand-in is shown
    python tools/rush2_sections.py exclusive [TRACK...] [--gap FT]
                                                              pairs of sections no region draws together whose boxes
                                                              are within FT (default 600) on the ground: copy/original
                                                              suspects (neighbors are nearly always drawn together)
    python tools/rush2_sections.py sheet OUT.png TRACK:A:B [TRACK:A:B...]
        one row per pair: A red, B blue, from above, from the side (x, y) and from the side (z, y)
    python tools/rush2_sections.py render TRACK COPY OUT.png [--real S,S...]
        draws stand-in COPY (red) over real sections (grey; default: the ones it overlaps plus every real section
        within its regions' hidden set whose box is near it) from above, from the side, and in perspective from
        each region that shows COPY.

TRACK = Rush 2 slot (0 Las Vegas, 1 Lower Manhattan, 2 Honolulu, 3 Upper Manhattan, ...). Sections by index or name.
RUSH2_ROM selects the ROM (tools/rush2049/roms.py).
"""
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rush2049'))
import model     # noqa: E402
import placement  # noqa: E402
import roms      # noqa: E402

BASE = 0x00400000


class Track:
    def __init__(self, r2, t):
        self.t = t
        self.masks = model.pvs_rush2(r2, t)
        self.n = len(self.masks)
        entries, self.recs = placement.parse_r2(r2.asset(0x3F + t))
        self.top = []
        i = entries[0][1] if entries else -1
        while i >= 0:
            self.top.append(self.recs[i])
            i = self.recs[i].next
        self.geo = r2.asset(0x33 + t)
        self.models = {m['name']: m for m in model.R2Model(self.geo).models}
        self.img = None

    def shows(self, region, s):
        return bool(self.masks[region] >> s & 1)

    def standin(self, s):
        return s < self.n and not self.shows(s, s)

    def regions_showing(self, s):
        return [g for g in range(self.n) if self.shows(g, s)]

    def index(self, key):
        if isinstance(key, int) or key.isdigit():
            return int(key)
        return next(i for i, r in enumerate(self.top) if r.name == key)

    def box(self, s):
        r = self.top[s]
        b = r.bbox
        return [r.pos[0] + b[0], r.pos[1] + b[1], r.pos[2] + b[2], r.pos[0] + b[3], r.pos[1] + b[4], r.pos[2] + b[5]]

    def triangles(self, s):
        """World-space triangles of section s's model, LOD 0 (F3DEX2 G_VTX/G_TRI1/G_TRI2, units 1/16 ft)."""
        if self.img is None:
            self.img, _ = model.emulate_rush2_load(self.geo, BASE)
        r = self.top[s]
        m = self.models.get(r.name)
        if not m or not m['lods']:
            return []
        img, out, vb = self.img, [], [None] * 64

        def world(v):
            x, y, z = v[0] / 16, v[1] / 16, v[2] / 16
            M = r.m
            return (r.pos[0] + M[0] * x + M[3] * y + M[6] * z, r.pos[1] + M[1] * x + M[4] * y + M[7] * z,
                    r.pos[2] + M[2] * x + M[5] * y + M[8] * z)
        stack, o, steps = [], m['lods'][0][3] + BASE, 0
        while steps < 200000:
            steps += 1
            w0, w1 = struct.unpack_from('>II', img, o - BASE)
            op = w0 >> 24
            if op == 0x01:
                cnt = (w0 >> 12) & 0xFF
                v0 = ((w0 >> 1) & 0x7F) - cnt
                a = (w1 & 0xFFFFFF) - BASE
                for k in range(cnt):
                    if 0 <= v0 + k < 64:
                        vb[v0 + k] = world(struct.unpack_from('>hhh', img, a + k * 16))
            elif op in (0x05, 0x06):
                for w in ((w0, w1) if op == 0x06 else (w0,)):
                    t = [vb[((w >> sh) & 0xFF) // 2] for sh in (16, 8, 0)]
                    if None not in t:
                        out.append(t)
            elif op == 0xDE:
                if not (w0 >> 16) & 0xFF:
                    stack.append(o + 8)
                o = w1 & 0xFFFFFF
                continue
            elif op == 0xDF:
                if not stack:
                    break
                o = stack.pop()
                continue
            o += 8
        return out


def overlap(a, b):
    v = 1.0
    for k in range(3):
        d = min(a[k + 3], b[k + 3]) - max(a[k], b[k])
        if d <= 0:
            return 0.0
        v *= d
    return v


def volume(a):
    return (a[3] - a[0]) * (a[4] - a[1]) * (a[5] - a[2])


def hidden_with(tr, s):
    """Real sections the game hides in every region that shows stand-in s."""
    regs = tr.regions_showing(s)
    return [j for j in range(tr.n) if not tr.standin(j) and not any(tr.shows(g, j) for g in regs)]


def copies(tr):
    out = []
    for s in range(tr.n):
        if not tr.standin(s):
            continue
        hits = [(j, overlap(tr.box(s), tr.box(j)) / volume(tr.box(s))) for j in hidden_with(tr, s)]
        out.append((s, [h for h in hits if h[1] > 0]))
    return out


def exclusive(tr, max_gap):
    """(gap, a, b): sections a < b that no region draws together, boxes within max_gap on the ground."""
    sets = [set(tr.regions_showing(s)) for s in range(tr.n)]
    out = []
    for a in range(tr.n):
        for b in range(a + 1, tr.n):
            if sets[a] & sets[b] or not sets[a] or not sets[b]:
                continue
            A, B = tr.box(a), tr.box(b)
            dx = max(0, max(A[0], B[0]) - min(A[3], B[3]))
            dz = max(0, max(A[2], B[2]) - min(A[5], B[5]))
            d = (dx * dx + dz * dz) ** 0.5
            if d <= max_gap:
                out.append((d, a, b))
    return sorted(out)


def silhouettes(tr, step=1.0):
    """For every pair of sections no region draws together: how alike they look from where one stands in for the
    other. From each region g that shows X (camera at the center of g's box), both are drawn as 360-degree
    panoramas (azimuth x elevation, `step` degrees a pixel) and compared: overlap / union of the covered pixels.
    Yields (median overlap over g, x, y, regions) for both orders of each pair."""
    import math
    import numpy as np
    from PIL import Image, ImageDraw

    W, H = int(360 / step), int(180 / step)
    tris = {}
    masks = {}

    def mask(g, s):
        key = (g, s)
        if key in masks:
            return masks[key]
        if s not in tris:
            tris[s] = np.array(tr.triangles(s)) if tr.triangles(s) else np.zeros((0, 3, 3))
        b = tr.box(g)
        cam = np.array([(b[0] + b[3]) / 2, (b[1] + b[4]) / 2, (b[2] + b[5]) / 2])
        img = Image.new('1', (W, H), 0)
        d = ImageDraw.Draw(img)
        if len(tris[s]):
            v = tris[s] - cam
            az = np.degrees(np.arctan2(v[..., 2], v[..., 0]))
            el = np.degrees(np.arctan2(v[..., 1], np.hypot(v[..., 0], v[..., 2])))
            for t in range(len(v)):
                a = az[t]
                if a.max() - a.min() > 180:   # straddles the seam behind: draw it twice, shifted
                    a = np.where(a < 0, a + 360, a)
                    shifts = (0, -360)
                else:
                    shifts = (0,)
                for sh in shifts:
                    pts = [((a[k] + sh + 180) / step, (90 - el[t][k]) / step) for k in range(3)]
                    d.polygon(pts, fill=1)
        m = np.array(img, dtype=bool)
        masks[key] = m
        return m

    sets = [set(tr.regions_showing(s)) for s in range(tr.n)]
    for _, a, b in exclusive(tr, float('inf')):
        for x, y in ((a, b), (b, a)):
            vals = []
            for g in sorted(sets[x]):
                mx, my = mask(g, x), mask(g, y)
                u = (mx | my).sum()
                if u:
                    vals.append((mx & my).sum() / u)
            if vals:
                vals.sort()
                yield vals[len(vals) // 2], x, y, len(vals)


def sheet(r2, out_png, pairs):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    import numpy as np

    tracks = {}
    fig, axes = plt.subplots(len(pairs), 3, figsize=(13, 3.2 * len(pairs)), squeeze=False)
    for row, (t, a, b) in enumerate(pairs):
        tr = tracks.setdefault(t, Track(r2, t)) if t not in tracks else tracks[t]
        a, b = tr.index(a), tr.index(b)
        ta, tb = np.array(tr.triangles(a)), np.array(tr.triangles(b))
        for ax, idx in zip(axes[row], ([0, 2], [0, 1], [2, 1])):
            for tri, fc, ec in ((tb, (0, 0.3, 1, 0.2), (0, 0.2, 0.8, 0.7)), (ta, (1, 0, 0, 0.2), (0.8, 0, 0, 0.8))):
                if len(tri):
                    ax.add_collection(PolyCollection(tri[..., idx], facecolors=fc, edgecolors=ec, linewidths=0.4))
            ax.autoscale()
            ax.set_aspect('equal')
            if idx == [0, 2]:
                ax.invert_yaxis()
        axes[row][0].set_title('t%d  red %d %s%s   blue %d %s%s' % (
            t, a, tr.top[a].name, '*' if tr.standin(a) else '', b, tr.top[b].name, '*' if tr.standin(b) else ''),
            fontsize=8, loc='left')
    fig.tight_layout()
    fig.savefig(out_png, dpi=70)


def render(tr, s, out_png, real):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    import numpy as np

    regs = tr.regions_showing(s)
    cams = [np.array([(tr.box(g)[0] + tr.box(g)[3]) / 2, tr.box(g)[1] + 40, (tr.box(g)[2] + tr.box(g)[5]) / 2])
            for g in regs[:4]]
    copy_t = np.array(tr.triangles(s))
    real_t = [np.array(tr.triangles(j)) for j in real]
    real_t = np.concatenate([x for x in real_t if len(x)]) if any(len(x) for x in real_t) else np.zeros((0, 3, 3))
    pts = np.concatenate([copy_t.reshape(-1, 3), real_t.reshape(-1, 3)])
    ctr = pts.mean(axis=0)

    views = [('top (x, z)', lambda p: p[..., [0, 2]]), ('side (x, y)', lambda p: p[..., [0, 1]]),
             ('side (z, y)', lambda p: p[..., [2, 1]])]
    for g, cam in zip(regs, cams):
        f = ctr - cam
        f /= np.linalg.norm(f)
        right = np.cross(f, [0, 1, 0])
        right /= np.linalg.norm(right)
        up = np.cross(right, f)

        def persp(p, cam=cam, f=f, right=right, up=up):
            d = p - cam
            z = np.maximum((d * f).sum(-1), 1.0)
            return np.stack([(d * right).sum(-1) / z, (d * up).sum(-1) / z], -1)
        views.append(('from region %d %s' % (g, tr.top[g].name), persp))

    fig, axes = plt.subplots(2, (len(views) + 1) // 2, figsize=(6 * ((len(views) + 1) // 2), 11))
    for ax, (title, proj) in zip(axes.flat, views):
        if len(real_t):
            ax.add_collection(PolyCollection(proj(real_t), facecolors=(0.6, 0.6, 0.6, 0.35),
                                             edgecolors=(0.3, 0.3, 0.3, 0.5), linewidths=0.3))
        if len(copy_t):
            ax.add_collection(PolyCollection(proj(copy_t), facecolors=(1, 0, 0, 0.25), edgecolors=(0.8, 0, 0, 0.9),
                                             linewidths=0.6))
        ax.autoscale()
        ax.set_aspect('equal')
        if title.startswith('top'):
            ax.invert_yaxis()
        ax.set_title(title, fontsize=9)
    for ax in list(axes.flat)[len(views):]:
        ax.axis('off')
    fig.suptitle('track %d: stand-in %d %s (red) over %s (grey)' % (
        tr.t, s, tr.top[s].name, ', '.join('%d %s' % (j, tr.top[j].name) for j in real)), fontsize=10)
    fig.tight_layout()
    fig.savefig(out_png, dpi=90)


def main(argv):
    if not argv:
        print(__doc__)
        return 1
    r2 = roms.Rush2()
    cmd = argv[0]
    if cmd == 'list':
        tr = Track(r2, int(argv[1]))
        for s in range(tr.n):
            r = tr.top[s]
            print('%3d %-14s %s pos=(%6.0f %5.0f %6.0f) box=%s shown_from=%s' % (
                s, r.name, 'STAND-IN' if tr.standin(s) else '        ', *r.pos,
                ['%.0f' % v for v in tr.box(s)], tr.regions_showing(s)))
    elif cmd == 'region':
        tr = Track(r2, int(argv[1]))
        for g in map(tr.index, argv[2].split(',')):
            print('region %d %s shows %s' % (g, tr.top[g].name, [s for s in range(tr.n) if tr.shows(g, s)]))
    elif cmd == 'copies':
        for t in (map(int, argv[1:]) if len(argv) > 1 else range(12)):
            tr = Track(r2, t)
            for s, hits in copies(tr):
                print('t%-2d %3d %-14s %s' % (t, s, tr.top[s].name, ', '.join(
                    '%d %s %.1f%%' % (j, tr.top[j].name, 100 * v) for j, v in hits) if hits else '-'))
    elif cmd == 'exclusive':
        gap = float(argv[argv.index('--gap') + 1]) if '--gap' in argv else 600.0
        args = [a for i, a in enumerate(argv[1:], 1) if a != '--gap' and argv[i - 1] != '--gap']
        for t in (map(int, args) if args else range(12)):
            tr = Track(r2, t)
            for d, a, b in exclusive(tr, gap):
                print('%d:%d:%d  %5.0f  %-14s%s %-14s%s' % (t, a, b, d, tr.top[a].name, '*' if tr.standin(a) else ' ',
                                                            tr.top[b].name, '*' if tr.standin(b) else ' '))
    elif cmd == 'sheet':
        pairs = [tuple(int(x) if x.isdigit() else x for x in p.split(':')) for p in argv[2:]]
        sheet(r2, argv[1], pairs)
        print('wrote', argv[1])
    elif cmd == 'render':
        tr = Track(r2, int(argv[1]))
        s = tr.index(argv[2])
        if '--real' in argv:
            real = [tr.index(x) for x in argv[argv.index('--real') + 1].split(',')]
        else:
            real = [j for j, _ in dict(copies(tr))[s]]
        render(tr, s, argv[3], real)
        print('wrote', argv[3])
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
