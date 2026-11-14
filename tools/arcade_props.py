#!/usr/bin/env python3
"""N64 SF Rush's breakables against Rush The Rock's objects (docs/sfrush_arcade.md#breakables).

For each SF Rush course: the N64 placement's breakables (cones, meters, trees, traffic lights, trash munchers, flags,
fences, windows, gas signs), the arcade object standing at each one's spot (within 6 ft, under 25 ft across), and the
arcade objects' kinds by their makeup (texture, triangle count, height, width), with how many of each the arcade track
places in all. It is how the arcade converter's breakable_kind rule (src/arcade2049/arcade_track.cpp) was found: an
arcade object kind found at N64 breakable spots is that breakable, wherever the arcade places it.

    python tools/arcade_props.py [COURSE ...] [--spots]

COURSE: golden_gate_arcade, embarcadero_arcade, market_arcade, downtown_arcade, the_heights_arcade, sunset_arcade,
the_rock_arcade (default all). --spots also lists every N64 breakable and what stands there. Reads build/rush1.z64 (or
RUSH1_ROM) and the arcade data (%LOCALAPPDATA%/ProjectR/Data/sfrushrk, or SFRUSH_ARCADE_DATA).
"""
import collections, math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
for sub in ('rush1', 'arcade2049', 'rush2049'):
    sys.path.insert(0, os.path.join(HERE, sub))
sys.path.insert(0, HERE)
os.environ.setdefault('RUSH1_ROM', os.path.join(os.path.dirname(HERE), 'build', 'rush1.z64'))

import arcade, track1  # noqa: E402
from r1 import Rush1  # noqa: E402

DATA = os.environ.get('SFRUSH_ARCADE_DATA', os.path.join(os.environ.get('LOCALAPPDATA', ''), 'ProjectR', 'Data', 'sfrushrk'))
# (course, arcade TRACKS folder), in N64 track order (The Rock is N64 track 7).
SFRUSH = [('golden_gate_arcade', 'TRACK1'), ('embarcadero_arcade', 'TRACK2'), ('market_arcade', 'TRACK3'),
          ('downtown_arcade', 'TRACK5'), ('the_heights_arcade', 'TRACK6'), ('sunset_arcade', 'TRACK7'),
          ('the_rock_arcade', 'TRACK4')]
# N64 breakable model prefixes (src/rush1/track1_convert.cpp r1_classes).
CLASSES = ('CONE1L', 'METERL', 'TREEHIT', 'FLAG2L', 'FENCEL', 'GASIGNL', 'WINDOWBL', 'PMUNCH', 'TMUNCHL', 'T5GATEL')


def n64_breakables(rom, t):
    """(class, world position) of N64 track t's breakable records: section position plus its own."""
    d = rom.asset(track1.PLACEMENT + t)
    base = struct.unpack_from('>I', d, 4)[0]
    rec = lambda i: d[base + i * 0x64:base + (i + 1) * 0x64]
    s16 = lambda b, o: struct.unpack_from('>h', b, o)[0]
    out, i = [], 0
    while i >= 0:
        pp = struct.unpack_from('>3f', rec(i), 0x34)
        c = s16(rec(i), 0x46)
        while c >= 0:
            k = rec(c)
            n = k[:16].split(b'\0')[0].decode('latin1')
            if n.startswith(CLASSES):
                kp = struct.unpack_from('>3f', k, 0x34)
                out.append((next(x for x in CLASSES if n.startswith(x)), tuple(pp[a] + kp[a] for a in range(3))))
            c = s16(k, 0x44)
        i = s16(rec(i), 0x44)
    return out


def arcade_objects(t):
    """Per worlds.rom placement: (name, makeup, center, width): makeup = (textures, triangles, height, width) rounded."""
    out = []
    for name, _, m, pos in t.worlds():
        o = t.object_named(name)
        if o is None or not o.lods[0].verts:
            continue
        lod = o.lods[0]
        vs = [[pos[a] + sum(m[3 * a + b] * v[0][b] for b in range(3)) for a in range(3)] for v in lod.verts]
        lo = [min(v[a] for v in vs) for a in range(3)]
        hi = [max(v[a] for v in vs) for a in range(3)]
        width = max(hi[0] - lo[0], hi[2] - lo[2])
        tex = tuple(sorted({t.textures[tr[3]].name for tr in lod.tris if tr[3] < len(t.textures)}))
        out.append((name, (tex, len(lod.tris), round(hi[1] - lo[1]), round(width)),
                    [(lo[a] + hi[a]) / 2 for a in range(3)], width))
    return out


def main():
    spots = '--spots' in sys.argv
    only = [a for a in sys.argv[1:] if not a.startswith('--')]
    rom = Rush1()
    for t, (course, folder) in enumerate(SFRUSH):
        if only and course not in only:
            continue
        n64 = n64_breakables(rom, t)
        objs = arcade_objects(arcade.Track(os.path.join(DATA, 'TRACKS', folder)))
        placed = collections.Counter(mk for _, mk, _, _ in objs)
        found = collections.defaultdict(collections.Counter)
        lines = []
        for cls, p in n64:
            best = min(objs, key=lambda e: math.hypot(e[2][0] - p[0], e[2][2] - p[2]))
            d = math.hypot(best[2][0] - p[0], best[2][2] - p[2])
            if d < 6 and best[3] < 25:
                found[best[1]][cls] += 1
                lines.append('    %-9s at (%.0f, %.0f, %.0f): %s, %.1f ft off, %s' % (cls, *p, best[0], d, best[1]))
            else:
                lines.append('    %-9s at (%.0f, %.0f, %.0f): nothing (nearest %s, %.0f ft)' % (cls, *p, best[0], d))
        print('%s: N64 %s' % (course, dict(collections.Counter(c for c, _ in n64))))
        for mk, classes in found.items():
            print('  %s at N64 %s spots; the arcade places %d' % (mk, dict(classes), placed[mk]))
        if spots:
            print('\n'.join(lines))


if __name__ == '__main__':
    main()
