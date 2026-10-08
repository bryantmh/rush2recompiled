"""Rush 1 car decals as Rush 2 stripe tiles (reference implementation of src/car1_decals.cpp; research notes in
docs/rush1_research.md section 11).

Rush 2's cars are the Rush 1 cars re-textured: the panels keep their shape (both games use the same car-local
coordinates) but the texture layout was redone, so panels can't be paired by pixel. Instead each Rush 2 panel texel is
located on the car body (barycentric through the panel's triangles), the closest point on the Rush 1 body (same side,
similar normal, within DIST) is found and the Rush 1 texel there is sampled. A texel is part of the decal where that
Rush 1 texel is a fixed colour (not one of the ten paint ramps) of the car's decal kind and Rush 2 has paint there.
The result is one mask per Rush 2 panel texture (D0_1 .. D0_6); the C++ port must give the same masks bit for bit.

  python cardecal.py CAR [outdir]    preview PNG: Rush 2 panel | Rush 1 projected | decal mask
  python cardecal.py check           print the mask hashes (compared with the C++ by cpp_test)
"""
import hashlib, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
from cartex import *

# Cars with a decal worth keeping: kind 'chroma' = coloured fixed texels (flames), 'white' = white fixed texels.
CARS = {'CAMARO': 'chroma', 'HOTROD': 'chroma', 'TAXI': 'white', 'VWBUS': 'white'}
R1_PARTS = ('FL1', 'FR1', 'RL1', 'RR1', 'TOP1', 'WIN1')   # Rush 1 D0 body panels (+ greenhouse, own texels)
R2_PARTS = ('FL1', 'FR1', 'RL1', 'RR1', 'TOP1')
DIST = 3.0          # farthest a Rush 2 texel's body point may be from the Rush 1 body
MIN_SPECK = 6       # decal components smaller than this (8-connected) are dropped


def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def unit(v):
    n = math.sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2])
    return None if n < 1e-9 else (v[0] / n, v[1] / n, v[2] / n)


def closest_bary(p, a, b, c):
    """Barycentric coordinates of the point of triangle abc closest to p (Ericson, Real-Time Collision Detection)."""
    ab, ac, ap = sub(b, a), sub(c, a), sub(p, a)
    d1, d2 = dot(ab, ap), dot(ac, ap)
    if d1 <= 0 and d2 <= 0: return (1.0, 0.0, 0.0)
    bp = sub(p, b); d3, d4 = dot(ab, bp), dot(ac, bp)
    if d3 >= 0 and d4 <= d3: return (0.0, 1.0, 0.0)
    vc = d1 * d4 - d3 * d2
    if vc <= 0 and d1 >= 0 and d3 <= 0:
        v = d1 / (d1 - d3); return (1 - v, v, 0.0)
    cp = sub(p, c); d5, d6 = dot(ab, cp), dot(ac, cp)
    if d6 >= 0 and d5 <= d6: return (0.0, 0.0, 1.0)
    vb = d5 * d2 - d1 * d6
    if vb <= 0 and d2 >= 0 and d6 <= 0:
        w = d2 / (d2 - d6); return (1 - w, 0.0, w)
    va = d3 * d6 - d5 * d4
    if va <= 0 and (d4 - d3) >= 0 and (d5 - d6) >= 0:
        w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); return (0.0, 1 - w, w)
    den = 1.0 / (va + vb + vc); v = vb * den; w = vc * den
    return (1 - v - w, v, w)


def luma(c): return 0.3 * c[0] + 0.59 * c[1] + 0.11 * c[2]


def drop_specks(mask, w, h):
    seen = [False] * (w * h); out = [0] * (w * h)
    for s in range(w * h):
        if mask[s] and not seen[s]:
            comp = [s]; seen[s] = True; k = 0
            while k < len(comp):
                cy, cx = divmod(comp[k], w); k += 1
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        ny, nx = cy + dy, cx + dx
                        if 0 <= ny < h and 0 <= nx < w and mask[ny * w + nx] and not seen[ny * w + nx]:
                            seen[ny * w + nx] = True; comp.append(ny * w + nx)
            if len(comp) >= MIN_SPECK:
                for q in comp: out[q] = 1
    return out


def project(car, want_projection=False):
    """-> {panel name: (w, h, mask[w*h] of 0/1, projected R1 indices or None)} for the car's Rush 2 D0 panels."""
    kind = CARS[car]
    d1, d2 = load(1, car), load(2, car)
    pal1 = [rgba16(c) for c in r1_palettes(d1)[7][1]]       # the RED palette; fixed colours are the same in all ten
    pal2 = carpalette()
    m1, m2 = meshes(d1, 1), meshes(d2, 2)
    pre1 = next(n for n in m1 if n.endswith('D0_FL1'))[:-6]  # model prefix differs per game (BUGATTI / BUGAT)
    pre2 = next(n for n in m2 if n.endswith('D0_FL1'))[:-6]
    r1 = []
    for part in R1_PARTS:
        for vs, tex in m1.get(f'{pre1}D0_{part}', []):
            if tex is None: continue
            P = [(v[0], v[1], v[2]) for v in vs]; n = unit(cross(sub(P[1], P[0]), sub(P[2], P[0])))
            if n is None: continue
            r1.append((P, [(v[3], v[4]) for v in vs], tex, n, (P[0][0] + P[1][0] + P[2][0]) / 3.0))
    names2 = {t['name']: t for t in parse(d2, 2)[1]}
    by_off = {t['data']: t for t in names2.values()}
    panels = {}
    for part in R2_PARTS:
        for vs, tex in m2.get(f'{pre2}D0_{part}', []):
            t = by_off.get(tex[0]) if tex else None
            if t is not None: panels.setdefault(t['name'], []).append(vs)
    out = {}
    for name, tris in sorted(panels.items()):
        t = names2[name]; w, h = t['w'], t['h']; base = t['data']
        proj = [-1] * (w * h); mask = [0] * (w * h)
        for vs in tris:
            P = [(v[0], v[1], v[2]) for v in vs]; UV = [(v[3], v[4]) for v in vs]
            n2 = unit(cross(sub(P[1], P[0]), sub(P[2], P[0])))
            if n2 is None: continue
            a = (UV[0][0] - UV[2][0], UV[1][0] - UV[2][0]); b = (UV[0][1] - UV[2][1], UV[1][1] - UV[2][1])
            det = a[0] * b[1] - a[1] * b[0]
            if abs(det) < 1e-9: continue
            x0, y0 = math.floor(min(u[0] for u in UV)), math.floor(min(u[1] for u in UV))
            x1, y1 = math.ceil(max(u[0] for u in UV)), math.ceil(max(u[1] for u in UV))
            for j in range(max(y0, 0), min(y1, h - 1) + 1):
                for i in range(max(x0, 0), min(x1, w - 1) + 1):
                    rx, ry = i - UV[2][0], j - UV[2][1]
                    l0 = (rx * b[1] - ry * a[1]) / det; l1 = (a[0] * ry - b[0] * rx) / det
                    l2 = 1 - l0 - l1
                    if min(l0, l1, l2) < -0.02: continue
                    p = tuple(l0 * P[0][k] + l1 * P[1][k] + l2 * P[2][k] for k in range(3))
                    best = None
                    for k, (Pk, UVk, tex1, nk, cx) in enumerate(r1):
                        if abs(dot(nk, n2)) < 0.5: continue
                        if (cx < 0) != (p[0] < 0) and abs(p[0]) > 3: continue
                        bc = closest_bary(p, *Pk)
                        q = tuple(bc[0] * Pk[0][c] + bc[1] * Pk[1][c] + bc[2] * Pk[2][c] for c in range(3))
                        dd = sub(q, p); dist = math.sqrt(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2])
                        if best is None or dist < best[0]: best = (dist, k, bc)
                    if best is None or best[0] > DIST: continue
                    _, k, bc = best
                    Pk, UVk, tex1, nk, cx = r1[k]
                    u = math.floor((bc[0] * UVk[0][0] + bc[1] * UVk[1][0] + bc[2] * UVk[2][0]) - tex1[3] + 0.5)
                    v = math.floor((bc[0] * UVk[0][1] + bc[1] * UVk[1][1] + bc[2] * UVk[2][1]) - tex1[4] + 0.5)
                    u = min(max(u, 0), tex1[1] - 1); v = min(max(v, 0), tex1[2] - 1)
                    idx = d1[tex1[0] + v * tex1[1] + u]
                    s = j * w + i
                    proj[s] = idx
                    c1 = pal1[idx]; c2 = pal2[d2[base + s]]
                    l1c, l2c = luma(c1), luma(c2)
                    # chroma: g >= 16 leaves out the fixed dark reds (tail lights, shadows)
                    decal = (kind == 'chroma' and max(c1[:3]) - min(c1[:3]) > 50 and c1[1] >= 16) or \
                            (kind == 'white' and l1c > 190 and l1c - l2c > 40)
                    mask[s] = 1 if (decal and idx != 0 and idx not in R1_BODY and d2[base + s] in R2_BODY) else 0   # idx 0 = transparent
        out[name] = (w, h, drop_specks(mask, w, h), proj if want_projection else None)
    return out


def lod_tile(mask, w, h):
    """The _4 mip: a quarter size in each direction, a texel set where at least half of its 4x4 block is."""
    return [1 if sum(mask[(y * 4 + dy) * w + x * 4 + dx] for dy in range(4) for dx in range(4)) >= 8 else 0
            for y in range(h // 4) for x in range(w // 4)]


def digest(car):
    hsh = hashlib.sha1()
    for name, (w, h, mask, _) in sorted(project(car).items()):
        hsh.update(name.encode() + bytes([w, h]) + bytes(mask) + bytes(lod_tile(mask, w, h)))
    return hsh.hexdigest()


if __name__ == '__main__':
    from PIL import Image
    if sys.argv[1] == 'check':
        for c in CARS: print(c, digest(c))
        sys.exit()
    car = sys.argv[1]; outdir = sys.argv[2] if len(sys.argv) > 2 else 'tmp/cartex'
    res = project(car, True)
    d1, d2 = load(1, car), load(2, car)
    pal1 = [rgba16(c) for c in r1_palettes(d1)[7][1]]; pal2 = carpalette()
    names2 = {t['name']: t for t in parse(d2, 2)[1]}
    S = 3; names = sorted(res)
    img = Image.new('RGBA', (3 * (64 * S + 6), max(1, len(names)) * (64 * S + 6)), (40, 40, 40, 255))
    for r, n in enumerate(names):
        w, h, mask, proj = res[n]; base = names2[n]['data']
        for c, f in enumerate((lambda s: pal2[d2[base + s]],
                               lambda s: pal1[proj[s]] if proj[s] >= 0 else (90, 0, 90, 255),
                               lambda s: (255, 255, 0, 255) if mask[s] else (30, 30, 30, 255))):
            t = Image.new('RGBA', (w, h))
            for y in range(h):
                for x in range(w): t.putpixel((x, y), f(y * w + x))
            img.paste(t.resize((w * S, h * S), Image.NEAREST), (c * (64 * S + 6), r * (64 * S + 6)))
    os.makedirs(outdir, exist_ok=True)
    img.save(os.path.join(outdir, f'{car}_decal.png')); print(names)
