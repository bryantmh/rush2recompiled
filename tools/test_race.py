#!/usr/bin/env python3
"""Starts a race straight from the title screen (RUSH2_TEST_RACE, src/test_race.cpp) in a hidden, silent game and
reports how it went: when it loaded and started, each human car's type and how far it drove, and whether the game
got past the race (test aid; docs/menus.md#starting-a-race-without-the-menus).

    python tools/test_race.py TRACK [--cars T,T,...] [--laps N] [--drones N] [--script S] [--seconds S]
                              [--turbo] [--finish S] [--mod NAME] [--out DIR] [--env K=V ...] [--games K=V ...]
                              [--config NAME.K=V ...]
                              [--shots S,S,...] [--records PLACEMENT_BIN] [--user-settings] [--show] [--exe EXE]

TRACK `menus` sets no race up: the game boots to the title and the --script drives the menus (as JUST PLAY: the
data folder has no saved players). Otherwise TRACK is a track select id (0-11 Rush 2, 12-17 Rush 2049, 18-24 SF Rush, 25-28 stunt arenas, 29 obstacle course,
30-37 battle arenas, 38-63 custom tracks, 64-79 California Speed) or a custom track's folder name. --cars gives one
car type per player (1-4 players; Rush 2 0-21, 2049 23-35, California Speed 36-56, mod cars 57-72). --script is the
--input-script (default: gas held from 1 s), counted from launch: the race is up about 5 s in.

The game runs as a sim run (RUSH2_SIM=1, src/sim_run.cpp): hidden, deaf to real controllers, no audio, from a data
folder of its own (tmp/test_race/data), so the player's settings and saves are never read or written. Before each run
its settings and saves are cleared (its caches are kept), the ROMs in build/ are hard-linked in, build/games.json is
copied (the same games on; --games K=V sets one of its options, V as JSON or else a string; --config NAME.K=V sets
one in NAME.json, e.g. cheats.unlock_all_parts=true), and the mods enabled
in build/mods.json are linked in and enabled, plus --mod ones and the mod holding a TRACK folder (--mod-dir PATH links
a mod folder from anywhere, such as a converter's output, or copies a .rtz, in place of the installed mod of its id). Real time by
default; --turbo runs the race as fast as the machine goes from its first tick (the script's presses stop then).
--finish S finishes the players' cars S seconds into the race (RUSH2_TEST_FINISH) to reach the race-over screens.
The run is closed after --seconds (by its PID). race_watch.py does the watching: DIR/samples.jsonl, DIR/events.txt,
DIR/run.log (the game's output).
"""
import argparse, json, math, os, shutil, subprocess, sys, zipfile

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


def set_options(name, pairs):
    """Sets options K=V (V as JSON, or else a string) in the data folder's NAME.json, keeping its others."""
    path = os.path.join(DATA, name + '.json')
    cfg = json.load(open(path, encoding='utf-8')) if os.path.exists(path) else {}
    for kv in pairs:
        k, _, v = kv.partition('=')
        try:
            cfg[k] = json.loads(v)
        except ValueError:
            cfg[k] = v
    with open(path, 'w', encoding='utf-8') as f:
        json.dump(cfg, f, indent=4)


#: Settings files --user-settings copies from build/ (how the game looks and plays; never the saves or the sound).
USER_SETTINGS = ['graphics.json', 'general.json', 'cheats.json', 'circuit.json', 'npc_cars.json']


def prepare_data(track, extra_mods, games=(), configs=(), mod_dirs=(), user_settings=False):
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
    if user_settings:
        for n in USER_SETTINGS:
            if os.path.exists(os.path.join(BUILD, n)):
                shutil.copyfile(os.path.join(BUILD, n), os.path.join(DATA, n))
        # A test window never opens fullscreen over the user's desktop, whatever their own window mode is.
        if os.path.exists(os.path.join(DATA, 'graphics.json')):
            set_options('graphics', ['wm_option=Windowed'])
    if games:
        set_options('games', games)
    for entry in configs:
        name, _, kv = entry.partition('.')
        set_options(name, [kv])
    enabled = []
    try:
        with open(os.path.join(BUILD, 'mods.json'), encoding='utf-8') as f:
            enabled = list(json.load(f).get('enabled_mods', []))
    except (OSError, ValueError):
        pass
    enabled += extra_mods
    src_mods = os.path.join(BUILD, 'mods')
    entries = os.listdir(src_mods) if os.path.isdir(src_mods) else []
    if not track.isdigit() and track != 'menus':
        for e in entries:
            if os.path.isdir(os.path.join(src_mods, e, 'tracks', track)) or os.path.isdir(os.path.join(src_mods, e, 'rush2', 'tracks', track)):
                enabled.append(mod_name(e))
    mods = os.path.join(DATA, 'mods')
    os.makedirs(mods, exist_ok=True)
    for n in os.listdir(mods):
        p = os.path.join(mods, n)
        if os.path.isdir(p) and not os.path.isjunction(p) and not os.path.islink(p):
            shutil.rmtree(p)      # a --mod-dir copy, or a mod the game rebuilt into a folder
        else:
            remove_link(p)
    wanted = []
    replaced = set()
    for d in mod_dirs:
        if os.path.isfile(d):
            # A packed mod (.rtz): copied, since the game may replace it (the arcade mods' rebuild).
            with zipfile.ZipFile(d) as z:
                mid = json.loads(z.read('mod.json'))['id']
            shutil.copyfile(d, os.path.join(mods, mid + '.rtz'))
            replaced.add(mid)
            wanted.append(mid)
            continue
        with open(os.path.join(d, 'mod.json'), encoding='utf-8') as f:
            mid = json.load(f)['id']
        link(os.path.abspath(d), os.path.join(mods, mid))
        replaced.add(mid)
        wanted.append(mid)
    for e in entries:
        if mod_name(e) in enabled and not e.endswith('.7z') and mod_name(e) not in replaced:
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
    ap.add_argument('--mod-dir', action='append', default=[], help='a mod folder to link in (or a .rtz to copy in), '
                    'in place of the installed mod with the same id (its mod.json id)')
    ap.add_argument('--out', default=os.path.join(REPO, 'tmp', 'test_race', 'out'))
    ap.add_argument('--env', action='append', default=[], help='extra environment variable K=V for the game')
    ap.add_argument('--games', action='append', default=[], help='games.json option K=V for the run')
    ap.add_argument('--config', action='append', default=[],
                    help='option of another settings file for the run, NAME.K=V (e.g. cheats.unlock_all_parts=true)')
    ap.add_argument('--shots', help='screenshot the game window at these seconds after launch (comma separated) into '
                    'DIR/shot_<seconds>.png; the window is shown for it (RUSH2_SIM_SHOW), without taking the focus')
    ap.add_argument('--records', help="a custom track's placement.bin: report which of its records moved during the race "
                    "(race_watch.py --records; moving objects)")
    ap.add_argument('--peek', action='append', default=[], help='an RDRAM u32 (hex) to print the smallest and largest of '
                    'during the race (race_watch.py --peek, repeatable; e.g. heap pointers 8010C438 8010C454)')
    ap.add_argument('--user-settings', action='store_true', help="copy build/'s graphics, general, cheats, circuit "
                    "and opponent settings in (profiling what the player sees: MSAA, SSAO and the like change RT64's "
                    "work); the saves and sound settings stay the run's own")
    ap.add_argument('--show', action='store_true', help='show the window (RUSH2_SIM_SHOW, without taking the focus): '
                    'a hidden window presents differently, so profile with it shown')
    ap.add_argument('--exe', help="run this game exe instead of build/'s (e.g. .release/build/Rush2Recompiled.exe, "
                    'the Release build players get, for crashes the local build doesn\'t show)')
    ap.add_argument('--data', help="the run's data folder (default tmp/test_race/data); give each run that may "
                    "overlap another session's its own")
    a = ap.parse_args()
    if a.data:
        global DATA
        DATA = os.path.abspath(a.data)
    if a.records and not os.path.isfile(a.records):
        sys.exit('no placement file %s (the converted track: track_mod_test.exe output)' % a.records)
    mods = prepare_data(a.track, a.mod, a.games, a.config, a.mod_dir, a.user_settings)
    env = {k: v for k, v in os.environ.items() if not k.startswith('RUSH2_')}
    env.update(RUSH2_SIM='1', RUSH2_DATA_DIR=DATA, RUSH2_SIM_TURBO='1' if a.turbo else '0')
    if a.track != 'menus':
        env.update(RUSH2_TEST_RACE=a.track, RUSH2_TEST_RACE_CARS=a.cars)
    if a.laps is not None:
        env['RUSH2_TEST_RACE_LAPS'] = str(a.laps)
    if a.drones is not None:
        env['RUSH2_TEST_RACE_DRONES'] = str(a.drones)
    if a.finish is not None:
        env['RUSH2_TEST_FINISH'] = str(a.finish)
    for kv in a.env:
        k, _, v = kv.partition('=')
        env[k] = v
    if a.shots or a.show:
        env['RUSH2_SIM_SHOW'] = '1'
    script = a.script or '1:A:%g' % max(1.0, a.seconds - 1)
    print('track %s, cars %s, mods %s' % (a.track, a.cars, ', '.join(mods) or 'none'), flush=True)
    os.makedirs(a.out, exist_ok=True)
    r = subprocess.run([sys.executable, os.path.join(HERE, 'race_watch.py'), '--launch', script, '--seconds',
                        str(a.seconds), '--out', a.out] + (['--shots', a.shots] if a.shots else []) +
                       (['--records', os.path.abspath(a.records)] if a.records else []) +
                       [v for p in a.peek for v in ('--peek', p)] +
                       (['--exe', a.exe] if a.exe else []), env=env, capture_output=True, text=True,
                       timeout=a.seconds + 60)
    with open(os.path.join(a.out, 'watch.txt'), 'w') as f:
        f.write(r.stdout + r.stderr)
    if a.records or a.peek:
        print(r.stdout.rstrip())
    log = os.path.join(a.out, 'run.log')
    if os.path.exists(log):
        for line in open(log, errors='replace'):
            if line.startswith('[test_race]'):
                print(line.rstrip())
    return summarize(a.out, len(a.cars.split(',')))


if __name__ == '__main__':
    sys.exit(main())
