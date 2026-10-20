#!/usr/bin/env python3
"""Lists the jal call sites of functions in California Speed's (or Rush 2's) code and the function each sits in (the
nearest preceding stack-frame prologue), research aid.

    python tools/calspeed/callers.py cs|r2 ADDR [ADDR...]
"""
import os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'rush2049'))
os.environ.setdefault('CALSPEED_ROM', os.path.join(REPO, 'build', 'calspeed.z64'))
import verify_names as vn

game = sys.argv[1]
targets = {int(a, 16) for a in sys.argv[2:]}
for base, d in vn.segments(game):
    for i in range(0, len(d) - 3, 4):
        w = struct.unpack('>I', d[i:i + 4])[0]
        if w >> 26 != 3:
            continue
        pc = base + i
        t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        if t not in targets:
            continue
        a = pc
        while a > base:
            a -= 4
            v = vn.word(game, a)
            if v is not None and v >> 16 == 0x27BD and v & 0x8000:
                break
        print('%08X calls %08X  (in %08X)' % (pc, t, a))
