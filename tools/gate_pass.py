"""Replays Rush 2's gate test along a converted track's AI lines, offline: which gates each line counts, where, and
which ones it misses or counts away from the road it crosses them on.

    python tools/gate_pass.py TRACK.glb [TRACK.glb ...] [--laps N] [--radius-ft R] [--verbose]

The gate test (func_800A1468, docs/rush2049_research/checkpoints.md#8-car-tracker-checkpoint-window-and-respawn-point-rush-2-vs-2049-vs-sf-rush):
only the car's next gate is tested; a change of side of its line counts when the car is then within the gate's
radius from its center seen from above; a change outside it is used up. The radius is the larger of the marker's
half width (X scale) and its pass_radius custom property (m), as the mod converter writes it into the path file
(docs/custom_tracks.md). --radius-ft overrides every gate's radius (to try a value).

Each line (AIPATH_main, AIPATH_lane1-4: the ribbons' centers) is driven N laps (closed lines repeat; open ones
once). A count is flagged WIDE when the line crossed that gate's line over its half width away from the marker, so
it counted on another stretch of road than the one the marker is on (an early count). Needs numpy.
"""
import argparse, json, struct, sys
import numpy as np

FT = 3.2808399


def load(path):
    data = open(path, 'rb').read()
    jlen = struct.unpack_from('<I', data, 12)[0]
    js = json.loads(data[20:20 + jlen])
    blen = struct.unpack_from('<I', data, 20 + jlen)[0]
    return js, data[28 + jlen:28 + jlen + blen]


def accessor(js, bin_, i):
    a = js['accessors'][i]
    bv = js['bufferViews'][a['bufferView']]
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}[a['type']]
    dt = np.dtype({5126: np.float32, 5125: np.uint32, 5123: np.uint16, 5121: np.uint8}[a['componentType']])
    o = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    st = bv.get('byteStride') or n * dt.itemsize
    raw = np.frombuffer(bin_, np.uint8, (a['count'] - 1) * st + n * dt.itemsize, o)
    rows = np.lib.stride_tricks.as_strided(raw, (a['count'], n * dt.itemsize), (st, 1))
    return rows.copy().view(dt).reshape(a['count'], n)


def line_points(js, bin_, node):
    """A ribbon's center line (its vertices come in pairs across it), in feet, game x/z order kept as glTF's."""
    prim = js['meshes'][node['mesh']]['primitives'][0]
    v = accessor(js, bin_, prim['attributes']['POSITION']).astype(np.float64) * FT
    return (v[0::2] + v[1::2]) / 2


def gates_of(js):
    out = []
    for nd in js['nodes']:
        name = nd.get('name') or ''
        if not (name.startswith('CHECKPOINT_') or name in ('FINISH', 'START')) or 'matrix' not in nd:
            continue
        m = np.array(nd['matrix'], dtype=np.float64).reshape(4, 4)
        x = m[0, :3]
        half = np.linalg.norm(x) * FT
        pass_r = float((nd.get('extras') or {}).get('pass_radius', 0.0)) * FT
        out.append({'name': name, 'pos': m[3, :3] * FT, 'side': x / np.linalg.norm(x), 'half': half,
                    'radius': max(half, pass_r)})
    # race order on a circuit: FINISH, then the checkpoints by number
    def key(g):
        n = g['name']
        return (0, 0) if n in ('START', 'FINISH') else (1, int(n.split('_')[1]))
    return sorted(out, key=key)


def run(path, laps, radius_ft, verbose):
    js, bin_ = load(path)
    gates = gates_of(js)
    # Race order: the order AIPATH_main crosses the markers on their own road (a run-in course meets START first).
    main_node = next((nd for nd in js['nodes'] if nd.get('name') == 'AIPATH_main' and 'mesh' in nd), None)
    if main_node is not None:
        m = line_points(js, bin_, main_node)
        first = {}
        for k, g in enumerate(gates):
            nrm = np.array([-g['side'][2], 0.0, g['side'][0]])
            s = (m - g['pos']) @ nrm
            for i in range(len(m) - 1):
                if (s[i] >= 0) != (s[i + 1] >= 0):
                    x = m[i] + (m[i + 1] - m[i]) * (s[i] / (s[i] - s[i + 1]))
                    if np.hypot(x[0] - g['pos'][0], x[2] - g['pos'][2]) <= g['half']:
                        first[k] = i
                        break
        if np.linalg.norm(m[0] - m[-1]) >= 300:   # open: from its start; a circuit keeps FINISH first
            gates = [gates[k] for k in sorted(range(len(gates)), key=lambda k: first.get(k, 1 << 30))]
    if radius_ft:
        for g in gates:
            g['radius'] = radius_ft
    print(f'{path}: ' + ', '.join(f"{g['name']} {g['half']:.0f}/{g['radius']:.0f} ft" for g in gates))
    problems = 0
    for nd in js['nodes']:
        name = nd.get('name') or ''
        if not name.startswith('AIPATH_') or 'mesh' not in nd:
            continue
        p = line_points(js, bin_, nd)
        closed = np.linalg.norm(p[0] - p[-1]) < 300
        pts = np.concatenate([p] * laps) if closed else p

        def crossing(g, a, b):
            nrm = np.array([-g['side'][2], 0.0, g['side'][0]])
            sa, sb = (a - g['pos']) @ nrm, (b - g['pos']) @ nrm
            if (sa >= 0) == (sb >= 0):
                return None
            x = a + (b - a) * (sa / (sa - sb))
            return float(np.hypot(x[0] - g['pos'][0], x[2] - g['pos'][2]))

        # The next gate: the first one the line crosses on its marker's road.
        nxt = None
        for i in range(len(pts) - 1):
            for k, g in enumerate(gates):
                r = crossing(g, pts[i], pts[i + 1])
                if r is not None and r <= g['half']:
                    nxt = k
                    break
            if nxt is not None:
                break
        if nxt is None:
            print(f'  {name}: crosses no gate')
            problems += 1
            continue
        counted, wide, used_up = [], [], []
        for i in range(len(pts) - 1):
            g = gates[nxt]
            r = crossing(g, pts[i], pts[i + 1])
            if r is None:
                continue
            if r <= g['radius']:
                counted.append((i % len(p), g['name'], r))
                if r > g['half']:
                    wide.append((i % len(p), g['name'], r))
                nxt = (nxt + 1) % len(gates)
            elif r <= g['half'] * 3:
                used_up.append((i % len(p), g['name'], r))
        expect = laps * len(gates) if closed else None
        bad = (expect is not None and len(counted) < expect - 1) or wide
        problems += bool(bad)
        print(f'  {name}: {len(p)} points, {"closed" if closed else "open"}, {len(counted)} counts'
              + (f' of ~{expect}' if expect else '') + (f', {len(wide)} WIDE' if wide else ''))
        for i, gn, r in wide:
            print(f'    WIDE {gn} at point {i}, {r:.0f} ft from the marker')
        for i, gn, r in used_up:
            print(f'    missed {gn} at point {i}, {r:.0f} ft from the marker')
        if verbose:
            for i, gn, r in counted:
                print(f'    {gn} at point {i}, {r:.0f} ft')
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('glb', nargs='+')
    ap.add_argument('--laps', type=int, default=3)
    ap.add_argument('--radius-ft', type=float, default=0.0)
    ap.add_argument('--verbose', action='store_true')
    a = ap.parse_args()
    bad = sum(run(g, a.laps, a.radius_ft, a.verbose) for g in a.glb)
    print('ALL OK' if not bad else f'{bad} line(s) with problems')
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
