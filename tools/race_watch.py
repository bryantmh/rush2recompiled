#!/usr/bin/env python3
"""Watches a running race from outside the game (read-only, ReadProcessMemory) and logs what every car does: laps,
finishing, wrecks, respawns and stuck spells, with where each happened on the lap (test aid).

    python tools/race_watch.py --pid PID [--seconds S] [--path PATH_BIN] [--out DIR]
    python tools/race_watch.py --launch "<input script>" [--seconds S] [--path PATH_BIN] [--out DIR]

--launch starts build/Rush2Recompiled.exe with that --input-script (environment variables such as RUSH2_TEST_TRACK
pass through), watches it, and closes that process (by its PID) at the end. --pid watches a game already running and
leaves it alone. --path takes the race's path file (tools/track_mod_test.bat output, path.bin) so events are placed
by spine point and lap distance. Writes DIR/samples.jsonl (every car every 0.1 s, 'wall' the seconds since launch), DIR/events.txt and (--launch) DIR/pid.txt, prints the
summary. Stops early once every computer car has finished or the game exits.

--trace CAR instead logs every change of that car's wreck and drawing fields (TRACE_FIELDS: wrecked +0x648, flags
+0x7F4 with 0x8 blink-translucent and 0x10 hidden, armed +0x804, glide +0x6C8, state +0x300/+0x304/+0x308/+0x309
blink, +0x342 wreck state, +0xE4 drawn flags), polled every ~2 ms, to DIR/trace.txt (wreck to respawn timing).
--wreck-at S (with --trace) wrecks that car S wall-clock seconds after the race state begins, writing +0x648 = 1 as a
crash does (the only write this tool makes).
--records PLACEMENT_BIN instead samples every placement record (the converted track's placement.bin, from
tools/track_mod_test.bat, gives their count and names; RDRAM: pointer 0x8010C15C, 0x64 bytes each, matrix +0x10,
position +0x34) every 0.37 s for --seconds of the race and prints the ones whose matrix or position changed, and
how far: moving objects that should turn or travel but don't show up as never changing.
--peek ADDR (repeatable, hex) instead prints the u32 at each RDRAM address during the race, smallest and largest seen
over --seconds (scene_node_table_count 0x800FAE58: how full the scene node table at 0x80400000 is (include/scene_nodes.h)); --peek-always
samples in every game state too (a race that crashes while it loads never reaches the race state); --peek-changes
instead prints a line, timed from launch, each time one of them changes (menu and attract timelines).
--pos (with --trace) adds the car's positions, rounded to a foot: physics +0x224, car state +0 (where a computer car's
body node is put, cam_update_all_players_8009D9F4) and the body node's own (node +0xF0 of the table at 0x80400000, 0x38 each, its
matrix +4, position +0x24; nhid = its hidden flag 0x400), logging a line whenever one moves.
RDRAM is found by matching the boot code at 0x80000450 from the Rush 2 ROM (tools/rush2049/roms.py; RUSH2_ROM).
Fields: car state 0x801124A0 + car x 0x354 (position +0, place +0xE9, finished +0xEA, wreck state +0x342, out of
deaths +0x343), physics car 0x800F5470 + car x 0x81C (laps +0x7FD, next checkpoint +0x7FE, brake +0x730, throttle
+0x734, kind +0x7E6: 0 human, type +0x7EA, up = row 1 y of the world -> car matrix +0x2CC: below 0 upside down), game time 0x80117488, game state 0x8010C0D0, cars 0x8010C15A.
"""
import argparse, ctypes, ctypes.wintypes as wt, json, math, os, re, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
import roms, paths  # noqa: E402

k32 = ctypes.WinDLL('kernel32', use_last_error=True)


class MBI(ctypes.Structure):
    _fields_ = [('BaseAddress', ctypes.c_void_p), ('AllocationBase', ctypes.c_void_p), ('AllocationProtect', wt.DWORD),
                ('PartitionId', wt.WORD), ('RegionSize', ctypes.c_size_t), ('State', wt.DWORD),
                ('Protect', wt.DWORD), ('Type', wt.DWORD)]


k32.VirtualQueryEx.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.POINTER(MBI), ctypes.c_size_t]
k32.ReadProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                  ctypes.POINTER(ctypes.c_size_t)]
k32.OpenProcess.restype = wt.HANDLE
k32.WriteProcessMemory.argtypes = [wt.HANDLE, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t,
                                   ctypes.POINTER(ctypes.c_size_t)]


class Rdram:
    """Read-only view of a running game's RDRAM (words native-endian, bytes at address ^ 3, halves at ^ 2)."""

    def __init__(self, pid, write=False):
        # PROCESS_QUERY_INFORMATION | PROCESS_VM_READ (| PROCESS_VM_OPERATION | PROCESS_VM_WRITE for poke)
        self.h = k32.OpenProcess(0x0438 if write else 0x0410, False, pid)
        r2 = roms.Rush2()
        want = list(struct.unpack('>4I', r2.read(0x80000450, 16)))
        self.base = None
        addr, m = 0, MBI()
        while k32.VirtualQueryEx(self.h, ctypes.c_void_p(addr), ctypes.byref(m), ctypes.sizeof(m)):
            if m.State == 0x1000 and m.RegionSize >= 0x400000:
                d = self._read(m.BaseAddress + 0x450, 16)
                if d and list(struct.unpack('<4I', d)) == want:
                    self.base = m.BaseAddress
                    break
            addr = (m.BaseAddress or 0) + m.RegionSize
            if addr >= 1 << 47:
                break

    def _read(self, addr, n):
        buf, got = ctypes.create_string_buffer(n), ctypes.c_size_t()
        ok = k32.ReadProcessMemory(self.h, ctypes.c_void_p(addr), buf, n, ctypes.byref(got))
        return buf.raw if ok else None

    def block(self, vaddr, n):
        return self._read(self.base + (vaddr - 0x80000000), n)

    def poke_byte(self, vaddr, value):
        """Writes one byte at a game address (test aid; needs Rdram(pid, write=True))."""
        buf, done = ctypes.c_ubyte(value & 0xFF), ctypes.c_size_t()
        return k32.WriteProcessMemory(self.h, ctypes.c_void_p(self.base + ((vaddr ^ 3) - 0x80000000)),
                                      ctypes.byref(buf), 1, ctypes.byref(done))


SCENE_NODES = 0x80400000   # the scene node table (include/scene_nodes.h; the game's 0x800D9E90 moved there)


def word_view(blob, start):
    """Accessors into a block read at vaddr `start`."""
    def w(a): return struct.unpack_from('<I', blob, a - start)[0]
    def f(a): return struct.unpack_from('<f', blob, a - start)[0]
    def b(a): return struct.unpack_from('<b', blob, (a ^ 3) - start)[0]
    def h(a): return struct.unpack_from('<h', blob, (a ^ 2) - start)[0]
    return w, f, b, h


STATES, STATE_SIZE = 0x801124A0, 0x354
CARS, CAR_SIZE = 0x800F5470, 0x81C


def sample(rd):
    s = rd.block(STATES, STATE_SIZE * 8)
    c = rd.block(CARS, CAR_SIZE * 8)
    g = rd.block(0x8010C0D0, 0x100)
    t = rd.block(0x80117488, 4)
    clock = rd.block(0x8010C204, 4)   # race_time_float: the checkpoint clock (time left)
    if not (s and c and g and t and clock):
        return None
    _, sf, sb, _ = word_view(s, STATES)
    _, cf, cb, ch = word_view(c, CARS)
    gw, _, gb, _ = word_view(g, 0x8010C0D0)
    out = {'time': struct.unpack('<f', t)[0], 'state': gw(0x8010C0D0), 'cars': gb(0x8010C15A),
           'clock': round(struct.unpack('<f', clock)[0], 2), 'list': []}
    for k in range(8):
        st, ph = STATES + k * STATE_SIZE, CARS + k * CAR_SIZE
        out['list'].append({'pos': [round(sf(st + i * 4), 1) for i in range(3)], 'place': sb(st + 0xE9),
                            'finished': sb(st + 0xEA), 'wreck': sb(st + 0x342), 'out': sb(st + 0x343),
                            'laps': cb(ph + 0x7FD), 'next_cp': ch(ph + 0x7FE), 'throttle': round(cf(ph + 0x734), 2),
                            'brake': round(cf(ph + 0x730), 2), 'human': ch(ph + 0x7E6) == 0,
                            'up': round(cf(ph + 0x2DC), 2), 'type': cb(ph + 0x7EA)})
    return out


class Locator:
    """Spine point and lap distance of a position (nearest spine point in 3D, height weighted down)."""

    def __init__(self, path_bin):
        p = paths.parse(open(path_bin, 'rb').read())
        self.spine = p['spine']
        self.lanes = [ln['points'] for ln in p['lanes']]

    def where(self, pos):
        i = min(range(len(self.spine)), key=lambda k: (self.spine[k][0] - pos[0]) ** 2 + (self.spine[k][2] - pos[2]) ** 2
                + ((self.spine[k][1] - pos[1]) * 0.5) ** 2)
        s = self.spine[i]
        a, b = self.spine[i - 1], self.spine[(i + 1) % len(self.spine)]
        t = (b[0] - a[0], b[2] - a[2])
        n = math.hypot(*t) or 1.0
        side = ((pos[0] - s[0]) * -t[1] + (pos[2] - s[2]) * t[0]) / n
        lane_mph = [ln[i][3] for ln in self.lanes]
        return 'spine %d (%.0f ft along), %+.0f ft from the centre line, %+.0f ft above it, lanes %s mph' % (
            i, i * 20.0, side, pos[1] - s[1], '/'.join(str(v) for v in lane_mph))


TRACE_FIELDS = [('wrecked', 'car', 0x648, 'b'), ('flags', 'car', 0x7F4, 'x'), ('armed', 'car', 0x804, 'h'),
                ('glide', 'car', 0x6C8, 'h'), ('s300', 'st', 0x300, 'h'), ('s304', 'st', 0x304, 'f'),
                ('s308', 'st', 0x308, 'b'), ('s309', 'st', 0x309, 'b'), ('wreck', 'st', 0x342, 'b'),
                ('drawn', 'st', 0xE4, 'x')]


def trace(rd, car, seconds, out, proc, wreck_at=None, pos=False):
    """Logs each change of car's TRACE_FIELDS with the game time; wreck_at wrecks the car (+0x648 = 1, as a crash
    does) that many seconds into the race."""
    lines, prev, t0, race_start = [], None, time.time(), None
    while time.time() - t0 < seconds and not (proc and proc.poll() is not None):
        s = rd.block(STATES + car * STATE_SIZE, STATE_SIZE)
        c = rd.block(CARS + car * CAR_SIZE, CAR_SIZE)
        t = rd.block(0x80117488, 4)
        g = rd.block(0x8010C0D0, 4)
        if not (s and c and t and g):
            time.sleep(0.01)
            continue
        now = struct.unpack('<f', t)[0]
        if struct.unpack('<I', g)[0] == 3 and race_start is None:
            race_start = time.time()
        if wreck_at is not None and race_start is not None and time.time() - race_start >= wreck_at:
            rd.poke_byte(CARS + car * CAR_SIZE + 0x648, 1)
            lines.append('%8.3f  -- wrecked by race_watch' % now)
            print(lines[-1], flush=True)
            wreck_at = None
        views = {'st': word_view(s, STATES + car * STATE_SIZE), 'car': word_view(c, CARS + car * CAR_SIZE)}
        base = {'st': STATES + car * STATE_SIZE, 'car': CARS + car * CAR_SIZE}
        cur = {}
        for name, where, off, kind in TRACE_FIELDS:
            w, f, b, h = views[where]
            a = base[where] + off
            cur[name] = {'b': b, 'h': h, 'f': lambda x: round(f(x), 2), 'x': lambda x: '%X' % w(x)}[kind](a)
        if pos:
            def xyz(blob, start):
                return '%d,%d,%d' % tuple(round(struct.unpack_from('<f', blob, start + i * 4)[0]) for i in range(3))
            cur['ppos'] = xyz(c, 0x224)
            cur['spos'] = xyz(s, 0)
            node = struct.unpack_from('<i', s, 0xF0)[0]
            m = rd.block(SCENE_NODES + node * 0x38 + 4, 4) if node >= 0 else None
            mat = rd.block(struct.unpack('<I', m)[0] + 0x24, 12) if m and struct.unpack('<I', m)[0] >= 0x80000000 else None
            cur['npos'] = xyz(mat, 0) if mat else '-'
            nf = rd.block(SCENE_NODES + node * 0x38, 4) if node >= 0 else None
            cur['nhid'] = (struct.unpack('<I', nf)[0] >> 10) & 1 if nf else '-'
        key = {k: v for k, v in cur.items() if k != 's304'}
        if prev is None or key != prev:
            line = '%8.3f  ' % struct.unpack('<f', t)[0] + '  '.join('%s=%s' % kv for kv in cur.items())
            lines.append(line)
            print(line, flush=True)
            prev = key
        time.sleep(0.002)
    with open(os.path.join(out, 'trace.txt'), 'w') as f:
        f.write('\n'.join(lines) + '\n')


def records(rd, placement_bin, seconds, proc):
    """Samples the placement records' matrices and positions every 0.37 s during the race; prints each record's
    largest matrix and position change, moving ones first."""
    pl = open(placement_bin, 'rb').read()
    count = (len(pl) - 0x18) // 0x64
    names = [pl[0x18 + i * 0x64:0x18 + i * 0x64 + 16].split(b'\0')[0].decode('latin1') for i in range(count)]
    snaps, t0 = [], time.time()
    while time.time() - t0 < seconds and not (proc and proc.poll() is not None):
        g, b = rd.block(0x8010C0D0, 4), rd.block(0x8010C15C, 4)
        if g and b and struct.unpack('<I', g)[0] == 3:
            base = struct.unpack('<I', b)[0]
            blob = rd.block(base, count * 0x64) if base >= 0x80000000 else None
            if blob:
                snaps.append([struct.unpack_from('<12f', blob, i * 0x64 + 0x10) for i in range(count)])
        time.sleep(0.37)    # not a whole second: flipbooks loop in 0.5 and 1 s
    if len(snaps) < 2:
        print('no race samples')
        return
    rows = []
    for i in range(count):
        dm = max(abs(s[i][k] - snaps[0][i][k]) for s in snaps for k in range(9))
        dp = max(math.dist(s[i][9:12], snaps[0][i][9:12]) for s in snaps)
        rows.append((dm + dp > 1e-4, dm, dp, i))
    print('%d samples, %d records, %d changed' % (len(snaps), count, sum(r[0] for r in rows)))
    for moved, dm, dp, i in sorted(rows, key=lambda r: (not r[0], r[3])):
        if moved or re.search(r'M\d{3}$', names[i]):
            print('  %4d %-16s matrix change %.3f, moved %.1f ft, at (%.0f, %.0f, %.0f)' % (
                i, names[i], dm, dp, *snaps[-1][i][9:12]))


def peek(rd, addrs, seconds, proc, always=False):
    """Samples u32 words at addrs during the race (always: in every state); prints each one's range."""
    seen, t0 = {a: [] for a in addrs}, time.time()
    while time.time() - t0 < seconds and not (proc and proc.poll() is not None):
        g = rd.block(0x8010C0D0, 4)
        if g and (always or struct.unpack('<I', g)[0] == 3):
            for a in addrs:
                v = rd.block(a, 4)
                if v:
                    seen[a].append(struct.unpack('<I', v)[0])
        time.sleep(0.25)
    for a, vs in seen.items():
        print('0x%08X: ' % a + ('min %d, max %d (%d samples)' % (min(vs), max(vs), len(vs)) if vs else 'no race samples'))


def peek_changes(rd, addrs, seconds, proc, launched):
    """Polls u32 words at addrs in every game state and prints a line, timed from launch, whenever one changes."""
    last, t0 = {}, time.time()
    while time.time() - t0 < seconds and not (proc and proc.poll() is not None):
        for a in addrs:
            v = rd.block(a, 4)
            if v:
                v = struct.unpack('<I', v)[0]
                if last.get(a) != v:
                    print('%7.2f  0x%08X = 0x%08X' % (time.time() - launched, a, v), flush=True)
                    last[a] = v
        time.sleep(0.02)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pid', type=int)
    ap.add_argument('--launch')
    ap.add_argument('--exe', help='with --launch: run this game exe instead of build/Rush2Recompiled.exe (a release '
                    "build such as .release/build's, to compare with the local build)")
    ap.add_argument('--seconds', type=float, default=360)
    ap.add_argument('--path')
    ap.add_argument('--out', default=os.path.join(REPO, 'tmp', 'race_watch'))
    ap.add_argument('--trace', type=int, help="log changes of this car's wreck and drawing fields instead")
    ap.add_argument('--records', help='placement.bin of the raced custom track: report which records move')
    ap.add_argument('--pos', action='store_true', help="with --trace: also log the car's positions")
    ap.add_argument('--peek', action='append', type=lambda v: int(v, 16), help='print this u32 during the race')
    ap.add_argument('--peek-always', action='store_true', help='with --peek: sample in every game state, not just racing')
    ap.add_argument('--peek-changes', action='store_true', help='with --peek: print every change (timed from launch, '
                    'every game state) instead of the range')
    ap.add_argument('--wreck-at', type=float, help='with --trace: wreck that car this many seconds into the race')
    ap.add_argument('--shots', help='screenshot the window at these seconds after launch (comma separated) into '
                    'DIR/shot_<seconds>.png; a simulation run needs RUSH2_SIM_SHOW=1 to show its window')
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    proc = None
    shots = sorted(float(v) for v in a.shots.split(',')) if a.shots else []
    if shots:
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rush1'))
        import shots as shot_tool

    def take_shots():
        # The shots that are due, also before the game has started (the launcher's build progress screen).
        while shots and time.time() - launched >= shots[0]:
            hwnd = shot_tool.find_window(pid)
            if hwnd:
                shot_tool.capture(hwnd, os.path.join(a.out, 'shot_%g.png' % shots[0]))
            shots.pop(0)

    def wait(seconds):
        end = time.time() + seconds
        while time.time() < end:
            take_shots()
            time.sleep(0.05)
    if a.launch:
        exe = os.path.abspath(a.exe) if a.exe else os.path.join(REPO, 'build', 'Rush2Recompiled.exe')
        log = open(os.path.join(a.out, 'run.log'), 'w')
        proc = subprocess.Popen([exe, '--input-script', a.launch], cwd=os.path.dirname(exe), stdout=log,
                                stderr=subprocess.STDOUT)
        pid = proc.pid
        with open(os.path.join(a.out, 'pid.txt'), 'w') as f:   # for tools attaching to this run
            f.write(str(pid))
        launched = time.time()
        wait(4)
    else:
        pid = a.pid
        launched = time.time()
    rd = None
    for _ in range(60):
        rd = Rdram(pid, write=a.wreck_at is not None)
        if rd.base:
            break
        wait(1)
    if not rd or not rd.base:
        print('RDRAM not found')
        return 1
    if a.peek and a.peek_changes:
        peek_changes(rd, a.peek, a.seconds, proc, launched)
        if proc and proc.poll() is None:
            proc.terminate()
        return 0
    if a.peek:
        peek(rd, a.peek, a.seconds, proc, a.peek_always)
        if proc and proc.poll() is None:
            proc.terminate()
        return 0
    if a.records:
        records(rd, a.records, a.seconds, proc)
        if proc and proc.poll() is None:
            proc.terminate()
        return 0
    if a.trace is not None:
        trace(rd, a.trace, a.seconds, a.out, proc, a.wreck_at, a.pos)
        if proc and proc.poll() is None:
            proc.terminate()
        return 0
    loc = Locator(a.path) if a.path else None
    where = (lambda p: loc.where(p)) if loc else (lambda p: '(%.0f, %.0f, %.0f)' % tuple(p))
    samples = open(os.path.join(a.out, 'samples.jsonl'), 'w')
    events = []
    prev, still_since, started, t0 = None, {}, None, time.time()
    finish_time = {}
    def event(t, car, text, pos):
        line = '%7.1f s  car %d  %s  at %s' % (t, car, text, where(pos))
        events.append(line)
        print(line, flush=True)
    while time.time() - t0 < a.seconds:
        if proc and proc.poll() is not None:
            break
        take_shots()
        cur = sample(rd)
        if cur is None:
            time.sleep(0.1)
            continue
        cur['wall'] = round(time.time() - launched, 2)   # seconds since launch (or since attaching)
        samples.write(json.dumps(cur) + '\n')
        n = max(1, min(8, cur['cars']))
        racing = cur['state'] == 3
        if racing and started is None:
            started = cur['time']
        if prev and racing and prev['state'] == 3:
            t = cur['time'] - started
            for k in range(n):
                c, p = cur['list'][k], prev['list'][k]
                if c['wreck'] and not p['wreck']:
                    event(t, k, 'wrecked (state %d)' % c['wreck'], p['pos'])
                jump = math.dist(c['pos'], p['pos'])
                if jump > 60 and not c['wreck']:
                    event(t, k, 'put back %.0f ft from where it was (from %s)' % (jump, where(p['pos'])), c['pos'])
                if c['laps'] != p['laps']:
                    event(t, k, 'lap %d' % c['laps'], c['pos'])
                if c['finished'] == 1 and p['finished'] != 1:
                    finish_time[k] = t
                    event(t, k, 'finished, place %d' % (c['place'] + 1), c['pos'])
                moving = jump > 0.5
                if c['finished'] == 1 or c['human'] or moving or c['wreck']:
                    if still_since.get(k, (None,))[0] is not None and t - still_since[k][0] >= 3:
                        event(t, k, 'moving again after %.0f s still' % (t - still_since[k][0]), c['pos'])
                    still_since[k] = (None,)
                elif still_since.get(k, (None,))[0] is None:
                    still_since[k] = (t, False)
                elif not still_since[k][1] and t - still_since[k][0] >= 3:
                    still_since[k] = (still_since[k][0], True)
                    event(t, k, 'stuck (still 3 s, throttle %.2f)' % c['throttle'], c['pos'])
            drones = [k for k in range(n) if not cur['list'][k]['human']]
            if drones and all(cur['list'][k]['finished'] == 1 for k in drones):
                print('every computer car finished')
                break
        prev = cur
        time.sleep(0.1)
    samples.close()
    last = prev
    summary = []
    if last:
        for k in range(max(1, min(8, last['cars']))):
            c = last['list'][k]
            summary.append('car %d%s: laps %d, %s, wrecks %d' % (
                k, ' (human)' if c['human'] else '', c['laps'],
                'finished %.1f s place %d' % (finish_time[k], c['place'] + 1) if k in finish_time else
                ('finished' if c['finished'] == 1 else 'not finished'),
                sum(1 for e in events if e.split('car ')[1].startswith('%d ' % k) and 'wrecked' in e)))
    with open(os.path.join(a.out, 'events.txt'), 'w') as f:
        f.write('\n'.join(events + [''] + summary) + '\n')
    print('\n'.join([''] + summary))
    if proc and proc.poll() is None:
        proc.terminate()
    return 0


if __name__ == '__main__':
    sys.exit(main())
