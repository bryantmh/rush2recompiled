#!/usr/bin/env python3
"""Stress test online lobbies with many ways of splitting the players over machines (test aid, docs/online.md#test-aids).

    python tools/online_matrix.py OUT_DIR 8x1 4x2 2x4 1+3+4 3+3+2 --track 5 --finish 40

Each layout is the players per machine, the host first: "1+3+4" = a host with 1 player and joiners with 3 and 4
("8x1" = eight machines of one). For each it runs tools/online_test.py (quick race, RUSH2_ONLINE_TEST host:N/join, its
own port) with every machine driving its players (RUSH2_ONLINE_BOT, a seed each) and logging the state hash
(RUSH2_ONLINE_HASHLOG), then reads the machines' console logs and reports per layout: the race's load state on each
machine (must match), the frame hashes (every machine's hash at a frame must match the others'), resyncs, dropped
players, the frame the race ended on each machine, and any instance that exited early (a crash; launch from
PowerShell, not Git Bash, so a crash leaves a dump in %LOCALAPPDATA%\\CrashDumps). Cars cycle through
--cars (Rush 2, Rush 2049, California Speed types by default) so each layout races a mix. --analyze only re-reads an
earlier run's logs.
Options: --track ID, --finish S (RUSH2_TEST_FINISH), --drones N, --stagger S, --time S (how long a layout runs),
--env "NAME=VALUE ..." added to every instance, --shots T,T more screenshot times. Runs muted (tools/online_test.py).
"""
import argparse, os, re, subprocess, sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def parse_layout(text):
    if 'x' in text:
        n, k = text.split('x')
        return [int(k)] * int(n)
    return [int(v) for v in text.split('+')]


def analyze(out, layout):
    machines = []
    for i in range(len(layout)):
        name = 'm%d' % i
        path = os.path.join(out, name + '.log')
        text = open(path, errors='replace').read() if os.path.exists(path) else ''
        m = {'name': name, 'players': layout[i], 'hashes': {}, 'resyncs': [], 'drops': [], 'errors': []}
        for line in text.splitlines():
            if g := re.search(r'race loaded at frame \d+, state (\w+), track (\w+), cars (\w+) \((\d+) players', line):
                m['load'] = (g[1], g[2], g[3])
                m['race_players'] = int(g[4])
            elif g := re.search(r'\[online\] frame (\d+) hash (\w+)', line):
                m['hashes'][int(g[1])] = g[2]
            elif g := re.search(r'\[online\] resync \d+ at frame (\d+): (.*)', line):
                m['resyncs'].append((int(g[1]), g[2][:120]))
            elif g := re.search(r'race ended before frame (\d+)', line):
                m.setdefault('finished', []).append((int(g[1]), line.strip()))
            elif 'dropped player' in line or 'left the race' in line:
                m['drops'].append(line.strip())
            elif g := re.search(r'\[online\] race (over|quit) at frame (\d+)', line):
                m['end'] = '%s@%s' % (g[1], g[2])
            elif re.search(r"race ended:|can't start|rejected|lost the lobby|can't be held|set the cars up "
                           r"differently|Assertion|exception|Unhandled", line, re.I):
                m['errors'].append(line.strip()[:160])
        # A joiner's race ending on the host's race-over frame is the normal end (Msg::Finished); earlier is not.
        end_frame = int(m['end'].split('@')[1]) if 'end' in m else 1 << 30
        m['drops'] += [line for f, line in m.pop('finished', []) if f < end_frame]
        machines.append(m)
    ok = True
    lines = []
    loads = {m.get('load') for m in machines}
    if None in loads or len(loads) != 1:
        ok = False
        lines.append('  load states differ or missing: ' + ', '.join('%s=%s' % (m['name'], m.get('load')) for m in machines))
    frames = sorted(set().union(*(m['hashes'] for m in machines)))
    bad = [f for f in frames if len({m['hashes'][f] for m in machines if f in m['hashes']}) > 1]
    common = [f for f in frames if all(f in m['hashes'] for m in machines)]
    if bad:
        ok = False
        lines.append('  hash differs at frames %s' % bad[:10])
    for m in machines:
        if m['resyncs'] or m['drops'] or m['errors'] or 'end' not in m:
            ok = False
        lines.append('  %s (%d local): players %s, hashes %d, end %s, resyncs %d%s%s%s' % (
            m['name'], m['players'], m.get('race_players'), len(m['hashes']), m.get('end'), len(m['resyncs']),
            ''.join('\n      resync @%d %s' % r for r in m['resyncs'][:4]),
            ''.join('\n      ' + d for d in m['drops'][:4]),
            ''.join('\n      ' + e for e in m['errors'][:4])))
    ends = {m.get('end') for m in machines}
    if len(ends) != 1:
        ok = False
        lines.append('  race end differs: ' + ', '.join('%s=%s' % (m['name'], m.get('end')) for m in machines))
    lines.insert(0, '%s %s: %s (%d frames hashed on all, last %s)' % (
        '+'.join(map(str, layout)), 'OK' if ok else 'PROBLEM', out, len(common), common[-1] if common else None))
    return ok, '\n'.join(lines)


ap = argparse.ArgumentParser()
ap.add_argument('out')
ap.add_argument('layouts', nargs='+')
ap.add_argument('--track', default='5')
ap.add_argument('--finish', default='40')
ap.add_argument('--drones')
ap.add_argument('--cars', default='0,24,3,30,40,7,27,12,33,50,18,23')
ap.add_argument('--stagger', type=float, default=3.0)
ap.add_argument('--time', type=float, default=0, help='seconds a layout runs (default: by machine count)')
ap.add_argument('--port', type=int, default=47300)
ap.add_argument('--env', default='')
ap.add_argument('--shots', default='', help='more screenshot times (s from the first launch), comma separated')
ap.add_argument('--analyze', action='store_true')
args = ap.parse_args()

cars = args.cars.split(',')
results = []
for li, text in enumerate(args.layouts):
    layout = parse_layout(text)
    out = os.path.join(args.out, text.replace('+', '_'))
    if not args.analyze:
        total = sum(layout)
        port = args.port + li
        common = 'RUSH2_ONLINE_HASHLOG=1 RUSH2_ONLINE_TRACK=%s RUSH2_TEST_FINISH=%s %s' % (args.track, args.finish, args.env)
        if args.drones is not None:
            common += ' RUSH2_ONLINE_DRONES=' + args.drones
        cmd = [sys.executable, os.path.join(REPO, 'tools', 'online_test.py'), out, '--stagger', str(args.stagger)]
        at = 0
        for i, n in enumerate(layout):
            mine = ','.join(cars[(at + k + li) % len(cars)] for k in range(n))
            at += n
            role = 'host:%d RUSH2_ONLINE_PORT=%d' % (total, port) if i == 0 else 'join:127.0.0.1:%d' % port
            cmd += ['--instance', 'm%d|RUSH2_ONLINE_TEST=%s RUSH2_ONLINE_LOCAL=%d RUSH2_ONLINE_CARS=%s '
                    'RUSH2_ONLINE_BOT=%d RUSH2_ONLINE_NAME=m%d %s|' % (i, role, n, mine, 101 + i * 7, i, common)]
        run_time = args.time or (len(layout) * args.stagger + float(args.finish) + 45)
        shots = [len(layout) * args.stagger + 30, run_time] + [float(t) for t in args.shots.split(',') if t]
        cmd += ['--times', ','.join('%g' % t for t in sorted(set(shots)))]   # mid-race, after and --shots
        print('running', text, 'for %.0f s' % run_time, flush=True)
        # Its output is passed on as it comes (the instances' PIDs, for a debugger to attach) and kept in run.txt.
        os.makedirs(out, exist_ok=True)
        with open(os.path.join(out, 'run.txt'), 'w') as f:
            run = subprocess.Popen(cmd, cwd=REPO, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            for line in run.stdout:
                print(line, end='', flush=True)
                f.write(line)
            run.wait(timeout=run_time + 90)
    ok, report = analyze(out, layout)
    # online_test.py reports an instance that exited before the end (a crash: code 3221225477 = access violation).
    run_text = open(os.path.join(out, 'run.txt')).read() if os.path.exists(os.path.join(out, 'run.txt')) else ''
    for line in run_text.splitlines():
        if ' exited before ' in line:
            ok = False
            report = report.replace(' OK: ', ' PROBLEM: ', 1) + '\n  ' + line.strip()
    print(report, flush=True)
    results.append((text, ok))
print('summary: ' + ', '.join('%s %s' % (t, 'OK' if ok else 'PROBLEM') for t, ok in results))
