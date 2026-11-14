r"""Which N64 coins (Rush 2049) and keys (SF Rush) have the same spot on the arcade versions of their courses, written as
the C++ table src/arcade2049/arcade_collectibles.inc (docs/arcade2049.md#coins-and-keys-on-the-arcade-versions); and
where the PS1 SF Rush's keys go (docs/sfrush_ps1.md#keys): on the sfrush_ps1 mod's tracks and the arcade versions (the
same table) and on the N64 courses (src/rush1/ps1_keys.inc).

    python -I tools/arcade_collectibles.py --ps1 OUT/sfrush_ps1 [--write] [--verbose]

--ps1 is the sfrush_ps1 mod folder the PS1 converter writes (cmd //c "tools\sfrush_ps1\cpp_test\build.bat OUT DISC"),
with SFRUSH_PS1_DISC the PS1 disc (its RUSH.EXE holds the keys). --write needs it, so no table loses the PS1's rows.

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
sys.path.insert(0, HERE)    # glb_view (the PS1 tables); -I leaves the script's folder off the path
os.environ.setdefault('RUSH2049_ROM', os.path.join(ROOT, 'build', 'rush2049.z64'))
os.environ.setdefault('RUSH1_ROM', os.path.join(ROOT, 'build', 'rush1.z64'))

import arcade, collision, track1
from roms import Rush2049
from r1 import Rush1

OUT = os.path.join(ROOT, 'src', 'arcade2049', 'arcade_collectibles.inc')
OUT_N64 = os.path.join(ROOT, 'src', 'rush1', 'ps1_keys.inc')
GROUND_TOLERANCE = 3.0
RING_RADIUS, RING_POINTS, RING_NEEDED = 30.0, 8, 6
WALL_REACH = 4.0
MARKER_REACH = 3.0      # ft: a key is the arcade's own when its key marker is placed this close
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
        return False, 'no floor under it on this version'
    if g64 is None or abs(g64 - garc) > GROUND_TOLERANCE:
        return False, 'floor %.0f ft under it here, %s on the source' % (y - garc, 'none' if g64 is None else '%.0f' % (y - g64))
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
        return False, 'a wall beside it here'
    return True, 'ground %.0f ft below, %d of %d around match' % (y - garc, same, RING_POINTS)


# ---- the PS1 SF Rush's keys (docs/sfrush_ps1.md#keys) ----
#
# The PS1 game has 4 keys on each of its tracks (RUSH.EXE 0x800A5E44). Its tracks 1-3 are the N64 courses Golden Gate,
# Embarcadero and Market in the PS1's own models (their snow, night and stunt versions are the sfrush_ps1 mod's), track
# 4 is its own course (Chinatown). The PS1 tracks are in the PS1's frame, not the N64's, and their models differ by a
# few ft here and there, so each spot is compared through a local fit: the PS1 floors within FIT_RADIUS of it, placed by
# a fit of the whole course (tools/sfrush_ps1/n64_census.py's fit2d), then fitted onto the N64 floors there (a turn
# about the vertical and a shift).
#   - A PS1 key is the N64 key within MATCH_RADIUS of it across and MATCH_HEIGHT up or down (the courses' hills differ
#     by up to about 15 ft: Golden Gate's N64 KEY4 is 17 ft above the PS1 key at its spot), or else a key of its own: record KEYPn (PS1 key n, bit 8 + n - 1 on that course, src/collectibles.cpp).
#   - Each version of a course gets every key of the course whose spot it has, by judge() against the version the key
#     is from: the N64's keys against the N64 course, the PS1's own against the PS1 night version. The PS1's snow and
#     night versions always have the PS1's own keys (the PS1 places them in every condition).
#   - A key keeps its height above the floor of the version it comes from. The PS1's key model has its origin 1.6 ft
#     above the ground, SF Rush's KEYL1 (which every version draws) 4 ft (the N64 courses' ground keys), so the PS1's
#     are raised by PS1_KEY_RAISE.
PS1_TRACKS = ['golden_gate', 'embarcadero', 'market', 'chinatown']   # PS1 track index 0-3; 0-2 = SF Rush courses 0-2
PS1_CONDITIONS = ['', '_snow', '_night', '_stunt']
PS1_COURSE = 7 + 6 + 4      # Chinatown's course in src/collectibles.cpp (after the 2049 race tracks and arenas)
PS1_KEYS = 0x800A5E44       # 16-byte records: s32 x, y, height (0.1 ft, PS1 frame), track 0-3; ended by x = -1
FIT_RADIUS, MATCH_RADIUS, MATCH_HEIGHT = 250.0, 40.0, 20.0
FEET_PER_METER = 3.2808399
PS1_KEY_RAISE = 2.4
IDENTITY = (1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0)


def ps1_keys():
    """{PS1 track: [world xyz]} of the PS1's keys (raised by PS1_KEY_RAISE), in the sfrush_ps1 mod's Rush 2 world
    frame: the converter's game frame (PS1 y, height, PS1 x) is written to glTF with x kept (src/sfrush_ps1/ps1_track.cpp gl()), and the track
    converter reads glTF back with x negated."""
    sys.path.insert(0, os.path.join(HERE, 'sfrush_ps1'))
    from ps1 import Disc, EXE_ADDR
    exe = Disc().exe()
    out, o = {}, PS1_KEYS - EXE_ADDR
    while True:
        x, y, h, t = struct.unpack_from('<4i', exe, o)
        if x == -1:
            return out
        out.setdefault(t, []).append((-y / 10, -h / 10 + PS1_KEY_RAISE, x / 10))
        o += 16


def glb_polys(path):
    """(flags, [world xyz], normal) of a converted track's COL_ meshes (walls get collision.TYPE_WALL's type 5)."""
    import glb_view
    g, b = glb_view.load(path)
    out = []
    for n in g['nodes']:
        name = n.get('name', '')
        if not name.startswith('COL_') or 'mesh' not in n or name.startswith('COL_KILL'):
            continue
        wall = name.startswith('COL_WALL')
        for prim in g['meshes'][n['mesh']]['primitives']:
            P = glb_view.accessor(g, b, prim['attributes']['POSITION'])
            I = glb_view.accessor(g, b, prim['indices']).reshape(-1, 3).astype(int)
            for tri in I:
                pts = [(-float(P[i][0]) * FEET_PER_METER, float(P[i][1]) * FEET_PER_METER,
                        float(P[i][2]) * FEET_PER_METER) for i in tri]
                out.append((5 if wall else 0, pts, normal_of(pts)))
    return out


class Fit:
    """PS1 world -> N64 world near one spot: x' = R x + T, R a turn about the vertical."""
    def __init__(self, R, T):
        import numpy as np
        self.R, self.T = np.asarray(R, float), np.asarray(T, float)

    def to_n64(self, p):
        import numpy as np
        return tuple(float(v) for v in self.R @ np.asarray(p, float) + self.T)

    def to_ps1(self, p):
        import numpy as np
        return tuple(float(v) for v in self.R.T @ (np.asarray(p, float) - self.T))

    def polys(self, polys, around, reach=120.0):
        """The polygons with a corner within reach (x/z) of the N64 point around, moved to the N64 frame."""
        c = self.to_ps1(around)
        out = []
        for f, pts, n in polys:
            if min(math.hypot(q[0] - c[0], q[2] - c[2]) for q in pts) > reach and not inside(c[0], c[2], pts):
                continue
            out.append((f, [self.to_n64(q) for q in pts], tuple(float(v) for v in self.R @ n)))
        return out


class CourseFit:
    """The PS1 version of an N64 course placed on it: a whole-course fit, refined around each spot."""
    def __init__(self, ps1_floors, n64_floors):
        import numpy as np
        from scipy.spatial import cKDTree
        sys.path.insert(0, os.path.join(HERE, 'sfrush_ps1'))
        import n64_census
        rng = np.random.default_rng(1)
        def samples(floors, step):
            T = np.array([pts[:3] for _, pts, _ in floors], float)
            return n64_census.sample(T, step, rng)[0]
        self.A, self.B = samples(ps1_floors, 3.0), samples(n64_floors, 3.0)
        a = self.A[rng.choice(len(self.A), min(len(self.A), 20000), replace=False)][:, [0, 2]] / FEET_PER_METER
        b = self.B[rng.choice(len(self.B), min(len(self.B), 8000), replace=False)][:, [0, 2]] / FEET_PER_METER
        # fit2d takes the N64 onto the PS1 (meters): xz' = s (mirror xz) R^T + t, s = 1.
        _, mir, R, s, t = n64_census.fit2d(b, a, cKDTree(a))
        if mir != 1:
            raise RuntimeError('the PS1 course fits the N64 one mirrored: the PS1 world frame is wrong')
        R3 = np.eye(3); R3[0, 0], R3[0, 2], R3[2, 0], R3[2, 2] = R[0, 0], R[0, 1], R[1, 0], R[1, 1]
        Ri = R3.T   # PS1 -> N64
        T = -Ri @ np.array([t[0] * FEET_PER_METER, 0.0, t[1] * FEET_PER_METER])
        moved = self.A @ Ri.T + T
        d, i = cKDTree(self.B[:, [0, 2]]).query(moved[:, [0, 2]])
        T[1] = float(np.median(self.B[i[d < 3], 1] - moved[d < 3, 1]))
        self.R, self.T = Ri, T
        self.tree = cKDTree(self.B)

    def near(self, p_ps1):
        """The fit around a PS1 world point: the PS1 floors within FIT_RADIUS fitted onto the N64's (trimmed ICP),
        and its median miss (ft)."""
        import numpy as np
        A = self.A[np.hypot(self.A[:, 0] - p_ps1[0], self.A[:, 2] - p_ps1[2]) < FIT_RADIUS]
        R, T = self.R.copy(), self.T.copy()
        if len(A) < 50:     # the PS1 course doesn't reach it: the whole-course fit
            return Fit(R, T), float('inf')
        for _ in range(30):
            X = A @ R.T + T
            d, i = self.tree.query(X)
            k = d <= np.quantile(d, 0.7)
            a, b = X[k], self.B[i[k]]
            ca, cb = a.mean(0), b.mean(0)
            U, _, Vt = np.linalg.svd((a - ca)[:, [0, 2]].T @ (b - cb)[:, [0, 2]])
            if np.linalg.det(Vt.T @ U.T) < 0:
                Vt[1] *= -1
            r2 = Vt.T @ U.T
            r3 = np.eye(3); r3[0, 0], r3[0, 2], r3[2, 0], r3[2, 2] = r2[0, 0], r2[0, 1], r2[1, 0], r2[1, 1]
            R, T = r3 @ R, r3 @ T + cb - r3 @ ca
        return Fit(R, T), float(np.median(self.tree.query(A @ R.T + T)[0]))

    def near_n64(self, p_n64):
        """The fit around an N64 world point (found from the whole-course fit's PS1 point there)."""
        return self.near(Fit(self.R, self.T).to_ps1(p_n64))


def lift(pos, src, dst):
    """pos (a point over the floors src) at the same height above the floors dst, at the same x/z."""
    g0 = floor_under(src[0], pos[0], pos[2], pos[1] + 2)
    g1 = floor_under(dst[0], pos[0], pos[2], pos[1] + 2 + GROUND_TOLERANCE)
    if g0 is None or g1 is None:
        return tuple(pos)
    return (pos[0], g1 + pos[1] - g0, pos[2])


def ps1_tables(rom1, ps1_dir, data_rk, verbose):
    """The PS1's keys: (rows for the sfrush_ps1 mod's tracks, PS1 keys for the N64 courses {course: [key]}, rows for
    the arcade versions)."""
    keys = ps1_keys()
    rows, n64_extra, arcade_rows = [], {}, []
    for t, track in enumerate(PS1_TRACKS):
        versions = [track + c for c in PS1_CONDITIONS
                    if os.path.exists(os.path.join(ps1_dir, 'tracks', track + c, 'track.glb'))]
        own = [('KEYP%d' % (n + 1), 8 + n, IDENTITY, p) for n, p in enumerate(keys.get(t, []))]
        if t == 3:  # Chinatown: its own course; every version has its keys.
            for v in versions:
                rows.append(('sfrush_ps1', v, PS1_COURSE, own))
                print('%-20s %2d PS1 keys' % (v, len(own)))
            continue
        polys = {v: glb_polys(os.path.join(ps1_dir, 'tracks', v, 'track.glb')) for v in versions}
        ref = track + '_night'
        n64 = split(n64_rush1_polys(rom1, t))
        fit = CourseFit(split(polys[ref])[0], n64[0])
        n64_keys = keys_rush1(rom1, t)
        arcade_track, arcade_folder = SFRUSH[t]
        arc = split(arcade.Track(os.path.join(data_rk, 'TRACKS', arcade_folder)).collision())
        kept = {v: [] for v in versions}
        print('%s: PS1 keys' % track)
        for name, bit, m, kp in own:
            f, err = fit.near(kp)
            p64 = f.to_n64(kp)
            ps1_near = split(f.polys(polys[ref], p64))
            same = None
            for nn, nb, nm, npos in n64_keys:
                across = math.hypot(p64[0] - npos[0], p64[2] - npos[2])
                if across <= MATCH_RADIUS and abs(p64[1] - npos[1]) <= MATCH_HEIGHT:
                    same = (nn, nb, across)
            print('  %-6s at (%5.0f %4.0f %5.0f), N64 (%5.0f %4.0f %5.0f), local fit %.1f ft: %s' % (
                name, *kp, *p64, err, 'the N64 %s (%.0f ft away across)' % (same[0], same[2]) if same else 'a new spot'))
            if same:
                for v in versions:
                    kept[v].append((same[0], same[1], m, kp))     # at the PS1's own spot
                continue
            for v in versions:
                if v in (ref, track + '_snow'):
                    kept[v].append((name, bit, m, kp))
                    continue
                ok, why = judge(split(polys[ref]), split(polys[v]), kp)
                if ok:
                    kept[v].append((name, bit, m, lift(kp, split(polys[ref]), split(polys[v]))))
                print('    %-20s %s: %s' % (v, 'kept' if ok else 'DROP', why))
            for label, other, out in (('N64', n64, None), (arcade_track, arc, arcade_rows)):
                ok, why = judge(ps1_near, other, p64)
                print('    %-20s %s: %s' % (label, 'kept' if ok else 'DROP', why))
                if not ok:
                    continue
                k = (name, bit, m, lift(p64, ps1_near, other))
                if out is None:
                    n64_extra.setdefault(t, []).append(k)
                else:
                    out.append(('sfrush_arcade', arcade_track, t, [k]))
        print('%s: N64 keys on the PS1 versions' % track)
        for name, bit, m, npos in n64_keys:
            if any(k[0] == name for k in kept[ref]):
                continue    # the PS1 has it, at its own spot
            f, err = fit.near_n64(npos)
            for v in versions:
                near = split(f.polys(polys[v], npos))
                ok, why = judge(n64, near, npos)
                if ok:
                    kept[v].append((name, bit, m, f.to_ps1(lift(npos, n64, near))))
                if verbose or not ok or v == ref:
                    print('  %-6s %-20s %s: %s (local fit %.1f ft)' % (name, v, 'kept' if ok else 'DROP', why, err))
        for v in versions:
            rows.append(('sfrush_ps1', v, t, sorted(kept[v], key=lambda k: k[1])))
            print('%-20s %2d keys' % (v, len(kept[v])))
    return rows, n64_extra, arcade_rows


def key_markers(t):
    """Rush The Rock's own key markers: worlds.rom placements of objects drawn only in an ATARI texture (TRACK5-7, at the
    N64 keys' spots; the converter leaves them out, docs/sfrush_arcade.md). Their positions."""
    atari = {i for i, x in enumerate(t.textures) if x.name.upper().startswith('ATARI')}
    out = []
    for name, _, _, pos in t.worlds():
        o = t.object_named(name)
        if o and o.lods[0].tris and all(tr[3] in atari for tr in o.lods[0].tris):
            out.append(pos)
    return out


def course_table(label, course, pickups, n64_polys, arcade_dir, verbose):
    t = arcade.Track(arcade_dir)
    arc = split(t.collision())
    n64 = split(n64_polys)
    markers = key_markers(t)
    kept = []
    for name, bit, m, pos in pickups:
        ok, why = judge(n64, arc, pos)
        # A dropped key the arcade has its own marker at (The Heights KEY1-2, Sunset KEY6-8) is only noted: left out
        # for now, see TODO.txt (renders of the five spots: tools/glb_view.py on the pre-2026-11-13 arcade glbs).
        if not ok and any(math.dist(pos, p) <= MARKER_REACH for p in markers):
            why += "; the arcade's own key marker is here"
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
    ps1_dir = sys.argv[sys.argv.index('--ps1') + 1] if '--ps1' in sys.argv else None
    if write and ps1_dir is None:
        sys.exit('--write needs --ps1 OUT/sfrush_ps1 (the PS1 keys are in the same tables)')
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
    n64_extra = {}
    if ps1_dir is not None:
        ps1_rows, n64_extra, arcade_extra = ps1_tables(rom1, ps1_dir, datark, verbose)
        for mod, track, course, kept in arcade_extra:
            next(r for r in rows if r[0] == mod and r[1] == track)[3].extend(kept)
        rows += ps1_rows
    if not write:
        return
    lines = ['// Generated by tools/arcade_collectibles.py (docs/arcade2049.md#coins-and-keys-on-the-arcade-versions): the',
             '// N64 coins and keys whose spot the arcade version of their course has, and the PS1 SF Rush keys of the',
             "// arcade versions and the sfrush_ps1 mod's tracks (docs/sfrush_ps1.md#keys). Do not edit by hand.",
             '// { mod id, track folder, course (collectibles.cpp: SF Rush 0-6, Rush 2049 7-12, PS1 17), record, bit, matrix,',
             '//   position }']
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
    lines = ['// Generated by tools/arcade_collectibles.py (docs/sfrush_ps1.md#keys): the PS1 SF Rush keys whose spot the N64',
             '// course has, placed by src/rush1/track1_convert.cpp. Do not edit by hand.',
             '// { course (SF Rush 0-6), record, bit, position }']
    for t in sorted(n64_extra):
        for name, bit, m, pos in n64_extra[t]:
            lines.append('{ %d, "%s", %d, { %s } },' % (t, name, bit, ', '.join(lit(v) for v in pos)))
    with open(OUT_N64, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote', os.path.relpath(OUT_N64, ROOT))


if __name__ == '__main__':
    main()
