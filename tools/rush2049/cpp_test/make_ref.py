"""Writes the Python reference for static_paths=False to cpp_test/out/ref_nostatic/track1..6 (slot 2, HAWAII) and stunt1..4 (slot 11, STUNT1).
The static_paths=True reference is tools/rush2049/out (python track.py)."""
import os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
os.chdir(os.path.dirname(HERE))

import track

for k in range(1, 7):
    track.build(k, 2, os.path.join(HERE, 'out', 'ref_nostatic', 'track%d' % k), static_paths=False)
for n in range(1, 5):
    track.build(track.STUNT_FIRST + n - 1, 11, os.path.join(HERE, 'out', 'ref_nostatic', 'stunt%d' % n),
                static_paths=False)
