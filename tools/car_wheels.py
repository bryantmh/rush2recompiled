"""Prints where a running race puts each car's wheels (test aid for tire placement): per race car its type (physics
+0x7EA), descriptor (physics +0, as car_race_start_pose_setup_8009A264 reads it), the descriptor's wheel positions
(+0x7C, 4 x xyz, feet), the positions the wheels are drawn at (car state +0xA8 + 0xC x wheel, copied from the
descriptor at race start; carselect_preview_update_8005A598 draws them there) and the TIRE SIZE scales it draws them
with: row 0 for a computer car, physics +0x7E0 + 1 for a human (+0x7E8 == 2), of the front / rear wheel scale tables
(include/car_types.h: 73 types, 9 rows, the copies of 0x800C0A6C / 0x800C0B74).

    python tools/car_wheels.py PID [CARS]

Reads RDRAM through tools/race_watch.py's reader (read-only).
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import race_watch as rw

pid = int(sys.argv[1])
count = int(sys.argv[2]) if len(sys.argv) > 2 else 8
rd = rw.Rdram(pid)
if rd.base is None:
    sys.exit('RDRAM not found')
w = lambda a: struct.unpack('<I', rd.block(a, 4))[0]
f = lambda a: struct.unpack('<f', rd.block(a, 4))[0]
b = lambda a: struct.unpack('<B', rd.block(a ^ 3, 1))[0]
TYPES, ROWS = 73, 9
FRONT = 0x80200000 + sum(((TYPES * size * (9 if rows == 3 else 1)) + 7) & ~7 for size, rows in
                         ((4, 1), (4, 1), (4, 1), (1, 1), (4, 1), (4, 1), (4, 1), (4, 1), (4, 3)))
REAR = FRONT + ((TYPES * 4 * ROWS + 7) & ~7)
for car in range(count):
    ph = rw.CARS + car * rw.CAR_SIZE
    st = rw.STATES + car * rw.STATE_SIZE
    desc = w(ph)
    fmt = lambda xs: ' '.join('(%.2f %.2f %.2f)' % tuple(xs[i:i + 3]) for i in range(0, 12, 3))
    dw = [f(desc + 0x7C + 4 * i) for i in range(12)] if 0x80000000 <= desc < 0x80800000 else []
    sw = [f(st + 0xA8 + 4 * i) for i in range(12)]
    t = b(ph + 0x7EA)
    human = b(ph + 0x7E8) == 2
    row = b(ph + 0x7E0) + 1 if human else 0
    scale = (f(FRONT + (row * TYPES + t) * 4), f(REAR + (row * TYPES + t) * 4)) if t < TYPES and row < ROWS else (0, 0)
    print('car %d type %d desc %08X %s row %d tire size %.2f / %.2f' % (car, t, desc, 'human' if human else 'computer',
                                                                     row, scale[0], scale[1]))
    print('   descriptor ' + (fmt(dw) if dw else '-'))
    print('   drawn      ' + fmt(sw))
