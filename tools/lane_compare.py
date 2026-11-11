"""Lines a converted course's AI lanes up against the N64 SF Rush's lanes for the same course and reports how far
apart they run and their speeds, by part of the lap. Answers "do the PS1's lanes follow the N64's line, and at what
speeds" (docs/sfrush_ps1.md#collision); the ground under them is tools/lane_surfaces.py.

    python tools/lane_compare.py --n64 0 --rush2 tmp/gg_out3_night [--bins 40]

The two are fitted by a 2D rigid (optionally mirrored) transform over the lanes' points (ICP from 24 start angles),
then each converted lane point is matched to the nearest N64 lane point. Printed per lap bin (of the converted lane
0): the median distance to the N64's nearest lane, and both games' median lane speeds there (mph).
"""
import argparse, math, os, struct, sys
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'rush1'))
sys.path.insert(0, os.path.join(HERE, 'rush2049'))


def n64_lanes(track):
    """[(x, z, mph)] per lane in tools/lane_surfaces.py's N64 frame."""
    from r1 import Rush1
    import paths1
    rom = Rush1()
    out = []
    for _, pts in paths1.lanes(rom.asset(58 + track)):
        out.append([(q[0][0], q[0][2], max(1, (q[1] * 30 + 22) // 44) if q[1] > 0 else 0) for q in pts])
    return out


def rush2_lanes(folder):
    import paths
    p = paths.parse(open(os.path.join(folder, 'path.bin'), 'rb').read())
    return [[(q[0], q[2], q[3]) for q in lane['points']] for lane in p['lanes'] if lane['count']]


def fit(src, dst):
    """Rigid 2D transform (R, t, mirrored) taking src points onto dst by ICP; returns it and the mean error."""
    best = None
    for mirror in (False, True):
        s0 = src * ([1, -1] if mirror else [1, 1])
        for k in range(24):
            a = 2 * math.pi * k / 24
            R = np.array([[math.cos(a), -math.sin(a)], [math.sin(a), math.cos(a)]])
            t = dst.mean(0) - s0.mean(0) @ R.T
            for _ in range(40):
                moved = s0 @ R.T + t
                d2 = ((moved[:, None, :] - dst[None, :, :]) ** 2).sum(2)
                nn = dst[d2.argmin(1)]
                # Procrustes of s0 onto nn.
                ms, md = s0.mean(0), nn.mean(0)
                U, _, Vt = np.linalg.svd((s0 - ms).T @ (nn - md))
                R = (U @ Vt).T
                if np.linalg.det(R) < 0:
                    Vt[1] *= -1
                    R = (U @ Vt).T
                t = md - ms @ R.T
            err = np.sqrt(d2.min(1)).mean()
            if best is None or err < best[0]:
                best = (err, R, t, mirror)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--n64', type=int, required=True)
    ap.add_argument('--rush2', required=True)
    ap.add_argument('--bins', type=int, default=40)
    a = ap.parse_args()
    n = n64_lanes(a.n64)
    r = rush2_lanes(a.rush2)
    N = np.array([(p[0], p[1]) for lane in n for p in lane], float)
    Nmph = np.array([p[2] for lane in n for p in lane], float)
    sub = lambda arr, k: arr[::max(1, len(arr) // k)]
    err, R, t, mirror = fit(sub(np.array([(p[0], p[1]) for p in r[0]], float), 400), sub(N, 1500))
    print('fit: mean %.1f ft apart, rotation %.1f deg, mirrored %s' % (err, math.degrees(math.atan2(R[1, 0], R[0, 0])), mirror))
    tr = lambda P: (P * ([1, -1] if mirror else [1, 1])) @ R.T + t
    lane0 = r[0]
    seg = [math.dist(lane0[i][:2], lane0[(i + 1) % len(lane0)][:2]) for i in range(len(lane0))]
    total = sum(seg)
    frac = np.cumsum([0] + seg[:-1]) / total
    print('converted lane 0 %.0f ft; N64 lanes %s ft' % (total, ', '.join(
        '%.0f' % sum(math.dist(l[i][:2], l[(i + 1) % len(l)][:2]) for i in range(len(l))) for l in n)))
    print('  lap %      at (conv. x, z)    off N64 line ft (lanes 0-3)   speed mph conv / N64')
    for b in range(a.bins):
        sel = [i for i in range(len(lane0)) if b / a.bins <= frac[i] < (b + 1) / a.bins]
        if not sel:
            continue
        p0 = lane0[sel[0]]
        offs, sp_r, sp_n = [], [], []
        for lane in r:
            P = np.array([(lane[i][0], lane[i][1]) for i in sel if i < len(lane)], float)
            if not len(P):
                continue
            d2 = ((tr(P)[:, None, :] - N[None, :, :]) ** 2).sum(2)
            offs.append(np.median(np.sqrt(d2.min(1))))
            sp_r += [lane[i][2] for i in sel if i < len(lane)]
            sp_n += list(Nmph[d2.argmin(1)])
        print('  %4.1f-%4.1f  (%6.0f, %6.0f)   %s   %4.0f / %4.0f' % (
            100 * b / a.bins, 100 * (b + 1) / a.bins, p0[0], p0[1], ' '.join('%5.0f' % o for o in offs),
            np.median(sp_r), np.median(sp_n)))


if __name__ == '__main__':
    main()
