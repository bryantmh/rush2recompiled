#!/usr/bin/env python3
"""Custom-track converter case suite: builds a synthetic track (track.glb + track.toml) for every road layout and
authoring mistake the converter's race direction, start and checkpoint-crossing logic has to handle, converts each with
tools/track_mod_test.bat, and checks the report and the crossings written into path.bin / path_backward.bin against the
ones the layout was built with (docs/custom_tracks.md, "Checkpoint crossings").

    python tools/track_mod_cases.py [OUT_DIR] [--only NAME[,NAME...]] [--list]

OUT_DIR defaults to tmp/track_mod_cases. Each case prints PASS or FAIL with its reasons, and "game search" when Rush 2's
own load-time search (func_80092D6C, which custom tracks skip) would have put a crossing elsewhere: that is what the
case guards against. Layouts are built in game feet (y up) along a centre line in race order; the AI path ribbon, the
road (COL_ROAD, drawn too) and the markers are made from it, then a case's mistakes are applied.
"""
import math, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
from stock_track_glb import Glb, gate_matrix, to_gltf, FEET_PER_METER  # noqa: E402
import paths  # noqa: E402

STEP = 10.0          # ft between centre line samples (and ribbon rungs)
SPINE = 20.0         # the converter's spine spacing
ROAD_HALF = 50.0     # road half width, ft
RIBBON_HALF = 35.0   # AIPATH_main half width, ft
GATE_HALF = 50.0     # a marker's default half width, ft
EXE = os.path.join(REPO, 'tmp', 'track_mod_test', 'track_mod_test.exe')
SOURCES = [os.path.join(REPO, p) for p in ('src/track_mod/track_mod_convert.cpp', 'tools/track_mod_test.cpp',
                                            'src/gltf_scene.cpp', 'include/track_mod.h')]


# --- centre lines (closed, race order, game feet) -------------------------------------------------------------------

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
    """A closed centre line resampled every STEP ft; position, tangent at a distance s along it."""

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
    (s, 'curve' | 'road') the spine's height there must be the curve's (kept in the air) or the road's (dropped)."""

    def __init__(self, name, line, gates, start=0.0, reverse=False, overshoot=0.0, kinks=(), dupes=(), errors=(),
                 warnings=(), allow=(), wrong=(), gaps=(), floors=(), lift=None, heights=(), note=''):
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


def build(c, folder):
    g = Glb()
    L = c.line.length
    n = int(round(L / STEP))
    verts, nrms, tris = [], [], []
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
            tris.append((base, base + 1, base + 2))
    g.mesh('COL_ROAD_case', verts, nrms, tris)
    g.mesh('TRACK_visual', verts, nrms, tris)
    rv, rn, rt = [], [], []
    for _, p, lat in rungs_of(c):
        rv += [to_gltf(tuple(p[i] - lat[i] * RIBBON_HALF for i in range(3))),
               to_gltf(tuple(p[i] + lat[i] * RIBBON_HALF for i in range(3)))]
        rn += [(0.0, 1.0, 0.0)] * 2
    for i in range(len(rv) // 2 - 1):
        rt += [(2 * i, 2 * i + 1, 2 * i + 2), (2 * i + 1, 2 * i + 3, 2 * i + 2)]
    g.mesh('AIPATH_main', rv, rn, rt)
    for gate in c.gates:
        p, t, lat = c.line.at(gate.s), c.line.tangent(gate.s), c.line.lateral(gate.s)
        pos = (p[0] + lat[0] * gate.side, p[1] + gate.up, p[2] + lat[2] * gate.side)
        d = (t[0] * gate.arrow, 0.0, t[2] * gate.arrow)
        g.empty(gate.name, gate_matrix(pos, d, gate.half))
    os.makedirs(folder, exist_ok=True)
    with open(os.path.join(folder, 'track.glb'), 'wb') as f:
        f.write(g.bytes())
    with open(os.path.join(folder, 'track.toml'), 'w', newline='\n') as f:
        f.write('[track]\nname = "%s"\n' % c.name)


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
    return fails, notes


# --- the cases ----------------------------------------------------------------------------------------------------

def cases():
    o = oval()
    L = o.length
    std = lambda: [Gate('FINISH', 0.05 * L), Gate('CHECKPOINT_1', 0.3 * L), Gate('CHECKPOINT_2', 0.55 * L),
                   Gate('CHECKPOINT_3', 0.8 * L)]
    out = [
        Case('oval', o, std(), note='control'),
        Case('ribbon_reversed', o, std(), reverse=True, note='curve drawn against the race'),
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
        Case('marker_off_centre', o,
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
    all_cases = cases()
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
