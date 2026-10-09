"""Rush 1 car decals painted onto Rush 2's car panel textures (reference implementation of src/car1_decals.cpp;
research notes in docs/rush1_research.md section 11).

Rush 2's cars are the Rush 1 cars re-textured: the panels keep their shape (both games use the same car-local
coordinates) but the texture layout was redone, so panels can't be paired by pixel. Instead each Rush 2 panel texel is
located on the car body (barycentric through the panel's triangles); the Rush 1 surface facing the same way that the
line along the Rush 2 normal crosses farthest out (what is visible there; else the closest point of the Rush 1 body on
the same side, within DIST) gives the Rush 1 texel. A texel is part of the decal where that
Rush 1 texel is one of the car's decal palette indices (CARS: the Camaro's flames, the Taxi's white checks, the VW
Bus's swirls, the VW Bug's sunburst; companion indices such as the Taxi's black checks only next to those) and Rush 2
has paint there. Each texel is sampled SUB x SUB times (Rush 1 has more texels on some panels) and it is decal where any sample is (Rush 1
blends its decal edges into its paint, so a majority rule thins every shape).
The decal keeps Rush 1's colours: a texel is the nearest of Rush 2's fixed car palette entries (96-111, 144-207, the same for
every paint colour) to the mean of its decal samples. The result is one colour map per Rush 2 panel texture (D0_1 ..
D0_6, Rush 2 car palette indices, 0 = none) plus its quarter-size mip; the C++ port must give the same bytes. The rest
of the panel keeps Rush 2's paint, so MAIN and ACCENT still colour the car.

  python cardecal.py CAR [outdir]    per-panel preview PNG with a texel grid (see preview())
  python cardecal.py check           print the result hashes (compared with the C++ by cpp_test)
"""
import hashlib, math, os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
from cartex import *

# Cars with a decal of their own in Rush 1 (the others only have racing stripes, which Rush 2's STRIPE values cover):
# (Rush 1 palette index ranges of the decal, companion ranges kept only within REACH texels of a decal texel, the
# Rush 2 panels (D0_n) it goes on). Fixed Rush 1 colours: 33-49 / 100-106 the Camaro's flame ramps (yellow to red),
# 145-148 whites, 157-159 and 31 blacks; on the VW Bus and Bug 33-63 is a white ramp.
CARS = {
    'CAMARO': ([(33, 49), (100, 106)], [(147, 156), (161, 175)], (1, 2, 3, 4, 5, 6)),
    'VWBUS': ([(33, 63)], [], (1, 2, 3, 4, 5, 6)),
    'VWBUG': ([(33, 63)], [], (1, 2, 3, 4, 5, 6)),
    'TAXI': ([(145, 148)], [(31, 31), (157, 159)], (1, 2, 3, 4, 5, 6)),
}
# Hand-made cuts: {car: {panel n: [(x0, y0, x1, y1), ...]}}, texel rectangles (inclusive) of a Rush 2 panel D0_n where
# the decal is dropped (fragments where Rush 2's windows and pillars sit apart from Rush 1's). Previews with a texel grid:
# python cardecal.py CAR; the result on the car: tools/rush1/carview.py.
CUTS = {
    'CAMARO': {},
    'VWBUS': {},
    'VWBUG': {},
    'TAXI': {},
}
R1_PARTS = ('FL1', 'FR1', 'RL1', 'RR1', 'TOP1', 'WIN1')   # Rush 1 D0 body panels (+ greenhouse, own texels)
R2_PARTS = ('FL1', 'FR1', 'RL1', 'RR1', 'TOP1')
DIST = 3.0          # farthest a Rush 2 texel's body point may be from the Rush 1 body
MIN_SPECK = 6       # decal components smaller than this (8-connected) are dropped
SUB = 3             # sub-samples per texel in each direction (Rush 1 has more texels on some panels)
REACH = 2           # companion texels count within this many texels (in x and y) of a decal texel
# Rush 2 car palette entries that keep their colour for every paint choice: the game's palette class table (ranges at
# 0x800C5670, built by func_800854AC) tints 1-31 / 33-63 with MAIN / ACCENT and hands 64-95, 112-143 and 208-255 to
# func_80084EDC, which overwrites them with blends of the paint and stripe colours; only 32, 96-111 and 144-207 stay.
R2_FIXED = [*range(96, 112), *range(144, 208)]


def in_ranges(i, ranges): return any(a <= i <= b for a, b in ranges)


def nearest_fixed(c, pal2):
    """Index of the opaque fixed Rush 2 car palette entry closest to colour c (first on a tie)."""
    best = None
    for i in R2_FIXED:
        p = pal2[i]
        if p[3] == 0: continue
        d = (p[0] - c[0]) ** 2 + (p[1] - c[1]) ** 2 + (p[2] - c[2]) ** 2
        if best is None or d < best[0]: best = (d, i)
    return best[1]


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


def ray_hit(p, n, tri):
    """Where the line p + t*n crosses triangle tri (Moller-Trumbore): (t, barycentric) or (None, None)."""
    e1, e2 = sub(tri[1], tri[0]), sub(tri[2], tri[0])
    h = cross(n, e2); a = dot(e1, h)
    if abs(a) < 1e-9: return None, None
    f = 1.0 / a; sv = sub(p, tri[0])
    u = f * dot(sv, h)
    if u < 0 or u > 1: return None, None
    q = cross(sv, e1); v = f * dot(n, q)
    if v < 0 or u + v > 1: return None, None
    return f * dot(e2, q), (1 - u - v, u, v)


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
    """-> {panel name: (w, h, colours[w*h] (Rush 2 palette index, 0 = no decal), projected R1 indices or None)} for the
    car's Rush 2 D0 panels."""
    primary, companion, on = CARS[car]
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
        samples = [[-1] * (SUB * SUB) for _ in range(w * h)]    # Rush 1 index at each sub-sample, -1 = none
        decal_here = name[-1] in '123456' and int(name[-1]) in on
        for vs in (tris if decal_here else []):
            P = [(v[0], v[1], v[2]) for v in vs]; UV = [(v[3], v[4]) for v in vs]
            n2 = unit(cross(sub(P[1], P[0]), sub(P[2], P[0])))
            if n2 is None: continue
            a = (UV[0][0] - UV[2][0], UV[1][0] - UV[2][0]); b = (UV[0][1] - UV[2][1], UV[1][1] - UV[2][1])
            det = a[0] * b[1] - a[1] * b[0]
            if abs(det) < 1e-9: continue
            # Rush 1 triangles this one can match: similar normal and the same side of the car.
            cand = [k for k, r in enumerate(r1) if dot(r[3], n2) >= 0.5]
            x0, y0 = math.floor(min(u[0] for u in UV)), math.floor(min(u[1] for u in UV))
            x1, y1 = math.ceil(max(u[0] for u in UV)), math.ceil(max(u[1] for u in UV))
            for j in range(max(y0, 0), min(y1, h - 1) + 1):
                for i in range(max(x0, 0), min(x1, w - 1) + 1):
                    for q in range(SUB * SUB):
                        rx = i + (q % SUB + 0.5) / SUB - UV[2][0]; ry = j + (q // SUB + 0.5) / SUB - UV[2][1]
                        l0 = (rx * b[1] - ry * a[1]) / det; l1 = (a[0] * ry - b[0] * rx) / det
                        l2 = 1 - l0 - l1
                        if min(l0, l1, l2) < -0.02: continue
                        p = tuple(l0 * P[0][k] + l1 * P[1][k] + l2 * P[2][k] for k in range(3))
                        # The outermost Rush 1 surface crossing the line through p along the normal (what is
                        # visible there), else the closest point of the Rush 1 body.
                        hit = None
                        for k in cand:
                            t, bc = ray_hit(p, n2, r1[k][0])
                            if t is not None and abs(t) <= DIST and (hit is None or t > hit[0]): hit = (t, k, bc)
                        if hit is None:
                            for k in cand:
                                Pk, UVk, tex1, nk, cx = r1[k]
                                if (cx < 0) != (p[0] < 0) and abs(p[0]) > 3: continue
                                bc = closest_bary(p, *Pk)
                                qq = tuple(bc[0] * Pk[0][c] + bc[1] * Pk[1][c] + bc[2] * Pk[2][c] for c in range(3))
                                dd = sub(qq, p); dist = math.sqrt(dd[0] * dd[0] + dd[1] * dd[1] + dd[2] * dd[2])
                                if dist <= DIST and (hit is None or dist < hit[0]): hit = (dist, k, bc)
                        if hit is None: continue
                        _, k, bc = hit
                        Pk, UVk, tex1, nk, cx = r1[k]
                        u = math.floor((bc[0] * UVk[0][0] + bc[1] * UVk[1][0] + bc[2] * UVk[2][0]) - tex1[3])
                        v = math.floor((bc[0] * UVk[0][1] + bc[1] * UVk[1][1] + bc[2] * UVk[2][1]) - tex1[4])
                        u = min(max(u, 0), tex1[1] - 1); v = min(max(v, 0), tex1[2] - 1)
                        samples[j * w + i][q] = d1[tex1[0] + v * tex1[1] + u]
        proj = [smp[SUB * SUB // 2] for smp in samples]
        # A texel is decal where any of its sub-samples is (Rush 1 blends its decal edges into the paint, so a
        # majority rule thins every shape); companions also need a decal texel nearby.
        paint = [d2[base + s] in R2_BODY for s in range(w * h)]
        mask = [0] * (w * h); comp = [0] * (w * h)
        for s, smp in enumerate(samples):
            got = [x for x in smp if x >= 0]
            pc = sum(in_ranges(x, primary) for x in got); cc = sum(in_ranges(x, companion) for x in got)
            if decal_here and paint[s] and got:
                mask[s] = int(pc > 0)
                comp[s] = int(not mask[s] and cc > 0 and 2 * (pc + cc) >= len(got))
        near = list(mask)
        for s in range(w * h):
            if comp[s]:
                y, x = divmod(s, w)
                near[s] = int(any(mask[yy * w + xx] for yy in range(max(y - REACH, 0), min(y + REACH, h - 1) + 1)
                                  for xx in range(max(x - REACH, 0), min(x + REACH, w - 1) + 1)))
        for x0, y0, x1, y1 in CUTS[car].get(int(name[-1]) if name[-1] in '123456' else 0, []):
            for y in range(max(y0, 0), min(y1, h - 1) + 1):
                for x in range(max(x0, 0), min(x1, w - 1) + 1): near[y * w + x] = 0
        mask = drop_specks(near, w, h)
        # A decal texel's colour is the mean of its decal sub-samples.
        colours = [0] * (w * h)
        for s in range(w * h):
            if mask[s]:
                cs_ = [pal1[x] for x in samples[s] if x >= 0 and (in_ranges(x, primary) or in_ranges(x, companion))]
                colours[s] = nearest_fixed(tuple(sum(c[k] for c in cs_) // len(cs_) for k in range(3)), pal2)
        out[name] = (w, h, colours, proj if want_projection else None)
    return out


def lod_tile(colours, w, h):
    """The _4 mip: a quarter size in each direction; a texel has the decal where at least half of its 4x4 block does,
    in the block's most common decal colour (lowest index on a tie)."""
    out = []
    for y in range(h // 4):
        for x in range(w // 4):
            block = [colours[(y * 4 + dy) * w + x * 4 + dx] for dy in range(4) for dx in range(4)]
            lit = [c for c in block if c]
            out.append(0 if len(lit) < 8 else min(set(lit), key=lambda c: (-lit.count(c), c)))
    return out


def digest(car):
    hsh = hashlib.sha1()
    for name, (w, h, colours, _) in sorted(project(car).items()):
        hsh.update(name.encode() + bytes([w, h]) + bytes(colours) + bytes(lod_tile(colours, w, h)))
    return hsh.hexdigest()


def preview(car, outdir, scale=8):
    """Per-panel PNG (outdir/<CAR>_decal.png): left the Rush 2 panel as the game paints it (the decal
    on top), right the Rush 1 texels projected onto it (magenta = none), with an 8-texel grid for writing CUTS."""
    from PIL import Image, ImageDraw
    import carview
    d1, d2 = load(1, car), load(2, car)
    pal1 = [rgba16(c) for c in r1_palettes(d1)[7][1]]
    pal2 = carview.r2_palette((200, 40, 40))
    names2 = {t['name']: t for t in parse(d2, 2)[1]}
    res = project(car, True)
    S = scale; tiles = []
    for n, (w, h, colours, proj) in sorted(res.items()):
        base = names2[n]['data']
        im = Image.new('RGB', (2 * w * S + 20, h * S + 16), (40, 40, 40)); dr = ImageDraw.Draw(im)
        for y in range(h):
            for x in range(w):
                s = y * w + x; i2 = d2[base + s]
                dr.rectangle([x * S, 16 + y * S, x * S + S - 1, 16 + y * S + S - 1], fill=pal2[colours[s] or i2][:3])
                c = pal1[proj[s]][:3] if proj[s] >= 0 else (90, 0, 90)
                dr.rectangle([w * S + 20 + x * S, 16 + y * S, w * S + 20 + x * S + S - 1, 16 + y * S + S - 1], fill=c)
        for off in (0, w * S + 20):
            for x in range(0, w + 1, 8):
                dr.line([off + x * S, 16, off + x * S, 16 + h * S], fill=(0, 255, 255)); dr.text((off + x * S + 1, 2), str(x), fill=(0, 255, 255))
            for y in range(0, h + 1, 8):
                dr.line([off, 16 + y * S, off + w * S, 16 + y * S], fill=(0, 255, 255)); dr.text((off + 1, 16 + y * S + 1), str(y), fill=(0, 255, 255))
        dr.text((w * S - 70, 2), n, fill=(255, 255, 0))
        tiles.append(im)
    sheet = Image.new('RGB', (max(t.width for t in tiles), sum(t.height + 8 for t in tiles)), (25, 25, 25))
    y = 0
    for t in tiles:
        sheet.paste(t, (0, y)); y += t.height + 8
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, f'{car}_decal.png'); sheet.save(path); return path


if __name__ == '__main__':
    if sys.argv[1] == 'check':
        for c in CARS: print(c, digest(c))
        sys.exit()
    print(preview(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else 'tmp/cartex'))
