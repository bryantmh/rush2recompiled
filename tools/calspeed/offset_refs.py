#!/usr/bin/env python3
"""Lists the loads and stores of a struct offset (any base register but $sp) in California Speed's or Rush 2's code,
with the function each sits in (research aid: who writes a car field).

    python tools/calspeed/offset_refs.py cs|r2 OFFSET [--stores]
"""
import os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'rush2049'))
os.environ.setdefault('CALSPEED_ROM', os.path.join(REPO, 'build', 'calspeed.z64'))
import rabbitizer
import verify_names as vn

game = sys.argv[1]
off = int(sys.argv[2], 16)
stores_only = '--stores' in sys.argv
STORES = {0x28, 0x29, 0x2B, 0x39, 0x3D}
LOADS = {0x20, 0x21, 0x23, 0x24, 0x25, 0x31, 0x35}
for base, d in vn.segments(game):
    for i in range(0, len(d) - 3, 4):
        w = struct.unpack('>I', d[i:i + 4])[0]
        op = w >> 26
        if op not in STORES and (stores_only or op not in LOADS):
            continue
        if w & 0xFFFF != off or (w >> 21) & 0x1F == 29:
            continue
        pc = base + i
        a = pc
        while a > base:
            a -= 4
            v = vn.word(game, a)
            if v is not None and v >> 16 == 0x27BD and v & 0x8000:
                break
        print('%08X %-40s (in %08X)' % (pc, rabbitizer.Instruction(w, vram=pc).disassemble(), a))
