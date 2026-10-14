"""Diffs RDRAM dumps written by the game's RUSH2_RAM_DUMP test aid (src/ram_watch.cpp, docs/online.md#test-aids).

    python tools/ram_diff.py A.bin B.bin [START-END ...] [--max N] [--gap N]

Prints each run of changed bytes (runs closer than --gap bytes, default 4, are merged) with its address, the old and
new bytes, and the nearest Rush 2 global at or below it from syms/names/rush2_data.toml. Optional hex ranges
(800C2140-800C2280) limit the diff. Dumps cover 0x80000000-0x80800000 in N64 byte order.
"""
import argparse
import os
import re
import bisect

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
BASE = 0x80000000


def load_names():
    names = []
    path = os.path.join(REPO, 'syms', 'names', 'rush2_data.toml')
    vram = None
    for line in open(path, encoding='utf-8'):
        m = re.match(r'vram = (0x[0-9A-Fa-f]+)', line)
        if m:
            vram = int(m.group(1), 16)
            continue
        m = re.match(r'name = "([^"]+)"', line)
        if m and vram is not None:
            names.append((vram, m.group(1)))
            vram = None
    names.sort()
    return names


def label(names, keys, addr):
    i = bisect.bisect_right(keys, addr) - 1
    if i < 0:
        return ''
    vram, name = names[i]
    return name if vram == addr else '%s+0x%X' % (name, addr - vram)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('a')
    ap.add_argument('b')
    ap.add_argument('ranges', nargs='*')
    ap.add_argument('--max', type=int, default=400, help='most runs to print')
    ap.add_argument('--gap', type=int, default=4, help='merge runs closer than this')
    args = ap.parse_args()
    a = open(args.a, 'rb').read()
    b = open(args.b, 'rb').read()
    ranges = []
    for r in args.ranges:
        lo, hi = r.split('-')
        ranges.append((int(lo, 16) - BASE, int(hi, 16) - BASE))
    if not ranges:
        ranges = [(0, min(len(a), len(b)))]
    names = load_names()
    keys = [v for v, _ in names]
    printed = 0
    for lo, hi in ranges:
        i = lo
        while i < hi and printed < args.max:
            if a[i] == b[i]:
                i += 1
                continue
            start = i
            end = i + 1
            while end < hi:
                if a[end] != b[end]:
                    end += 1
                    continue
                look = end
                while look < min(hi, end + args.gap) and a[look] == b[look]:
                    look += 1
                if look < hi and look < end + args.gap:
                    end = look
                    continue
                break
            n = end - start
            show = min(n, 16)
            print('%08X +%-4X %-40s %s -> %s%s' % (BASE + start, n, label(names, keys, BASE + start),
                  a[start:start + show].hex(), b[start:start + show].hex(), ' ...' if n > show else ''))
            printed += 1
            i = end


if __name__ == '__main__':
    main()
