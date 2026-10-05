"""For arrays found by taint.py: the index multiplier at each site and the room before the next referenced address.

    python extent.py BASE [BASE ...]      (hex)
Prints each array's tainted sites with the shift/multiply applied to the index just before (guessed stride), and the
next address above the array's base that any code references (refscan), i.e. how far four entries can grow in place.
"""
import bisect
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
import refscan  # noqa: E402
import taint  # noqa: E402


def main():
    bases = [int(x, 16) for x in sys.argv[1:]]
    allrefs = refscan.scan([(0x80000000, 0x81000000)])
    addrs = sorted({v for f, a, op, args, v, lui in allrefs})
    fs = taint.funcs()
    arg_taint = taint.propagate(fs)
    sites = {b: [] for b in bases}
    for name, ins in fs.items():
        found = taint.scan(name, ins, arg_taint.get(name))
        for addr, op, base, stride in found:
            for b in bases:
                if base & 0xFFFFFFFF == b:
                    # Look back for shifts of any register near the site.
                    k = next(i for i, x in enumerate(ins) if x[0] == addr)
                    shifts = [x[1] + ' ' + x[2] for x in ins[max(0, k - 12):k] if x[1] in ('sll', 'mult', 'multu', 'subu')]
                    sites[b].append((name, addr, op, shifts[-3:]))
    for b in bases:
        i = bisect.bisect_right(addrs, b)
        nxt = addrs[i] if i < len(addrs) else None
        print('%08X  next referenced %s (+0x%X)' % (b, '%08X' % nxt if nxt else '-', (nxt - b) if nxt else 0))
        for s in sites[b][:6]:
            print('    %s %s %s   %s' % s)


if __name__ == '__main__':
    main()
