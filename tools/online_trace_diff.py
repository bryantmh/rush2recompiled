"""Compares two RUSH2_ONLINE_TRACE recordings (src/online/online_game.cpp, docs/online.md#test-aids): the cars'
physics structs before each lockstep frame. Prints the first frame where they differ and, for it and the next few,
which car and field offsets changed.

    python tools/online_trace_diff.py A/cars.bin B/cars.bin [--frames N]
"""
import sys

CAR = 0x81C
REC = 8 * CAR


def main():
    a = open(sys.argv[1], 'rb').read()
    b = open(sys.argv[2], 'rb').read()
    frames = 3
    if '--frames' in sys.argv:
        frames = int(sys.argv[sys.argv.index('--frames') + 1])
    n = min(len(a), len(b)) // REC
    first = None
    for f in range(n):
        if a[f * REC:(f + 1) * REC] != b[f * REC:(f + 1) * REC]:
            first = f
            break
    if first is None:
        print('identical for %d frames' % n)
        return
    print('first difference at frame %d (of %d)' % (first, n))
    for f in range(first, min(n, first + frames)):
        ra, rb = a[f * REC:(f + 1) * REC], b[f * REC:(f + 1) * REC]
        diffs = [i for i in range(REC) if ra[i] != rb[i]]
        by_car = {}
        for i in diffs:
            by_car.setdefault(i // CAR, []).append(i % CAR)
        print('frame %d:' % f)
        for car, offs in sorted(by_car.items()):
            words = sorted({o & ~3 for o in offs})
            print('  car %d: %d bytes, words %s' % (car, len(offs), ' '.join('%03X' % w for w in words[:24])))


if __name__ == '__main__':
    main()
