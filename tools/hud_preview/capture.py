"""Records race HUD captures for tools/hud_preview (test aid; docs/hud_layout.md#captures).

    py tools/hud_preview/capture.py OUT_DIR CASE [CASE ...]
    py tools/hud_preview/capture.py --list

Each CASE is one game launch (65-80 s) through ONE RACE on a Rush 2 track with RUSH2_HUD_CAPTURE set, so the game
writes OUT_DIR/CASE/frames.jsonl (every widget, what has been seen of it, the settings, the layout's output, the
printed text) and OUT_DIR/CASE/tex/, plus screenshots (shots.py) to compare the previewer's pictures with. Runs cover
the countdown, driving (A held), the bonus time banner (RUSH2_TEST_BONUS_TIME) and the finish overlay
(RUSH2_TEST_FINISH). Settings a case changes (odometer, split screen layout, Stunts on All Tracks) are written to
build/*.json for the run and put back after; build/saves is backed up and restored, and the pak is checked.
"""
import json, os, shutil, subprocess, sys, time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
BUILD = os.path.join(REPO, 'build')

STUNTS = {'cheats.json': {'cheat_stunts_all_tracks': True}}
STACKED = {'graphics.json': {'split_screen': 'TopBottom'}}
SIDE = {'graphics.json': {'split_screen': 'SideBySide'}}
# 'track_select' (not a file): values for the save's track select (the race track select's BATTLE row: race_battle).
RACE_BATTLE = {'track_select': {'race_battle': 1}}
# 'env' (not a file): test aids for the run (RUSH2_TEST_DEATHS: the DEATHS option, the skull).
DEATHS = {'env': {'RUSH2_TEST_DEATHS': '1'}}
# name: players, manual gearbox per player, settings {file: {key: value}}[, mode, track]. mode is the Start Game row:
# race (ONE RACE), stunt (STUNT, an arena) or battle (BATTLE, a Rush 2049 arena; needs the Rush 2049 ROM).
CASES = {
    # Every element that can show: every player on manual, the stunt panel on.
    'p1_all':     (1, '1',    STUNTS),
    'p2_stacked_all': (2, '11', {**STUNTS, **STACKED}),
    'p2_side_all': (2, '11',  {**STUNTS, **SIDE}),
    'p3_all':     (3, '111',  STUNTS),
    'p4_all':     (4, '1111', STUNTS),
    'arena1':     (1, '1',    {}, 'stunt', 11),
    'arena2':     (2, '11',   SIDE, 'stunt', 11),
    'arena2_stacked': (2, '11', STACKED, 'stunt', 11),
    'arena4':     (4, '1111', {}, 'stunt', 11),
    'battle1':    (1, '1',    {}, 'battle', 30),
    'battle2':    (2, '11',   SIDE, 'battle', 30),
    'battle4':    (4, '1111', {}, 'battle', 30),
    # A race with the BATTLE row (Rush 2049's bar, weapon and power-up over Rush 2's HUD; R2_BATTLE_TEST=give4 hands
    # out a weapon so it and its ammo show).
    'race_battle1': (1, '1',  RACE_BATTLE),
    'race_battle2': (2, '11', {**RACE_BATTLE, **SIDE}),
    'race_battle4': (4, '1111', RACE_BATTLE),
    'race_battle4_stunts': (4, '1111', {**RACE_BATTLE, **STUNTS}),
    'race_battle4_43': (4, '1111', {**RACE_BATTLE, **STUNTS, 'graphics.json': {'ar_option': 'Original'}}),
    'race_battle2_stacked_43': (2, '11', {**RACE_BATTLE, **STUNTS, 'graphics.json': {'split_screen': 'TopBottom', 'ar_option': 'Original'}}),
    # The DEATHS option's skull, the stunt panel and every gearbox manual.
    'deaths1':    (1, '1',    {**DEATHS, **STUNTS}),
    'deaths2_side': (2, '11', {**DEATHS, **STUNTS, **SIDE}),
    'deaths2_stacked': (2, '11', {**DEATHS, **STUNTS, **STACKED}),
    'deaths4':    (4, '1111', {**DEATHS, **STUNTS}),
    'p1':         (1, '1',    {}),
    'p1_auto':    (1, '0',    {}),
    'p2_stacked': (2, '10',   {'graphics.json': {'split_screen': 'TopBottom'}}),
    'p2_side':    (2, '10',   {'graphics.json': {'split_screen': 'SideBySide'}}),
    'p2_side_noodo': (2, '01', {'graphics.json': {'split_screen': 'SideBySide'}, 'games.json': {'rush2049_odometer': False}}),
    'p3':         (3, '101',  {}),
    'p4':         (4, '1010', {}),
    'p4_noodo':   (4, '0101', {'games.json': {'rush2049_odometer': False}}),
    'p4_stunts':  (4, '1010', {'cheats.json': {'cheat_stunts_all_tracks': True}}),
    'p2_stunts':  (2, '10',   {'graphics.json': {'split_screen': 'SideBySide'}, 'cheats.json': {'cheat_stunts_all_tracks': True}}),
}
TRACK = 2  # a fog-free Rush 2 track (docs/players4.md)


ROWS = {'race': 0, 'stunt': 4, 'battle': 5}  # Start Game rows down from ONE RACE (docs/menus.md#start-game-rows)


def script(players, mode='race'):
    rows = ROWS[mode]
    s = ['8:START'] + ['%.1f:DD' % (11 + 0.6 * i) for i in range(rows)]
    t = 11 + 0.6 * rows
    s.append('%.1f:A' % t)
    joins = ['P2START', 'P3START', 'P4START']
    for i in range(players - 1):
        s.append('%.1f:%s' % (t + 1 + i * 0.5, joins[i]))
    pads = ['A', 'P2A', 'P3A', 'P4A'][:players]
    start = int(t + 1.5 + 0.5 * players)  # after the joins
    for at in range(start, start + 22, 2):
        s += ['%d:%s' % (at, p) for p in pads]
    s += ['%d:%s:60' % (start + 23, p) for p in pads]
    return ','.join(s)


def set_json(path, values):
    with open(path) as f:
        d = json.load(f)
    for k, v in values.items():
        d[k] = v
    with open(path, 'w', newline='') as f:
        json.dump(d, f, indent=4)


def run(out, name):
    players, manual, settings = CASES[name][:3]
    mode = CASES[name][3] if len(CASES[name]) > 3 else 'race'
    track = CASES[name][4] if len(CASES[name]) > 4 else TRACK
    case_dir = os.path.join(out, name)
    if os.path.exists(case_dir):
        shutil.rmtree(case_dir)
    os.makedirs(case_dir)
    backups = {}
    files = [f for f in settings if f not in ('track_select', 'env')]
    for file in files + ['saves/rush2.n64.us.json']:
        backups[file] = open(os.path.join(BUILD, file), 'rb').read()
    try:
        for file in files:
            set_json(os.path.join(BUILD, file), settings[file])
        save = os.path.join(BUILD, 'saves', 'rush2.n64.us.json')
        with open(save) as f:
            d = json.load(f)
        d.setdefault('track_select', {})['selected'] = -1
        d['track_select'].update(settings.get('track_select', {}))
        with open(save, 'w', newline='') as f:
            json.dump(d, f, indent=4)
        env = dict(os.environ, RUSH2_HUD_CAPTURE=os.path.abspath(case_dir), RUSH2_TEST_TRACK=str(track),
                   RUSH2_TEST_MANUAL=manual, RUSH2_TEST_BONUS_TIME='22', RUSH2_TEST_FINISH='30')
        if 'track_select' in settings:
            env['R2_BATTLE_TEST'] = 'give4'
        env.update(settings.get('env', {}))
        if players >= 3:
            env['RUSH2_TEST_PLAYERS'] = str(players)
        # 1-2 player races start about 22 s in, 3-4 player ones about 36 s (more players to join and pick)
        times = ['23', '25', '32', '40', '46', '54', '60'] if players <= 2 else ['37', '39', '46', '54', '60', '68', '76']
        if mode != 'race':
            times = ['30', '36', '42', '48', '54', '62', '70']
        subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'rush1', 'shots.py'), case_dir, script(players, mode), *times],
                       env=env, cwd=REPO, timeout=180)
    finally:
        for file, data in backups.items():
            with open(os.path.join(BUILD, file), 'wb') as f:
                f.write(data)
    frames = os.path.join(case_dir, 'frames.jsonl')
    n = sum(1 for _ in open(frames)) if os.path.exists(frames) else 0
    print('%s: %d frames' % (name, n))


def main():
    if '--list' in sys.argv:
        for k, c in CASES.items():
            p, m, st = c[:3]
            print('%-16s %d players, manual %s, %s %s' % (k, p, m, c[3] if len(c) > 3 else 'race', st or 'current settings'))
        return
    out, names = sys.argv[1], sys.argv[2:]
    for n in names:
        if n not in CASES:
            sys.exit('unknown case %s (--list)' % n)
    saves = os.path.join(BUILD, 'saves')
    backup = os.path.join(REPO, 'tmp', 'hud_capture_saves_backup')
    if os.path.exists(backup):
        shutil.rmtree(backup)
    shutil.copytree(saves, backup)
    try:
        for n in names:
            run(out, n)
    finally:
        shutil.rmtree(saves)
        shutil.copytree(backup, saves)
    subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'pak_check.py')], cwd=REPO)


if __name__ == '__main__':
    main()
