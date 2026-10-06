"""Conversion of a San Francisco Rush (Rush 1, USA) race track to the files of a Rush 2 track slot (prototype of
src/track1_convert.cpp; docs/rush1_research.md has the formats and evidence).

convert(r1, t, prefix, backward) -> dict(geometry, placement, collision, path, pvs, pvs_count, demo_starts)
t: Rush 1 track 0-6. prefix: the Rush 2 slot's track prefix (placement tree name, <prefix>FINISH).

Usage: python track1.py            converts all 7 tracks both ways and runs the checks
"""
import math, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'rush2049'))
import paths as r2paths
from r1 import Rush1

GEOMETRY, PLACEMENT, COLLISION, COLLISION_BACK, PATH, PATH_BACK, OBJECTS = 37, 0, 44, 51, 58, 65, 12
TEXTURE_BANK = 36   # Shared track textures, loaded right after the track geometry: its segment covers both
FINISH_MODELS = 0x800C7AA0      # char*[7]: the model holding each track's finish banner
CHECKPOINTS = (0x800C7B6C, 0x800C7B88)  # forward / backward: ptr per track to 0x18-byte records
PVS_TABLES = [0x800CD0F8, 0x800CD488, 0x800CDB78, 0x800CE108, 0x800CE7F8, 0x800CEE38, 0x800CF298]
PVS_COUNTS = 0x800CF77C

# Rush 2's placement name classifier (func_800815DC): exact names, then prefixes. A kept Rush 1 name must not match.
R2_EXACT = ['BIGCHR1', 'BIGCHR2', 'BOAT', 'CABLECAR', 'CANNON', 'CROWDSCR', 'DOCKWHIS', 'FIRECRCK', 'FOGHORN',
            'FOUNTAIN', 'KIDSPLAY', 'OCEAN', 'PARKBIRD', 'SEAGULL', 'SMLCLAP', 'SMLHOOT', 'VOLCANO', 'KLAX',
            'HARDRIVE', 'PETERP', 'PITFIGTER', 'RAMPART', 'MARBLE']
R2_PREFIXES = ['MARKER', 'TIME', 'COLLISION', 'CONE1', 'FENCE', 'FLAG2', 'GASIGN', 'GATE', 'KEY', 'METER', 'TREEHIT',
               'WINDOW', 'BALL', 'SHATPANE', 'CURVEHIT', 'THINKHIT', 'BUMPHIT', 'DIPHIT', 'PCAREHIT', 'NOPASHIT',
               'RIGHTHIT', 'LEFTHIT', 'ZONEHIT', 'MPH45HIT', 'MPH75HIT', 'YIELDHIT', 'REDUCHIT', 'STOPHIT', 'SLOWHIT',
               'NYTREEHT', 'GRANDWIN', 'GLAMPHIT', 'CHAIRHIT', 'DESKHIT', 'MAPSIGN', 'SRFBRD', 'USFLAG', 'UMBRELLA',
               'RATCONE', 'P737', 'PJET', 'F23', 'ENGTABLE', 'DOTHEDEW', 'NYLGATE']

# Rush 1 breakables raced as Rush 2 breakable classes, drawn with Rush 1's models: (Rush 1 model prefix, replacement for
# the prefix in the record name or None to keep it, behaviour id of the Rush 2 class model). The class gives the sound
# and debris handling; src/track1.cpp redirects Rush 2's lookup of the class model to the record's own model.
R1_CLASSES = [('CONE1L', None, 2), ('METERL', None, 5), ('TREEHIT', None, 5), ('FLAG2L', None, 23), ('FENCEL', None, 4),
              ('GASIGNL', None, 7), ('WINDOWBL', 'SHATPANEBL', 9), ('PMUNCH_01L', 'CURVEHITPMUNCH', 5),
              ('TMUNCHL', 'TREEHITTMUNCH', 5)]
# Rush 2 breakable pieces (resolved by name at race start) -> Rush 1's: (Rush 2 prefix, Rush 1 prefix, count, shift);
# tree pieces TREEHITnO1 -> TREEHITnL1.
R1_PIECES = [('CONE1O', 'CONE1L', 1, 0), ('METERO', 'METERL', 1, 0), ('SHATPANEO', 'WINDOWBL', 7, 0),
             ('FENCEO', 'FENCEL', 12, 0), ('FLAG2O', 'FLAG2L', 10, -1), ('GASIGNO', 'GASIGNL', 3, 0)]
# Rush 1 models from the shared object file (asset 12) that the classes and keys use.
R1_SHARED_OBJECTS = ['CONE1L1', 'METERL1', 'TREEHIT1L1', 'TREEHIT4L1', 'KEYL1']
# Keys (KEYL1, key number 1-8 at record +0x4A) become Rush 2 key records KEY1-8 (the KEY class, behaviour 8) drawn with
# Rush 1's key model; src/collectibles.cpp gives key n bit n - 1.
R1_KEY = 'KEYL'
R1_KEY_MODEL = 'KEYL1'
R1_KEYS_MAX = 8
KEY_BEHAVIOUR = 8
# Left out: MARKER and TIME are unknown.
R1_DROPPED = ['MARKER', 'TIME', 'BIGCHEER2']
# Rush 1 sound emitters -> the Rush 2 emitters with the same sound and range (SMALLHOOT behaves as BIGCHR1, BIGCHEER as
# BIGCHR2; FIRECRK gets Rush 1's own sound at run time). BIGCHEER2 does nothing in Rush 1 and is left out.
R1_EMITTERS = {'CCAR': 'CABLECAR', 'FIRECRK': 'FIRECRCK', 'SMALLHOOT': 'BIGCHR1', 'BIGCHEER': 'BIGCHR2'}


class ConvertError(Exception):
    pass


def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def u16(d, o): return struct.unpack_from('>H', d, o)[0]
def s16(d, o): return struct.unpack_from('>h', d, o)[0]
def cname(d, o, n=16): return d[o:o + n].split(b'\0')[0].decode('latin1')


def r2_classified(name):
    return name in R2_EXACT or any(name.startswith(p) for p in R2_PREFIXES)


def safe_name(name):
    """A Rush 1 model name Rush 2's classifier leaves alone."""
    if not r2_classified(name):
        return name
    out = 'R1' + name
    if len(out) > 15 or r2_classified(out):
        raise ConvertError('no safe name for ' + name)
    return out


# ---------------------------------------------------------------------------------------------------------------------
# Model containers

class Container:
    """A Rush 1 model container: 0x20-byte header {names ptr, model count, 0, 0, textures ptr, count, palettes ptr,
    count}, 0x34-byte model records from +0x20 (Rush 2's layout), 0x18 name, 0x20 texture and 0x18 palette records.
    Pointers are segment addresses; every pointer in the file uses the file's own segment."""

    def __init__(self, data):
        self.d = data
        h = struct.unpack_from('>8I', data, 0)
        self.seg = h[0] >> 24
        self.n_models = h[1]
        self.names_at = self.off(h[0])
        self.tex_at, self.n_tex = self.off(h[4]), h[5]
        self.pal_at, self.n_pal = self.off(h[6]), h[7]

    def off(self, a):
        if a >> 24 != self.seg or (a & 0xFFFFFF) >= len(self.d):
            raise ConvertError('pointer %08X outside the file' % a)
        return a & 0xFFFFFF

    def model(self, i):
        o = 0x20 + i * 0x34
        lods = []
        for l in range(4):
            handle, flags, dist, dl = struct.unpack_from('>HHfI', self.d, o + 4 + l * 12)
            lods.append((flags, dist, self.off(dl) if dl else 0))
        return u32(self.d, o), lods

    def name(self, i):
        return cname(self.d, self.names_at + i * 0x18)

    def radius(self, i):
        return struct.unpack_from('>f', self.d, self.names_at + i * 0x18 + 16)[0]

    def texture(self, i):
        o = self.tex_at + i * 0x20
        return cname(self.d, o), self.d[o:o + 0x20]

    def palette(self, i):
        o = self.pal_at + i * 0x18
        return cname(self.d, o), self.d[o:o + 0x18]


# F3DEX (1.x) -> F3DEX2 command translation. Every command is 8 bytes in both.
G1_VTX, G1_DL, G1_BRANCH_Z, G1_TRI2, G1_RDPHALF_1, G1_ENDDL = 0x04, 0x06, 0xB0, 0xB1, 0xB4, 0xB8
G1_OTHERMODE_L, G1_OTHERMODE_H, G1_TEXTURE, G1_TRI1, G1_SPNOOP = 0xB9, 0xBA, 0xBB, 0xBF, 0x00
G1_CLEARGEOMETRYMODE, G1_SETGEOMETRYMODE = 0xB6, 0xB7
GEOMETRY_BITS = [(0x200, 0x200000), (0x1000, 0x200), (0x2000, 0x400)]  # F3DEX -> F3DEX2 bits that moved


# Rush 1 lists switch the texture LUT and texture LOD and don't always switch them back; Rush 2 expects the LUT on
# RGBA16 (set once per frame, which its car lists rely on) and LOD off after a model. Every model list ends with:
RESTORE_STATE = struct.pack('>IIII', 0xE3001001, 0x8000, 0xE3000F00, 0)   # SETOTHERMODE_H TEXTLUT RGBA16, TEXTLOD TILE


def geometry_bits(m):
    out = m
    for a, b in GEOMETRY_BITS:
        out &= ~a
    for a, b in GEOMETRY_BITS:
        if m & a:
            out |= b
    return out


def translate(w0, w1):
    """One F3DEX command as F3DEX2 (pointers left as they are). Returns (w0, w1)."""
    op = w0 >> 24
    if op == G1_VTX:
        n = (w0 >> 10) & 0x3F
        v0 = ((w0 >> 16) & 0xFF) // 2
        if (w0 & 0x3FF) != n * 16 - 1 or n == 0 or v0 + n > 32:
            raise ConvertError('odd G_VTX %08X' % w0)
        return 0x01000000 | (n << 12) | ((v0 + n) << 1), w1
    if op == G1_TRI1:
        return 0x05000000 | (w1 & 0xFFFFFF), 0
    if op == G1_TRI2:
        return 0x06000000 | (w0 & 0xFFFFFF), w1
    if op in (G1_OTHERMODE_L, G1_OTHERMODE_H):
        sft, ln = (w0 >> 8) & 0xFF, w0 & 0xFF
        return ((0xE2 if op == G1_OTHERMODE_L else 0xE3) << 24) | ((32 - sft - ln) << 8) | (ln - 1), w1
    if op == G1_TEXTURE:
        return 0xD7000000 | (w0 & 0xFFFF00) | ((w0 & 0xFF) << 1), w1
    if op == G1_SETGEOMETRYMODE:
        return 0xD9FFFFFF, geometry_bits(w1)
    if op == G1_CLEARGEOMETRYMODE:
        return 0xD9000000 | (~geometry_bits(w1) & 0xFFFFFF), 0
    if op == G1_SPNOOP:
        return 0xE0000000, 0
    if op == G1_ENDDL:
        return 0xDF000000, 0
    if 0xE4 <= op <= 0xFF and op not in (0xE4, 0xE5):
        return w0, w1
    raise ConvertError('unsupported F3DEX command %08X %08X' % (w0, w1))


class Builder:
    """Builds a Rush 2 container from Rush 1 containers.

    Layout: 0x28-byte header, each source file verbatim (vertices, texels and palettes stay where they are, at the
    file's base), the texture-load lists ([7]..[8], which Rush 2's loader rebases linearly: SETTIMG only), the model
    lists, then the tables. Rush 2 rebases a model list by walking it without entering G_DL calls (VTX, DL, SETTIMG
    pointers), so each model's lists are flattened: called lists are inlined, except lists that only load textures,
    which stay calls into the texture-load range. A BRANCH_Z (near/far detail) always takes the near branch."""

    def __init__(self):
        self.out = bytearray(0x28)
        self.sources = []          # (Container, base)
        self.loads = {}            # (source, offset) -> index into load_lists
        self.load_lists = []       # [(bytes, [pointer fixups as (pos, source)])]
        self.models = []           # (name, lod_count, [(flags, dist, list bytes, fixups)], radius)
        self.textures = []         # (name, record bytes, data kind, data ref, palette name)
        self.palettes = []         # (name, record bytes, source)
        self.kinds = {}            # model name -> behaviour id (name record +0x14)

    def add_source(self, c):
        while len(self.out) % 8:
            self.out.append(0)
        self.sources.append((c, len(self.out)))
        self.out += c.d
        return len(self.sources) - 1

    def cmd(self, s, o):
        return struct.unpack_from('>II', self.sources[s][0].d, o)

    def is_load_list(self, s, o, depth=0):
        """A list that only sets RDP state, texture and other modes and loads textures (no vertices, no triangles)."""
        c = self.sources[s][0]
        for _ in range(4096):
            w0, w1 = self.cmd(s, o)
            op = w0 >> 24
            if op == G1_ENDDL:
                return True
            if op == G1_DL:
                if depth > 8 or not self.is_load_list(s, c.off(w1), depth + 1):
                    return False
                if (w0 >> 16) & 0xFF == 1:
                    return True
            elif op in (G1_VTX, G1_TRI1, G1_TRI2, G1_RDPHALF_1, G1_BRANCH_Z, 0xE4, 0xE5):
                return False
            o += 8
        return False

    def emit(self, s, o, out, fixups, loads_ok, depth=0):
        """Appends the flattened, translated list at source offset o (without its G_ENDDL) to out."""
        c = self.sources[s][0]
        if depth > 16:
            raise ConvertError('display lists nest too deep')
        for _ in range(65536):
            w0, w1 = self.cmd(s, o)
            op = w0 >> 24
            if op == G1_ENDDL:
                return
            if op == G1_DL:
                target = c.off(w1)
                if loads_ok and self.is_load_list(s, target):
                    key = (s, target)
                    if key not in self.loads:
                        body, fx = bytearray(), []
                        self.emit(s, target, body, fx, False, depth + 1)
                        body += struct.pack('>II', 0xDF000000, 0)
                        self.loads[key] = len(self.load_lists)
                        self.load_lists.append((body, fx))
                    fixups.append((len(out) + 4, ('load', self.loads[key])))
                    out += struct.pack('>II', 0xDE000000, 0)
                else:
                    self.emit(s, target, out, fixups, loads_ok, depth + 1)
                if (w0 >> 16) & 0xFF == 1:
                    return
                o += 8
                continue
            if op == G1_RDPHALF_1:
                nxt = self.cmd(s, o + 8)[0] >> 24
                if nxt != G1_BRANCH_Z:
                    raise ConvertError('RDPHALF_1 without BRANCH_Z at %X' % o)
                self.emit(s, c.off(w1), out, fixups, loads_ok, depth + 1)
                return
            nw0, nw1 = translate(w0, w1)
            if op in (G1_VTX, 0xFD):
                fixups.append((len(out) + 4, ('src', s, c.off(w1))))
                nw1 = 0
            out += struct.pack('>II', nw0, nw1)
            o += 8
        raise ConvertError('display list at %X has no end' % o)

    def add_model(self, s, i, name):
        c = self.sources[s][0]
        count, lods = c.model(i)
        out = []
        for flags, dist, dl in lods:
            if dl == 0:
                out.append((flags & 0x7, dist, None, None))
                continue
            body, fx = bytearray(), []
            self.emit(s, dl, body, fx, True)
            body += RESTORE_STATE + struct.pack('>II', 0xDF000000, 0)
            out.append((flags & 0x7, dist, body, fx))
        self.models.append((name, count, out, c.radius(i)))

    def vertices_of(self, model):
        """Output offsets of the vertices a model loads (G_VTX fixups into the sources' copies)."""
        at = set()
        for flags, dist, body, fx in model[2]:
            if body is None:
                continue
            for pos, ref in fx:
                if ref[0] != 'src' or pos < 4 or body[pos - 4] != 0x01:
                    continue
                n = (struct.unpack_from('>I', body, pos - 4)[0] >> 12) & 0xFF
                for k in range(n):
                    at.add(self.sources[ref[1]][1] + ref[2] + k * 16)
        return at

    def base_origin(self, placed, family):
        """Moves the models `family` (placed models with their animation frames and pieces, which share their origin)
        up so the lowest vertex of the `placed` ones is at y 0 (Rush 2's breakable models have their origin at the
        base); returns the shift in vertex units."""
        mine, base = set(), set()
        for name in sorted(family):
            model = [m for m in self.models if m[0] == name]
            if not model:
                raise ConvertError('no model %s to move' % name)
            v = self.vertices_of(model[-1])
            mine |= v
            if name in placed:
                base |= v
        if not base:
            return 0
        for m in self.models:
            if m[0] not in family and mine & self.vertices_of(m):
                raise ConvertError('model %s shares vertices with %s' % (m[0], min(family)))
        lowest = min(struct.unpack_from('>h', self.out, a + 2)[0] for a in base)
        for a in mine:
            struct.pack_into('>h', self.out, a + 2, struct.unpack_from('>h', self.out, a + 2)[0] - lowest)
        return -lowest

    def add_empty_model(self, name):
        self.models.append((name, 1, [(0, 0.0, struct.pack('>II', 0xDF000000, 0), []), (0, 0.0, None, None),
                                      (0, 0.0, None, None), (0, 0.0, None, None)], 0.0))

    def add_texture(self, s, i, name):
        c = self.sources[s][0]
        tname, rec = c.texture(i)
        data = u32(rec, 24)
        pal = s16(rec, 22)
        palname = c.palette(pal)[0] if 0 <= pal < c.n_pal else None
        target = c.off(data)
        if self.is_load_list(s, target):
            key = (s, target)
            if key not in self.loads:
                body, fx = bytearray(), []
                self.emit(s, target, body, fx, False)
                body += struct.pack('>II', 0xDF000000, 0)
                self.loads[key] = len(self.load_lists)
                self.load_lists.append((body, fx))
            ref = ('load', self.loads[key])
        else:
            ref = ('src', s, target)
        self.textures.append((name, bytearray(rec), ref, palname, s))

    def add_palette(self, s, name, newname):
        c = self.sources[s][0]
        for i in range(c.n_pal):
            n, rec = c.palette(i)
            if n == name:
                self.palettes.append((newname, bytearray(rec), s))
                return
        raise ConvertError('no palette ' + name)

    def build(self):
        out = self.out
        while len(out) % 8:
            out.append(0)
        load_at = []
        start7 = len(out)
        for body, fx in self.load_lists:
            load_at.append(len(out))
            out += body
        end8 = len(out)

        def resolve(ref):
            if ref[0] == 'load':
                return load_at[ref[1]]
            return self.sources[ref[1]][1] + ref[2]

        for i, (body, fx) in enumerate(self.load_lists):
            for pos, ref in fx:
                struct.pack_into('>I', out, load_at[i] + pos, resolve(ref))
        models = sorted(self.models, key=lambda m: m[0].encode()[:15])
        recs = []
        for name, count, lods, radius in models:
            rec = bytearray(struct.pack('>I', count))
            for flags, dist, body, fx in lods:
                at = 0
                if body is not None:
                    at = len(out)
                    out += body
                    for pos, ref in fx:
                        struct.pack_into('>I', out, at + pos, resolve(ref))
                rec += struct.pack('>HHfI', 0, flags, dist, at)
            recs.append(rec)
        names = [m[0] for m in models]
        if len(set(n.encode()[:15] for n in names)) != len(names):
            raise ConvertError('duplicate model names')
        textures = sorted(self.textures, key=lambda t: t[0].encode())
        palettes = sorted(self.palettes, key=lambda p: p[0].encode())
        pal_names = [p[0] for p in palettes]
        model_at = len(out)
        for rec in recs:
            out += rec
        tex_at = len(out)
        for name, rec, ref, palname, s in textures:
            r = bytearray(rec)
            r[0:16] = name.encode().ljust(16, b'\0')
            struct.pack_into('>H', r, 20, 0x0002 if u16(rec, 20) == 0 else u16(rec, 20))
            struct.pack_into('>h', r, 22, pal_names.index(palname) if palname in pal_names else -1)
            struct.pack_into('>I', r, 24, resolve(ref))
            struct.pack_into('>I', r, 28, 0)
            out += r
        pal_at = len(out)
        for name, rec, s in palettes:
            r = bytearray(rec)
            r[0:16] = name.encode().ljust(16, b'\0')
            struct.pack_into('>I', r, 20, self.sources[s][1] + self.sources[s][0].off(u32(rec, 20)))
            out += r
        name_at = len(out)
        for name, count, lods, radius in models:
            out += name.encode().ljust(16, b'\0') + struct.pack('>fHH', radius, self.kinds.get(name, 0), 0)
        struct.pack_into('>10I', out, 0, model_at, name_at, tex_at, pal_at, len(models), len(textures),
                         len(palettes), start7, end8, 0)
        return bytes(out)


def object_class(name):
    for c in R1_CLASSES:
        if name.startswith(c[0]):
            return c
    return None


def placed_objects(r, t):
    d = r.asset(PLACEMENT + t)
    base = u32(d, 4)
    return {cname(d, o) for o in range(base, len(d) - 0x63, 0x64)}


def piece_models(names):
    out = {}
    for r2, r1, count, shift in R1_PIECES:
        for k in range(1, count + 1):
            if r1 + str(k + shift) in names:
                out[r2 + str(k)] = names[r1 + str(k + shift)]
    for k in range(1, 5):
        if 'TREEHIT%dL1' % k in names:
            out['TREEHIT%dO1' % k] = names['TREEHIT%dL1' % k]
    return out


def convert_geometry(r, t, prefix):
    track = Container(r.asset(GEOMETRY + t) + r.asset(TEXTURE_BANK))
    objects = Container(r.asset(OBJECTS))
    finish = r.s(r.w(FINISH_MODELS + t * 4))
    b = Builder()
    st = b.add_source(track)
    so = b.add_source(objects)
    names = {}
    for i in range(track.n_models):
        n = track.name(i)
        new = prefix + 'FINISH' if n == finish else safe_name(n)
        names[n] = new
        b.add_model(st, i, new)
    if finish not in names:
        raise ConvertError('finish model %s missing' % finish)
    for n in R1_SHARED_OBJECTS:
        if n in names:
            continue
        for i in range(objects.n_models):
            if objects.name(i) == n:
                names[n] = safe_name(n)
                b.add_model(so, i, names[n])
        if n not in names:
            raise ConvertError('object model %s missing' % n)
    b.kinds[names[R1_KEY_MODEL]] = KEY_BEHAVIOUR
    shifts = {}
    families = {}
    for n in sorted(placed_objects(r, t)):
        c = object_class(n)
        if c is not None and n in names:
            b.kinds[names[n]] = c[2]
            families.setdefault(n.rstrip('0123456789'), set()).add(n)
    for f, placed in sorted(families.items()):
        family = {new for old, new in names.items() if old.rstrip('0123456789') == f}
        shift = b.base_origin({names[n] for n in placed}, family)
        for n in placed:
            shifts[n] = shift
    b.add_empty_model('R1EMPTY')
    for i in range(track.n_tex):
        if track.texture(i)[0] == 'CHKPOINT':
            b.add_texture(st, i, 'CHKPNT')
            b.add_palette(st, 'CHKPOINT', 'CHKPOINT')
    for i in range(objects.n_tex):
        if objects.texture(i)[0] == 'FINISH':
            b.add_texture(so, i, 'FINISH')
            b.add_palette(so, 'FINISH', 'FINISH')
    if len(b.textures) != 2:
        raise ConvertError('CHKPOINT/FINISH textures missing')
    return b.build(), names, shifts


# ---------------------------------------------------------------------------------------------------------------------
# Placement

def convert_placement(r, t, prefix, names, record_models=None, shifts=None):
    """Rush 1 placement (Rush 2's record layout, 0x64 bytes; children positions relative to their unrotated parent)
    as a Rush 2 placement with tree name `prefix`. The top-level chain (the track sections; their order is the
    visibility region order) is kept as it is."""
    if record_models is None:
        record_models = {}
    d = r.asset(PLACEMENT + t)
    if u32(d, 0) != 1:
        raise ConvertError('placement has %d trees' % u32(d, 0))
    base = u32(d, 4)
    rec = lambda i: d[base + i * 0x64:base + (i + 1) * 0x64]
    name = lambda i: cname(rec(i), 0)
    nxt = lambda i: s16(rec(i), 0x44)
    child = lambda i: s16(rec(i), 0x46)
    top = []
    i = 0
    while i >= 0:
        top.append(i)
        i = nxt(i)
    out_recs = []   # (record bytes, children list of record bytes)
    keys = set()
    for i in top:
        if name(i) not in names:
            raise ConvertError('section %s has no model' % name(i))
        r0 = bytearray(rec(i))
        r0[0:16] = names[name(i)].encode().ljust(16, b'\0')
        struct.pack_into('>I', r0, 0x40, u32(r0, 0x40) & ~0x1000)
        struct.pack_into('>I', r0, 0x48, 0)
        if struct.unpack_from('>9f', r0, 0x10) != (1, 0, 0, 0, 1, 0, 0, 0, 1) and child(i) >= 0:
            raise ConvertError('rotated section %s has children' % name(i))
        ppos = struct.unpack_from('>3f', r0, 0x34)
        kids = []
        c = child(i)
        while c >= 0:
            if child(c) >= 0:
                raise ConvertError('nested children under %s' % name(i))
            k = bytearray(rec(c))
            n = name(c)
            world = False
            if n in R1_EMITTERS:
                new, world = R1_EMITTERS[n], True
            elif n.startswith(R1_KEY):
                number = s16(k, 0x4A)
                if not 1 <= number <= R1_KEYS_MAX or number in keys:
                    raise ConvertError('key %s has number %d' % (n, number))
                keys.add(number)
                new, world = 'KEY%d' % number, True
                record_models[new] = names[R1_KEY_MODEL]
            elif any(n.startswith(p) for p in R1_DROPPED):
                new = None
            else:
                if n not in names:
                    raise ConvertError('object %s has no model' % n)
                new = names[n]
                oc = object_class(n)
                if oc is not None:
                    record = oc[1] + n[len(oc[0]):] if oc[1] else n
                    if len(record) > 15 or not r2_classified(record):
                        raise ConvertError('no class record for ' + n)
                    record_models[record] = names[n]
                    new, world = record, True
            if new is not None:
                k[0:16] = new.encode().ljust(16, b'\0')
                if world:
                    pos = struct.unpack_from('>3f', k, 0x34)
                    down = (shifts or {}).get(n, 0) / 16.0
                    struct.pack_into('>3f', k, 0x34, *(a + b - (down if i == 1 else 0.0)
                                                       for i, (a, b) in enumerate(zip(pos, ppos))))
                struct.pack_into('>I', k, 0x40, (u32(k, 0x40) & ~0x1000) if not world else 0x40)
                struct.pack_into('>I', k, 0x48, 0)
                struct.pack_into('>6f', k, 0x4C, 0, 0, 0, 0, 0, 0)
                kids.append(k)
            c = nxt(c)
        out_recs.append((r0, kids))
    # Pre-order: the top-level chain, then each section's children.
    n_top = len(out_recs)
    first_child = []
    at = n_top
    for r0, kids in out_recs:
        first_child.append(at if kids else -1)
        at += len(kids)
    body = bytearray()
    for k, (r0, kids) in enumerate(out_recs):
        struct.pack_into('>hh', r0, 0x44, k + 1 if k + 1 < n_top else -1, first_child[k])
        body += r0
    for k, (r0, kids) in enumerate(out_recs):
        for j, kid in enumerate(kids):
            struct.pack_into('>hh', kid, 0x44, first_child[k] + j + 1 if j + 1 < len(kids) else -1, -1)
            body += kid
    head = struct.pack('>II', 1, 0x18) + prefix.encode().ljust(16, b'\0')
    return head + bytes(body)


# ---------------------------------------------------------------------------------------------------------------------
# Collision

def f32s(d, o, n): return list(struct.unpack_from('>%df' % n, d, o))


def to_render(c):
    """Rush 1 collision space -> render (Rush 2) space: collision (c0, c1, c2) = render (z, x, -y)."""
    return [c[1], -c[2], c[0]]


def convert_collision(d):
    """Rush 1 collision (0x20-byte header, polygons of 0x1A bytes, collision space (z, x, -y), polygon frames with the
    surface in local x/y and depth along local z) as Rush 2's (0xC-byte header, 0x18-byte polygons, render space,
    surface in local z/x and height along local y)."""
    h = struct.unpack_from('>16H', d, 0)
    ns, nn, np_, nv, lb, vb = h[:6]
    wrap = h[14]
    o = 0x20
    segs = d[o:o + ns * 0x84]; o += ns * 0x84
    nodes = d[o:o + nn * 0x14]; o += nn * 0x14
    polys = d[o:o + np_ * 0x1A]; o += np_ * 0x1A
    verts = d[o:o + nv * 8]; o += nv * 8
    leaf = d[o:o + lb]; o += lb
    vlist = d[o:o + vb]; o += vb
    if o != len(d):
        raise ConvertError('collision size mismatch')
    # A = render <- collision; B = Rush 2 local <- Rush 1 local: (y, -z, x). Rush 1 multiplies by the transpose of its
    # stored matrices (local = M^T (p - origin)), Rush 2 by the matrix itself, so M2 = B M1^T A^-1.
    A = [[0, 1, 0], [0, 0, -1], [1, 0, 0]]          # render = A @ coll
    Ainv = [[0, 0, 1], [1, 0, 0], [0, -1, 0]]       # coll = Ainv @ render
    B = [[0, 1, 0], [0, 0, -1], [1, 0, 0]]

    def mat(M, N):
        return [[sum(M[i][k] * N[k][j] for k in range(3)) for j in range(3)] for i in range(3)]

    # Segments: origin and the lerped vectors are world points/directions; the matrix maps world to segment frame.
    out_segs = bytearray()
    seg_list = [segs[i * 0x84:(i + 1) * 0x84] for i in range(ns)]
    if wrap:
        seg_list.append(seg_list[wrap])   # Rush 1 wraps the last segment to `wrap`, Rush 2 to 0: give it its own next
    for s in seg_list:
        f = f32s(s, 0, 33)
        g = list(f)
        g[0:3] = to_render(f[0:3])
        M = [[f[3 + 3 * j + i] for j in range(3)] for i in range(3)]   # Rush 1 stores it transposed (func_80078414)
        M2 = mat(mat(B, M), Ainv)
        g[3:12] = [x for row in M2 for x in row]
        for at in (12, 15, 18):
            g[at:at + 3] = to_render(f[at:at + 3])
        out_segs += struct.pack('>33f', *g)
    out_polys = bytearray()
    new_verts = bytearray(verts)
    local = set()
    for i in range(np_):
        p = polys[i * 0x1A:(i + 1) * 0x1A]
        flags, info = struct.unpack_from('>HH', p, 0)
        m = struct.unpack_from('>9h', p, 6)
        voff = u16(p, 0x18)
        idx, _, _ = decode_vlist(vlist, voff, info & 0xF)
        M = [[m[3 * b + a] for b in range(3)] for a in range(3)]      # Rush 1 stores it transposed (func_80079B90)
        M2 = mat(mat(B, M), Ainv)
        out_polys += struct.pack('>HH9hH', flags, info, *[x for row in M2 for x in row], voff)
        for k in idx[1:]:
            local.add(k)
        world = idx[0]
        if world in local:
            raise ConvertError('vertex %d is both a polygon origin and a local vertex' % world)
    # World origins map collision -> render space and local vertices Rush 1 local -> Rush 2 local; both are (y, -z, x).
    for k in range(nv):
        x, y, z = decode_vert(verts, k)
        struct.pack_into('>hhhH', new_verts, k * 8, *encode_vert(y, -z, x))
    if len(seg_list) > 0xFFFF or lb > 0xFFFF or vb > 0xFFFF:
        raise ConvertError('collision too large')
    head = struct.pack('>6H', len(seg_list), nn, np_, nv, lb, vb)
    return head + bytes(out_segs) + nodes + bytes(out_polys) + bytes(new_verts) + leaf + vlist


def decode_vlist(buf, off, count):
    out = []
    while count > 0:
        v = (buf[off] << 8) | buf[off + 1]; off += 2
        run = 0
        if count >= 2 and buf[off] >= 0xC0:
            run = buf[off] & 0x3F; off += 1
        count -= run + 1
        out += list(range(v, v + run + 1))
    return out, off, None


def decode_vert(v, k):
    x, y, z, f = struct.unpack_from('>hhhH', v, k * 8)
    return x * 32 + ((f >> 10) & 31), y * 32 + ((f >> 5) & 31), z * 32 + (f & 31)   # in 1/32 units


def encode_vert(x, y, z):
    """Values in 1/32 units -> s16 integer parts and the packed fractions."""
    ix, iy, iz = x >> 5, y >> 5, z >> 5
    if not all(-32768 <= a < 32768 for a in (ix, iy, iz)):
        raise ConvertError('collision vertex out of range')
    return ix, iy, iz, ((x & 31) << 10) | ((y & 31) << 5) | (z & 31)


# ---------------------------------------------------------------------------------------------------------------------
# AI path

def lane_mph(fps):
    """Rush 1's lane speeds are in ft/s (its driver, func_8007CDD4, compares lane x 1.05 with the car's speed); Rush 2's
    are in mph (x 1.4667 = 22 / 15, func_80074990). Rounded, and a moving point stays moving."""
    return 0 if fps == 0 else max(1, (fps * 30 + 22) // 44)


def lanes(d):
    o = 0; out = []
    for l in range(4):
        n, loop, end, x = struct.unpack_from('>4h', d, o)
        pts = []
        for k in range(n):
            c0, c1, c2, sp, be, ix = struct.unpack_from('>hhhBBh', d, o + 8 + k * 10)
            pts.append((c1, -c2, c0, lane_mph(sp), be))
        out.append(dict(loop=loop, end=end, points=pts)); o += 8 + n * 10
    if o != len(d):
        raise ConvertError('path size mismatch')
    return out


def checkpoints(r, t, backward):
    p = r.w(CHECKPOINTS[backward] + t * 4); out = []
    for i in range(13):
        d = r.read(p + i * 0x18, 0x18)
        x, y, z, w = struct.unpack('>4f', d[:16]); fl, a, b, c = struct.unpack('>4h', d[16:24])
        if fl == -1:
            return out, b
        out.append(dict(pos=(x, y, z), flags=fl, times=(a, b, c)))
    raise ConvertError('checkpoint list has no end')


def path_branches(spine, lanes, d2):
    """Branches for the lanes' own routes. Rush 2 tracks a car on the spine, and only looks at branches when the car
    is over 40 ft from it (func_80090570); its wrong-way check, respawn point and checkpoint window all come from
    that. Rush 1 tracks 4 and 5 have alternate routes that only some lanes drive: each stretch of a lane over 40 ft
    from the spine and the branches so far that gets over 100 ft from them (an alternate route, not a lane on the
    far side of a wide road) becomes a branch, extended until it is back within 20 ft (at most 30 points each way).
    Rush 2 links branch ends to the nearest spine or branch point at load (func_80092060) and walks branches forward
    when it moves a respawn point on, so a stretch that would rejoin the spine behind where it left (the lanes
    closing their lap on track 6) is left out."""
    out = []
    for pts in lanes[1:]:
        ref = [spine] + out
        dd = [min(d2(p, q) for r_ in ref for q in r_) for p in pts]
        k = 0
        while k < len(pts):
            if dd[k] <= 1600:
                k += 1
                continue
            a = k
            while k < len(pts) and dd[k] > 1600:
                k += 1
            b = k - 1
            if max(dd[a:b + 1]) <= 10000:
                continue
            s = a
            while s > 0 and a - s < 30 and dd[s] > 400:
                s -= 1
            e = b
            while e < len(pts) - 1 and e - b < 30 and dd[e] > 400:
                e += 1
            leave = min(range(len(spine)), key=lambda j: d2(spine[j], pts[s]))
            rejoin = min(range(len(spine)), key=lambda j: d2(spine[j], pts[e]))
            if 0 < rejoin - leave < len(spine) // 2 and len(out) < r2paths.MAX_BRANCH:
                out.append(pts[s:e + 1])
    return out


GATE_RADII = (300, 250, 200, 160, 130, 100, 80)
GATE_MARGIN = 30


def widen_gates(gates, paths, es):
    """Rush 1 counts a checkpoint once the car's place on the track passes it, however far to the side the car is
    (func_8009FFCC); Rush 2's gate also needs the car within its radius, and a missed gate keeps the car's checkpoint
    window (and so its respawn point) behind it. Each gate gets the largest of GATE_RADII that keeps every crossing
    where it was and that no path crosses (within the radius plus a margin) on its way from the previous gate."""
    if len(gates) < 2:
        return

    def ok(i):
        g = gates[i]
        prev = gates[i - 1]
        lim = (math.sqrt(g['r2']) + GATE_MARGIN) ** 2
        for pts, lp, l in paths:
            c = r2paths.crossing(g, pts)
            if abs(c - (g['k'] if l is None else es[i][l])) > 40:
                return False
            j = r2paths.crossing(prev, pts)
            for _ in range(len(pts)):
                nx = j + 1 if j + 1 < len(pts) else lp
                if nx == c:
                    break
                p, q = pts[j], pts[nx]
                sp = (p[0] - g['pos'][0]) * g['dir'][0] + (p[2] - g['pos'][2]) * g['dir'][2]
                sq = (q[0] - g['pos'][0]) * g['dir'][0] + (q[2] - g['pos'][2]) * g['dir'][2]
                if (sp < 0) != (sq < 0):
                    dp = (p[0] - g['pos'][0]) ** 2 + (p[2] - g['pos'][2]) ** 2
                    dq = (q[0] - g['pos'][0]) ** 2 + (q[2] - g['pos'][2]) ** 2
                    if min(dp, dq) <= lim:
                        return False
                j = nx
            else:
                return False
        return True

    base = [g['r2'] for g in gates]
    for i, g in enumerate(gates):
        for rad in GATE_RADII:
            if rad * rad <= base[i]:
                break
            g['r2'] = rad * rad
            if ok(i):
                break
            g['r2'] = base[i]
    # Widening a gate moves its crossings, which bound the next gate's stretch: put back any gate that no longer holds.
    changed = True
    while changed:
        changed = False
        for i, g in enumerate(gates):
            if g['r2'] != base[i] and not ok(i):
                g['r2'] = base[i]
                changed = True


def convert_path(r, t, backward):
    """Rush 1 lanes (the race loop ends at point `end` and continues at point `loop`) and checkpoint list as a Rush 2
    path: spine = lane 0, lanes = the four lanes, one lap each ([0, end)), checkpoints on the spine with Rush 2 flags
    (2 arm on the first, 4 on Rush 1's loop start, 1 on Rush 1's lap line)."""
    L = lanes(r.asset((PATH_BACK if backward else PATH) + t))
    cps, loop_cp = checkpoints(r, t, backward)
    if not 1 <= len(cps) <= 10:
        raise ConvertError('%d checkpoints' % len(cps))
    lane_pts, loops = [], []
    for ln in L:
        end = ln['end'] if 2 <= ln['end'] <= len(ln['points']) else len(ln['points'])
        lane_pts.append([p for p in ln['points'][:end]])
        loops.append(ln['loop'] if 0 <= ln['loop'] < end else 0)
    spine = [p[:3] for p in lane_pts[0]]
    n = len(spine)
    lane_xyz = [[q[:3] for q in pts] for pts in lane_pts]

    def d2(p, q):
        return (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2])

    branches = path_branches(spine, lane_xyz, d2)

    def nearest(pts, pos, lo, hi):
        return min(range(lo, max(hi, lo + 1)), key=lambda j: d2(pts[j], pos))

    # Checkpoints in race order: each one's nearest spine and lane points after the previous checkpoint's (tracks
    # pass some places twice), the first in the first half.
    ks, es = [], []
    for i, c in enumerate(cps):
        lo = 0 if i == 0 else ks[-1] + 1
        ks.append(nearest(spine, c['pos'], lo, n // 2 if i == 0 else n))
        es.append([nearest(pts, c['pos'], 0 if i == 0 else es[-1][l] + 1, len(pts) // 2 if i == 0 else len(pts))
                   for l, pts in enumerate(lane_xyz)])
    # Gates: Rush 2 finds a path's crossing as the first point (from index 0) where the path changes sides of the
    # gate plane within the radius (func_80092884). A gate sits on a spine point, as wide as its lanes need; where a
    # path passes the gate elsewhere first, the gate moves along the spine until every crossing is the intended one.
    out_cps = []
    for i, c in enumerate(cps):
        flags = (2 if i == 0 else 0) | (4 if i == loop_cp else 0) | (1 if c['flags'] & 2 else 0)
        found = None
        for step in range(41):
            kk = ks[i] + ((step + 1) // 2) * (1 if step % 2 else -1)
            if not 0 <= kk < n:
                continue
            a, b = spine[max(kk - 2, 0)], spine[min(kk + 2, n - 1)]
            dx, dz = b[0] - a[0], b[2] - a[2]
            ln_ = math.sqrt(dx * dx + dz * dz) or 1.0
            gate = dict(pos=tuple(float(x) for x in spine[kk]), dir=(dx / ln_, 0.0, dz / ln_), r2=0, flags=flags,
                        time1=45, time2=45, cross=[-1] * 20, dist=0.0)
            rad = 60.0
            for l, pts in enumerate(lane_xyz):
                e = es[i][l]
                best = None
                for j in range(max(e - 40, 0), min(e + 40, len(pts))):
                    p, q = pts[j], pts[(j + 1) % len(pts)]
                    sp = (p[0] - gate['pos'][0]) * gate['dir'][0] + (p[2] - gate['pos'][2]) * gate['dir'][2]
                    sq = (q[0] - gate['pos'][0]) * gate['dir'][0] + (q[2] - gate['pos'][2]) * gate['dir'][2]
                    if (sp < 0) != (sq < 0):
                        px, pz = p[0] - gate['pos'][0], p[2] - gate['pos'][2]
                        qx, qz = q[0] - gate['pos'][0], q[2] - gate['pos'][2]
                        dd = max(math.sqrt(px * px + pz * pz), math.sqrt(qx * qx + qz * qz))
                        best = dd if best is None else min(best, dd)
                if best is not None:
                    rad = max(rad, best + 10)
            rad = min(rad, 600.0)
            gate['r2'] = int(rad * rad)
            ok = abs(r2paths.crossing(gate, spine) - kk) <= 40
            for l, pts in enumerate(lane_xyz):
                ok = ok and abs(r2paths.crossing(gate, pts) - es[i][l]) <= 40
            if ok:
                found = gate
                break
        if found is None:
            raise ConvertError('no gate for checkpoint %d' % i)
        found['k'] = kk
        out_cps.append(found)
    widen_gates(out_cps, [(spine, loops[0], None)] + [(pts, loops[l], l) for l, pts in enumerate(lane_xyz)], es)
    for g in out_cps:
        del g['k']
    p = dict(base_time=90, loop_cp=0, finish_cp=0, arm_cp=0, n_cp=len(out_cps), hdr_pad=0, cps=out_cps,
             spine=[tuple(int(x) for x in s) for s in spine],
             branches=[dict(alt=0, points=[tuple(int(x) for x in q) for q in b]) for b in branches],
             lanes=[dict(points=[(int(q[0]), int(q[1]), int(q[2]), q[3], q[4]) for q in pts], unk2=0, unk3=0)
                    for pts in lane_pts], raw=b'')
    data = r2paths.build(p, clear_runtime=True)
    demo = [0, n // 4, n // 2, 3 * n // 4]
    return data, demo


# ---------------------------------------------------------------------------------------------------------------------

def pvs(r, t):
    count = r.read(PVS_COUNTS + t, 1)[0]
    return r.read(PVS_TABLES[t], count * 16), count


def convert(r, t, prefix='HAWAII', backward=False):
    geometry, names, shifts = convert_geometry(r, t, prefix)
    placement = convert_placement(r, t, prefix, names, None, shifts)
    collision = convert_collision(r.asset((COLLISION_BACK if backward else COLLISION) + t))
    path, demo = convert_path(r, t, backward)
    table, count = pvs(r, t)
    return dict(geometry=geometry, placement=placement, collision=collision, path=path, pvs=table, pvs_count=count,
                demo=demo)


if __name__ == '__main__' and len(sys.argv) == 1:
    r = Rush1()
    for t in range(7):
        for b in (0, 1):
            res = convert(r, t, 'HAWAII', bool(b))
            p = r2paths.parse(res['path'])
            err = r2paths.validate(p)
            rc = r2paths.recompute(p)
            print('track', t + 1, 'bwd' if b else 'fwd', 'geometry', hex(len(res['geometry'])), 'placement',
                  hex(len(res['placement'])), 'collision', hex(len(res['collision'])), 'path', hex(len(res['path'])),
                  'pvs', res['pvs_count'], 'errors', err)
            print('   cps', [(c['flags'], int(math.sqrt(c['r2']))) for c in p['cps']],
                  'cross', [row[:5] for row in rc['cross']])


def check_geometry(g):
    """Rush 2 container checks: every model list walks (as Rush 2's relocator does) to its G_ENDDL through valid
    F3DEX2 commands; pointers stay in the file; G_DL calls land in the texture-load range, whose lists only hold
    state and texture loads."""
    err = []
    h = struct.unpack_from('>10I', g, 0)
    model_at, name_at, tex_at, pal_at, nm, nt, np_, s7, e8, _ = h
    names = [cname(g, name_at + i * 0x18) for i in range(nm)]
    if [n.encode()[:15] for n in names] != sorted(n.encode()[:15] for n in names):
        err.append('names not sorted')
    ok_ops = {0x01, 0x05, 0x06, 0xD7, 0xD9, 0xDE, 0xDF, 0xE0, 0xE2, 0xE3} | set(range(0xE6, 0x100))
    load_ops = {0xD7, 0xDF, 0xE0, 0xE2, 0xE3} | set(range(0xE6, 0x100))

    def walk(o, ops, what):
        for _ in range(200000):
            if o + 8 > len(g):
                err.append('%s runs off the file' % what); return
            w0, w1 = struct.unpack_from('>II', g, o)
            op = w0 >> 24
            if op not in ops:
                err.append('%s: bad op %08X at %X' % (what, w0, o)); return
            if op == 0x01:
                cnt = (w0 >> 12) & 0xFF
                if w1 + cnt * 16 > len(g): err.append('%s: VTX out of file' % what)
            if op == 0xDE and not s7 <= w1 < e8: err.append('%s: DL call outside the load range' % what)
            if op == 0xFD and w1 >= len(g): err.append('%s: SETTIMG out of file' % what)
            if op == 0xDF: return
            o += 8
    for i in range(nm):
        for l in range(4):
            dl = u32(g, model_at + i * 0x34 + 4 + l * 12 + 8)
            if dl:
                walk(dl, ok_ops, names[i])
    o = s7
    while o < e8:
        w0 = u32(g, o)
        if (w0 >> 24) not in load_ops: err.append('load range: bad op %08X at %X' % (w0, o))
        o += 8
    for i in range(nt):
        r = g[tex_at + i * 0x20:tex_at + i * 0x20 + 0x20]
        if not s7 <= u32(r, 24) < e8: err.append('texture %s data outside the load range' % cname(r, 0))
    return err, set(names)


def check_placement(pl, names, shared):
    err = []
    base = u32(pl, 4)
    for o in range(base, len(pl), 0x64):
        n = cname(pl, o)
        if n in R2_EXACT or any(n.startswith(p) for p in ('MARKER', 'TIME', 'COLLISION')):
            continue
        cls = [p for p in R2_PREFIXES if n.startswith(p)]
        if cls:
            continue   # a Rush 2 class: its model is Rush 2's (shared assets)
        if n not in names and n not in shared:
            err.append('placement name %s has no model' % n)
    return err


def selfcheck():
    import roms
    r = Rush1()
    r2 = roms.Rush2()
    shared = set()
    for a in (0x12, 0x14):
        d = r2.asset(a); h = struct.unpack_from('>10I', d, 0)
        shared |= {cname(d, h[1] + i * 0x18) for i in range(h[4])}
    bad = 0
    for t in range(7):
        for b in (0, 1):
            res = convert(r, t, 'HAWAII', bool(b))
            ge, names = check_geometry(res['geometry'])
            pe = check_placement(res['placement'], names, shared)
            p = r2paths.parse(res['path'])
            e = ge + pe + r2paths.validate(p)
            clash = names & shared
            if clash: e.append('names clash with Rush 2 shared models: %s' % sorted(clash))
            bad += len(e)
            print('track', t + 1, 'bwd' if b else 'fwd', 'OK' if not e else e[:6])
    print('ALL OK' if not bad else '%d problems' % bad)


if __name__ == '__main__' and len(sys.argv) > 1 and sys.argv[1] == 'check':
    selfcheck()
