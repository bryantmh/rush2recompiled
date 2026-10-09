"""Renders a car's textured body from several angles in Rush 1 and Rush 2 (offline, no game run), to compare the SF
Rush decal (tools/rush1/cardecal.py, src/car1_decals.cpp) with the Rush 1 car it comes from.

Usage: python carview.py CAR OUT.png [--scale N] [--views left,rear,...]
       python carview.py --score [CAR ...]     how much of Rush 1's decal the Rush 2 car shows (see score())   CAR = a tools/rush1/cartex.py car name;
       a car outside cardecal.CARS gets no decal; each view is 260x200 pixels times N
Rows: Rush 1 (RED paint) | Rush 2 with the decal | Rush 2 without. Columns: left, right, front 3/4, rear 3/4, top.
Rush 2's grey paint ramps are tinted like Rush 1's red so the two read alike. Car axes: x side, y up, z length.
"""
import math, os, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
import cardecal
from cartex import *

PARTS = ('FL1', 'FR1', 'RL1', 'RR1', 'TOP1', 'WIN1')
VIEWS = [('left', 90, 10), ('right', -90, 10), ('front', 35, 20), ('rear', 215, 20), ('top', 90, 80)]
BASE_W, BASE_H = 260, 200


def textured_tris(d, game, palette, override=None, flag=None):
    """-> ([(P[3], UV[3], texels, flags)], palette rgb, alpha) for the car's D0 body. texels: np.uint8 2D; flags: bool 2D
    marking decal texels (flag(texels) for Rush 1, the decal's texels for Rush 2 with `override`), or None."""
    m = meshes(d, game)
    pre = next(n for n in m if n.endswith('D0_FL1'))[:-6]
    out = []
    pal = np.array([c[:3] for c in palette], dtype=np.uint8)
    alpha = np.array([c[3] for c in palette], dtype=np.uint8)
    if game == 1:
        alpha[:] = 255      # Rush 1 draws index 0 (its glass blue) opaque
    cache, flags = {}, {}
    for part in PARTS:
        for vs, tex in m.get(f'{pre}D0_{part}', []):
            if tex is None: continue
            off, w, h, uls, ult = tex
            if off not in cache:
                tx = np.frombuffer(bytes(d[off:off + w * h]), dtype=np.uint8).reshape(h, w).copy()
                flags[off] = flag(tx) if flag is not None else np.zeros((h, w), dtype=bool)
                if override is not None:
                    # The game's SF Rush paint (src/car1_stripes.cpp): the decal over Rush 2's own paint.
                    if off in override:
                        o = np.array(override[off], dtype=np.uint8).reshape(h, w)
                        tx = np.where(o != 0, o, tx)
                        flags[off] = o != 0
                cache[off] = tx
            out.append(([v[:3] for v in vs], [(v[3] - uls, v[4] - ult) for v in vs], cache[off], flags[off]))
    return out, pal, alpha


def render(tris, pal, alpha, yaw, pitch, scale, flags_out=None):
    """The car from (yaw, pitch); with flags_out (bool H x W array) also marks the pixels showing decal texels."""
    W, H = int(BASE_W * scale), int(BASE_H * scale)
    img = np.full((H, W, 3), 40, dtype=np.uint8)
    zb = np.full((H, W), -1e9)
    cy, sy = math.cos(math.radians(yaw)), math.sin(math.radians(yaw))
    cp, sp = math.cos(math.radians(pitch)), math.sin(math.radians(pitch))

    def xf(p):
        x, y, z = p[0], p[1] - 35, p[2]
        x, z = x * cy - z * sy, x * sy + z * cy          # yaw about y
        y, z = y * cp - z * sp, y * sp + z * cp          # pitch about x
        return (W / 2 + x * scale, H / 2 - y * scale, z)
    light = np.array([0.4, 0.8, 0.45]); light /= np.linalg.norm(light)
    for P, UV, tx, fl in tris:
        q = [xf(p) for p in P]
        (x0, y0, z0), (x1, y1, z1), (x2, y2, z2) = q
        den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
        if abs(den) < 1e-6: continue
        a = np.subtract(P[1], P[0]); b = np.subtract(P[2], P[0]); n = np.cross(a, b)
        nn = np.linalg.norm(n)
        shade = 0.55 + 0.45 * abs(float(np.dot(n / nn, light))) if nn > 0 else 1.0
        bx0, bx1 = max(int(min(x0, x1, x2)), 0), min(int(max(x0, x1, x2)) + 1, W - 1)
        by0, by1 = max(int(min(y0, y1, y2)), 0), min(int(max(y0, y1, y2)) + 1, H - 1)
        if bx0 > bx1 or by0 > by1: continue
        X, Y = np.meshgrid(np.arange(bx0, bx1 + 1) + 0.5, np.arange(by0, by1 + 1) + 0.5)
        l0 = ((y1 - y2) * (X - x2) + (x2 - x1) * (Y - y2)) / den
        l1 = ((y2 - y0) * (X - x2) + (x0 - x2) * (Y - y2)) / den
        l2 = 1 - l0 - l1
        inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
        z = l0 * z0 + l1 * z1 + l2 * z2
        sub = zb[by0:by1 + 1, bx0:bx1 + 1]
        draw = inside & (z > sub)
        if not draw.any(): continue
        h, w = tx.shape
        u = np.floor(l0 * UV[0][0] + l1 * UV[1][0] + l2 * UV[2][0]).astype(int) % w
        v = np.floor(l0 * UV[0][1] + l1 * UV[1][1] + l2 * UV[2][1]).astype(int) % h
        idx = tx[v, u]
        draw &= alpha[idx] > 0
        sub[draw] = z[draw]
        img[by0:by1 + 1, bx0:bx1 + 1][draw] = (pal[idx][draw] * shade).astype(np.uint8)
        if flags_out is not None:
            flags_out[by0:by1 + 1, bx0:bx1 + 1][draw] = fl[v, u][draw]
    return img


def r2_palette(red):
    """Rush 2's CARPALETTE with the grey main ramp (1-31) tinted like Rush 1's red paint; the accent ramp stays grey."""
    pal = [list(c) for c in carpalette()]
    for i in range(1, 32):
        g = pal[i][0]
        pal[i] = [min(255, int(g * red[0] / 255 * 1.4)), min(255, int(g * red[1] / 255 * 1.4)),
                  min(255, int(g * red[2] / 255 * 1.4)), pal[i][3]]
    return [tuple(c) for c in pal]


def sheet(car, out, scale, views=VIEWS):
    d1, d2 = load(1, car), load(2, car)
    pal1 = [rgba16(c) for c in r1_palettes(d1)[7][1]]
    pal2 = r2_palette((200, 40, 40))
    names2 = {t['name']: t for t in parse(d2, 2)[1]}
    decal = {names2[n]['data']: colours for n, (w, h, colours, _) in cardecal.project(car).items()}         if car in cardecal.CARS else None
    rows = [textured_tris(d1, 1, pal1), textured_tris(d2, 2, pal2, decal), textured_tris(d2, 2, pal2)]
    W, H = int(BASE_W * scale), int(BASE_H * scale)
    img = Image.new('RGB', (W * len(views), H * len(rows)))
    for r, (tris, pal, alpha) in enumerate(rows):
        for c, (_, yaw, pitch) in enumerate(views):
            img.paste(Image.fromarray(render(tris, pal, alpha, yaw, pitch, scale)), (c * W, r * H))
    img.save(out)


def dilate(m, r):
    out = m.copy()
    for dy in range(-r, r + 1):
        for dx in range(-r, r + 1):
            out |= np.roll(np.roll(m, dy, 0), dx, 1)
    return out


def score(car, scale=1.5, reach=2):
    """How much of Rush 1's decal the Rush 2 car shows: per view, coverage = share of Rush 1's decal pixels with a Rush 2
    decal pixel within `reach` pixels, precision = the same the other way round. Returns the totals."""
    primary = cardecal.CARS[car][0]
    # Rush 1's decal = its primary indices (the companions, greys and blacks, also colour its trim and tyres).
    want = np.array([cardecal.in_ranges(i, primary) for i in range(256)])
    d1, d2 = load(1, car), load(2, car)
    pal1 = [rgba16(c) for c in r1_palettes(d1)[7][1]]
    pal2 = r2_palette((200, 40, 40))
    names2 = {t['name']: t for t in parse(d2, 2)[1]}
    decal = {names2[n]['data']: colours for n, (w, h, colours, _) in cardecal.project(car).items()}
    r1 = textured_tris(d1, 1, pal1, flag=lambda tx: want[tx])
    r2 = textured_tris(d2, 2, pal2, decal)
    tot = [0, 0, 0, 0]
    for name, yaw, pitch in VIEWS:
        W, H = int(BASE_W * scale), int(BASE_H * scale)
        a = np.zeros((H, W), dtype=bool); b = np.zeros((H, W), dtype=bool)
        render(*r1, yaw, pitch, scale, a); render(*r2, yaw, pitch, scale, b)
        hit_a = int((a & dilate(b, reach)).sum()); hit_b = int((b & dilate(a, reach)).sum())
        tot[0] += hit_a; tot[1] += int(a.sum()); tot[2] += hit_b; tot[3] += int(b.sum())
        print(f'  {car:7} {name:6} coverage {hit_a / max(a.sum(), 1):5.0%} of {int(a.sum()):5} px   '
              f'precision {hit_b / max(b.sum(), 1):5.0%} of {int(b.sum()):5} px')
    print(f'  {car:7} total  coverage {tot[0] / max(tot[1], 1):5.0%}   precision {tot[2] / max(tot[3], 1):5.0%}')
    return tot[0] / max(tot[1], 1), tot[2] / max(tot[3], 1)


if __name__ == '__main__':
    if sys.argv[1] == '--score':
        for car in sys.argv[2:] or cardecal.CARS:
            score(car)
        sys.exit()
    args = sys.argv[1:]
    scale = 1.0
    if '--scale' in args:
        i = args.index('--scale'); scale = float(args[i + 1]); del args[i:i + 2]
    views = VIEWS
    if '--views' in args:
        i = args.index('--views'); want = args[i + 1].split(','); del args[i:i + 2]
        views = [v for v in VIEWS if v[0] in want]
    sheet(args[0], args[1], scale, views)
    print(args[1])
