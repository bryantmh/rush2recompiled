"""How hard the computer cars are, offline, on Rush 2's circuits and California Speed's tracks under each CS Physics
mode (docs/calspeed_research.md#5-paths). Every AI lane: the drones at lane speed x 1.05 (func_80074990) within their
car's top speed, acceleration and cornering grip, against the best lap the player's car could drive on the same lane
(its top speed, acceleration and cornering grip; both brake at 40 ft/s^2). Prints drone time / player's best time
(higher = easier drones) overall and per track. Drivetrains follow docs/rush2049_research/cars.md#10-car-speeds-v.

    python tools/calspeed/ai_pace.py [--corner-g G] [--crest-g G]

--corner-g / --crest-g: Partial's lane caps (src/rush2049/track2049.cpp, defaults 3.25 and 0 = none).
CALSPEED_ROM and RUSH2049_ROM default to build/; the Rush 2 ROM is roms.py's default.
"""
import argparse, io, os, sys, math, contextlib, statistics as st
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'rush2049')); sys.path.insert(0, HERE)
os.environ.setdefault('CALSPEED_ROM', os.path.join(REPO, 'build', 'calspeed.z64'))
os.environ.setdefault('RUSH2049_ROM', os.path.join(REPO, 'build', 'rush2049.z64'))
ap = argparse.ArgumentParser()
ap.add_argument('--corner-g', type=float, default=3.25)
ap.add_argument('--crest-g', type=float, default=0.0)
ARGS = ap.parse_args()
with contextlib.redirect_stdout(io.StringIO()):
    import car_tables as CT
import cs, paths, roms
G, MPH = 32.17, 5280 / 3600
ROW = CT.ROWS['STANDARD']
BRAKE = 40.0


def capped(pos, mph, loop, corner_g=2.5, vertical_g=4.0, brake=40.0, reach=60.0, gap=1000.0):
    """rush2::calspeed::capped_lane_speeds in Python."""
    g, k = 32.17, 5280 / 3600
    n = len(pos)
    step = lambda i, j: math.hypot(pos[j][0] - pos[i][0], pos[j][2] - pos[i][2])

    def nxt(i, s):
        j = i + s
        if loop:
            j %= n
        return j if 0 <= j < n else -1

    def point_at(i, s):
        d, j = 0.0, i
        while d < reach:
            q = nxt(j, s)
            if q < 0 or q == i:
                return -1
            st = step(j, q)
            if st >= gap:
                return -1
            d += st
            j = q
        return j

    v = [m * k for m in mph]
    for i in range(n):
        a, c = point_at(i, -1), point_at(i, 1)
        if a < 0 or c < 0:
            continue
        ab, bc, ca = step(a, i), step(i, c), step(c, a)
        ar = abs((pos[i][0] - pos[a][0]) * (pos[c][2] - pos[a][2]) - (pos[i][2] - pos[a][2]) * (pos[c][0] - pos[a][0]))
        if ar < 1e-6:
            continue
        v[i] = min(v[i], math.sqrt(corner_g * g * ab * bc * ca / (2 * ar)))
    for i in range(n if vertical_g > 0 else 0):
        a, c = point_at(i, -1), point_at(i, 1)
        if a < 0 or c < 0:
            continue
        before = after = 0.0
        j = a
        while j != i:
            before += step(j, nxt(j, 1))
            j = nxt(j, 1)
        j = i
        while j != c:
            after += step(j, nxt(j, 1))
            j = nxt(j, 1)
        if before <= 0 or after <= 0:
            continue
        up = (pos[i][1] - pos[a][1]) / before
        dn = (pos[c][1] - pos[i][1]) / after
        kk = abs(up - dn) / (0.5 * (before + after))
        if kk > 1e-6:
            v[i] = min(v[i], math.sqrt(vertical_g * g / kk))
    for rep in range(2 if loop else 1):
        for i in range(n - 1, -1, -1):
            j = nxt(i, 1)
            if j < 0:
                continue
            d = step(i, j)
            if d >= gap:
                continue
            v[i] = min(v[i], math.sqrt(v[j] ** 2 + 2 * brake * d))
    return [max(1, min(round(x / k), m)) for x, m in zip(v, mph)]


def make_car(mass, ks, gears, fd=3.3):
    def accel(v):
        best = 0.0
        for g in range(4):
            rpm = 9.549 * v * gears[g] * fd
            if rpm > 9200:
                continue
            best = max(best, max(CT.torque_at(ROW, rpm), 0) * ks[min(g, 2)] * gears[g] * fd)
        return (best - 0.0135 * v * v - 105) / mass
    return accel


def top(acc):
    v = 100.0
    while acc(v) > 0:
        v += 0.05
    return v


R2 = [c for c in CT.R2 if c['t'] != 20]
r2_mass = st.mean(c['mass'] for c in R2)
r2_car = make_car(r2_mass, [st.mean(c['k'][j] for c in R2) for j in range(3)],
                  [st.mean(c['gears'][j] for c in R2) for j in range(4)])
cs_car = make_car(140.417, [1.9, 1.95, 2.05], [3.05, 1.68, 1.17, 0.88])
T2, TC = top(r2_car), top(cs_car)
R2G = 2.6
PARTIAL_G = 2 * 2 * 2.6 * 1100 / r2_mass / G   # front axle: 2 tires x 2.6 x 1100 lb cap, half the car's need
CSG = 2 * 2 * 7.8 * 1100 / 140.417 / G
print('CS native drone cornering %.2f g' % CSG)
print('top speeds: Rush 2 avg %.1f mph, CS native avg %.1f mph' % (T2 / MPH, TC / MPH))
print('player cornering: Rush 2 %.2f g, Partial %.2f g' % (R2G, PARTIAL_G))


def radius_at(pts, d, i, loop):
    n = len(pts)

    def walk(s):
        dist, j = 0.0, i
        while dist < 60:
            k = j + s
            if loop:
                k %= n
            if k < 0 or k >= n:
                return None
            step = d[min(j, k)] if not (loop and abs(j - k) > 1) else d[n - 1]
            if step >= 1000:
                return None
            dist += step
            j = k
        return j
    a, c = walk(-1), walk(1)
    if a is None or c is None:
        return None
    P = [pts[a], pts[i], pts[c]]
    ab = math.hypot(P[1][0] - P[0][0], P[1][2] - P[0][2])
    bc = math.hypot(P[2][0] - P[1][0], P[2][2] - P[1][2])
    ca = math.hypot(P[0][0] - P[2][0], P[0][2] - P[2][2])
    ar = abs((P[1][0] - P[0][0]) * (P[2][2] - P[0][2]) - (P[1][2] - P[0][2]) * (P[2][0] - P[0][0]))
    return None if ar < 1e-6 else ab * bc * ca / (2 * ar)


def lane_time(pts, target, acc, lat_g, loop, radii):
    n = len(pts)
    d = [math.hypot(pts[(i + 1) % n][0] - pts[i][0], pts[(i + 1) % n][2] - pts[i][2]) for i in range(n)]
    v = list(target)
    if lat_g:
        for i, r in enumerate(radii):
            if r is not None:
                v[i] = min(v[i], math.sqrt(lat_g * G * r))
    steps = n if loop else n - 1
    for rep in range(2 if loop else 1):
        for i in range(steps - 1, -1, -1):
            j = (i + 1) % n
            if d[i] < 1000:
                v[i] = min(v[i], math.sqrt(v[j] ** 2 + 2 * BRAKE * d[i]))
    for rep in range(2 if loop else 1):
        for i in range(steps):
            j = (i + 1) % n
            if d[i] < 1000:
                v[j] = min(v[j], math.sqrt(max(v[i] ** 2 + 2 * max(acc(v[i]), 0) * d[i], 0)))
    t = 0.0
    for i in range(steps):
        if d[i] < 1000:
            t += d[i] / max((v[i] + v[(i + 1) % n]) / 2, 10)
    return t


LANES = {}


def lanes_of(key, p):
    if key not in LANES:
        out = []
        for lane in p['lanes']:
            pts = [q[:3] for q in lane['points']]
            mph = [q[3] for q in lane['points']]
            if len(pts) < 10 or not all(mph):
                continue
            loop = math.hypot(pts[0][0] - pts[-1][0], pts[0][2] - pts[-1][2]) < 1000
            n = len(pts)
            d = [math.hypot(pts[(i + 1) % n][0] - pts[i][0], pts[(i + 1) % n][2] - pts[i][2]) for i in range(n)]
            radii = [radius_at(pts, d, i, loop) for i in range(n)]
            out.append((pts, mph, loop, radii, capped(pts, mph, loop)))
        LANES[key] = out
    return LANES[key]


def evaluate(key, p, drone_acc, drone_top, scale, player_acc, player_top, player_g, use_cap=False, drone_g=None):
    out = []
    for pts, mph, loop, radii, cap in lanes_of(key, p):
        lanes = cap if use_cap else mph
        td = lane_time(pts, [min(m * scale * 1.05 * MPH, drone_top) for m in lanes], drone_acc, drone_g if drone_g else 0, loop, radii)
        tp = lane_time(pts, [player_top] * len(pts), player_acc, player_g, loop, radii)
        out.append((td, tp))
    return out


def ratio(results):
    return sum(a for a, b in results) / sum(b for a, b in results)


r2 = roms.Rush2()
c = cs.CalSpeed()
r2p = [(('r2', t, b), paths.parse(r2.asset(0x57 + t + b))) for t in range(7) for b in (0, 12)]
csp = [(('cs', t), paths.parse(c.asset(cs.PATH + t))) for t in range(cs.TRACK_COUNT)]

r2res = sum((evaluate(k, p, r2_car, T2, 1.0, r2_car, T2, R2G, drone_g=R2G) for k, p in r2p), [])
base = ratio(r2res)
print('drone time / best player time (higher = drones easier)')
print('  Rush 2 circuits, Rush 2 physics:            %.3f' % base)
for t in range(7):
    print('    track %d: %.3f' % (t, ratio(sum((evaluate(k, p, r2_car, T2, 1.0, r2_car, T2, R2G, drone_g=R2G) for k, p in r2p if k[1] == t), []))))


def cs_ratio(scale, use_cap, drone, dtop, pg, tracks=None):
    return ratio(sum((evaluate(k, p, drone, dtop, scale, r2_car, T2, pg, use_cap, CSG if drone is cs_car else R2G) for k, p in csp
                      if tracks is None or k[1] in tracks), []))


print('  CS, Partial, uncapped (Full CS's lanes):      %.3f' % cs_ratio(1.0, False, cs_car, TC, PARTIAL_G))
print('  CS, Full Rush 2 (Rush 2 drones, capped):     %.3f' % cs_ratio(1.0, True, r2_car, T2, R2G))
for t in range(16):
    print('    %-9s Partial uncapped %.3f  Full Rush 2 %.3f' % (cs.PREFIXES[t], cs_ratio(1.0, False, cs_car, TC, PARTIAL_G, [t]),
                                                    cs_ratio(1.0, True, r2_car, T2, R2G, [t])))


def solve(f):
    lo, hi = 0.6, 1.2
    for _ in range(25):
        mid = (lo + hi) / 2
        if f(mid) < base:
            hi = mid
        else:
            lo = mid
    return (lo + hi) / 2


def partial_capped(corner_g, crest_g, tracks=None):
    out = []
    for k, p in csp:
        if tracks is not None and k[1] not in tracks:
            continue
        for pts, mph, loop, radii, _ in lanes_of(k, p):
            lanes = capped(pts, mph, loop, corner_g=corner_g, vertical_g=crest_g)
            td = lane_time(pts, [min(m * 1.05 * MPH, TC) for m in lanes], cs_car, CSG, loop, radii)
            tp = lane_time(pts, [T2] * len(pts), r2_car, PARTIAL_G, loop, radii)
            out.append((td, tp))
    return ratio(out)


per = [partial_capped(ARGS.corner_g, ARGS.crest_g, [t]) for t in range(16)]
print('  CS, Partial capped (corners %.2f g, crests %s): %.3f' % (ARGS.corner_g, ARGS.crest_g or 'none',
                                                                partial_capped(ARGS.corner_g, ARGS.crest_g)))
print('    ' + ', '.join('%s %.3f' % (cs.PREFIXES[t], x) for t, x in enumerate(per)))
