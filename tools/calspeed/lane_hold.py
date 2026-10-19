"""How well computer cars hold their AI lanes in race recordings (test aid for California Speed's lane speeds,
docs/calspeed_research.md#5-paths; record races with tools/calspeed/sim_wrecks.py).

    python tools/calspeed/lane_hold.py REC.r2rc [REC.r2rc ...] [--spots] [--wrecks]

The path is the Rush 2 (assets 0x57-0x6E) or California Speed (assets 110-125) path whose lanes the cars drive
nearest. Per recording: every computer car's distance from the nearest lane (x/z, any of the 4: median, 90th
percentile, share of time past 15 ft), lateral g it pulls (99th percentile, from its velocity between physics steps),
wrecks, and race time (from the .done file). --spots lists each time a car runs more than 15 ft wide (once per 3 s
per car): where, how far, at what speed, and its lane point's speed (California Speed's own mph). --wrecks lists
every wreck: where, and the car's speed, distance off the lane and up vector 1 s before.
CALSPEED_ROM and RUSH2_ROM name the ROMs (defaults: build/calspeed.z64 and the repo's rush2.us.recomp.z64).
"""
import json, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'rush2049'))
os.environ.setdefault('CALSPEED_ROM', os.path.join(REPO, 'build', 'calspeed.z64'))
import paths, roms, cs, sim_wrecks

WIDE = 15.0
CELL = 100


def lane_segments(p):
    out = []
    for lane in p['lanes']:
        pts = lane['points']
        for i in range(len(pts) - 1):
            a, b = pts[i], pts[i + 1]
            if math.hypot(b[0] - a[0], b[2] - a[2]) < 1000:
                out.append((a[0], a[2], b[0], b[2], a[3]))
    grid = {}
    for s in out:
        for gx in range(int(min(s[0], s[2]) // CELL) - 1, int(max(s[0], s[2]) // CELL) + 2):
            for gz in range(int(min(s[1], s[3]) // CELL) - 1, int(max(s[1], s[3]) // CELL) + 2):
                grid.setdefault((gx, gz), []).append(s)
    return grid


def nearest(grid, x, z):
    best = (999.0, None)
    for s in grid.get((int(x // CELL), int(z // CELL)), []):
        dx, dz = s[2] - s[0], s[3] - s[1]
        t = max(0.0, min(1.0, ((x - s[0]) * dx + (z - s[1]) * dz) / ((dx * dx + dz * dz) or 1)))
        d = math.hypot(x - (s[0] + dx * t), z - (s[1] + dz * t))
        if d < best[0]:
            best = (d, s)
    return best


_paths = []
def candidates():
    if not _paths:
        r2 = roms.Rush2()
        for i in range(24):
            _paths.append(('Rush 2 asset %#x' % (0x57 + i), lane_segments(paths.parse(r2.asset(0x57 + i)))))
        c = cs.CalSpeed()
        for t in range(cs.TRACK_COUNT):
            _paths.append(('California Speed ' + cs.PREFIXES[t], lane_segments(paths.parse(c.asset(cs.PATH + t)))))
    return _paths


def analyze(rec, spots, wrecks_list=False):
    head, fields, n, width, rows, vals = sim_wrecks.read_record(rec)
    fi = {f: i for i, f in enumerate(fields)}
    def v(r, c, f): return vals[r * width + 2 + c * len(fields) + fi[f]]
    drones = [c for c in range(n) if head['kinds'][c] == 1]
    sample = [(v(r, c, 'x'), v(r, c, 'z')) for r in range(0, rows, 600) for c in drones]
    name, grid = min(candidates(), key=lambda p: sorted(nearest(p[1], x, z)[0] for x, z in sample)[len(sample) // 2])
    times = [None] * n
    if os.path.exists(rec + '.done'):
        with open(rec + '.done', encoding='utf-8') as f:
            times = json.load(f).get('finish_times') or times
    print('%s: %s, %.0f s recorded' % (os.path.basename(rec), name, rows / 60.0))
    total = dict(wide=0, wrecks=0, n=0)
    for c in drones:
        ds, gs, wrecks, was, last_spot, prev = [], [], 0, False, -999, None
        for r in range(rows):
            wreck = v(r, c, 'wreck') != 0 and not v(r, c, 'finished')   # not after the finish (a road's end)
            wrecks += wreck and not was
            if wrecks_list and wreck and not was:
                b = max(0, r - 60)
                d, s = nearest(grid, v(b, c, 'x'), v(b, c, 'z'))
                print('    car %d wrecked at %5.0f s at (%.0f, %.0f, %.0f); 1 s before: %3.0f mph, %3.0f ft off the lane, '
                      'up y %.2f' % (c, r / 60.0, v(r, c, 'x'), v(r, c, 'y'), v(r, c, 'z'), v(b, c, 'speed') / 1.46667,
                                     d, v(b, c, 'uy')))
            was = wreck
            if wreck or v(r, c, 'finished'):
                prev = None
                continue
            clock = vals[r * width + 1]
            vel = (v(r, c, 'vx'), v(r, c, 'vz'))
            if prev is None:
                prev = (clock, vel)
            elif clock - prev[0] >= 0.03:
                dt = clock - prev[0]
                sp = math.hypot(*vel)
                if sp > 60:
                    ax, az = (vel[0] - prev[1][0]) / dt, (vel[1] - prev[1][1]) / dt
                    gs.append(abs(vel[0] * az - vel[1] * ax) / sp / 32.17)
                prev = (clock, vel)
            if r % 6:
                continue
            x, z = v(r, c, 'x'), v(r, c, 'z')
            d, s = nearest(grid, x, z)
            ds.append(d)
            if spots and d > WIDE and r - last_spot > 180:
                last_spot = r
                print('    car %d at %5.0f s: %4.0f ft wide at (%.0f, %.0f, %.0f), %3.0f mph, lane %s mph' % (
                    c, r / 60.0, d, x, v(r, c, 'y'), z, v(r, c, 'speed') / 1.46667, s[4] if s else '-'))
        ds.sort()
        gs.sort()
        m = len(ds) or 1
        wide = sum(d > WIDE for d in ds) / m * 100
        total['wide'] += wide
        total['wrecks'] += wrecks
        total['n'] += 1
        t = times[c] if c < len(times) else None
        print('  car %d type %2d  lane distance median %4.1f p90 %4.1f  past %.0f ft %4.1f%%  lateral g p99 %.2f  '
              'wrecks %d  time %s' % (c, head['types'][c], ds[m // 2] if ds else 0, ds[int(m * .9)] if ds else 0, WIDE,
                                      wide, gs[int(len(gs) * .99)] if gs else 0, wrecks,
                                      '%.1f s' % t if t is not None else '-'))
    finished = [t for c, t in enumerate(times) if c in drones and t is not None]
    print('  all: past %.0f ft %.1f%% of the time, %d wrecks, best time %s' % (
        WIDE, total['wide'] / max(1, total['n']), total['wrecks'], '%.1f s' % min(finished) if finished else '-'))
    return total


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    for rec in args:
        analyze(rec, '--spots' in sys.argv, '--wrecks' in sys.argv)
