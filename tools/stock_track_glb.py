#!/usr/bin/env python3
"""Exports the stock race tracks as custom-track folders (track.glb + track.toml) and checks them with the custom-track
converter, so the converter's checks and AI speeds can be judged against the original games' tracks in aggregate.

Each track's glb holds what a modder would build (docs/custom_tracks.md): the collision as COL_<surface>_* meshes (and
again as the visible mesh), FINISH and CHECKPOINT_n Empties from the path file's checkpoints, and AIPATH_main as a
ribbon whose edges put the converter's outer lanes (+-0.6 of the half width) on the stock outer lanes.

    python tools/stock_track_glb.py OUT_DIR [--check]

--check runs tools/track_mod_test.bat on every exported track (OUT_DIR/<track>/out) and prints, for the converter's
messages, how many tracks raise each one, plus its AI target speeds next to the stock lanes'. Rush 2's ten circuit
tracks and Rush 2049's six race tracks are exported (RUSH2_ROM / RUSH2049_ROM as for tools/rush2049/roms.py).
"""
import json, math, os, re, struct, subprocess, sys
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
import roms, paths, collision  # noqa: E402

FEET_PER_METER = 3.2808399
# Collision polygon type (flags & 0xF) -> the COL_ name the converter maps back to it (track_mod_convert.cpp).
SURFACE = {0: 'ROAD', 1: 'DIRT', 2: 'GRASS', 3: 'WATER', 5: 'WALL', 6: 'WALL', 7: 'KILL'}
RUSH2_TRACKS = ['VEGAS', 'NYONE', 'HAWAII', 'NYTWO', 'ALCATRAZ', 'LA', 'SEATTLE', 'HALFPIPE', 'CRASH', 'PIPE', 'ATARI']


def to_gltf(p):
    """Game feet (x negated against glTF) -> glTF meters."""
    return (-p[0] / FEET_PER_METER, p[1] / FEET_PER_METER, p[2] / FEET_PER_METER)


def polygons(c):
    """World-space polygons of a parsed Rush 2 collision as (type, [game xyz...], normal): vertex 0 is the world
    origin, the others local, world = v0 + M^T local with M's rows the polygon's axes (row 1 its normal)."""
    out = []
    for p in c.polys:
        if len(p['verts']) < 3:
            continue
        m = [v / 16384.0 for v in p['matrix']]
        rows = (m[0:3], m[3:6], m[6:9])
        v0 = collision.vert_xyz(c.verts[p['verts'][0]])
        pts = [v0]
        for vi in p['verts'][1:]:
            l = collision.vert_xyz(c.verts[vi])
            pts.append(tuple(v0[k] + l[0] * rows[0][k] + l[1] * rows[1][k] + l[2] * rows[2][k] for k in range(3)))
        out.append((p['flags'] & 0xF, pts, rows[1]))
    return out


def cross(a, b):
    return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])


def sub(a, b):
    return tuple(x - y for x, y in zip(a, b))


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


class Glb:
    """A minimal glTF 2.0 binary writer: meshes (positions, normals, uvs, one plain material) and Empties."""

    def __init__(self):
        self.js = {'asset': {'version': '2.0', 'generator': 'rush2-recomp tools/stock_track_glb.py'}, 'scene': 0,
                   'scenes': [{'nodes': []}], 'nodes': [], 'meshes': [], 'accessors': [], 'bufferViews': [],
                   'buffers': [], 'materials': [{'name': 'stock', 'pbrMetallicRoughness': {
                       'baseColorFactor': [0.5, 0.5, 0.5, 1.0], 'metallicFactor': 0.0}}]}
        self.bin = bytearray()

    def _view(self, data, target):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.js['bufferViews'].append({'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data), 'target': target})
        self.bin += data
        return len(self.js['bufferViews']) - 1

    def _accessor(self, values, kind, width, ctype, target, minmax=False):
        fmt = '<%d%s' % (len(values), 'f' if ctype == 5126 else 'I')
        a = {'bufferView': self._view(struct.pack(fmt, *values), target), 'componentType': ctype,
             'count': len(values) // width, 'type': kind}
        if minmax:
            a['min'] = [min(values[k::width]) for k in range(width)]
            a['max'] = [max(values[k::width]) for k in range(width)]
        self.js['accessors'].append(a)
        return len(self.js['accessors']) - 1

    def mesh(self, name, verts, normals, tris):
        """verts/normals: glTF xyz tuples; tris: index triples (counter-clockwise front)."""
        pos = [c for v in verts for c in v]
        nrm = [c for v in normals for c in v]
        prim = {'attributes': {'POSITION': self._accessor(pos, 'VEC3', 3, 5126, 34962, True),
                               'NORMAL': self._accessor(nrm, 'VEC3', 3, 5126, 34962),
                               'TEXCOORD_0': self._accessor([0.0] * (2 * len(verts)), 'VEC2', 2, 5126, 34962)},
                'indices': self._accessor([i for t in tris for i in t], 'SCALAR', 1, 5125, 34963), 'material': 0}
        self.js['meshes'].append({'name': name, 'primitives': [prim]})
        self._node({'name': name, 'mesh': len(self.js['meshes']) - 1})

    def empty(self, name, matrix):
        self._node({'name': name, 'matrix': matrix})

    def _node(self, n):
        self.js['nodes'].append(n)
        self.js['scenes'][0]['nodes'].append(len(self.js['nodes']) - 1)

    def bytes(self):
        while len(self.bin) % 4:
            self.bin.append(0)
        self.js['buffers'] = [{'byteLength': len(self.bin)}]
        j = json.dumps(self.js, separators=(',', ':')).encode()
        j += b' ' * (-len(j) % 4)
        return (struct.pack('<III', 0x46546C67, 2, 28 + len(j) + len(self.bin)) + struct.pack('<II', len(j), 0x4E4F534A)
                + j + struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin))


def gate_matrix(pos, direction, radius):
    """An Empty at a checkpoint (game pos, game direction, radius ft): its -Z (Blender's green Y arrow) along the
    direction, Y up, X scaled to the half width in meters. Column-major."""
    f = (-direction[0], 0.0, direction[2])
    n = math.hypot(f[0], f[2]) or 1.0
    f = (f[0] / n, 0.0, f[2] / n)
    z = (-f[0], 0.0, -f[2])
    y = (0.0, 1.0, 0.0)
    x = cross(y, z)
    s = radius / FEET_PER_METER
    t = to_gltf(pos)
    return [x[0] * s, x[1] * s, x[2] * s, 0, y[0], y[1], y[2], 0, z[0], z[1], z[2], 0, t[0], t[1], t[2], 1]


def ribbon(path):
    """AIPATH_main rungs along the spine, across it: the outermost of the four lanes' nearest points on each side
    become +-0.6 of the rung's half width, as the converter places its outer lanes. Some tracks repeat a lane (Vegas
    and Atari have lane 0 = lane 3), so the spread of all four is taken; where they all meet, the median width."""
    lanes = [ln['points'] for ln in path['lanes']]
    spine = path['spine']

    def nearest(pts, p):
        return min(pts, key=lambda q: (q[0] - p[0]) ** 2 + (q[2] - p[2]) ** 2)
    rungs = []
    for i, s in enumerate(spine):
        a, b = spine[i - 1 if i else 0], spine[min(i + 1, len(spine) - 1)]
        t = (b[0] - a[0], b[2] - a[2])
        n = math.hypot(*t) or 1.0
        lat = (-t[1] / n, t[0] / n)
        offs = [(q[0] - s[0]) * lat[0] + (q[2] - s[2]) * lat[1] for q in (nearest(l, s) for l in lanes)]
        rungs.append((s, lat, min(offs), max(offs)))
    spreads = sorted(hi - lo for _, _, lo, hi in rungs)
    median = spreads[len(spreads) // 2]
    out = []
    for s, lat, lo, hi in rungs:
        if hi - lo < median * 0.25:
            lo, hi = -median / 2, median / 2
        # Centred on the stock spine (its points are the race's progress line), wide enough for the outer lanes.
        half = max(-lo, hi) / 0.6
        mid = s
        out.append(((mid[0] - lat[0] * half, mid[1], mid[2] - lat[1] * half),
                    (mid[0] + lat[0] * half, mid[1], mid[2] + lat[1] * half)))
    return out


def export(name, coll_rush2, path_bytes, out_dir):
    c = collision.Collision.parse(coll_rush2, collision.R2)
    path = paths.parse(path_bytes)
    g = Glb()
    by_type = defaultdict(lambda: ([], [], []))
    visual = ([], [], [])
    unknown = defaultdict(int)
    for ptype, pts, normal in polygons(c):
        surface = SURFACE.get(ptype)
        if surface is None:
            unknown[ptype] += 1
            surface = 'WALL'
        for target in (by_type[surface], visual):
            verts, nrms, tris = target
            for k in range(1, len(pts) - 1):
                tri = [pts[0], pts[k], pts[k + 1]]
                # The converter's front (game space) is -(v1-v0)x(v2-v0); keep it on the polygon's normal side.
                if dot(cross(sub(tri[1], tri[0]), sub(tri[2], tri[0])), normal) > 0:
                    tri[1], tri[2] = tri[2], tri[1]
                base = len(verts)
                verts += [to_gltf(p) for p in tri]
                nrms += [(-normal[0], normal[1], normal[2])] * 3
                tris.append((base, base + 1, base + 2))
    for surface, (verts, nrms, tris) in sorted(by_type.items()):
        g.mesh('COL_%s_stock' % surface, verts, nrms, tris)
    g.mesh('TRACK_visual', *visual)
    # The header's finish checkpoint (2049's race 4 and 5 flag none), then the others in race order from it. A stock
    # gate's direction points back against the race (every stock path file); the game's gate test takes a side change
    # either way (0x800A15BC), while the converter takes FINISH's arrow as the race direction, so it is turned round.
    cps = path['cps']
    f = path['finish_cp'] if 0 <= path['finish_cp'] < len(cps) else 0
    for i in range(len(cps)):
        cp = cps[(f + i) % len(cps)]
        back = tuple(-d for d in cp['dir'])
        g.empty('FINISH' if i == 0 else 'CHECKPOINT_%d' % i, gate_matrix(cp['pos'], back, math.sqrt(cp['r2'])))
    verts, nrms, tris = [], [], []
    for a, b in ribbon(path):
        verts += [to_gltf(a), to_gltf(b)]
        nrms += [(0.0, 1.0, 0.0)] * 2
    n = len(verts) // 2
    for i in range(n - 1):
        tris += [(2 * i, 2 * i + 1, 2 * i + 2), (2 * i + 1, 2 * i + 3, 2 * i + 2)]
    g.mesh('AIPATH_main', verts, nrms, tris)
    folder = os.path.join(out_dir, name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'track.glb'), 'wb') as f:
        f.write(g.bytes())
    with open(os.path.join(folder, 'track.toml'), 'w', newline='\n') as f:
        f.write('[track]\nname = "%s"\n' % name)
    lanes = [q[3] for ln in path['lanes'] for q in ln['points']]
    return folder, lanes, dict(unknown)


def tracks():
    r2 = roms.Rush2()
    for t, name in enumerate(RUSH2_TRACKS):
        if name == 'HALFPIPE':
            continue
        yield 'r2_' + name.lower(), r2.asset(0x4B + t), r2.asset(0x57 + t)
    q = roms.Rush2049()
    for k in range(1, 7):
        yield 'r49_race%d' % k, collision.convert(q.file(138 + k)), q.file(157 + k)


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    out_dir = os.path.abspath(sys.argv[1])
    check = '--check' in sys.argv[2:]
    counts = defaultdict(set)
    speed_rows = []
    for name, coll, path_bytes in tracks():
        folder, lanes, unknown = export(name, coll, path_bytes, out_dir)
        if unknown:
            print('%s: polygon types without a COL_ name, exported as walls: %s' % (name, unknown))
        if not check:
            print('wrote', folder)
            continue
        res = subprocess.run(['cmd', '/c', os.path.join(HERE, 'track_mod_test.bat'), folder, os.path.join(folder, 'out')],
                             capture_output=True, text=True)
        text = res.stdout + res.stderr
        ok = 'OK: wrote' in text
        for line in text.splitlines():
            m = re.match(r'(ERROR|Warning): (.*)', line)
            if m:
                # Group by the message's wording with numbers and object names taken out.
                key = re.sub(r'[-\d.]+', '#', re.sub(r'CHECKPOINT_\d+|FINISH', 'GATE', m.group(2)))[:110]
                counts[(m.group(1), key)].add(name)
        m = re.search(r'target speeds (\d+)-(\d+) mph', text)
        lanes.sort()
        speed_rows.append('%-12s %s  converter %s mph   stock lanes min %d / 10%% %d / 50%% %d' % (
            name, 'converts' if ok else 'FAILS   ', '%s-%s' % m.groups() if m else '?', lanes[0],
            lanes[len(lanes) // 10], lanes[len(lanes) // 2]))
    if check:
        print('\nConverter messages over %d stock tracks:' % len(speed_rows))
        for (level, key), names in sorted(counts.items(), key=lambda kv: -len(kv[1])):
            print('  %2d tracks  %-7s %s' % (len(names), level, key))
        print('\nAI speeds:')
        for row in speed_rows:
            print('  ' + row)
    return 0


if __name__ == '__main__':
    sys.exit(main())
