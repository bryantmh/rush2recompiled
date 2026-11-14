#!/usr/bin/env python3
"""Where a converted track's drivable floor crosses each gate's line outside the radius Rush 2 counts it in.

    python tools/gate_shortcuts.py TRACK.glb [--plot OUT.png] [--radius-ft R] [--min-ft W]

Rush 2's gate test (func_800A1468, tools/gate_pass.py): only the car's next gate is tested, and a change of side of
its (vertical, endless) plane counts only when the car is then within the gate's radius of its center seen from
above; a crossing outside the radius is used up, so that gate, and every one after it, is never counted again and the
race runs out of time. A driver who crosses the line on any floor outside the radius while that gate is next (a
shortcut, a parallel street, a lower deck) misses it.

For each gate: every stretch of drivable floor (COL_ROAD, COL_DIRT, COL_GRASS, COL_VERGE, COL_CHANNEL) its plane cuts,
as lateral distance from the gate's center along its line and height, and whether the radius covers it; uncovered
stretches at least --min-ft wide (default 8) are listed (and, as ALSO COUNTS, other floor inside the radius: a
crossing there counts the gate off its own road), with whether the AI lines run there (another stretch of the
course, which tools/gate_pass.py replays) or not (floor off the route: a shortcut or side street), and then whether it
lies beside the stretch of the course between the gate before and this one, where crossing it misses the gate. --plot draws each gate's neighborhood from above (game
handedness: glTF x mirrored): floor gray, walls red, AI path blue, the gate's line counted green and uncounted
magenta, its radius as a circle. --overview draws the whole track (floor off the AI lines orange) with every gate's crossings. --radius-ft overrides the radius (to try a value); --marker uses
the marker's half width alone (as before pass_radius). Needs numpy (and matplotlib).
"""
import argparse, os, sys
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb_view as gv  # noqa: E402

FT = 3.2808399
FLOOR = ['COL_ROAD', 'COL_DIRT', 'COL_GRASS', 'COL_VERGE', 'COL_CHANNEL']


def tris(g, b, keys):
    t = gv.triangles(g, b, keys)
    return t[0] * FT if t else np.zeros((0, 3, 3))


def gates(g):
    out = []
    for n in g['nodes']:
        nm = n.get('name', '')
        if not (nm.startswith('CHECKPOINT_') or nm == 'FINISH'):
            continue
        m = gv.node_matrix(n)
        side = m[:3, 0].copy()
        half = np.linalg.norm(side) * FT
        side /= np.linalg.norm(side)
        pr = (n.get('extras') or {}).get('pass_radius')
        out.append({'name': nm, 'c': m[:3, 3] * FT, 'side': side, 'half': half,
                    'radius': max(half, pr * FT if pr else 0.0)})
    order = lambda e: 0 if e['name'] == 'FINISH' else int(e['name'].split('_')[1])
    return sorted(out, key=order)


def cut(T, c, side):
    """The plane through c across `side` (vertical) cut through triangles T: (lateral s0, s1, height) per triangle."""
    normal = np.array([-side[2], 0.0, side[0]])
    normal /= np.linalg.norm(normal)
    d = (T - c) @ normal
    out = []
    for tri, dd in zip(T, d):
        pts = []
        for i in range(3):
            a, b = i, (i + 1) % 3
            if (dd[a] < 0) != (dd[b] < 0):
                t = dd[a] / (dd[a] - dd[b])
                pts.append(tri[a] + t * (tri[b] - tri[a]))
        if len(pts) == 2:
            s = [float((p - c) @ side) for p in pts]
            out.append((min(s), max(s), float((pts[0][1] + pts[1][1]) / 2)))
    return out


def strips(segs, gap=2.0, dy=8.0):
    """Segments merged into stretches of floor: (s0, s1, height)."""
    segs = sorted(segs)
    out = []
    for s0, s1, y in segs:
        for st in out:
            if abs(st[2] - y) < dy and s0 <= st[1] + gap and s1 >= st[0] - gap:
                st[0], st[1] = min(st[0], s0), max(st[1], s1)
                break
        else:
            out.append([s0, s1, y])
    # Merge again (a later segment can join two stretches).
    merged = True
    while merged:
        merged = False
        for i in range(len(out)):
            for j in range(i + 1, len(out)):
                a, b = out[i], out[j]
                if abs(a[2] - b[2]) < dy and a[0] <= b[1] + gap and a[1] >= b[0] - gap:
                    out[i] = [min(a[0], b[0]), max(a[1], b[1]), (a[2] + b[2]) / 2]
                    del out[j]
                    merged = True
                    break
            if merged:
                break
    return sorted(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('glb')
    ap.add_argument('--plot')
    ap.add_argument('--overview', help='the whole track from above: floor off the AI lines orange, gate lines')
    ap.add_argument('--radius-ft', type=float)
    ap.add_argument('--bounds', action='store_true', help='how wide each gate may be (see radius_bounds)')
    ap.add_argument('--marker', action='store_true', help="use the marker's half width, not pass_radius")
    ap.add_argument('--min-ft', type=float, default=8.0)
    ap.add_argument('--reach-ft', type=float, default=1500.0, help='how far along each gate line to look')
    a = ap.parse_args()
    g, b = gv.load(a.glb)
    floor = tris(g, b, FLOOR)
    gs = gates(g)
    lines = np.concatenate([tris(g, b, [k]).reshape(-1, 3) for k in
                            ('AIPATH_main', 'AIPATH_lane1', 'AIPATH_lane2', 'AIPATH_lane3', 'AIPATH_lane4')])
    print(os.path.basename(os.path.dirname(a.glb)) or a.glb)
    report = []
    # The main AI line's center, in order (tools/gate_pass.py: the ribbon's vertices come in pairs across it).
    import gate_pass
    js, bin_ = gate_pass.load(a.glb)
    main = next(gate_pass.line_points(js, bin_, nd) for nd in js['nodes'] if nd.get('name') == 'AIPATH_main')
    gate_at = [int(np.argmin(np.linalg.norm(main - e['c'], axis=1))) for e in gs]
    also = []
    for k, e in enumerate(gs):
        r = a.radius_ft or (e['half'] if a.marker else e['radius'])
        near = np.abs(((floor - e['c']) @ e['side'])).min(1) < a.reach_ft
        st = strips(cut(floor[near], e['c'], e['side']))
        missed = []
        for s0, s1, y in st:
            # The part of the stretch outside the radius, each side of the center.
            for lo, hi in ((s0, min(s1, -r)), (max(s0, r), s1)):
                if hi - lo >= a.min_ft:
                    missed.append((lo, hi, y))
        own = [x for x in st if x[0] <= 0 <= x[1] and abs(x[2] - e['c'][1]) < 15]
        reach = max(-own[0][0], own[0][1]) if own else 0.0
        print('  %-13s radius %4.0f ft (marker half width %3.0f), %d stretches of floor cross its line; the floor '
              'under it reaches %.0f ft' % (e['name'], r, e['half'], len(st), reach))
        # Other floor its radius reaches: a crossing there counts the gate too, off its own road.
        for s0, s1, y in st:
            if own and (s0, s1, y) == tuple(own[0]):
                continue
            lo, hi = max(s0, -r), min(s1, r)
            if hi - lo < a.min_ft:
                continue
            mid = e['c'] + e['side'] * (lo + hi) / 2
            mid[1] = y
            off = np.linalg.norm(lines - mid, axis=1).min() if len(lines) else float('nan')
            at = int(np.argmin(np.linalg.norm(main - mid, axis=1)))
            prev = gate_at[(k - 1) % len(gs)]
            here = gate_at[k]
            while_next = (prev <= at < here) if prev < here else (at >= prev or at < here)
            also.append((k, lo, hi, y, off))
        for lo, hi, y in missed:
            mid = e['c'] + e['side'] * (lo + hi) / 2
            mid[1] = y
            off = np.linalg.norm(lines - mid, axis=1).min() if len(lines) else float('nan')
            # Beside which stretch of the course (nearest main path point), and is that while this gate is next?
            at = int(np.argmin(np.linalg.norm(main - mid, axis=1)))
            prev = gate_at[(k - 1) % len(gs)]
            here = gate_at[k]
            while_next = (prev <= at < here) if prev < here else (at >= prev or at < here)
            print('    not counted: %+6.0f to %+6.0f ft along the line, %3.0f ft wide, height %4.0f ft (gate %4.0f), '
                  '%s' % (lo, hi, hi - lo, y, e['c'][1],
                          'on the AI lines (another stretch of the course)' if off < 60 else
                          'OFF the AI lines by %.0f ft, beside path point %d of %d, %s' % (
                              off, at, len(main), 'WHILE THIS GATE IS NEXT' if while_next else
                              'not while this gate is next')))
        report.append((e, r, st, missed))
    if a.bounds:
        radius_bounds(floor, main, gs, gate_at, [r for e, r, st, missed in report])
        return
    bypass(floor, main, gs, gate_at, report, also)
    if a.plot:
        plot(a.plot, g, b, floor, report)
    if a.overview:
        overview(a.overview, g, b, floor, lines, report)


def grid(floor, cell):
    """The floor on a grid of cell ft: levels {(ix, iz): [heights]}, nodes [(cell, level)], their index, centers x, z."""
    levels = {}
    for t in floor:
        x, z, y = t[:, 0], t[:, 2], t[:, 1]
        i0, i1 = int(np.floor(x.min() / cell)), int(np.ceil(x.max() / cell))
        k0, k1 = int(np.floor(z.min() / cell)), int(np.ceil(z.max() / cell))
        gi, gk = np.meshgrid(np.arange(i0, i1 + 1), np.arange(k0, k1 + 1), indexing='ij')
        px, pz = (gi + 0.5) * cell, (gk + 0.5) * cell
        d = (z[1] - z[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (z[0] - z[2])
        if abs(d) < 1e-9:
            continue
        l0 = ((z[1] - z[2]) * (px - x[2]) + (x[2] - x[1]) * (pz - z[2])) / d
        l1 = ((z[2] - z[0]) * (px - x[2]) + (x[0] - x[2]) * (pz - z[2])) / d
        l2 = 1 - l0 - l1
        m = (l0 >= -1e-6) & (l1 >= -1e-6) & (l2 >= -1e-6)
        hy = l0 * y[0] + l1 * y[1] + l2 * y[2]
        for ci, ck, h in zip(gi[m], gk[m], hy[m]):
            hs = levels.setdefault((int(ci), int(ck)), [])
            for h0 in hs:
                if abs(h0 - h) < 2.0:
                    break
            else:
                hs.append(float(h))
    nodes = [(c, j) for c, hs in levels.items() for j in range(len(hs))]
    index = {nd: n for n, nd in enumerate(nodes)}
    cx = np.array([(c[0] + 0.5) * cell for c, j in nodes])
    cz = np.array([(c[1] + 0.5) * cell for c, j in nodes])
    return levels, nodes, index, cx, cz


def neighbors(levels, index, n_cell, h, step):
    ci, ck = n_cell
    for di, dk in ((1, 0), (-1, 0), (0, 1), (0, -1)):
        for jj, hh in enumerate(levels.get((ci + di, ck + dk), [])):
            if abs(hh - h) < step:
                yield index[((ci + di, ck + dk), jj)]


def radius_bounds(floor, main, gs, gate_at, radii, cell=4.0, step=5.0, away=150.0, min_cells=1500):
    """How wide each gate's radius may be. With the gate's whole line cut out of the floor (and every other gate's
    counted stretch), the floor just before the gate on the main path floods region B, just after it region A. Each
    place the line crosses floor joins its two sides: B to A is a way past the gate (the radius must reach it: the
    least radius), B to B or B to anything else is a crossing on another road a car can reach while the gate is next
    (the radius must stop short of it: the most); floor in an island under min_cells cells (a scrap cut off by a
    height step or crack) is no road. A gate is right when its radius lies between them."""
    from collections import deque
    levels, nodes, index, cx, cz = grid(floor, cell)
    seg = np.linalg.norm(np.diff(main, axis=0, append=main[:1]), axis=1)

    def along(i, ft):
        st_ = 1 if ft > 0 else -1
        left = abs(ft)
        while left > 0:
            j = (i + st_) % len(main)
            left -= seg[min(i, j)] if seg[min(i, j)] < 500 else 0.0
            i = j
        return i

    lats, ds = [], []
    for e in gs:
        normal = np.array([-e['side'][2], 0.0, e['side'][0]])
        ds.append((cx - e['c'][0]) * normal[0] + (cz - e['c'][2]) * normal[2])
        lats.append((cx - e['c'][0]) * e['side'][0] + (cz - e['c'][2]) * e['side'][2])
    print('  how wide each gate may be (B: floor before it, A: after it; %g ft grid):' % cell)
    out = []
    for k, e in enumerate(gs):
        on_line = np.abs(ds[k]) < cell * 0.75
        cut = on_line.copy()
        for k2 in range(len(gs)):
            if k2 != k:
                cut |= (np.abs(ds[k2]) < cell * 0.75) & (np.abs(lats[k2]) <= radii[k2])

        def node_at(p):
            best, bd = None, 1e18
            ci, ck = int(np.floor(p[0] / cell)), int(np.floor(p[2] / cell))
            for di in range(-3, 4):
                for dk in range(-3, 4):
                    for j, h in enumerate(levels.get((ci + di, ck + dk), [])):
                        dd = di * di + dk * dk + ((h - p[1]) / cell) ** 2
                        n = index[((ci + di, ck + dk), j)]
                        if dd < bd and not cut[n]:
                            best, bd = n, dd
            return best

        def flood(n0):
            seen = np.zeros(len(nodes), bool)
            if n0 is None:
                return seen
            seen[n0] = True
            q = deque([n0])
            while q:
                n = q.popleft()
                c, j = nodes[n]
                for m in neighbors(levels, index, c, levels[c][j], step):
                    if not seen[m] and not cut[m]:
                        seen[m] = True
                        q.append(m)
            return seen

        B = flood(node_at(main[along(gate_at[k], -away)]))
        A = flood(node_at(main[along(gate_at[k], away)]))
        small = {}   # node -> whether its region is an island too small to be a road (a scrap cut off by a step)

        def island(m):
            if m not in small:
                reg = flood(m)
                tiny = reg.sum() < min_cells
                for x in np.nonzero(reg)[0]:
                    small[int(x)] = tiny
            return small[m]
        normal = np.array([-e['side'][2], 0.0, e['side'][0]])
        need, bad, bad_at = 0.0, float('inf'), None
        for n in np.nonzero(on_line)[0]:
            c, j = nodes[n]
            h = levels[c][j]
            sides = []
            for sgn in (-1, 1):
                p = np.array([cx[n], h, cz[n]]) + normal * sgn * cell * 1.5
                ci, ck = int(np.floor(p[0] / cell)), int(np.floor(p[2] / cell))
                found = None
                for jj, hh in enumerate(levels.get((ci, ck), [])):
                    m = index[((ci, ck), jj)]
                    if abs(hh - h) < step and not cut[m]:
                        found = m
                sides.append('B' if found is not None and B[found] else 'A' if found is not None and A[found] else
                             'O' if found is not None and not island(found) else '-')
            lat = abs(lats[k][n])
            if sorted(sides) == ['A', 'B']:
                need = max(need, lat)
            elif 'B' in sides and sides != ['B', '-'] and sides != ['-', 'B']:
                if lat < bad:
                    bad, bad_at = lat, (cx[n], h, cz[n], ''.join(sides))
        r = radii[k]
        ok = need <= r < bad
        where = '' if bad_at is None else ' (nearest at (%.0f, %.0f, %.0f) game ft, sides %s)' % (
            -bad_at[0], bad_at[1], bad_at[2], bad_at[3])
        print('    %-13s radius %4.0f: must reach %4.0f, must stay under %s%s: %s' % (
            e['name'], r, need, '%.0f' % bad if bad < float('inf') else 'no limit', where,
            'OK' if ok else 'TOO SMALL' if r < need else 'TOO WIDE' if need < bad else 'CONFLICT (no radius works)'))
        out.append((need, bad))
    return out


def bypass(floor, main, gs, gate_at, report, also=(), cell=4.0, step=5.0, away=150.0):
    """Whether the floor lets a car past a gate without crossing its counted stretch. The floor is laid on a grid
    (cell ft; a cell can hold several floors at different heights) and cells join their neighbors whose floor is
    within `step` ft of height. Every gate's counted stretch (cells its plane passes through inside its radius, any
    height: the radius is in x/z) is cut out; then, for each gate, if the floor just before it on the main path still
    joins the floor just after it, there is a way round: the shortest is printed, with its length, where it crosses that
gate's line and its middle (game feet), against the path's own length past the gate."""
    from collections import deque
    levels, nodes, index, cx, cz = grid(floor, cell)
    cutn = np.zeros(len(nodes), bool)
    for e, r, st, missed in report:
        normal = np.array([-e['side'][2], 0.0, e['side'][0]])
        d = (cx - e['c'][0]) * normal[0] + (cz - e['c'][2]) * normal[2]
        lat = (cx - e['c'][0]) * e['side'][0] + (cz - e['c'][2]) * e['side'][2]
        cutn |= (np.abs(d) < cell * 0.75) & (np.abs(lat) <= r)
    label = np.full(len(nodes), -1)
    nl = 0
    for s0 in range(len(nodes)):
        if label[s0] >= 0 or cutn[s0]:
            continue
        label[s0] = nl
        q = deque([s0])
        while q:
            n = q.popleft()
            (ci, ck), j = nodes[n]
            h = levels[(ci, ck)][j]
            for di, dk in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                hs = levels.get((ci + di, ck + dk))
                if not hs:
                    continue
                for jj, hh in enumerate(hs):
                    if abs(hh - h) < step:
                        m2 = index[((ci + di, ck + dk), jj)]
                        if label[m2] < 0 and not cutn[m2]:
                            label[m2] = nl
                            q.append(m2)
        nl += 1

    def node_at(p):
        best, bd = None, 1e18
        ci, ck = int(np.floor(p[0] / cell)), int(np.floor(p[2] / cell))
        for di in range(-3, 4):
            for dk in range(-3, 4):
                for j, h in enumerate(levels.get((ci + di, ck + dk), [])):
                    dd = di * di + dk * dk + ((h - p[1]) / cell) ** 2
                    n = index[((ci + di, ck + dk), j)]
                    if dd < bd and not cutn[n]:
                        best, bd = n, dd
        return best

    seg = np.linalg.norm(np.diff(main, axis=0, append=main[:1]), axis=1)

    def along(i, ft):
        """The main path point ft feet on from point i (back if negative), wrapping."""
        step_ = 1 if ft > 0 else -1
        left = abs(ft)
        while left > 0:
            j = (i + step_) % len(main)
            left -= seg[min(i, j)] if seg[min(i, j)] < 500 else 0.0   # (not the jump from the last point to the first)
            i = j
        return i

    # Other floor inside a gate's radius (ALSO COUNTS): can a car get there while that gate is next, from the floor
    # just before the gate without passing any gate's counted stretch? Then a crossing there counts the gate off its
    # own road. The cells either side of the line there are looked up in the cut floor's regions.
    if also:
        print("  other floor inside a gate's radius (a crossing there counts the gate too):")
    for k, lo, hi, y, off in also:
        e = gs[k]
        normal = np.array([-e['side'][2], 0.0, e['side'][0]])
        nb = node_at(main[along(gate_at[k], -away)])
        reach_it = False
        for f in np.linspace(lo, hi, 9):
            for side in (-1, 1):
                q = e['c'] + e['side'] * f + normal * side * cell * 2.0
                q[1] = y
                n = node_at(q)
                if n is not None and nb is not None and label[n] == label[nb] and                         abs(levels[nodes[n][0]][nodes[n][1]] - y) < 10:
                    reach_it = True
        print('    %-13s %+6.0f to %+6.0f ft along the line, height %4.0f ft (gate %4.0f), %s: %s'
              % (e['name'], lo, hi, y, e['c'][1], 'on the AI lines' if off < 60 else 'off the AI lines by %.0f ft' % off,
                 'REACHABLE while it is next' if reach_it else 'not reachable while it is next'))
    print("  ways round each gate (every gate's counted stretch cut out of the floor, %g ft grid):" % cell)
    for k, e in enumerate(gs):
        i = gate_at[k]
        ib, ia = along(i, -away), along(i, away)
        nb, na = node_at(main[ib]), node_at(main[ia])
        if nb is None or na is None:
            print('    %-13s (no floor under the path near it)' % e['name'])
            continue
        if label[nb] != label[na]:
            print('    %-13s none' % e['name'])
            continue
        # The shortest way round (breadth first over the grid, 4 ft a step), and where it crosses this gate's line.
        prev = {nb: -1}
        q = deque([nb])
        while q and na not in prev:
            n = q.popleft()
            (ci, ck), j = nodes[n]
            h = levels[(ci, ck)][j]
            for di, dk in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                for jj, hh in enumerate(levels.get((ci + di, ck + dk), [])):
                    if abs(hh - h) < step:
                        m2 = index[((ci + di, ck + dk), jj)]
                        if m2 not in prev and not cutn[m2]:
                            prev[m2] = n
                            q.append(m2)
        way = [na]
        while prev[way[-1]] >= 0:
            way.append(prev[way[-1]])
        way.reverse()
        normal = np.array([-e['side'][2], 0.0, e['side'][0]])
        pts = np.array([[cx[n], levels[nodes[n][0]][nodes[n][1]], cz[n]] for n in way])
        d = (pts - e['c']) @ normal
        lat = (pts - e['c']) @ e['side']
        cross = [(round(float(lat[i])), round(float(pts[i][1]))) for i in range(1, len(pts)) if (d[i - 1] < 0) != (d[i] < 0)]
        route = 2 * away
        game = lambda q: '(%.0f, %.0f, %.0f)' % (-q[0], q[1], q[2])
        print('    %-13s JOINED: a way round %.0f ft long (the path past it %.0f ft) from %s to %s, crossing its line at '
              '(lateral ft, height ft) %s; its middle %s' % (e['name'], len(way) * cell, route, game(pts[0]),
                                                           game(pts[-1]), cross, game(pts[len(pts) // 2])))


def overview(out, g, b, floor, lines, report):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    from matplotlib.collections import PolyCollection
    ctr = floor.mean(1)
    # Off the route: over 60 ft from every AI line point (3D).
    off = np.array([np.linalg.norm(lines - c, axis=1).min() > 60 for c in ctr])
    fig, ax = plt.subplots(figsize=(14, 14))
    for sel, col in ((~off, '#b0b0b0'), (off, '#ff9900')):
        ax.add_collection(PolyCollection([np.c_[-t[:, 0], t[:, 2]] for t in floor[sel]], facecolors=col, edgecolors='none'))
    ax.plot(-lines[:, 0], lines[:, 2], 'b.', ms=0.6)
    lo, hi = ctr.min(0), ctr.max(0)
    for e, r, st, missed in report:
        c, sd = e['c'], e['side']
        for s0, s1, y in st:
            p0, p1 = c + sd * s0, c + sd * s1
            ax.plot([-p0[0], -p1[0]], [p0[2], p1[2]], '-', color='#00aa00', lw=1.5)
        for s0, s1, y in missed:
            p0, p1 = c + sd * s0, c + sd * s1
            ax.plot([-p0[0], -p1[0]], [p0[2], p1[2]], '-', color='#ff00ff', lw=3)
        ax.add_patch(plt.Circle((-c[0], c[2]), r, fill=False, color='g'))
        ax.annotate(e['name'].replace('CHECKPOINT_', 'CP'), (-c[0], c[2]), color='k', fontsize=12, weight='bold')
    ax.set_xlim(-hi[0] - 100, -lo[0] + 100)
    ax.set_ylim(lo[2] - 100, hi[2] + 100)
    ax.set_aspect('equal')
    plt.tight_layout()
    plt.savefig(out, dpi=60)


def plot(out, g, b, floor, report):
    import matplotlib
    matplotlib.use('Agg')
    import matplotlib.pyplot as plt
    wall = tris(g, b, ['COL_WALL'])
    path = tris(g, b, ['AIPATH_main']).reshape(-1, 3)
    mx = lambda v: -v  # game handedness
    fig, axs = plt.subplots(1, len(report), figsize=(7 * len(report), 7))
    for ax, (e, r, st, missed) in zip(np.atleast_1d(axs), report):
        c, sd = e['c'], e['side']
        R = max(450.0, r * 1.3, max([max(abs(s0), abs(s1)) for s0, s1, _ in st] + [0]) * 1.1)
        R = min(R, 1500.0)
        for T, col in ((floor, '#c8c8c8'), (wall, '#cc3333')):
            ctr = T.mean(1)
            sel = (np.abs(ctr[:, 0] - c[0]) < R) & (np.abs(ctr[:, 2] - c[2]) < R)
            for t in T[sel]:
                ax.fill(mx(t[:, 0]), t[:, 2], color=col, lw=0)
        ax.plot(mx(path[:, 0]), path[:, 2], 'b.', ms=1.5)
        for s0, s1, y in st:
            p0, p1 = c + sd * s0, c + sd * s1
            ax.plot(mx(np.array([p0[0], p1[0]])), [p0[2], p1[2]], '-', color='#00aa00', lw=3)
        for s0, s1, y in missed:
            p0, p1 = c + sd * s0, c + sd * s1
            ax.plot(mx(np.array([p0[0], p1[0]])), [p0[2], p1[2]], '-', color='#ff00ff', lw=4)
        ax.add_patch(plt.Circle((mx(c[0]), c[2]), r, fill=False, color='g'))
        ax.set_xlim(mx(c[0]) - R, mx(c[0]) + R)
        ax.set_ylim(c[2] - R, c[2] + R)
        ax.set_aspect('equal')
        ax.set_title('%s: radius %.0f ft, %d uncounted' % (e['name'], r, len(missed)))
    plt.tight_layout()
    plt.savefig(out, dpi=55)


if __name__ == '__main__':
    main()
