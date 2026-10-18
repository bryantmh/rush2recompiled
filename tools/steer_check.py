"""Summarizes the player car (car 0) of a race_watch run (test aid for car handling, e.g. rollovers): speed, heading
change and how upright it stayed through the race, from DIR/samples.jsonl.

    python tools/steer_check.py tmp/steer_47 [tmp/steer_36 ...]

up is row 1 y of the car's world -> car matrix (+0x2CC; logged by race_watch since it gained the field): 1 upright,
0 on its side, below 0 upside down.
"""
import json, math, os, sys

for d in sys.argv[1:]:
    rows = [json.loads(l) for l in open(os.path.join(d, 'samples.jsonl'))]
    start = max(i for i, r in enumerate(rows) if r['state'] == 3 and (i == 0 or rows[i - 1]['state'] != 3))
    race = rows[start:]
    top, turn, ups, wrecks, prev_heading, prev = 0.0, 0.0, [], 0, None, None
    for r in race:
        c = r['list'][0]
        if prev is not None and r['time'] > prev[0]:
            dx, dz = c['pos'][0] - prev[1][0], c['pos'][2] - prev[1][2]
            v = math.hypot(dx, dz) / (r['time'] - prev[0])
            if v < 400:
                top = max(top, v)
                if v > 20:
                    h = math.atan2(dx, dz)
                    if prev_heading is not None:
                        turn += abs((h - prev_heading + math.pi) % (2 * math.pi) - math.pi)
                    prev_heading = h
        if 'up' in c:
            ups.append(c['up'])
        wrecks += 1 if c['wreck'] and (prev is None or not prev[2]) else 0
        prev = (r['time'], c['pos'], c['wreck'])
    print('%s: race %.0f s, top %.0f ft/s, turned %.0f deg, wrecks %d, lowest up %s, time on its side or worse %s' % (
        d, race[-1]['time'] - race[0]['time'], top, math.degrees(turn), wrecks,
        '%.2f' % min(ups) if ups else '-', '%.1f s' % (sum(1 for u in ups if u < 0.3) * 0.1) if ups else '-'))
