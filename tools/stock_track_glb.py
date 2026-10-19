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
    """A minimal glTF 2.0 binary writer: meshes (positions, normals, uvs, one plain material), Empties, node
    hierarchies and node animations."""

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
        view = self._view(struct.pack(fmt, *values), target)
        if target is None:
            del self.js['bufferViews'][view]['target']
        a = {'bufferView': view, 'componentType': ctype,
             'count': len(values) // width, 'type': kind}
        if minmax:
            a['min'] = [min(values[k::width]) for k in range(width)]
            a['max'] = [max(values[k::width]) for k in range(width)]
        self.js['accessors'].append(a)
        return len(self.js['accessors']) - 1

    def mesh(self, name, verts, normals, tris, matrix=None, parent=None, trs=None, color=None, attrs=None):
        """verts/normals: glTF xyz tuples; tris: index triples (counter-clockwise front); matrix: the node's (column
        major), else none; parent: a node index to put it under; trs: (translation, rotation xyzw, scale). Returns
        the node's index. color: (r, g, b) 0-1 for a plain material of its own. attrs: {'_NAME': [one float per
        vertex]}, custom attributes (the AI path's _SPEED and _AI_MODE)."""
        material = 0
        if color:
            self.js['materials'].append({'name': 'color%d' % len(self.js['materials']), 'pbrMetallicRoughness': {
                'baseColorFactor': [color[0], color[1], color[2], 1.0], 'metallicFactor': 0.0}})
            material = len(self.js['materials']) - 1
        pos = [c for v in verts for c in v]
        nrm = [c for v in normals for c in v]
        prim = {'attributes': {'POSITION': self._accessor(pos, 'VEC3', 3, 5126, 34962, True),
                               'NORMAL': self._accessor(nrm, 'VEC3', 3, 5126, 34962),
                               'TEXCOORD_0': self._accessor([0.0] * (2 * len(verts)), 'VEC2', 2, 5126, 34962)},
                'indices': self._accessor([i for t in tris for i in t], 'SCALAR', 1, 5125, 34963), 'material': material}
        for key, values in (attrs or {}).items():
            prim['attributes'][key] = self._accessor([float(v) for v in values], 'SCALAR', 1, 5126, 34962)
        self.js['meshes'].append({'name': name, 'primitives': [prim]})
        node = {'name': name, 'mesh': len(self.js['meshes']) - 1}
        if matrix:
            node['matrix'] = matrix
        return self._node(node, parent, trs)

    def image(self, name, png):
        """Embeds PNG bytes; returns the texture index (one texture per image, with its own sampler)."""
        self.js.setdefault('images', []).append({'name': name, 'mimeType': 'image/png',
                                                 'bufferView': self._view(png, None)})
        del self.js['bufferViews'][-1]['target']
        return len(self.js['images']) - 1

    def material(self, name, image=None, alpha_mode='OPAQUE', double_sided=False, wrap=(10497, 10497)):
        """A material with an optional base color image (an index from image()); wrap = glTF sampler (wrapS, wrapT).
        Returns its index."""
        m = {'name': name, 'pbrMetallicRoughness': {'baseColorFactor': [1.0, 1.0, 1.0, 1.0], 'metallicFactor': 0.0}}
        if image is not None:
            self.js.setdefault('samplers', []).append({'wrapS': wrap[0], 'wrapT': wrap[1]})
            self.js.setdefault('textures', []).append({'source': image, 'sampler': len(self.js['samplers']) - 1})
            m['pbrMetallicRoughness']['baseColorTexture'] = {'index': len(self.js['textures']) - 1}
        if alpha_mode != 'OPAQUE':
            m['alphaMode'] = alpha_mode
        if double_sided:
            m['doubleSided'] = True
        self.js['materials'].append(m)
        return len(self.js['materials']) - 1

    def textured_mesh(self, name, prims, matrix=None, parent=None, trs=None):
        """A mesh of several primitives, each (material, verts, normals, uvs, colors or None, tris): verts/normals
        glTF xyz, uvs (u, v), colors (r, g, b, a) 0-1. Returns the node's index."""
        out = []
        for material, verts, normals, uvs, colors, tris in prims:
            at = {'POSITION': self._accessor([c for v in verts for c in v], 'VEC3', 3, 5126, 34962, True),
                  'NORMAL': self._accessor([c for v in normals for c in v], 'VEC3', 3, 5126, 34962),
                  'TEXCOORD_0': self._accessor([c for v in uvs for c in v], 'VEC2', 2, 5126, 34962)}
            if colors:
                at['COLOR_0'] = self._accessor([c for v in colors for c in v], 'VEC4', 4, 5126, 34962)
            out.append({'attributes': at, 'material': material,
                        'indices': self._accessor([i for t in tris for i in t], 'SCALAR', 1, 5125, 34963)})
        self.js['meshes'].append({'name': name, 'primitives': out})
        node = {'name': name, 'mesh': len(self.js['meshes']) - 1}
        if matrix:
            node['matrix'] = matrix
        return self._node(node, parent, trs)

    def empty(self, name, matrix=None, parent=None, trs=None):
        n = {'name': name}
        if matrix:
            n['matrix'] = matrix
        return self._node(n, parent, trs)

    def _node(self, n, parent=None, trs=None):
        if trs:
            n['translation'], n['rotation'], n['scale'] = [list(map(float, v)) for v in trs]
        self.js['nodes'].append(n)
        i = len(self.js['nodes']) - 1
        if parent is None:
            self.js['scenes'][0]['nodes'].append(i)
        else:
            self.js['nodes'][parent].setdefault('children', []).append(i)
        return i

    def animate(self, name, node, times, translation=None, rotation=None, scale=None, interpolation='LINEAR'):
        """A node animation: key times (s) and, per key, the values of whichever of translation, rotation (xyzw) and
        scale are given."""
        anim = {'name': name, 'channels': [], 'samplers': []}
        t = self._accessor([float(x) for x in times], 'SCALAR', 1, 5126, None, True)
        for path, values, kind, width in (('translation', translation, 'VEC3', 3), ('rotation', rotation, 'VEC4', 4),
                                          ('scale', scale, 'VEC3', 3)):
            if values is None:
                continue
            out = self._accessor([float(c) for v in values for c in v], kind, width, 5126, None)
            anim['samplers'].append({'input': t, 'output': out, 'interpolation': interpolation})
            anim['channels'].append({'sampler': len(anim['samplers']) - 1, 'target': {'node': node, 'path': path}})
        self.js.setdefault('animations', []).append(anim)

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


RUN_UP = 100.0   # ft of path before an open track's START, for the grid behind it


def closed_path(spine):
    """Whether the spine is a loop: its ends are no further apart than three of its usual steps."""
    if len(spine) < 3:
        return False
    steps = sorted(math.dist(spine[i], spine[i + 1]) for i in range(len(spine) - 1))
    return math.dist(spine[0], spine[-1]) <= 3 * steps[len(steps) // 2]


def strip(rungs, closed):
    """A ribbon mesh's (verts, normals, tris) from rungs of game points (a, b), joined end to start when closed."""
    verts, nrms, tris = [], [], []
    for a, b in rungs:
        verts += [to_gltf(a), to_gltf(b)]
        nrms += [(0.0, 1.0, 0.0)] * 2
    n = len(rungs)
    for i in range(n if closed else n - 1):
        j = (i + 1) % n
        tris += [(2 * i, 2 * i + 1, 2 * j), (2 * i + 1, 2 * j + 1, 2 * j)]
    return verts, nrms, tris


def race_dir(spine, p, closed, at=None):
    """The spine's direction of travel (game x, z) at spine point `at`, else at its point nearest p."""
    n = len(spine)
    if at is not None and 0 <= at < n:
        i = at
    else:
        i = min(range(n), key=lambda k: (spine[k][0] - p[0]) ** 2 + (spine[k][2] - p[2]) ** 2)
    a = spine[(i - 1) % n if closed else max(i - 1, 0)]
    b = spine[(i + 1) % n if closed else min(i + 1, n - 1)]
    return (b[0] - a[0], b[2] - a[2])


def lane_ribbons(path, closed, half=1.5):
    """The four stock lanes as AIPATH_lane1..4 ribbons (name, rungs, speeds, modes), lane1 the leftmost in the race's
    direction as the converter numbers them (track_mod_convert.cpp: left of travel is (-fz, fx) in game space).
    Each ribbon is 2 x `half` ft wide around the lane; its _SPEED (mph) and _AI_MODE are the lane bytes."""
    spine = path['spine']
    out = []
    for ln in path['lanes']:
        pts = ln['points']
        if len(pts) < 2:
            continue
        rungs, side = [], 0.0
        for i, q in enumerate(pts):
            a = pts[(i - 1) % len(pts) if closed else max(i - 1, 0)]
            b = pts[(i + 1) % len(pts) if closed else min(i + 1, len(pts) - 1)]
            t = (b[0] - a[0], b[2] - a[2])
            m = math.hypot(*t) or 1.0
            lat = (-t[1] / m, t[0] / m)
            rungs.append(((q[0] - lat[0] * half, q[1], q[2] - lat[1] * half),
                          (q[0] + lat[0] * half, q[1], q[2] + lat[1] * half)))
            f = race_dir(spine, q, closed)
            s = min(spine, key=lambda k: (k[0] - q[0]) ** 2 + (k[2] - q[2]) ** 2)
            side += (q[0] - s[0]) * -f[1] + (q[2] - s[2]) * f[0]
        out.append((side / len(pts), rungs, [q[3] for q in pts], [q[4] for q in pts]))
    out.sort(key=lambda r: -r[0])
    return [('AIPATH_lane%d' % (k + 1), r[1], r[2], r[3]) for k, r in enumerate(out)]


def build_glb(coll_rush2, path_bytes, visual=True, lanes=False):
    """A custom track's glb (Glb) of a Rush 2 format collision and path file: COL_<surface>_stock meshes, the gates
    as FINISH / CHECKPOINT_n Empties, AIPATH_main, and with `visual` the collision again as TRACK_visual. `lanes` adds
    the four lanes as AIPATH_lane1..4 with their speeds and steering (tools/trackview/export.py), which the converter
    then uses as they are. An open path is written as path_layout says: a point-to-point track (START, the
    checkpoints between, FINISH) or a run-in circuit (START, the gates in race order, the path ending at the loop
    start), with AIPATH_main left open. Returns (glb, polygon types exported as walls, the parsed path, the layout).
    The track.toml needs the layout's key (`point_to_point = true` / `run_in = true`)."""
    c = collision.Collision.parse(coll_rush2, collision.R2)
    path = paths.parse(path_bytes)
    g = Glb()
    by_type = defaultdict(lambda: ([], [], []))
    shown = ([], [], [])
    unknown = defaultdict(int)
    for ptype, pts, normal in polygons(c):
        surface = SURFACE.get(ptype)
        if surface is None:
            unknown[ptype] += 1
            surface = 'WALL'
        for target in (by_type[surface], shown) if visual else (by_type[surface],):
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
    if visual:
        g.mesh('TRACK_visual', *shown)
    # The header's finish checkpoint (2049's race 4 and 5 flag none), then the others in race order from it. A stock
    # gate's direction points back against the race (every stock path file); the game's gate test takes a side change
    # either way (0x800A15BC), while the converter takes FINISH's arrow as the race direction, so each gate's arrow is
    # set along the spine's travel where it crosses (other games' converted paths needn't follow Rush 2's habit).
    spine = path['spine']
    closed = closed_path(spine)
    cps = path['cps']
    layout = path_layout(path)
    if layout == 'point_to_point':
        s, e = path_start(cps), path_finish(cps)
        named = [('START', s)] + [('CHECKPOINT_%d' % (k + 1), i) for k, i in enumerate(range(s + 1, e))] + [('FINISH', e)]
    elif layout == 'run_in':
        # The gates in file order (the race's): START, FINISH where it is (Golden Gate has a checkpoint after it), the
        # others numbered in order. The converter flags the loop start itself, from where AIPATH_main ends.
        s, e = path_start(cps), path_finish(cps)
        named, k = [], 0
        for i in range(s, len(cps)):
            if i == s:
                named.append(('START', i))
            elif i == e:
                named.append(('FINISH', i))
            else:
                k += 1
                named.append(('CHECKPOINT_%d' % k, i))
    else:
        f = path['finish_cp'] if 0 <= path['finish_cp'] < len(cps) else 0
        named = [('FINISH' if i == 0 else 'CHECKPOINT_%d' % i, (f + i) % len(cps)) for i in range(len(cps))]
    if layout != 'circuit':
        closed = False
    rungs = ribbon(path)
    for name, i in named:
        cp = cps[i]
        pos = cp['pos']
        if name == 'START':
            # The grid lines up behind START, so a custom track's AIPATH_main needs RUN_UP ft before it. California
            # Speed's start lines are at their paths' first point and the game moves them on for the grid
            # (src/calspeed/calspeed_convert.cpp move_start): START goes RUN_UP ft along. Rush 2's and San Francisco
            # Rush's run-in starts stay where they are: the path is drawn back RUN_UP ft behind them instead, along
            # its first step.
            if 0 <= cp['cross'][0] < len(spine):
                at = cp['cross'][0]
            else:
                at = min(range(len(spine)), key=lambda k: math.dist(spine[k], pos))
            run = sum(math.dist(spine[k], spine[k + 1]) for k in range(at))
            if run < RUN_UP and layout == 'point_to_point':
                k = at
                while run < RUN_UP and k + 1 < len(spine):
                    run += math.dist(spine[k], spine[k + 1])
                    k += 1
                if k != at:
                    pos = tuple(float(v) for v in spine[k])
            elif run < RUN_UP and len(rungs) >= 2:
                (a0, b0), (a1, b1) = rungs[0], rungs[1]
                d = [(a0[c] + b0[c] - a1[c] - b1[c]) / 2 for c in range(3)]
                m = math.hypot(d[0], d[2]) or 1.0
                d = (d[0] / m, 0.0, d[2] / m)
                steps = int(math.ceil((RUN_UP - run) / 20.0))
                rungs = [(tuple(a0[c] + d[c] * 20.0 * j for c in range(3)), tuple(b0[c] + d[c] * 20.0 * j for c in range(3)))
                         for j in range(steps, 0, -1)] + rungs
        # Along the spine where the gate's own crossing is (cross[0], the path file's): a gate where two stretches
        # pass close (New York One's checkpoint 4) would find the other stretch by distance.
        travel = race_dir(spine, pos, closed, None if pos is not cp['pos'] else cp['cross'][0])
        along = cp['dir'][0] * travel[0] + cp['dir'][2] * travel[1] >= 0
        arrow = tuple(cp['dir']) if along else tuple(-d for d in cp['dir'])
        if pos is not cp['pos']:
            m = math.hypot(*travel) or 1.0
            arrow = (travel[0] / m, 0.0, travel[1] / m)
        g.empty(name, gate_matrix(pos, arrow, math.sqrt(cp['r2'])))
    g.mesh('AIPATH_main', *strip(rungs, closed and lanes))
    if lanes:
        for name, lane_rungs, speed, mode in lane_ribbons(path, closed):
            verts, nrms, tris = strip(lane_rungs, closed)
            g.mesh(name, verts, nrms, tris, attrs={'_SPEED': [v for v in speed for _ in (0, 1)],
                                                  '_AI_MODE': [v for v in mode for _ in (0, 1)]})
    return g, dict(unknown), path, layout


def path_start(cps):
    """The start line: the first checkpoint flagged 2 (lap counting armed)."""
    return next((i for i, c in enumerate(cps) if c['flags'] & 2), 0)


def path_finish(cps):
    """The finish (lap) line: the first checkpoint flagged 1."""
    return next((i for i, c in enumerate(cps) if c['flags'] & 1), 0)


def path_layout(path):
    """'circuit', 'run_in' or 'point_to_point' (track.toml's keys) for a parsed path file. An open spine whose start
    line isn't its finish line is a run-in circuit when its laps loop from a later checkpoint (flags 4 or the header's
    loop_cp: Rush 2's New York One, Rush 2049's race 6, San Francisco Rush's Golden Gate; the spine then ends at that
    checkpoint's crossing), else point to point (California Speed's courses)."""
    cps = path['cps']
    s, e = path_start(cps), path_finish(cps)
    if closed_path(path['spine']) or s == e or not any(c['flags'] & 2 for c in cps):
        return 'circuit'
    loops = [i for i, c in enumerate(cps) if c['flags'] & 4] + ([path['loop_cp']] if path['loop_cp'] >= 0 else [])
    return 'run_in' if any(i != s for i in loops) else 'point_to_point'


def export(name, coll_rush2, path_bytes, out_dir):
    g, unknown, path, layout = build_glb(coll_rush2, path_bytes)
    folder = os.path.join(out_dir, name)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'track.glb'), 'wb') as f:
        f.write(g.bytes())
    with open(os.path.join(folder, 'track.toml'), 'w', newline='\n') as f:
        f.write('[track]\nname = "%s"\n' % name + ('%s = true\n' % layout if layout != 'circuit' else ''))
    lanes = [q[3] for ln in path['lanes'] for q in ln['points']]
    return folder, lanes, unknown


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
