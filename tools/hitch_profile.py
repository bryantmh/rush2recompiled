"""Reports the long frames in a hitch profiler log and the code each thread ran during them (test aid).

Usage: python tools/hitch_profile.py LOG [--ms 20] [--game-ms 45] [--threads N] [--depth N] [--from S] [--to S] [--folded DIR]

LOG comes from a run with RUSH2_HITCH_LOG=<file> (src/hitch_profiler.cpp, docs/hitch_profiler.md). A hitch is a gap
of more than --ms milliseconds between two RT64 presents (what a frame time overlay shows) or of more than --game-ms
between two display lists the game sent (it runs at 30 fps, so 33 ms is normal) (a game-side stall, which RT64 can hide by presenting the same picture again). For each hitch it
lists the threads that were busy and, per thread, the dominant call path: the chain of functions that most of that
thread's samples in the hitch went through, down to where it branches, then the branches. Symbols come from the PDBs
(the recompiled functions keep their names, e.g. race_setup_800A5ADC). At the end, the functions that account for the
most hitch time across the whole run. --folded writes each hitch's stacks in folded form (one file per hitch) for
flame graph viewers such as speedscope.
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
dbghelp.SymInitializeW(HPROC, None, False)
dbghelp.SymLoadModuleExW.argtypes = [wt.HANDLE, wt.HANDLE, wt.LPCWSTR, wt.LPCWSTR, ctypes.c_uint64, wt.DWORD, ctypes.c_void_p, wt.DWORD]
dbghelp.SymLoadModuleExW.restype = ctypes.c_uint64
dbghelp.SymFromAddrW.argtypes = [wt.HANDLE, ctypes.c_uint64, ctypes.POINTER(ctypes.c_uint64), ctypes.POINTER(SYMBOL_INFOW)]
for base, size, path in modules:
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
    print('\n=== hitch %d at %.3f s: %s%s' % (hi, a / 1e6, kinds_s, ('   marks: ' + ' '.join(ev)) if ev else ''))
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
