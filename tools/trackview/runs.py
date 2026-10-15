"""AI runs for trackview: races a track headless in the real game and keeps the recording for the page's replay.

build/Rush2Recompiled.exe runs as a simulation (RUSH2_SIM=1, src/sim_run.cpp): hidden, silent, deaf to real
controllers, and from the start of the race at the fastest tick rate the machine manages. It keeps its settings and
save in a data folder of its own (RUSH2_DATA_DIR = tmp/trackview/sim_data/), so nothing the player has set changes the
race and the player's data is never touched. Before each run that folder's settings, saves and mod configs are deleted
(its derived-data caches are kept), the ROMs are hard-linked in from build/, and its only mod, "trackview", is
generated: mods/trackview/tracks/<folder> is a directory junction to the track folder, so any track folder the viewer
lists can be raced, whether or not it is in an enabled mod.

The game goes through the menus with an input script (RUSH2_TEST_MOD_FOLDER picks the track on the track select,
src/rush2049/track2049_menu.cpp; RUSH2_TEST_CAR=0 player 1's car, which waits on the grid), records the race
(RUSH2_RACE_RECORD, src/race_record.cpp) and exits once every computer car has finished or got stuck. A run takes
seconds, so only the latest is kept: tmp/trackview/runs/<folder>.r2rc, replaced by the next run of the track. A run
that isn't done in TIMEOUT is closed, by its PID only.

    python tools/trackview/runs.py FOLDER [--backward]     (a run from the command line)

See: docs/custom_tracks.md#race-replays
"""
import json, os, shutil, subprocess, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(REPO, 'build')
EXE = os.path.join(BUILD, 'Rush2Recompiled.exe')
RUNS = os.path.join(REPO, 'tmp', 'trackview', 'runs')
DATA = os.path.join(REPO, 'tmp', 'trackview', 'sim_data')
#: ROMs the game reads from its data folder (src/data_location.cpp data_files).
ROMS = ['rush2.n64.us.z64', 'rush2049.z64', 'rush2049_dc.pak', 'rush1.z64']
#: Settings and saves, deleted before each run so it starts from defaults (caches such as track_cache are kept).
RESET = ['saves', 'mod_config', 'ghosts', 'track_reports']
MOD = 'trackview'
#: Title screen START, then A through the menus to the race (the custom-track recipe in docs/custom_tracks.md).
SCRIPT = '8:START,11:A,13:A,22:A,25:A,27:A,32:A'
#: A run longer than this (wall clock) is closed: a full race takes about 30 s (20 s of it the menus), so a run still
#: going after 2 minutes has hung.
TIMEOUT = 2 * 60

lock = threading.Lock()
current = {}   # the run in progress: folder, name, pid, started, state


def run_path(folder):
    """The track's recording (the latest run's), whether or not it exists yet."""
    return os.path.join(RUNS, os.path.basename(os.path.abspath(folder)) + '.r2rc')


def latest(folder):
    """The track's recording: {stamp (its time), done (src/race_record.cpp's summary), backward}, or None."""
    p = run_path(folder)
    if not os.path.exists(p):
        return None
    done = None
    try:
        with open(p + '.done', encoding='utf-8') as f:
            done = json.load(f)
    except (OSError, ValueError):
        pass
    return dict(stamp=os.path.getmtime(p), done=done)


def status():
    with lock:
        return dict(current)


def remove_link(path):
    """Removes a junction or link without touching what it points to."""
    if os.path.isjunction(path) or os.path.islink(path):
        os.rmdir(path) if os.path.isdir(path) else os.unlink(path)


def prepare_data(folder):
    """Sets up DATA for a run of `folder`: defaults, the ROMs, and the trackview mod holding just this track."""
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
            try:
                os.link(src, dst)
            except OSError:
                shutil.copyfile(src, dst)
    mods = os.path.join(DATA, 'mods')
    tracks = os.path.join(mods, MOD, 'tracks')
    os.makedirs(tracks, exist_ok=True)
    for n in os.listdir(tracks):
        remove_link(os.path.join(tracks, n))
    name = os.path.basename(os.path.abspath(folder))
    r = subprocess.run(['cmd', '/c', 'mklink', '/J', os.path.join(tracks, name), os.path.abspath(folder)],
                       capture_output=True, text=True)
    if r.returncode != 0:
        raise OSError('mklink failed: ' + r.stdout + r.stderr)
    with open(os.path.join(mods, MOD, 'mod.json'), 'w', encoding='utf-8') as f:
        json.dump({'game_id': 'rush2', 'id': MOD, 'display_name': 'Trackview', 'description': 'tools/trackview AI runs',
                   'short_description': 'tools/trackview AI runs', 'version': '1.0.0', 'authors': ['trackview'],
                   'minimum_recomp_version': '0.0.0'}, f, indent=4)
    with open(os.path.join(DATA, 'mods.json'), 'w', encoding='utf-8') as f:
        json.dump({'enabled_mods': [MOD], 'mod_order': [MOD]}, f, indent=4)


def start(folder, backward=False):
    """Starts a run; returns {'error'} or the run's status."""
    with lock:
        if current.get('state') == 'running':
            return {'error': 'a run is already going (%s)' % current.get('folder_name')}
    if not os.path.exists(EXE):
        return {'error': 'build/Rush2Recompiled.exe not built'}
    try:
        prepare_data(folder)
    except OSError as e:
        return {'error': 'could not set up %s: %s' % (DATA, e)}
    os.makedirs(RUNS, exist_ok=True)
    rec = run_path(folder)
    for old in (rec, rec + '.done'):
        if os.path.exists(old):
            os.remove(old)
    env = {k: v for k, v in os.environ.items() if not k.startswith('RUSH2_')}
    env.update(RUSH2_SIM='1', RUSH2_DATA_DIR=DATA, RUSH2_TEST_MOD_FOLDER=os.path.basename(os.path.abspath(folder)),
               RUSH2_RACE_RECORD=rec, RUSH2_TEST_CAR='0')
    if backward:
        env['RUSH2_TEST_BACKWARD'] = '1'
    log = open(rec[:-5] + '.log', 'w')
    proc = subprocess.Popen([EXE, '--input-script', SCRIPT], cwd=BUILD, env=env, stdout=log, stderr=subprocess.STDOUT)
    with lock:
        current.clear()
        current.update(folder=os.path.abspath(folder), folder_name=os.path.basename(folder), backward=backward,
                       pid=proc.pid, started=time.time(), state='running')
    threading.Thread(target=watch, args=(proc, rec, log), daemon=True).start()
    return status()


def watch(proc, rec, log):
    """Waits for the game to exit (it does once the recording is done); closes it (this PID only) on a timeout."""
    t0 = time.time()
    state = 'done'
    while proc.poll() is None:
        if time.time() - t0 > TIMEOUT:
            state = 'timed out'
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
            break
        if os.path.exists(rec + '.done') and time.time() - os.path.getmtime(rec + '.done') > 15:
            proc.terminate()   # done but still running: shouldn't happen (src/sim_run.cpp exits)
            break
        time.sleep(0.5)
    if state == 'done' and not os.path.exists(rec + '.done'):
        state = 'game exited before the race ended'
    log.close()
    with lock:
        current['state'] = state
        current['ended'] = time.time()
        current['exit_code'] = proc.returncode


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder')
    ap.add_argument('--backward', action='store_true')
    a = ap.parse_args()
    print(start(a.folder, a.backward))
    while status().get('state') == 'running':
        time.sleep(1)
    s = status()
    print(s, 'in %.1f s' % (s['ended'] - s['started']))
