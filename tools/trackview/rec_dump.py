"""Prints a race recording (src/race_record.cpp, tools/trackview runs) as text: the header and, every N seconds of
the physics clock, the game clock and each car's position, speed, lap, next checkpoint, place and state.

    python tools/trackview/rec_dump.py RECORDING.r2rc [--every SECONDS] [--cars 0,1,...]

See: docs/custom_tracks.md#race-replays
"""
import argparse, json, struct


def load(path):
    d = open(path, 'rb').read()
    assert d[:4] == b'R2RC', 'not a race recording'
    hl = struct.unpack_from('<I', d, 8)[0]
    h = json.loads(d[12:12 + hl])
    stride = 2 + h['cars'] * len(h['fields'])
    body = d[12 + hl:]
    ticks = len(body) // (stride * 4)
    rows = [struct.unpack_from('<%df' % stride, body, t * stride * 4) for t in range(ticks)]
    return h, rows


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('path')
    ap.add_argument('--every', type=float, default=5)
    ap.add_argument('--cars')
    a = ap.parse_args()
    h, rows = load(a.path)
    F = h['fields']; nf = len(F)
    cars = [int(c) for c in a.cars.split(',')] if a.cars else range(h['cars'])
    print(json.dumps(h))
    print('%d ticks' % len(rows))
    next_t = -1
    for r in rows:
        t = r[1] - rows[0][1]
        if t < next_t and r is not rows[-1]:
            continue
        next_t = t + a.every
        print('phys %7.2f  game %7.2f' % (t, r[0]))
        for c in cars:
            f = dict(zip(F, r[2 + c * nf:2 + (c + 1) * nf]))
            print('   car %d  (%7.1f %6.1f %7.1f)  %5.1f mph  lap %d cp %d  place %d%s%s%s' % (
                c, f['x'], f['y'], f['z'], abs(f['speed']) * 60 / 88, f['laps'], f['next_cp'], f['place'] + 1,
                '  finished' if f['finished'] else '', '  wreck %d' % f['wreck'] if f['wreck'] else '',
                '  respawn' if f['respawn'] >= 0 else ''))


if __name__ == '__main__':
    main()
