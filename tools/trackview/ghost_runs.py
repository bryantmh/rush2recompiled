"""Ghost record + replay test for a custom track: does a ghost recorded on it play back on its recording?

Two launches of build/Rush2Recompiled.exe in a data folder of its own (tmp/trackview/ghost_data, set up like
runs.py's: defaults, the ROMs, a mod holding just this track), so the player's settings, saves and ghosts are never
touched. They are simulation runs in real time (RUSH2_SIM=1, RUSH2_SIM_TURBO=0: the only way RUSH2_DATA_DIR is
honored): hidden, silent, and player 1 skips car-against-car tests and the stuck reset (src/sim_run.cpp):
  1. ONE RACE: player 1 drives a scripted line (full gas, steering pulses: walls, jumps and wrecks included) and
     RUSH2_GHOST_TEST_TICKS keeps the recording after TICKS ticks as if the car had finished (src/ghost.cpp).
  2. GHOST RACE: the car select picks the fastest kept ghost (that one); RUSH2_GHOST_DEBUG=1 logs at every keyframe how
     far the ghost is from its recording, and every put-back.
Backward picks BACKWARD on the track select (the ghost list is keyed at the car select, so RUSH2_TEST_BACKWARD,
applied at race start, would miss it). Each launch is closed by its PID only.

    python tools/trackview/ghost_runs.py TRACK_FOLDER [--backward] [--mirror] [--ticks 1800] [--track ID]

See: docs/custom_tracks.md#race-replays
"""
import argparse, os, re, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import runs

DATA = os.path.join(runs.REPO, 'tmp', 'trackview', 'ghost_data')
OUT = os.path.join(runs.REPO, 'tmp', 'trackview', 'ghost_runs')
#: Start Game rows down to GHOST RACE (docs/menus.md#start-game-rows: ONE RACE, CIRCUIT, PRACTICE, GHOST RACE).
GHOST_ROW = '11:DD,11.6:DD,12.2:DD,'
#: Gas from just after the car select, with steering pulses so the line meets walls and the road's edges.
DRIVE = '33:A:200,40:SL:0.6,47:SR:0.8,55:SL:1.2,63:SR:0.5,70:SR:1.0,78:SL:0.7,86:SR:1.5,95:SL:0.6'


def script(ghost, backward, mirror):
    # Track select (from 20 s): rows TRACK, BACKWARD, MIRROR (src/rush2049/track2049_menu.cpp); DR turns one on.
    steps = []
    if backward or mirror:
        steps += ['DD', 'DR'] if backward else ['DD']
        steps += ['DD', 'DR', 'DU'] if mirror else []
        steps += ['DU']
    opts = ''.join('%.1f:%s,' % (18 + 0.5 * i, s) for i, s in enumerate(steps))
    head = '8:START,' + (GHOST_ROW + '13.5:A,15.5:A,' if ghost else '11:A,13:A,')
    return head + opts + '22:A,25:A,27:A,32:A,' + DRIVE


def launch(folder, ghost, a, log_path, until, timeout):
    env = {k: v for k, v in os.environ.items() if not k.startswith('RUSH2_')}
    # RUSH2_DATA_DIR is only honored by a simulation run (src/main.cpp); without turbo its script presses keep reaching
    # player 1 all race (src/sim_run.cpp), so the car drives, hidden and silent, on its own data folder.
    env.update(RUSH2_SIM='1', RUSH2_SIM_TURBO='0', RUSH2_DATA_DIR=DATA,
               RUSH2_GHOST_TEST_TICKS=str(a.ticks))
    if a.car >= 0:
        env['RUSH2_TEST_CAR'] = str(a.car)
    if a.track is not None:
        env['RUSH2_TEST_TRACK'] = str(a.track)
    else:
        env['RUSH2_TEST_MOD_FOLDER'] = os.path.basename(os.path.abspath(folder))
    if ghost:
        env['RUSH2_GHOST_DEBUG'] = '1'
    log = open(log_path, 'w')
    proc = subprocess.Popen([runs.EXE, '--input-script', script(ghost, a.backward, a.mirror)], cwd=runs.BUILD, env=env,
                            stdout=log, stderr=subprocess.STDOUT)
    t0 = time.time()
    try:
        while proc.poll() is None and time.time() - t0 < timeout:
            time.sleep(1)
            with open(log_path, errors='replace') as f:
                if until(f.read()):
                    time.sleep(2)
                    break
    finally:
        if proc.poll() is None:
            proc.terminate()
            try:
                proc.wait(10)
            except subprocess.TimeoutExpired:
                proc.kill()
        log.close()
    return open(log_path, errors='replace').read()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('folder')
    ap.add_argument('--backward', action='store_true')
    ap.add_argument('--mirror', action='store_true')
    ap.add_argument('--track', type=int, help='race this track select id instead (a stock track, to compare)')
    ap.add_argument('--car', type=int, default=-1, help='player 1 car type (RUSH2_TEST_CAR). Default -1 leaves the car select as it opens: with RUSH2_TEST_CAR the ghost is saved as type 22 (no car) and its list skips it')
    ap.add_argument('--ticks', type=int, default=1800, help='recording length in ticks (60 a second)')
    a = ap.parse_args()
    runs.DATA = DATA
    runs.prepare_data(a.folder)
    os.makedirs(OUT, exist_ok=True)
    tag = ('track%d' % a.track if a.track is not None else os.path.basename(os.path.abspath(a.folder))) + ('_backward' if a.backward else '') + ('_mirror' if a.mirror else '')
    secs = a.ticks / 60
    rec = launch(a.folder, False, a, os.path.join(OUT, tag + '_record.log'), lambda s: '[ghost] New ghost' in s,
                 60 + secs)
    m = re.search(r'\[ghost\] New ghost (\S+)', rec)
    if not m:
        print('record: no ghost kept; see', os.path.join(OUT, tag + '_record.log'))
        return 1
    print('record: kept', m.group(1))
    # Replay: until the ghost has played its whole recording (or its own recording of this run is kept).
    play = launch(a.folder, True, a, os.path.join(OUT, tag + '_play.log'), lambda s: '[ghost] New ghost' in s,
                  60 + secs + 15)
    loaded = re.search(r'\[ghost\] Car (\d+) is the ghost of', play)
    print('play: ghost loaded' if loaded else 'play: NO ghost in the race', '(%s)' % m.group(1))
    drifts = [float(x) for x in re.findall(r'tick \d+: ([\d.]+) ft from the recording', play)]
    putbacks = re.findall(r'drifted ([\d.]+) ft by tick (\d+)', play)
    if drifts:
        print('play: %d keyframes, drift max %.4f ft, mean %.4f ft' % (len(drifts), max(drifts), sum(drifts) / len(drifts)))
    print('play: %d put-back(s)%s' % (len(putbacks), ': ' + ', '.join('%s ft @%s' % p for p in putbacks[:8]) if putbacks else ''))
    print('logs in', OUT)
    return 0 if loaded else 1


if __name__ == '__main__':
    sys.exit(main())
