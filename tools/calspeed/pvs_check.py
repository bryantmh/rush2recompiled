#!/usr/bin/env python3
"""Checks California Speed tracks' visibility regions as Rush 2 and California Speed each choose them (research aid,
docs/calspeed_research.md#visibility-regions).

For points along each track's AI lanes (camera a few feet over the road), works out the camera's region with Rush 2's
rule (pvs_choose_camera_region_8007C06C: box bottom <= dy, no top test) and California Speed's (func_800BFD6C: the
camera inside the box on all three axes), and how many of the track's sections each region's mask hides, including
the section the camera is in. Lists the places where the two rules disagree.

    python tools/calspeed/pvs_check.py [TRACK...] [--height FT] [--all] [--runs]   TRACK = 0-15 (California Speed's order)

--runs lists each lane's stretches where Rush 2's rule hides the camera's section (lane, first and last point, length,
middle point and heading): spots to put a car for an in-game check (RUSH2_CS_PLACE).
"""
import argparse, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
import cs
import paths
import placement

PVS_COUNTS, PVS_TABLES = 0x800C4C80, 0x800C0680


def regions(recs, count):
    """The first `count` records of the top-level chain (the visibility regions, in order)."""
    out, i = [], 0
    while i >= 0 and len(out) < count:
        out.append(recs[i])
        i = recs[i].next
    return out


def score(r, cam, top_test):
    dx, dy, dz = (cam[k] - r.pos[k] for k in range(3))
    lo, hi = r.bbox[0:3], r.bbox[3:6]
    if not (lo[0] <= dx <= hi[0] and lo[2] <= dz <= hi[2] and lo[1] <= dy):
        return None
    if top_test and dy > hi[1]:
        return None
    s = math.sqrt(dx * dx + dz * dz)
    if hi[1] * 0.75 < dy:
        s += dy - lo[1]
    return s


def choose(regs, cam, top_test):
    best, best_s = -1, None
    for i, r in enumerate(regs):
        s = score(r, cam, top_test)
        if s is not None and (best < 0 or s < best_s):
            best, best_s = i, s
    return best


def hidden(masks, region, n):
    if region < 0:
        return set()
    # Two big-endian u64: section k's bit is bit k of the first for k < 64, then 64-127; a clear bit hides it.
    m = masks[region]
    return {k for k in range(n) if not m[7 - k // 8 if k < 64 else 15 - (k - 64) // 8] >> (k % 8) & 1}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tracks', nargs='*', type=int)
    ap.add_argument('--height', type=float, default=8.0, help='camera height over the lane point (ft)')
    ap.add_argument('--all', action='store_true', help='list every disagreeing point, not the first few per track')
    ap.add_argument('--runs', action='store_true', help="list the lanes' stretches Rush 2's rule blanks")
    a = ap.parse_args()
    c = cs.CalSpeed()
    for t in a.tracks or range(cs.TRACK_COUNT):
        count = c.read(PVS_COUNTS + t, 1)[0]
        if count == 0:
            continue
        first = sum(c.read(PVS_COUNTS + k, 1)[0] for k in range(t))
        raw = c.read(PVS_TABLES + first * 16, count * 16)
        masks = [raw[i * 16:i * 16 + 16] for i in range(count)]
        _, recs = placement.parse_r2(c.asset(cs.PLACEMENT + t))
        regs = regions(recs, count)
        p = paths.parse(c.asset(cs.PATH + t))
        pts = [(x, y + a.height, z) for lane in p['lanes'] for (x, y, z, _, _) in lane['points']]
        differ = blank_r2 = blank_cs = 0
        shown = 0
        for cam in pts:
            r2 = choose(regs, cam, False)
            rc = choose(regs, cam, True)
            h2, hc = hidden(masks, r2, count), hidden(masks, rc, count)
            if r2 != rc:
                differ += 1
            if rc >= 0 and rc in h2 and r2 != rc:
                blank_r2 += 1
                if shown < 8 or a.all:
                    shown += 1
                    print('  %-12s cam (%.0f, %.0f, %.0f): Rush 2 region %d hides %d sections incl. the camera\'s %d; '
                          'California Speed region %d hides %d' % (cs.PREFIXES[t], *cam, r2, len(h2), rc, rc, len(hc)))
            if rc >= 0 and rc in hc:
                blank_cs += 1
        if a.runs:
            for li, lane in enumerate(p['lanes']):
                lp = lane['points']
                bad = []
                for x, y, z, _, _ in lp:
                    cam = (x, y + a.height, z)
                    r2, rc = choose(regs, cam, False), choose(regs, cam, True)
                    bad.append(rc >= 0 and r2 != rc and rc in hidden(masks, r2, count))
                i = 0
                while i < len(lp):
                    if not bad[i]:
                        i += 1
                        continue
                    j = i
                    while j + 1 < len(lp) and bad[j + 1]:
                        j += 1
                    m = (i + j) // 2
                    n = lp[min(m + 1, len(lp) - 1)]
                    dx, dz = n[0] - lp[m][0], n[2] - lp[m][2]
                    d = math.hypot(dx, dz) or 1.0
                    print('  lane %d points %d-%d (%d): middle (%d, %d, %d) heading (%.2f, %.2f)' %
                          (li, i, j, j - i + 1, lp[m][0], lp[m][1], lp[m][2], dx / d, dz / d))
                    i = j + 1
        print('%2d %-9s %3d regions, %5d points: rules differ at %d, Rush 2 hides the camera\'s section at %d '
              '(California Speed at %d)' % (t, cs.PREFIXES[t], count, len(pts), differ, blank_r2, blank_cs))


if __name__ == '__main__':
    main()
