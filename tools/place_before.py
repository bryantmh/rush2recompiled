#!/usr/bin/env python3
"""A RUSH2_CS_PLACE value that parks the player car on a custom track's AI path DIST feet before the path point nearest
a target, facing along the path, so a test shot looks at the target (the chase camera trails ~20 ft).

    python tools/place_before.py TRACK.glb X,Y,Z [DIST]      (game feet; DIST default 150)

Prints x,y,z,dx,dz,0,1 for `tools/test_race.py TRACK --env RUSH2_CS_PLACE=<it> --script "1:B:59"` (the held brake keeps
the car parked: test runs hold the gas otherwise). Game feet = the glb's meters x 3.2808399 with x negated (the custom
track converter's frame). The path is the AIPATH* meshes' points in order.
"""
import json, struct, sys

import numpy as np

FPM = 3.2808399


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    b = open(sys.argv[1], 'rb').read()
    n = struct.unpack_from('<I', b, 12)[0]
    j = json.loads(b[20:20 + n])
    bo = 20 + n + 8
    paths = []
    for nd in j['nodes']:
        if not nd.get('name', '').startswith('AIPATH') or 'mesh' not in nd:
            continue
        t = np.array(nd.get('translation', [0, 0, 0]), float)
        for p in j['meshes'][nd['mesh']]['primitives']:
            a = j['accessors'][p['attributes']['POSITION']]
            v = j['bufferViews'][a['bufferView']]
            o = bo + v.get('byteOffset', 0) + a.get('byteOffset', 0)
            P = (np.frombuffer(b, '<f4', a['count'] * 3, o).reshape(-1, 3) + t) * FPM
            P[:, 0] *= -1
            paths.append(P)
    if not paths:
        print('no AIPATH meshes')
        return 1
    q = np.array([float(v) for v in sys.argv[2].split(',')])
    dist = float(sys.argv[3]) if len(sys.argv) > 3 else 150.0
    P = min(paths, key=lambda P: np.linalg.norm(P - q, axis=1).min())
    k = int(np.linalg.norm(P - q, axis=1).argmin())
    i, run = k, 0.0
    while run < dist:
        i = (i - 1) % len(P)
        run += float(np.linalg.norm(P[(i + 1) % len(P)] - P[i]))
    d = P[k] - P[i]
    d = d / max(np.hypot(d[0], d[2]), 1e-9)
    print('%.0f,%.0f,%.0f,%.3f,%.3f,0,1' % (P[i][0], P[i][1] + 2, P[i][2], d[0], d[2]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
