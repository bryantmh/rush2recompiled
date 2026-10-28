#!/usr/bin/env python3
"""Lists California Speed's animated track textures (palette cycles and scrolls) per track (research aid,
docs/calspeed_research.md "Animated textures").

    python tools/calspeed/texanim.py

Reads the per-track list table at 0x800BF86C (indexed by the track id at 0x800E779C) that the update at 0x800B989C
runs each frame: 0x14-byte records, terminated by a null name pointer.
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cs

TABLE = 0x800BF86C
NAMES = ['MONTEREY', 'SILICON', 'HWY1', 'SF', 'CENTRAL', 'SC', 'SD', 'CLOVER', 'LA', 'SHASTA', 'YOSEMITE', 'MOJAVE',
         'FUJI', 'OVAL', 'CALI', 'SA']
# Record kind (+9), 0x800B989C's jump table 0x800DC218: 0-9 palette (rotate, blink, swap, traffic light), 10-13 move
# the tile origin by the step, wrapping at the limit.
KINDS = {0: 'rotate up', 1: 'nothing', 2: 'rotate down', 3: 'blink', 4: 'blink', 5: 'swap', 6: 'rotate 3',
         10: 'scroll s -', 11: 'scroll t +', 12: 'scroll t -', 13: 'scroll s +'}


def cstr(c, a):
    b = b''
    while len(b) < 40:
        ch = c.read(a + len(b), 1)
        if ch == b'\0':
            break
        b += ch
    return b.decode('latin1')


def main():
    c = cs.CalSpeed()
    for t, prefix in enumerate(NAMES):
        p = c.w(TABLE + 4 * t)
        print('%2d %s' % (t, prefix))
        while p and c.w(p):
            # +0 texture name, +4 s16 first/position, +6 s16 last/limit, +8 s8 step, +9 kind, +A s8 period (ticks)
            name = cstr(c, c.w(p))
            first, last, step, kind, period = struct.unpack('>hhbbb', c.read(p + 4, 7))
            print('     %-10s kind %2d %-10s first %4d last %4d step %3d period %d'
                  % (name, kind, KINDS.get(kind, 'palette'), first, last, step, period))
            p += 0x14


if __name__ == '__main__':
    main()
