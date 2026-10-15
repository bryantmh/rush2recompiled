"""Triage of taint.py's arrays: which can't hold four (or --entries N) entries where they are.

For each array (base, stride) found by taint.py, the first address any code references at or past two entries
(base + 2 * stride) bounds the room the array has. Arrays without room for four entries (eight for online races,
docs/online.md#eight-players), and not already moved by relocate.py, are printed with that bound. With --entries,
arrays relocate.py moved are checked against their new size too.

    python triage.py [--entries N]
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
import refscan  # noqa: E402
import relocate  # noqa: E402
import taint  # noqa: E402


def main():
    entries = int(sys.argv[sys.argv.index('--entries') + 1]) if '--entries' in sys.argv else 4
    fs = taint.funcs()
    arg_taint = taint.propagate(fs)
    arrays = {}
    for n, ins in fs.items():
        for addr, op, base, stride in taint.scan(n, ins, arg_taint.get(n)):
            arrays.setdefault(base, {}).setdefault(stride, set()).add(n)
    refs = sorted({v for f, a, op, args, v, lui in refscan.scan([(0x80000000, 0x81000000)])})
    moved = [(old, end) for old, end, new, size, m in relocate.layout()]
    for base in sorted(arrays):
        if not (0x800BC000 <= base < 0x80130000):
            continue
        mv = [m for m in relocate.layout() if m[0] <= base < m[1]]
        if mv:
            for stride, users in sorted(arrays[base].items()):
                if 0 < stride <= 0x1000 and mv[0][3] - (base - mv[0][0]) < entries * stride:
                    print('%08X stride 0x%-4X moved, room for %d  %s' % (
                        base, stride, (mv[0][3] - (base - mv[0][0])) // stride, ' '.join(sorted(users))[:90]))
            continue
        for stride, users in sorted(arrays[base].items()):
            if stride <= 0 or stride > 0x1000:
                continue
            i = bisect.bisect_left(refs, base + 2 * stride)
            bound = refs[i] if i < len(refs) else 0xFFFFFFFF
            if bound < base + entries * stride:
                print('%08X stride 0x%-4X room for %d  next %08X  %s' % (base, stride, (bound - base) // stride, bound,
                                                                         ' '.join(sorted(users))[:90]))


if __name__ == '__main__':
    main()
