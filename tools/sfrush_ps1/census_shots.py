"""Matching close-up renders of the N64 and PS1 versions of a spot, for the n64_census.py findings.

Reads the fit from n64_census.py's <tag>_census.json, moves the N64 dump (_placed.glb, feet) into the PS1 conversion's
frame and draws both with glb_view's perspective renderer from the same cameras: a driver's view from the route point
nearest --dist m from the target, and an aerial view from the same side. --hl tints PS1 nodes (a candidate piece).

  python tools/sfrush_ps1/census_shots.py tmp/census/golden_gate_snow_census.json OUT.png --at 740 0 0 --dist 250 \
      --hl PS1_SCENERY_90 --title "id 91 bridge card"
"""
import argparse, json, os, sys
import numpy as np
from PIL import Image, ImageDraw
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import glb_view as G


def load(path, keep=()):
    g, b = G.load(path)
    return G.triangles(g, b, list(keep), 0), G.textures(g, b), g, b


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('census'); ap.add_argument('out')
    ap.add_argument('--at', type=float, nargs=3, required=True, help='target, PS1 glb meters')
    ap.add_argument('--dist', type=float, default=60, help='driver view: route point about this far from the target')
    ap.add_argument('--eye', type=float, nargs=3, help='driver eye instead of the route pick')
    ap.add_argument('--aerial', type=float, default=1.0, help='aerial view distance factor (0 = none)')
    ap.add_argument('--hl', nargs='*', default=[], help='PS1 node name prefixes to tint red')
    ap.add_argument('--title', default='')
    ap.add_argument('--width', type=int, default=640); ap.add_argument('--height', type=int, default=360)
    a = ap.parse_args()
    rep = json.load(open(a.census))
    f = rep['fit']; R = np.array(f['R']); t = np.array(f['t'])
    ps1 = rep['track']; n64 = f'build/asset_dump/models/sfrush/tracks/{rep["n64"]}/_placed.glb'
    keep_n = lambda n: n.startswith('TRACK') or n.endswith('FINISH')
    gn, bn = G.load(n64)
    names = [n.get('name', '') for n in gn['nodes'] if 'mesh' in n]
    N = G.triangles(gn, bn, sorted({n for n in names if keep_n(n)}), 0); Ntex = G.textures(gn, bn)
    P = N[0].reshape(-1, 3)
    xz = f['scale'] * ((P[:, [0, 2]] * np.array([f['mirror'], 1])) @ R.T) + t
    N = (np.c_[xz[:, 0], P[:, 1] * f['scale'] + f['dy'], xz[:, 1]].reshape(-1, 3, 3),) + tuple(N[1:])
    gp, bp = G.load(ps1)
    Pt = G.triangles(gp, bp, ['PS1_', 'FINISH_BANNER'], 0); Ptex = G.textures(gp, bp)
    if a.hl:
        H = G.triangles(gp, bp, a.hl, 0)
        if H is not None:
            Hc = H[2].copy(); Hc[..., 0] = 1.0; Hc[..., 1] *= 0.45; Hc[..., 2] *= 0.45
            H = (H[0], H[1], Hc, H[3])
            Pt = tuple(np.concatenate([y, x]) for x, y in zip(Pt, H))   # first drawn wins a depth tie
    route = G.triangles(gp, bp, ['AIPATH_main'], 0)[0].reshape(-1, 3)
    at = np.array(a.at)
    if a.eye: eye = np.array(a.eye)
    else:
        d = np.linalg.norm(route - at, axis=1); eye = route[np.argmin(np.abs(d - a.dist))] + [0, 3, 0]
    views = [('driver', eye, at)]
    if a.aerial:
        h = eye - at; h[1] = 0; h = h / (np.linalg.norm(h) + 1e-9) * max(a.dist, 60) * a.aerial
        views.append(('aerial', at + h + [0, max(a.dist, 60) * a.aerial, 0], at))
    W, Hh = a.width, a.height
    out = Image.new('RGB', (2 * W + 6, len(views) * (Hh + 6) + 24), (0, 0, 0)); dr = ImageDraw.Draw(out)
    dr.text((4, 4), f'{a.title}   left N64, right PS1 (red = candidate)   target {np.round(at).tolist()}', fill=(255, 255, 0))
    for r, (nm, e, c) in enumerate(views):
        for k, (tris, tex) in enumerate(((N, Ntex), (Pt, Ptex))):
            im = G.render_eye(tris, tex, e, c, 70, W, Hh, (110, 150, 200))
            out.paste(im, (k * (W + 6), 24 + r * (Hh + 6)))
        dr.text((6, 28 + r * (Hh + 6)), f'{nm} eye {np.round(e).tolist()}', fill=(255, 255, 255))
    out.save(a.out); print(a.out)


if __name__ == '__main__':
    main()
