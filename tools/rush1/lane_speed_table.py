"""The N64 San Francisco Rush's own AI lane speeds as the custom-track converter's [ai] speed_quantiles
(src/track_mod/track_mod_convert.cpp target_speeds: a path's points ranked by the tightest corner in the next 500 ft
take the speeds at their ranks), and with --compare the radius -> speed fit beside the converter's stock table (fitted
the same way to Rush 2's and Rush 2049's lanes). The PlayStation SF Rush's courses (src/sfrush_ps1/ps1_track.cpp)
take their N64 versions' quantiles, Chinatown all seven tracks': the stock table had the computer cars 10-30 mph slow
in Golden Gate's corners.

    python tools/rush1/lane_speed_table.py [--tracks 0,1,2] [--compare]

RUSH1_ROM: the N64 ROM (tools/rush1/r1.py). Lanes are the forward races' (assets 58-64), all four of each track.
"""
import argparse, bisect, math, os, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r1 import Rush1  # noqa: E402
import paths1  # noqa: E402

STEP = 20.0      # ft between resampled points (the converter's spine_step)
AHEAD = 500.0    # ft the tightest radius is looked for ahead (the converter's)
# The converter's table radii (ft) and its stock speeds (mph), for --compare.
TABLE = [(91, 83), (114, 97), (154, 107), (205, 118), (270, 124), (347, 131), (430, 136), (538, 142), (725, 149),
         (1088, 153), (1800, 158)]


def resample(points, closed):
    """(x, z, mph) points every STEP ft along the lane (horizontal), the speed of the point each falls after."""
    out = []
    pts = points + ([points[0]] if closed else [])
    carry = 0.0
    for a, b in zip(pts, pts[1:]):
        d = math.hypot(b[0] - a[0], b[1] - a[1])
        t = carry
        while t < d:
            f = t / d
            out.append((a[0] + (b[0] - a[0]) * f, a[1] + (b[1] - a[1]) * f, a[2]))
            t += STEP
        carry = t - d
    return out


def radii(sp, closed):
    n = len(sp)
    r = []
    for i in range(n):
        ia, ic = (i - 3) % n if closed else max(i - 3, 0), (i + 3) % n if closed else min(i + 3, n - 1)
        a, b, c = sp[ia], sp[i], sp[ic]
        ab = (b[0] - a[0], b[1] - a[1]); bc = (c[0] - b[0], c[1] - b[1]); ac = (c[0] - a[0], c[1] - a[1])
        area2 = abs(ab[0] * bc[1] - ab[1] * bc[0])
        r.append(math.hypot(*ab) * math.hypot(*bc) * math.hypot(*ac) / (2 * area2) if area2 > 1e-3 else 1e9)
    k = int(AHEAD / STEP)
    return [min(r[(i + j) % n] if closed else r[min(i + j, n - 1)] for j in range(k + 1)) for i in range(n)]


ap = argparse.ArgumentParser()
ap.add_argument('--tracks', default='0,1,2,3,4,5,6')
ap.add_argument('--compare', action='store_true', help='print the stock table beside the fitted one')
a = ap.parse_args()

rom = Rush1()
rs, speeds = [], []
for t in [int(x) for x in a.tracks.split(',')]:
    lanes = paths1.lanes(rom.asset(58 + t))
    for (h, pts) in lanes:
        # Lane points: (x, y up, z) feet; speed ft/s -> mph as the recomp's port does (track1_convert.cpp lane_mph).
        p = [(q[0][0], q[0][2], max(1, (q[1] * 30 + 22) // 44)) for q in pts if q[1] > 0]
        if len(p) < 10:
            continue
        sp = resample(p, True)
        for r, q in zip(radii(sp, True), sp):
            rs.append(r)
            speeds.append(q[2])
rs.sort()
speeds.sort()
n = len(rs)
print('%d lane points over tracks %s' % (n, a.tracks))
# [ai] speed_quantiles: the lanes' speeds (every 20 ft of lane) at 0, 5, ..., 100%; the converter ranks its own path's
# points by the radius ahead and gives each the speed at its rank. The slowest is the 1st percentile: a lane's few
# near-stopped points (1 mph) aren't a corner speed.
print('speed_quantiles = [' + ', '.join('%d' % speeds[min(n - 1, int(n * max(k / 20, 0.01)))] for k in range(21)) + ']')
if a.compare:
    for r, s in TABLE:
        q = bisect.bisect_left(rs, r) / n
        print('  radius %5d ft: quantile %.2f, SF Rush %3d mph, stock %3d mph' % (r, q, speeds[min(n - 1, int(q * n))], s))
