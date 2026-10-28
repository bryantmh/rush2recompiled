#!/usr/bin/env python3
"""The road a traced car felt, from a game log with RUSH2_SMOOTH_TRACE=<car address, hex> (src/smooth_collision.cpp
prints every wheel probe of that car: wheel, road point x y z, polygon flags, height above the road). Run a race with
tools/test_race.py --env RUSH2_SMOOTH_TRACE=800F6CC4 (a computer car; 800F5470 is player 1) and pass its run.log.

    python tools/calspeed/road_trace.py RUNLOG [RUNLOG ...]

Per log: the road's vertical kicks (second difference of the road point's height over one physics step, 1/60 s, in g)
on spline road (poly flag 0x1000), flat floor and joins between the two, and the wheels' height swing above the road
(their mean per step about a 1 s mean: the body bouncing on its springs) with its strongest frequency.
Compare runs with RUSH2_SPLINE_FACE=1 / RUSH2_SMOOTH_FILLET=0 for the old surfaces.
docs/calspeed_research.md, "Spline lift" and "Crease fillets".
"""
import sys
import numpy as np


def kicks(rows):
    by = {}
    for r in rows:
        by.setdefault(int(r[0]), []).append((float(r[1]), float(r[2]), float(r[3]), int(r[4], 16)))
    out = {'spline': [], 'flat': [], 'join': []}
    for v in by.values():
        a = np.array([r[:3] for r in v])
        fl = np.array([r[3] for r in v])
        step = np.linalg.norm(np.diff(a[:, [0, 2]], axis=0), axis=1)
        d2 = (a[2:, 1] - 2 * a[1:-1, 1] + a[:-2, 1]) * 3600.0 / 32.2
        moving = (step[1:] > 0.5) & (step[:-1] > 0.5) & (step[1:] < 20) & (step[:-1] < 20)
        sp = [(fl[k:len(fl) - 2 + k] & 0x1000) != 0 for k in range(3)]
        kind = np.where(sp[0] & sp[1] & sp[2], 'spline', np.where(~sp[0] & ~sp[1] & ~sp[2], 'flat', 'join'))
        for k in out:
            out[k].extend(np.abs(d2[moving & (kind == k)]))
    return out


def swing(rows):
    steps, cur = [], {}
    for r in rows:
        w = int(r[0])
        if w in cur:
            steps.append(cur)
            cur = {}
        cur[w] = (float(r[5]), float(r[1]), float(r[3]))
    full = [s for s in steps if len(s) == 4]
    hs = np.array([np.mean([v[0] for v in s.values()]) for s in full])
    pos = np.array([list(s.values())[0][1:] for s in full])
    sp = np.r_[0, np.linalg.norm(np.diff(pos, axis=0), axis=1)]
    ok = (sp > 1) & (sp < 20) & (np.abs(hs) < 3)
    d = (hs - np.convolve(hs, np.ones(60) / 60, 'same'))[ok]
    x = hs[ok] - hs[ok].mean()
    f = np.abs(np.fft.rfft(x * np.hanning(len(x))))
    fr = np.fft.rfftfreq(len(x), 1 / 60.0)
    band = (fr > 0.3) & (fr < 15)
    return np.sqrt((d ** 2).mean()), fr[band][np.argmax(f[band])]


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for log in sys.argv[1:]:
        rows = [l.split()[1:] for l in open(log, errors='replace') if l.startswith('[trace]') and len(l.split()) >= 7]
        if not rows:
            print('%s: no [trace] lines (run with RUSH2_SMOOTH_TRACE=<car>)' % log)
            continue
        print(log)
        for k, v in kicks(rows).items():
            v = np.array(v)
            if len(v):
                print('  %-6s %6d steps  mean %5.2f g  95%% %5.1f  over 10 g %5.2f%%  max %.0f' % (
                    k, len(v), v.mean(), np.percentile(v, 95), 100 * (v > 10).mean(), v.max()))
        rms, peak = swing(rows)
        print('  wheels\' height swing RMS %.3f ft, strongest at %.2f Hz' % (rms, peak))


if __name__ == '__main__':
    main()
