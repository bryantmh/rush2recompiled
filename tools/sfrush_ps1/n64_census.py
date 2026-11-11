"""Census of N64 SF Rush geometry missing from (or added by) a converted PS1 track.

Aligns the N64 course dump (build/asset_dump/models/sfrush/tracks/<Name>/_placed.glb) onto a PS1 conversion
(track.glb from the Games tab build or tools/sfrush_ps1/cpp_test/build.bat) with a trimmed similarity ICP over
surface samples, then reports:
  missing  - N64 surface with no PS1 surface within --gap m (clustered, N64 node names, size, height)
  ps1-only - PS1 surface with no N64 surface within --gap m (per PS1 chunk)
  overlap  - PS1 chunk pairs that cover the same place (LOD / duplicate candidates)
and writes a top-down picture <out>/<track>_census.png (gray PS1, red missing N64, blue PS1-only, labels = cluster).

  python tools/sfrush_ps1/n64_census.py OUT/sfrush_ps1/tracks/embarcadero_snow/track.glb Embarcadero --out tmp/census
"""
import argparse, json, os, sys
import numpy as np
from scipy.spatial import cKDTree
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import glb_view as G

def tris(path, keep):
    g, b = G.load(path)
    out = {}
    def walk(ni, M):
        n = g['nodes'][ni]
        L = np.array(n['matrix']).reshape(4, 4).T if 'matrix' in n else np.eye(4)
        if 'translation' in n: L = L.copy(); L[:3, 3] += n['translation']
        M = M @ L
        name = n.get('name', '')
        if 'mesh' in n and keep(name):
            for p in g['meshes'][n['mesh']]['primitives']:
                P = G.accessor(g, b, p['attributes']['POSITION']); P = (np.c_[P, np.ones(len(P))] @ M.T)[:, :3]
                I = G.accessor(g, b, p['indices']).reshape(-1, 3).astype(int) if 'indices' in p else np.arange(len(P)).reshape(-1, 3)
                out.setdefault(name, []).append(P[I])
        for c in n.get('children', []): walk(c, M)
    for ni in g['scenes'][0]['nodes']: walk(ni, np.eye(4))
    return {k: np.concatenate(v) for k, v in out.items()}

def sample(T, step, rng):
    """Area-weighted surface points (about one per step^2), with the owning triangle index."""
    a = 0.5 * np.linalg.norm(np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]), axis=1)
    n = np.maximum(1, np.round(a / step ** 2)).astype(int)
    idx = np.repeat(np.arange(len(T)), n)
    r1, r2 = rng.random(len(idx)), rng.random(len(idx))
    s = np.sqrt(r1)
    w = np.c_[1 - s, s * (1 - r2), s * r2]
    return (T[idx] * w[:, :, None]).sum(1), idx

def fit2d(B, A, tree):
    """Similarity (mirror, R, s, t) taking B (N64 xz, meters) onto A (PS1 xz) by brute force: occupancy grids
    cross-correlated (FFT) for each mirror and quarter turn at 8 m, then rotation (0.25 deg) and scale (0.25 %) swept
    around the winner at 2 m. ICP is no use here: the N64 dump has far scenery the PS1 lacks."""
    from numpy.fft import rfft2, irfft2
    def corr(Y, cell):
        lo = A.min(0)
        n = int(2 ** np.ceil(np.log2(max(np.ptp(A, 0).max(), np.ptp(Y, 0).max()) / cell * 2)))
        def grid(P, o):
            G_ = np.zeros((n, n)); q = np.floor((P - o) / cell).astype(int)
            ok = (q >= 0).all(1) & (q < n).all(1); np.add.at(G_, (q[ok, 0], q[ok, 1]), 1)
            return np.minimum(G_, 1)
        ga, gb = grid(A, lo), grid(Y, Y.min(0))
        c = irfft2(rfft2(ga) * np.conj(rfft2(gb)), (n, n)); k = np.unravel_index(c.argmax(), (n, n))
        sh = np.array([k[0] if k[0] < n // 2 else k[0] - n, k[1] if k[1] < n // 2 else k[1] - n])
        return c.max(), lo - Y.min(0) + sh * cell
    rot = lambda d: np.array([[np.cos(np.radians(d)), -np.sin(np.radians(d))], [np.sin(np.radians(d)), np.cos(np.radians(d))]])
    best = None
    for mir in (1, -1):
        for ang in (0, 90, 180, 270):
            sc_, t = corr((B * np.array([mir, 1])) @ rot(ang).T, 8.0)
            if best is None or sc_ > best[0]: best = (sc_, mir, ang)
    _, mir, ang0 = best; Bm = B * np.array([mir, 1]); best = None
    for ang in ang0 + np.arange(-1, 1.01, 0.25):
        for s in (1.0,):
            sc_, t = corr(s * (Bm @ rot(ang).T), 2.0)
            if best is None or sc_ > best[0]: best = (sc_, ang, s, t)
    _, ang, s, t = best; R = rot(ang)
    e = np.median(tree.query(s * (Bm @ R.T) + t)[0])
    return e, mir, R, s, t

def clusters(P, cell):
    """Connected components of points on a cell grid (8-neighbor, any height)."""
    key = {}
    for j, q in enumerate(np.floor(P[:, [0, 2]] / cell).astype(int)): key.setdefault(tuple(q), []).append(j)
    lab, out = {}, []
    for k in key:
        if k in lab: continue
        stack, comp = [k], []; lab[k] = len(out)
        while stack:
            c = stack.pop(); comp += key[c]
            for dx in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    m = (c[0] + dx, c[1] + dz)
                    if m in key and m not in lab: lab[m] = len(out); stack.append(m)
        out.append(np.array(comp))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('ps1'); ap.add_argument('n64', help='N64 dump course folder name, e.g. Golden_Gate')
    ap.add_argument('--out', default='tmp/census'); ap.add_argument('--gap', type=float, default=6.0)
    ap.add_argument('--min-area', type=float, default=150.0, help='m^2 of missing surface a node needs to be listed')
    ap.add_argument('--zoom', type=float, nargs=4, action='append', metavar=('X0', 'Z0', 'X1', 'Z1'),
                    help='also draw N64 | PS1 side by side over this glb x/z box (filled, shaded by height); repeatable')
    ap.add_argument('--lod', type=float, default=4.0, help='m within which a PS1 chunk counts as covering another')
    a = ap.parse_args()
    rng = np.random.default_rng(1)
    ps = tris(a.ps1, lambda n: n.startswith('PS1_') or n == 'FINISH_BANNER')
    nn = tris(f'build/asset_dump/models/sfrush/tracks/{a.n64}/_placed.glb', lambda n: n.startswith('TRACK') or n.endswith('FINISH'))
    pnames = list(ps); PT = np.concatenate([ps[k] for k in pnames]); powner = np.repeat(np.arange(len(pnames)), [len(ps[k]) for k in pnames])
    nnames = list(nn); NT = np.concatenate([nn[k] for k in nnames]); nowner = np.repeat(np.arange(len(nnames)), [len(nn[k]) for k in nnames])
    # N64 units: scale guess so both extents match before sampling
    k0 = 1 / 3.2808; NT = NT * k0          # dump is in feet
    PP, pi = sample(PT, 2.0, rng); NP, ni = sample(NT, 2.0, rng)
    ptree = cKDTree(PP[:, [0, 2]])
    sub = NP[rng.choice(len(NP), min(len(NP), 6000), replace=False)][:, [0, 2]]
    e, mir, R, s, t = fit2d(sub, PP[:, [0, 2]], cKDTree(PP[rng.choice(len(PP), min(len(PP), 20000), replace=False)][:, [0, 2]]))
    xz = s * ((NP[:, [0, 2]] * np.array([mir, 1])) @ R.T) + t
    # height: scale like xz, offset = median of nearest-in-plan differences on matching points
    y = NP[:, 1] * s
    d, i = ptree.query(xz)
    m = d < 2
    dy = PP[i[m], 1] - y[m]; y += np.median(dy)
    N3 = np.c_[xz[:, 0], y, xz[:, 1]]
    print(f'{a.n64}: fit median {e:.2f} m  mirror {mir}  scale {s * k0:.4f}  rot {np.degrees(np.arctan2(R[1, 0], R[0, 0])):.1f}  dy {np.median(dy):.1f}')
    T3 = cKDTree(PP); dn = T3.query(N3)[0]
    miss = dn > a.gap
    route = tris(a.ps1, lambda n: n == 'AIPATH_main')['AIPATH_main'].reshape(-1, 3)
    rtree = cKDTree(route[:, [0, 2]])
    report = {'track': a.ps1, 'n64': a.n64, 'fit': {'median_m': float(e), 'mirror': int(mir), 'scale': float(s * k0),
              'R': R.tolist(), 't': t.tolist(), 'dy': float(np.median(dy))}, 'n64_nodes': [], 'ps1_only': [], 'overlap': []}
    # per N64 node: how much of it the PS1 lacks, where, how far from the route
    for k, nm in enumerate(nnames):
        sel = nowner[ni] == k
        if not sel.any(): continue
        q = N3[sel]; mf = miss[sel].mean(); area = sel.sum() * 4.0
        if area * mf < a.min_area: continue
        mq = q[miss[sel]]
        report['n64_nodes'].append({'node': nm, 'missing_frac': round(float(mf), 2), 'missing_m2': round(area * mf),
            'route_m': round(float(rtree.query(mq[:, [0, 2]])[0].min())), 'y': [round(float(mq[:, 1].min())), round(float(mq[:, 1].max()))],
            'min': np.round(mq.min(0)).tolist(), 'max': np.round(mq.max(0)).tolist()})
    report['n64_nodes'].sort(key=lambda r: (r['route_m'] > 40, -r['missing_m2']))
    # missing FLAT surfaces (decks, ground, ramps) within 80 m of the route, clustered, by height against the road:
    # 'level' = drivable-height ground (shortcuts, alleys), 'above' = decks/roofs, 'below' = underpasses/water
    NA_ = NT[ni]; nrm = np.cross(NA_[:, 1] - NA_[:, 0], NA_[:, 2] - NA_[:, 0])
    up = np.abs(nrm[:, 1]) / (np.linalg.norm(nrm, axis=1) + 1e-9) > 0.7
    rd, ri = rtree.query(N3[:, [0, 2]]); dh = N3[:, 1] - route[ri, 1]
    report['flat'] = []
    for cls, sel in (('level', np.abs(dh) <= 6), ('above', dh > 6), ('below', dh < -6)):
        m_ = miss & up & (rd < 80) & sel
        P_ = N3[m_]; O_ = nowner[ni[m_]]; D_ = rd[m_]; H_ = dh[m_]
        for c in clusters(P_, 4.0):
            area = len(c) * 4.0
            if area < 300: continue
            names = {}
            for o in O_[c]: names[nnames[o]] = names.get(nnames[o], 0) + 1
            report['flat'].append({'class': cls, 'area_m2': round(area), 'route_m': round(float(D_[c].min())),
                'dh': [round(float(H_[c].min())), round(float(H_[c].max()))], 'min': np.round(P_[c].min(0)).tolist(),
                'max': np.round(P_[c].max(0)).tolist(), 'n64_nodes': sorted(names, key=names.get, reverse=True)[:4]})
    report['flat'].sort(key=lambda r: -r['area_m2'])
    # PS1-only per chunk
    ntree = cKDTree(N3); dp = ntree.query(PP)[0]
    for k, nm in enumerate(pnames):
        sel = powner[pi] == k
        if not sel.any(): continue
        f = (dp[sel] > a.gap).mean(); area = sel.sum() * 4.0
        if f > 0.5 and area * f >= a.min_area / 3:
            q = PP[sel]
            report['ps1_only'].append({'chunk': nm, 'frac': round(float(f), 2), 'area_m2': round(area), 'min': np.round(q.min(0)).tolist(), 'max': np.round(q.max(0)).tolist()})
    # overlapping PS1 chunks (LOD / duplicate candidates): fraction of A's samples within --lod m of B's surface
    trees = {k: cKDTree(PP[powner[pi] == k]) for k in range(len(pnames)) if (powner[pi] == k).sum() > 20}
    box = {k: (t_.data.min(0) - 2, t_.data.max(0) + 2) for k, t_ in trees.items()}
    for ka, ta in trees.items():
        A_ = ta.data
        for kb, tb in trees.items():
            if kb == ka or (box[ka][1] < box[kb][0]).any() or (box[kb][1] < box[ka][0]).any(): continue
            f = (tb.query(A_, distance_upper_bound=a.lod)[0] < a.lod).mean()
            if f > 0.5 and len(A_) >= 30:
                report['overlap'].append({'chunk': pnames[ka], 'inside': pnames[kb], 'frac': round(float(f), 2),
                                          'tris': len(ps[pnames[ka]]), 'other_tris': len(ps[pnames[kb]])})
    os.makedirs(a.out, exist_ok=True)
    tag = os.path.basename(os.path.dirname(a.ps1))
    json.dump(report, open(os.path.join(a.out, tag + '_census.json'), 'w'), indent=1)
    # picture: gray PS1, blue PS1-only, red N64 the PS1 lacks (brighter = higher), green route, labels = n64_nodes index
    from PIL import Image, ImageDraw
    lo = PP[:, [0, 2]].min(0) - 20; hi = PP[:, [0, 2]].max(0) + 20; S = 2400 / (hi - lo).max()
    im = Image.new('RGB', tuple(((hi - lo) * S).astype(int) + 1), (20, 20, 20)); dr = ImageDraw.Draw(im)
    def px(v): return ((v[0] - lo[0]) * S, (v[1] - lo[1]) * S)
    for q in PP: dr.point(px(q[[0, 2]]), (95, 95, 95))
    for q in PP[dp > a.gap]: dr.point(px(q[[0, 2]]), (70, 120, 255))
    MP = N3[miss]; y0 = np.median(PP[:, 1])
    for q in MP[np.argsort(MP[:, 1])]:
        h = min(1, max(0, (q[1] - y0) / 40)); dr.point(px(q[[0, 2]]), (int(140 + 115 * h), int(40 + 120 * h), 40))
    for q in route[::3]: dr.point(px(q[[0, 2]]), (60, 230, 60))
    for j, r in enumerate(report['n64_nodes']):
        if r['route_m'] <= 40: dr.text(px(((np.array(r['min']) + r['max']) / 2)[[0, 2]]), str(j), fill=(255, 255, 255))
    im.save(os.path.join(a.out, tag + '_census.png'))
    # zoomed side-by-side: N64 aligned (left) and PS1 (right), triangles filled by height, route green
    NT3 = NT.reshape(-1, 3); xz3 = s * ((NT3[:, [0, 2]] * np.array([mir, 1])) @ R.T) + t
    NA = np.c_[xz3[:, 0], NT3[:, 1] * s + np.median(dy), xz3[:, 1]].reshape(-1, 3, 3)
    for zi, (x0, z0, x1, z1) in enumerate(a.zoom or []):
        W = 900; S2 = W / (x1 - x0); H = int((z1 - z0) * S2)
        im = Image.new('RGB', (2 * W + 10, H), (15, 15, 15)); dr = ImageDraw.Draw(im)
        for side, TT, own in ((0, NA, nowner), (1, PT, powner)):
            c = TT.mean(1); k = (c[:, 0] > x0 - 50) & (c[:, 0] < x1 + 50) & (c[:, 2] > z0 - 50) & (c[:, 2] < z1 + 50)
            for tr in TT[k][np.argsort(TT[k][:, :, 1].max(1))]:
                yy = tr[:, 1].mean(); h = min(1, max(0, (yy + 5) / 60))
                col = (int(40 + 215 * h), int(40 + 150 * (1 - abs(h - .5) * 2)), int(220 * (1 - h)))
                dr.polygon([(side * (W + 10) + (v[0] - x0) * S2, (v[2] - z0) * S2) for v in tr], fill=col, outline=(0, 0, 0))
            for q in route[(route[:, 0] > x0) & (route[:, 0] < x1) & (route[:, 2] > z0) & (route[:, 2] < z1)][::2]:
                dr.point((side * (W + 10) + (q[0] - x0) * S2, (q[2] - z0) * S2), (60, 255, 60))
        for gx in range(int(x0 // 50 + 1) * 50, int(x1), 50):
            dr.text(((gx - x0) * S2, 2), str(gx), fill=(255, 255, 255)); dr.text((W + 10 + (gx - x0) * S2, 2), str(gx), fill=(255, 255, 255))
        for gz in range(int(z0 // 50 + 1) * 50, int(z1), 50):
            dr.text((2, (gz - z0) * S2), str(gz), fill=(255, 255, 255)); dr.text((W + 12, (gz - z0) * S2), str(gz), fill=(255, 255, 255))
        im.save(os.path.join(a.out, f'{tag}_zoom{zi}.png'))
    for j, r in enumerate(report['n64_nodes']): print(j, r)
    for r in report['flat']: print('flat', r)
    for r in report['ps1_only']: print('ps1-only', r)
    for r in report['overlap']: print('overlap', r)

if __name__ == '__main__':
    main()
