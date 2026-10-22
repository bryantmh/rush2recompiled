"""Reads a RUSH2_CSPHYS_SLIDES recording (src/calspeed/calspeed_physics.cpp: a player's car under California Speed's
physics, the 4 s before and 2 s after each time it starts to slide) and says what set each slide off.

    python tools/calspeed/slides.py FILE [--run N] [--rows K]

--run picks the game launch in the file (each launch starts with a '# t speed' header; default the last one). For each
slide: the trigger, found walking back from the slide's start to where the yaw rate began to build:
  landing   the car was off the road (all four wheels, surface 8) within 0.3 s before the yaw began
  rear off  the rear wheels left the road while the fronts stayed on (a crest, a crease, a lift)
  spike     a tire's load jumped past 2.5 x its value the step before while on the road
  steering  none of those: the yaw came with the steering
and K steps (default 8) from just before the trigger: speed, slip, yaw rate, steering, per wheel compression, surface,
load, road polygon, and the car's position.
"""
import argparse


def parse(path, run):
    runs, cur = [], None
    for line in open(path, encoding='utf-8'):
        line = line.rstrip('\n')
        if line.startswith('# t speed'):
            cur = []
            runs.append(cur)
        elif line.startswith('## slide'):
            cur.append({'title': line, 'rows': []})
        elif line and not line.startswith('#') and cur:
            a, b, c = line.split('|')
            h, w, p = a.split(), b.split(), c.split()
            n = len(w) // 4
            cur[-1]['rows'].append(dict(
                t=float(h[0]), v=float(h[1]), slip=float(h[2]), yaw=float(h[3]), steer=float(h[4]),
                comp=[float(w[n * i]) for i in range(4)], surf=[w[n * i + 1] for i in range(4)],
                load=[float(w[n * i + 2]) for i in range(4)],
                poly=[int(w[n * i + 5]) if n > 5 else -1 for i in range(4)], pos=[float(x) for x in p]))
    return runs[run]


def air(r, wheels):
    return all(r['surf'][i].startswith('8/') for i in wheels)


def trigger(rows, start):
    # Walk back from the slide's start while the yaw rate keeps its sign and is still building (or above 0.15).
    sign = 1 if rows[start]['yaw'] >= 0 else -1
    k = start
    while k > 0 and rows[k - 1]['yaw'] * sign > 0.15:
        k -= 1
    lo = k
    while lo > 0 and rows[k]['t'] - rows[lo - 1]['t'] <= 0.3:
        lo -= 1
    for j in range(k, lo - 1, -1):
        if air(rows[j], range(4)):
            return 'landing', j
    for j in range(lo, k + 1):
        if air(rows[j], (2, 3)) and not air(rows[j], (0, 1)):
            return 'rear off', j
    for j in range(lo + 1, k + 1):
        for i in range(4):
            if not rows[j]['surf'][i].startswith('8/') and rows[j - 1]['load'][i] > 100 and \
                    rows[j]['load'][i] > 2.5 * rows[j - 1]['load'][i]:
                return 'spike', j
    return 'steering', k


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('file')
    ap.add_argument('--run', type=int, default=-1)
    ap.add_argument('--rows', type=int, default=8)
    a = ap.parse_args()
    counts = {}
    for s in parse(a.file, a.run):
        rows = s['rows']
        start = next((i for i, r in enumerate(rows) if abs(r['slip']) > 15 and r['v'] > 40), len(rows) - 1)
        kind, j = trigger(rows, start)
        counts[kind] = counts.get(kind, 0) + 1
        print('%s  -> %s at t %.3f' % (s['title'], kind, rows[j]['t']))
        for r in rows[max(0, j - 2):j + a.rows - 2]:
            print('   t %.3f v %3.0f slip %+5.1f yaw %+5.2f st %+5.2f comp %s srf %s load %s poly %s pos %s' % (
                r['t'], r['v'], r['slip'], r['yaw'], r['steer'], ' '.join('%5.2f' % x for x in r['comp']),
                ' '.join(r['surf']), ' '.join('%4.0f' % x for x in r['load']), r['poly'],
                ' '.join('%.0f' % x for x in r['pos'])))
    print('triggers: ' + ', '.join('%s %d' % kv for kv in counts.items()))


if __name__ == '__main__':
    main()
