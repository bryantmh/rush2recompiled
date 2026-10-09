"""Pairs Dreamcast Rush 2049 object names with the N64 ones, for the Dreamcast source's name map.

    dc_names.py DC_FILES_DIR [--emit src/rush2049_dc_names.inc] [--report]

The two versions name many objects differently (DC BKWBARIERL3_LOD = N64 BKWBARIERG1, F1FLAGL1_LOD1 = F1FLAGG28),
and the recomp's code and the converters work in N64 names, so a Dreamcast source renames its objects to the N64's.
Pairs come from, in order:
  1. placement: N64 and DC placement records of the same track at the same position and matrix;
  2. models: per file pair, objects whose decoded geometry has the same bounds (N64 = DC x 16) and close triangle
     counts, among those still unpaired.
Same-named objects need no entry. DC_FILES_DIR holds the disc's files decompressed (dc_cdi.py all); the N64 files
come from RUSH2049_ROM (roms.py). See docs/rush2049_research/dreamcast.md.
"""
import os, re, struct, sys

sys.path.insert(0, os.path.dirname(__file__))
from roms import Rush2049
from model import M49
from dc_model import DCModel

TRACKS = ['TRACK%d' % k for k in range(1, 7)] + ['DM%d' % k for k in range(1, 9)] + ['STUNT%d' % k for k in range(1, 5)] + ['OBSTACLE1']
# N64 model file -> DC file, besides track geometry (100 + id).
MODEL_FILES = {82 + i: 'TARGETST%d' % (i + 1) for i in range(6)}
MODEL_FILES.update({87 + c: 'CAR%d' % c for c in range(1, 14)})
MODEL_FILES.update({56: 'SELCAR', 60: 'SELTRK', 61: 'HUD', 62: 'HUD', 63: 'HUDBATTLE', 68: 'TARGETSNOPAK', 76: 'WEAPONS',
                    77: 'WINGS', 78: 'VEHICLES', 79: 'VEHICLES'})
for i, t in enumerate(TRACKS):
    MODEL_FILES[101 + i] = t


def placement(d, end):
    o, n = struct.unpack_from(end + 'II', d, 0)
    chunks = {}
    for i in range(n):
        tag = d[o + i * 12:o + i * 12 + 4]
        if end == '<':
            tag = tag[::-1]
        chunks[tag.decode()] = struct.unpack_from(end + 'II', d, o + i * 12 + 4)
    wo, wn = chunks['WOBJ']
    recs = []
    for i in range(wn):
        r = wo + i * 0x68
        name = d[r:r + 16].split(b'\0')[0].decode('latin1')
        vals = struct.unpack_from(end + '12f', d, r + 0x10)
        recs.append((name, tuple(round(v, 2) for v in vals)))
    return recs


def n64_bounds(d, dl):
    vb = [None] * 64
    pts, tris, stack, o = [], 0, [], dl
    for _ in range(200000):
        w0, w1 = struct.unpack_from('>II', d, o)
        op = w0 >> 24
        if op == 1:
            n = (w0 >> 12) & 0xFF
            v0 = ((w0 >> 1) & 0x7F) - n
            for i in range(n):
                vb[v0 + i] = struct.unpack_from('>hhh', d, w1 + i * 16)
                pts.append(vb[v0 + i])
        elif op == 5:
            tris += 1
        elif op == 6:
            tris += 2
        elif op == 0xDE:
            if (w0 >> 16) & 0xFF == 0:
                stack.append(o + 8)
            o = w1 & 0xFFFFFF
            continue
        elif op == 0xDF:
            if stack:
                o = stack.pop()
                continue
            break
        o += 8
    return pts, tris


def box(pts):
    if not pts:
        return None
    return [(min(p[i] for p in pts), max(p[i] for p in pts)) for i in range(3)]


def stem(name):
    """Name without its numbering and LOD suffix: BKWBARIERL3_LOD -> BKWBARIER, F1FLAGG28 -> F1FLAG."""
    s = re.sub(r'(L\d+)?_LO?D?\d*$', '', name)
    s = re.sub(r'[GLO]\d+$', '', s)
    return s.rstrip('_')


# Track geometry pieces: placement and models of a track both come from the disc, so they keep the disc's names
# (they are numbered differently and split differently from the N64's).
PIECE = re.compile(r'^(TRACK\d|DM\d|STUNT\d|OBSTACLE\d)L\d+$')

# N64 objects the disc models once: N64 file, N64 name -> disc object. The disc tints one battle coin by code.
ALIASES = {
    (62, 'SMOKERSMOKE'): 'SMOKEG1',
    (62, 'COIN_GLOWG1'): 'COIN_GLOWL3_LOD',
    (62, 'HULKL1_LOD1'): 'HULK_DCL1_LOD1',
    (87, 'TRAINNIGHTL7_LO'): 'TRAINNIGHTL11_L',
    (63, 'BCOIN_BLUEG1'): 'BATTLECOING1',
    (63, 'BCOIN_GREENG1'): 'BATTLECOING1',
    (63, 'BCOIN_REDG1'): 'BATTLECOING1',
    (63, 'BCOIN_YELLOWG1'): 'BATTLECOING1',
}
# Textures the N64 has that the disc makes by code: N64 file -> [(N64 texture, disc texture, RGB the disc's is
# multiplied by)]. The N64's battle coins are one texture under four palettes of pure blue, green, red and yellow.
TINTED = {
    63: [('BCOIN_BLUE', 'BATTLECOIN', 0x0000FF), ('BCOIN_GREEN', 'BATTLECOIN', 0x00FF00),
         ('BCOIN_RED', 'BATTLECOIN', 0xFF0000), ('BCOIN_YELLOW', 'BATTLECOIN', 0xFFFF00)],
}


def list_texture(nm, lst):
    """N64 texture record name a texture-load list loads (by its first SETTIMG), or ''. A record names either its
    texels or (track geometry) its load list, IMAG-relative."""
    d, p = nm.d, lst
    imag = nm.c['IMAG'][0]
    for t in nm.textures:
        if t['texels'] in (lst - imag, lst):
            return t['name']
    while p + 8 <= len(d) and d[p] != 0xDF:
        if d[p] == 0xFD:
            a = struct.unpack_from('>I', d, p + 4)[0] & 0xFFFFFF
            for t in nm.textures:
                if t['texels'] == a:
                    return t['name']
            return ''
        p += 8
    return ''


def first_dl_state(nm, dl):
    """(name of the first texture the list loads, first render mode word) of an N64 object list."""
    d, p, tex, mode = nm.d, dl, '', 0
    txld = nm.c['TXLD']
    while p + 8 <= len(d) and d[p] != 0xDF:
        w0, w1 = struct.unpack_from('>II', d, p)
        if w0 >> 24 == 0xDE and not tex and txld[0] <= (w1 & 0xFFFFFF) < txld[0] + txld[1]:
            tex = list_texture(nm, w1 & 0xFFFFFF)
        if w0 == 0xE200001C and not mode:
            mode = w1
        p += 8
    return tex, mode


def texture_modes(nm):
    """{texture name: render mode} of an N64 model file: the mode most of the triangles drawn with the texture use
    (lists walked from every LOD, following G_DL calls; texture loads are calls into TXLD)."""
    d = nm.d
    txld = nm.c['TXLD']
    counts = {}
    seen = set()

    def walk(p, state):
        stack = []
        for _ in range(200000):
            if p + 8 > len(d):
                return
            w0, w1 = struct.unpack_from('>II', d, p)
            op = w0 >> 24
            if op == 0xDE:
                a = w1 & 0xFFFFFF
                if txld[0] <= a < txld[0] + txld[1]:
                    state[0] = list_texture(nm, a)
                elif (w0 >> 16) & 0xFF == 0 and a not in seen:
                    seen.add(a)
                    stack.append(p + 8)
                    p = a
                    continue
            elif w0 == 0xE200001C:
                state[1] = w1
            elif op in (0x05, 0x06) and state[0] and state[1]:
                c = counts.setdefault(state[0], {})
                c[state[1]] = c.get(state[1], 0) + (2 if op == 0x06 else 1)
            elif op == 0xDF:
                if not stack:
                    return
                p = stack.pop()
                continue
            p += 8

    for o in nm.objects:
        for l in o['lods']:
            if l[3]:
                walk(l[3], ['', 0])
    return {t: max(c.items(), key=lambda kv: kv[1])[0] for t, c in counts.items()}


def numbering(name):
    return [int(x) for x in re.findall(r'\d+', name)] or [0]


def main():
    dcdir = sys.argv[1]
    rom = Rush2049()
    pairs = {}  # dc -> n64
    notes = []

    def add(dc, n64, why):
        if dc != n64 and dc not in pairs and not PIECE.match(dc):
            pairs[dc] = n64
            notes.append('%-16s -> %-16s %s' % (dc, n64, why))

    for i, t in enumerate(TRACKS):
        n = placement(rom.file(120 + i), '>')
        try:
            c = placement(open(os.path.join(dcdir, t + 'WORLDS.LZS'), 'rb').read(), '<')
        except FileNotFoundError:
            continue
        where = {}
        for name, v in n:
            where.setdefault(v, []).append(name)
        for name, v in c:
            cand = where.get(v)
            if cand and len(cand) == 1:
                add(name, cand[0], 'placement %s' % t)

    n64_names = set()
    for nf in MODEL_FILES:
        try:
            n64_names |= set(o['name'] for o in M49(rom.file(nf)).objects)
        except Exception:
            pass
    for nf, dcname in sorted(MODEL_FILES.items()):
        try:
            nm = M49(rom.file(nf))
            cm = DCModel(open(os.path.join(dcdir, dcname + '.LZS'), 'rb').read())
        except Exception as e:
            notes.append('skip %d %s: %s' % (nf, dcname, e))
            continue
        # Names the N64 has anywhere are the N64's objects (the Dreamcast HUD file holds N64 files 61 and 62).
        n_objs = n64_names
        used = set(o['name'] for o in cm.objects if o['name'] in n_objs) | set(pairs.get(o['name']) for o in cm.objects)
        n_sig = {}
        for o in nm.objects:
            if o['name'] in used or not o['lods'] or not o['lods'][0][3]:
                continue
            pts, tris = n64_bounds(nm.d, o['lods'][0][3])
            n_sig[o['name']] = (box(pts), tris)
        dc_sig = {}
        for o in cm.objects:
            if o['name'] in n_objs or o['name'] in pairs or not o['lods']:
                continue
            strips, _ = cm.decode(o['lods'][0][3], bool(o['lods'][0][1] & 0x10))
            pts = [tuple(v * 16 for v in p[0]) for s in strips for p in s[2] if p[0]]
            dc_sig[o['name']] = box(pts)
        # Closest bounds first, between names sharing their stem or first three letters. Some animation frames are
        # modeled turned half a turn about y on the Dreamcast (FLAG2G2 = FLAG2O1 with x and z negated), so bounds are
        # also compared turned. Frames of one animation look alike; ties go to the same place in the numbering.
        def rank(name, pool):
            same = sorted((n for n in pool if stem(n) == stem(name)), key=numbering)
            return same.index(name) if name in same else 0
        cands = []
        for dname, b in dc_sig.items():
            if b is None:
                continue
            turned = [(-b[0][1], -b[0][0]), b[1], (-b[2][1], -b[2][0])]
            for name, (nb, nt) in n_sig.items():
                same_stem = stem(name) == stem(dname)
                if nb is None or (name[:3] != dname[:3] and not same_stem):
                    continue
                size = max(1.0, max(hi - lo for lo, hi in b))
                err = min(max(abs(nb[k][j] - bb[k][j]) for k in range(3) for j in range(2)) / size for bb in (b, turned))
                if err < (0.08 if same_stem else 0.02):
                    cands.append((round(err, 2), abs(rank(dname, dc_sig) - rank(name, n_sig)), err, dname, name))
        done_d, done_n = set(), set()
        for _, _, err, dname, name in sorted(cands):
            if dname in done_d or name in done_n:
                continue
            done_d.add(dname)
            done_n.add(name)
            add(dname, name, 'geometry %d/%s err %.3f' % (nf, dcname, err))
        for dname in dc_sig:
            if dname not in done_d:
                notes.append('unpaired %-16s in %s' % (dname, dcname))

    if '--report' in sys.argv:
        print('\n'.join(notes))
    print('%d pairs' % len(pairs))
    # Per N64 model file (not track geometry): its objects in order, each with the disc object it is made from, and
    # where the N64 draws an object differently: its LOD flags, the texture its texture-swap handle names, the texture
    # its list loads where the disc's object takes one by code instead, and the render mode for that.
    objects, missing = [], []
    for nf, dcname in sorted(MODEL_FILES.items()):
        if nf >= 101:
            continue
        try:
            nm = M49(rom.file(nf))
            cm = DCModel(open(os.path.join(dcdir, dcname + '.LZS'), 'rb').read())
        except Exception:
            continue
        dco = {}
        for o in cm.objects:
            dco.setdefault(pairs.get(o['name'], o['name']), o)
        dc_by_name = {o['name']: o for o in cm.objects}
        for o in nm.objects:
            src = dco.get(o['name'])
            if (nf, o['name']) in ALIASES:
                src = dc_by_name.get(ALIASES[(nf, o['name'])])
            if src is None:
                missing.append('%d %s' % (nf, o['name']))
                continue
            flags, handle, bind, mode = -1, '', '', 0
            l, q = o['lods'][0], src['lods'][0]
            if (l[1] & 0x8017) != (q[1] & 0x8017):
                flags = l[1]
            if l[1] & 1:
                handle = nm.textures[l[0]]['name'] if l[0] < len(nm.textures) else ''
                if not (q[1] & 1):
                    mode = first_dl_state(nm, l[3])[1]
            elif q[1] & 1:
                bind, mode = first_dl_state(nm, l[3])
                if nf in TINTED and o['name'].endswith('G1') and o['name'][:-2] in [t[0] for t in TINTED[nf]]:
                    bind = o['name'][:-2]
            objects.append((nf, o['name'], src['name'], flags, handle, bind, mode))
    if '--report' in sys.argv:
        print('no disc object for: ' + ', '.join(missing))
    if '--emit' in sys.argv:
        path = sys.argv[sys.argv.index('--emit') + 1]
        with open(path, 'w', newline='\n') as f:
            f.write('// Generated by tools/rush2049/dc_names.py from the N64 ROM and the disc; do not edit.\n')
            # Each user defines the table macros it wants; the rest expand to nothing, and all are undefined at the end.
            macros = ('RENAME', 'OBJECT', 'TEXTURE', 'SIZE', 'TINTED', 'TEXMODE')
            for mac in macros:
                f.write('#ifndef %s\n#define %s(...)\n#endif\n' % (mac, mac))
            f.write('// RENAME(disc name, N64 name): objects the two versions name differently.\n')
            for dc, n64 in sorted(pairs.items()):
                f.write('RENAME("%s", "%s")\n' % (dc, n64))
            f.write('// OBJECT(N64 file, N64 name, disc object, LOD flags or -1, swap texture, bound texture, render mode):\n')
            f.write("// the objects of the N64 model files other than track geometry, in the N64's order.\n")
            for nf, n, src, flags, handle, bind, mode in objects:
                f.write('OBJECT(%d, "%s", "%s", %d, "%s", "%s", 0x%08X)\n' % (nf, n, src, flags, handle, bind, mode))
            f.write('// TEXTURE(N64 file, name, N64 width, height): the textures a non-track N64 file names (code looks them\n')
            f.write('// up), kept at no more than the N64 size.\n')
            for nf, dcname in sorted(MODEL_FILES.items()):
                if nf >= 101:
                    continue
                for t in M49(rom.file(nf)).textures:
                    f.write('TEXTURE(%d, "%s", %d, %d)\n' % (nf, t['name'], t['w'], t['h']))
            f.write('// SIZE(N64 file, bytes): non-track N64 files, whose size the made ones keep within (fixed load windows).\n')
            for nf in sorted(MODEL_FILES):
                if nf < 101:
                    f.write('SIZE(%d, %d)\n' % (nf, len(rom.file(nf))))
            f.write('// TINTED(N64 file, N64 texture, disc texture, RGB): textures the disc colors by code.\n')
            for nf, ts in sorted(TINTED.items()):
                for n, src, rgb in ts:
                    f.write('TINTED(%d, "%s", "%s", 0x%06X)\n' % (nf, n, src, rgb))
            f.write('// TEXMODE(N64 file, texture, render mode): the mode the N64 draws most of a texture\'s triangles with\n')
            f.write('// (opaque, alpha-tested edges or translucent); disc textures the N64 lacks go by their alpha.\n')
            for nf in sorted(MODEL_FILES):
                for t, mode in sorted(texture_modes(M49(rom.file(nf))).items()):
                    f.write('TEXMODE(%d, "%s", 0x%08X)\n' % (nf, t, mode))
            for mac in macros:
                f.write('#undef %s\n' % mac)


if __name__ == '__main__':
    main()
