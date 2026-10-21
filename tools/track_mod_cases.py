#!/usr/bin/env python3
"""Custom-track converter case suite: builds a synthetic track (track.glb + track.toml) for every road layout and
authoring mistake the converter's race direction, start and checkpoint-crossing logic has to handle, converts each with
tools/track_mod_test.bat, and checks the report and the crossings written into path.bin / path_backward.bin against the
ones the layout was built with (docs/custom_tracks.md, "Checkpoint crossings").

    python tools/track_mod_cases.py [OUT_DIR] [--only NAME[,NAME...]] [--list]

OUT_DIR defaults to tmp/track_mod_cases. Each case prints PASS or FAIL with its reasons, and "game search" when Rush 2's
own load-time search (func_80092D6C, which custom tracks skip) would have put a crossing elsewhere: that is what the
case guards against. Layouts are built in game feet (y up) along a center line in race order; the AI path ribbon, the
road (COL_ROAD, drawn too) and the markers are made from it, then a case's mistakes are applied.
"""
import math, os, random, re, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
from stock_track_glb import Glb, gate_matrix, to_gltf, FEET_PER_METER  # noqa: E402
import paths  # noqa: E402

STEP = 10.0          # ft between center line samples (and ribbon rungs)
SPINE = 20.0         # the converter's spine spacing
ROAD_HALF = 50.0     # road half width, ft
RIBBON_HALF = 35.0   # AIPATH_main half width, ft
GATE_HALF = 50.0     # a marker's default half width, ft
EXE = os.path.join(REPO, 'tmp', 'track_mod_test', 'track_mod_test.exe')
SOURCES = [os.path.join(REPO, p) for p in ('src/track_mod/track_mod_convert.cpp', 'tools/track_mod_test.cpp',
                                            'src/gltf_scene.cpp', 'include/track_mod.h')]


# --- center lines (closed, race order, game feet) -------------------------------------------------------------------

def chaikin(pts, n=4):
    for _ in range(n):
        out = []
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            out.append(tuple(a[k] * 0.75 + b[k] * 0.25 for k in range(3)))
            out.append(tuple(a[k] * 0.25 + b[k] * 0.75 for k in range(3)))
        pts = out
    return pts


class Line:
    """A closed center line resampled every STEP ft; position, tangent at a distance s along it."""

    def __init__(self, pts):
        cum = [0.0]
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            cum.append(cum[-1] + math.dist(a, b))
        self.raw, self.cum, self.length = pts, cum, cum[-1]

    def at(self, s):
        s %= self.length
        lo, hi = 0, len(self.raw)
        while hi - lo > 1:
            mid = (lo + hi) // 2
            if self.cum[mid] <= s:
                lo = mid
            else:
                hi = mid
        a, b = self.raw[lo], self.raw[(lo + 1) % len(self.raw)]
        f = (s - self.cum[lo]) / max(1e-9, self.cum[lo + 1] - self.cum[lo])
        return tuple(a[k] + (b[k] - a[k]) * f for k in range(3))

    def tangent(self, s):
        a, b = self.at(s - 2.0), self.at(s + 2.0)
        d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
        n = math.sqrt(sum(c * c for c in d)) or 1.0
        return tuple(c / n for c in d)

    def lateral(self, s):
        t = self.tangent(s)
        n = math.hypot(t[0], t[2]) or 1.0
        return (-t[2] / n, 0.0, t[0] / n)


def oval():
    pts = []
    for i in range(48):
        a = i / 48 * 2 * math.pi
        pts.append((900 * math.cos(a), 0.0, -500 * math.sin(a)))
    return Line(chaikin(pts, 3))


def oval_dip():
    """The oval with a smooth dip 20 ft deep, about 400 ft long, at 45% of the lap."""
    pts = []
    for i in range(96):
        a = i / 96 * 2 * math.pi
        y = -20.0 * math.exp(-((a - 0.45 * 2 * math.pi) / 0.18) ** 2)
        pts.append((900 * math.cos(a), y, -500 * math.sin(a)))
    return Line(chaikin(pts, 2))


def serpentine():
    """Three parallel legs 130 ft apart (A +x, B -x, C +x), then a return loop round the outside: a compact track."""
    L = 1500
    pts = [(0, 0, 0), (L, 0, 0), (L + 90, 0, 65), (L, 0, 130), (0, 0, 130), (-90, 0, 195), (0, 0, 260), (L, 0, 260),
           (L + 400, 0, 450), (L, 0, 700), (0, 0, 700), (-500, 0, 400), (-500, 0, -250), (-200, 0, -150)]
    return Line(chaikin([tuple(map(float, p)) for p in pts], 4))


def figure8(bridge):
    """A lemniscate crossing itself at the origin; with bridge, the t = 0 pass is 50 ft above the t = pi pass."""
    pts = []
    for i in range(400):
        t = i / 400 * 2 * math.pi
        y = 50.0 * (1 + math.cos(t)) / 2 if bridge else 0.0
        pts.append((900 * math.sin(t), y, 900 * math.sin(t) * math.cos(t)))
    return Line(pts)


def spiral():
    """Two turns spiralling in from r 520 to 440, then a bridge back out over the turn between: legs 40 ft apart side
    by side, running the same way."""
    pts = []
    n = 600
    for i in range(n):
        t = i / n * 4 * math.pi
        r = 520 - 80 * t / (4 * math.pi)
        ramp = min(1.0, min(t, 4 * math.pi - t) / 0.6)   # up to the bridge at either end
        pts.append((r * math.cos(t), 50.0 * (1 - ramp), r * math.sin(t)))
    return Line(pts)


def helix():
    """Two turns of a helix rising 60 ft a turn (the second right above the first, the same way), then down round the
    outside back to the start."""
    pts = []
    for i in range(500):
        t = i / 500 * 4 * math.pi
        pts.append((500 * math.cos(t), 60 * t / (2 * math.pi), 500 * math.sin(t)))
    for i in range(1, 300):
        u = i / 300
        a = 4 * math.pi + u * 2 * math.pi                  # once more round, outside, coming down
        r = 500 + 450 * math.sin(math.pi * u)
        pts.append((r * math.cos(a), 120 * (1 - u), r * math.sin(a)))
    return Line(pts)


def vertical_loop():
    """An oval whose straight has a vertical loop (radius 45 ft, top going backwards 90 ft up), coming out a road's
    width (110 ft) to the side, as loops do."""
    pts = []
    for i in range(48):
        a = i / 48 * 2 * math.pi
        pts.append((900 * math.cos(a), 0.0, -500 * math.sin(a)))
    base = chaikin(pts, 3)
    # Bottom straight (z = -500 side? the oval runs clockwise seen from above); splice the loop in at the point nearest
    # (0, 0, 500).
    k = min(range(len(base)), key=lambda i: math.dist(base[i], (0, 0, 500)))
    p, q = base[k], base[(k + 1) % len(base)]
    d = (q[0] - p[0], q[2] - p[2])
    n = math.hypot(*d)
    d = (d[0] / n, d[1] / n)
    lat = (-d[1], d[0])
    loop = []
    for i in range(1, 120):
        t = i / 120 * 2 * math.pi
        fwd = 45 * math.sin(t) + 10 * t / (2 * math.pi)
        side = 110 * t / (2 * math.pi)
        loop.append((p[0] + d[0] * fwd + lat[0] * side, 45 * (1 - math.cos(t)), p[2] + d[1] * fwd + lat[1] * side))
    rest = [(x + lat[0] * 110, y, z + lat[1] * 110) for (x, y, z) in base[k + 1:] + base[:k]]
    # Ease the 110 ft shift back out over the rest of the lap.
    m = len(rest)
    rest = [(x - lat[0] * 110 * (j / m), y, z - lat[1] * 110 * (j / m)) for j, (x, y, z) in enumerate(rest)]
    return Line([p] + loop + rest)


# --- a case --------------------------------------------------------------------------------------------------------

class Gate:
    def __init__(self, name, s, half=GATE_HALF, arrow=1, side=0.0, up=0.0, exact=False):
        self.name, self.s, self.half, self.arrow, self.side, self.up, self.exact = name, s, half, arrow, side, up, exact


class Case:
    """line: the layout; gates: FINISH first, at distances along the line (race order); start: where the ribbon begins;
    reverse: the ribbon drawn against the race; overshoot: ft the ribbon's end runs past its start (negative: an open
    curve with that gap); kinks: distances where the ribbon steps back 4 ft; dupes: distances where a rung repeats;
    errors / warnings: regexes the report must hold (any other ERROR fails, and so does any other warning about the
    markers or the race direction); allow: regexes of errors or warnings that may appear; wrong: gates whose crossing
    isn't checked (the case expects an error for them, or either outcome is right); gaps: (from, to) distances with
    no road built (a jump's gap); floors: (from, to, depth) road built that far below the line there (ground under a
    jump); lift: s -> ft the ribbon is drawn above the road (a curve not following the road's height); heights:
    (s, 'curve' | 'road') the spine's height there must be the curve's (kept in the air) or the road's (dropped);
    export: how AIPATH_main's mesh is written (ribbon_mesh); nopath: the path files' crossings aren't checked (a
    broken or missing AIPATH_main or FINISH: the track still loads, on a stand-in path); ribbon: False leaves
    AIPATH_main out; visual: False leaves the visible mesh out (only COL_ROAD); movers: adds the MOVE_ objects of
    mover_defs and checks their conversion (check_movers); lanes: {k: (offset(s), reverse, used)} paints
    AIPATH_lane<k> at offset(s) ft left of the line (reverse: drawn against the race), and the path files' four lanes
    must be those (used: False, the converter has to leave that lane offset from AIPATH_main) and the offset ones;
    objects: adds the billboards, props, translucent card, far mover and AI path values of object_defs and checks them
    (check_objects); splines: the road is spline road, a ROAD_SPLINES segment per road piece with a rise along it
    (spline_segments), and check_splines checks the file's SEG section, flags and segment indices."""

    def __init__(self, name, line, gates, start=0.0, reverse=False, overshoot=0.0, kinks=(), dupes=(), errors=(),
                 warnings=(), allow=(), wrong=(), gaps=(), floors=(), lift=None, heights=(), export='pairs',
                 nopath=False, ribbon=True, visual=True, movers=False, lanes=None, objects=False, splines=False,
                 scripts=False, note=''):
        self.__dict__.update(locals())
        del self.__dict__['self']


def rungs_of(c):
    L = c.line.length
    count = int(round(L / STEP))
    step = L / count
    ss = [c.start + k * step for k in range(count)]
    if c.overshoot > 0:
        ss.append(c.start + L + c.overshoot)
    elif c.overshoot < 0:
        ss = [s for s in ss if s - c.start <= L + c.overshoot]
    # Exactly-on-a-rung markers: snap them onto a sample.
    for g in c.gates:
        if g.exact:
            k = min(range(len(ss)), key=lambda i: abs(((ss[i] - g.s + L / 2) % L) - L / 2))
            g.s = ss[k] % L
    out = []
    for s in ss:
        p, lat = c.line.at(s), c.line.lateral(s)
        if c.lift:
            p = (p[0], p[1] + c.lift(s), p[2])
        out.append((s, p, lat))
        if any(abs(((s - k + L / 2) % L) - L / 2) < step / 2 for k in c.dupes):
            out.append((s, p, lat))
        for k in c.kinks:
            if abs(((s - k + L / 2) % L) - L / 2) < step / 2:
                t = c.line.tangent(s)
                out.append((s, tuple(p[i] - t[i] * 4.0 for i in range(3)), lat))
    if c.reverse:
        out.reverse()
    return out


def ribbon_mesh(c):
    """AIPATH_main's vertices (glTF), triangles and node matrix, written the way c.export says an exporter would:
    pairs: one left/right vertex pair per rung in order, open (the suite's plain ribbon); blender_cyclic: Blender's
    glTF export of a cyclic curve, a closed strip whose UV seam stores the first rung's two vertices twice each (the
    copies first, then the rest in pairs, the last quad using the copies); welded: a closed strip sharing the first
    rung's vertices, no seam; flat: every triangle with its own three vertices (flat shading); shuffled: pairs stored
    in a random order; scaled: pairs under a node scaled 4x (vertices at a quarter); on_edge: the ribbon standing up
    (a flat-drawn curve at Tilt 0); thin: 0.6 ft across; tube: a closed square tube (Bevel Depth); pinch: pairs with
    one rung pinched to a point (as one of Atari's stock rungs is)."""
    rungs = rungs_of(c)
    half = 0.3 if c.export == 'thin' else RIBBON_HALF
    side = []
    for k, (_, p, lat) in enumerate(rungs):
        if c.export == 'on_edge':
            lat = (0.0, 1.0, 0.0)
        if c.export == 'pinch' and k == len(rungs) // 3:
            lat = (0.0, 0.0, 0.0)
        side.append((tuple(p[i] - lat[i] * half for i in range(3)), tuple(p[i] + lat[i] * half for i in range(3))))
    n = len(side)
    quad = lambda a0, b0, a1, b1: [(a0, b0, a1), (b0, b1, a1)]
    if c.export == 'tube':
        verts, tris = [], []
        for a, b in side:
            verts += [a, b, (b[0], b[1] + 2 * half, b[2]), (a[0], a[1] + 2 * half, a[2])]
        for k in range(n):
            j = (k + 1) % n
            for e in range(4):
                tris += quad(4 * k + e, 4 * k + (e + 1) % 4, 4 * j + e, 4 * j + (e + 1) % 4)
        return [to_gltf(v) for v in verts], tris, None
    if c.export in ('blender_cyclic', 'welded'):
        # Blender: v0, v1 = a0 (u 0 and 1), v2, v3 = b0, then a_k, b_k; the closing quad uses the u = 1 copies.
        if c.export == 'blender_cyclic':
            verts = [side[0][0], side[0][0], side[0][1], side[0][1]]
            ia, ib, ia_end, ib_end = [0], [2], 1, 3
        else:
            verts = [side[0][0], side[0][1]]
            ia, ib, ia_end, ib_end = [0], [1], 0, 1
        for a, b in side[1:]:
            ia.append(len(verts))
            verts.append(a)
            ib.append(len(verts))
            verts.append(b)
        tris = []
        for k in range(n):
            if k + 1 < n:
                tris += quad(ia[k], ib[k], ia[k + 1], ib[k + 1])
            else:
                tris += quad(ia[k], ib[k], ia_end, ib_end)
        return [to_gltf(v) for v in verts], tris, None
    verts = [v for ab in side for v in ab]
    tris = []
    for k in range(n - 1):
        tris += quad(2 * k, 2 * k + 1, 2 * k + 2, 2 * k + 3)
    matrix = None
    if c.export == 'flat':
        verts = [verts[i] for t in tris for i in t]
        tris = [(3 * i, 3 * i + 1, 3 * i + 2) for i in range(len(tris))]
    elif c.export == 'shuffled':
        order = list(range(len(verts)))
        random.Random(7).shuffle(order)
        where = {old: new for new, old in enumerate(order)}
        verts = [verts[i] for i in order]
        tris = [tuple(where[i] for i in t) for t in tris]
    elif c.export == 'scaled':
        verts = [tuple(x * 0.25 for x in v) for v in verts]
        matrix = [4.0, 0, 0, 0, 0, 4.0, 0, 0, 0, 0, 4.0, 0, 0, 0, 0, 1.0]
    return [to_gltf(v) for v in verts], tris, matrix


def lane_mesh(c, offset, reverse):
    """A painted lane's ribbon (2 ft wide, pairs, open with its ends a step apart): its center offset(s) ft left of
    the line."""
    L = c.line.length
    count = int(round(L / STEP))
    side = []
    for k in range(count):
        s = k * L / count
        p, lat = c.line.at(s), c.line.lateral(s)
        m = tuple(p[i] + lat[i] * offset(s) for i in range(3))
        side.append((tuple(m[i] - lat[i] for i in range(3)), tuple(m[i] + lat[i] for i in range(3))))
    if reverse:
        side.reverse()
    verts = [to_gltf(v) for ab in side for v in ab]
    tris = []
    for k in range(len(side) - 1):
        tris += [(2 * k, 2 * k + 1, 2 * k + 2), (2 * k + 1, 2 * k + 3, 2 * k + 2)]
    return verts, tris


def check_lanes(c, fwd, back, fails):
    """Every lane point's offset across the line, in slot order, is the painted or offset lanes left to right (lane 1
    first) or right to left, in both path files (the backward one is the same road: point i is forward n - i)."""
    L = c.line.length
    finish = c.gates[0].s
    for label, data in (('forward', fwd), ('backward', back)):
        n = len(data['spine'])
        bad, order = 0, None
        for i in range(n):
            f = i if label == 'forward' else (n - i) % n
            p = data['spine'][i]
            # The spine point's own distance along the line (the spine's steps drift from SPINE along the chords).
            s = min((finish + f * SPINE + d * 0.5 for d in range(-160, 161)),
                    key=lambda t: (c.line.at(t)[0] - p[0]) ** 2 + (c.line.at(t)[2] - p[2]) ** 2)
            want = []
            for k, frac in enumerate((0.6, 0.2, -0.2, -0.6)):
                lane = (c.lanes or {}).get(k + 1)
                want.append(lane[0](s) if lane and lane[2] else frac * RIBBON_HALF)
            lat = c.line.lateral(s)
            got = [(q[0] - p[0]) * lat[0] + (q[2] - p[2]) * lat[2] for q in (ln['points'][i] for ln in data['lanes'])]
            ok = [o for o in ('ltr', 'rtl') if all(abs(a - b) < 2.5 for a, b in zip(got, want if o == 'ltr' else want[::-1]))]
            if not ok or (order and order not in ok):
                if not bad:
                    fails.append('%s lanes at point %d: %s, want %s' % (label, i, ['%.1f' % v for v in got],
                                                                         ['%.1f' % v for v in want]))
                bad += 1
            elif not order and len(ok) == 1:
                order = ok[0]
        if bad > 1:
            fails.append('%s lanes wrong at %d of %d points' % (label, bad, n))


def build(c, folder):
    g = Glb()
    L = c.line.length
    n = int(round(L / STEP))
    verts, nrms, tris, segv = [], [], [], []
    inside = lambda s, a, b: (s - a) % L < (b - a) % L
    pieces = [(k, 0.0) for k in range(n) if not any(inside((k + 0.5) * L / n, a, b) for a, b in c.gaps)]
    pieces += [(k, d) for a, b, d in c.floors for k in range(n) if inside((k + 0.5) * L / n, a, b)]
    for k, depth in pieces:
        s0, s1 = k * L / n, (k + 1) * L / n
        quad = []
        for s in (s0, s1):
            p, lat = c.line.at(s), c.line.lateral(s)
            p = (p[0], p[1] - depth, p[2])
            quad.append(tuple(p[i] - lat[i] * ROAD_HALF for i in range(3)))
            quad.append(tuple(p[i] + lat[i] * ROAD_HALF for i in range(3)))
        t, lat = c.line.tangent(s0), c.line.lateral(s0)
        up = (lat[1] * t[2] - lat[2] * t[1], lat[2] * t[0] - lat[0] * t[2], lat[0] * t[1] - lat[1] * t[0])
        for tri in ((quad[0], quad[1], quad[2]), (quad[1], quad[3], quad[2])):
            e1 = [tri[1][i] - tri[0][i] for i in range(3)]
            e2 = [tri[2][i] - tri[0][i] for i in range(3)]
            cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            if sum(cr[i] * up[i] for i in range(3)) > 0:   # the converter's front is -cross (stock_track_glb.py)
                tri = (tri[0], tri[2], tri[1])
            base = len(verts)
            verts += [to_gltf(v) for v in tri]
            nrms += [(-up[0], up[1], up[2])] * 3
            segv += [float(k)] * 3
            tris.append((base, base + 1, base + 2))
    g.mesh('COL_ROAD_case', verts, nrms, tris, attrs={'_SEGMENT': segv} if c.splines else None)
    if c.splines:
        g.empty('ROAD_SPLINES')
        g.js['nodes'][-1]['extras'] = {'segments': spline_segments(c, n)}
    if c.visual:
        g.mesh('TRACK_visual', verts, nrms, tris)
    if c.ribbon:
        rv, rt, matrix = ribbon_mesh(c)
        attrs = {'_SPEED': [float(OBJ_SPEED)] * len(rv), '_AI_MODE': [float(OBJ_MODE)] * len(rv)} if c.objects else None
        g.mesh('AIPATH_main', rv, [(0.0, 1.0, 0.0)] * len(rv), rt, matrix, attrs=attrs)
    for k, (offset, reverse, _) in (c.lanes or {}).items():
        lv, lt = lane_mesh(c, offset, reverse)
        g.mesh('AIPATH_lane%d' % k, lv, [(0.0, 1.0, 0.0)] * len(lv), lt)
    for gate in c.gates:
        p, t, lat = c.line.at(gate.s), c.line.tangent(gate.s), c.line.lateral(gate.s)
        pos = (p[0] + lat[0] * gate.side, p[1] + gate.up, p[2] + lat[2] * gate.side)
        d = (t[0] * gate.arrow, 0.0, t[2] * gate.arrow)
        g.empty(gate.name, gate_matrix(pos, d, gate.half))
    toml = build_movers(g, c) if c.movers else ''
    toml += build_objects(g, c) if c.objects else ''
    toml += build_scripts(g, c) if c.scripts else ''
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'track.glb'), 'wb') as f:
        f.write(g.bytes())
    with open(os.path.join(folder, 'track.toml'), 'w', newline='\n') as f:
        f.write('[track]\nname = "%s"\n' % c.name + toml)


def convert(folder):
    out = os.path.join(folder, 'out')
    fresh = os.path.exists(EXE) and all(os.path.getmtime(EXE) > os.path.getmtime(s) for s in SOURCES)
    cmd = [EXE, folder, out] if fresh else ['cmd', '/c', os.path.join(HERE, 'track_mod_test.bat'), folder, out]
    res = subprocess.run(cmd, capture_output=True, text=True, cwd=REPO)
    return out, res.stdout + res.stderr


def check(c, folder):
    out, text = convert(folder)
    report = open(os.path.join(out, 'report.txt'), encoding='utf-8').read() if os.path.exists(os.path.join(out, 'report.txt')) else text
    fails, notes = [], []
    errs = re.findall(r'^ERROR: (.*)$', report, re.M)
    warns = re.findall(r'^Warning: (.*)$', report, re.M)
    for rx in c.errors:
        if not any(re.search(rx, e) for e in errs):
            fails.append('missing error /%s/' % rx)
    for e in errs:
        if not any(re.search(rx, e) for rx in list(c.errors) + list(c.allow)):
            fails.append('unexpected ERROR: ' + e[:150])
    for rx in c.warnings:
        if not any(re.search(rx, w) for w in warns):
            fails.append('missing warning /%s/' % rx)
    marker_rx = r'FINISH|CHECKPOINT_|AIPATH_main runs|arrow'
    for w in warns:
        if re.search(marker_rx, w) and not any(re.search(rx, w) for rx in list(c.warnings) + list(c.allow)):
            fails.append('unexpected warning: ' + w[:150])
    pf = os.path.join(out, 'path.bin')
    if not os.path.exists(pf):
        fails.append('no path.bin written')
        return fails, notes
    if c.nopath:
        return fails, notes
    L = c.line.length
    finish = c.gates[0].s
    fwd = paths.parse(open(pf, 'rb').read())
    back = paths.parse(open(os.path.join(out, 'path_backward.bin'), 'rb').read())
    n = len(fwd['spine'])
    # Race direction and start: spine point 0 on the finish line, point 1 ahead of it along the race.
    p0, p1, fp, ft = fwd['spine'][0], fwd['spine'][1], c.line.at(finish), c.line.tangent(finish)
    if (p1[0] - p0[0]) * ft[0] + (p1[2] - p0[2]) * ft[2] <= 0:
        fails.append('spine runs against the race')
    if math.hypot(p0[0] - fp[0], p0[2] - fp[2]) > 15:
        fails.append('spine starts %.0f ft from the finish line' % math.hypot(p0[0] - fp[0], p0[2] - fp[2]))
    names = [g.name for g in c.gates]
    order = [c.gates[0]] + sorted(c.gates[1:], key=lambda g: int(g.name.split('_')[1]))
    for i, g in enumerate(order):
        if g.name in c.wrong:
            continue
        q = (g.s - finish) % L
        want = 0 if i == 0 else (int(q / (L / n)) + 1) % n
        cp = fwd['cps'][i]
        for slot in range(5):
            got = cp['cross'][slot]
            if min(abs(got - want), n - abs(got - want)) > 2:
                fails.append('%s forward slot %d crossing %d, want ~%d' % (g.name, slot, got, want))
                break
        bi = 0 if i == 0 else len(order) - i
        bwant = 0 if want == 0 else (n - want + 1) % n
        bgot = back['cps'][bi]['cross'][0]
        if min(abs(bgot - bwant), n - abs(bgot - bwant)) > 2:
            fails.append('%s backward crossing %d, want ~%d' % (g.name, bgot, bwant))
        game = paths.crossing(cp, fwd['spine'])
        if min(abs(game - want), n - abs(game - want)) > 2:
            notes.append('game search: %s at %d (right: %d)' % (g.name, game, want))
    for s, kind in c.heights:
        q = (s - finish) % L
        k = int(round(q / (L / n))) % n
        road = c.line.at(s)[1]
        want = road + (c.lift(s) if c.lift else 0.0) if kind == 'curve' else road
        got = fwd['spine'][k][1]
        if abs(got - want) > 4:
            fails.append('spine height at %.0f ft along: %d, want the %s height %.0f' % (q, got, kind, want))
    del names
    if c.lanes:
        check_lanes(c, fwd, back, fails)
    if c.movers:
        check_movers(c, out, report, fails)
    if c.objects:
        check_objects(c, out, report, fails, fwd)
    if c.splines:
        check_splines(c, out, report, fails)
    if c.scripts:
        check_scripts(c, out, report, fails)
    return fails, notes


# --- scripted moving objects, trigger boxes, covered collision (scripts=True) -------------------------------------------
# A gate box on the road (TRIGGER_GATE) and an unused one, five scripted movers beside the road (a door opened by the
# gate that closes again 5 s later, a door that starts open, a once that starts 3 s in, a pingpong started by the gate on
# lap 2 and a loop started by odd laps, which is also a moving billboard with collision), and a covered patch of road
# (COL_ROAD_tunnel, custom property covered = 1). check_scripts reads the SCRP / TRIG chunks, the billboard's record
# flag and the covered polygons' info bits.
def script_defs(c):
    L = c.line.length

    def beside(s, side=90.0):
        p, lat = c.line.at(s), c.line.lateral(s)
        return to_gltf((p[0] + lat[0] * side, p[1], p[2] + lat[2] * side))

    pts = [beside(k * 0.1 * L) for k in (1, 2, 3, 4, 6)]
    up = lambda p, h: (p[0], p[1] + h, p[2])
    return [
        dict(name='door', toml='mode = "door"\nopen = "trigger gate"\nclose_delay = 5\n', mode=4, open_at_start=0,
             events=[[], [(1, 0, 0)], []], delays=(-1.0, 5.0), at=pts[0], T=[pts[0], up(pts[0], 4)]),
        dict(name='shut', toml='mode = "door"\nclose = ["trigger GATE", "time 30"]\nopen_at_start = true\nopen_delay = 2.5\n',
             mode=4, open_at_start=1, events=[[], [], [(1, 0, 0), (0, -1, 30.0)]], delays=(2.5, -1.0), at=pts[1],
             T=[pts[1], up(pts[1], -4)]),
        dict(name='drop', toml='mode = "once"\nstart = "time 3"\n', mode=3, open_at_start=0,
             events=[[(0, -1, 3.0)], [], []], delays=(-1.0, -1.0), at=pts[2], T=[pts[2], up(pts[2], -6)]),
        dict(name='train', toml='mode = "pingpong"\nstart = "trigger gate lap 2"\n', mode=2, open_at_start=0,
             events=[[(1, 0, 0)], [], []], lap=2, delays=(-1.0, -1.0), at=pts[3], T=[pts[3], up(pts[3], 3)]),
        dict(name='card', toml='mode = "loop"\nstart = "odd_lap"\n', mode=1, open_at_start=0,
             events=[[(2, -1, 0)], [], []], delays=(-1.0, -1.0), at=pts[4], T=[pts[4], up(pts[4], 2), pts[4]],
             billboard=True),
    ]


def build_scripts(g, c):
    toml = ''
    for m in script_defs(c):
        node = g.empty('MOVE_' + m['name'], trs=(m['at'], (0, 0, 0, 1), (1, 1, 1)))
        if m.get('billboard'):
            g.js['nodes'][node]['extras'] = {'billboard': True}
        verts, nrms, tris = box(2.0, 2.0)
        g.mesh('MOVEVIS_' + m['name'], verts, nrms, tris, parent=node)
        g.mesh('COL_WALL_' + m['name'], verts, nrms, tris, parent=node)
        g.animate('MOVE_%sAction' % m['name'], node, [float(k) * 2 for k in range(len(m['T']))], m['T'], None, None)
        toml += '\n[movers.%s]\n%s' % (m['name'], m['toml'])
    # The gate: a box over the road at 5 % of the lap; and one no mover names.
    for name, s in (('TRIGGER_gate', 0.05), ('TRIGGER_unused', 0.5)):
        p = c.line.at(s * c.line.length)
        verts, nrms, tris = box(20.0, 10.0)
        g.mesh(name, [(v[0] + to_gltf(p)[0], v[1] + to_gltf(p)[1], v[2] + to_gltf(p)[2]) for v in verts], nrms, tris)
    # A covered patch beside the road: COL_ROAD_tunnel with covered = 1.
    p = to_gltf(c.line.at(0.7 * c.line.length))
    verts, nrms, tris = floor(10.0)
    g.mesh('COL_ROAD_tunnel', [(v[0] + p[0] + 40.0, v[1] + p[1], v[2] + p[2]) for v in verts], nrms, tris)
    g.js['nodes'][-1]['extras'] = {'covered': 1}
    return toml


def check_scripts(c, out, report, fails):
    import collision
    pb = open(os.path.join(out, 'mover_paths.bin'), 'rb').read() if os.path.exists(os.path.join(out, 'mover_paths.bin')) else b''
    if not pb:
        fails.append('no mover_paths.bin')
        return
    be32 = lambda d, o: struct.unpack_from('>I', d, o)[0]
    chunks = {}
    d = be32(pb, 0)
    while d + 12 <= len(pb) and pb[d:d + 4].isalpha():
        chunks[pb[d:d + 4].decode()] = (be32(pb, d + 4), be32(pb, d + 8))
        d += 12
    if 'SCRP' not in chunks or 'TRIG' not in chunks:
        fails.append('mover file chunks %s, want PTHD, SCRP and TRIG' % sorted(chunks))
        return
    defs = script_defs(c)
    o, count = chunks['SCRP']
    if count != len(defs):
        fails.append('SCRP has %d scripts, want %d' % (count, len(defs)))
        return
    for m in defs:
        mode, flags, n0, n1, n2 = pb[o], pb[o + 1], pb[o + 2], pb[o + 3], pb[o + 4]
        od, cd = struct.unpack_from('>ff', pb, o + 8)
        o += 16
        got = []
        for n in (n0, n1, n2):
            evs = []
            for _ in range(n):
                kind, box_, lap, secs = pb[o], struct.unpack_from('>h', pb, o + 2)[0], struct.unpack_from('>h', pb, o + 4)[0], \
                    struct.unpack_from('>f', pb, o + 8)[0]
                evs.append((kind, box_, round(secs, 3)))
                if kind == 1 and lap != m.get('lap', 0):
                    fails.append('movers.%s: trigger lap %d, want %d' % (m['name'], lap, m.get('lap', 0)))
                o += 12
            got.append(evs)
        if mode != m['mode'] or flags != m['open_at_start']:
            fails.append('movers.%s: script mode %d flags %d, want %d %d' % (m['name'], mode, flags, m['mode'], m['open_at_start']))
        if got != [[tuple(e) for e in l] for l in m['events']]:
            fails.append('movers.%s: events %s, want %s' % (m['name'], got, m['events']))
        if (round(od, 3), round(cd, 3)) != m['delays']:
            fails.append('movers.%s: delays %s, want %s' % (m['name'], (od, cd), m['delays']))
    o, count = chunks['TRIG']
    if count != 2:
        fails.append('TRIG has %d boxes, want 2' % count)
    if not re.search(r'TRIGGER_UNUSED isn.t used', report):
        fails.append('no warning for the unused TRIGGER_unused')
    # The moving billboard's placement record: node flag 0x8000.
    placement = open(os.path.join(out, 'placement.bin'), 'rb').read()
    records = [tuple(map(int, l.split())) for l in open(os.path.join(out, 'mover_records.txt')).read().split('\n') if l.strip()]
    for j, m in enumerate(defs):
        rec = [r for r in records if r[0] == j]
        if not rec:
            fails.append('MOVE_%s has no placement record' % m['name'])
            continue
        flag = be32(placement, 0x18 + rec[0][2] * 0x64 + 0x40)
        if bool(flag & 0x8000) != bool(m.get('billboard')):
            fails.append('MOVE_%s record flags %#x (billboard %s)' % (m['name'], flag, bool(m.get('billboard'))))
    # Covered polygons: info bit 14 set on COL_ROAD_tunnel's, and only there.
    coll = collision.Collision.parse(open(os.path.join(out, 'collision.bin'), 'rb').read(), collision.R2)
    covered = [p for p in coll.polys if p['info'] & 0xC000]
    if not covered or any(p['info'] & 0xC000 != 0x4000 for p in covered):
        fails.append('covered polygons: %d, info %s, want COL_ROAD_tunnel\'s with 0x4000' %
                     (len(covered), sorted({hex(p['info'] & 0xC000) for p in covered})))
    if len(covered) > 2:
        fails.append('%d covered polygons, want COL_ROAD_tunnel\'s (1 or 2)' % len(covered))


# --- moving objects (MOVE_*) ------------------------------------------------------------------------------------------
# A case with movers=True gets four animated objects beside the road (outside its 50 ft half width), each testing a
# different part of the animation -> 2049 path conversion; check_movers() then replays every key with Rush 2049's own
# pose arithmetic (movers::quat_to_matrix, rows scaled, a point times the matrix) and compares it with the glTF pose.

def quat_axis(axis, deg):
    n = math.sqrt(sum(a * a for a in axis))
    h = math.radians(deg) / 2
    return tuple(a / n * math.sin(h) for a in axis) + (math.cos(h),)


def trs_matrix(t, q, s):
    """glTF TRS -> column-major 4x4 (as src/gltf_scene.cpp node_matrix)."""
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    r = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]
    m = [0.0] * 16
    for c in range(3):
        for k in range(3):
            m[c * 4 + k] = r[c * 3 + k] * s[c]
    m[12:15] = list(t)
    m[15] = 1.0
    return m


def mat_mul(a, b):
    return [sum(a[k * 4 + r] * b[c * 4 + k] for k in range(4)) for c in range(4) for r in range(4)]


def mat_point(m, v):
    return tuple(m[r] * v[0] + m[4 + r] * v[1] + m[8 + r] * v[2] + m[12 + r] for r in range(3))


def quat_to_matrix_2049(q):
    """movers::quat_to_matrix (func_800BFBE8): row-major, elements 1, 3, 5, 7 negated."""
    x, y, z, w = q
    xs, ys, zs = x * 2, y * 2, z * 2
    xx, yy, zz = x * xs, y * ys, z * zs
    xy, wz, yz, wx, xz, wy = x * ys, w * zs, y * zs, w * xs, x * zs, w * ys
    return [1 - (yy + zz), -(xy + wz), xz - wy, -(xy - wz), 1 - (xx + zz), -(yz + wx), xz + wy, -(yz - wx), 1 - (xx + yy)]


def box(h, lift=0.0):
    """A box of half size h (m), bottom at lift - h... centered on (0, lift, 0): verts, normals, outward CCW tris."""
    vs = [(sx * h, lift + sy * h, sz * h) for sx in (-1, 1) for sy in (-1, 1) for sz in (-1, 1)]
    faces = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    verts, nrms, tris = [], [], []
    for f in faces:
        a, b, c = (vs[i] for i in f[:3])
        e1 = [b[i] - a[i] for i in range(3)]
        e2 = [c[i] - a[i] for i in range(3)]
        n = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
        ln = math.sqrt(sum(v * v for v in n))
        n = tuple(v / ln for v in n)
        center = [sum(vs[i][k] for i in f) / 4 for k in range(3)]
        out = sum(n[k] * (center[k] - (lift if k == 1 else 0.0)) for k in range(3)) > 0
        base = len(verts)
        verts += [vs[i] for i in f]
        nrms += [n if out else tuple(-v for v in n)] * 4
        tris += [(base, base + 1, base + 2), (base, base + 2, base + 3)] if out else \
                [(base, base + 2, base + 1), (base, base + 3, base + 2)]
    return verts, nrms, tris


def floor(h):
    """A flat square floor of half size h (m) at local y 0, facing up."""
    verts = [(-h, 0.0, -h), (-h, 0.0, h), (h, 0.0, h), (h, 0.0, -h)]
    return verts, [(0.0, 1.0, 0.0)] * 4, [(0, 1, 2), (0, 2, 3)]


def mover_defs(c):
    """The four test objects: name, mode, a game position beside the road, the node's exported pose (Blender writes
    the current frame's, not the first key's), its keys, a parent pose (or None), and its parts."""
    L = c.line.length

    def beside(s, side=90.0):
        p, lat = c.line.at(s), c.line.lateral(s)
        return to_gltf((p[0] + lat[0] * side, p[1], p[2] + lat[2] * side))

    a, b, d, e = beside(0.15 * L), beside(0.25 * L), beside(0.4 * L), beside(0.6 * L, -90.0)
    yaw = [quat_axis((0, 1, 0), k * 90.0) for k in range(5)]
    return [
        # A closed loop turning about y (the last key is the first's pose: 4 nodes).
        dict(name='spin', mode='loop', at=a, file_trs=(a, yaw[1], (1, 1, 1)), times=[0, 1, 2, 3, 4],
             T=[a] * 5, R=yaw, S=None, parent=None, parts=[('vis', box(3.0, 3.0))]),
        # Back and forth with a scale change and a wall that moves with it.
        dict(name='slide', mode='pingpong', at=b, file_trs=(b, (0, 0, 0, 1), (1, 1, 1)), times=[0, 1.5, 3],
             T=[b, (b[0] + 6, b[1], b[2]), (b[0] + 12, b[1] + 2, b[2])], R=None,
             S=[(1, 1, 1), (1.5, 1, 1), (1, 2, 1)], parent=None,
             parts=[('vis', box(2.0, 2.0)), ('COL_WALL_slide', box(2.0, 2.0))]),
        # A ride: a floor that rises 9 m while a car is on it.
        dict(name='lift', mode='ride', at=d, file_trs=(d, (0, 0, 0, 1), (1, 1, 1)), times=[0, 4],
             T=[d, (d[0], d[1] + 9, d[2])], R=None, S=None, parent=None,
             parts=[('vis', floor(6.0)), ('COL_ROAD_lift', floor(6.0))]),
        # Under a turned, scaled parent; turns about a tilted axis and doesn't end where it began (a loop that jumps).
        dict(name='tilt', mode='loop', at=e, file_trs=((0, 2, 0), (0, 0, 0, 1), (1, 1, 1)), times=[0, 2, 5],
             T=[(0, 2, 0), (1, 3, 0), (0, 4, 1)], R=[quat_axis((1, 1, 0), 0), quat_axis((1, 1, 0), 60), quat_axis((1, 1, 0), 150)],
             S=None, parent=(e, quat_axis((0, 1, 0), 30), (2, 2, 2)), parts=[('vis', box(1.5))]),
    ]


def build_movers(g, c):
    toml = ''
    for m in mover_defs(c):
        parent = None
        if m['parent']:
            parent = g.empty('MOVEPARENT_' + m['name'], trs=m['parent'])
        node = g.empty('MOVE_' + m['name'], parent=parent, trs=m['file_trs'])
        for part, (verts, nrms, tris) in m['parts']:
            g.mesh(part if part != 'vis' else 'MOVEVIS_' + m['name'], verts, nrms, tris, parent=node,
                   color=m.get('color') if part == 'vis' else None)
        g.animate('MOVE_%sAction' % m['name'], node, m['times'], m['T'], m['R'], m['S'])
        toml += '\n[movers.%s]\nmode = "%s"\n' % (m['name'], m['mode'])
    return toml


def check_movers(c, out, report, fails):
    import collision
    pb = open(os.path.join(out, 'mover_paths.bin'), 'rb').read() if os.path.exists(os.path.join(out, 'mover_paths.bin')) else b''
    if not pb:
        fails.append('no mover_paths.bin')
        return
    be32 = lambda d, o: struct.unpack_from('>I', d, o)[0]
    dir_at = be32(pb, 0)
    assert pb[dir_at:dir_at + 4] == b'PTHD'
    base, count = be32(pb, dir_at + 4), be32(pb, dir_at + 8)
    paths = []
    for k in range(count):
        h = base + k * 36
        name = pb[h:h + 16].split(b'\0')[0].decode()
        flags, nodes, group = be32(pb, h + 16), struct.unpack_from('>h', pb, h + 20)[0], struct.unpack_from('>h', pb, h + 22)[0]
        off = be32(pb, h + 28)
        ns = []
        for j in range(nodes):
            f = struct.unpack_from('>16fI', pb, off + j * 0x44)
            ns.append(dict(pos=f[0:3], dir=f[3:6], scale=f[6:9], quat=f[9:13], dist=f[13], time=f[14], speed=f[15], flags=f[16]))
        paths.append(dict(name=name, flags=flags, group=group, nodes=ns))
    defs = mover_defs(c)
    if len(paths) != len(defs):
        fails.append('%d mover paths, want %d' % (len(paths), len(defs)))
        return
    placement = open(os.path.join(out, 'placement.bin'), 'rb').read()
    records = [tuple(map(int, l.split())) for l in open(os.path.join(out, 'mover_records.txt')).read().split('\n') if l.strip()]
    want_flags = {'loop': 0x2, 'pingpong': 0x1, 'ride': 0x4040}
    for j, (m, p) in enumerate(zip(defs, paths)):
        if not re.search(r'^MOVE_%s: ' % m['name'], report, re.M):
            fails.append('no report line for MOVE_%s' % m['name'])
        if (p['flags'] & 0x4043) != want_flags[m['mode']]:
            fails.append('MOVE_%s flags %#x, want mode bits %#x' % (m['name'], p['flags'], want_flags[m['mode']]))
        has_col = any(part.startswith('COL_') for part, _ in m['parts'])
        if bool(p['group']) != has_col or bool(p['flags'] & 0x20) != has_col:
            fails.append('MOVE_%s group %d flags %#x, collision %s' % (m['name'], p['group'], p['flags'], has_col))
        same_turn = not m['R'] or abs(sum(x * y for x, y in zip(m['R'][0], m['R'][-1]))) > 0.99999
        closed = m['mode'] == 'loop' and m['T'][-1] == m['T'][0] and same_turn and (m['S'] or [0])[-1] == (m['S'] or [0])[0]
        want_nodes = len(m['times']) - (1 if closed else 0)
        if len(p['nodes']) != want_nodes:
            fails.append('MOVE_%s %d nodes, want %d' % (m['name'], len(p['nodes']), want_nodes))
        # Every node's pose against the glTF pose at its key, for each vertex of the object.
        parent = trs_matrix(*m['parent']) if m['parent'] else trs_matrix((0, 0, 0), (0, 0, 0, 1), (1, 1, 1))
        worst = 0.0
        for k, node in enumerate(p['nodes']):
            t = m['T'][k]
            r = m['R'][k] if m['R'] else m['file_trs'][1]
            s = m['S'][k] if m['S'] else m['file_trs'][2]
            w = mat_mul(parent, trs_matrix(t, r, s))
            mm = quat_to_matrix_2049(node['quat'])
            mm = [mm[i] * node['scale'][i // 3] for i in range(9)]
            for _, (verts, _, _) in m['parts']:
                for v in verts:
                    want = to_game_pt(mat_point(w, v))
                    loc = to_game_pt(v)
                    got = tuple(loc[0] * mm[i] + loc[1] * mm[3 + i] + loc[2] * mm[6 + i] + node['pos'][i] for i in range(3))
                    worst = max(worst, math.dist(want, got))
            if k == 0:
                rec = [r for r in records if r[0] == j]
                if not rec:
                    fails.append('MOVE_%s has no placement record' % m['name'])
                else:
                    o = 0x18 + rec[0][2] * 0x64
                    rm = struct.unpack_from('>12f', placement, o + 16)
                    if max(abs(rm[i] - mm[i]) for i in range(9)) > 1e-3 or max(abs(rm[9 + i] - node['pos'][i]) for i in range(3)) > 0.01:
                        fails.append('MOVE_%s placement record pose differs from its first node' % m['name'])
        if worst > 0.05:
            fails.append('MOVE_%s: a node pose is %.3f ft off the glTF pose' % (m['name'], worst))
    # Moving collision: MOVER records per group, floors flagged 0x20, and listed in the cells along their travel.
    mc = open(os.path.join(out, 'mover_collision.bin'), 'rb').read()
    n_poly, n_mover = struct.unpack_from('>H', mc, 4)[0], struct.unpack_from('>H', mc, 8)[0]
    info = [struct.unpack_from('>H', mc, 0x10 + i * 0x18 + 2)[0] for i in range(n_poly)]
    movers = [struct.unpack_from('>hH', mc, 0x10 + n_poly * 0x18 + i * 0x20) for i in range(n_mover)]
    coll = collision.Collision.parse(open(os.path.join(out, 'collision.bin'), 'rb').read(), collision.R2)
    for j, (m, p) in enumerate(zip(defs, paths)):
        if not p['group']:
            continue
        polys = [poly for g_, poly in movers if g_ == p['group']]
        if not polys:
            fails.append('MOVE_%s: no MOVER records' % m['name'])
            continue
        if m['name'] == 'lift' and not all(info[q] & 0x20 and info[q] >> 11 == p['group'] for q in polys):
            fails.append('MOVE_lift: floor polygons not flagged as a platform of its group')
        for k, node in enumerate(p['nodes']):
            for q in polys:
                pts = collision.world_polygon(coll, coll.polys[q])
                # the polygon's rest pose moved to this node: the object's displacement and turn
                mm0 = quat_to_matrix_2049(p['nodes'][0]['quat'])
                mm0 = [mm0[i] * p['nodes'][0]['scale'][i // 3] for i in range(9)]
                mmk = quat_to_matrix_2049(node['quat'])
                mmk = [mmk[i] * node['scale'][i // 3] for i in range(9)]
                inv = invert3(mm0)
                cx = sum(pt[0] for pt in pts) / len(pts)
                cy = sum(pt[1] for pt in pts) / len(pts)
                cz = sum(pt[2] for pt in pts) / len(pts)
                d = (cx - p['nodes'][0]['pos'][0], cy - p['nodes'][0]['pos'][1], cz - p['nodes'][0]['pos'][2])
                loc = tuple(sum(d[r] * inv[r * 3 + i] for r in range(3)) for i in range(3))
                wx = sum(loc[r] * mmk[r * 3 + 0] for r in range(3)) + node['pos'][0]
                wz = sum(loc[r] * mmk[r * 3 + 2] for r in range(3)) + node['pos'][2]
                leaf = collision.leaf_for_cell(coll, collision.cell(wx, collision.R2), collision.cell(wz, collision.R2), collision.R2)
                if not leaf or q not in leaf:
                    fails.append('MOVE_%s polygon %d not in the cell at node %d (%.0f, %.0f)' % (m['name'], q, k, wx, wz))
                    break


# --- billboards, props, translucency, far movers, authored AI values (objects=True) -----------------------------------
# A case with objects=True adds: three BILLBOARD_ cards (two of one mesh and scale, which share a model) in a BLEND
# material, both PROP_ kinds and a billboard prop, a translucent TRACK_glass card, MOVE_far whose mesh
# reaches past the 2000 ft vertex reach, and _SPEED / _AI_MODE on AIPATH_main; check_objects() reads them back.

OBJ_SPEED, OBJ_MODE = 77, 5


def card(w, h):
    """An upright card w x h (m) in the local x-y plane, its front toward +z (Blender's front view), base at y 0."""
    verts = [(-w / 2, 0.0, 0.0), (w / 2, 0.0, 0.0), (w / 2, h, 0.0), (-w / 2, h, 0.0)]
    return verts, [(0.0, 0.0, 1.0)] * 4, [(0, 1, 2), (0, 2, 3)]


def object_defs(c):
    L = c.line.length

    def beside(s, side=90.0):
        p, lat = c.line.at(s), c.line.lateral(s)
        return to_gltf((p[0] + lat[0] * side, p[1], p[2] + lat[2] * side))

    return dict(
        billboards=[('a', beside(0.12 * L), 2.0), ('b', beside(0.14 * L), 2.0), ('c', beside(0.16 * L), 1.0)],
        props=[('KNOCK_box', -2, beside(0.20 * L), quat_axis((0, 1, 0), 30), False),
               ('TIP_sign', -3, beside(0.22 * L), (0, 0, 0, 1), False),
               ('KNOCK_card', -2, beside(0.24 * L), (0, 0, 0, 1), True)],
        glass=beside(0.30 * L), far=beside(0.70 * L))


def build_objects(g, c):
    d = object_defs(c)
    glass = g.material('glass', None, 'BLEND', True)
    plain = g.material('plain')

    def prims(mesh, mat):
        v, n, t = mesh
        return [(mat, v, n, [(0.0, 0.0)] * len(v), None, t)]
    for name, at, scale in d['billboards']:
        g.textured_mesh('BILLBOARD_' + name, prims(card(2.0, 3.0), glass), trs=(at, (0, 0, 0, 1), (scale,) * 3))
    for name, _, at, rot, bill in d['props']:
        node = g.textured_mesh('PROP_' + name, prims(card(1.0, 1.0) if bill else box(0.5, 0.5), plain),
                               trs=(at, rot, (1, 1, 1)))
        if bill:
            g.js['nodes'][node]['extras'] = {'billboard': True}
    g.textured_mesh('TRACK_glass', prims(card(4.0, 4.0), glass), trs=(d['glass'], (0, 0, 0, 1), (1, 1, 1)))
    node = g.empty('MOVE_far', trs=(d['far'], (0, 0, 0, 1), (1, 1, 1)))
    g.textured_mesh('MOVEVIS_far', prims(box(3.0, 800.0), plain), parent=node)    # 800 m (2,625 ft) up
    g.animate('MOVE_farAction', node, [0, 1, 2, 3, 4], None, [quat_axis((0, 1, 0), k * 90.0) for k in range(5)], None)
    return '\n[movers.far]\nmode = "loop"\n'


def check_objects(c, out, report, fails, fwd):
    d = object_defs(c)
    for rx in (r'^Billboards: 3 BILLBOARD_', r'^Props: 3 PROP_', r'^MOVE_far: .* [1-9]\d* triangles'):
        if not re.search(rx, report, re.M):
            fails.append('no report line /%s/' % rx)
    pl = open(os.path.join(out, 'placement.bin'), 'rb').read()
    count = (len(pl) - 0x18) // 0x64
    recs = []
    for i in range(count):
        o = 0x18 + i * 0x64
        recs.append(dict(name=pl[o:o + 16].split(b'\0')[0].decode(), m=struct.unpack_from('>9f', pl, o + 0x10),
                         pos=struct.unpack_from('>3f', pl, o + 0x34), flags=struct.unpack_from('>I', pl, o + 0x40)[0]))

    def at(p):
        g = to_game_pt(p)
        found = [r for r in recs if math.dist(r['pos'], g) < 0.05]
        return found[0] if found else None
    names = {}
    for name, p, _ in d['billboards']:
        r = at(p)
        if not r:
            fails.append('BILLBOARD_%s: no placement record at its position' % name)
            continue
        names[name] = r['name']
        if r['flags'] & 0x48000 != 0x48000:
            fails.append('BILLBOARD_%s flags %#x: want a camera card (0x8000) in the late pass (0x40000)' % (name, r['flags']))
    if len(names) == 3 and (names['a'] != names['b'] or names['a'] == names['c']):
        fails.append('billboard models %s: a and b (same mesh and scale) should share one, c have its own' % names)
    pf = os.path.join(out, 'props.txt')
    props = [l.split() for l in open(pf).read().splitlines()] if os.path.exists(pf) else []
    for name, kind, p, rot, bill in d['props']:
        r = at(p)
        rows = [pr for pr in props if r and int(pr[0]) == recs.index(r)]
        if not r or not rows:
            fails.append('PROP_%s: no prop record' % name)
            continue
        if int(rows[0][1]) != kind:
            fails.append('PROP_%s type %s, want %d' % (name, rows[0][1], kind))
        if bool(r['flags'] & 0x8000) != bill:
            fails.append('PROP_%s flags %#x: camera card should be %s' % (name, r['flags'], bill))
        if name == 'KNOCK_box' and abs(abs(r['m'][0]) - math.cos(math.radians(30))) > 1e-3:
            fails.append('PROP_KNOCK_box placed unturned (m %s)' % (r['m'][:3],))
    if not any(r['flags'] & 0x40000 and not r['flags'] & 0x8000 for r in recs):
        fails.append('TRACK_glass: no section in the late pass')
    pb = open(os.path.join(out, 'mover_paths.bin'), 'rb').read() if os.path.exists(os.path.join(out, 'mover_paths.bin')) else b''
    if pb:
        dir_at = struct.unpack_from('>I', pb, 0)[0]
        base = struct.unpack_from('>I', pb, dir_at + 4)[0]
        node = struct.unpack_from('>I', pb, base + 28)[0]
        scale = struct.unpack_from('>3f', pb, node + 24)
        if scale[0] < 1.2:
            fails.append('MOVE_far path scale %.2f: its coarser vertex units aren\'t made up' % scale[0])
    else:
        fails.append('MOVE_far: no mover_paths.bin')
    mc = os.path.join(out, 'mover_collision.bin')
    if not os.path.exists(mc) or os.path.getsize(mc) < 0x10:
        fails.append('no mover_collision.bin with MOVE_far (no collision): the movers code would never start')
    bad = [(k, pt[3], pt[4]) for k, lane in enumerate(fwd['lanes']) for pt in lane['points'] if (pt[3], pt[4]) != (OBJ_SPEED, OBJ_MODE)]
    if bad:
        fails.append('%d lane points without the authored speed %d / mode %d (first: lane %d %d / %d)' % (
            len(bad), OBJ_SPEED, OBJ_MODE, *bad[0]))


SPLINE_RISE = 0.02    # the splines case's up slope at each segment's start (its end slope is minus it: a hump)


def spline_segments(c, n):
    """A segment per road piece k of build() (n pieces): start line across the road at the piece's start, length the
    piece's, half-widths ROAD_HALF, edge slopes SPLINE_RISE (a 0.02 x 10 ft / 4 = 0.05 ft hump mid-piece), as the
    game's 33-float SEG records (docs/rush2049_research/collision.md)."""
    L = c.line.length
    out = []
    for k in range(n):
        s0, s1 = k * L / n, (k + 1) * L / n
        o, t, lat = c.line.at(s0), c.line.tangent(s0), c.line.lateral(s0)
        across = [-x for x in lat]      # u 0 at the +lateral side
        up = (t[1] * across[2] - t[2] * across[1], t[2] * across[0] - t[0] * across[2], t[0] * across[1] - t[1] * across[0])
        if up[1] < 0:
            across = list(lat)
            up = tuple(-x for x in up)
        # The next start line in this frame (rows across, up, along): its ends fix the length's change across (0x60:
        # a bend's outside is longer) and the half-widths' change along (0x68 / 0x70), so it is t = 1, u = 0 / 1.
        o1, lat1 = c.line.at(s1), c.line.lateral(s1)
        sgn = 1.0 if across == [-x for x in lat] else -1.0
        frame = lambda p: [sum(r[i] * (p[i] - o[i]) for i in range(3)) for r in (across, up, t)]
        left = frame([o1[i] + sgn * lat1[i] * ROAD_HALF for i in range(3)])     # u = 0 end (-across)
        right = frame([o1[i] - sgn * lat1[i] * ROAD_HALF for i in range(3)])    # u = 1 end
        taper = (right[2] - left[2]) / (right[0] - left[0])
        length = left[2] - left[0] * taper
        out.append(list(o) + list(across) + list(up) + list(t) + list(across) + list(t) + list(t) +
                   [2 * ROAD_HALF, length, 0.0, taper, -ROAD_HALF, (left[0] + ROAD_HALF) / left[2],
                    ROAD_HALF, (right[0] - ROAD_HALF) / right[2], SPLINE_RISE, SPLINE_RISE, 0.0, 0.0])
    return out


def check_splines(c, out, report, fails):
    import collision as C
    import numpy as np
    n = int(round(c.line.length / STEP))
    want = spline_segments(c, n)
    if not re.search(r'^Spline road: %d segments, \d+ polygons' % n, report, re.M):
        fails.append('no "Spline road: %d segments" report line' % n)
    col = C.Collision.parse(open(os.path.join(out, 'collision.bin'), 'rb').read(), C.R2)
    if len(col.segs) != n:
        fails.append('SEG section holds %d segments, want %d' % (len(col.segs), n))
        return
    got = [struct.unpack('>33f', s) for s in col.segs]
    worst = max(abs(a - b) for g, w in zip(got, want) for a, b in zip(g, w))
    if worst > 1e-3:
        fails.append('SEG records differ from ROAD_SPLINES by %.4f' % worst)
    segs = [C.seg_fields(g) for g in got]
    flat = off = 0
    for p in col.polys:
        if p['flags'] & 0xF != 0:
            continue
        if not p['flags'] & 0x1000 or p['seg'] is None:
            flat += 1
            continue
        w = np.array(C.world_polygon(col, p))
        k = p['seg']
        h, t, u = C.seg_surface(segs[k], segs[(k + 1) % n], w)
        # Its corners: in its own segment's stretch (t 0-1, u 0-1), the flat road being the cubic's lift above the
        # surface (the game puts the surface point the lift below the start lines' joining, along the normal).
        lift = SPLINE_RISE * t * (1 - t) * segs[k]['58']
        if (t < -0.01).any() or (t > 1.01).any() or (u < -0.01).any() or (u > 1.01).any() or np.abs(h - lift).max() > 0.05:
            off += 1
    if flat:
        fails.append('%d road polygons without a segment (flag 0x1000 and an index)' % flat)
    if off:
        fails.append('%d spline polygons off their segment (outside its stretch, or the surface misplaced)' % off)


def to_game_pt(p):
    return (-p[0] * FEET_PER_METER, p[1] * FEET_PER_METER, p[2] * FEET_PER_METER)


def invert3(m):
    a, b, c, d, e, f, g, h, i = m
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    return [(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
            (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
            (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]


# --- the cases ----------------------------------------------------------------------------------------------------

def cases():
    o = oval()
    L = o.length
    std = lambda: [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.3 * L), Gate('CHECKPOINT_2', 0.55 * L),
                   Gate('CHECKPOINT_3', 0.8 * L)]
    out = [
        Case('oval', o, std(), note='control'),
        Case('movers', o, std(), movers=True, note='MOVE_ objects: loop, ping-pong, ride, parented; poses, moving collision'),
        Case('scripts', o, std(), scripts=True, warnings=('TRIGGER_UNUSED',),
             note='scripted MOVE_ objects (door, once, start events), TRIGGER_ boxes, a moving billboard, covered collision'),
        Case('objects', o, std(), objects=True,
             note='BILLBOARD_ (shared models), PROP_ kinds, BLEND late pass, MOVE_ past the vertex reach, _SPEED/_AI_MODE'),
        Case('splines', o, std(), splines=True, note='ROAD_SPLINES + _SEGMENT: SEG section, flag 0x1000, indices'),
        Case('ribbon_reversed', o, std(), reverse=True, note='curve drawn against the race'),
        Case('painted_lanes', o, std(),
             lanes={1: (lambda s: 28.0, False, True), 3: (lambda s: -2.0 + 5.0 * math.sin(s / L * 28 * math.pi), True, True)},
             note='AIPATH_lane1 and a wavy AIPATH_lane3 drawn backward; lanes 2 and 4 offset'),
        Case('painted_lanes_reversed_main', o, std(), reverse=True,
             lanes={k: ((lambda o: lambda s: o)(o), False, True) for k, o in ((1, 30.0), (2, 10.0), (3, -10.0), (4, -30.0))},
             note='all four lanes painted, AIPATH_main drawn against the race'),
        Case('painted_lane_far', o, std(), lanes={2: (lambda s: 300.0, False, False)},
             warnings=[r"AIPATH_lane2 doesn't run beside AIPATH_main anywhere"], note='a lane off on another road: unused'),
        Case('ribbon_starts_elsewhere', o, std(), start=0.62 * L, note='curve starts mid-lap'),
        Case('seam_overlap_finish', o, std(), start=0.05 * L - 3, overshoot=4.5,
             note="curve's ends overlap across FINISH"),
        Case('seam_overlap_checkpoint', o, std(), start=0.3 * L - 2, overshoot=5,
             note="curve's ends overlap across a checkpoint"),
        Case('open_curve', o, std(), start=0.7 * L, overshoot=-60, warnings=[r"isn't a closed loop"],
             note='curve not cyclic, gap away from the markers'),
        Case('open_curve_gap_at_finish', o, std(), start=0.05 * L + 3, overshoot=-60,
             warnings=[r"isn't a closed loop"], note='open curve whose gap straddles FINISH'),
        Case('checkpoint_arrow_reversed', o,
             [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.3 * L, arrow=-1), Gate('CHECKPOINT_2', 0.55 * L),
              Gate('CHECKPOINT_3', 0.8 * L)], warnings=[r"CHECKPOINT_1's green arrow points against"]),
        Case('finish_arrow_reversed', o,
             [Gate('FINISH', 0.05 * L, arrow=-1), Gate('CHECKPOINT_1', 0.3 * L), Gate('CHECKPOINT_2', 0.55 * L),
              Gate('CHECKPOINT_3', 0.8 * L)], warnings=[r"FINISH's green arrow points against"]),
        Case('one_checkpoint_both_arrows_tie', o,
             [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.5 * L, arrow=-1)],
             warnings=[r"CHECKPOINT_1's green arrow points against"], note='1 vs 1: FINISH decides'),
        Case('marker_off_center', o,
             [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.3 * L, side=30), Gate('CHECKPOINT_2', 0.55 * L),
              Gate('CHECKPOINT_3', 0.8 * L)], note='still across the path'),
        Case('marker_beside_road', o,
             [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.3 * L, side=120), Gate('CHECKPOINT_2', 0.55 * L),
              Gate('CHECKPOINT_3', 0.8 * L)], errors=[r"CHECKPOINT_1 .* isn't crossed"], wrong=['CHECKPOINT_1']),
        Case('marker_high_above', o,
             [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.3 * L, up=60), Gate('CHECKPOINT_2', 0.55 * L),
              Gate('CHECKPOINT_3', 0.8 * L)], errors=[r"CHECKPOINT_1 .* isn't crossed"], wrong=['CHECKPOINT_1']),
        Case('marker_a_little_above', o,
             [Gate('FINISH', 0.05 * L, up=12), Gate('CHECKPOINT_1', 0.3 * L, up=20), Gate('CHECKPOINT_2', 0.55 * L),
              Gate('CHECKPOINT_3', 0.8 * L)], note='Empties placed at car height'),
        Case('numbered_out_of_order', o,
             [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_2', 0.3 * L), Gate('CHECKPOINT_1', 0.55 * L),
              Gate('CHECKPOINT_3', 0.8 * L)], errors=[r"aren't numbered in race order"]),
        Case('marker_exactly_on_rung', o,
             [Gate('FINISH', 0.05 * L, exact=True), Gate('CHECKPOINT_1', 0.3 * L, exact=True),
              Gate('CHECKPOINT_2', 0.55 * L), Gate('CHECKPOINT_3', 0.8 * L)]),
        Case('duplicate_rungs_at_markers', o, std(), dupes=[0.05 * L, 0.3 * L]),
        Case('export_blender_cyclic', o, std(), export='blender_cyclic',
             note="Blender's cyclic curve: the UV seam stores the first rung twice"),
        Case('export_blender_cyclic_mid_lap', o, std(), start=0.62 * L, export='blender_cyclic'),
        Case('export_blender_cyclic_reversed', o, std(), reverse=True, export='blender_cyclic'),
        Case('export_welded_loop', o, std(), export='welded', note='closed strip, no seam copies'),
        Case('export_flat_shaded', o, std(), export='flat', note='a vertex per face corner'),
        Case('export_shuffled', o, std(), export='shuffled', note='vertices stored in any order'),
        Case('export_scaled_node', o, std(), export='scaled', note='object scale 4, vertices at a quarter'),
        Case('ribbon_on_edge', o, std(), export='on_edge', errors=[r'stands on edge'], nopath=True,
             warnings=[r'center line'],
             note='curve drawn flat, Tilt 0: the ribbon stands up'),
        Case('ribbon_thin', o, std(), export='thin', errors=[r'ribbon is only 0\.\d m'], warnings=[r'center line']),
        Case('ribbon_pinched', o, std(), export='pinch', note='one rung pinched to a point: let through'),
        Case('no_aipath', o, std(), ribbon=False, errors=[r'No AIPATH_main'], nopath=True,
             warnings=[r'stand-in AI path straight through the markers'], note='loads on a stand-in path'),
        Case('no_aipath_finish_only', o, [Gate('FINISH', 0.05 * L)], ribbon=False, nopath=True,
             errors=[r'No AIPATH_main', r'No checkpoints'], warnings=[r'stand-in AI path straight round a circle'],
             allow=[r'FINISH is wide enough|crossed by the AI path']),
        Case('no_finish', o, std()[1:], errors=[r'No FINISH'], warnings=[r'stand-in finish line'], nopath=True,
             note='stand-in finish across the path near its start'),
        Case('no_finish_no_aipath', o, std()[1:], ribbon=False, errors=[r'No FINISH', r'No AIPATH_main'], nopath=True,
             warnings=[r'stand-in finish line', r'stand-in AI path'], note='nothing but road and checkpoints'),
        Case('collision_only', o, std(), visual=False, errors=[r'COL_\* meshes are drawn instead'],
             note='no visible mesh: the collision is drawn'),
        Case('markers_only_road', o, [], ribbon=False, errors=[r'No FINISH', r'No AIPATH_main', r'No checkpoints'],
             nopath=True, warnings=[r'stand-in finish line', r'round a circle'], allow=[r'crossed by the AI path'],
             note='no markers or path at all'),
        Case('ribbon_tube', o, std(), export='tube', errors=[r'closed tube'], nopath=True,
             warnings=[r'stand-in AI path'], note='Bevel Depth set: loads on a stand-in path'),
        Case('kink_at_markers', o, std(), kinks=[0.05 * L, 0.3 * L - 3], note='ribbon steps back 4 ft at the line'),
        Case('checkpoint_just_after_finish', o,
             [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.05 * L + 45), Gate('CHECKPOINT_2', 0.55 * L)]),
        Case('checkpoint_just_before_finish', o,
             [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.5 * L), Gate('CHECKPOINT_2', 0.05 * L - 45 + L)]),
    ]
    s = serpentine()
    S = s.length
    # Leg A runs from 0 to ~1500, B back from ~1650 to ~3150, C from ~3300 to ~4800 (measured along the line).
    a_mid, b_mid, c_mid = s_of(s, (750, 0, 0)), s_of(s, (750, 0, 130)), s_of(s, (750, 0, 260))
    out += [
        Case('compact_wide_gate_reaches_earlier_leg', s,
             [Gate('FINISH', s_of(s, (300, 0, 0))), Gate('CHECKPOINT_1', b_mid), Gate('CHECKPOINT_2', c_mid, half=150)],
             warnings=[r'CHECKPOINT_2 is wide enough to reach another stretch'],
             note="Wario Stadium: a checkpoint's width reaches the leg before it, running the other way"),
        Case('compact_wide_gate_reaches_later_leg', s,
             [Gate('FINISH', s_of(s, (300, 0, 0))), Gate('CHECKPOINT_1', a_mid + 300, half=150),
              Gate('CHECKPOINT_2', c_mid)],
             warnings=[r'CHECKPOINT_1 is wide enough to reach another stretch']),
        Case('compact_wide_finish', s,
             [Gate('FINISH', a_mid, half=150), Gate('CHECKPOINT_1', c_mid), Gate('CHECKPOINT_2', s_of(s, (0, 0, 700)))],
             warnings=[r'FINISH is wide enough to reach another stretch'], note='FINISH reaches the next leg over'),
        Case('compact_gate_reaches_same_way_leg', s,
             [Gate('FINISH', s_of(s, (300, 0, 0))), Gate('CHECKPOINT_1', a_mid, half=290), Gate('CHECKPOINT_2', c_mid)],
             errors=[r'CHECKPOINT_1 is crossed by the AI path 3 times'], wrong=['CHECKPOINT_1'],
             note='reaches B (other way) and C (same way): C and A cannot be told apart'),
        Case('compact_ribbon_reversed', s,
             [Gate('FINISH', s_of(s, (300, 0, 0))), Gate('CHECKPOINT_1', b_mid), Gate('CHECKPOINT_2', c_mid, half=150)],
             reverse=True, warnings=[r'CHECKPOINT_2 is wide enough']),
        Case('compact_normal_widths', s,
             [Gate('FINISH', s_of(s, (300, 0, 0))), Gate('CHECKPOINT_1', b_mid), Gate('CHECKPOINT_2', c_mid),
              Gate('CHECKPOINT_3', s_of(s, (-500, 0, 100)))]),
    ]
    f = figure8(True)
    F = f.length
    out += [
        Case('overpass_gate_under_bridge', f,
             [Gate('FINISH', 0.2 * F), Gate('CHECKPOINT_1', 0.5 * F), Gate('CHECKPOINT_2', 0.75 * F)],
             note='CHECKPOINT_1 on the lower road where the bridge crosses'),
        Case('overpass_gate_on_bridge', f,
             [Gate('FINISH', 0.0), Gate('CHECKPOINT_1', 0.25 * F), Gate('CHECKPOINT_2', 0.6 * F)],
             note='FINISH on the bridge over the lower road'),
    ]
    out += [
        Case('jump_gap_over_lower_road', f, [Gate('FINISH', 0.2 * F), Gate('CHECKPOINT_1', 0.5 * F),
                                             Gate('CHECKPOINT_2', 0.75 * F)],
             gaps=[(F - 60, 60)], heights=[(0.0, 'curve'), (F - 40, 'curve'), (40, 'curve')],
             allow=[r'no road under them'],
             note='the bridge has a gap where it crosses the lower road: the path stays in the air'),
    ]
    od = oval_dip()
    OD = od.length
    out += [
        Case('jump_gap_over_ground', o, [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.3 * L),
                                          Gate('CHECKPOINT_2', 0.6 * L)],
             gaps=[(0.45 * L - 50, 0.45 * L + 50)], floors=[(0.45 * L - 50, 0.45 * L + 50, 60.0)],
             heights=[(0.45 * L, 'curve')], allow=[r'no road under them'],
             note='a 100 ft gap with ground 60 ft below: the path stays in the air'),
        Case('sloppy_curve_over_dip', od, [Gate('FINISH', 0.05 * OD), Gate('CHECKPOINT_1', 0.3 * OD),
                                           Gate('CHECKPOINT_2', 0.6 * OD)],
             lift=lambda s: -od.at(s)[1], heights=[(0.45 * OD, 'road')],
             warnings=[r'above or below the road'],
             note='curve drawn flat over a 20 ft dip: it is dropped onto the road'),
    ]
    f0 = figure8(False)
    F0 = f0.length
    out += [
        Case('flat_crossing_gate_at_junction', f0,
             [Gate('FINISH', 0.2 * F0), Gate('CHECKPOINT_1', 0.5 * F0), Gate('CHECKPOINT_2', 0.75 * F0)],
             allow=[r'CHECKPOINT_1 is (wide enough|crossed by the AI path)'], wrong=['CHECKPOINT_1'],
             note='CHECKPOINT_1 where the road crosses itself at grade: the crossing road runs through it, a warning '
                  'or an error is right, the other gates must stay right'),
        Case('flat_crossing_gates_away', f0,
             [Gate('FINISH', 0.1 * F0), Gate('CHECKPOINT_1', 0.4 * F0), Gate('CHECKPOINT_2', 0.65 * F0)]),
    ]
    sp = spiral()
    SP = sp.length
    t_of = lambda frac_turns: SP * frac_turns / 2.0 * 0.97    # rough: distance by turns of the two
    out += [
        Case('side_by_side_same_way_narrow', sp,
             [Gate('FINISH', t_of(0.25), half=18), Gate('CHECKPOINT_1', t_of(0.75), half=18),
              Gate('CHECKPOINT_2', t_of(1.25), half=18), Gate('CHECKPOINT_3', t_of(1.75), half=18)],
             note='legs 40 ft apart, same way; gates narrower than that'),
        Case('side_by_side_same_way_wide', sp,
             [Gate('FINISH', t_of(0.25), half=18), Gate('CHECKPOINT_1', t_of(0.75), half=60),
              Gate('CHECKPOINT_2', t_of(1.25), half=18)],
             errors=[r'CHECKPOINT_1 is crossed by the AI path 2 times'], wrong=['CHECKPOINT_1'],
             note='a gate reaching the same-way leg beside it'),
    ]
    h = helix()
    H = h.length
    turn = 2 * math.pi * math.hypot(500, 60 / (2 * math.pi))
    out += [
        Case('stacked_same_way', h,
             [Gate('FINISH', 0.25 * turn), Gate('CHECKPOINT_1', 1.25 * turn), Gate('CHECKPOINT_2', 0.75 * turn + turn),
              Gate('CHECKPOINT_3', 2 * turn + 0.4 * (H - 2 * turn))],
             note='FINISH under CHECKPOINT_1, 60 ft apart, same way'),
        Case('stacked_reversed_ribbon', h,
             [Gate('FINISH', 0.25 * turn), Gate('CHECKPOINT_1', 1.25 * turn), Gate('CHECKPOINT_2', 0.75 * turn + turn)],
             reverse=True),
    ]
    v = vertical_loop()
    V = v.length
    loop_at = s_of(v, (0, 0, 500))
    out += [
        Case('vertical_loop_gate_before', v,
             [Gate('FINISH', loop_at - 30), Gate('CHECKPOINT_1', loop_at + 0.4 * V), Gate('CHECKPOINT_2', loop_at + 0.7 * V)],
             note='FINISH just before the loop; its top passes over backwards'),
        Case('vertical_loop_gate_under_top', v,
             [Gate('FINISH', loop_at + 0.5 * V), Gate('CHECKPOINT_1', loop_at + 0.8 * V), Gate('CHECKPOINT_2', loop_at + 5)],
             note='CHECKPOINT_2 at the foot of the loop'),
    ]
    return out


# --- arenas (track.toml type = "stunt" / "battle") --------------------------------------------------------------------
# An arena has no lap: the converter writes its START_n as the path's checkpoints (Rush 2049's battle starts player p
# at checkpoint [0, 2, 1, 3][p]), starts the route at START_1 facing along its arrow (Rush 2's stunt start: spine point
# 0 facing point 1) and, without AIPATH_main, covers the floor reachable from START_1 with route points. The layout:
# a 600 ft square floor at 0 walled round, a ramp from it up to a 150 ft deck at 30 ft, and a 100 ft island at 100 ft
# that nothing reaches.

ARENA_HALF = 300.0
DECK = ((150.0, 300.0), (-75.0, 75.0), 30.0)        # x range, z range, height (ft)
RAMP = ((-50.0, 150.0), (-40.0, 40.0))              # x range, z range: from 0 at x -50 to the deck's 30 at x 150
ISLAND = ((-250.0, -150.0), (150.0, 250.0), 100.0)


class ArenaCase:
    """kind: "stunt" or "battle"; starts: [(x, z, heading degrees)] as START_1.. (heading 0 = +z); pickups: [(name,
    x, z)] PICKUP_ markers 3 ft over the highest floor there; finish: a FINISH marker (x, z, heading) instead of / besides the starts; route:
    an AIPATH_main ribbon over these (x, z) points; errors / warnings / infos: regexes the report must hold."""

    def __init__(self, name, kind, starts=(), pickups=(), finish=None, route=None, errors=(), warnings=(), infos=(),
                 note=''):
        self.__dict__.update(locals())
        del self.__dict__['self']


def arena_floor_height(x, z):
    """The floors' heights at (x, z): the main floor, the ramp, the deck and the island where they are."""
    hs = []
    if abs(x) <= ARENA_HALF and abs(z) <= ARENA_HALF:
        hs.append(0.0)
    (rx0, rx1), (rz0, rz1) = RAMP
    if rx0 <= x <= rx1 and rz0 <= z <= rz1:
        hs.append(DECK[2] * (x - rx0) / (rx1 - rx0))
    for (x0, x1), (z0, z1), h in (DECK, ISLAND):
        if x0 <= x <= x1 and z0 <= z <= z1:
            hs.append(h)
    return hs


def arena_mesh(g, name, quads, up=True):
    """Quads (4 game points each, in order round the quad) as one mesh; up: their front faces up (floors), else
    toward -x of their own winding (walls take the winding as given)."""
    verts, nrms, tris = [], [], []
    for q in quads:
        for tri in ((q[0], q[1], q[2]), (q[0], q[2], q[3])):
            e1 = [tri[1][i] - tri[0][i] for i in range(3)]
            e2 = [tri[2][i] - tri[0][i] for i in range(3)]
            cr = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
            if up and cr[1] > 0:     # the converter's front is -cross (in game axes)
                tri = (tri[0], tri[2], tri[1])
            base = len(verts)
            verts += [to_gltf(v) for v in tri]
            nrms += [(0.0, 1.0, 0.0)] * 3
            tris.append((base, base + 1, base + 2))
    g.mesh(name, verts, nrms, tris)


def build_arena(c, folder):
    g = Glb()
    H = ARENA_HALF
    floors = [[(-H, 0, -H), (H, 0, -H), (H, 0, H), (-H, 0, H)]]
    (rx0, rx1), (rz0, rz1) = RAMP
    floors.append([(rx0, 0.0, rz0), (rx1, DECK[2], rz0), (rx1, DECK[2], rz1), (rx0, 0.0, rz1)])
    for (x0, x1), (z0, z1), h in (DECK, ISLAND):
        floors.append([(x0, h, z0), (x1, h, z0), (x1, h, z1), (x0, h, z1)])
    arena_mesh(g, 'COL_ROAD_arena', floors)
    arena_mesh(g, 'TRACK_arena', floors)
    walls = []
    for a, b in (((-H, -H), (H, -H)), ((H, -H), (H, H)), ((H, H), (-H, H)), ((-H, H), (-H, -H))):
        walls.append([(a[0], 0.0, a[1]), (b[0], 0.0, b[1]), (b[0], 40.0, b[1]), (a[0], 40.0, a[1])])
    arena_mesh(g, 'COL_WALL_arena', walls, up=False)
    for i, (x, z, heading) in enumerate(c.starts):
        d = (math.sin(math.radians(heading)), 0.0, math.cos(math.radians(heading)))
        g.empty('START_%d' % (i + 1), gate_matrix((x, 5.0, z), d, 20.0))
    if c.finish:
        x, z, heading = c.finish
        g.empty('FINISH', gate_matrix((x, 5.0, z), (math.sin(math.radians(heading)), 0.0, math.cos(math.radians(heading))), 40.0))
    for name, x, z in c.pickups:
        t = to_gltf((x, max(arena_floor_height(x, z)) + 3.0, z))
        g.empty(name, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, t[0], t[1], t[2], 1])
    if c.route:
        verts, tris = [], []
        for k, (x, z) in enumerate(c.route):
            verts += [to_gltf((x - 10, 0.0, z)), to_gltf((x + 10, 0.0, z))]
            if k:
                tris += [(2 * k - 2, 2 * k - 1, 2 * k), (2 * k - 1, 2 * k + 1, 2 * k)]
        g.mesh('AIPATH_main', verts, [(0.0, 1.0, 0.0)] * len(verts), tris)
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'track.glb'), 'wb') as f:
        f.write(g.bytes())
    with open(os.path.join(folder, 'track.toml'), 'w', newline='\n') as f:
        f.write('[track]\nname = "%s"\ntype = "%s"\n' % (c.name, c.kind))


def check_arena(c, folder):
    out, text = convert(folder)
    rp = os.path.join(out, 'report.txt')
    report = open(rp, encoding='utf-8').read() if os.path.exists(rp) else text
    fails, notes = [], []
    errs = re.findall(r'^ERROR: (.*)$', report, re.M)
    warns = re.findall(r'^Warning: (.*)$', report, re.M)
    infos = re.findall(r'^(?:Info: )?(.*)$', report, re.M)
    for rx in c.errors:
        if not any(re.search(rx, e) for e in errs):
            fails.append('missing error /%s/' % rx)
    for e in errs:
        if not any(re.search(rx, e) for rx in c.errors):
            fails.append('unexpected ERROR: ' + e[:150])
    for rx in c.warnings:
        if not any(re.search(rx, w) for w in warns):
            fails.append('missing warning /%s/' % rx)
    for w in warns:
        if not any(re.search(rx, w) for rx in c.warnings):
            fails.append('unexpected warning: ' + w[:150])
    for rx in c.infos:
        if not re.search(rx, report):
            fails.append('missing info /%s/' % rx)
    pf = os.path.join(out, 'path.bin')
    if not os.path.exists(pf):
        return fails + ['no path.bin written'], notes
    fwd = paths.parse(open(pf, 'rb').read())
    if open(pf, 'rb').read() != open(os.path.join(out, 'path_backward.bin'), 'rb').read():
        fails.append('the backward path differs (an arena has one path, used both ways)')
    starts = list(c.starts) or ([c.finish] if c.finish else [])
    if len(fwd['cps']) != max(1, len(starts[:4])):
        fails.append('%d checkpoints, want %d (the starts)' % (len(fwd['cps']), max(1, len(starts[:4]))))
    for i, (x, z, heading) in enumerate(starts[:4]):
        if i >= len(fwd['cps']):
            break
        cp = fwd['cps'][i]
        d = (math.sin(math.radians(heading)), math.cos(math.radians(heading)))
        if math.hypot(cp['pos'][0] - x, cp['pos'][2] - z) > 1 or min(abs(cp['pos'][1] - h) for h in arena_floor_height(x, z)) > 1:
            fails.append('checkpoint %d at %s, want START_%d on the floor at (%.0f, %.0f)' % (i, cp['pos'], i + 1, x, z))
        # rush2_track49_battle_start faces the car along -dir
        if -cp['dir'][0] * d[0] - cp['dir'][2] * d[1] < 0.99:
            fails.append('checkpoint %d direction %s, want -(%.2f, 0, %.2f)' % (i, cp['dir'], d[0], d[1]))
    sp = fwd['spine']
    if starts:
        x, z, heading = starts[0]
        d = (math.sin(math.radians(heading)), math.cos(math.radians(heading)))
        if math.hypot(sp[0][0] - x, sp[0][2] - z) > 1:
            fails.append('route starts at %s, want START_1 (%.0f, %.0f)' % (sp[0], x, z))
        a = (sp[1][0] - sp[0][0], sp[1][2] - sp[0][2])
        if (a[0] * d[0] + a[1] * d[1]) / (math.hypot(*a) or 1) < 0.99:
            fails.append('route point 1 %s isn\'t along START_1\'s arrow' % (sp[1],))
    for k, q in enumerate(sp):
        hs = arena_floor_height(q[0], q[2])
        if not hs or min(abs(q[1] - h) for h in hs) > 2:
            fails.append('route point %d %s isn\'t on a floor' % (k, q))
            break
        if abs(q[1] - ISLAND[2]) < 2 and ISLAND[0][0] <= q[0] <= ISLAND[0][1]:
            fails.append('route point %d %s is on the island nothing reaches' % (k, q))
            break
    if not c.route:
        # Coverage: every 60 ft of the main floor and the deck has a route point within 45 ft at its height.
        for cx in range(-270, 271, 60):
            for cz in range(-270, 271, 60):
                for h in arena_floor_height(cx, cz):
                    if h == ISLAND[2] or (0 < h < DECK[2]):
                        continue
                    if not any(math.hypot(q[0] - cx, q[2] - cz) < 45 and abs(q[1] - h) < 3 for q in sp):
                        fails.append('no route point near (%d, %.0f, %d)' % (cx, h, cz))
    for ln in fwd['lanes']:
        if [p[:3] for p in ln['points']] != [tuple(q) for q in sp]:
            fails.append('a lane isn\'t the route')
            break
    pk = os.path.join(out, 'pickups.txt')
    got = [tuple(float(x) for x in line.split()) for line in open(pk)] if os.path.exists(pk) else []
    want = [(name, x, z) for name, x, z in c.pickups] if c.kind == 'battle' else []
    kinds = {'PICKUP_CANNON': 0, 'PICKUP_GATLING': 1, 'PICKUP_GRENADE': 2, 'PICKUP_MINE': 3, 'PICKUP_MISSILE': 4,
             'PICKUP_RAM': 5, 'PICKUP_ROCKET': 6, 'PICKUP_SONIC': 7, 'PICKUP_POWERUP': 11}
    want = [w for w in want if w[0] in kinds]
    if len(got) != len(want):
        fails.append('%d pickups, want %d' % (len(got), len(want)))
    for (name, x, z), r in zip(want, got):
        if int(r[0]) != kinds[name] or math.hypot(r[1] - x, r[3] - z) > 1 or abs(r[2] - (max(arena_floor_height(x, z)) + 3.0)) > 0.6:
            fails.append('pickup %s at %s, want kind %d at (%.0f, 3, %.0f)' % (name, r, kinds[name], x, z))
    notes.append('%d route points' % len(sp))
    return fails, notes


def arena_cases():
    starts = [(-200.0, -200.0, 45.0), (200.0, 200.0, 225.0), (200.0, -200.0, -45.0), (-200.0, 200.0, 135.0)]
    pickups = [('PICKUP_MISSILE', 0.0, -150.0), ('PICKUP_POWERUP', 225.0, 0.0), ('PICKUP_SONIC', -100.0, 0.0),
               ('PICKUP_LASER', 50.0, 50.0)]
    return [
        ArenaCase('arena_battle', 'battle', starts=starts, pickups=pickups,
                  warnings=[r'PICKUP_LASER: unknown pickup'], infos=[r'Pickups: 2 weapon\(s\), 1 power-up spot',
                                                                     r"arena's route: \d+ points"],
                  note='4 starts, pickups (one on the deck), a ramp, an island out of reach'),
        ArenaCase('arena_battle_extra_starts', 'battle', starts=starts + [(0.0, 0.0, 0.0)], pickups=pickups[:1],
                  infos=[r'START_5 and up are ignored'], note='a fifth start'),
        ArenaCase('arena_battle_no_pickups', 'battle', starts=starts[:2], warnings=[r'No PICKUP_ markers'],
                  infos=[r'2 start\(s\)'], note='2 starts, no pickups'),
        ArenaCase('arena_stunt', 'stunt', starts=starts[:1], pickups=pickups[:1],
                  infos=[r'only placed in a battle arena'], note='stunt: one start; a pickup is ignored'),
        ArenaCase('arena_stunt_finish', 'stunt', finish=(0.0, -250.0, 0.0), note='FINISH stands in for START_1'),
        ArenaCase('arena_stunt_no_start', 'stunt', warnings=[r'No START_1'], note='no start at all: a stand-in'),
        ArenaCase('arena_stunt_route', 'stunt', starts=starts[:1],
                  route=[(-200.0 + 20 * k, -100.0) for k in range(20)], infos=[r"AIPATH_main is the arena's route"],
                  note="AIPATH_main as the route"),
    ]


def s_of(line, p):
    """Distance along line of its point nearest p (x/z)."""
    best, bs = 1e30, 0.0
    k = 0.0
    while k < line.length:
        q = line.at(k)
        d = (q[0] - p[0]) ** 2 + (q[2] - p[2]) ** 2 + (q[1] - p[1]) ** 2 * 0.01
        if d < best:
            best, bs = d, k
        k += 2.0
    return bs


def main():
    args = [a for a in sys.argv[1:]]
    only = None
    if '--only' in args:
        i = args.index('--only')
        only = set(args[i + 1].split(','))
        del args[i:i + 2]
    all_cases = cases() + arena_cases()
    if '--list' in args:
        for c in all_cases:
            print('%-40s %s' % (c.name, c.note))
        return 0
    out_dir = os.path.abspath(args[0] if args else os.path.join(REPO, 'tmp', 'track_mod_cases'))
    failed = 0
    for c in all_cases:
        if only and c.name not in only:
            continue
        folder = os.path.join(out_dir, c.name)
        if isinstance(c, ArenaCase):
            build_arena(c, folder)
            fails, notes = check_arena(c, folder)
        else:
            build(c, folder)
            fails, notes = check(c, folder)
        failed += bool(fails)
        print('%s  %-40s %s' % ('FAIL' if fails else 'PASS', c.name, c.note))
        for f in fails:
            print('        - ' + f)
        for n in notes:
            print('          (%s)' % n)
    print('\n%d failed' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
