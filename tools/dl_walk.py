#!/usr/bin/env python3
"""Walks an F3DEX2 display list in an RDRAM snapshot and lists every list it reaches (docs/crash_reports.md).

    python tools/dl_walk.py DUMP [START ...] [--host-order] [--all] [--max N]

DUMP is RDRAM from 0x80000000: a RUSH2_RAM_DUMP file (src/ram_watch.cpp, big-endian) or a display list fault's
dl_fault_rdram.bin (--host-order: 32-bit words in host order). START is a list address (default: both frame blocks'
task lists, 0x80255900 and 0x80295900). Follows G_DL calls (DE00) and branches (DE01), returns at G_ENDDL (DF) and
prints each list reached: its address, the region's owner (the spare RDRAM map in src/controls_menu.cpp), its command
count and where it was called from. A call whose target is outside RDRAM, in boot code/data (below 0x80040000) or
not 8-aligned is flagged BAD (the game's static lists in boot data, 0x80018800 up, are fine). --all prints every command of every list.
"""
import argparse, struct, sys

# Owners of RDRAM regions (src/controls_menu.cpp's map, src/players4.cpp's frame blocks, the game's own areas).
REGIONS = [
    (0x80000000, 0x80000400, 'exception vectors'),
    (0x80000400, 0x80018800, 'BOOT CODE'),
    (0x80018800, 0x80040000, 'boot data'),
    (0x80040000, 0x80250000, 'game code/data'),
    (0x80250000, 0x80290000, 'frame block 0'),
    (0x80290000, 0x802D0000, 'frame block 1'),
    (0x802D0000, 0x80400000, 'game data'),
    (0x80400000, 0x80420000, 'scene nodes'),
    (0x80420000, 0x80AA0000, 'game heap'),
    (0x80AA0000, 0x80ACA000, 'menus2049 art'),
    (0x80ACA000, 0x80AE0000, 'world polygons'),
    (0x80AE0000, 0x80B00000, 'hud 2D lists'),
    (0x80B00000, 0x80C00000, 'interpolation side lists'),
    (0x80C00000, 0x80C80000, 'controls_menu glyph images'),
    (0x80C80000, 0x80C90000, 'controls_menu glyph lists'),
    (0x80C90000, 0x80C99E00, 'track1_audio'),
    (0x80C9A000, 0x80C9AB00, 'odometer'),
    (0x80C9C000, 0x80C9EF00, 'pause_menu'),
    (0x80C9F000, 0x80C9F320, 'hud'),
    (0x80CA0000, 0x80CB0000, 'car2049'),
    (0x80CB0000, 0x80CEF448, 'ghost'),
    (0x80CF0000, 0x80D00000, 'widescreen'),
    (0x80D00000, 0x80E10000, 'wings_render'),
    (0x80E10000, 0x80E16000, 'online_game'),
    (0x80E1E000, 0x80E1EA00, 'arcade_checkpoints'),
    (0x80E1EA00, 0x80E1F200, 'collision leaf lists'),
    (0x80E20000, 0x80F18000, 'battle_render'),
    (0x80F18000, 0x80F20000, 'stunt2049'),
    (0x80F20000, 0x80F80000, 'car2049 lighting'),
]


def owner(a):
    for lo, hi, name in REGIONS:
        if lo <= a < hi:
            return name
    return 'unmapped'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('dump')
    ap.add_argument('start', nargs='*')
    ap.add_argument('--host-order', action='store_true')
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--max', type=int, default=200000, help='commands to walk at most')
    a = ap.parse_args()
    data = open(a.dump, 'rb').read()
    fmt = '<I' if a.host_order else '>I'

    def word(addr):
        o = (addr & 0x00FFFFFF)
        return struct.unpack_from(fmt, data, o)[0] if o + 4 <= len(data) else None

    starts = [int(s, 16) for s in a.start] or [0x80255900, 0x80295900]
    for start in starts:
        print(f'== task list {start:08X} ({owner(start)})')
        stack, lists, pc, n = [], {}, start, 0
        cur = start
        lists[cur] = [0, None]
        while n < a.max:
            w0, w1 = word(pc), word(pc + 4)
            if w0 is None:
                print(f'  ran off the dump at {pc:08X}')
                break
            n += 1
            lists[cur][0] += 1
            op = w0 >> 24
            if a.all:
                print(f'    {pc:08X}: {w0:08X} {w1:08X}')
            if op == 0xDE:
                target = 0x80000000 | (w1 & 0x00FFFFFF)
                bad = (w1 & 7) != 0 or target < 0x80018800 or target >= 0x81000000 or (w1 >> 24) not in (0x00, 0x80)
                if bad:
                    print(f'  BAD G_DL at {pc:08X} ({owner(pc)}) -> {w1:08X} ({owner(target)})')
                    break
                if (w0 >> 16) & 0xFF == 0:
                    stack.append((pc + 8, cur))
                if target not in lists:
                    lists[target] = [0, pc]
                cur, pc = target, target
                continue
            if op == 0xDF:
                if not stack:
                    break
                pc, cur = stack.pop()
                continue
            if op == 0xE9 and not stack and a.all:
                print(f'    FULLSYNC at {pc:08X}')
            pc += 8
        for addr, (count, called_from) in lists.items():
            frm = f' from {called_from:08X} ({owner(called_from)})' if called_from is not None else ''
            print(f'  {addr:08X}  {owner(addr):28s} {count:6d} cmds{frm}')


if __name__ == '__main__':
    main()
