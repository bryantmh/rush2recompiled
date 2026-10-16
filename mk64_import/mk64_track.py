#!/usr/bin/env python3
"""Builds a Mario Kart 64 course as a Rush 2 custom-track mod, straight from the MK64 (US) ROM.

    python mk64_import/mk64_track.py rainbow_road|all [more courses] [--rom "Mario Kart 64.n64"] [--out build/mods/mk64]

The output is an ordinary custom-track mod (docs/custom_tracks.md): mod.json and tracks/<course>/ with track.glb,
track.toml and logo.png. Nothing in it is special to the game: it goes through the same converter a Blender export
does, which is what it is for, whole real tracks to test the converter on. Rainbow Road is the tested case.

Where each part comes from (mk64 decomp names; mk64.py finds the tables in the ROM by their shape, courses.toml holds
what only MK64's code knows about a course):
  - visible track: the section DLs of d_course_<name>_dl_list (render_course_segments draws one set per camera
    section; together they draw every piece, so each triangle is taken once, less the stand-ins far sections draw over
    near pieces: see build()), with the textures and render modes they
    set and the vertex colors course init gives them (courses.toml vertex_colors). A course without such a list
    (MK64 draws those from code) falls back to every top-level packed DL.
  - collision: d_course_<name>_addr (TrackSections, parse_course_displaylists -> generate_collision_mesh) with MK64's
    rule that a triangle whose three vertices have flag 4 is left out. Steep triangles become COL_WALL_* facing the
    path (one face: back-to-back faces push a car that gets into a wall out through it), the rest COL_<surface>_* by
    the section's surface type (SURFACES) facing up. courses.toml wall_height raises the walls' collision (Rush 2
    cars jump far higher than karts) and kill_below adds a COL_KILL_* sheet under the course for Lakitu's falls.
  - AI path: the course's track path (TrackPathPoints 20 units apart in the race direction, point 0 on the start
    line), as a flat ribbon over the road, its width a share of the road's measured width there.
  - FINISH at path point 0, CHECKPOINT_n evenly along the lap, each as wide as the road there; courses.toml ai_speed
    adds AISPEED_ markers.
  - SKY: a dome shaded from the course's sky colors (top to horizon, MK64 draws the same gradient behind the course)
    and its star field (init_stars' StarData: a sprite per star at an angle round the camera and a height on screen,
    placed on the dome at that bearing and elevation, D_0D0293D8 as its texture).
  - battle courses (courses.toml type = "battle"): the DLs render_<course>() draws and collides with, START_1-4 at
    the 4 player battle spawns and PICKUP_ markers at the item boxes; track.toml type = "battle".
  - actors (courses.toml [[course.<name>.actors]]): billboards as two crossed copies with a COL_WALL trunk, models,
    and the spinning signs and the Yoshi egg as MOVE_ objects; segment 3 rebuilt from init_actors()' texture list.
Not taken: the objects MK64 runs from code (vehicles, thwomps, moles, Rainbow Road's neon signs ...), clouds
(mk64_import/README.md).

Units: MK64 world units map to meters by --scale (default 0.2: Rainbow Road is about 112 units, 22 m, wide; a kart
about 10 units, a Rush car 1.8 m). MK64's world is right handed with y up like glTF, so a point is (x, y, z) * scale.
"""
import argparse
import json
import struct
import math
import os
import sys
import tomllib
from collections import defaultdict

from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mk64  # noqa: E402
from glb import Glb, png_bytes  # noqa: E402

ROOT = os.path.dirname(HERE)

# MK64 surface type (include/mk64.h SURFACE_TYPE) -> Rush 2 collision surface. MK64's on-course dirt, sand and snow
# are full-speed race surfaces; only the *_OFFROAD types and grass slow karts. On-course loose surfaces become
# FASTDIRT, a custom surface (track.toml [surfaces.fastdirt]) that looks and sounds like dirt without the slowdown.
SURFACES = {0x01: 'ROAD', 0x04: 'ROAD', 0x06: 'ROAD', 0x09: 'ROAD', 0x0E: 'ROAD', 0x0F: 'ROAD', 0x10: 'ROAD',
            0x11: 'ROAD', 0xFC: 'ROAD', 0xFE: 'ROAD', 0xFF: 'ROAD',
            0x02: 'FASTDIRT', 0x03: 'FASTDIRT', 0x05: 'FASTDIRT', 0x0A: 'FASTDIRT',
            0x07: 'DIRT', 0x0D: 'DIRT', 0x08: 'GRASS', 0x0B: 'GRASS', 0x0C: 'WALL', 0xFD: 'KILL'}
# The render state render_<course>() (render_courses.c) sets before the section lists: courses.toml render_mode.
# Only DK's Jungle Parkway draws them alpha-tested (G_RM_AA_ZB_TEX_EDGE: its trees and leaves); the rest opaque.
RENDER_MODE = {'texedge': 0x00553078, 'opa': 0x00552078, 'xlu': 0x005049D8}
# Star sprites: MK64 places them by camera yaw at 320 px per (gCameraZoom 40 + 40) degrees (func_800788F8); the
# sprite is 16 px at scalePercent. Their height is a screen row (posY against the horizon row, func_80051ABC): a
# layer over whatever sky the screen shows. Rush 2's camera sees more sky, so the rows spread over STAR_ELEVATION
# (degrees above the horizon) in their order, lowest row lowest.
STAR_DEG_PER_PX = 80.0 / 320.0
STAR_ELEVATION = (3.0, 70.0)
SKY_TOP_DEG, SKY_BOTTOM_DEG = 30.0, 30.0    # where the top and bottom gradients end on the dome
STAND_IN_REACH = 300.0      # MK64 units: how much farther than the nearest section the nearest drawing section may be
STAND_IN_GAP = 40.0         # MK64 units: how far over or under a kept triangle a far-only one counts as its stand-in
SKY_RADIUS = 500.0         # m; the converter scales the sky to fit its model and draws it without depth


def normal(p):
    a, b, c = p
    u = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    v = (c[0] - a[0], c[1] - a[1], c[2] - a[2])
    n = (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])
    m = math.sqrt(sum(x * x for x in n))
    return tuple(x / m for x in n) if m else None


def color(hex_):
    return tuple(int(hex_[i:i + 2], 16) / 255.0 for i in (1, 3, 5))


class Ground:
    """Height of the road under a point (MK64 units), from the drivable triangles."""
    CELL = 64

    def __init__(self, tris):
        self.tris = tris
        self.grid = defaultdict(list)
        for i, t in enumerate(tris):
            xs, zs = [p[0] for p in t], [p[2] for p in t]
            for gx in range(int(min(xs)) // self.CELL, int(max(xs)) // self.CELL + 1):
                for gz in range(int(min(zs)) // self.CELL, int(max(zs)) // self.CELL + 1):
                    self.grid[gx, gz].append(i)

    def heights(self, x, z):
        """Every height of the ground at (x, z)."""
        return heights_at([self.tris[i] for i in self.grid.get((int(x) // self.CELL, int(z) // self.CELL), ())], x, z)

    def height(self, x, z, near_y, reach=40):
        best = None
        for i in self.grid.get((int(x) // self.CELL, int(z) // self.CELL), ()):
            a, b, c = self.tris[i]
            d = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2])
            if not d:
                continue
            l1 = ((b[2] - c[2]) * (x - c[0]) + (c[0] - b[0]) * (z - c[2])) / d
            l2 = ((c[2] - a[2]) * (x - c[0]) + (a[0] - c[0]) * (z - c[2])) / d
            l3 = 1 - l1 - l2
            if min(l1, l2, l3) < -1e-6:
                continue
            y = l1 * a[1] + l2 * b[1] + l3 * c[1]
            if abs(y - near_y) <= reach and (best is None or abs(y - near_y) < abs(best - near_y)):
                best = y
        return best

    def across(self, p, lat, step=2.0, limit=200):
        """How far the road runs from p to each side along lat (left, right), following its height."""
        out = []
        for sign in (-1, 1):
            d, y = 0.0, p[1]
            while d < limit:
                h = self.height(p[0] + lat[0] * sign * (d + step), p[2] + lat[1] * sign * (d + step), y, 15)
                if h is None:
                    break
                d += step
                y = h
            out.append(d)
        return out


def lateral(path, k):
    a, b = path[k - 1], path[(k + 1) % len(path)]
    t = (b[0] - a[0], b[2] - a[2])
    n = math.hypot(*t) or 1.0
    return (-t[1] / n, t[0] / n), (t[0] / n, t[1] / n)


def top_level_dls(seg7):
    """Segment 7 DLs no other DL calls: the fallback for courses without a section list."""
    starts, called = [0], set()
    for i, (w0, w1) in enumerate(seg7):
        if w0 >> 24 == 0xB8 and i + 1 < len(seg7):
            starts.append(i + 1)
        elif w0 >> 24 == 0x06:
            called.add((w1 & 0xFFFFFF) // 8)
    return [0x07000000 + s * 8 for s in starts if s not in called]


def heights_at(tris, x, z):
    """Heights at (x, z) of the triangles (three (x, y, z) points) that cover it."""
    out = []
    for (ax, ay, az), (bx, by, bz), (qx, qy, qz) in tris:
        d = (bx - ax) * (qz - az) - (qx - ax) * (bz - az)
        if abs(d) < 1e-9:
            continue
        u = ((x - ax) * (qz - az) - (qx - ax) * (z - az)) / d
        w = ((bx - ax) * (z - az) - (x - ax) * (bz - az)) / d
        if u >= 0 and w >= 0 and u + w <= 1:
            out.append(ay + u * (by - ay) + w * (qy - ay))
    return out


def section_points(c, sections):
    """Section id -> the (x, z) of its collision vertices, the ground a camera in that section stands over."""
    pts = defaultdict(set)
    for addr, surface, section, flags in sections:
        c.walk(addr, lambda st, vs, s=section: pts[s].update((v['pos'][0], v['pos'][2]) for v in vs))
    return {s: list(p) for s, p in pts.items()}


def render_class(state):
    rm = state['rmode'] or 0
    return 'xlu' if rm & 0x4000 else 'texedge' if rm & 0x1000 else 'opa'


def material(g, cache, tex, cls):
    """The glTF material for a texture (render_texture's) drawn in a render class, made once."""
    key = (tex['key'] if tex else None, cls)
    if key not in cache:
        alpha = {'opa': 'OPAQUE', 'texedge': 'MASK', 'xlu': 'BLEND'}[cls]
        if tex:
            img = Image.frombytes('RGBA', (tex['w'], tex['h']), mk64.decode_texture(tex))
            wrap = tuple('mirror' if f & 1 else 'clamp' if f & 2 else 'repeat' for f in (tex['cms'], tex['cmt']))
            name = 'mk64_tex%06X_%s' % (tex['key'][0] & 0xFFFFFF, cls)
            if tex['key'][0] >> 24 != 5:
                name = 'mk64_tex%08X_%s' % (tex['key'][0], cls)
            cache[key] = (name, g.material(name, png_bytes(img), wrap, alpha))
        else:
            name = 'mk64_shade_%s' % cls
            cache[key] = (name, g.material(name, alpha=alpha))
    return cache[key]


def prims_of(tris, matrix, to_m, both=False, white=False):
    """glTF primitives for (triangle of vertex dicts, texture, material index), one per material; matrix maps a vertex
    position (MK64 units) to world units. both: each triangle with both windings (the converter draws one face; MK64
    draws these with culling off). white: the vertex colors are normals (G_LIGHTING), so white."""
    by_mat = {}
    for t, tex, mat in tris:
        pts = [matrix(v['pos']) for v in t]
        nm = normal(pts) or (0.0, 1.0, 0.0)
        p = by_mat.setdefault(mat, {'pos': [], 'nrm': [], 'uv': [], 'col': [], 'tris': [], 'material': mat})
        base = len(p['pos'])
        for v, q in zip(t, pts):
            p['pos'].append(to_m(q))
            p['nrm'].append(nm)
            s, tt = v['uv']
            p['uv'].append(((s / 32.0) / tex['w'], (tt / 32.0) / tex['h']) if tex else (0.0, 0.0))
            p['col'].append((1.0, 1.0, 1.0, 1.0) if white else tuple(x / 255.0 for x in v['rgba']))
        p['tris'].append((base, base + 1, base + 2))
        if both:
            p['tris'].append((base, base + 2, base + 1))
    return list(by_mat.values())


def rot_y(deg):
    a = math.radians(deg)
    ca, sa = math.cos(a), math.sin(a)
    return lambda p: (p[0] * ca + p[2] * sa, p[1], -p[0] * sa + p[2] * ca)


def spawn_table(c, addr):
    """An ActorSpawnData table (s16 pos[3], u16 id) up to pos[0] = -32768: [(x, y, z, id)]."""
    out, off = [], addr & 0xFFFFFF
    while True:
        x, y, z, i = struct.unpack_from('>3hH', c.seg6, off)
        if x == -32768:
            return out
        out.append((x, y, z, i))
        off += 8


def trunk(prim, at, size, to_m):
    """An octagon of wall [radius, height] (MK64 units) round a point, facing out, added to a collision primitive."""
    (x, y, z), (rad, hgt) = at, size
    base = len(prim['pos'])
    for s in range(8):
        ang = 2 * math.pi * s / 8
        px, pz = x + rad * math.sin(ang), z + rad * math.cos(ang)
        prim['pos'] += [to_m((px, y - 5, pz)), to_m((px, y + hgt, pz))]
    for s in range(8):          # counter-clockwise seen from outside
        a0, a1 = base + 2 * s, base + 2 * ((s + 1) % 8)
        prim['tris'] += [(a0, a1, a1 + 1), (a0, a1 + 1, a0 + 1)]


def actors(g, c, cfg, ground, to_m, mats):
    """The course's actors (courses.toml [[course.<name>.actors]]) as meshes, as spawn_course_actors() places them.
    Billboards (MK64 turns them to face the camera) become two crossed copies, each with both faces, with a trunk
    of COL_WALL the size of the actor's bounding box; spinning signs and the Yoshi egg become MOVE_ objects."""
    if cfg.get('seg3'):
        c.load_seg3([t if isinstance(t, list) else (t, 0x800) for t in cfg['seg3']])
    walls = {'pos': [], 'tris': []}
    statics = defaultdict(list)     # name -> [primitives]
    movers = []
    for k, a in enumerate(cfg.get('actors', [])):
        name = a['name']
        dls = a.get('variants') or [a['dl'] if isinstance(a['dl'], list) else [a['dl']]]

        def material_for(st):
            tex = c.render_texture(st)
            return tex, material(g, mats, tex, render_class(st))[1]
        model = []                  # per variant: [(triangle of vertex dicts, texture, material)]
        for dl_set in dls:
            tris = []
            for dl in (dl_set if isinstance(dl_set, list) else [dl_set]):
                state = mk64.new_state(RENDER_MODE[a.get('render', 'texedge')])
                if a.get('tlut'):
                    state['tlut'] = a['tlut']
                if a.get('texture'):            # loaded by the render code (the piranha plant's first frame)
                    state['timg'] = (2, 1, a['texture'])
                    state['tmem'] = {0: state['timg']}
                c.walk(dl, lambda st, vs: tris.append(([dict(v) for v in vs],) + material_for(st)), state)
            model.append(tris)
        if a.get('spawns'):
            places = [(x, y, z, i, 0.0) for x, y, z, i in spawn_table(c, a['spawns'])]
        else:
            places = [(p[0], p[1], p[2], 0, p[3] if len(p) > 3 else 0.0) for p in a['at']]
        if a.get('ids') is not None:
            places = [p for p in places if p[3] in a['ids']]
        n = 0
        for x, y, z, i, yaw in places:
            tris = model[i if len(model) > 1 else 0] if (len(model) == 1 or i < len(model)) else None
            if not tris:
                continue
            if a.get('ground', True):
                h = ground.height(x, z, y, 60)
                if h is not None:
                    y = h
            n += 1
            white = a.get('lit', False)
            if a.get('spin') or a.get('circle'):
                node = g.empty('MOVE_%s%d' % (name, n), translation=to_m((x, y, z)))
                g.mesh('%s%d' % (name, n), prims_of(tris, lambda p: p, to_m, a.get('both', False), white), parent=node)
                if a.get('trunk'):          # its wall moves with it
                    col = {'pos': [], 'tris': []}
                    trunk(col, (0, 0, 0), a['trunk'], to_m)
                    g.mesh('COL_WALL_%s%d' % (name, n), [col], parent=node)
                movers.append((node, name, n, (x, y, z), a))
                continue
            turns = [yaw, yaw + 90.0] if a.get('billboard') else [yaw]
            for deg in turns:
                r = rot_y(deg)
                statics[name] += prims_of(tris, lambda p, r=r: tuple(q + o for q, o in zip(r(p), (x, y, z))),
                                          to_m, a.get('billboard') or a.get('both', False), white)
            if a.get('trunk'):
                trunk(walls, (x, y, z), a['trunk'], to_m)
        print('  actor %-16s %3d placed%s' % (name, n, ' (moving)' if a.get('spin') or a.get('circle') else ''))
    for name, prims in statics.items():
        merged = {}
        for p in prims:         # one primitive per material
            m = merged.setdefault(p['material'], {'pos': [], 'nrm': [], 'uv': [], 'col': [], 'tris': [],
                                                  'material': p['material']})
            base = len(m['pos'])
            for key in ('pos', 'nrm', 'uv', 'col'):
                m[key] += p[key]
            m['tris'] += [(i + base, j + base, k + base) for i, j, k in p['tris']]
        g.mesh('ACTOR_%s' % name, list(merged.values()))
    if walls['tris']:
        g.mesh('COL_WALL_actors', [walls])
    toml = []
    for node, name, n, (x, y, z), a in movers:
        if a.get('spin'):           # rot[1] += DEGREES(a) per tick: a turn in `spin` seconds
            times = [a['spin'] * q / 4 for q in range(5)]
            rots = [(0.0, math.sin(math.pi * q / 4), 0.0, math.cos(math.pi * q / 4)) for q in range(5)]
            g.animation('%s%d_spin' % (name, n), node, times, rotations=rots)
        else:                       # the Yoshi egg: round a circle while it spins
            cx, cz, radius, lap, turns = a['circle']
            keys = 48
            times = [lap * q / keys for q in range(keys + 1)]
            pos = [to_m((cx + radius * math.sin(2 * math.pi * q / keys), y, cz + radius * math.cos(2 * math.pi * q / keys)))
                   for q in range(keys + 1)]
            half = [-math.pi * turns * q / keys for q in range(keys + 1)]
            g.animation('%s%d_roll' % (name, n), node, times, translations=pos,
                        rotations=[(0.0, math.sin(h), 0.0, math.cos(h)) for h in half])
        toml.append('[movers.%s%d]\nmode = "loop"\n' % (name, n))
    return '\n'.join(toml)


def build(course, rom_path, out_dir, scale, ai_speed=True, double_walls=False):
    data = tomllib.load(open(os.path.join(HERE, 'courses.toml'), 'rb'))
    cfg = data['course'].get(course, {})
    battle = cfg.get('type') == 'battle'
    rom = mk64.load_rom(rom_path)
    c = mk64.Course(rom, mk64.COURSE_NAMES.index(course))
    c.load_segd(data['rom']['common_data'])
    for dl, a, r, g_, b in cfg.get('vertex_colors', []):
        c.set_vertex_colors(dl, a, r, g_, b)
    if battle:      # render_<course>() draws these DLs and course_generate_collision_mesh() collides with these
        dls, sections, path = cfg['draw'], [(dl, surface, 0, 0) for dl, surface in cfg['collision']], []
    else:
        dls = [cfg['dl_list']] if 'dl_list' in cfg else mk64.find_section_dls([c.seg9, c.seg6], len(c.seg6))
        if len(dls) < 4:
            dls = top_level_dls(c.seg7)
            print('  no section list: drawing the %d top-level packed DLs' % len(dls))
        sections = mk64.find_track_sections(c.seg6, len(c.seg7))
        path = mk64.find_track_path(c.seg6, cfg.get('path'))
    print('%s: %d vertices, %d section DLs, %d collision lists, %d path points, %d textures'
          % (course, len(c.vtx), len(dls), len(sections), len(path), len(c.texture_files)))

    def to_m(p):
        return (p[0] * scale, p[1] * scale, p[2] * scale)

    g = Glb('rush2-recomp mk64_import/mk64_track.py')
    sky_colors = sky(g, rom, data, cfg, course)

    # --- visible track ---
    groups = {}             # material key -> (texture, render class, [triangles])
    drawn = {}              # triangle (vertex ids) -> [material key, triangle, sections whose lists draw it]
    both_sides = set(cfg.get('double_sided', []))   # battle DLs render_<course>() draws with culling off
    mats = {}

    def visual(state, vs, section, both=False):
        key3 = tuple(id(v) for v in vs)
        if key3 in drawn:
            drawn[key3][2].add(section)
            return
        tex = c.render_texture(state)
        cls = render_class(state)
        groups.setdefault((tex['key'] if tex else None, cls), (tex, cls, []))
        drawn[key3] = [(tex['key'] if tex else None, cls), [dict(v) for v in vs], {section}]
        if both:
            drawn[key3 + ('back',)] = [(tex['key'] if tex else None, cls), [dict(v) for v in (vs[0], vs[2], vs[1])],
                                       {section}]
    for i, dl in enumerate(dls):
        # render_course_segments draws list (section - 1) * 4 + camera direction for the camera's collision section
        c.walk(dl, lambda st, vs, s=i // 4 + 1, b=dl in both_sides: visual(st, vs, s, b),
               mk64.new_state(RENDER_MODE[cfg.get('render_mode', 'opa')]))
    # A list also draws far parts of the course as they look from that section, some as cheap stand-ins: DK's start
    # straight seen from section 17 is one flat quad 8 units over the road, no collision. Taken with the rest it would
    # sit over the real piece. A stand-in is a triangle no section near it draws, lying over or under a triangle that
    # one does; far-only scenery with nothing under it (DK's jungle walls, its distant hills) stays.
    far = []
    if len(dls) >= 4 and not battle:
        near = section_points(c, sections)
        for mat, tri, secs in drawn.values():
            cx, cz = (sum(v['pos'][0] for v in tri) / 3, sum(v['pos'][2] for v in tri) / 3)
            dist = {s: min((x - cx) ** 2 + (z - cz) ** 2 for x, z in pts) ** 0.5 for s, pts in near.items()}
            if dist and min(dist.get(s, 1e9) for s in secs) > min(dist.values()) + STAND_IN_REACH:
                far.append((mat, tri))
            else:
                groups[mat][2].append(tri)
    else:
        for mat, tri, secs in drawn.values():
            groups[mat][2].append(tri)
    kept = [[v['pos'] for v in t] for v in groups.values() for t in v[2]]
    dropped = solid = 0
    for mat, tri in far:
        cx, cy, cz = (sum(v['pos'][i] for v in tri) / 3 for i in range(3))
        if any(abs(h - cy) < STAND_IN_GAP for h in heights_at(kept, cx, cz)):
            dropped += 1
            solid += not all(v['flag'] == 4 for v in tri)
        else:
            groups[mat][2].append(tri)
    if dropped:
        print('  %d far-section stand-in triangles left out (%d with collision)' % (dropped, solid))
    groups = {k: v for k, v in groups.items() if v[2]}
    for key, (tex, cls, tris) in sorted(groups.items(), key=lambda kv: str(kv[0])):
        name, mat = material(g, mats, tex, cls)
        g.mesh('TRACK_%s' % name, prims_of([(t, tex, mat) for t in tris], lambda p: p, to_m))
        print('  visual %-28s %5d triangles' % (name, len(tris)))

    # --- collision ---
    floors, walls = defaultdict(list), []
    seen = set()
    for addr, surface, section, flags in sections:
        def collide(state, vs, surface=surface):
            key3 = tuple(id(v) for v in vs)
            # generate_collision_mesh() leaves out a triangle whose vertices all have flag 4; the battle courses'
            # (course_generate_collision_mesh with the DL itself) take every one
            if key3 in seen or (not battle and all(v['flag'] == 4 for v in vs)):
                return
            seen.add(key3)
            pts = [v['pos'] for v in vs]
            nm = normal(pts)
            if nm is None:
                return
            kind = SURFACES.get(surface, 'ROAD')
            if abs(nm[1]) < 0.5 or kind == 'WALL':
                walls.append(pts)
            else:
                floors[kind].append(pts if nm[1] >= 0 else [pts[0], pts[2], pts[1]])
        c.walk(addr, collide)
    road = [t for kind in ('ROAD', 'FASTDIRT', 'DIRT', 'GRASS') for t in floors.get(kind, [])]
    ground = Ground(road)
    low = {}
    for t in walls:
        for p in t:
            low[p[0], p[2]] = min(low.get((p[0], p[2]), p[1]), p[1])
    raise_by = cfg.get('wall_height', 0.0) / scale

    def top(p):     # a wall vertex above another at the same x, z is on the top edge
        return (p[0], p[1] + raise_by, p[2]) if p[1] > low[p[0], p[2]] + 1 else p

    def floor_side(t, nm):
        """+1 or -1: the side of a wall with floor at its foot (an arena has no path to face). A block's top is
        level with its walls' top edges, so it doesn't count; neither side or both: 0."""
        h = math.hypot(nm[0], nm[2]) or 1.0
        cx, cz = (sum(p[i] for p in t) / 3 for i in (0, 2))
        lo, hi = min(p[1] for p in t), max(p[1] for p in t)
        has = [any(lo - 60 <= y <= hi - 2 for y in ground.heights(cx + nm[0] / h * s * 6, cz + nm[2] / h * s * 6))
               for s in (1, -1)]
        return 0 if has[0] == has[1] else 1 if has[0] else -1
    centroids = [tuple(sum(p[i] for p in t) / 3 for i in range(3)) for t in road]
    wall_prim = {'pos': [], 'tris': []}
    guessed = 0
    for t in walls:
        nm = normal(t)
        cx, cy, cz = (sum(p[i] for p in t) / 3 for i in range(3))
        if path:
            k = min(range(len(path)), key=lambda i: (path[i][0] - cx) ** 2 + (path[i][2] - cz) ** 2
                    + 4 * (path[i][1] - cy) ** 2)
            flip = nm[0] * (path[k][0] - cx) + nm[2] * (path[k][2] - cz) < 0
        else:
            side = floor_side(t, nm)
            if side == 0:       # face the nearest floor
                guessed += 1
                f = min(centroids, key=lambda q: (q[0] - cx) ** 2 + (q[2] - cz) ** 2 + 4 * (q[1] - cy) ** 2)
                side = 1 if nm[0] * (f[0] - cx) + nm[2] * (f[2] - cz) >= 0 else -1
            flip = side < 0
        if flip:
            t = [t[0], t[2], t[1]]
        t = [top(p) for p in t]
        base = len(wall_prim['pos'])
        wall_prim['pos'] += [to_m(p) for p in t]
        wall_prim['tris'].append((base, base + 1, base + 2))
        if double_walls:
            wall_prim['tris'].append((base, base + 2, base + 1))
    for kind, tris in sorted(floors.items()):
        prim = {'pos': [to_m(p) for t in tris for p in t], 'tris': [(3 * i, 3 * i + 1, 3 * i + 2) for i in range(len(tris))]}
        g.mesh('COL_%s_mk64' % kind, [prim])
    if walls:
        g.mesh('COL_WALL_mk64', [wall_prim])
    print('  collision: %s, %d wall triangles%s' % (', '.join('%d %s' % (len(v), k) for k, v in floors.items()), len(walls),
                                                   ' (%d facing the nearest floor)' % guessed if guessed else ''))

    # A sheet under the course in 64 m squares: kill_below (Lakitu's falls) 150 units under the lowest road, or
    # kill_y, the height a battle course's fall starts at (D_8015F8E4: a number, or "min": the lowest collision - 10).
    y_kill = None
    if cfg.get('kill_below'):
        y_kill = min(p[1] for t in road for p in t) - 150
    elif cfg.get('kill_y') == 'min':
        y_kill = min(p[1] for t in road + walls for p in t) - 10
    elif 'kill_y' in cfg:
        y_kill = cfg['kill_y']
    if y_kill is not None:
        xs = [p[0] for t in road for p in t]
        zs = [p[2] for t in road for p in t]
        x0, z0 = min(xs) * scale - 100, min(zs) * scale - 100
        cell = 64.0
        nx = int(math.ceil((max(xs) * scale + 100 - x0) / cell))
        nz = int(math.ceil((max(zs) * scale + 100 - z0) / cell))
        kill = {'pos': [(x0 + i * cell, y_kill * scale, z0 + j * cell) for i in range(nx + 1) for j in range(nz + 1)],
                'tris': []}
        for i in range(nx):
            for j in range(nz):
                a, b = i * (nz + 1) + j, (i + 1) * (nz + 1) + j
                kill['tris'] += [(a, a + 1, b + 1), (a, b + 1, b)]
        g.mesh('COL_KILL_void', [kill])

    movers = actors(g, c, cfg, ground, to_m, mats)

    def marker(name, p, fwd, half):
        """An Empty facing fwd (x, z): the race direction is its -Z (Blender's green Y arrow), X scaled to half."""
        z = (-fwd[0], 0.0, -fwd[1])
        x = (z[2], 0.0, -z[0])                  # y x z
        g.empty(name, [x[0] * half, x[1] * half, x[2] * half, 0, 0, 1, 0, 0, z[0], z[1], z[2], 0, p[0], p[1], p[2], 1])

    if battle:
        # --- starts and pickups ---
        # The 4 player battle spawns (spawn_players_4p_battle): each (x, z) at the height of the ground under
        # spawn_y, facing yaw (MK64: 0 faces +z, -0x4000 faces +x).
        for i, (x, z, yaw) in enumerate(cfg['spawns']):
            h = ground.height(x, z, cfg['spawn_y'], 200)
            a = yaw / 65536.0 * 2 * math.pi
            marker('START_%d' % (i + 1), to_m((x, (h if h is not None else cfg['spawn_y']) + 3, z)),
                   (-math.sin(a), math.cos(a)), 3.0)
        # Item boxes (spawn_all_item_boxes: on the ground under pos + 10) become Rush 2049's pickups, a weapon each
        # in turn and every third a power-up.
        kinds = ['MISSILE', 'ROCKET', 'POWERUP', 'MINE', 'GATLING', 'POWERUP', 'GRENADE', 'CANNON', 'POWERUP',
                 'SONIC', 'RAM', 'POWERUP']
        boxes = spawn_table(c, cfg['item_boxes'])
        for i, (x, y, z, _) in enumerate(boxes):
            h = ground.height(x, z, y + 10, 100)
            g.empty('PICKUP_%s.%03d' % (kinds[i % len(kinds)], i),
                    translation=to_m((x, (h if h is not None else y) + 5, z)))
        print('  %d starts, %d pickups' % (len(cfg['spawns']), len(boxes)))
    else:
        # --- AI path ribbon and gates ---
        rungs, centers, halves = [], [], []
        for k, p in enumerate(path):
            lat, _ = lateral(path, k)
            left, right = ground.across(p, lat)
            if left + right < 20:            # nothing measured (the path point a little off the road): keep it as is
                left = right = 50
            mid = (right - left) / 2
            cp = (p[0] + lat[0] * mid, p[1], p[2] + lat[1] * mid)
            h = ground.height(cp[0], cp[2], p[1])
            cp = (cp[0], h if h is not None else p[1], cp[2])
            half = (left + right) / 2
            centers.append(cp)
            halves.append(half)
            ends = []
            for sign in (-1, 1):
                q = (cp[0] + lat[0] * sign * 0.75 * half, cp[2] + lat[1] * sign * 0.75 * half)
                y = ground.height(q[0], q[1], cp[1], 15)
                ends.append((q[0], y if y is not None else cp[1], q[1]))
            rungs.append(ends)
        n = len(rungs)
        rib = {'pos': [to_m(e) for r in rungs for e in r], 'nrm': [(0.0, 1.0, 0.0)] * (2 * n), 'tris': []}
        for i in range(n):
            j = (i + 1) % n
            rib['tris'] += [(2 * i, 2 * j, 2 * i + 1), (2 * i + 1, 2 * j, 2 * j + 1)]
        g.mesh('AIPATH_main', [rib])
        widths = sorted(halves)
        print('  AI path: %d rungs, road %.0f-%.0f units wide (median %.0f)'
              % (n, 2 * widths[0], 2 * widths[-1], 2 * widths[n // 2]))

        lap = [0.0]
        for k in range(1, n):
            lap.append(lap[-1] + math.dist(path[k], path[k - 1]))
        total = lap[-1] + math.dist(path[0], path[-1])
        count = cfg.get('checkpoints', 6)
        gates = [0] + [min(range(n), key=lambda k: abs(lap[k] - total * i / (count + 1))) for i in range(1, count + 1)]
        for i, k in enumerate(gates):
            _, fwd = lateral(path, k)
            marker('FINISH' if i == 0 else 'CHECKPOINT_%d' % i, to_m(centers[k]), fwd, halves[k] * scale)
        for k, mph in (cfg.get('ai_speed', []) if ai_speed else []):
            p = to_m(centers[k])
            g.empty('AISPEED_%d' % mph, [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, p[0], p[1] + 1.0, p[2], 1])
        print('  lap %.0f units (%.2f km), %d checkpoints' % (total, total * scale / 1000, count))

    # --- files ---
    title = cfg.get('name', course.replace('_', ' ').title())
    track_dir = os.path.join(out_dir, 'tracks', course)
    os.makedirs(track_dir, exist_ok=True)
    with open(os.path.join(track_dir, 'track.glb'), 'wb') as f:
        f.write(g.bytes())
    fog = '#%02x%02x%02x' % sky_colors[1]
    with open(os.path.join(track_dir, 'track.toml'), 'w', newline='\n') as f:
        f.write('[track]\nname        = "%s"\n%sauthor      = "Nintendo (Mario Kart 64), imported by mk64_import"\n'
                'version     = "0.1"\ndescription = "Mario Kart 64\'s %s, converted from the MK64 ROM."\n\n'
                '[look]\nfog_color = "%s"\n\n%s[menu]\nlogo = "logo.png"\n%s'
                % (title, 'type        = "battle"\n' if battle else '', title.replace('"', ''), fog,
                   '[surfaces.fastdirt]\nlike = "dirt"\ndrag = 0\n\n' if 'FASTDIRT' in floors else '',
                   '\n' + movers if movers else ''))
    logo(title).save(os.path.join(track_dir, 'logo.png'))
    print('wrote', track_dir)


def sky_colors(rom, data, course):
    """The course's skybox colors from sTopSkyBoxColors / sBottomSkyBoxColors (skybox_and_splitscreen.c
    course_set_skybox_colours): (zenith, horizon above, horizon below, nadir) as RGB 0-255. The tables are s16 per
    channel and the vertex color takes the low byte (Koopa Troopa Beach's 1688 is 152)."""
    common = data['rom']
    out = []
    for table in (common['sky_top'], common['sky_bottom']):
        o = table - common['main_vram_to_rom'] + 12 * mk64.COURSE_NAMES.index(course)
        v = [x & 0xFF for x in struct.unpack_from('>6h', rom, o)]
        out += [tuple(v[0:3]), tuple(v[3:6])]
    return out


def sky(g, rom, data, cfg, course):
    """SKY: a dome around the origin shaded with the course's skybox colors, and its stars. MK64 draws the top
    gradient from the screen's top edge down to the horizon and the bottom one from the horizon down; on the dome
    the top color is at SKY_TOP_DEG and above, the bottom table's colors from the horizon down to SKY_BOTTOM_DEG.
    Returns the colors (the horizon color is the track's fog color)."""
    colors = sky_colors(rom, data, course)
    zenith, horizon, below, nadir = ([x / 255.0 for x in col] for col in colors)
    mat = g.material('mk64_sky')
    prim = {'pos': [], 'uv': [], 'col': [], 'tris': [], 'material': mat}
    rings = [-90, -SKY_BOTTOM_DEG, -0.5, 0, 10, SKY_TOP_DEG, 55, 75, 90]
    segs = 24

    def shade(el):
        if el >= 0:
            a, b, f = horizon, zenith, min(el / SKY_TOP_DEG, 1.0)
        else:
            a, b, f = below, nadir, min(-el / SKY_BOTTOM_DEG, 1.0)
        return tuple(a[i] + (b[i] - a[i]) * f for i in range(3)) + (1.0,)

    def inward(tri):
        a, b, cc = (prim['pos'][i] for i in tri)
        n = normal((a, b, cc))
        mid = [(a[i] + b[i] + cc[i]) / 3 for i in range(3)]
        return tri if n and sum(n[i] * mid[i] for i in range(3)) < 0 else (tri[0], tri[2], tri[1])
    for el in rings:
        for s in range(segs):
            az = 2 * math.pi * s / segs
            e = math.radians(el)
            prim['pos'].append((SKY_RADIUS * math.cos(e) * math.sin(az), SKY_RADIUS * math.sin(e),
                                SKY_RADIUS * math.cos(e) * math.cos(az)))
            prim['col'].append(shade(el))
            prim['uv'].append((0.0, 0.0))
    for r in range(len(rings) - 1):
        for s in range(segs):
            a, b = r * segs + s, r * segs + (s + 1) % segs
            for tri in ((a, b, b + segs), (a, b + segs, a + segs)):
                if len({tuple(round(x, 3) for x in prim['pos'][i]) for i in tri}) == 3:
                    prim['tris'].append(inward(tri))
    prims = [prim]

    stars = cfg.get('stars')
    if stars:
        common = data['rom']
        table = mk64.read_stars(rom, data['stars'][stars], common['main_vram_to_rom'])
        segd = mk64.mio0(rom, common['common_data'])
        off = common['star_texture'] & 0xFFFFFF
        tex = {'w': 16, 'h': 16, 'fmt': 4, 'siz': 0, 'data': segd[off:off + 128]}
        img = Image.frombytes('RGBA', (16, 16), mk64.decode_texture(tex))
        img.putdata([(255, 255, 255, a) for _, _, _, a in img.getdata()])     # SHADE color, TEXEL0 alpha
        smat = g.material('mk64_star', png_bytes(img), ('clamp', 'clamp'), 'MASK')
        sp = {'pos': [], 'uv': [], 'col': [], 'tris': [], 'material': smat}
        d = SKY_RADIUS * 0.95
        lo, hi = min(y for _, y, _ in table), max(y for _, y, _ in table)
        for rot, y, pct in table:
            az = -rot / 65536.0 * 2 * math.pi
            el = math.radians(STAR_ELEVATION[0] + (y - lo) / max(hi - lo, 1) * (STAR_ELEVATION[1] - STAR_ELEVATION[0]))
            f = (math.cos(el) * math.sin(az), math.sin(el), math.cos(el) * math.cos(az))
            rt = (f[2], 0.0, -f[0])
            n = math.hypot(rt[0], rt[2])
            rt = (rt[0] / n, 0.0, rt[2] / n)
            up = (f[1] * rt[2] - f[2] * rt[1], f[2] * rt[0] - f[0] * rt[2], f[0] * rt[1] - f[1] * rt[0])
            h = d * math.tan(math.radians(16 * pct / 100.0 * STAR_DEG_PER_PX / 2))
            base = len(sp['pos'])
            for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                sp['pos'].append(tuple(f[i] * d + rt[i] * su * h + up[i] * sv * h for i in range(3)))
                sp['uv'].append(((su + 1) / 2, (1 - sv) / 2))
                sp['col'].append((1.0, 1.0, 1.0, 1.0))
            for tri in ((base, base + 1, base + 2), (base, base + 2, base + 3)):
                a, b, cc = (sp['pos'][i] for i in tri)
                nn = normal((a, b, cc))
                sp['tris'].append(tri if sum(nn[i] * f[i] for i in range(3)) < 0 else (tri[0], tri[2], tri[1]))
        prims.append(sp)
        print('  sky: %d stars' % len(table))
    g.mesh('SKY', prims)
    return colors


RAINBOW = [(255, 64, 64), (255, 160, 32), (255, 240, 48), (64, 230, 64), (48, 160, 255), (176, 96, 255)]


def logo(title):
    """The name in rainbow letters with a dark outline, 512 x 128 (4:1; the game fits it), sized to fit."""
    img = Image.new('RGBA', (512, 128), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    text = title.upper()
    for size in range(84, 20, -2):
        try:
            font = ImageFont.truetype('arialbd.ttf', size)
        except OSError:
            font = ImageFont.load_default()
            break
        if sum(d.textlength(ch, font=font) for ch in text) + 12 <= 500:
            break
    widths = [d.textlength(ch, font=font) for ch in text]
    x = (512 - sum(widths)) / 2
    for i, ch in enumerate(text):
        d.text((x, 64), ch, font=font, fill=RAINBOW[i % len(RAINBOW)] + (255,), anchor='lm', stroke_width=5,
               stroke_fill=(16, 8, 48, 255))
        x += widths[i]
    return img


def write_mod_json(out_dir):
    with open(os.path.join(out_dir, 'mod.json'), 'w', newline='\n') as f:
        json.dump({'game_id': 'rush2', 'id': os.path.basename(os.path.normpath(out_dir)),
                   'display_name': 'Mario Kart 64 courses',
                   'description': "Mario Kart 64's race courses and battle courses as custom tracks and arenas, "
                                  'converted from the MK64 ROM by mk64_import/mk64_track.py.',
                   'short_description': 'Mario Kart 64 courses', 'version': '0.1.0',
                   'authors': ['Nintendo', 'mk64_import'], 'minimum_recomp_version': '0.0.0'}, f, indent=4)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('courses', nargs='+', choices=mk64.COURSE_NAMES + ['all'],
                    help='course names, or all (the race courses and the battle arenas)')
    ap.add_argument('--rom', default=os.environ.get('MK64_ROM', r'E:\Emulation\roms\n64\Mario Kart 64.n64'))
    ap.add_argument('--out', default=os.path.join(ROOT, 'build', 'mods', 'mk64'), help='mod folder')
    ap.add_argument('--scale', type=float, default=0.2, help='meters per MK64 unit')
    ap.add_argument('--no-ai-speed', action='store_true', help="leave out courses.toml's AISPEED_ markers")
    ap.add_argument('--double-walls', action='store_true', help='give walls both faces (tests the converter)')
    a = ap.parse_args()
    courses = mk64.COURSE_NAMES if 'all' in a.courses else a.courses
    for course in courses:
        build(course, a.rom, a.out, a.scale, not a.no_ai_speed, a.double_walls)
    write_mod_json(a.out)


if __name__ == '__main__':
    main()
