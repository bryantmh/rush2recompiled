"""The computer cars' pace in a race: their average speed, wrecks, the share of race time they spent braking above
100 mph (braking on straights shows here) and their top speed. Compares tracks or versions of one (an N64 course
against its PS1 night and snow versions, docs/sfrush_ps1.md#ai-lanes).

    python tools/drone_pace.py --race 19 embarcadero_night embarcadero_snow [--laps 3] [--catch-up] [--mod-dir DIR]
    python tools/drone_pace.py RUN [RUN ...]      (a race recording .r2rc, or a test_race.py / race_watch.py folder)
    [--wrecks]        (recordings) every computer car's wrecks: race time and place
    [--hits]          (recordings) where the computer cars hit walls: sudden sideways or backward jolts (over 8 g in
                      the ground plane between two of a car's steps, not braking or wrecked; one a second a car), grouped within 40 ft
    [--sections FT]   (recordings) the cars' mean time over each FT of their own distance driven, and where that stretch
                      starts (the median car's place): two versions of a course side by side show where one loses time

--race races each TRACK (a track select id or a custom track's folder name) headless, as the trackview runner does
(docs/custom_tracks.md#race-replays): a simulation run (RUSH2_SIM=1, src/sim_run.cpp: hidden, silent, about 50 times
real time) straight from the title (RUSH2_TEST_RACE), recorded every physics tick (RUSH2_RACE_RECORD,
src/race_record.cpp) until every computer car finishes; a run takes seconds. Each run starts from defaults in a data
folder of its own (tmp/drone_pace/data: settings and saves deleted, caches kept, the ROMs hard-linked from build/,
the mods enabled in build/mods.json linked in), with Drone Cheating off (the drones' pace is their lanes' alone;
--catch-up keeps it) and no race timer. --mod-dir races a mod folder (a converter's output, e.g.
tools/sfrush_ps1/cpp_test's OUT/sfrush_ps1) in place of the installed mod of its id. The recordings go to
tmp/drone_pace/<track>.r2rc.

Average speed: each computer car's distance over the ground (steps over 400 ft/s dropped as resets) over its time
until it finished (or the recording's end), averaged over the cars.
"""
import argparse, json, math, os, shutil, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, HERE)
BUILD = os.path.join(REPO, 'build')
EXE = os.path.join(BUILD, 'Rush2Recompiled.exe')
WORK = os.path.join(REPO, 'tmp', 'drone_pace')
DATA = os.path.join(WORK, 'data')
ROMS = ['rush2.n64.us.z64', 'rush2049.z64', 'rush2049_dc.pak', 'rush1.z64', 'calspeed.z64']
RESET = ['saves', 'mod_config', 'ghosts', 'track_reports']
#: Wall clock limit of one run: a race takes seconds (plus the boot, and a mod rebuild after a new build).
TIMEOUT = 4 * 60


def link(src, dst):
    try:
        os.link(src, dst)
    except OSError:
        shutil.copyfile(src, dst)


def prepare(catch_up, mod_dir=None):
    os.makedirs(DATA, exist_ok=True)
    for n in os.listdir(DATA):
        p = os.path.join(DATA, n)
        if n.endswith('.json') or n.endswith('.json.bak') or n == 'data_move.txt':
            os.remove(p)
        elif n in RESET and os.path.isdir(p):
            shutil.rmtree(p)
    for rom in ROMS:
        src, dst = os.path.join(BUILD, rom), os.path.join(DATA, rom)
        if os.path.exists(src) and not os.path.exists(dst):
            link(src, dst)
    # The same games on as the build's, and its enabled mods (the PS1 and arcade mods hold their courses).
    if os.path.exists(os.path.join(BUILD, 'games.json')):
        shutil.copyfile(os.path.join(BUILD, 'games.json'), os.path.join(DATA, 'games.json'))
    mods_json = {}
    try:
        mods_json = json.load(open(os.path.join(BUILD, 'mods.json'), encoding='utf-8'))
    except (OSError, ValueError):
        pass
    enabled = mods_json.get('enabled_mods', [])
    mods = os.path.join(DATA, 'mods')
    os.makedirs(mods, exist_ok=True)
    swap = json.load(open(os.path.join(mod_dir, 'mod.json'), encoding='utf-8'))['id'] if mod_dir else None
    for n in os.listdir(mods):   # a previous --mod-dir's folder (a junction) or a removed .rtz
        p = os.path.join(mods, n)
        if os.path.isjunction(p):
            os.rmdir(p)
        elif n == '%s.rtz' % swap:
            os.remove(p)
    for n in os.listdir(os.path.join(BUILD, 'mods')):
        src, dst = os.path.join(BUILD, 'mods', n), os.path.join(mods, n)
        if n.endswith('.rtz') and n != '%s.rtz' % swap and not os.path.exists(dst):
            link(src, dst)
    if swap:
        r = subprocess.run(['cmd', '/c', 'mklink', '/J', os.path.join(mods, swap), os.path.abspath(mod_dir)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise RuntimeError('mklink failed: ' + r.stdout + r.stderr)
        if swap not in enabled:
            enabled = enabled + [swap]
    with open(os.path.join(DATA, 'mods.json'), 'w', encoding='utf-8') as f:
        json.dump({'enabled_mods': enabled, 'mod_order': mods_json.get('mod_order', enabled)}, f, indent=4)
    if not catch_up:
        with open(os.path.join(DATA, 'general.json'), 'w', encoding='utf-8') as f:
            json.dump({'drone_catch_up': False}, f, indent=4)
    with open(os.path.join(DATA, 'cheats.json'), 'w', encoding='utf-8') as f:
        json.dump({'cheat_no_game_timer': True}, f, indent=4)


def race(track, catch_up, laps, mod_dir=None):
    """Races `track` headless; returns the recording's path or raises RuntimeError."""
    import hidden_desktop, time
    # Another session's build relinks the exe: wait for it to be back (and finished being written) rather than fail.
    for _ in range(180):
        if os.path.exists(EXE) and not subprocess.run(['tasklist', '/FI', 'IMAGENAME eq ninja.exe'], capture_output=True,
                                                      text=True).stdout.count('ninja.exe'):
            break
        time.sleep(2)
    prepare(catch_up, mod_dir)
    rec = os.path.join(WORK, '%s.r2rc' % track)
    for old in (rec, rec + '.done'):
        if os.path.exists(old):
            os.remove(old)
    env = {k: v for k, v in os.environ.items() if not k.startswith('RUSH2_')}
    env.update(RUSH2_SIM='1', RUSH2_DATA_DIR=DATA, RUSH2_TEST_RACE=str(track), RUSH2_RACE_RECORD=rec,
               RUSH2_TEST_CAR='0', RUSH2_TEST_RACE_LAPS=str(laps))
    with open(rec[:-5] + '.log', 'w') as log:
        proc = hidden_desktop.popen([EXE], cwd=BUILD, env=env, stdout=log, stderr=subprocess.STDOUT)
        try:
            proc.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()   # this run's PID only
            proc.wait()
            raise RuntimeError('%s: timed out' % track)
    if not os.path.exists(rec):
        raise RuntimeError('%s: no recording (%s)' % (track, rec[:-5] + '.log'))
    return rec


def from_recording(path):
    d = open(path, 'rb').read()
    if d[:4] != b'R2RC':
        return None
    hl = struct.unpack_from('<I', d, 8)[0]
    head = json.loads(d[12:12 + hl])
    fields = head['fields']
    fi = {f: i for i, f in enumerate(fields)}
    width = 2 + head['cars'] * len(fields)
    body = d[12 + hl:]
    rows = len(body) // (4 * width)
    vals = struct.unpack_from('<%df' % (rows * width), body)
    cars = []
    for c in range(head['cars']):
        if head['kinds'][c] != 1:
            continue
        base = 2 + c * len(fields)
        s = {'dist': 0.0, 'time': 0.0, 'wrecks': 0, 'brake_fast': 0.0, 'top': 0.0, 'finished': False,
             'wreck_at': [], 'trace': [], 'hits': []}
        was_wreck = False
        # The drones are stepped round robin (their `step`: the physics clock of the car's last step), so a car moves
        # only on some ticks: distances and times run from one of its steps to the next.
        last = None   # (step clock, x, z) at the car's previous step
        last_v = None   # (vx, vz) at it
        for r in range(rows):
            row = r * width + base
            if vals[row + fi['finished']]:
                s['finished'] = True
                break
            wreck = vals[row + fi['wreck']] != 0
            s['wrecks'] += wreck and not was_wreck
            if wreck and not was_wreck:
                s['wreck_at'].append((r / 60.0, c, vals[row + fi['x']], vals[row + fi['y']], vals[row + fi['z']]))
            was_wreck = wreck
            step, x, z = vals[row + fi['step']], vals[row + fi['x']], vals[row + fi['z']]
            if last is not None and step == last[0]:
                continue
            vx, vz = vals[row + fi['vx']], vals[row + fi['vz']]
            if last is not None and last_v is not None and not wreck and vals[row + fi['brake']] < 0.5:
                dt = step - last[0]
                if 0 < dt < 0.2:
                    a = math.hypot(vx - last_v[0], vz - last_v[1]) / dt
                    if a > 8 * 32.2:
                        if s['hits'] and r / 60.0 - s['hits'][-1][0] < 1.0:
                            if a / 32.2 > s['hits'][-1][5]:
                                s['hits'][-1] = (s['hits'][-1][0], c, x, vals[row + fi['y']], z, a / 32.2)
                        else:
                            s['hits'].append((r / 60.0, c, x, vals[row + fi['y']], z, a / 32.2))
            last_v = (vx, vz)
            if last is not None:
                dt = step - last[0]
                if 0 < dt < 1.0:
                    v = math.hypot(x - last[1], z - last[2]) / dt
                    if v < 400:
                        s['dist'] += v * dt
                        s['top'] = max(s['top'], v / 1.4667)
                        if vals[row + fi['brake']] > 0.5 and v / 1.4667 > 100:
                            s['brake_fast'] += dt
                    s['time'] += dt
                    s['trace'].append((s['dist'], s['time'], x, z))
            last = (step, x, z)
        cars.append(s)
    out = summary(cars, rows / 60.0, head.get('laps'))
    out['cars'] = cars
    return out


def show_wrecks(s):
    for t, c, x, y, z in sorted(w for car in s['cars'] for w in car['wreck_at']):
        print('  %6.1f s  car %d  wrecked at (%.0f, %.0f, %.0f)' % (t, c, x, y, z))


def show_hits(s):
    """The cars' wall hits grouped within 40 ft, most hit first: count, cars, where and the hardest jolt (g)."""
    groups = []
    for t, c, x, y, z, g in sorted(h for car in s['cars'] for h in car['hits']):
        for grp in groups:
            if math.hypot(grp['x'] - x, grp['z'] - z) < 40 and abs(grp['y'] - y) < 15:
                grp['n'] += 1
                grp['cars'].add(c)
                grp['g'] = max(grp['g'], g)
                break
        else:
            groups.append({'x': x, 'y': y, 'z': z, 'n': 1, 'cars': {c}, 'g': g})
    for grp in sorted(groups, key=lambda grp: -grp['n']):
        if grp['n'] < 2:
            continue
        print('  %3d hits  %d cars  at (%.0f, %.0f, %.0f)  up to %.0f g' %
              (grp['n'], len(grp['cars']), grp['x'], grp['y'], grp['z'], grp['g']))


def show_sections(s, ft):
    """Each car's time over each `ft` of its own distance, averaged over the cars, and the median car's place at
    the stretch's start."""
    import bisect
    n = int(max((car['dist'] for car in s['cars']), default=0) // ft)
    for k in range(n):
        times, xs, zs = [], [], []
        for car in s['cars']:
            tr = car['trace']
            d = [p[0] for p in tr]
            i0, i1 = bisect.bisect_left(d, k * ft), bisect.bisect_left(d, (k + 1) * ft)
            if i1 >= len(tr) or i0 >= len(tr):
                continue
            times.append(tr[i1][1] - tr[i0][1])
            xs.append(tr[i0][2])
            zs.append(tr[i0][3])
        if times:
            xs.sort(), zs.sort()
            print('  %6.0f ft  from (%6.0f, %6.0f)  %5.1f s' % (k * ft, xs[len(xs) // 2], zs[len(zs) // 2],
                                                             sum(times) / len(times)))


def from_samples(d):
    """A test_race.py / race_watch.py folder (DIR/samples.jsonl, sampled at the game's clock)."""
    rows = [json.loads(l) for l in open(os.path.join(d, 'samples.jsonl'))]
    race_rows = [r for r in rows if r['state'] == 3 and r['list']]
    if not race_rows:
        return None
    cars = {}
    prev = None
    for r in race_rows:
        for k, c in enumerate(r['list']):
            if c.get('human'):
                continue
            s = cars.setdefault(k, {'dist': 0.0, 'wrecks': 0, 'brake_fast': 0.0, 'time': 0.0, 'top': 0.0,
                                    'finished': False})
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
    return summary(list(cars.values()), race_rows[-1]['time'] - race_rows[0]['time'], None)


def summary(cars, seconds, laps):
    t = max(sum(s['time'] for s in cars), 1e-6)
    return {
        'seconds': seconds, 'drones': len(cars), 'laps': laps,
        'finished': sum(s['finished'] for s in cars),
        'mph': sum(s['dist'] for s in cars) / t / 1.4667,
        'wrecks': sum(s['wrecks'] for s in cars),
        'brake_fast': sum(s['brake_fast'] for s in cars) / t,
        'top': max((s['top'] for s in cars), default=0.0),
    }


def show(name, s):
    if s is None:
        print('%s: no race in it' % name)
        return
    print('%s: %.0f s%s, %d drones (%d finished), %.0f mph average, %d wrecks, braking above 100 mph %.1f%% of the '
          'time, top %.0f mph' % (name, s['seconds'], ', %s laps' % s['laps'] if s['laps'] else '', s['drones'],
                                  s['finished'], s['mph'], s['wrecks'], 100 * s['brake_fast'], s['top']))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('runs', nargs='+', help='with --race: tracks; else recordings or race_watch folders')
    ap.add_argument('--race', action='store_true', help='race each track headless first')
    ap.add_argument('--laps', type=int, default=3)
    ap.add_argument('--catch-up', action='store_true', help='keep Drone Cheating on')
    ap.add_argument('--mod-dir', help='a mod folder raced in place of the installed mod of its id')
    ap.add_argument('--wrecks', action='store_true', help='list every wreck (recordings)')
    ap.add_argument('--hits', action='store_true', help='where the cars hit walls (recordings)')
    ap.add_argument('--sections', type=float, help='mean time over each this many ft driven (recordings)')
    a = ap.parse_args()
    for r in a.runs:
        s = None
        if a.race:
            try:
                s = from_recording(race(r, a.catch_up, a.laps, a.mod_dir))
            except RuntimeError as e:
                print(e)
                continue
        elif r.endswith('.r2rc'):
            s = from_recording(r)
        else:
            show(r, from_samples(r))
            continue
        show(r, s)
        if s and a.wrecks:
            show_wrecks(s)
        if s and a.hits:
            show_hits(s)
        if s and a.sections:
            show_sections(s, a.sections)
        sys.stdout.flush()


if __name__ == '__main__':
    main()
