"""Writes the Python prototype's converted Rush 1 tracks to out/ref for main.cpp, plus Rush 2's asset 3."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'rush2049'))
import track1, roms
from r1 import Rush1

out = os.path.join(HERE, 'out', 'ref')
os.makedirs(out, exist_ok=True)
r = Rush1()
for t in range(7):
    for b in (0, 1):
        res = track1.convert(r, t, 'HAWAII', bool(b))
        if b == 0:
            for name in ('geometry', 'placement', 'pvs'):
                open(os.path.join(out, '%s%d.bin' % (name, t)), 'wb').write(res[name])
        open(os.path.join(out, 'collision%d%d.bin' % (b, t)), 'wb').write(res['collision'])
        open(os.path.join(out, 'path%d%d.bin' % (b, t)), 'wb').write(res['path'])
open(os.path.join(out, 'asset3.bin'), 'wb').write(roms.Rush2().asset(3))
