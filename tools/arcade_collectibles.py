"""Which N64 coins (Rush 2049) and keys (SF Rush) have the same spot on the arcade versions of their courses, written as
the C++ table src/arcade2049/arcade_collectibles.inc (docs/arcade2049.md#coins-and-keys-on-the-arcade-versions).

    python -I tools/arcade_collectibles.py [--write] [--verbose]

The arcade tracks are in the N64 games' world frame (feet, y up), so a pickup keeps its N64 position. It is kept when
the arcade track has the same place around it:
  - ground: the highest floor (not a wall) under the pickup, within 2 ft above it, is within 3 ft of the N64's;
  - surroundings: at 8 points 30 ft around it, the arcade's floor matches the N64's (both within 3 ft, or both none) at
    6 or more;
  - walls: no arcade wall comes within 4 ft of it between its floor and its height where the N64 has none.
Reads RUSH2049_ROM and RUSH1_ROM (default build/rush2049.z64, build/rush1.z64) and ProjectR's arcade data
(%LOCALAPPDATA%/ProjectR/Data/sf2049se and sfrushrk, or ARCADE2049_DATA / SFRUSH_ARCADE_DATA). Prints a line per
pickup; --write writes the table.
"""
import math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
for sub in ('rush2049', 'arcade2049', 'rush1'):
    sys.path.insert(0, os.path.join(HERE, sub))
os.environ.setdefault('RUSH2049_ROM', os.path.join(ROOT, 'build', 'rush2049.z64'))
os.environ.setdefault('RUSH1_ROM', os.path.join(ROOT, 'build', 'rush1.z64'))

import arcade, collision, track1
from roms import Rush2049
from r1 import Rush1

OUT = os.path.join(ROOT, 'src', 'arcade2049', 'arcade_collectibles.inc')
GROUND_TOLERANCE = 3.0
RING_RADIUS, RING_POINTS, RING_NEEDED = 30.0, 8, 6
WALL_REACH = 4.0
PROJECTR = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'ProjectR', 'Data')

# (mod id, mod track folder, arcade TRACKS folder, N64 track index). Courses are collectibles.cpp's: SF Rush 0-6, then
# Rush 2049 race tracks 7-12. Matches include/arcade2049.h's arcade_tracks / sfrush_tracks.
RUSH2049 = [('marina_arcade', 'TRACK1'), ('haight_arcade', 'TRACK2'), ('civic_arcade', 'TRACK3'),
            ('metro_arcade', 'TRACK5'), ('mission_arcade', 'TRACK6'), ('presidio_arcade', 'TRACK7')]
SFRUSH = [('golden_gate_arcade', 'TRACK1'), ('embarcadero_arcade', 'TRACK2'), ('market_arcade', 'TRACK3'),
          ('downtown_arcade', 'TRACK5'), ('the_heights_arcade', 'TRACK6'), ('sunset_arcade', 'TRACK7'),
          ('the_rock_arcade', 'TRACK4')]


# ---- surfaces: (flags, [world xyz], normal) polygons -> floors and walls with their x/z boxes ----

def split(polys):
    floors, walls = [], []
    for fl, pts, n in polys:
        if len(pts) < 3:
            continue
        xs = [p[0] for p in pts]; zs = [p[2] for p in pts]; ys = [p[1] for p in pts]
        box = (min(xs), max(xs), min(zs), max(zs), min(ys), max(ys))
        if (fl & 0xF) in collision.TYPE_WALL or abs(n[1]) < 0.3:
            walls.append((box, pts, n))
        else:
            floors.append((box, pts, n))
    return floors, walls


def inside(px, pz, pts):
    c = False
    for i in range(len(pts)):
        x1, z1 = pts[i][0], pts[i][2]
        x2, z2 = pts[(i + 1) % len(pts)][0], pts[(i + 1) % len(pts)][2]
        if (z1 > pz) != (z2 > pz) and px < (x2 - x1) * (pz - z1) / (z2 - z1) + x1:
            c = not c
    return c


def floor_under(floors, x, z, top):
    """The highest floor at (x, z) no higher than top, or None."""
    best = None
    for (x0, x1, z0, z1, _, _), pts, n in floors:
        if x0 <= x <= x1 and z0 <= z <= z1 and inside(x, z, pts):
            o = pts[0]
            y = o[1] - (n[0] * (x - o[0]) + n[2] * (z - o[2])) / n[1]
            if y <= top and (best is None or y > best):
                best = y
    return best


def wall_near(walls, p, low):
    """Whether a wall polygon comes within WALL_REACH of the vertical segment from (p.x, low, p.z) to p."""
    for (x0, x1, z0, z1, y0, y1), pts, n in walls:
        if x1 < p[0] - WALL_REACH or x0 > p[0] + WALL_REACH or z1 < p[2] - WALL_REACH or z0 > p[2] + WALL_REACH:
            continue
        if y1 < low or y0 > p[1]:
            continue
        # Distance from the segment's x/z point to the polygon's plane, and the point projected inside the polygon.
        o = pts[0]
        d = n[0] * (p[0] - o[0]) + n[2] * (p[2] - o[2])   # the segment is vertical: the plane's x/z part
        if abs(d) > WALL_REACH:
            continue
        q = (p[0] - n[0] * d, max(low, min(p[1], (y0 + y1) / 2)), p[2] - n[2] * d)
        # Inside test in the wall's own plane: drop the axis the normal is largest on.
        ax = max(range(3), key=lambda a: abs(n[a]))
        u, v = [a for a in range(3) if a != ax]
        c = False
        for i in range(len(pts)):
            a, b = pts[i], pts[(i + 1) % len(pts)]
            if (a[v] > q[v]) != (b[v] > q[v]) and q[u] < (b[u] - a[u]) * (q[v] - a[v]) / (b[v] - a[v]) + a[u]:
                c = not c
        if c:
            return True
    return False


def normal_of(pts):
    a, b, c = pts[0], pts[1], pts[2]
    u = [b[i] - a[i] for i in range(3)]; v = [c[i] - a[i] for i in range(3)]
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    L = math.hypot(*n) or 1.0
    n = tuple(x / L for x in n)
    return n if n[1] >= 0 else tuple(-x for x in n)


def n64_2049_polys(rom, k):
    c = collision.Collision.parse(rom.file(139 + k), collision.R49)
    out = []
    for p in c.polys:
        pts = collision.world_polygon(c, p)
        if len(pts) >= 3:
            out.append((p['flags'], pts, normal_of(pts)))
    return out


def n64_rush1_polys(r, t):
    c = collision.Collision.parse(track1.convert_collision(r.asset(track1.COLLISION + t)), collision.R2)
    out = []
    for p in c.polys:
        pts = collision.world_polygon(c, p)
        if len(pts) >= 3:
            out.append((p['flags'], pts, normal_of(pts)))
    return out


# ---- pickups: (record name, bit, matrix[9], world pos[3]) in the converters' order ----

def coins_2049(rom, k):
    """GOLDCOIN / SILVERCOIN records of 2049 placement file 120 + k, numbered per kind in record order as
    src/rush2049/track2049_convert.cpp does: silver KEYS0-7 = bits 0-7, gold KEYG0-7 = bits 8-15."""
    d = rom.file(120 + k)
    o, n = struct.unpack_from('>II', d, 0)
    chunks = {d[o + i * 12:o + i * 12 + 4].decode(): struct.unpack_from('>II', d, o + i * 12 + 4) for i in range(n)}
    wo, wn = chunks['WOBJ']
    counts = {'SILVERCOIN': 0, 'GOLDCOIN': 0}
    out = []
    for i in range(wn):
        r = wo + i * 0x68
        name = d[r:r + 16].split(b'\0')[0].decode('latin1')
        if name not in counts or counts[name] >= 8:
            continue
        n = counts[name]; counts[name] += 1
        gold = name == 'GOLDCOIN'
        out.append(('KEY%s%d' % ('G' if gold else 'S', n), (8 if gold else 0) + n,
                    struct.unpack_from('>9f', d, r + 0x10), struct.unpack_from('>3f', d, r + 0x34)))
    return out


def keys_rush1(r, t):
    """KEYL records of Rush 1 placement asset t: key n (record +0x4A) = KEYn, bit n - 1, at its section's position
    plus its own (src/rush1/track1_convert.cpp)."""
    d = r.asset(track1.PLACEMENT + t)
    base = struct.unpack_from('>I', d, 4)[0]
    rec = lambda i: d[base + i * 0x64:base + (i + 1) * 0x64]
    s16 = lambda b, o: struct.unpack_from('>h', b, o)[0]
    out = []
    i = 0
    while i >= 0:
        pp = struct.unpack_from('>3f', rec(i), 0x34)
        c = s16(rec(i), 0x46)
        while c >= 0:
            k = rec(c)
            if k[:16].split(b'\0')[0].startswith(b'KEYL'):
                n = s16(k, 0x4A)
                kp = struct.unpack_from('>3f', k, 0x34)
                out.append(('KEY%d' % n, n - 1, struct.unpack_from('>9f', k, 0x10),
                            tuple(pp[a] + kp[a] for a in range(3))))
            c = s16(k, 0x44)
        i = s16(rec(i), 0x44)
    return sorted(out, key=lambda e: e[1])


def judge(n64, arc, pos):
    """(kept, why)."""
    x, y, z = pos
    g64 = floor_under(n64[0], x, z, y + 2)
    garc = floor_under(arc[0], x, z, y + 2)
    if garc is None:
        return False, 'no arcade floor under it'
    if g64 is None or abs(g64 - garc) > GROUND_TOLERANCE:
        return False, 'arcade floor %.0f ft under it, N64 %s' % (y - garc, 'none' if g64 is None else '%.0f' % (y - g64))
    same = 0
    for i in range(RING_POINTS):
        a = 2 * math.pi * i / RING_POINTS
        rx, rz = x + RING_RADIUS * math.cos(a), z + RING_RADIUS * math.sin(a)
        top = max(g64, garc) + 40
        r64, rarc = floor_under(n64[0], rx, rz, top), floor_under(arc[0], rx, rz, top)
        same += (r64 is None and rarc is None) or (r64 is not None and rarc is not None and abs(r64 - rarc) <= GROUND_TOLERANCE)
    if same < RING_NEEDED:
        return False, 'surroundings differ (%d of %d points match)' % (same, RING_POINTS)
    if wall_near(arc[1], pos, garc + 1) and not wall_near(n64[1], pos, g64 + 1):
        return False, 'an arcade wall beside it'
    return True, 'ground %.0f ft below, %d of %d around match' % (y - garc, same, RING_POINTS)


def course_table(label, course, pickups, n64_polys, arcade_dir, verbose):
    arc = split(arcade.Track(arcade_dir).collision())
    n64 = split(n64_polys)
    kept = []
    for name, bit, m, pos in pickups:
        ok, why = judge(n64, arc, pos)
        if ok:
            kept.append((name, bit, m, pos))
        if verbose or not ok:
            print('  %-6s %-6s (%7.0f %5.0f %7.0f)  %s' % ('kept' if ok else 'DROP', name, *pos, why))
    print('%-20s %2d of %2d kept' % (label, len(kept), len(pickups)))
    return kept


def main():
    write = '--write' in sys.argv
    verbose = '--verbose' in sys.argv
    data49 = os.environ.get('ARCADE2049_DATA', os.path.join(PROJECTR, 'sf2049se'))
    datark = os.environ.get('SFRUSH_ARCADE_DATA', os.path.join(PROJECTR, 'sfrushrk'))
    rom49, rom1 = Rush2049(), Rush1()
    rows = []
    for k, (track, folder) in enumerate(RUSH2049):
        kept = course_table(track, 7 + k, coins_2049(rom49, k), n64_2049_polys(rom49, k),
                            os.path.join(data49, 'TRACKS', folder), verbose)
        rows.append(('rush2049_arcade', track, 7 + k, kept))
    for t, (track, folder) in enumerate(SFRUSH):
        # The Rock arcade is TRACK4 but N64 track 7 (index 6); the others follow the table's order.
        kept = course_table(track, t, keys_rush1(rom1, t), n64_rush1_polys(rom1, t),
                            os.path.join(datark, 'TRACKS', folder), verbose)
        rows.append(('sfrush_arcade', track, t, kept))
    if not write:
        return
    lines = ['// Generated by tools/arcade_collectibles.py (docs/arcade2049.md#coins-and-keys-on-the-arcade-versions): the',
             '// N64 coins and keys whose spot the arcade version of their course has. Do not edit by hand.',
             '// { mod id, track folder, course (collectibles.cpp: SF Rush 0-6, Rush 2049 7-12), record, bit, matrix, position }']
    def lit(v):     # a C++ float literal
        s = '%.9g' % v
        return s + ('f' if '.' in s or 'e' in s else '.0f')
    for mod, track, course, kept in rows:
        for name, bit, m, pos in kept:
            lines.append('{ "%s", "%s", %d, "%s", %d, { %s }, { %s } },' % (
                mod, track, course, name, bit, ', '.join(lit(v) for v in m), ', '.join(lit(v) for v in pos)))
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote', os.path.relpath(OUT, ROOT))


if __name__ == '__main__':
    main()
