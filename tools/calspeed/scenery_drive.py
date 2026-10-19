#!/usr/bin/env python3
"""California Speed's extended scenery in game: backs a car from the grid into the road behind the start (straight,
and in arcs to either side toward the sweep's edges) on each track with tools/test_race.py, and reports how far it
got, its lowest point against the grid's and wrecks: a car through the collision falls (a big drop, then the reset's
wreck). docs/calspeed_research.md#extended-scenery.

    python tools/calspeed/scenery_drive.py [TRACK ...] [--moves back,left,right] [--off] [--out DIR]

TRACK is California Speed's 0-15 (default: every point-to-point track); --off runs with the scenery off for a
baseline. Runs are silent sim races with their own data folder (tmp/scenery_drive/data).
"""
import argparse, json, math, os, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
POINT_TO_POINT = [0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11]
# Reverse is C-down + A; SL/SR the stick.
MOVES = {
    'back': '8:A:7,8:CD:7',
    'left': '8:A:7,8:CD:7,9.9:SL:0.5',
    'right': '8:A:7,8:CD:7,9.9:SR:0.5',
}


def run(track, move, on, out):
    cmd = [sys.executable, os.path.join(REPO, 'tools', 'test_race.py'), str(64 + track), '--cars', '0',
           '--games', f'calspeed_scenery={"true" if on else "false"}', '--script', MOVES[move], '--seconds', '17',
           '--data', os.path.join(REPO, 'tmp', 'scenery_drive', 'data'), '--out', out]
    try:
        subprocess.run(cmd, capture_output=True, text=True, timeout=100)
    except subprocess.TimeoutExpired:
        return None
    path = os.path.join(out, 'samples.jsonl')
    if not os.path.exists(path):
        return None
    pts = []
    for line in open(path):
        d = json.loads(line)
        if d.get('state') != 3:
            continue
        c = d['list'][0]
        pts.append((d['time'], c['pos'], c['wreck']))
    if not pts:
        return None
    grid = pts[0][1]
    far = max(math.dist((p[0], p[2]), (grid[0], grid[2])) for _, p, _ in pts)
    low = min(p[1] for _, p, _ in pts) - grid[1]
    wrecks = sum(1 for i in range(1, len(pts)) if pts[i][2] and not pts[i - 1][2])
    return far, low, wrecks, pts[-1][1]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tracks', nargs='*', type=int)
    ap.add_argument('--moves', default='back,left,right')
    ap.add_argument('--off', action='store_true')
    ap.add_argument('--out', default=os.path.join(REPO, 'tmp', 'scenery_drive'))
    a = ap.parse_args()
    for t in a.tracks or POINT_TO_POINT:
        for move in a.moves.split(','):
            out = os.path.join(a.out, f'{t}_{move}{"_off" if a.off else ""}')
            r = run(t, move, not a.off, out)
            if r is None:
                print(f'track {t:2} {move:5}: no race (see {out})', flush=True)
                continue
            far, low, wrecks, end = r
            flag = '  FELL?' if low < -15 or wrecks else ''
            print(f'track {t:2} {move:5}: {far:6.0f} ft from the grid at most, lowest {low:7.1f} ft, wrecks {wrecks}, '
                  f'ends at ({end[0]:.0f} {end[1]:.0f} {end[2]:.0f}){flag}', flush=True)


if __name__ == '__main__':
    main()
