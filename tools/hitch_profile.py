"""Reports the long frames in a hitch profiler log and the code each thread ran during them (test aid).

Usage: python tools/hitch_profile.py LOG [--ms 20] [--game-ms 45] [--threads N] [--depth N] [--from S] [--to S] [--folded DIR]
                                     [--brief] [--busy] [--timeline S] [--blind MS] [--sends MS] [--sym-path DIR]

LOG comes from a run with RUSH2_HITCH_LOG=<file> (src/hitch_profiler.cpp, docs/hitch_profiler.md). A hitch is a gap
of more than --ms milliseconds between two RT64 presents (what a frame time overlay shows) or of more than --game-ms
between two display lists the game sent (it runs at 30 fps, so 33 ms is normal) (a game-side stall, which RT64 can hide by presenting the same picture again). For each hitch it
lists the threads that were busy and, per thread, the dominant call path: the chain of functions that most of that
thread's samples in the hitch went through, down to where it branches, then the branches. Symbols come from the PDBs
(the recompiled functions keep their names, e.g. race_setup_800A5ADC). At the end, the functions that account for the
most hitch time across the whole run. --folded writes each hitch's stacks in folded form (one file per hitch) for
flame graph viewers such as speedscope. --brief prints one line per busy game or graphics thread per hitch instead: its
dominant path below the thread's own loop (where a stall's cause is), for comparing runs. --busy drops each thread's
waiting samples and prints its running ones' dominant path with their CPU ms, so a thread that waited most of a hitch
still shows what it did in the rest (the default report shows only paths holding half of all samples).
"""
import argparse, bisect, collections, ctypes, ctypes.wintypes as wt, os, sys

ap = argparse.ArgumentParser()
ap.add_argument('log')
ap.add_argument('--ms', type=float, default=20, help='present gap threshold in ms')
ap.add_argument('--game-ms', type=float, default=45, help='game frame gap threshold in ms')
ap.add_argument('--threads', type=int, default=4, help='busiest threads shown per hitch')
ap.add_argument('--depth', type=int, default=14, help='frames of each dominant path shown')
ap.add_argument('--from', dest='t0', type=float, default=0, help='ignore hitches before this many seconds')
ap.add_argument('--to', dest='t1', type=float, default=1e9, help='ignore hitches after this many seconds')
ap.add_argument('--folded', help='write folded stacks of each hitch into this folder')
ap.add_argument('--brief', action='store_true', help='one line per busy game or graphics thread per hitch')
ap.add_argument('--busy', action='store_true', help='per thread, only the samples that were running (not waiting), '
                'weighted by CPU: where a thread that waited most of a hitch spent the rest')
ap.add_argument('--timeline', type=float, metavar='S', help='only print, per S-second bucket, the presents and game '
                'frames and their longest gaps (where the frame pacing changes: menus, loads, races), then stop')
ap.add_argument('--blind', type=float, metavar='MS', help='only list the times the sampler saw nothing for more than '
                'MS ms (a thread it suspends is inside a long kernel call, e.g. a GPU memory allocation) with '
                'the last stack of each thread before the gap, then stop')
ap.add_argument('--sends', type=float, metavar='MS', help='only list the send_dl calls (D to d: how long RT64 held the '
                'game\'s gfx thread) of at least MS ms between --from and --to, then stop')
ap.add_argument('--sym-path', help='folder holding a copy of the run\'s exe and its PDB (another session may have '
                'rebuilt build/ since the run)')
args = ap.parse_args()

# ---- parse -------------------------------------------------------------------------------------------------------
modules, names, stacks, marks = [], {}, {}, collections.defaultdict(list)
samples = []  # (us, tid, stack id, cycles)
other_marks = []
with open(args.log, encoding='utf-8', errors='replace') as f:
    for line in f:
        k = line[:1]
        if k == 's':
            _, t, tid, sid, cyc = line.split()
            samples.append((int(t), int(tid), int(sid), int(cyc)))
        elif k == 'S':
            p = line.split()
            stacks[int(p[1])] = [int(x, 16) for x in p[2:]]
        elif k == 'E':
            _, t, tag = line.rstrip('\n').split(' ', 2)
            if tag in ('P', 'p', 'D', 'd'):
                marks[tag].append(int(t))
            else:
                other_marks.append((int(t), tag))
        elif k == 'T':
            _, tid, name = line.rstrip('\n').split(' ', 2)
            names[int(tid)] = name
        elif k == 'M':
            _, base, size, path = line.rstrip('\n').split(' ', 3)
            modules.append((int(base, 16), int(size, 16), path))
samples.sort()
sample_times = [s[0] for s in samples]

if args.timeline:
    print('   from    presents  max gap   p99 gap   game frames  max gap')
    P, D = sorted(marks['P']), sorted(marks['D'])
    end = max(P[-1] if P else 0, D[-1] if D else 0)
    w = int(args.timeline * 1e6)
    for b0 in range(0, end, w):
        row = []
        for ts in (P, D):
            i, j = bisect.bisect_left(ts, b0), bisect.bisect_left(ts, b0 + w)
            g = sorted((ts[k] - ts[k - 1]) / 1e3 for k in range(max(i, 1), j))
            row.append((j - i, g[-1] if g else 0, g[int(len(g) * 0.99)] if g else 0))
        print('%7.1f s  %6d  %7.1f   %7.1f   %8d     %7.1f' % (b0 / 1e6, row[0][0], row[0][1], row[0][2], row[1][0], row[1][1]))
    sys.exit(0)

if args.sends is not None:
    D, d = sorted(marks['D']), sorted(marks['d'])
    spans = []
    for t in D:
        j = bisect.bisect_left(d, t)
        if j < len(d) and args.t0 * 1e6 <= t <= args.t1 * 1e6:
            spans.append((d[j] - t) / 1e3)
            if spans[-1] >= args.sends:
                print('%8.3f s  send_dl %6.1f ms' % (t / 1e6, spans[-1]))
    if spans:
        s = sorted(spans)
        print('%d send_dl calls: median %.1f ms, p90 %.1f ms, max %.1f ms, total %.0f ms' % (
            len(s), s[len(s) // 2], s[int(len(s) * 0.9)], s[-1], sum(s)))
    sys.exit(0)

# ---- symbols -----------------------------------------------------------------------------------------------------
dbghelp = ctypes.WinDLL('dbghelp')


class SYMBOL_INFOW(ctypes.Structure):
    _fields_ = [('SizeOfStruct', wt.ULONG), ('TypeIndex', wt.ULONG), ('Reserved', ctypes.c_uint64 * 2),
                ('Index', wt.ULONG), ('Size', wt.ULONG), ('ModBase', ctypes.c_uint64), ('Flags', wt.ULONG),
                ('Value', ctypes.c_uint64), ('Address', ctypes.c_uint64), ('Register', wt.ULONG),
                ('Scope', wt.ULONG), ('Tag', wt.ULONG), ('NameLen', wt.ULONG), ('MaxNameLen', wt.ULONG),
                ('Name', wt.WCHAR * 512)]


HPROC = wt.HANDLE(0x7E57)
dbghelp.SymSetOptions(0x2 | 0x4 | 0x10)  # UNDNAME | DEFERRED_LOADS | LOAD_LINES
dbghelp.SymInitializeW.argtypes = [wt.HANDLE, wt.LPCWSTR, wt.BOOL]
dbghelp.SymInitializeW(HPROC, args.sym_path, False)
dbghelp.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
for base, size, path in modules:
    # A copy of the run's own image (and its PDB beside it) in --sym-path wins over the one in the log's path, which a
    # later build may have replaced.
    if args.sym_path and os.path.exists(os.path.join(args.sym_path, os.path.basename(path))):
        path = os.path.join(args.sym_path, os.path.basename(path))
    dbghelp.SymLoadModuleExW(HPROC, None, path, None, base, size, None, 0)
mod_bases = sorted((b, b + s, os.path.basename(p)) for b, s, p in modules)
sym_cache = {}


def sym(pc):
    r = sym_cache.get(pc)
    if r is None:
        info = SYMBOL_INFOW()
        info.SizeOfStruct = 88  # sizeof(SYMBOL_INFOW) with its one-character Name
        info.MaxNameLen = 511
        disp = ctypes.c_uint64()
        i = bisect.bisect_right(mod_bases, (pc, float('inf'))) - 1
        mod = mod_bases[i][2] if i >= 0 and pc < mod_bases[i][1] else '?'
        mod = mod.replace('.exe', '').replace('.dll', '').replace('.DLL', '')
        if dbghelp.SymFromAddrW(HPROC, pc, ctypes.byref(disp), ctypes.byref(info)):
            r = info.Name if mod == 'Rush2Recompiled' else '%s!%s' % (mod, info.Name)
        else:
            r = '%s+%x' % (mod, pc)
        sym_cache[pc] = r
    return r


def sym_stack(sid):
    # Outermost first, with recursion folded so a path doesn't repeat itself.
    out = []
    for pc in reversed(stacks.get(sid, [])):
        s = sym(pc)
        if not out or out[-1] != s:
            out.append(s)
    return out


# ---- hitches -----------------------------------------------------------------------------------------------------
def gaps(times, kind, ms):
    return [(a, b, kind) for a, b in zip(times, times[1:]) if b - a > ms * 1000]


raw = sorted(gaps(marks['P'], 'present', args.ms) + gaps(marks['D'], 'game', args.game_ms))
hitches = []  # [start, end, {kind: longest gap ms}]
for a, b, kind in raw:
    if not args.t0 * 1e6 <= a <= args.t1 * 1e6:
        continue
    if hitches and a < hitches[-1][1]:
        h = hitches[-1]
        h[1] = max(h[1], b)
        h[2][kind] = max(h[2].get(kind, 0), (b - a) / 1000)
    else:
        hitches.append([a, b, {kind: (b - a) / 1000}])


def tick_ms():
    # Typical spacing of one thread's samples (the sampler's tick).
    by = collections.defaultdict(list)
    for t, tid, _, _ in samples[:200000]:
        by[tid].append(t)
    d = sorted(b - a for ts in by.values() for a, b in zip(ts, ts[1:]))
    return d[len(d) // 4] / 1000 if d else 1.0


TICK = tick_ms()


def cycles_per_ms():
    # CPU clock, from the most cycles a thread used per millisecond between two of its samples (a thread that ran flat out).
    last, rates = {}, []
    for t, tid, _, cyc in samples[:400000]:
        if tid in last and t > last[tid] and cyc:
            rates.append(cyc / ((t - last[tid]) / 1000))
        last[tid] = t
    rates.sort()
    return rates[int(len(rates) * 0.99)] if rates else 3e6


CPMS = cycles_per_ms()
KEY = ('Gfx Thread', 'RT64 Present', 'RT64 Workload')
# --brief: frames every thread's stack starts with, and the ones that mean the thread was only waiting.
BRIEF_SKIP = ('RtlUserThreadStart', 'BaseThreadInitThunk', 'wcsrchr', '_Invoke', '_thread_func', 'run_thread_function',
              'boot_main_thread', 'game_thread_main_init', 'game_thread_entry', 'game_main_loop_frame', 'gfx_thread_func')
BRIEF_WAITS = ('WaitForSingleObject', 'SleepConditionVariable', 'waitWithPartialSpinning', 'loading_screen_wait_frame')
# --busy: leaf frames that mean the sample caught the thread blocked.
BUSY_WAITS = ('NtWaitForSingleObject', 'NtWaitForMultipleObjects', 'ZwWaitForAlertByThreadId', 'ZwDelayExecution',
              'NtDelayExecution', 'ZwRemoveIoCompletion', 'NtRemoveIoCompletion', 'NtUserMsgWaitForMultipleObjects',
              'ZwWaitForMultipleObjects', 'NtWaitForWorkViaWorkerFactory', 'ZwWaitForWorkViaWorkerFactory',
              'NtUserGetMessage', 'ZwYieldExecution', 'SwitchToThread')
offenders = collections.Counter()  # function -> ms of hitch time spent under it (deepest dominant node per thread)


def thread_name(tid):
    return names.get(tid, 'tid %d' % tid)


def dominant(stks):
    # Follows the most common child from the root while it holds at least half of the samples.
    n = len(stks)
    path, depth, cur = [], 0, stks
    while True:
        c = collections.Counter(s[depth] for s in cur if len(s) > depth)
        if not c:
            break
        f, k = c.most_common(1)[0]
        if k < n * 0.5:
            break
        path.append((f, k))
        cur = [s for s in cur if len(s) > depth and s[depth] == f]
        depth += 1
    branches = collections.Counter(s[depth] for s in cur if len(s) > depth)
    ends = sum(1 for s in cur if len(s) <= depth)
    return path, branches, ends


if args.blind:
    last = {}  # tid -> stack id of its latest sample
    prev = None
    for t, tid, sid, cyc in samples:
        if prev is not None and t - prev > args.blind * 1000 and args.t0 * 1e6 <= prev <= args.t1 * 1e6:
            print('\n=== sampler blind at %.3f s for %.1f ms; the last stack of each thread before it:' % (prev / 1e6, (t - prev) / 1e3))
            for ltid, lsid in last.items():
                st = sym_stack(lsid)
                print('  -- %s: %s' % (thread_name(ltid), ' > '.join(f[:60] for f in st[-args.depth:])))
            last = {}
        last[tid] = sid
        prev = t
    sys.exit(0)


print('log: %s   samples %d   tick %.2f ms   cpu %.1f GHz   presents %d   game frames %d   thresholds %.0f / %.0f ms' % (
    args.log, len(samples), TICK, CPMS / 1e6, len(marks['P']), len(marks['D']), args.ms, args.game_ms))
if args.folded:
    os.makedirs(args.folded, exist_ok=True)

for hi, (a, b, kinds) in enumerate(hitches):
    lo, up = bisect.bisect_left(sample_times, a), bisect.bisect_right(sample_times, b)
    by = collections.defaultdict(list)
    for t, tid, sid, cyc in samples[lo:up]:
        by[tid].append((sid, cyc))
    kinds_s = ', '.join('%s gap %.0f ms' % (k, v) for k, v in sorted(kinds.items()))
    ev = [tag for t, tag in other_marks if a <= t <= b]
    print('%s=== hitch %d at %.3f s: %s%s' % ('' if args.brief else '\n', hi, a / 1e6, kinds_s,
                                            ('   marks: ' + ' '.join(ev)) if ev else ''))
    cpu = {tid: sum(c for _, c in ss) / CPMS for tid, ss in by.items()}
    busy = sorted(by.items(), key=lambda kv: -cpu[kv[0]])
    # The threads that used the most CPU in the hitch, plus the game's and RT64's frame threads whatever they did.
    shown = [kv for kv in busy if cpu[kv[0]] > 1][:args.threads]
    for kv in busy:
        n = thread_name(kv[0])
        if (n in KEY or (n.startswith('[Game]') and cpu[kv[0]] > 0.5)) and kv not in shown:
            shown.append(kv)
    folded = collections.Counter()
    for tid, ss in shown:
        nbusy = cpu[tid]
        stks = [sym_stack(sid) for sid, _ in ss]
        path, branches, ends = dominant(stks)
        if args.busy:
            run = [(st, c) for st, (_, c) in zip(stks, ss) if st and not any(w in st[-1] or w in ' '.join(st[-4:])
                                                                             for w in BUSY_WAITS)]
            ms = sum(c for _, c in run) / CPMS
            if ms < 2:
                continue
            path, branches, ends = dominant([st for st, _ in run])
            frames = [f for f, _ in path if not any(w in f for w in BRIEF_SKIP)]
            print('  -- %s: running %.1f ms of %.1f ms CPU (%d samples)' % (thread_name(tid), ms, nbusy, len(run)))
            print('       ' + ' > '.join(f[:70] for f in frames[-args.depth:]))
            for f, k in branches.most_common(5):
                bm = sum(c for st, c in run if len(st) > len(path) and st[len(path)] == f) / CPMS
                print('        %5.1f ms  -> %s' % (bm, f[:150]))
            continue
        if args.brief:
            n = thread_name(tid)
            if (n.startswith('[Game]') or n == 'Gfx Thread') and nbusy >= 10:
                # Below the thread's entry and the game's main loop, and not a thread that was only waiting.
                frames = [f for f, _ in path if not any(w in f for w in BRIEF_SKIP)]
                tail = [f for f, _ in branches.most_common(2)]
                if not any(w in ' '.join(frames[-3:] + tail) for w in BRIEF_WAITS):
                    print('    %s %.0f ms: %s' % (n, nbusy, ' > '.join(f[:60] for f in frames[:10] + ['-> ' + t for t in tail])))
            continue
        print('  -- %s: %d samples, %.1f ms CPU' % (thread_name(tid), len(ss), nbusy))
        shown_path = path[-args.depth:]
        if len(path) > len(shown_path):
            print('       ... %d outer frames' % (len(path) - len(shown_path)))
        for f, k in shown_path:
            print('     %3.0f%%  %s' % (100 * k / len(ss), f))
        for f, k in branches.most_common(6):
            print('        %3.0f%%  -> %s' % (100 * k / len(ss), f))
        if ends:
            print('        %3.0f%%  -> (self)' % (100 * ends / len(ss)))
        if path and nbusy:
            offenders[(thread_name(tid).split(']')[0] + ']' if thread_name(tid).startswith('[') else thread_name(tid), path[-1][0])] += nbusy * path[-1][1] / len(ss)
        for s in stks:
            folded[thread_name(tid) + ';' + ';'.join(s)] += 1
    if args.folded:
        with open(os.path.join(args.folded, 'hitch%03d_%.1fs.folded' % (hi, a / 1e6)), 'w') as f:
            for k, v in folded.items():
                f.write('%s %d\n' % (k.replace(' ', '_'), v))

print('\n=== %d hitches; CPU ms under the deepest dominant function per thread, summed over all hitches' % len(hitches))
for (th, f), ms in offenders.most_common(30):
    print('  %7.0f ms  %-20s %s' % (ms, th, f))
