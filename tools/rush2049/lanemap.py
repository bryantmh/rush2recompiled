"""Car Speeds lane maps: Rush 2049 AI lane speeds onto Rush 2's speed profile, and back (docs/rush2049_research/cars.md §10).

Each speed maps to the other game's speed at the same percentile of the race paths' lane speeds (all four lanes,
weighted by distance): Rush 2's 7 circuit tracks and Rush 2049's 6 race tracks, both directions. Prints the two
256-entry tables for src/car2049.cpp and Rush 2's mean lane 0 speed after the Rush 2 -> 2049 map (the race time's
baseline, src/track2049.cpp).

Usage: python lanemap.py
"""
import bisect, math, sys
import roms, paths


def samples(files):
    """(speed, distance) per lane point, all four lanes of every file."""
    out = []
    for data in files:
        p = paths.parse(data)
        for lane in p['lanes']:
            pts = lane['points']
            for a, b in zip(pts, pts[1:] + pts[:1]):
                if a[3] > 0:
                    out.append((a[3], math.dist(a[:3], b[:3])))
    return out


class Profile:
    """Distance-weighted distribution of lane speeds."""
    def __init__(self, s):
        weight = {}
        for v, d in s:
            weight[v] = weight.get(v, 0.0) + d
        self.speeds = sorted(weight)
        total = sum(weight.values())
        # Mid-rank percentile of each speed, so equal speeds map to the middle of their share.
        self.pct = []
        acc = 0.0
        for v in self.speeds:
            self.pct.append((acc + weight[v] / 2) / total)
            acc += weight[v]

    def percentile(self, v):
        s, p = self.speeds, self.pct
        if v <= s[0]:
            return p[0] * v / s[0]
        if v >= s[-1]:
            return p[-1]
        i = bisect.bisect_right(s, v)
        t = (v - s[i - 1]) / (s[i] - s[i - 1])
        return p[i - 1] + t * (p[i] - p[i - 1])

    def speed(self, q):
        s, p = self.speeds, self.pct
        if q <= p[0]:
            return s[0] * q / p[0] if p[0] > 0 else s[0]
        if q >= p[-1]:
            return s[-1]
        i = bisect.bisect_right(p, q)
        t = (q - p[i - 1]) / (p[i] - p[i - 1])
        return s[i - 1] + t * (s[i] - s[i - 1])


TAIL = 0.05   # Below the 5th and above the 95th percentile the samples are few (one long 247 mph stretch is 2049's
              # top 1.9%), so speeds there scale with the two profiles' speeds at those percentiles instead.


def table(src, dst):
    """u8 speed in src's profile -> dst's speed at the same percentile, proportional in the tails."""
    lo_src, lo_dst = src.speed(TAIL), dst.speed(TAIL)
    hi_src, hi_dst = src.speed(1 - TAIL), dst.speed(1 - TAIL)
    out = [0]
    for v in range(1, 256):
        if v <= lo_src:
            m = v * lo_dst / lo_src
        elif v >= hi_src:
            m = v * hi_dst / hi_src
        else:
            m = dst.speed(src.percentile(v))
        out.append(max(1, min(255, int(round(m)))))
    return out


def lane0_mean(files, mapping=None):
    length = time = 0.0
    for data in files:
        pts = paths.parse(data)['lanes'][0]['points']
        for a, b in zip(pts, pts[1:] + pts[:1]):
            v = a[3] if mapping is None else mapping[a[3]]
            if v > 0:
                d = math.dist(a[:3], b[:3])
                length += d
                time += d / v
    return length / time


def main():
    r2, r49 = roms.Rush2(), roms.Rush2049()
    files2 = [r2.asset(0x57 + t) for t in list(range(7)) + list(range(12, 19))]
    files49 = [r49.file(k) for k in list(range(158, 164)) + list(range(177, 183))]
    p2, p49 = Profile(samples(files2)), Profile(samples(files49))
    to_rush2, to_2049 = table(p49, p2), table(p2, p49)
    for name, t in (('lane_to_rush2', to_rush2), ('lane_to_2049', to_2049)):
        print('    constexpr uint8_t %s[256] = {' % name)
        for i in range(0, 256, 16):
            print('        ' + ', '.join('%3d' % x for x in t[i:i + 16]) + ',')
        print('    };')
    print('Rush 2 lane 0 mean %.1f, after lane_to_2049 %.1f; 2049 lane 0 mean %.1f, after lane_to_rush2 %.1f' % (
        lane0_mean(files2), lane0_mean(files2, to_2049), lane0_mean(files49), lane0_mean(files49, to_rush2)))
    for v in (100, 120, 140, 150, 160, 170, 180, 190, 200, 247):
        print('2049 %3d -> Rush 2 %3d (x%.3f)' % (v, to_rush2[v], to_rush2[v] / v))
    return 0


if __name__ == '__main__':
    sys.exit(main())
