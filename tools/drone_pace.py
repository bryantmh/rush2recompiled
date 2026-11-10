"""Summarizes the computer cars of race_watch / test_race runs, to compare their pace between runs (e.g. a track's snow
version against its night one): per run, the drones' mean distance covered in the race, their wrecks, and the share of
race time they spent braking above 100 mph (braking on straights shows here).

    python tools/drone_pace.py tmp/pace_market_snow tmp/pace_market_night [...]

Distance is summed from DIR/samples.jsonl positions (horizontal, steps faster than 400 ft/s dropped as resets).
"""
import json, math, os, sys


def summarize(d):
    rows = [json.loads(l) for l in open(os.path.join(d, 'samples.jsonl'))]
    race = [r for r in rows if r['state'] == 3 and r['list']]
    if not race:
        return None
    t0 = race[0]['time']
    cars = {}
    prev = None
    for r in race:
        for k, c in enumerate(r['list']):
            if c.get('human'):
                continue
            s = cars.setdefault(k, {'dist': 0.0, 'wrecks': 0, 'brake_fast': 0.0, 'time': 0.0, 'top': 0.0})
            if prev is not None and k < len(prev['list']):
                p = prev['list'][k]
                dt = r['time'] - prev['time']
                if dt > 0:
                    v = math.hypot(c['pos'][0] - p['pos'][0], c['pos'][2] - p['pos'][2]) / dt
                    if v < 400:
                        s['dist'] += v * dt
                        s['top'] = max(s['top'], v / 1.4667)
                        s['time'] += dt
                        if c['brake'] > 0.5 and v / 1.4667 > 100:
                            s['brake_fast'] += dt
                if c['wreck'] and not p['wreck']:
                    s['wrecks'] += 1
        prev = r
    secs = race[-1]['time'] - t0
    n = max(len(cars), 1)
    return {
        'seconds': secs,
        'drones': len(cars),
        'dist': sum(s['dist'] for s in cars.values()) / n,
        'wrecks': sum(s['wrecks'] for s in cars.values()),
        'brake_fast': sum(s['brake_fast'] for s in cars.values()) / max(sum(s['time'] for s in cars.values()), 1e-6),
        'top': max((s['top'] for s in cars.values()), default=0.0),
    }


for d in sys.argv[1:]:
    s = summarize(d)
    if s is None:
        print('%s: no race samples' % d)
        continue
    print('%s: %.0f s, %d drones, mean %.0f ft, %d wrecks, braking above 100 mph %.1f%% of the time, top %.0f mph' %
          (d, s['seconds'], s['drones'], s['dist'], s['wrecks'], 100 * s['brake_fast'], s['top']))
