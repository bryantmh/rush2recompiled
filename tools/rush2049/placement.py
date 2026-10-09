"""Track object placement files of Rush 2 and Rush 2049, plus 2049's PATH/PTHD animated-object paths.

Findings and evidence: docs/rush2049_research/placement.md.

Run `python placement.py` for the self-check: it parses all 12 Rush 2 placement files (assets 0x3F-0x4A) and all
2049 placement files (files 120-138), lists every referenced object name, says how the game classifies it and
whether its model resolves in the files that are loaded with that track, then converts the six 2049 race tracks.
`python placement.py --verbose` also prints every record.

Formats (all big-endian):

Rush 2 placement file (asset 0x3F + track)
    u32 count (always 1)                    directory of placement trees, relocated by func_800A5110
    count x { u32 offset, char name[16] }   offset of the first record, name = track prefix (0x800C182C table)
    records, 100 (0x64) bytes each:
        +0x00 char name[16]
        +0x10 f32 m[9]       3x3 rotation/scale, row-major, as used by the node (node+4 points here)
        +0x34 f32 pos[3]     world position (made parent-relative in place for breakables, func_80081790)
        +0x40 u32 flags      copied to scene node +0 (always 0x40 in the stock files)
        +0x44 s16 next       index of the next sibling record, -1 = none
        +0x46 s16 child      index of the first child record, -1 = none
        +0x48 u32 0
        +0x4C f32 bbmin[3], bbmax[3]   culling box, only registered for top-level records (func_8007FC80)

Rush 2049 placement file (file 120 + track)
    u32 dir_offset, u32 chunk_count (4)
    WHDR: the Rush 2 header above (count, {offset, name}), offset relative to the file
    WOBJ: records, 104 (0x68) bytes: the Rush 2 record with an extra s32 at +0x4C (dynamic-object id, -1 = static);
          the box moves to +0x50
    GTLD: u32 list of dynamic-object ids, grouped per GDAT leaf
    GDAT: 28-byte quadtree nodes over x/z: s16 parent, u16 kind (1 = inner, 0x10 = leaf), f32 x0, x1, z0, z1,
          inner: s16 child[4]; leaf: u16 count, u16 first (into GTLD), u32 0
    chunk directory: {tag, offset, count}

Rush 2049 track geometry (files 101-119) PTHD chunk: 36-byte path headers
    +0x00 char name[16]  object type (2049 dynamic-object table 0x80117530), e.g. TROLLEY, TRAPDOOR2
    +0x10 u32 flags      path behaviour (see PTHD_FLAGS)
    +0x14 s16 count      number of PATH nodes
    +0x16 s16 trigger    trigger / collision-group id (matches collision polygon flag bits 0xF800 >> 11)
    +0x18 u32 0
    +0x1C u32 offset     file offset of the first PATH node
    +0x20 s32 id         dynamic-object id (also used in GTLD), -1 = none
PATH chunk: 68-byte nodes
    +0x00 f32 pos[3]     node position
    +0x0C f32 dir[3]     unit direction towards the next node
    +0x18 f32 scale[3]
    +0x24 f32 quat[4]    orientation (x, y, z, w)
    +0x34 f32 dist       segment length
    +0x38 f32 time       segment duration in seconds (or the pause length)
    +0x3C f32 speed      speed at this node (units/s); speed is interpolated linearly along the segment
    +0x40 u32 flags      see NODE_FLAGS
"""
import math
import struct
import sys

import roms

R2_TRACKS = ['VEGAS', 'NYONE', 'HAWAII', 'NYTWO', 'ALCATRAZ', 'LA', 'SEATTLE', 'HALFPIPE', 'CRASH', 'PIPE',
             'ATARI', 'STUNT1']
R2_REC = 0x64
R49_REC = 0x68

PTHD_FLAGS = {                       # func_800B2DF8 (spawn), func_800C0AC0 (update), func_800C04CC (move)
    0x0001: 'ping-pong at the ends',
    0x0002: 'loop: wrap from the last node to node 0 (else restart at node 0, func_800C085C)',
    0x0004: 'present in forward races',
    0x0008: 'present in backward races (0x80152570)',
    0x0010: 'creation parameter passed to func_800ABCC8 [I]',
    0x0020: 'carries collision group `trigger`: its polygons move with the object (func_800BF838)',
    0x0040: 'waits for a trigger before moving',
    0x0080: 'stops at each end until the next trigger',
    0x0100: 'halted (runtime)',
    0x0200: 'trigger received (runtime)',
    0x0400: 'trigger pending (runtime)',
    0x0800: 'trigger switches collision group `trigger` (func_800B2CB4 / func_800B2D20)',
    0x1000: 'TRIGGER pad that fires group `trigger`',
    0x2000: 'linked trigger [I]',
    0x4000: 'one-shot: reverse and halt at the end',
    0x8000: 'silent',
}
NODE_FLAGS = {
    0x00000001: 'spawn an object here at race start',
    0x00000002: 'no translation on this segment (rotation only, e.g. rotors)',
    0x00000004: 'with 0x10: no rotation update',
    0x00000008: 'constant speed on this segment',
    0x00000010: 'with 0x4: no rotation update',
    0x00000040: 'halt at this node and wait for the next trigger',
    0x00001000: 'already spawned (runtime)',
    0x01000000: 'battle mode only [I]',
    0x10000000: 'first node; constant speed',
}


def be(fmt, data, off=0):
    return struct.unpack_from('>' + fmt, data, off)


def cstr(b):
    return b.split(b'\0')[0].decode('latin1')


# ----------------------------------------------------------------------------------------------------------------
# Records


class Record:
    __slots__ = ('index', 'name', 'm', 'pos', 'flags', 'next', 'child', 'pad', 'dyn_id', 'bbox', 'parent', 'depth')

    def __init__(self, index, name, m, pos, flags, nxt, child, pad, dyn_id, bbox):
        self.index, self.name, self.m, self.pos = index, name, list(m), list(pos)
        self.flags, self.next, self.child, self.pad, self.dyn_id, self.bbox = flags, nxt, child, pad, dyn_id, list(bbox)
        self.parent, self.depth = -1, 0

    def __repr__(self):
        return '<%d %s pos=(%.0f,%.0f,%.0f) flags=%x next=%d child=%d id=%d>' % (
            self.index, self.name, *self.pos, self.flags, self.next, self.child, self.dyn_id)


def _link(records, root=0):
    """Fill parent/depth by walking the sibling/child tree like func_80081790."""
    seen = set()

    def walk(i, parent, depth):
        while i >= 0:
            if i in seen or i >= len(records):
                raise ValueError('placement tree loops or points outside the file (record %d)' % i)
            seen.add(i)
            r = records[i]
            r.parent, r.depth = parent, depth
            if r.child >= 0:
                walk(r.child, i, depth + 1)
            i = r.next
    if records:
        walk(root, -1, 0)
    return len(seen)


def parse_r2(data):
    """Rush 2 placement file -> (entries [(name, first_record)], [Record])."""
    count = be('I', data)[0]
    entries, first = [], None
    for k in range(count):
        off, = be('I', data, 4 + k * 0x14)
        name = cstr(data[8 + k * 0x14:0x18 + k * 0x14])
        if first is None:
            first = off
        entries.append((name, (off - first) // R2_REC))
    n = (len(data) - first) // R2_REC
    recs = []
    for i in range(n):
        o = first + i * R2_REC
        v = be('9f3fIhhI6f', data, o + 16)
        recs.append(Record(i, cstr(data[o:o + 16]), v[0:9], v[9:12], v[12], v[13], v[14], v[15], -1, v[16:22]))
    _link(recs, entries[0][1] if entries else 0)
    return entries, recs


def write_r2(entries, recs):
    """Inverse of parse_r2 (one tree per entry, records written in index order)."""
    hdr = struct.pack('>I', len(entries))
    base = 4 + 0x14 * len(entries)
    for name, first in entries:
        hdr += struct.pack('>I', base + first * R2_REC) + name.encode('latin1').ljust(16, b'\0')
    out = bytearray(hdr)
    for r in recs:
        out += r.name.encode('latin1').ljust(16, b'\0')
        out += struct.pack('>9f3fIhhI6f', *r.m, *r.pos, r.flags, r.next, r.child, r.pad, *r.bbox)
    return bytes(out)


def parse_49(data):
    """Rush 2049 placement file -> dict(entries, records, gtld, gdat, chunks)."""
    dir_off, nchunks = be('II', data)
    chunks = {}
    for k in range(nchunks):
        tag = data[dir_off + k * 12:dir_off + k * 12 + 4].decode('latin1')
        chunks[tag] = be('II', data, dir_off + k * 12 + 4)
    wo, _ = chunks['WHDR']
    count = be('I', data, wo)[0]
    entries = []
    obj_off, obj_n = chunks['WOBJ']
    for k in range(count):
        off, = be('I', data, wo + 4 + k * 0x14)
        entries.append((cstr(data[wo + 8 + k * 0x14:wo + 0x18 + k * 0x14]), (off - obj_off) // R49_REC))
    recs = []
    for i in range(obj_n):
        o = obj_off + i * R49_REC
        v = be('9f3fIhhIi6f', data, o + 16)
        recs.append(Record(i, cstr(data[o:o + 16]), v[0:9], v[9:12], v[12], v[13], v[14], v[15], v[16], v[17:23]))
    _link(recs, entries[0][1] if entries else 0)
    go, gn = chunks['GTLD']
    gtld = list(be('%di' % gn, data, go))
    do, dn = chunks['GDAT']
    gdat = []
    for k in range(dn):
        o = do + k * 28
        parent, kind = be('hH', data, o)
        box = be('4f', data, o + 4)
        if kind & 0x10:
            cnt, start = be('HH', data, o + 20)
            gdat.append(dict(parent=parent, kind=kind, box=box, ids=gtld[start:start + cnt]))
        else:
            gdat.append(dict(parent=parent, kind=kind, box=box, children=list(be('4h', data, o + 20))))
    return dict(entries=entries, records=recs, gtld=gtld, gdat=gdat, chunks=chunks)


def parse_paths(geometry):
    """PTHD/PATH of a 2049 track geometry file -> [dict(name, flags, trigger, id, nodes=[dict])]."""
    c = roms.Rush2049.chunks(None, geometry)
    if 'PTHD' not in c:
        return []
    o, n = c['PTHD']
    paths = []
    for k in range(n):
        h = o + k * 36
        flags, count, trigger, zero, off, dyn_id = be('IhhIIi', geometry, h + 16)
        nodes = []
        for j in range(count):
            p = off + j * 68
            v = be('16fI', geometry, p)
            nodes.append(dict(pos=v[0:3], dir=v[3:6], scale=v[6:9], quat=v[9:13], dist=v[13], time=v[14],
                              speed=v[15], flags=v[16]))
        paths.append(dict(name=cstr(geometry[h:h + 16]), flags=flags, trigger=trigger, id=dyn_id, nodes=nodes))
    return paths


# ----------------------------------------------------------------------------------------------------------------
# Model name tables


def r2_model_names(data):
    """Named objects of a Rush 2 model container (header word 1 = table, word 4 = count, 0x18-byte entries)."""
    if len(data) < 40:
        return {}
    h = be('10I', data)
    o, n = h[1], h[4]
    if n == 0 or n > 5000 or o + n * 0x18 > len(data):
        return {}
    out = {}
    for k in range(n):
        e = o + k * 0x18
        out[cstr(data[e:e + 16])] = be('h', data, e + 0x14)[0]      # name -> behaviour id (+0x14)
    return out


def r49_model_names(data):
    """OBHD names of a 2049 chunked model file. OBHD names are char[16] (compared on 15 chars, func_80095120)."""
    try:
        c = roms.Rush2049.chunks(None, data)
    except Exception:
        return set()
    if 'OBHD' not in c:
        return set()
    o, n = c['OBHD']
    return {cstr(data[o + k * 0x58:o + k * 0x58 + 16]) for k in range(n)}


# ----------------------------------------------------------------------------------------------------------------
# Rush 2 classification (func_800815DC)


class R2Rules:
    """Name tables Rush 2's placement walker uses, read from the ROM."""

    def __init__(self, r2):
        s = lambda a: r2.s(a) if a else None
        self.sounds = [(s(r2.w(0x800C5330 + k * 8)), r2.w(0x800C5334 + k * 8)) for k in range(23)]
        self.markers = [(s(r2.w(0x800C53E8 + k * 8)), r2.w(0x800C53EC + k * 8)) for k in range(3)]
        self.breakables = []
        a = 0x800C5400
        while a < 0x800C55F8:
            self.breakables.append((s(r2.w(a)), s(r2.w(a + 4)), r2.h(a + 8), r2.h(a + 10)))
            a += 12

    def classify(self, name):
        """-> (type, model_name or None, extra). Types: 0 model, 1 breakable, 2 '_F' only, 3 '_B' only,
        4 breakable flag 2, 5 sound emitter / MARKER / TIME / COLLISION (no node)."""
        for n, snd in self.sounds:                       # exact match (strcmp)
            if name == n:
                return 5, None, snd
        for n, val in self.markers:                      # prefix match
            if name.startswith(n):
                return 5, None, val
        for n, debris, fl, extra in self.breakables:     # prefix match, first wins
            if name.startswith(n):
                t = 1
                if fl & 2:
                    t = 4
                if fl & 1:
                    if '_B' in name:
                        t = 3
                    if '_F' in name:
                        t = 2
                return t, debris, extra
        return 0, name, 0


# ----------------------------------------------------------------------------------------------------------------
# Rush 2049 dynamic-object table (0x80117530, 122 x 0x30)


class R49Types:
    KINDS = {0: 'sign/in-place anim (sub-kind)', 2: 'knock-over prop', 4: 'path follower', 5: 'mine',
             6: 'coin', 7: 'misc (collision, flag, glass...)'}

    def __init__(self, r49):
        self.types = []
        base = 0x80117530
        for k in range(122):
            o = base + k * 0x30 - r49.MAIN_VRAM
            w = struct.unpack('>12I', r49.main[o:o + 0x30])
            name = self._s(r49, w[0])
            model = self._s(r49, w[1]) if w[1] else None
            self.types.append(dict(index=k, name=name, model=model, init=w[2], update=w[3], flags=w[4],
                                   anim=(w[5] >> 16) - 0x10000 if w[5] >> 31 else w[5] >> 16,
                                   kind=(w[5] >> 8) & 0xFF, sub=w[5] & 0xFF,
                                   param=struct.unpack('>f', struct.pack('>I', w[6]))[0],
                                   moving=bool(w[4] & 0x8000)))

    @staticmethod
    def _s(r49, a):
        o = a - r49.MAIN_VRAM
        return r49.main[o:r49.main.index(b'\0', o)].decode('latin1')

    def classify(self, name):
        """Same as func_800ABCC8: the first entry whose name is a prefix of the record name."""
        for t in self.types:
            if name.startswith(t['name']):
                return t
        return None


# ----------------------------------------------------------------------------------------------------------------
# Conversion 2049 -> Rush 2

# 2049 dynamic type -> Rush 2 placement name prefix (resolved by Rush 2's own breakable table to a model in asset
# 0x14 or 0x12, which Rush 2 always loads for a race). Debris models FENCEO1, GASIGNO1, ... live in some Rush 2 track
# geometries only, so FENCE etc. are not mapped.
R49_TO_R2 = {'YIELDHIT': 'YIELDHIT', 'SHATPANE': 'SHATPANE', 'FLAG2': 'FLAG2', 'COLLISION': 'COLLISION'}
# Coins become Rush 2 key records (the KEY class, behaviour 8): silver coins KEYS0-7 and gold coins KEYG0-7, numbered
# per kind in record order. src/collectibles.cpp gives each its bit (silver 0-7, gold 8-15) and draws 2049's coin model.
R49_COINS = {'SILVERCOIN': 'KEYS', 'GOLDCOIN': 'KEYG'}
R49_COINS_PER_KIND = 8
# Types that are dropped (2049-only game systems, or nothing to show). BULB and GUARDRAIL have no model and type flags
# 0x60004, which 2049's spawner (func_800ABCC8) refuses: they do nothing in 2049 either.
R49_DROP = {'BULB', 'GUARDRAIL', 'WPR_MINE', 'TRIGGER'} | \
    {t for t in ['WEPICON_CANN', 'WEPICON_GATT', 'WEPICON_GREN', 'WEPICON_HEAL', 'WEPICON_INVS', 'WEPICON_MINE',
                 'WEPICON_MISS', 'WEPICON_RAM', 'WEPICON_ROCK', 'WEPICON_SHLD', 'WEPICON_SONC', 'WEPICON_POWUP']}
R2_SAFE_DEBRIS_ASSETS = (0x12, 0x14)       # loaded for every race (func_800A4C98)


def is_prop(t):
    """Objects 2049 knocks over when a car hits them (src/rush2049/track2049_props.cpp): kind 2 (CONE1, GASPUMP, RAT,
    RATCONE), the signs (kind 0, sub-kinds 1-2) and CACTUS."""
    return t['kind'] == 2 or (t['kind'] == 0 and t['sub'] in (1, 2)) or t['name'] == 'CACTUS'


def prop_model(model):
    """A prop's model in the converted geometry: X49<model>, which Rush 2's prefix classifier doesn't take for one of
    its breakables (CONE1G1 would be a CONE1, STOPHITG1 a STOPHIT)."""
    return ('X49' + model)[:15]


def _mat3(m):
    return [m[0:3], m[3:6], m[6:9]]


def _relative(child, parent):
    """Make a child transform relative to its parent node (row-major m, p = row vector convention)."""
    P = _mat3(parent.m)
    # inverse of the parent 3x3 (general, handles scale)
    a, b, c = P[0]
    d, e, f = P[1]
    g, h, i = P[2]
    det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g)
    if abs(det) < 1e-12:
        return child.m, [child.pos[k] - parent.pos[k] for k in range(3)]
    inv = [[(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det],
           [(f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det],
           [(d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det]]
    dp = [child.pos[k] - parent.pos[k] for k in range(3)]
    # row-vector convention: world = local * P + p_parent  ->  local = (world - p_parent) * P^-1
    pos = [sum(dp[r] * inv[r][col] for r in range(3)) for col in range(3)]
    C = _mat3(child.m)
    m = [sum(C[row][k] * inv[k][col] for k in range(3)) for row in range(3) for col in range(3)]
    return m, pos


def _is_identity(m, eps=1e-4):
    return all(abs(m[k] - (1.0 if k in (0, 4, 8) else 0.0)) < eps for k in range(9))


def convert_ex(placement_2049, geometry_2049, prefix=None, types=None, extra_models=(), static_paths=True,
               r2_rules=None, finish=None):
    """Convert a 2049 placement file to Rush 2's format.

    placement_2049, geometry_2049: decompressed files (120+t, 101+t).
    prefix: Rush 2 tree name; must be the replaced slot's prefix (table 0x800C182C, e.g. 'VEGAS') because
            func_800A5110 looks the tree up by that name. Default: the 2049 name ('TRACK1').
    types: R49Types (read from the 2049 ROM) - required.
    extra_models: names of models the converted geometry will also contain (e.g. 2049 files 82-87 merged in).
    static_paths: add a static record for every PTHD object at its spawn node (node flag 1).
    r2_rules: R2Rules; when given, every emitted name is checked against Rush 2's own name classifier.
    finish: optional dict(pos=(x,y,z), m=9 floats) - adds the top-level '<prefix>FINISH' record Rush 2 draws
            (func_8008F080 needs that model; 2049 has no separate finish object).
    Returns (bytes, report). report['animated'] lists everything that needs runtime code to move,
    report['requires'] the model names the converted geometry must contain.
    """
    if types is None:
        raise ValueError('types (R49Types) is required')
    p = parse_49(placement_2049)
    recs = p['records']
    geo_names = r49_model_names(geometry_2049) | set(extra_models)
    report = dict(kept=[], mapped=[], static=[], dropped=[], animated=[], warnings=[], requires=set())

    def model_ok(n):
        return n in geo_names or n[:15] in geo_names

    keep = {}
    coins = {k: 0 for k in R49_COINS}
    for r in recs:
        t = types.classify(r.name)
        if t is None:
            if not model_ok(r.name):
                report['dropped'].append((r.name, 'model not in geometry'))
                continue
            keep[r.index] = (r.name, 'kept')
            continue
        tn = t['name']
        if tn in R49_COINS:
            if coins[tn] >= R49_COINS_PER_KIND:
                report['dropped'].append((r.name, 'more than %d coins of a kind' % R49_COINS_PER_KIND))
                continue
            keep[r.index] = ('%s%d' % (R49_COINS[tn], coins[tn]), 'mapped')
            coins[tn] += 1
            continue
        if tn in R49_DROP or tn.startswith('WEPICON') or t['kind'] == 6:
            report['dropped'].append((r.name, '2049-only (%s)' % types.KINDS.get(t['kind'], t['kind'])))
            continue
        if is_prop(t):
            if t['model'] and model_ok(prop_model(t['model'])):
                keep[r.index] = (prop_model(t['model']), 'static')
            else:
                report['dropped'].append((r.name, 'model %s not available' % t['model']))
            continue
        if tn in R49_TO_R2:
            new = R49_TO_R2[tn]
            suffix = '_F' if '_FW' in r.name else '_B' if '_BW' in r.name else ''
            keep[r.index] = (new + suffix, 'mapped')
            continue
        model = t['model']
        if model and model_ok(model):
            keep[r.index] = (model[:15], 'static')
            if t['kind'] == 4 or (t['kind'] == 0 and t['sub'] in (3, 4, 5)):
                report['animated'].append(dict(source='placement', name=r.name, type=tn, model=model,
                                               pos=tuple(r.pos), m=tuple(r.m), id=r.dyn_id, kind=t['kind']))
            continue
        report['dropped'].append((r.name, 'model %s not available' % model))

    # top-level records must all create a node (func_80081790 assigns parents by counting siblings)
    for r in recs:
        if r.parent == -1 and r.index not in keep:
            report['warnings'].append('top-level %s dropped: later children may attach to the wrong parent' % r.name)

    # rebuild the tree: walk in the original order and keep the original nesting
    order = []

    def walk(i, parent_out):
        while i >= 0:
            r = recs[i]
            if r.index in keep:
                idx = len(order)
                order.append((r, parent_out))
                if r.child >= 0:
                    walk(r.child, idx)
            elif r.child >= 0:
                walk(r.child, parent_out)          # promote children of a dropped record
            i = r.next
    walk(p['entries'][0][1] if p['entries'] else 0, -1)

    # path objects as static children of the section containing their spawn point (func_800AB53C does the same)
    extra = []
    if static_paths:
        for path in parse_paths(geometry_2049):
            t = types.classify(path['name'])
            if t is None or not t['model'] or not model_ok(t['model']):
                if t is None or t['name'] != 'TRIGGER':
                    report['dropped'].append((path['name'], 'path object without a model'))
                continue
            spawns = [n for n in path['nodes'] if n['flags'] & 1] or path['nodes'][:1]
            for n in spawns:
                m = _quat_to_m(n['quat'], n['scale'])
                extra.append((path, t, n, m))
                report['animated'].append(dict(source='path', name=path['name'], type=t['name'], model=t['model'],
                                               pos=tuple(n['pos']), m=tuple(m), id=path['id'], kind=t['kind'],
                                               path_flags=path['flags'], trigger=path['trigger'],
                                               nodes=len(path['nodes']), moving_collision=t['moving'] or
                                               bool(path['flags'] & 0x20)))

    # emit
    top = [k for k, (r, par) in enumerate(order) if par == -1]

    def section_for(pos):
        for k in top:
            r = order[k][0]
            lo, hi = r.bbox[0:3], r.bbox[3:6]
            if lo == hi:
                continue
            if all(lo[a] <= pos[a] - r.pos[a] <= hi[a] for a in range(3)) or \
                    all(lo[a] <= pos[a] <= hi[a] for a in range(3)):
                return k
        return top[0] if top else -1

    items = []
    for k, (r, par) in enumerate(order):
        name, how = keep[r.index]
        m, pos = list(r.m), list(r.pos)
        if par >= 0:
            parent = order[par][0]
            if how in ('kept', 'static'):
                m, pos = _relative(r, parent)          # Rush 2 only makes breakables relative by itself
            elif not _is_identity(parent.m):
                report['warnings'].append('%s: parent %s is rotated; Rush 2 subtracts only its position' %
                                          (r.name, parent.name))
        # bits 0x380000 / 0xFFC00000 of a Rush 2 node hold the culling-box type and index (func_8007FC80),
        # 0x40000 = draw in the second (late) pass, 0x400 = hidden, 0x100 << view = hidden in that view.
        flags = r.flags & 0x7FFFF
        if r.flags & ~0x7FFFF:
            report.setdefault('flag_bits_dropped', set()).add(r.flags & ~0x7FFFF)
        bbox = r.bbox if par == -1 else [0.0] * 6
        items.append(dict(name=name, m=m, pos=pos, flags=flags, bbox=bbox, parent=par, src=r))
        report[{'kept': 'kept', 'mapped': 'mapped', 'static': 'static'}[how]].append((r.name, name))
    # path objects: world-space top-level records after the sections, like 2049's unparented objects (the
    # visibility tables only hide the first sections, so they stay drawn wherever they move)
    for path, t, n, m in extra:
        items.append(dict(name=t['model'][:15], m=list(m), pos=list(n['pos']), flags=0x40, bbox=[0.0] * 6, parent=-1,
                          src=None))
        report['static'].append((path['name'], t['model'][:15]))

    if finish is not None:
        fname = (prefix or p['entries'][0][0]) + 'FINISH'
        items.append(dict(name=fname[:15], m=list(finish.get('m', (1, 0, 0, 0, 1, 0, 0, 0, 1))),
                          pos=list(finish['pos']), flags=0x40, bbox=[0.0] * 6, parent=-1, src=None))
        report['static'].append(('finish', fname))

    # names Rush 2 must resolve, and a check against Rush 2's own classifier
    for it in items:
        intended = 'model'
        if r2_rules is not None:
            typ, model, _ = r2_rules.classify(it['name'])
            got = 'model' if typ == 0 else 'special'
            src_how = None
            if it['src'] is not None:
                src_how = keep[it['src'].index][1]
            intended = 'special' if src_how == 'mapped' else 'model'
            if got != intended:
                report['warnings'].append('%s is classified by Rush 2 as type %d (%s)' % (it['name'], typ, model))
            if typ == 0:
                report['requires'].add(it['name'])
        elif it['src'] is None or keep[it['src'].index][1] != 'mapped':
            report['requires'].add(it['name'])

    # children lists in output order; Rush 2 wants every record's children contiguous via next links only
    kids = {}
    for k, it in enumerate(items):
        kids.setdefault(it['parent'], []).append(k)
    # renumber so that the file order is a pre-order walk (like the stock files)
    new_order = []

    def emit(par):
        for k in kids.get(par, []):
            new_order.append(k)
            emit(k)
    emit(-1)
    pos_of = {k: n for n, k in enumerate(new_order)}
    # func_80081790 gives the children of the k-th sibling to node (first node of the chain + k): every sibling
    # that precedes a record with children must create exactly one node, i.e. be a plain model.
    for par, lst in kids.items():
        for n, k in enumerate(lst):
            if kids.get(k) and any(items[j]['src'] is not None and keep[items[j]['src'].index][1] == 'mapped'
                                   for j in lst[:n + 1]):
                report['warnings'].append('children of %s would attach to the wrong node' % items[k]['name'])
    nxt, child = {}, {}
    for par, lst in kids.items():
        for a, b in zip(lst, lst[1:]):
            nxt[a] = pos_of[b]
        if par >= 0:
            child[par] = pos_of[lst[0]]

    name = (prefix or p['entries'][0][0])[:15]
    blob = bytearray(struct.pack('>II', 1, 0x18) + name.encode('latin1').ljust(16, b'\0'))
    for k in new_order:
        it = items[k]
        blob += it['name'].encode('latin1')[:15].ljust(16, b'\0')
        blob += struct.pack('>9f3fIhhI6f', *it['m'], *it['pos'], it['flags'], nxt.get(k, -1), child.get(k, -1), 0,
                            *it['bbox'])
    report['records'] = len(new_order)
    return bytes(blob), report


def convert(placement_2049, geometry_2049, prefix=None, types=None, **kw):
    """Convert a 2049 placement file (file 120+t) to a Rush 2 placement file (asset 0x3F+slot)."""
    if types is None:
        types = R49Types(roms.Rush2049())
    return convert_ex(placement_2049, geometry_2049, prefix, types, **kw)[0]


def _quat_to_m(q, s=(1, 1, 1)):
    x, y, z, w = q
    n = math.sqrt(x * x + y * y + z * z + w * w) or 1.0
    x, y, z, w = x / n, y / n, z / n, w / n
    # row-major rotation in the row-vector convention used by the placement matrices [I]
    m = [1 - 2 * (y * y + z * z), 2 * (x * y + z * w), 2 * (x * z - y * w),
         2 * (x * y - z * w), 1 - 2 * (x * x + z * z), 2 * (y * z + x * w),
         2 * (x * z + y * w), 2 * (y * z - x * w), 1 - 2 * (x * x + y * y)]
    for r in range(3):
        for c in range(3):
            m[r * 3 + c] *= s[r]
    return m


# ----------------------------------------------------------------------------------------------------------------
# Self-check

R49_PLACEMENT_GEOMETRY = {120 + t: 101 + t for t in range(19)}       # 120-125 race, 126-133 battle, 134-137 stunt, 138
R49_SHARED_MODELS = [61, 62, 63, 67, 68, 76, 77, 78, 79, 81]          # loaded by func_800BB9B0 (some per mode) [I]


def selfcheck(verbose=False):
    r2 = roms.Rush2()
    rules = R2Rules(r2)
    shared_r2 = {}
    for a in R2_SAFE_DEBRIS_ASSETS:
        shared_r2.update(r2_model_names(r2.asset(a)))
    print('=== Rush 2 placement files (assets 0x3F-0x4A) ===')
    totals = dict(files=0, records=0, missing=0)
    for t in range(12):
        data = r2.asset(0x3F + t)
        entries, recs = parse_r2(data)
        geo = r2_model_names(r2.asset(0x33 + t))
        loaded = dict(shared_r2)
        loaded.update(geo)
        names, flags = {}, set()
        for r in recs:
            typ, model, extra = rules.classify(r.name)
            key = (r.name, typ, model)
            names[key] = names.get(key, 0) + 1
            flags.add(r.flags)
        n_reached = _link(recs, entries[0][1])
        print('\n[%d %s] %d records (%d in tree), tree %r, flags %s, %d bytes' % (
            t, R2_TRACKS[t], len(recs), n_reached, entries, sorted(hex(f) for f in flags), len(data)))
        missing = []
        groups = {}
        for (name, typ, model), cnt in sorted(names.items()):
            if typ == 5:
                res = 'no model'
            else:
                res = 'ok' if model in loaded else 'MISSING'
                if res == 'MISSING':
                    missing.append(name)
            label = {0: 'model', 1: 'breakable', 2: 'breakable _F', 3: 'breakable _B', 4: 'breakable/2',
                     5: 'sound/marker/collision'}[typ]
            groups.setdefault(label, []).append('%s%s%s[%s]' % (name, 'x%d' % cnt if cnt > 1 else '',
                                                               '->' + model if model and model != name else '',
                                                               res))
        for label, lst in groups.items():
            print('  %-22s %s' % (label + ':', ' '.join(lst) if verbose or label != 'model' else
                                  '%d names, %d missing' % (len(lst), sum('MISSING' in x for x in lst))))
        if missing:
            print('  MISSING:', missing)
        totals['files'] += 1
        totals['records'] += len(recs)
        totals['missing'] += len(missing)
    print('\nRush 2 total: %(files)d files, %(records)d records, %(missing)d unresolved names' % totals)

    r49 = roms.Rush2049()
    types = R49Types(r49)
    shared49 = set()
    for k in R49_SHARED_MODELS:
        shared49 |= r49_model_names(r49.file(k))
    print('\n=== Rush 2049 placement files (120-138) ===')
    for k in range(120, 139):
        data = r49.file(k)
        p = parse_49(data)
        geo_file = R49_PLACEMENT_GEOMETRY[k]
        geom = r49.file(geo_file)
        geo = r49_model_names(geom)
        per_track = r49_model_names(r49.file(82 + (k - 120))) if k < 126 else set()
        loaded = geo | per_track | shared49
        recs = p['records']
        n_reached = _link(recs, p['entries'][0][1])
        flags = sorted({hex(r.flags) for r in recs})
        ids = sorted({r.dyn_id for r in recs if r.dyn_id >= 0})
        depth = max(r.depth for r in recs) if recs else 0
        rot_sections = sum(1 for r in recs if r.parent == -1 and not _is_identity(r.m))
        print('\n[file %d, geometry %d] tree %r: %d records (%d in tree), depth %d, flags %s, dyn ids %d..%d, '
              'GTLD %d, GDAT %d nodes (%d leaves), rotated sections %d' % (
                  k, geo_file, p['entries'], len(recs), n_reached, depth, flags, ids[0] if ids else -1,
                  ids[-1] if ids else -1, len(p['gtld']), len(p['gdat']),
                  sum(1 for g in p['gdat'] if 'ids' in g), rot_sections))
        groups, missing = {}, []
        static = {}
        for r in recs:
            t = types.classify(r.name)
            if t is None:
                ok = r.name in loaded
                static[r.name] = ok
                if not ok:
                    missing.append(r.name)
                continue
            model = t['model']
            res = 'no model' if not model else ('ok' if model[:15] in loaded else 'MISSING')
            if res == 'MISSING':
                missing.append('%s->%s' % (r.name, model))
            label = '%s (%s)' % (t['name'], types.KINDS.get(t['kind'], t['kind']))
            groups.setdefault(label, []).append((r.name, model, res))
        print('  static models: %d names, %d missing' % (len(static), sum(1 for v in static.values() if not v)))
        if verbose:
            print('   ', ' '.join(sorted(static)))
        for label, lst in sorted(groups.items()):
            names = sorted({n for n, _, _ in lst})
            print('  %-40s x%-3d %s -> %s [%s]' % (label, len(lst), ','.join(names), lst[0][1],
                                                   ','.join(sorted({x for _, _, x in lst}))))
        if missing:
            print('  MISSING:', sorted(set(missing)))
        paths = parse_paths(geom)
        if paths:
            summary = {}
            for pth in paths:
                t = types.classify(pth['name'])
                model = t['model'] if t else None
                res = 'no model' if not model else ('ok' if model[:15] in loaded else 'MISSING')
                key = (pth['name'], model, res)
                summary.setdefault(key, []).append(pth)
            print('  PTHD paths: %d' % len(paths))
            for (name, model, res), lst in summary.items():
                print('    %-11s x%-2d model %-16s [%s] flags %s trigger %s nodes %s ids %s' % (
                    name, len(lst), model, res, ','.join(sorted({'%x' % x['flags'] for x in lst})),
                    ','.join(sorted({str(x['trigger']) for x in lst})),
                    ','.join(str(len(x['nodes'])) for x in lst), ','.join(str(x['id']) for x in lst)))

    print('\n=== Conversion of the 2049 race tracks ===')
    for t in range(6):
        extra = r49_model_names(r49.file(82 + t)) | r49_model_names(r49.file(78))
        blob, rep = convert_ex(r49.file(120 + t), r49.file(101 + t), prefix=R2_TRACKS[t], types=types,
                               extra_models=extra, r2_rules=rules)
        entries, recs = parse_r2(blob)
        ok = sum(1 for r in recs if rules.classify(r.name)[0] in (1, 2, 3, 4) and
                 rules.classify(r.name)[1] in shared_r2)
        assert write_r2(entries, recs) == blob
        print('TRACK%d -> %s: %d records (%d bytes); kept %d, mapped %d (%d breakables on Rush 2 models), static '
              'stand-ins %d, dropped %d, animated %d%s' % (
                  t + 1, R2_TRACKS[t], len(recs), len(blob), len(rep['kept']), len(rep['mapped']), ok,
                  len(rep['static']), len(rep['dropped']), len(rep['animated']),
                  ', flag bits dropped %s' % sorted(hex(x) for x in rep['flag_bits_dropped'])
                  if rep.get('flag_bits_dropped') else ''))
        dropped = {}
        for n, why in rep['dropped']:
            dropped.setdefault(why, set()).add(n)
        for why, ns in dropped.items():
            print('    dropped (%s): %s' % (why, ' '.join(sorted(ns))))
        for w in rep['warnings']:
            print('    warning:', w)
        an = {}
        for a in rep['animated']:
            an.setdefault((a['source'], a['type']), 0)
            an[(a['source'], a['type'])] += 1
        print('    geometry must provide %d models (+ %sFINISH, textures CHKPNT/FINISH)' % (
            len(rep['requires']), R2_TRACKS[t]))
        print('    animated:', ', '.join('%s %s x%d' % (s, ty, c) for (s, ty), c in sorted(an.items())))


if __name__ == '__main__':
    selfcheck('--verbose' in sys.argv)
