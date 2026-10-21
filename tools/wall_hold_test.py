#!/usr/bin/env python3
"""Drives the player's car into a wall polygon and reports whether the wall holds it (test aid).

    python tools/wall_hold_test.py COLLISION_BIN --pid PID|PID_FILE --wall N[,N...] [--angle DEG] [--speed FT_S] [--back FT]
                                   [--secs S] [--after S] [--out FILE]

Attaches to a running race (tools/test_race.py, any track; COLLISION_BIN is that track's collision.bin, a converter
output such as tools/track_mod_test.bat's) and, for each wall polygon N in turn: puts car 0 on the road --back ft in
front of the middle of the wall's face (the highest floor under that spot from 3 ft under the wall's foot to its top), turned
--angle degrees into the wall from running along it, at --speed ft/s forward (car frame +0x3C), and samples for --secs
the signed distance of each body point (+0x260) and wheel contact point (+0x230) from the wall's plane (positive in
front) while it lies across the wall's face (or over its top). The wall held if no point got more than 2 ft behind its face (or over its top) while across it. Writes RDRAM (position +0x224 and the
car state copy +0, matrix +0x2CC, velocity +0x34), like tmp/cs_tunnel's tests; the race's other cars are untouched.
"""
import argparse, ctypes, json, math, os, struct, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
import race_watch as rw  # noqa: E402
import collision as C  # noqa: E402

CAR, STATE = 0x800F5470, 0x801124A0
k32 = ctypes.WinDLL('kernel32', use_last_error=True)


def poke_f32(rd, vaddr, value):
    buf, done = ctypes.c_float(value), ctypes.c_size_t()
    return k32.WriteProcessMemory(rd.h, ctypes.c_void_p(rd.base + (vaddr - 0x80000000)), ctypes.byref(buf), 4,
                                  ctypes.byref(done))


def floor_under(c, x, z, top, bottom=-1e9):
    """The highest floor (type <= 2, facing up) whose footprint holds (x, z), from bottom up to top."""
    best = None
    for p in c.polys:
        n = [v / 16384 for v in p['matrix'][3:6]]
        if (p['flags'] & 0xF) > 2 or n[1] < 0.7:
            continue
        w = C.world_polygon(c, p)
        s = [(w[(i + 1) % len(w)][0] - w[i][0]) * (z - w[i][2]) - (w[(i + 1) % len(w)][2] - w[i][2]) * (x - w[i][0])
             for i in range(len(w))]
        if not (min(s) >= 0 or max(s) <= 0):
            continue
        y = w[0][1] - ((x - w[0][0]) * n[0] + (z - w[0][2]) * n[2]) / n[1]
        if bottom <= y <= top and (best is None or y > best):
            best = y
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('collision')
    ap.add_argument('--pid', required=True, help="the game's process id, or a file holding it (test_race.py --out DIR: DIR/pid.txt)")
    ap.add_argument('--wall', required=True)
    ap.add_argument('--angle', type=float, default=25.0)
    ap.add_argument('--speed', type=float, default=70.0)
    ap.add_argument('--back', type=float, default=8.0)
    ap.add_argument('--secs', type=float, default=1.5)
    ap.add_argument('--after', type=float, default=2.0, help='seconds of racing before the first drop')
    ap.add_argument('--out')
    a = ap.parse_args()
    c = C.Collision.parse(open(a.collision, 'rb').read(), C.R2)
    if not a.pid.isdigit():
        t = time.time()
        while not os.path.exists(a.pid) and time.time() - t < 30:
            time.sleep(0.5)
        a.pid = open(a.pid).read().strip()
    rd, t = None, time.time()
    while time.time() - t < 30:      # the game maps RDRAM a few seconds after it starts
        rd = rw.Rdram(int(a.pid), write=True)
        if rd.base:
            break
        time.sleep(1)
    if not rd.base:
        raise SystemExit('no RDRAM in process %s' % a.pid)
    out = open(a.out, 'w') if a.out else None

    def read():
        blk = rd.block(CAR, 0x81C)
        g = rd.block(0x8010C0D0, 4)
        if not (blk and g):
            return None
        f = lambda o: struct.unpack_from('<f', blk, o)[0]
        return dict(state=struct.unpack('<i', g)[0], pos=[f(0x224 + 4 * k) for k in range(3)],
                    body=[[f(0x260 + 12 * i + 4 * k) for k in range(3)] for i in range(4)],
                    wheel=[[f(0x230 + 12 * i + 4 * k) for k in range(3)] for i in range(4)],
                    types=[struct.unpack_from('<i', blk, 0x5FC + 4 * i)[0] for i in range(4)])

    t0 = None
    while True:
        d = read()
        if d and d['state'] == 3:
            t0 = t0 or time.time()
            if time.time() - t0 >= a.after:
                break
        time.sleep(0.1)
    results = []
    for wi in [int(v) for v in a.wall.split(',')]:
        p = c.polys[wi]
        w = C.world_polygon(c, p)
        n = [v / 16384 for v in p['matrix'][3:6]]
        h = math.hypot(n[0], n[2])
        nh = (n[0] / h, n[2] / h)                         # the face's horizontal normal
        along = (-nh[1], nh[0])
        mid = [sum(v[k] for v in w) / len(w) for k in range(3)]
        top = max(v[1] for v in w)
        x, z = mid[0] + nh[0] * a.back, mid[2] + nh[1] * a.back
        fy = floor_under(c, x, z, top, min(v[1] for v in w) - 3)
        if fy is None:
            print('wall %d: no floor %.0f ft in front of it' % (wi, a.back))
            continue
        ang = math.radians(a.angle)
        fwd = (along[0] * math.cos(ang) - nh[0] * math.sin(ang), along[1] * math.cos(ang) - nh[1] * math.sin(ang))
        rows = [(fwd[1], 0.0, -fwd[0]), (0.0, 1.0, 0.0), (fwd[0], 0.0, fwd[1])]
        for _ in range(3):      # a few times over a few steps, so the physics step can't put back an old position
            for k, v in enumerate((x, fy + 1.2, z)):
                poke_f32(rd, CAR + 0x224 + 4 * k, v)
                poke_f32(rd, STATE + 4 * k, v)
            for r in range(3):
                for k in range(3):
                    poke_f32(rd, CAR + 0x2CC + 12 * r + 4 * k, rows[r][k])
            for k, v in enumerate((0.0, 0.0, a.speed)):
                poke_f32(rd, CAR + 0x34 + 4 * k, v)
            time.sleep(0.03)
        dist = lambda q: (q[0] - w[0][0]) * n[0] + (q[1] - w[0][1]) * n[1] + (q[2] - w[0][2]) * n[2]

        def across(q):
            # q lies across the wall's face: its projection along the normal is inside the polygon (the wall's own
            # extent, not the plane's), or above its top (passed over it)
            s = []
            for i in range(len(w)):
                e = [w[(i + 1) % len(w)][k] - w[i][k] for k in range(3)]
                r = [q[k] - w[i][k] for k in range(3)]
                cr = [e[1] * r[2] - e[2] * r[1], e[2] * r[0] - e[0] * r[2], e[0] * r[1] - e[1] * r[0]]
                if abs(e[1]) < 0.5 * math.hypot(e[0], e[2]) and (w[i][1] + w[(i + 1) % len(w)][1]) / 2 >= top - 0.5:
                    continue        # the top edge: points over the wall count too
                s.append(cr[0] * n[0] + cr[1] * n[1] + cr[2] * n[2])
            return min(s) >= 0 or max(s) <= 0
        low_body = low_wheel = 99.0
        wall_types = 0
        end = None
        t1 = time.time()
        while time.time() - t1 < a.secs:
            d = read()
            if d:
                db = min([dist(q) for q in d['body'] if across(q)] or [99.0])
                dw = min([dist(q) for q in d['wheel'] if across(q)] or [99.0])
                low_body, low_wheel = min(low_body, db), min(low_wheel, dw)
                wall_types += sum(1 for t in d['types'] if t in (5, 6))
                if across(d['pos']) or end is not None:
                    end = dist(d['pos'])
                if out:
                    out.write(json.dumps(dict(wall=wi, t=round(time.time() - t1, 3), body=round(db, 2),
                                              wheel=round(dw, 2), types=d['types'], pos=[round(v, 2) for v in d['pos']],
                                              wheels=[[round(v, 2) for v in q] + [round(dist(q), 2), across(q)]
                                                      for q in d['wheel']])) + '\n')
            time.sleep(0.02)
        held = min(low_body, low_wheel) > -2.0
        results.append(held)
        ys = [v[1] for v in w]
        print('wall %d (y %.1f..%.1f, floor %.1f): %s; deepest past the plane: body %.2f ft, wheels %.2f ft; car '
              'center at the end %s; wheel samples on a wall: %d' % (wi, min(ys), max(ys), fy, 'HELD' if held else 'PASSED THROUGH',
              -low_body, -low_wheel, 'never across it' if end is None else '%.1f ft %s' % (abs(end), 'in front' if end > 0 else 'behind'),
              wall_types), flush=True)
    if out:
        out.close()
    print('%d of %d walls held' % (sum(results), len(results)))


if __name__ == '__main__':
    main()
