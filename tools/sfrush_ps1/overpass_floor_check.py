"""Checks the Embarcadero overpass of a converted PS1 track.glb: where a drawn road surface facing up
(PS1_N64_OVERPASS, PS1_SCENERY_43) has no collision floor (COL_*, not walls) within 1.5 m of it. Prints a top-down map
(rows z every 4 m, columns x every 2 m: 'x' drawn without a floor, '#' both, '.' nothing drawn), the count, and the drawn
and floor heights along the middle (x 1222 m). docs/sfrush_ps1.md#embarcadero-overpass

    python tools/sfrush_ps1/overpass_floor_check.py TRACK.glb
"""
import os, sys
import numpy as np
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
import glb_view as G
gltf, b = G.load(sys.argv[1])
def tris(pred):
    out = []
    for n in gltf['nodes']:
        if 'mesh' not in n or not pred(n.get('name', '')): continue
        for pr in gltf['meshes'][n['mesh']]['primitives']:
            P = G.accessor(gltf, b, pr['attributes']['POSITION'])
            I = G.accessor(gltf, b, pr['indices']).astype(int).ravel() if 'indices' in pr else np.arange(len(P))
            out.append(P[I].reshape(-1, 3, 3))
    return np.concatenate(out)
def up(T, signed):
    nr = np.cross(T[:, 1]-T[:, 0], T[:, 2]-T[:, 0]); ny = nr[:, 1] / (np.linalg.norm(nr, axis=1) + 1e-9)
    return T[(ny > .7) if signed else (np.abs(ny) > .5)]
box = lambda T: T[(T[:, :, 0].max(1) > 1200) & (T[:, :, 0].min(1) < 1248) & (T[:, :, 2].max(1) > -2000) & (T[:, :, 2].min(1) < -1280) & (T[:, :, 1].max(1) > 12)]
D = box(up(tris(lambda n: n in ('PS1_N64_OVERPASS', 'PS1_SCENERY_43')), True)); C = box(up(tris(lambda n: n.startswith('COL_') and 'WALL' not in n), False))
def heights(T, x, z):
    hs = []
    for t in T:
        if x < t[:, 0].min() or x > t[:, 0].max() or z < t[:, 2].min() or z > t[:, 2].max(): continue
        d = [(t[(i+1) % 3, 0]-t[i, 0])*(z-t[i, 2]) - (t[(i+1) % 3, 2]-t[i, 2])*(x-t[i, 0]) for i in range(3)]
        if all(v >= 0 for v in d) or all(v <= 0 for v in d):
            s = sum(d)
            if abs(s) > 1e-9: hs.append((t[2, 1]*d[0] + t[0, 1]*d[1] + t[1, 1]*d[2]) / s)
    return hs
bad = 0
for z in np.arange(-1998, -1280, 4.0):
    row = ''
    for x in np.arange(1203, 1246, 2.0):
        dh = heights(D, x, z); ch = heights(C, x, z)
        miss = [h for h in dh if not any(abs(h - c) < 1.5 for c in ch)]
        row += 'x' if miss else '#' if dh else '.'
        bad += bool(miss)
    print('%6.0f %s' % (z, row))
print('cells drawn without a floor:', bad)
print('z: drawn heights at x 1222 | floor heights')
for z in np.arange(-1990, -1284, 24.0):
    print('%6.0f' % z, [round(float(h), 1) for h in sorted(heights(D, 1222.0, z))], '|', [round(float(h), 1) for h in sorted(heights(C, 1222.0, z))])
