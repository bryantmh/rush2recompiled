"""Races California Speed tracks headless and counts every computer car's wrecks and rollovers (test aid for the
California Speed road hold, src/calspeed/calspeed_rom.cpp).

Each run is a simulation (RUSH2_SIM=1, src/sim_run.cpp: hidden, silent, fastest tick rate from the race's start) in a
data folder of its own (tmp/sim_wrecks/data, its settings and saves deleted before each run, its caches kept), so
nothing the player has set changes the race and the player's data is never touched. The ROMs (calspeed.z64 included)
are hard-linked in from build/. Player 1 drives car 0 and waits on the grid; the computer cars race with the default
options (California Speed's cars among them). The race is recorded (RUSH2_RACE_RECORD, src/race_record.cpp) and the
game exits once every computer car has finished or got stuck; a run still going after TIMEOUT is closed by its PID.

    python tools/calspeed/sim_wrecks.py [--tracks 64-79] [--hold on|off|both] [--no-catch-up] [--env K=V ...]
                                        [--tag NAME]

--no-catch-up turns Drone Cheating off (the drones' pace is their lanes' alone); --env passes RUSH2_* switches to the
game; --tag names the recordings (tmp/sim_wrecks/run_<track>_<hold>[_<tag>].r2rc, read by lane_hold.py).

Prints, per track and road hold setting: each computer car's wrecks (wreck state 0 -> nonzero), rollovers (up vector
y below 0 while not wrecked) and whether it finished, and totals. Wrecks are split by what led to them over the 2 s
before: rolled (upside down), stalled (below 30 ft/s: a spin into a wall, or stuck) or at speed (put back while moving:
off its lane, under the course, a fall).
"""
import argparse, json, os, shutil, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(REPO, 'build')
EXE = os.path.join(BUILD, 'Rush2Recompiled.exe')
WORK = os.path.join(REPO, 'tmp', 'sim_wrecks')
DATA = os.path.join(WORK, 'data')
ROMS = ['rush2.n64.us.z64', 'rush2049.z64', 'rush1.z64', 'calspeed.z64']
RESET = ['saves', 'mod_config', 'ghosts', 'track_reports']
#: Title START, then A through the menus to the race (California Speed's recipe: car select about 25 s in).
SCRIPT = '8:START,11:A,13:A,24:A,60:A,62:A,66:A'
TIMEOUT = 2 * 60


def prepare(hold, catch_up=True):
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
            try:
                os.link(src, dst)
            except OSError:
                shutil.copyfile(src, dst)
    if not hold:
        with open(os.path.join(DATA, 'games.json'), 'w', encoding='utf-8') as f:
            json.dump({'calspeed_road_hold': False}, f, indent=4)
    if not catch_up:
        with open(os.path.join(DATA, 'general.json'), 'w', encoding='utf-8') as f:
            json.dump({'drone_catch_up': False}, f, indent=4)
    # No race timer: on most tracks the first checkpoint is more than a minute away, and time running out ended the race.
    with open(os.path.join(DATA, 'cheats.json'), 'w', encoding='utf-8') as f:
        json.dump({'cheat_no_game_timer': True}, f, indent=4)


def read_record(path):
    with open(path, 'rb') as f:
        d = f.read()
    if d[:4] != b'R2RC':
        return None
    hlen = struct.unpack_from('<I', d, 8)[0]
    head = json.loads(d[12:12 + hlen])
    fields, n = head['fields'], head['cars']
    width = 2 + n * len(fields)
    body = d[12 + hlen:]
    rows = len(body) // (4 * width)
    vals = struct.unpack_from('<%df' % (rows * width), body)
    return head, fields, n, width, rows, vals


def summarize(path):
    rec = read_record(path)
    if rec is None:
        return None
    head, fields, n, width, rows, vals = rec
    fi = {f: i for i, f in enumerate(fields)}
    out = []
    for c in range(n):
        if head['kinds'][c] != 1:
            continue
        base = 2 + c * len(fields)
        wrecks = rolls = 0
        why = {'rolled': 0, 'stalled': 0, 'at speed': 0}
        was_wreck = was_roll = False
        finished = False
        for r in range(rows):
            row = r * width + base
            # A wreck after the car finished (a point-to-point road's end) doesn't count.
            wreck = vals[row + fi['wreck']] != 0 and vals[row + fi['finished']] == 0
            roll = vals[row + fi['uy']] < 0 and not wreck
            if wreck and not was_wreck:
                wrecks += 1
                before = range(max(0, r - 120), r)
                if any(vals[k * width + base + fi['uy']] < 0 for k in before):
                    why['rolled'] += 1
                elif any(vals[k * width + base + fi['speed']] < 30 for k in before):
                    why['stalled'] += 1
                else:
                    why['at speed'] += 1
            rolls += roll and not was_roll
            was_wreck, was_roll = wreck, roll
            finished = finished or vals[row + fi['finished']] != 0
        out.append(dict(car=c, type=head['types'][c], wrecks=wrecks, rolls=rolls, finished=finished, why=why))
    return out, rows


def run(track, hold, catch_up=True, extra_env=None, tag=''):
    prepare(hold, catch_up)
    os.makedirs(WORK, exist_ok=True)
    rec = os.path.join(WORK, 'run_%d_%s%s.r2rc' % (track, 'on' if hold else 'off', '_' + tag if tag else ''))
    for old in (rec, rec + '.done'):
        if os.path.exists(old):
            os.remove(old)
    env = {k: v for k, v in os.environ.items() if not k.startswith('RUSH2_')}
    env.update(RUSH2_SIM='1', RUSH2_DATA_DIR=DATA, RUSH2_TEST_TRACK=str(track), RUSH2_RACE_RECORD=rec,
               RUSH2_TEST_CAR='0')
    env.update(extra_env or {})
    with open(rec[:-5] + '.log', 'w') as log:
        proc = subprocess.Popen([EXE, '--input-script', SCRIPT], cwd=BUILD, env=env, stdout=log,
                                stderr=subprocess.STDOUT)
        try:
            proc.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            proc.kill()   # this run's PID only
            proc.wait()
            return None, 'timed out'
    done = None
    if os.path.exists(rec + '.done'):
        with open(rec + '.done', encoding='utf-8') as f:
            done = json.load(f).get('why')
    if not os.path.exists(rec):
        return None, 'no recording'
    return summarize(rec), done


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--tracks', default='64-79', help='track select ids: 64-79 or a comma list')
    ap.add_argument('--hold', default='both', choices=['on', 'off', 'both'])
    ap.add_argument('--no-catch-up', action='store_true', help='Drone Cheating off')
    ap.add_argument('--env', action='append', default=[], help='K=V passed to the game')
    ap.add_argument('--tag', default='', help='recording name suffix')
    a = ap.parse_args()
    if '-' in a.tracks:
        lo, hi = (int(x) for x in a.tracks.split('-'))
        tracks = list(range(lo, hi + 1))
    else:
        tracks = [int(x) for x in a.tracks.split(',')]
    holds = {'on': [True], 'off': [False], 'both': [True, False]}[a.hold]
    totals = {h: [0, 0, 0, 0] for h in holds}   # wrecks, rolls, finished, cars
    causes = {h: {} for h in holds}
    for t in tracks:
        for h in holds:
            started = time.time()
            res, done = run(t, h, not a.no_catch_up, dict(e.split('=', 1) for e in a.env), a.tag)
            label = 'track %d hold %-3s' % (t, 'on' if h else 'off')
            if res is None:
                print('%s  %s' % (label, done), flush=True)
                continue
            cars, rows = res
            w = sum(c['wrecks'] for c in cars)
            r = sum(c['rolls'] for c in cars)
            f = sum(c['finished'] for c in cars)
            totals[h] = [totals[h][0] + w, totals[h][1] + r, totals[h][2] + f, totals[h][3] + len(cars)]
            why = {}
            for c in cars:
                for k, n in c['why'].items():
                    why[k] = why.get(k, 0) + n
                    causes[h][k] = causes[h].get(k, 0) + n
            print('%s  %4.0f s race, %3.0f s wall  wrecks %2d (%s)  rollovers %2d  finished %d/%d  (%s)  %s' % (
                label, rows / 60.0, time.time() - started, w, ', '.join('%s %d' % kv for kv in why.items()), r, f,
                len(cars), done,
                ' '.join('t%d:%d/%d' % (c['type'], c['wrecks'], c['rolls']) for c in cars)), flush=True)
    for h in holds:
        w, r, f, n = totals[h]
        print('TOTAL hold %-3s  wrecks %d (%s)  rollovers %d  finished %d/%d' % (
            'on' if h else 'off', w, ', '.join('%s %d' % kv for kv in causes[h].items()), r, f, n))


if __name__ == '__main__':
    main()
