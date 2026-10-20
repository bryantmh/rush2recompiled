#!/usr/bin/env python3
"""Prints the f32 (and raw word) at N64 addresses in Rush 2 or California Speed (research aid: the constants their
physics code loads).

    python tools/calspeed/rf.py r2|cs ADDR [ADDR...]
"""
import os, struct, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'rush2049'))
game = sys.argv[1]
if game == 'r2':
    import roms
    rom = roms.Rush2()
else:
    import cs
    rom = cs.CalSpeed()
for a in sys.argv[2:]:
    a = int(a, 16)
    w = struct.unpack('>I', rom.read(a, 4))[0]
    print('%08X %08X %g' % (a, w, struct.unpack('>f', struct.pack('>I', w))[0]))
