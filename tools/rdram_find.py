#!/usr/bin/env python3
"""Finds bytes in a running game's RDRAM (read-only, through race_watch.py's Rdram): every game address where a byte
pattern sits, as the game sees memory (big-endian), and optionally the bytes at given addresses. A test aid for
"where did the game put this asset" questions (a texture's texels, a model, a table).

    python tools/rdram_find.py PID [--hex BYTES] [--file PATH --offset HEX [--length N]] [--dump ADDR[:N] ...]
                               [--size MB]

--hex gives the pattern as hex digits; --file/--offset/--length take it from a file (an asset as
tools/rush2049/roms.py reads it, for instance), 32 bytes by default. --dump prints N (default 32) bytes at a game
address. --size: how much RDRAM to search (default 16 MB from 0x80000000, the game's own memory; the recomp's
allocations above it are searched with a larger size).
"""
import argparse, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from race_watch import Rdram  # noqa: E402


def game_bytes(rd, vaddr, n):
    """n bytes at game address vaddr, in the game's (big-endian) order: RDRAM holds words native-endian."""
    start = vaddr & ~3
    raw = rd.block(start, ((vaddr + n + 3) & ~3) - start)
    if raw is None:
        return None
    swapped = bytearray()
    for i in range(0, len(raw), 4):
        swapped += raw[i:i + 4][::-1]
    return bytes(swapped[vaddr - start:vaddr - start + n])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('pid', type=int)
    ap.add_argument('--hex', help='the pattern, hex digits')
    ap.add_argument('--file', help='take the pattern from this file')
    ap.add_argument('--offset', type=lambda v: int(v, 16), default=0, help='its offset in --file (hex)')
    ap.add_argument('--length', type=int, default=32, help='its length (default 32)')
    ap.add_argument('--dump', action='append', default=[], help='ADDR[:N]: print N bytes at this game address (hex)')
    ap.add_argument('--size', type=int, default=16, help='MB of RDRAM to search (default 16)')
    a = ap.parse_args()

    rd = Rdram(a.pid)
    if rd.base is None:
        sys.exit('no RDRAM found in process %d' % a.pid)
    for d in a.dump:
        addr, _, n = d.partition(':')
        addr, n = int(addr, 16), int(n or 32)
        b = game_bytes(rd, addr, n)
        print('%08X: %s' % (addr, b.hex() if b else 'unreadable'))

    pattern = None
    if a.hex:
        pattern = bytes.fromhex(a.hex)
    elif a.file:
        with open(a.file, 'rb') as f:
            f.seek(a.offset)
            pattern = f.read(a.length)
    if not pattern:
        return
    found = 0
    chunk = 1 << 20
    for start in range(0x80000000, 0x80000000 + (a.size << 20), chunk):
        b = game_bytes(rd, start, chunk + len(pattern) - 1)
        if b is None:
            continue
        i = b.find(pattern)
        while i >= 0 and i < chunk:
            print('%08X' % (start + i))
            found += 1
            i = b.find(pattern, i + 1)
    print('%d match(es) of %d bytes' % (found, len(pattern)))


if __name__ == '__main__':
    main()
