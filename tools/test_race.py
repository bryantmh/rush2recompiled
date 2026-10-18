#!/usr/bin/env python3
"""Starts a race straight from the title screen (RUSH2_TEST_RACE, src/test_race.cpp) in a hidden, silent game and
reports how it went: when it loaded and started, each human car's type and how far it drove, and whether the game
got past the race (test aid; docs/menus.md#starting-a-race-without-the-menus).

    python tools/test_race.py TRACK [--cars T,T,...] [--laps N] [--drones N] [--script S] [--seconds S]
                              [--turbo] [--finish S] [--mod NAME] [--out DIR] [--env K=V ...]

TRACK is a track select id (0-11 Rush 2, 12-17 Rush 2049, 18-24 SF Rush, 25-28 stunt arenas, 29 obstacle course,
30-37 battle arenas, 38-63 custom tracks, 64-79 California Speed) or a custom track's folder name. --cars gives one
car type per player (1-4 players; Rush 2 0-21, 2049 23-35, California Speed 36-56, mod cars 57-72). --script is the
--input-script (default: gas held from 1 s), counted from launch: the race is up about 5 s in.

The game runs as a sim run (RUSH2_SIM=1, src/sim_run.cpp): hidden, deaf to real controllers, no audio, from a data
folder of its own (tmp/test_race/data), so the player's settings and saves are never read or written. Before each run
its settings and saves are cleared (its caches are kept), the ROMs in build/ are hard-linked in, build/games.json is
copied (the same games on), and the mods enabled in build/mods.json are linked in and enabled, plus --mod ones and
the mod holding a TRACK folder. Real time by default; --turbo runs the race as fast as the machine goes from its first
tick (the script's presses stop then). --finish S finishes the players' cars S seconds into the race
(RUSH2_TEST_FINISH) to reach the race-over screens. The run is closed after --seconds (by its PID). race_watch.py
does the watching: DIR/samples.jsonl, DIR/events.txt, DIR/run.log (the game's output).
"""
import argparse, json, math, os, shutil, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
BUILD = os.path.join(REPO, 'build')
DATA = os.path.join(REPO, 'tmp', 'test_race', 'data')
#: ROMs the game reads from its data folder (src/data_location.cpp data_files).
ROMS = ['rush2.n64.us.z64', 'rush2049.z64', 'rush2049_dc.pak', 'rush1.z64', 'calspeed.z64']
#: Settings and saves, cleared before each run (caches such as track_cache are kept).
RESET = ['saves', 'mod_config', 'ghosts', 'track_reports']
STATES = {0: 'front end', 1: 'track select', 2: 'car select', 3: 'racing', 6: 'post-race', 7: 'race start',
          9: 'countdown', 10: 'race intro'}


def link(src, dst):
    if os.path.isdir(src):
        r = subprocess.run(['cmd', '/c', 'mklink', '/J', dst, src], capture_output=True, text=True)
        if r.returncode != 0:
            raise OSError('mklink failed: ' + r.stdout + r.stderr)
        return
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def remove_link(path):
    """Removes a junction, link or linked file without touching what it points to."""
    if os.path.isjunction(path) or os.path.islink(path):
        os.rmdir(path) if os.path.isdir(path) else os.unlink(path)
    elif os.path.isfile(path):
        os.remove(path)


def mod_name(entry):
    return entry[:-4] if entry.endswith('.rtz') else entry


def prepare_data(track, extra_mods):
    os.makedirs(DATA, exist_ok=True)
    for n in os.listdir(DATA):
        p = os.path.join(DATA, n)
        if n.endswith('.json') or n.endswith('.json.bak') or n == 'data_move.txt':
            os.remove(p)
        elif n in RESET and os.path.isdir(p) and not os.path.isjunction(p):
            shutil.rmtree(p)
    for rom in ROMS:
        src, dst = os.path.join(BUILD, rom), os.path.join(DATA, rom)
        if os.path.exists(src) and not os.path.exists(dst):
            link(src, dst)
    if os.path.exists(os.path.join(BUILD, 'games.json')):
        shutil.copyfile(os.path.join(BUILD, 'games.json'), os.path.join(DATA, 'games.json'))
    enabled = []
    try:
        with open(os.path.join(BUILD, 'mods.json'), encoding='utf-8') as f:
            enabled = list(json.load(f).get('enabled_mods', []))
    except (OSError, ValueError):
        pass
    enabled += extra_mods
    src_mods = os.path.join(BUILD, 'mods')
    entries = os.listdir(src_mods) if os.path.isdir(src_mods) else []
    if not track.isdigit():
        for e in entries:
            if os.path.isdir(os.path.join(src_mods, e, 'tracks', track)) or os.path.isdir(os.path.join(src_mods, e, 'rush2', 'tracks', track)):
                enabled.append(mod_name(e))
    mods = os.path.join(DATA, 'mods')
    os.makedirs(mods, exist_ok=True)
    for n in os.listdir(mods):
        remove_link(os.path.join(mods, n))
    wanted = []
    for e in entries:
        if mod_name(e) in enabled and not e.endswith('.7z'):
            link(os.path.join(src_mods, e), os.path.join(mods, e))
            if mod_name(e) not in wanted:
                wanted.append(mod_name(e))
    with open(os.path.join(DATA, 'mods.json'), 'w', encoding='utf-8') as f:
        json.dump({'enabled_mods': wanted, 'mod_order': wanted}, f, indent=4)
    return wanted


def summarize(out, players):
    path = os.path.join(out, 'samples.jsonl')
    if not os.path.exists(path):
        print('no samples (the game never ran?)')
        return 1
    rows = [json.loads(l) for l in open(path)]
    if not rows:
        print('no samples')
        return 1
    prev = None
    first, last = {}, {}
    for r in rows:
        if prev is None or r['state'] != prev['state']:
            print('%6.1f s  %s' % (r['wall'], STATES.get(r['state'], 'state %d' % r['state'])))
        if r['state'] in (3, 9, 10) and r['list']:
            for k in range(min(players, len(r['list']))):
                first.setdefault(k, r['list'][k])
                last[k] = r['list'][k]
        prev = r
    for k in range(players):
        if k not in first:
            print('player %d: no race samples' % (k + 1))
            continue
        moved = math.dist(first[k]['pos'], last[k]['pos'])
        print('player %d: car type %d, %.0f ft from the grid, laps %d' % (k + 1, last[k]['type'], moved, last[k]['laps']))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('track')
    ap.add_argument('--cars', default='0')
    ap.add_argument('--laps', type=int)
    ap.add_argument('--drones', type=int)
    ap.add_argument('--script')
    ap.add_argument('--seconds', type=float, default=25)
    ap.add_argument('--turbo', action='store_true')
    ap.add_argument('--finish', type=float)
    ap.add_argument('--mod', action='append', default=[])
    ap.add_argument('--out', default=os.path.join(REPO, 'tmp', 'test_race', 'out'))
    ap.add_argument('--env', action='append', default=[], help='extra environment variable K=V for the game')
    a = ap.parse_args()
    mods = prepare_data(a.track, a.mod)
    env = {k: v for k, v in os.environ.items() if not k.startswith('RUSH2_')}
    env.update(RUSH2_SIM='1', RUSH2_DATA_DIR=DATA, RUSH2_SIM_TURBO='1' if a.turbo else '0', RUSH2_TEST_RACE=a.track,
               RUSH2_TEST_RACE_CARS=a.cars)
    if a.laps is not None:
        env['RUSH2_TEST_RACE_LAPS'] = str(a.laps)
    if a.drones is not None:
        env['RUSH2_TEST_RACE_DRONES'] = str(a.drones)
    if a.finish is not None:
        env['RUSH2_TEST_FINISH'] = str(a.finish)
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    script = a.script or '1:A:%g' % max(1.0, a.seconds - 1)
    print('track %s, cars %s, mods %s' % (a.track, a.cars, ', '.join(mods) or 'none'), flush=True)
    os.makedirs(a.out, exist_ok=True)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'race_watch.py'), '--launch', script, '--seconds',
                        str(a.seconds), '--out', a.out], env=env, capture_output=True, text=True,
                       timeout=a.seconds + 60)
    with open(os.path.join(a.out, 'watch.txt'), 'w') as f:
        f.write(r.stdout + r.stderr)
    log = os.path.join(a.out, 'run.log')
    if os.path.exists(log):
        for line in open(log, errors='replace'):
            if line.startswith('[test_race]'):
                print(line.rstrip())
    return summarize(a.out, len(a.cars.split(',')))


if __name__ == '__main__':
    sys.exit(main())
