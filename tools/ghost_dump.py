"""Prints a recorded ghost's run (build/saves/ghosts/*.ghost, src/rush2049/ghost_logic.cpp save()): its header, then
per second the car's speed (physics struct +0x3D4, ft/s, from the once-a-second keyframes), the distance covered so
far, and how much of that second the throttle and brake were down. Compares a player's pace with the AI's (e.g. snow
target speeds against what a player really drives).

    python tools/ghost_dump.py build/saves/ghosts/mod-golden_gate_snow_forward_3lap_BRYANT_1.ghost [--every 5]
"""
import argparse, struct

ap = argparse.ArgumentParser()
ap.add_argument('ghost')
ap.add_argument('--every', type=int, default=1, help='print every n-th second')
a = ap.parse_args()

data = open(a.ghost, 'rb').read()
assert data[:8] == b'R2GHOST3', 'not a ghost file'
o = 8
state, track, backward, mirror, laps, car_type = struct.unpack_from('<bBBBBB', data, o)
name = data[o + 15:o + 31].split(b'\0')[0].decode(errors='replace')
step, race_time, position, count = struct.unpack_from('<ffII', data, o + 32)
o += 48
physics_time, game_time = struct.unpack_from('<ff', data, o)
o += 8


def vector(size):
    global o
    n = struct.unpack_from('<I', data, o)[0]
    o += 4
    start = o
    o += n * size
    return n, start


n_inputs, inputs_at = vector(36)        # Input: steer, clutch, throttle, brake, gear(h) wings(B) pad, stick x/y, clocks
vector(4)                               # patch_start
vector(8)                               # patches
vector(4)                               # start
vector(4)                               # descriptor
n_keys = struct.unpack_from('<I', data, o)[0]
o += 4
keys = []
for _ in range(n_keys):
    n, at = vector(4)
    keys.append((n, at))

print('%s: track %d%s, %d laps, car type %d, %s, race %.2f s, %d ticks, %d keyframes' %
      (a.ghost, track, ' backward' if backward else '', laps, car_type, name, race_time, n_inputs, n_keys))

speed_off = 0x3D4 // 4
dist = 0.0
prev_v = None
speeds = []
for s, (n, at) in enumerate(keys):
    v = struct.unpack_from('<f', data, at + speed_off * 4)[0] if n > speed_off else float('nan')
    if prev_v is not None:
        dist += (prev_v + v) / 2
    prev_v = v
    speeds.append(v / 1.4667)
    t0, t1 = s * 60, min((s + 1) * 60, n_inputs)
    thr = brk = 0.0
    for i in range(t0, t1):
        # The recording keeps them in the car's order (+0x730 brake, +0x734 throttle).
        _, _, brake, throttle = struct.unpack_from('<ffff', data, inputs_at + i * 36)
        thr += throttle
        brk += brake
    k = max(t1 - t0, 1)
    if s % a.every == 0:
        print('%4d s  %5.0f mph  %7.0f ft  throttle %3.0f%%  brake %3.0f%%' % (s, v / 1.4667, dist, 100 * thr / k, 100 * brk / k))
if speeds:
    print('mean %.0f mph, top %.0f mph, %.0f ft over %d s' % (sum(speeds) / len(speeds), max(speeds), dist, len(speeds)))
