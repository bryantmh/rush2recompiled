"""Builds every Rush 2 file a converted Rush 2049 race track needs (prototype of the runtime converter).

    python track.py K SLOT OUTDIR     K = 2049 track id + 1 (race tracks 1-6, stunt arenas 15-18),
                                      SLOT = Rush 2 track slot it replaces

Writes OUTDIR/geometry.bin (asset 0x33+slot), placement.bin (0x3F+slot), collision.bin (0x4B+slot),
path.bin (0x57+slot), pathb.bin (0x63+slot), pvs.bin (the slot's visibility table), tables.txt (per-track table
values) and texanim.txt (animated textures as patch sites in geometry.bin, texanim.py), and checks the results with the Rush 2 validators.

Geometry: the 2049 track file plus its per-track object file (82+K) and the shared flag/trigger models (file 78) are
merged into one Rush 2 container. A Rush 2 container has one texture-load list range ([7]..[8], rebased by a linear
scan), so the chunks are regrouped by kind: all IMAG chunks, then all TXLD chunks, then all OBJS chunks, then the
TLUT-restoring list copies and the Rush 2 tables. Pointers are moved to the new position of the chunk they point
into.
"""
import os, struct, sys
from collections import Counter

import roms
import model
import placement
import collision
import paths
import texanim
from model import u32, u16, M49, walk_reloc, end_state, list_end, load_flags, G_TLUT_RGBA16, LOD_FLAGS_KEPT

PREFIXES = ['VEGAS', 'NYONE', 'HAWAII', 'NYTWO', 'ALCATRAZ', 'LA', 'SEATTLE', 'HALFPIPE', 'CRASH', 'PIPE', 'ATARI',
            'STUNT1']
SHARED_MODEL_FILES = [78, 68]      # F1FLAG / F2FLAG frames, TRIGGEROFF / TRIGGERON; GOLDCOIN / SILVERCOIN
# The coins' models run Rush 2's key behaviour (8): the placement makes coins KEY records (placement.R49_COINS).
COIN_KINDS = {'GOLDCOING_COIN': 8, 'SILVERCOINS_COI': 8}
FOG_COLOURS_2049 = 0x80114658      # 3 bytes per 2049 track id
DEMO_LISTS_2049 = 0x801173D8       # ptr[12]: forward 0-5, backward 6-11
DEMO_COUNTS_2049 = 0x80117408      # s16[12]
RECORD_SEEDS_2049 = 0x8002E870     # boot segment, f32[t + 19 * backward]
STUNT_FIRST = 15                   # k of stunt arena 1 (2049 track id 14)


BLEND_RUSH2 = (0xF9000000, 0x00000010)   # G_SETBLENDCOLOR as Rush 2's frame setup leaves it (0x80020178)


def end_blend(d, start, flags):
    """Blend colour a list leaves set (following G_DL calls and resolving conditional ops), or None."""
    stack, o, blend = [], start, None
    for _ in range(200000):
        w0, w1 = u32(d, o), u32(d, o + 4)
        op = w0 >> 24
        if op == 0xF9:
            blend = w1
        if op == 0xE0 and d[o + 1] == 1 and not (flags >> u16(d, o + 2)) & 1:
            o = w1 & 0xFFFFFF
            continue
        if op == 0xDE:
            if not (w0 >> 16) & 0xFF:
                stack.append(o + 8)
            o = w1 & 0xFFFFFF
            continue
        if op == 0xDF:
            if not stack:
                return blend
            o = stack.pop()
            continue
        o += 8
    raise model.DLError('list %#x does not end' % start)


def merge_models(files, rename=None, dummies=(), dummy_textures=(), exclude=(), kinds=None):
    """Merges 2049 model containers into one Rush 2 container. Returns (bytes, report). Models named in exclude are
    left out (their data stays in the file, unreferenced). kinds: output model name -> behaviour id of its name record
    (default: the 2049 object's)."""
    rename = rename or {}
    report = Counter()
    srcs = [M49(d) for d in files]
    out = bytearray(model.HEADER)
    moved = []   # per file: list of (old start, old end, new start)

    def place(kind):
        for i, m in enumerate(srcs):
            o, n = m.c[kind]
            while len(out) & 7:
                out.append(0)
            moved[i].append((o, o + n, len(out)))
            out.extend(m.d[o:o + n])

    for _ in srcs:
        moved.append([])
    place('IMAG')
    while len(out) & 7:
        out.append(0)
    txld_start = len(out)
    place('TXLD')
    txld_end = len(out)
    place('OBJS')

    def new_addr(i, a):
        for o, e, n in moved[i]:
            if o <= a < e or (a == e and o < e):
                return a - o + n
        raise ValueError('file %d: pointer %#x outside IMAG/TXLD/OBJS' % (i, a))

    done = set()

    def fix(i, o_old, add_imag=False):
        """Rewrites the pointer word at old offset o_old of file i (which must lie in a moved chunk)."""
        o_new = new_addr(i, o_old)
        if o_new in done:
            return
        done.add(o_new)
        w = u32(srcs[i].d, o_old)
        a = (w & 0xFFFFFF) + (srcs[i].imag if add_imag else 0)
        struct.pack_into('>I', out, o_new, (w & 0xFF000000) | new_addr(i, a))

    lod_lists = {}   # (file, old dl) -> new dl
    for i, m in enumerate(srcs):
        for ob in m.objects:
            for t, f, dist, dl, vtx in ob['lods']:
                if not dl or (i, dl) in lod_lists:
                    continue
                ptrs, conds, fv = walk_reloc(m.d, dl)
                for p in ptrs:
                    fix(i, p)
                for o, cnd, tgt in conds:
                    fix(i, o + 4)
                lod_lists[(i, dl)] = new_addr(i, dl)
        ta, ts = m.c['TXLD']
        d = m.d
        for o in range(ta, ta + ts, 8):
            op = d[o]
            if op == 0xFD or (op == 0xE1 and o + 8 < ta + ts and d[o + 8] == 0x04):
                fix(i, o + 4, add_imag=True)

    # Lists that leave the texture LUT mode or the blend colour other than Rush 2 expects get a copy ending in a
    # restore of both (see model.py; the blend colour's alpha is the alpha-compare threshold of the HUD's translucent
    # widgets, which 2049's 0xBE hides).
    restored = {}
    for key, s in sorted(lod_lists.items(), key=lambda kv: kv[1]):
        states = {end_state(out, s, load_flags(1, mir)) for mir in (False, True)}
        blends = {end_blend(out, s, load_flags(1, mir)) for mir in (False, True)}
        if states <= {None, G_TLUT_RGBA16[1]} and blends <= {None, BLEND_RUSH2[1]}:
            continue
        e = list_end(out, s)
        ns = len(out)
        ptrs, conds, fv = walk_reloc(out, s)
        body = bytearray(out[s:e])
        for p in ptrs + [o + 4 for o, _, _ in conds]:
            w = u32(out, p)
            if s <= (w & 0xFFFFFF) <= e:
                struct.pack_into('>I', body, p - s, (w & 0xFF000000) | ((w & 0xFFFFFF) - s + ns))
        out.extend(body + struct.pack('>6I', G_TLUT_RGBA16[0], G_TLUT_RGBA16[1], BLEND_RUSH2[0], BLEND_RUSH2[1],
                                      0xDF000000, 0))
        restored[s] = ns
        report['lists given a TLUT restore'] += 1
    empty_dl = len(out)
    out.extend(struct.pack('>2I', 0xDF000000, 0))

    # Rush 2 tables. Names are unique across the merged files (first one wins) and sorted for the binary search.
    entries, have = [], set()
    for i, m in enumerate(srcs):
        for ob in m.objects:
            n = rename.get(ob['name'], ob['name']).encode('latin1')[:15]
            if n in have:
                report['duplicate names skipped'] += 1
                continue
            if n.decode('latin1') in exclude:
                report['excluded names'] += 1
                continue
            have.add(n)
            entries.append((n, i, ob))
    for name in dummies:
        n = name.encode('latin1')[:15]
        if n not in have:
            have.add(n)
            entries.append((n, -1, None))
            report['dummy models'] += 1
    entries.sort(key=lambda e: e[0])

    model_off = len(out)
    for n, i, ob in entries:
        rec = bytearray(0x34)
        if ob is None:
            struct.pack_into('>IHHfI', rec, 0, 1, 0, 0, 0.0, empty_dl)
        else:
            struct.pack_into('>I', rec, 0, max(1, min(4, ob['n'])))
            for j in range(min(4, ob['n'])):
                t, f, dist, dl, vtx = ob['all_lods'][j]
                ndl = 0
                if dl:
                    ndl = lod_lists[(i, dl)]
                    ndl = restored.get(ndl, ndl)
                struct.pack_into('>HHfI', rec, 4 + j * 12, t, f & LOD_FLAGS_KEPT, dist, ndl)
        out.extend(rec)
    # Palette indices in texture records index the slot's palette table, so a merged file's indices are offset by
    # the palettes of the files before it.
    pal_base, pals = [], []
    for i, m in enumerate(srcs):
        pal_base.append(len(pals))
        for p in m.palettes:
            pals.append((p['name'].encode('latin1')[:15], p['a'], new_addr(i, p['pal'] + m.imag)))
    textures = []
    tex_names = set()
    for i, m in enumerate(srcs):
        for t in m.textures:
            n = t['name'].encode('latin1')[:15]
            if n in tex_names:
                report['duplicate texture names skipped'] += 1
                continue
            tex_names.add(n)
            idx = t['idx']
            if idx & 0xFFFF != 0xFFFF:
                idx = (idx & 0xFFFF0000) | ((idx + pal_base[i]) & 0xFFFF)
            textures.append((n, t['w'], t['h'], idx, new_addr(i, t['texels'] + m.imag), t['fmt']))
    for name in dummy_textures:
        n = name.encode('latin1')[:15]
        if n not in tex_names:
            tex_names.add(n)
            textures.append((n, 0, 0, 0xFFFF, empty_dl, 0))   # texture-load list = an empty list
            report['dummy textures'] += 1
    tex_off = len(out)
    for n, w, h, idx, data, fmt in textures:
        rec = bytearray(0x20)
        rec[:16] = n.ljust(16, b'\0')
        struct.pack_into('>HHIII', rec, 16, w, h, idx, data, fmt)
        out.extend(rec)
    pal_off = len(out)
    for n, a, data in pals:
        rec = bytearray(0x18)
        rec[:16] = n.ljust(16, b'\0')
        struct.pack_into('>II', rec, 16, a, data)
        out.extend(rec)
    name_off = len(out)
    for n, i, ob in entries:
        rec = bytearray(0x18)
        rec[:16] = n.ljust(16, b'\0')
        if ob is not None:
            struct.pack_into('>fHH', rec, 16, ob['radius'], (kinds or {}).get(n.decode('latin1'), ob['kind']), 0)
        out.extend(rec)
    struct.pack_into('>10I', out, 0, model_off, name_off, tex_off, pal_off, len(entries), len(textures), len(pals),
                     txld_start, txld_end, 0)
    report['models'] = len(entries)
    report['textures'] = len(textures)
    report['palettes'] = len(pals)
    return bytes(out), report


def path_files(k):
    """2049 AI path files (forward, backward) of 2049 track id k - 1. Race tracks have both; the stunt arenas only
    one, which serves both ways (Rush 2 has no backward stunt races). 2049's loader takes file 0x9E + id (func_800BB9B0)
    for both, so stunt arena n is file 171 + n, although the editor names inside the files run the other way."""
    if k <= 6:
        return 157 + k, 176 + k
    if STUNT_FIRST <= k < STUNT_FIRST + 4:
        return 157 + k, 157 + k
    raise ValueError('no paths for 2049 track %d' % k)


def build(k, slot, outdir, static_paths=True):
    q = roms.Rush2049()
    r2 = roms.Rush2()
    prefix = PREFIXES[slot]
    os.makedirs(outdir, exist_ok=True)
    problems = []

    # Models Rush 2's shared assets also define (breakable glass and flags) are left to Rush 2: the placement maps
    # those objects to Rush 2's breakable classes, whose behaviour comes from Rush 2's name records.
    shared = set()
    for a in (0x12, 0x14):
        shared |= {m['name'] for m in model.R2Model(r2.asset(a)).models}
    geo_files = [q.file(100 + k)] + ([q.file(81 + k)] if k <= 6 else []) + [q.file(f) for f in SHARED_MODEL_FILES]
    types = placement.R49Types(q)
    rename = {'SKYSKY': 'SKYO1', 'STUNTSKYSKY': 'SKYO1'}
    # Props keep their 2049 models under names Rush 2's prefix classifier doesn't take for its breakables.
    rename.update({t['model']: placement.prop_model(t['model']) for t in types.types if placement.is_prop(t) and t['model']})
    geometry, grep = merge_models(geo_files, rename=rename,
                                  dummies=[prefix + 'FINISH', prefix + 'FINISHB'],
                                  dummy_textures=['CHKPNT', 'FINISH'], exclude=shared, kinds=COIN_KINDS)
    gm = model.R2Model(geometry)
    errs, _ = gm.check()
    errs += model.load_test(geometry)
    problems += ['geometry: ' + e for e in errs[:20]]
    names = {m['name'] for m in gm.models}

    place, prep = placement.convert_ex(q.file(119 + k), q.file(100 + k), prefix, types, extra_models=names,
                                       static_paths=static_paths, r2_rules=placement.R2Rules(r2))
    problems += ['placement: ' + w for w in prep['warnings']]
    missing = sorted(n for n in prep['requires'] if n not in names and n not in shared)
    problems += ['placement needs missing model ' + n for n in missing]
    clash = sorted(names & shared)
    if clash:
        problems.append('geometry names also in shared assets (shared slot may win): %s' % clash)

    crep = {}
    coll = collision.convert(q.file(138 + k), report=crep)
    fwd, bwd = path_files(k)
    fwd = paths.convert(q.file(fwd))
    bwd = paths.convert(q.file(bwd))
    if k > 6:
        fwd = bwd = paths.spine_lanes(fwd, q.file(138 + k))
        problems += ['path: ' + e for e in paths.validate(paths.parse(fwd))]
    pvs = model.pvs_rush2_bytes(model.pvs_2049(q, k))
    npvs = q.main[model.R49_PVS_COUNT - q.MAIN_VRAM + k - 1]

    for name, data in (('geometry', geometry), ('placement', place), ('collision', coll), ('path', fwd),
                       ('pathb', bwd), ('pvs', pvs)):
        open(os.path.join(outdir, name + '.bin'), 'wb').write(data)
    fog = q.main[FOG_COLOURS_2049 - q.MAIN_VRAM + (k - 1) * 3:][:3]
    with open(os.path.join(outdir, 'tables.txt'), 'w') as f:
        f.write('fog %s\npvs_count %d\n' % (fog.hex(), npvs))
    with open(os.path.join(outdir, 'texanim.txt'), 'w') as f:
        f.write(texanim.describe(q, k, geometry))
    print('2049 track %d -> slot %d (%s): geometry %#x (%s), placement %#x (%d records), collision %#x, paths %#x/%#x'
          % (k, slot, prefix, len(geometry), dict(grep), len(place), prep['records'], len(coll), len(fwd), len(bwd)))
    print('  fog %s, pvs regions %d, animated objects %d, collision %s' %
          (fog.hex(), npvs, len(prep['animated']), {a: b for a, b in crep.items() if not isinstance(b, (list, dict))}))
    for p in problems:
        print('  PROBLEM', p)
    return not problems


if __name__ == '__main__':
    if len(sys.argv) == 4:
        sys.exit(0 if build(int(sys.argv[1]), int(sys.argv[2]), sys.argv[3]) else 1)
    ok = True
    for k in range(1, 7):
        ok &= build(k, 2, os.path.join('out', 'track%d' % k))
    for k in range(STUNT_FIRST, STUNT_FIRST + 4):
        ok &= build(k, 11, os.path.join('out', 'stunt%d' % (k - STUNT_FIRST + 1)))
    sys.exit(0 if ok else 1)
