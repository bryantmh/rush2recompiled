#!/usr/bin/env python3
"""Fits a track select diorama onto its track: the similarity transform (scale, turn, mirror, offset) that lays the
diorama's red route line over the track's AI path, seen from above. Fill Scenery Gaps reads the diorama through it
(src/scenery_fill_guide.cpp, docs/graphics_effects.md#diorama-guides), so the table there is this tool's output.

    python tools/diorama_fit.py DIORAMA.glb ROUTE OUT.png [--track FILL.glb] [--init S DEG MIRROR TX TZ]

DIORAMA.glb is a Dump Models diorama (build/asset_dump/models/<game>/menus/track_select/<MODEL>.glb), in its own
units. ROUTE is the track's AI path in the fill builder's frame (feet; x is the game's physics x negated):
- r2:SLOT        a Rush 2 track's path file from the ROM (RUSH2_ROM; slot 0-10 as in tools/car_probes_test.cpp)
- path:FILE      a Rush 2 format path file (the Rush 1 conversions: tools/rush1/cpp_test/out/ref/path0N.bin)
- glb:TRACK.glb  a custom track's AIPATH_main (glTF meters, x as the builder's)
Prints the transform as the table wants it: builder (x, z) = S * R(DEG) * (MIRROR ? (-dx, dz) : (dx, dz)) + (TX, TZ),
with R turning +x toward +z, and the fit's mean distance both ways (feet). OUT.png draws the track's fill glb (--track,
tools/car_probes_test.cpp --fill output) in grey, the diorama's triangles transformed in their kind's color and the
path in black, to check it. --init skips the search and refines from the given transform.
Needs numpy, scipy and Pillow.
"""
import argparse, math, os, struct, sys
import numpy as np
from PIL import Image, ImageDraw
from scipy.spatial import cKDTree

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
import glb_view  # noqa: E402

FT = 3.2808399


def diorama(path):
    """World triangles [n,3,3] and each one's color: its texture's mean times its vertex colors' mean (0-255)."""
    gltf, bin_ = glb_view.load(path)
    texs = glb_view.textures(gltf, bin_)
    means = [t[0][:, :, :3].reshape(-1, 3).mean(0) for t in texs]
    tris, cols = [], []

    def walk(ni, parent):
        node = gltf['nodes'][ni]
        m = parent @ glb_view.node_matrix(node)
        if 'mesh' in node:
            for prim in gltf['meshes'][node['mesh']]['primitives']:
                pos = glb_view.accessor(gltf, bin_, prim['attributes']['POSITION'])
                idx = (glb_view.accessor(gltf, bin_, prim['indices']).astype(np.int64).ravel() if 'indices' in prim
                       else np.arange(len(pos)))
                idx = idx[:len(idx) // 3 * 3]
                p = (np.c_[pos, np.ones(len(pos))] @ m.T)[:, :3]
                t = p[idx].reshape(-1, 3, 3)
                c = np.ones((len(t), 3))
                if 'COLOR_0' in prim['attributes']:
                    vc = glb_view.accessor(gltf, bin_, prim['attributes']['COLOR_0'])[:, :3]
                    if vc.max() > 1.5:
                        vc = vc / 255.0
                    c = vc[idx].reshape(-1, 3, 3).mean(1)
                base = np.full(3, 1.0)
                if 'material' in prim:
                    mat = gltf['materials'][prim['material']]
                    pbr = mat.get('pbrMetallicRoughness', {})
                    ti = pbr.get('baseColorTexture', {}).get('index')
                    if ti is not None:
                        base = means[ti]
                    else:
                        base = np.array(pbr.get('baseColorFactor', [1, 1, 1, 1])[:3])
                tris.append(t)
                cols.append(c * base * 255.0)
        for ch in node.get('children', []):
            walk(ch, m)

    for ni in gltf['scenes'][gltf.get('scene', 0)]['nodes']:
        walk(ni, np.eye(4))
    return np.concatenate(tris), np.concatenate(cols)


def is_route(cols):
    r, g, b = cols[:, 0], cols[:, 1], cols[:, 2]
    return (r > 90) & (r > 1.8 * g) & (r > 1.8 * b)


def upward(tris):
    e1 = tris[:, 1] - tris[:, 0]
    e2 = tris[:, 2] - tris[:, 0]
    n = np.cross(e1, e2)
    ln = np.linalg.norm(n, axis=1) + 1e-12
    return np.abs(n[:, 1]) / ln > 0.5


def sample_tris(tris, step):
    """Points spread over the triangles (x, z), about one per step^2 of area."""
    out = []
    rng = np.random.default_rng(1)
    for t in tris:
        a = t[0, [0, 2]]; b = t[1, [0, 2]]; c = t[2, [0, 2]]
        area = abs((b[0] - a[0]) * (c[1] - a[1]) - (c[0] - a[0]) * (b[1] - a[1])) / 2
        n = max(1, int(area / (step * step)))
        u = rng.random((n, 2))
        f = u.sum(1) > 1
        u[f] = 1 - u[f]
        out.append(a + u[:, :1] * (b - a) + u[:, 1:] * (c - a))
    return np.concatenate(out)


def route(spec):
    kind, _, arg = spec.partition(':')
    if kind in ('r2', 'path'):
        if kind == 'r2':
            import roms
            data = roms.Rush2().asset(0x57 + int(arg))
        else:
            data = open(arg, 'rb').read()
        import paths
        p = paths.parse(data)
        pts = [(-x, z) for x, y, z in p['spine']]
        for b in p['branches']:
            pts += [(-x, z) for x, y, z in b['points']]
        return np.array(pts, float), [len(p['spine'])]
    if kind == 'glb':
        gltf, bin_ = glb_view.load(arg)
        for node in gltf['nodes']:
            if node.get('name') == 'AIPATH_main' and 'mesh' in node:
                prim = gltf['meshes'][node['mesh']]['primitives'][0]
                pos = glb_view.accessor(gltf, bin_, prim['attributes']['POSITION'])
                m = glb_view.node_matrix(node)
                p = (np.c_[pos, np.ones(len(pos))] @ m.T)[:, :3] * FT
                return p[:, [0, 2]], [len(p)]
        sys.exit('no AIPATH_main in ' + arg)
    sys.exit('route: r2:SLOT, path:FILE or glb:FILE')


def densify(p, step):
    out = [p[0]]
    for a, b in zip(p[:-1], p[1:]):
        d = np.linalg.norm(b - a)
        if d > 3 * step * 20:     # a jump (branch start): no segment
            out.append(b)
            continue
        n = max(1, int(d / step))
        for k in range(1, n + 1):
            out.append(a + (b - a) * k / n)
    return np.array(out)


def apply(T, pts):
    s, deg, mirror, tx, tz = T
    q = pts.copy()
    if mirror:
        q[:, 0] = -q[:, 0]
    c, si = math.cos(math.radians(deg)), math.sin(math.radians(deg))
    return np.c_[s * (c * q[:, 0] - si * q[:, 1]) + tx, s * (si * q[:, 0] + c * q[:, 1]) + tz]


def score(T, dio, tree_route, route_pts):
    a = apply(T, dio)
    d1 = tree_route.query(a)[0]
    d2 = cKDTree(a).query(route_pts)[0]
    return np.mean(np.minimum(d1, 2000)) + np.mean(np.minimum(d2, 2000))


def refine(T, dio, tree_route, route_pts, iters=40):
    """Similarity ICP (Umeyama on nearest pairs both ways), mirror fixed."""
    s, deg, mirror, tx, tz = T
    src = dio.copy()
    if mirror:
        src[:, 0] = -src[:, 0]
    for _ in range(iters):
        a = apply((s, deg, False, tx, tz), src)
        _, i1 = tree_route.query(a)
        _, i2 = cKDTree(a).query(route_pts)
        P = np.r_[src, src[i2]]
        Q = np.r_[route_pts[i1], route_pts]
        mp, mq = P.mean(0), Q.mean(0)
        Pc, Qc = P - mp, Q - mq
        H = Pc.T @ Qc
        U, S, Vt = np.linalg.svd(H)
        R = Vt.T @ U.T
        if np.linalg.det(R) < 0:
            Vt[1] *= -1
            R = Vt.T @ U.T
        s = S.sum() / (Pc ** 2).sum()
        deg = math.degrees(math.atan2(R[1, 0], R[0, 0]))
        t = mq - s * R @ mp
        tx, tz = t
    return (s, deg, mirror, tx, tz)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('diorama'); ap.add_argument('route'); ap.add_argument('out')
    ap.add_argument('--track')
    ap.add_argument('--init', nargs=5, type=float)
    a = ap.parse_args()

    tris, cols = diorama(a.diorama)
    red = is_route(cols) & upward(tris)
    if not red.any():
        sys.exit('no red route triangles')
    rt = tris[red]
    ext = np.ptp(rt[:, :, [0, 2]].reshape(-1, 2), axis=0).max()
    dio = sample_tris(rt, ext / 150)
    rpts, _ = route(a.route)
    rd = densify(rpts, np.ptp(rpts, axis=0).max() / 400)
    tree = cKDTree(rd)

    if a.init:
        best = refine((a.init[0], a.init[1], bool(a.init[2]), a.init[3], a.init[4]), dio, tree, rd)
    else:
        s0 = math.sqrt(rd.var(0).sum() / dio.var(0).sum())
        cands = []
        for mirror in (False, True):
            for deg in range(0, 360, 10):
                T = (s0, deg, mirror, 0, 0)
                c = apply(T, dio).mean(0)
                T = (s0, deg, mirror, rd.mean(0)[0] - c[0], rd.mean(0)[1] - c[1])
                cands.append((score(T, dio, tree, rd), T))
        cands.sort(key=lambda c: c[0])
        best, bs = None, 1e30
        for _, T in cands[:12]:
            R = refine(T, dio, tree, rd)
            sc = score(R, dio, tree, rd)
            if sc < bs:
                best, bs = R, sc
    s, deg, mirror, tx, tz = best
    sc = score(best, dio, tree, rd)
    print('S %.4f DEG %.2f MIRROR %d TX %.1f TZ %.1f   mean distance %.1f ft (diorama extent %.0f units, track %.0f ft)'
          % (s, deg % 360, mirror, tx, tz, sc / 2, ext, np.ptp(rd, axis=0).max()))

    # The check picture.
    allp = apply(best, tris[:, :, [0, 2]].reshape(-1, 2)).reshape(-1, 3, 2)
    lo = np.minimum(allp.reshape(-1, 2).min(0), rd.min(0)) - 500
    hi = np.maximum(allp.reshape(-1, 2).max(0), rd.max(0)) + 500
    W = 1400
    px = W / (hi - lo).max()
    img = Image.new('RGB', (int((hi[0] - lo[0]) * px) + 1, int((hi[1] - lo[1]) * px) + 1), (40, 40, 40))
    dr = ImageDraw.Draw(img)
    f = lambda p: ((p[0] - lo[0]) * px, (p[1] - lo[1]) * px)
    order = np.argsort(tris[:, :, 1].mean(1))
    for i in order:
        if not upward(tris[i:i + 1])[0]:
            continue
        c = tuple(int(v) for v in np.clip(cols[i], 0, 255))
        dr.polygon([f(p) for p in allp[i]], fill=c)
    if a.track:
        tr = track_points(a.track)
        for p in tr[::max(1, len(tr) // 60000)]:
            x, y = f(p)
            dr.point((x, y), fill=(255, 255, 255))
    for p, q in zip(rd[:-1], rd[1:]):
        if np.linalg.norm(q - p) < 400:
            dr.line([f(p), f(q)], fill=(0, 0, 0), width=2)
    img.save(a.out)


def track_points(path):
    """The fill glb's vertices (x, z feet) other than the underlay's."""
    gltf, bin_ = glb_view.load(path)
    out = []

    def walk(ni, parent):
        node = gltf['nodes'][ni]
        m = parent @ glb_view.node_matrix(node)
        if 'mesh' in node and node.get('name', '') != 'SCENERYFILL':
            for prim in gltf['meshes'][node['mesh']]['primitives']:
                pos = glb_view.accessor(gltf, bin_, prim['attributes']['POSITION'])
                p = (np.c_[pos, np.ones(len(pos))] @ m.T)[:, :3]
                out.append(p[:, [0, 2]])
        for ch in node.get('children', []):
            walk(ch, m)

    for ni in gltf['scenes'][gltf.get('scene', 0)]['nodes']:
        walk(ni, np.eye(4))
    return np.concatenate(out)


if __name__ == '__main__':
    main()
