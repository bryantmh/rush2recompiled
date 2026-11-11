r"""Where the PS1 SF Rush's Time Sprint clocks go on every version of their courses (docs/sfrush_ps1.md#time-sprint),
written as the C++ table src/sfrush_ps1/time_sprint_clocks.inc (read by src/time_sprint.cpp).

    python -I tools/sfrush_ps1/time_sprint_clocks.py --ps1 OUT/sfrush_ps1 [--write] [--verbose]

--ps1 is a sfrush_ps1 mod folder (its tracks/<folder>/track.glb, as the PS1 converter writes them or unzipped from
build/mods/sfrush_ps1.rtz), with SFRUSH_PS1_DISC the PS1 disc (RUSH.EXE holds the clocks). Reads RUSH1_ROM (default
build/rush1.z64) and the SF Rush arcade data (SFRUSH_ARCADE_DATA, default ProjectR's sfrushrk).

The PS1 places 71 clocks (RUSH.EXE 0x800A59C4, 16-byte records: s32 x, y, height in 0.1 ft of the PS1 frame, PS1
track 0-3) only in Time Sprint mode, the model EXTRA/TIME.PSV (an hourglass 5 ft across and 5.6 ft tall, its origin
at its middle). Each version of a course gets the clocks whose spot it has, judged much as
tools/arcade_collectibles.py judges the PS1 keys but looser (clock_judge: the floor under it within 6 ft, the floor
30 ft around it matching at 4 of 8 points, no new wall beside it; many clocks lie off the drones' line, on the
courses' other routes, so the AI lanes say nothing about whether one can be reached): the PS1 mod's versions against
its night version, the N64 course through a local fit of the PS1 course onto it, and the arcade version (in the N64
frame) the same way. A clock keeps its height above the floor of
the version it comes from, its middle at least CLOCK_MIDDLE (4 ft) up so it hangs clear of the road. Prints a line per clock and version; --write writes the table.
"""
import math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOLS = os.path.dirname(HERE)
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
sys.path.insert(0, HERE)

import arcade_collectibles as ac

OUT = os.path.join(ROOT, 'src', 'sfrush_ps1', 'time_sprint_clocks.inc')
PS1_CLOCKS = 0x800A59C4
# A clock needs its spot, not the same surroundings a key does: the floor under it within CLOCK_GROUND ft of the source
# version's (it is moved onto it), the floor at RING_NEEDED of the 8 ring points within CLOCK_GROUND too (a road a few
# ft wider or narrower still passes), and no wall at it that the source lacks. It keeps its height above the floor, but
# its middle at least CLOCK_MIDDLE up: the PS1's records put the middle 1-3 ft up (2 ft mostly) and the hourglass is
# 2.8 ft from middle to foot, so as recorded it stands sunk in the road. 4 ft is where SF Rush's own keys hang
# (arcade_collectibles.PS1_KEY_RAISE raises the PS1's keys to it), and the foot clears the road by 1.2 ft.
CLOCK_GROUND, RING_NEEDED = 6.0, 4
CLOCK_MIDDLE = 4.0
NUDGE_STEPS = (4.0, 8.0, 12.0, 16.0)


def clock_judge(src, dst, pos):
    """(kept, why, the clock's position on dst)."""
    x, y, z = pos
    gs = ac.floor_under(src[0], x, z, y + 2)
    gd = ac.floor_under(dst[0], x, z, y + 2 + CLOCK_GROUND)
    if gs is None:
        return False, 'no floor under it on the source', pos
    if gd is None:
        return False, 'no floor under it on this version', pos
    if abs(gd - gs) > CLOCK_GROUND:
        return False, 'floor %.0f ft under it here, %.0f on the source' % (y - gd, y - gs), pos
    same = 0
    for k in range(ac.RING_POINTS):
        a = 2 * math.pi * k / ac.RING_POINTS
        rx, rz = x + ac.RING_RADIUS * math.cos(a), z + ac.RING_RADIUS * math.sin(a)
        top = max(gs, gd) + 40
        r0, r1 = ac.floor_under(src[0], rx, rz, top), ac.floor_under(dst[0], rx, rz, top)
        same += (r0 is None and r1 is None) or (r0 is not None and r1 is not None and abs(r0 - r1) <= CLOCK_GROUND)
    if same < RING_NEEDED:
        return False, 'surroundings differ (%d of %d points match)' % (same, ac.RING_POINTS), pos
    q = (x, gd + max(y - gs, CLOCK_MIDDLE), z)
    if ac.wall_near(dst[1], q, gd + 1) and not ac.wall_near(src[1], pos, gs + 1):
        return False, 'a wall beside it here', pos
    return True, 'floor %.0f ft below, %d of %d around match' % (q[1] - gd, same, ac.RING_POINTS), q


def clock_place(src, dst, pos):
    """clock_judge at pos, else at the nearest of the points up to NUDGE_REACH ft around it that passes (a wall a few
    ft nearer on this version, or a local fit a few ft off): (kept, why, position)."""
    ok, why, q = clock_judge(src, dst, pos)
    if ok:
        return ok, why, q
    for r in NUDGE_STEPS:
        for k in range(16):
            a = 2 * math.pi * k / 16
            p = (pos[0] + r * math.cos(a), pos[1], pos[2] + r * math.sin(a))
            ok2, why2, q2 = clock_judge(src, dst, p)
            if ok2:
                return True, '%s (moved %.0f ft; at its spot: %s)' % (why2, r, why), q2
    return False, why, pos


def ps1_clocks():
    """{PS1 track: [world xyz]} of the clocks (their middles) in the sfrush_ps1 mod's Rush 2 world frame (as
    arcade_collectibles.ps1_keys)."""
    from ps1 import Disc, EXE_ADDR
    exe = Disc().exe()
    out, o = {}, PS1_CLOCKS - EXE_ADDR
    while True:
        x, y, h, t = struct.unpack_from('<4i', exe, o)
        if x == -1:
            return out
        out.setdefault(t, []).append((-y / 10, -h / 10, x / 10))
        o += 16


def main():
    write = '--write' in sys.argv
    verbose = '--verbose' in sys.argv
    if '--ps1' not in sys.argv:
        sys.exit(__doc__)
    ps1_dir = sys.argv[sys.argv.index('--ps1') + 1]
    datark = os.environ.get('SFRUSH_ARCADE_DATA', os.path.join(ac.PROJECTR, 'sfrushrk'))
    from r1 import Rush1
    rom1 = Rush1()
    clocks = ps1_clocks()
    rows = []   # (mod id or "", track folder or "", course, clock index, xyz)
    for t, track in enumerate(ac.PS1_TRACKS):
        versions = [track + c for c in ac.PS1_CONDITIONS
                    if os.path.exists(os.path.join(ps1_dir, 'tracks', track + c, 'track.glb'))]
        own = clocks.get(t, [])
        course = ac.PS1_COURSE if t == 3 else t
        if t == 3:  # Chinatown: its own course, the same model in every condition.
            raw = {v: ac.split(ac.glb_polys(os.path.join(ps1_dir, 'tracks', v, 'track.glb'))) for v in versions}
            for v in versions:
                for i, p in enumerate(own):
                    ok, why, q = clock_place(raw['chinatown'], raw[v], p)
                    if ok:
                        rows.append(('sfrush_ps1', v, course, i, q))
                    else:
                        print('  clock %2d %-20s DROP: %s' % (i, v, why))
                print('%-20s %2d clocks' % (v, len(own)))
            continue
        raw = {v: ac.glb_polys(os.path.join(ps1_dir, 'tracks', v, 'track.glb')) for v in versions}
        polys = {v: ac.split(raw[v]) for v in versions}
        ref = track + '_night'
        n64 = ac.split(ac.n64_rush1_polys(rom1, t))
        fit = ac.CourseFit(polys[ref][0], n64[0])
        arcade_track, arcade_folder = ac.SFRUSH[t]
        arc = ac.split(ac.arcade.Track(os.path.join(datark, 'TRACKS', arcade_folder)).collision())
        kept = {v: 0 for v in versions + ['N64', arcade_track]}
        print('%s: %d PS1 clocks' % (track, len(own)))
        for i, p in enumerate(own):
            f, err = fit.near(p)
            p64 = f.to_n64(p)
            print('  clock %2d at (%6.0f %5.0f %6.0f), N64 (%6.0f %5.0f %6.0f), local fit %.1f ft' % (i, *p, *p64, err))
            for v in versions:
                ok, why, q = clock_place(polys[ref], polys[v], p)
                if ok:
                    rows.append(('sfrush_ps1', v, course, i, q))
                    kept[v] += 1
                if verbose or not ok:
                    print('    %-20s %s: %s' % (v, 'kept' if ok else 'DROP', why))
            ps1_near = ac.split(f.polys(raw[ref], p64))
            for label, other, key in (('N64', n64, ('', '')), (arcade_track, arc, ('sfrush_arcade', arcade_track))):
                ok, why, q = clock_place(ps1_near, other, p64)
                print('    %-20s %s: %s' % (label, 'kept' if ok else 'DROP', why))
                if ok:
                    rows.append((key[0], key[1], course, i, q))
                    kept[label] += 1
        for v, n in kept.items():
            print('%-20s %2d of %2d clocks' % (v, n, len(own)))
    if not write:
        return
    lines = ['// Generated by tools/sfrush_ps1/time_sprint_clocks.py (docs/sfrush_ps1.md#time-sprint): the PS1 SF Rush\'s',
             '// Time Sprint clocks on every version of their courses that has their spot. Do not edit by hand.',
             '// { mod id ("" = the N64 course), track folder, course (SF Rush 0-2, Chinatown 17), PS1 clock index,',
             '//   position of its middle (Rush 2 world ft) }']
    def lit(v):
        s = '%.9g' % v
        return s + ('f' if '.' in s or 'e' in s else '.0f')
    for mod, folder, course, i, p in rows:
        lines.append('{ "%s", "%s", %d, %d, { %s } },' % (mod, folder, course, i, ', '.join(lit(v) for v in p)))
    with open(OUT, 'w', newline='\n') as f:
        f.write('\n'.join(lines) + '\n')
    print('wrote', os.path.relpath(OUT, ROOT))


if __name__ == '__main__':
    main()
