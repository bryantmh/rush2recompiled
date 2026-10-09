"""Model containers of Rush 2 and Rush 2049, and conversion of a 2049 track geometry file to a Rush 2 one.

Findings and evidence are in docs/rush2049_research/geometry.md. Run `python model.py` from tools/rush2049 for the
self-check over the Rush 2 tracks and cars and the 2049 tracks (101-106) and track object files (82-87).

Rush 2 container (all offsets file relative; the loader func_80077CC0 adds the load address):
    header  10 u32: [0] model table, [1] name table, [2] texture table, [3] palette table, [4] model/name count,
            [5] texture count, [6] palette count, [7] start and [8] end of the texture-load display lists, [9] 0
    model   0x34 bytes: u32 lod count (1..4), then 4 x {u16 texture handle, u16 flags, f32 distance, u32 dl}
    name    0x18 bytes: char name[16], f32 radius, u16 kind (breakable class), u16 0. Sorted (binary search over
            15 characters), name i <-> model i.
    texture 0x20 bytes: char name[16], u16 w, u16 h, u8 fmt, u8 siz, s16 palette index, u32 texels or load list,
            u32 flags
    palette 0x18 bytes: char name[16], u32 0x000F8000 / 0x00FFC000, u32 palette data
Rush 2049 container: word 0 = offset of {tag, offset, size or count} entries: IMAG, TXLD, OBHD (0x58), PLHD (0x18),
    TXHD (0x24), OBJS, and for tracks PATH (bytes) and PTHD (0x24 records).
    OBHD    char name[16] (stale bytes after the NUL), f32 radius, u16 kind, s16 lod count,
            4 x {u16 texture handle, u16 flags, f32 distance, u32 dl, u32 vertices}
    TXHD    the Rush 2 texture record + u32; PLHD = the Rush 2 palette record. TXLD SETTIMG and TXHD/PLHD data
            pointers are IMAG-relative, everything else file-relative.
"""
import struct, sys
from collections import Counter

# ---------------------------------------------------------------------------------------------------------------
# Small helpers

def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def u16(d, o): return struct.unpack_from('>H', d, o)[0]
def f32(d, o): return struct.unpack_from('>f', d, o)[0]
def cstr(b): return b.split(b'\0')[0].decode('latin1')


# Conditional display-list op shared by both games (relocators func_8007786C / func_80096734):
#   E0 01 cccc  tttttttt  "unless load flag bit c is set, branch (G_DL no-push) to t".
# Load flags: bit 1 = one player, bit 2 = two players, bit 3 = not mirrored, bit 4 = mirrored.
COND_NAMES = {1: '1P', 2: '2P', 3: 'NORMAL', 4: 'MIRROR'}


def load_flags(players=1, mirror=False):
    f = 0
    if players == 1: f |= 2
    if players == 2: f |= 4
    f |= 0x10 if mirror else 0x08
    return f


class DLError(Exception):
    pass


def _reloc_kind(d, o, op):
    """Which commands the shared relocator rebases (func_8007786C/func_80096734): VTX, LOAD_UCODE, DL, MTX,
    MOVEMEM, RDPHALF_1 when followed by BRANCH_Z or LOAD_UCODE, and SETTIMG/SETZIMG/SETCIMG."""
    if op in (0x01, 0xDD, 0xDE, 0xDA, 0xDC) or op >= 0xFD:
        return True
    if op == 0xE1 and o + 8 < len(d):
        return d[o + 8] in (0x04, 0xDD)
    return False


def _skipped(op):
    """Ops the relocator ignores entirely (0x09-0xBF and 0xC0-0xD5)."""
    return (op & 0xC0) in (0x40, 0x80) or (9 <= op < 0x40) or (0xC0 <= op < 0xD6)


def walk_reloc(d, start, end=None, flags=None):
    """Walks one model display list exactly as the load-time relocator does and returns
    (pointer word offsets it rebases, conditional ops [(offset, cond, target)], first VTX address or None).
    The relocator does not follow G_DL, only advances past it; BRANCH_Z jumps to the RDPHALF_1 address
    (abandoning the fall-through); G_ENDDL ends the walk."""
    end = len(d) if end is None else end
    ptrs, conds = [], []
    first_vtx = None
    o = start
    half1 = None
    steps = 0
    while True:
        steps += 1
        if steps > 200000 or o < 0 or o + 8 > end:
            raise DLError('display list at %#x runs off the data (at %#x)' % (start, o))
        op = d[o]
        if _skipped(op):
            o += 8
            continue
        if _reloc_kind(d, o, op):
            ptrs.append(o + 4)
        if op == 0x01 and first_vtx is None:
            first_vtx = u32(d, o + 4) & 0xFFFFFF
        if op == 0xE0 and d[o + 1] == 0x01:
            conds.append((o, u16(d, o + 2), u32(d, o + 4) & 0xFFFFFF))
        if op == 0xE1:
            half1 = u32(d, o + 4) & 0xFFFFFF
        if op == 0x04:
            if half1 is None:
                raise DLError('BRANCH_Z without RDPHALF_1 at %#x' % o)
            o = half1
            continue
        if op == 0xDF:
            return ptrs, conds, first_vtx
        o += 8


def vtx_chain(d, addr):
    """Vertices from addr while the flag halfword has bit 15 (func_80077810 / func_800966D8)."""
    n = 0
    while True:
        if addr + 16 > len(d):
            raise DLError('vertex chain at %#x runs off the data' % addr)
        fl = u16(d, addr + 6)
        n += 1
        if not fl & 0x8000:
            return n
        addr += 16


class Checker:
    """Validates display lists the way the game will see them after relocation: every pointer the relocator
    rebases (plus those in texture-load lists) must land inside the file, and anything reached through G_DL
    must itself be fully relocated by someone."""

    def __init__(self, d, tex_ranges, base_of_tex_ptrs=0):
        self.d = d
        self.tex_ranges = tex_ranges          # [(start, end)] of texture-load lists (rebased by a linear scan)
        self.tex_base = base_of_tex_ptrs      # value added to SETTIMG in texture-load lists (2049: IMAG offset)
        self.stats = Counter()
        self.errors = []
        self.top = set()                      # starts of model display lists
        self.relocated = set()                # pointer words the relocator rebases

    def in_tex(self, o):
        return any(a <= o < b for a, b in self.tex_ranges)

    def check_ptr(self, where, addr, size):
        if addr + size > len(self.d) or addr < 0:
            self.errors.append('%#x: pointer %#x+%#x outside file (%#x)' % (where, addr, size, len(self.d)))
            return False
        return True

    def model_dl(self, start):
        """Registers one LOD display list: runs the relocator walk over it. finish() then validates."""
        d = self.d
        self.top.add(start)
        try:
            ptrs, conds, fv = walk_reloc(d, start)
        except DLError as e:
            self.errors.append(str(e)); return
        if fv is not None:
            try: self.stats['vtx chain'] += vtx_chain(d, fv)
            except DLError as e: self.errors.append(str(e))
        for o, c, t in conds:
            self.stats['cond %s' % COND_NAMES.get(c, c)] += 1
            self.check_ptr(o, t, 8)
        self.relocated.update(ptrs)

    def finish(self):
        seen = set()
        for s in sorted(self.top):
            self._walk(s, self.relocated, 0, seen)

    def _walk(self, start, relocated, depth, seen):
        d = self.d
        if depth > 10:
            self.errors.append('display list nesting too deep at %#x' % start); return
        if start in seen:
            return
        seen.add(start)
        o = start
        intex = self.in_tex(start)
        while True:
            if o + 8 > len(d):
                self.errors.append('list %#x runs off the end' % start); return
            w0, w1 = u32(d, o), u32(d, o + 4)
            op = w0 >> 24
            a = w1 & 0xFFFFFF
            self.stats['op %02X' % op] += 1
            if op in (0x01, 0xDE, 0xDA, 0xDC, 0xFD, 0xFE, 0xFF, 0xDD) or (op == 0xE1 and d[o + 8] in (0x04, 0xDD)):
                ok_reloc = (o + 4) in relocated if not intex else (op in (0xFD,) or op == 0xE1)
                if not ok_reloc:
                    self.errors.append('%#x: op %02X pointer not rebased by the loader (list %#x)' % (o, op, start))
                tgt = a + (self.tex_base if intex else 0)
                size = {0x01: ((w0 >> 12) & 0xFF) * 16, 0xDA: 64, 0xDC: ((w0 >> 19) & 0x1F) * 8 + 8}.get(op, 8)
                if op in (0xFD,):
                    size = 2  # real size depends on the following LOADBLOCK; checked as a point
                self.check_ptr(o, tgt, size)
                if op == 0xDE:
                    # Sub-lists are not entered by the relocator: a texture-load list (linear scan) or another
                    # model list are fine, anything else must not hold pointers (checked by the walk).
                    self._walk(tgt, relocated, depth + 1, seen)
                    if (w0 >> 16) & 0xFF:
                        return
            if op == 0xE0 and (w0 >> 16) & 0xFF == 1:
                pass  # conditional branch; checked in model_dl
            if op == 0xDF:
                return
            if op == 0x04:
                return  # BRANCH_Z: target walked by the relocator
            o += 8


# ---------------------------------------------------------------------------------------------------------------
# Rush 2

class R2Model:
    """Parsed Rush 2 model container."""

    def __init__(self, d):
        self.d = d
        h = self.h = struct.unpack_from('>10I', d, 0)
        if h[9] != 0 or h[0] + h[4] * 0x34 != h[2] or h[2] + h[5] * 0x20 != h[3]:
            raise ValueError('not a Rush 2 model container')
        if h[1] + h[4] * 0x18 > len(d) or h[7] > h[8] or h[8] > len(d):
            raise ValueError('bad Rush 2 header')
        self.models = []
        for i in range(h[4]):
            o = h[0] + i * 0x34
            n = u32(d, o)
            lods = [struct.unpack_from('>HHfI', d, o + 4 + j * 12) for j in range(4)]
            no = h[1] + i * 0x18
            name = cstr(d[no:no + 16])
            radius, kind, pad = struct.unpack_from('>fHH', d, no + 16)
            self.models.append(dict(name=name, radius=radius, kind=kind, n=n, lods=lods[:n], all_lods=lods))
        self.textures = []
        for i in range(h[5]):
            o = h[2] + i * 0x20
            w, hh, idx, tex, fmt = struct.unpack_from('>HHIII', d, o + 16)
            self.textures.append(dict(name=cstr(d[o:o + 16]), w=w, h=hh, idx=idx, texels=tex, fmt=fmt))
        self.palettes = []
        for i in range(h[6]):
            o = h[3] + i * 0x18
            a, pal = struct.unpack_from('>II', d, o + 16)
            self.palettes.append(dict(name=cstr(d[o:o + 16]), a=a, pal=pal))

    def names_sorted(self):
        names = [m['name'].encode('latin1')[:15] for m in self.models]
        return all(names[i] < names[i + 1] for i in range(len(names) - 1))

    def check(self, flags=None):
        """Validates the container; returns (errors, stats)."""
        d, h = self.d, self.h
        errors = []
        ck = Checker(d, [(h[7], h[8])])
        if not self.names_sorted():
            errors.append('names not sorted (binary search in func_8005BCB4 would miss)')
        # Texture-load lists: linear scan relocating SETTIMG and RDPHALF_1+BRANCH_Z (func_80077B38).
        for o in range(h[7], h[8], 8):
            op = d[o]
            if op == 0xFD:
                ck.check_ptr(o, u32(d, o + 4) & 0xFFFFFF, 2)
            if op in (0x01, 0xDA, 0xDC, 0xDE) :
                errors.append('%#x: op %02X in texture-load lists is not rebased' % (o, op))
        for i, m in enumerate(self.models):
            if not 1 <= m['n'] <= 4:
                errors.append('%s: lod count %d' % (m['name'], m['n']))
            for t, f, dist, dl in m['lods']:
                if dl:
                    ck.model_dl(dl)
                ck.stats['lodflag %#x' % f] += 1
        ck.finish()
        for t in self.textures:
            ck.check_ptr(0, t['texels'], 2)
        for p in self.palettes:
            ck.check_ptr(0, p['pal'], 2)
        return errors + ck.errors, ck.stats


# ---------------------------------------------------------------------------------------------------------------
# Rush 2049

def chunks49(d):
    o = u32(d, 0)
    out = {}
    while o + 12 <= len(d):
        tag = d[o:o + 4]
        if not tag.isalpha():
            break
        out[tag.decode()] = (u32(d, o + 4), u32(d, o + 8))
        o += 12
    return out


class M49:
    """Parsed Rush 2049 model container."""
    OBHD = 0x58
    TXHD = 0x24
    PLHD = 0x18
    PTHD = 0x24

    def __init__(self, d):
        self.d = d
        c = self.c = chunks49(d)
        for t in ('IMAG', 'TXLD', 'OBHD', 'PLHD', 'TXHD', 'OBJS'):
            if t not in c:
                raise ValueError('missing chunk ' + t)
        self.imag = c['IMAG'][0]
        o, n = c['OBHD']
        self.objects = []
        for i in range(n):
            r = o + i * self.OBHD
            # name[16]: 2049's own lookup compares 15 characters (func_80092E2C -> func_80095120), so bytes
            # 12-15 are part of the name (TRACK2WATER01); after the NUL they hold stale bytes.
            name = cstr(d[r:r + 16])
            radius, kind, nlod = struct.unpack_from('>fHh', d, r + 16)
            flags = 0
            lods = [struct.unpack_from('>HHfII', d, r + 0x18 + j * 16) for j in range(4)]
            self.objects.append(dict(name=name, flags=flags, radius=radius, kind=kind, n=nlod, lods=lods[:nlod],
                                     all_lods=lods, rec=r))
        o, n = c['TXHD']
        self.textures = []
        for i in range(n):
            r = o + i * self.TXHD
            w, h, idx, tex, fmt, extra = struct.unpack_from('>HHIIII', d, r + 16)
            self.textures.append(dict(name=cstr(d[r:r + 16]), w=w, h=h, idx=idx, texels=tex, fmt=fmt, extra=extra))
        o, n = c['PLHD']
        self.palettes = []
        for i in range(n):
            r = o + i * self.PLHD
            a, pal = struct.unpack_from('>II', d, r + 16)
            self.palettes.append(dict(name=cstr(d[r:r + 16]), a=a, pal=pal))
        self.paths = []
        if 'PTHD' in c:
            o, n = c['PTHD']
            for i in range(n):
                r = o + i * self.PTHD
                self.paths.append(dict(name=cstr(d[r:r + 16]), words=struct.unpack_from('>5I', d, r + 16)))

    def check(self):
        d, c = self.d, self.c
        ta, ts = c['TXLD']
        ck = Checker(d, [(ta, ta + ts)], self.imag)
        errors = []
        for o in range(ta, ta + ts, 8):
            if d[o] == 0xFD:
                ck.check_ptr(o, (u32(d, o + 4) & 0xFFFFFF) + self.imag, 2)
        for ob in self.objects:
            for t, f, dist, dl, vtx in ob['lods']:
                if dl:
                    ck.model_dl(dl)
                if vtx:
                    ck.check_ptr(ob['rec'], vtx, 16)
                ck.stats['lodflag %#x' % f] += 1
        ck.finish()
        for t in self.textures:
            ck.check_ptr(0, t['texels'] + self.imag, 2)
        for p in self.palettes:
            ck.check_ptr(0, p['pal'] + self.imag, 2)
        names = [o['name'].encode('latin1')[:15] for o in self.objects]
        if any(names[i] >= names[i + 1] for i in range(len(names) - 1)):
            errors.append('OBHD names not sorted')
        return errors + ck.errors, ck.stats


# ---------------------------------------------------------------------------------------------------------------
# Conversion

HEADER = 0x28
SHIFT = HEADER - 8    # 2049 data from file offset 8 (IMAG) moves to 0x28 (right after the Rush 2 header)

# 2049 LOD flags that have a Rush 2 meaning at the same bit (func_8009C8F0 vs func_8007AA48):
#   0x1 texture swap, 0x2 post display list, 0x4 primitive colour from the node, 0x8000 texture handle valid.
# 0x10 (lit) has no Rush 2 LOD equivalent: Rush 2 lights per scene node (node flag 0x800 = unlit).
LOD_FLAGS_KEPT = 0x8007


G_TLUT_RGBA16 = (0xE3001001, 0x00008000)   # what Rush 2 keeps in place between models (func_8007AA48 sets it
                                           # once per frame; its own lists and cars never change it)


def list_end(d, start):
    """Offset of the G_ENDDL that ends a contiguous display list (2049 and Rush 2 model lists have no
    BRANCH_Z and their conditional targets stay inside the list)."""
    o = start
    while d[o] != 0xDF:
        if d[o] == 0x04:
            raise DLError('BRANCH_Z in list %#x' % start)
        o += 8
    return o


def end_state(d, start, flags):
    """Othermode-H TLUT value (shift 14) a list leaves behind, following G_DL calls and resolving the
    conditional op for the given load flags. None = list never sets it."""
    stack, o, tlut, n = [], start, None, 0
    while n < 200000:
        n += 1
        w0, w1 = u32(d, o), u32(d, o + 4)
        op = w0 >> 24
        if op == 0xE3 and 32 - ((w0 >> 8) & 0xFF) - ((w0 & 0xFF) + 1) == 14:
            tlut = w1
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
                return tlut
            o = stack.pop()
            continue
        o += 8
    raise DLError('list %#x does not end' % start)


def convert_model(data, rename=None, restore_tlut=True, dummies=()):
    """Converts a 2049 model container (track geometry 101-106, track objects 82-87, any OBHD file) into a
    Rush 2 model container. Returns (bytes, report).

    rename:       {2049 name: Rush 2 name} (e.g. SKYSKY -> SKYO1).
    restore_tlut: lists that leave the texture LUT mode other than RGBA16 get a copy, ending with
                  G_SETOTHERMODE_H(TEXTLUT = RGBA16), which the model then points to. 2049 lists set the TLUT mode
                  before every texture; Rush 2 sets it once per frame and its cars rely on it.
    dummies:      extra names to define as empty models (G_ENDDL), for names Rush 2 code looks up unchecked.

    Layout: Rush 2 header (0x28 bytes); the 2049 file from IMAG onwards shifted by SHIFT bytes (IMAG, TXLD, OBHD,
    PLHD, TXHD, OBJS, PATH, PTHD keep their relative layout, so PATH/PTHD stay readable for later use); the copied
    lists and the dummy list; then the Rush 2 model, texture, palette and name tables. Display lists are byte
    identical except pointer words: object-list pointers move by SHIFT, texture-load SETTIMGs (IMAG-relative in
    2049) become file-relative."""
    m = M49(data)
    c = m.c
    report = Counter()
    rename = rename or {}
    # The whole 2049 file after its first 8 bytes, including its chunk directory (kept up to date below so the
    # PATH/PTHD animation data stays findable: report['2049 directory'] = its offset in the new file).
    out = bytearray(HEADER) + bytearray(data[8:])
    out += bytes((-len(out)) & 7)
    d0 = u32(data, 0)
    for i, (tag, (o, n)) in enumerate(c.items()):
        struct.pack_into('>I', out, d0 + SHIFT + i * 12 + 4, o + SHIFT)
    report['2049 directory'] = d0 + SHIFT
    if 'PTHD' in c:
        # PTHD +0x18 / +0x1C are file-relative pointers (func_80096CA8 rebases both); keep them file-relative.
        po, pn = c['PTHD']
        for i in range(pn):
            for f in (0x18, 0x1C):
                at = po + i * M49.PTHD + f + SHIFT
                if u32(out, at):
                    struct.pack_into('>I', out, at, u32(out, at) + SHIFT)

    def put(o, v):
        struct.pack_into('>I', out, o, v & 0xFFFFFFFF)

    done = set()

    def shift_ptr(o_new, add):
        if o_new in done:
            report['shared pointer words'] += 1
            return
        done.add(o_new)
        w = u32(out, o_new)
        put(o_new, (w & 0xFF000000) | (((w & 0xFFFFFF) + add) & 0xFFFFFF))

    # Object lists: rebase exactly the words the relocator rebases (walked on the original data), plus the
    # targets of conditional ops (the relocator rebases those when it turns them into branches).
    for ob in m.objects:
        for t, f, dist, dl, vtx in ob['lods']:
            if not dl:
                continue
            ptrs, conds, fv = walk_reloc(data, dl)
            for p in ptrs:
                shift_ptr(p + SHIFT, SHIFT)
            for o, cnd, tgt in conds:
                shift_ptr(o + 4 + SHIFT, SHIFT)
                report['conditional ops %s' % COND_NAMES.get(cnd, cnd)] += 1
    # Texture-load lists: SETTIMG and RDPHALF_1+BRANCH_Z, IMAG-relative -> file-relative.
    ta, ts = c['TXLD']
    for o in range(ta, ta + ts, 8):
        op = data[o]
        if op == 0xFD or (op == 0xE1 and o + 8 < ta + ts and data[o + 8] == 0x04):
            shift_ptr(o + 4 + SHIFT, m.imag + SHIFT)

    # Lists that leave TLUT != RGBA16: append a copy ending in a TLUT restore.
    new_dl = {}
    if restore_tlut:
        for ob in m.objects:
            for t, f, dist, dl, vtx in ob['lods']:
                if not dl or dl + SHIFT in new_dl:
                    continue
                s = dl + SHIFT
                states = {end_state(out, s, load_flags(1, mir)) for mir in (False, True)}
                if states <= {None, G_TLUT_RGBA16[1]}:
                    continue
                e = list_end(out, s)
                ns = len(out)
                ptrs, conds, fv = walk_reloc(out, s)
                body = bytearray(out[s:e])
                for p in ptrs + [o + 4 for o, _, _ in conds]:
                    w = u32(out, p)
                    if s <= (w & 0xFFFFFF) <= e:
                        struct.pack_into('>I', body, p - s, (w & 0xFF000000) | ((w & 0xFFFFFF) - s + ns))
                out += body + struct.pack('>4I', G_TLUT_RGBA16[0], G_TLUT_RGBA16[1], 0xDF000000, 0)
                new_dl[s] = ns
                report['lists given a TLUT restore'] += 1
    dummy_dl = None
    if dummies:
        dummy_dl = len(out)
        out += struct.pack('>2I', 0xDF000000, 0)

    # Tables, sorted by the 15-character names Rush 2's binary search compares.
    entries = [(rename.get(ob['name'], ob['name']).encode('latin1')[:15], ob) for ob in m.objects]
    have = {n for n, _ in entries}
    for name in dummies:
        n = name.encode('latin1')[:15]
        if n not in have:
            entries.append((n, None))
            report['dummy models'] += 1
    entries.sort(key=lambda e: e[0])
    for i in range(len(entries) - 1):
        if entries[i][0] == entries[i + 1][0]:
            raise ValueError('duplicate name %r' % entries[i][0])

    model_off = len(out)
    for n, ob in entries:
        rec = bytearray(0x34)
        if ob is None:
            struct.pack_into('>IHHfI', rec, 0, 1, 0, 0, 0.0, dummy_dl)
        else:
            struct.pack_into('>I', rec, 0, max(1, min(4, ob['n'])))
            for j in range(min(4, ob['n'])):
                t, f, dist, dl, vtx = ob['all_lods'][j]
                report['2049 lod flag %#x' % f] += 1
                ndl = new_dl.get(dl + SHIFT, dl + SHIFT) if dl else 0
                struct.pack_into('>HHfI', rec, 4 + j * 12, t, f & LOD_FLAGS_KEPT, dist, ndl)
        out += rec
    tex_off = len(out)
    for t in m.textures:
        # The first 0x20 bytes of a TXHD record are the Rush 2 texture record (same fields read by the shared
        # texture-load builder func_8007825C / func_80099BFC); +0x18 is IMAG-relative in 2049.
        rec = bytearray(0x20)
        rec[:16] = t['name'].encode('latin1')[:15].ljust(16, b'\0')
        struct.pack_into('>HHIII', rec, 16, t['w'], t['h'], t['idx'], t['texels'] + m.imag + SHIFT, t['fmt'])
        out += rec
    pal_off = len(out)
    for p in m.palettes:
        rec = bytearray(0x18)
        rec[:16] = p['name'].encode('latin1')[:15].ljust(16, b'\0')
        struct.pack_into('>II', rec, 16, p['a'], p['pal'] + m.imag + SHIFT)
        out += rec
    name_off = len(out)
    for n, ob in entries:
        rec = bytearray(0x18)
        rec[:16] = n.ljust(16, b'\0')
        if ob is not None:
            struct.pack_into('>fHH', rec, 16, ob['radius'], ob['kind'], 0)
        out += rec
    hdr = (model_off, name_off, tex_off, pal_off, len(entries), len(m.textures), len(m.palettes),
           ta + SHIFT, ta + ts + SHIFT, 0)
    struct.pack_into('>10I', out, 0, *hdr)
    report['objects'] = len(entries)
    return bytes(out), report


def convert_track(data_2049, rename=None, dummies=()):
    """2049 track geometry file (101-106) -> Rush 2 track geometry container (asset 0x33 + t). The sky object is
    renamed to the name Rush 2's Vegas sky path looks up (func_800A45A8)."""
    names = {'SKYSKY': 'SKYO1'}
    names.update(rename or {})
    out, _ = convert_model(data_2049, names, dummies=dummies)
    return out


# ---------------------------------------------------------------------------------------------------------------
# Potentially visible sets (per-track tables in each game's main data, not in the geometry file)

R2_PVS = [0x800C6538, 0x800C6A78, 0x800C7048, 0x800C7788, 0x800C7F38, 0x800C8588, 0x800C8B28, 0x800C9268,
          0x800C9508, 0x800C9818, 0x800C9BA8, 0x800C9F18]   # func_8007C27C jump table 0x800CF9A4
R2_PVS_COUNT = 0x800CA1A8                                   # u8 per track: sections under PVS control
R49_PVS = [0x8011B898, 0x8011BFE8, 0x8011C738, 0x8011CE88, 0x8011D618, 0x8011DC88]  # func_8009EBC0, tracks 1-6
R49_PVS_OBSTACLE = 0x8011E5B8                               # func_8009EBC0, the obstacle course (k 19)
R49_PVS_COUNT = 0x8011E748                                  # u8 per 2049 track id (0-18)


def pvs_rush2(r2, t):
    """Rush 2 PVS of track t: list (per region) of 128-bit masks, bit i = section node i visible.
    Entry = two big-endian u64 (sections 0-63, 64-127)."""
    n = r2.read(R2_PVS_COUNT, 12)[t]
    out = []
    for reg in range(n):
        hi, lo = struct.unpack('>QQ', r2.read(R2_PVS[t] + reg * 16, 16))
        out.append(hi | (lo << 64))
    return out


def pvs_2049(q, k):
    """2049 PVS of 2049 track id k - 1: race tracks 1-6 and the obstacle course (19) have one, the stunt arenas none.
    Entry = four big-endian u32, bit (i & 31) of word i >> 5."""
    n = q.main[R49_PVS_COUNT - q.MAIN_VRAM + k - 1]
    if 7 <= k <= 14:
        return []   # battle DM5 counts 14 regions, but its table isn't found yet (TODO): everything draws
    base = R49_PVS[k - 1] if k <= 6 else R49_PVS_OBSTACLE
    out = []
    for reg in range(n):
        w = [q.w(base + reg * 16 + 4 * j) for j in range(4)]
        out.append(w[0] | (w[1] << 32) | (w[2] << 64) | (w[3] << 96))
    return out


def pvs_rush2_bytes(masks):
    """Rush 2 table bytes for a list of 128-bit masks (2049 words 0,1 / 2,3 become one u64 each)."""
    return b''.join(struct.pack('>QQ', m & (2**64 - 1), m >> 64) for m in masks)


# ---------------------------------------------------------------------------------------------------------------
# Emulation of Rush 2's load-time processing, then an RSP-style walk of the result

def emulate_rush2_load(d, base=0x00400000, flags=None):
    """Applies what func_80077CC0 does to a container loaded at `base` (physical, < 16 MB): the texture-list scan
    (func_80077B38), then the per-LOD relocator walk with conditional rewriting (func_8007786C), then the texture
    and palette table pointers. Returns (bytearray image, absolute LOD list addresses)."""
    flags = load_flags() if flags is None else flags
    img = bytearray(d)
    h = struct.unpack_from('>10I', img, 0)

    def rd(o): return u32(img, o)

    def wr(o, v): struct.pack_into('>I', img, o, v & 0xFFFFFFFF)

    def reloc(o):
        w = rd(o)
        wr(o, (w & 0x0F000000) | ((w + base) & 0xFFFFFF))   # segment -1: keep the nibble (func_8007786C)

    # func_80077B38 over [h7, h8)
    for o in range(h[7], h[8], 8):
        op = img[o]
        if _skipped(op) and not (0xC0 <= op < 0xD6 and False):
            continue
        nxt = img[o + 8] if o + 8 < len(img) else 0
        if op == 0xFD:
            reloc(o + 4)
        if op == 0xE1 and nxt == 0x04:
            reloc(o + 4)
    lods = []
    for i in range(h[4]):
        r = h[0] + i * 0x34
        n = rd(r)
        for j in range(n):
            e = r + 4 + j * 12
            dl = rd(e + 8)
            wr(e + 8, dl + base)          # the loader adds the base to the LOD list pointer
            lods.append(dl + base)
            # func_8007786C on the file-relative list
            o, half1 = dl, None
            while True:
                op = img[o]
                if _skipped(op):
                    o += 8
                    continue
                nxt = img[o + 8]
                if op in (0x01, 0xDD, 0xDE, 0xDA, 0xDC) or op >= 0xFD or (op == 0xE1 and nxt in (0x04, 0xDD)):
                    reloc(o + 4)
                if op == 0xE0 and img[o + 1] == 1 and not (flags >> u16(img, o + 2)) & 1:
                    t = rd(o + 4)
                    wr(o, 0xDE010000)
                    wr(o + 4, (t & 0x0F000000) | ((t + base) & 0xFFFFFF))
                if op == 0xE1:
                    half1 = rd(o + 4)
                if op == 0x04:
                    o = (half1 & 0xFFFFFF) - base
                    continue
                if op == 0xDF:
                    break
                o += 8
    for i in range(h[5]):
        o = h[2] + i * 0x20 + 0x18
        wr(o, rd(o) + base)
    for i in range(h[6]):
        o = h[3] + i * 0x18 + 0x14
        wr(o, rd(o) + base)
    return img, lods


def rsp_walk(img, base, starts):
    """Walks the loaded lists like the RSP would (G_DL push/branch, conditional ops already resolved) and returns
    a list of problems: addresses outside the loaded file, leftover conditional ops on a taken path, runaway
    lists. Also returns command counts."""
    errs, c = [], Counter()
    end = base + len(img)
    seen = set()

    def ok(a, size, where, what):
        if not (base <= a and a + size <= end):
            errs.append('%#x: %s address %#x outside [%#x, %#x)' % (where, what, a, base, end))
            return False
        return True

    for s in starts:
        stack, o, n = [], s, 0
        if not ok(s, 8, s, 'list'):
            continue
        while True:
            n += 1
            if n > 100000:
                errs.append('list %#x does not end' % s); break
            off = o - base
            w0, w1 = u32(img, off), u32(img, off + 4)
            op = w0 >> 24
            c[op] += 1
            a = w1 & 0xFFFFFF
            if op == 0x01:
                ok(a, ((w0 >> 12) & 0xFF) * 16, o, 'vertex')
            elif op in (0xFD, 0xDA, 0xDC):
                ok(a, 8, o, 'data')
            elif op == 0xE0 and (w0 >> 16) & 0xFF == 1:
                c['conditional kept (condition true)'] += 1
            if op == 0xDE:
                if not ok(a, 8, o, 'list'):
                    break
                if not (w0 >> 16) & 0xFF:
                    if len(stack) > 9:
                        errs.append('%#x: display list stack overflow' % o); break
                    stack.append(o + 8)
                o = a
                continue
            if op == 0xDF:
                if not stack:
                    break
                o = stack.pop()
                continue
            o += 8
    return errs, c


def load_test(d):
    """Emulated loads for 1P/2P x normal/mirror; returns problems found."""
    errs = []
    for players in (1, 2):
        for mirror in (False, True):
            img, lods = emulate_rush2_load(d, 0x00400000, load_flags(players, mirror))
            e, c = rsp_walk(img, 0x00400000, lods)
            errs += ['%dP %s: %s' % (players, 'mirror' if mirror else 'normal', x) for x in e]
    return errs


# ---------------------------------------------------------------------------------------------------------------
# Self-check

def selfcheck():
    import roms
    r2 = roms.Rush2()
    q = roms.Rush2049()
    bad = 0

    def show(label, errs, st, extra=''):
        nonlocal bad
        bad += bool(errs)
        keys = ', '.join('%s=%d' % (k, v) for k, v in sorted(st.items()) if k.startswith(('cond', 'lodflag', 'vtx')))
        print('%-24s %s  %s %s' % (label, 'OK ' if not errs else 'ERR', keys, extra))
        for e in errs[:6]:
            print('      ', e)

    def tlut_leaks(d, lods):
        return sum(1 for dl in lods for mir in (False, True)
                   if end_state(d, dl, load_flags(1, mir)) not in (None, G_TLUT_RGBA16[1]))

    print('== Rush 2 tracks (asset 0x33 + t): parse, pointer check, emulated load 1P/2P x normal/mirror')
    for t in range(12):
        d = r2.asset(0x33 + t)
        m = R2Model(d)
        e, st = m.check()
        e += load_test(d)
        leaks = tlut_leaks(d, [l[3] for mm in m.models for l in mm['lods']])
        show('track %d (%d models)' % (t, len(m.models)), e, st, 'TLUT leaks=%d' % leaks)
    print('== Rush 2 cars')
    for i in (0x1D, 0x1E, 0x25, 0x2F, 0x30, 0x32):
        d = r2.asset(i)
        m = R2Model(d)
        e, st = m.check()
        e += load_test(d)
        show('car %s (%d models)' % (r2.CARS[i - 0x1D], len(m.models)), e, st)
    print('== 2049 files (101-106 tracks, 82-87 track objects)')
    for k in list(range(101, 107)) + list(range(82, 88)):
        d = q.file(k)
        m = M49(d)
        e, st = m.check()
        leaks = tlut_leaks(d, [l[3] for o in m.objects for l in o['lods']])
        show('file %d (%d objects)' % (k, len(m.objects)), e, st, 'TLUT leaks=%d' % leaks)
    print('== converted 2049 -> Rush 2 (parse, pointer check, emulated load, DL bytes vs original)')
    for k in list(range(101, 107)) + list(range(82, 88)):
        data = q.file(k)
        if k >= 101:
            out = convert_track(data)
            _, rep = convert_model(data, {'SKYSKY': 'SKYO1'})
        else:
            out, rep = convert_model(data)
        m = R2Model(out)
        e, st = m.check()
        e += load_test(out)
        src = M49(data)
        ren = {'SKYSKY': 'SKYO1'} if k >= 101 else {}
        byname = {ren.get(o['name'], o['name']).encode('latin1')[:15].decode('latin1'): o for o in src.objects}
        for mo in m.models:
            ob = byname.get(mo['name'])
            if ob is None:
                e.append('unexpected model %s' % mo['name']); continue
            if mo['n'] != ob['n'] or abs(mo['radius'] - ob['radius']) > 0:
                e.append('header mismatch %s' % mo['name'])
            for (t, f, dist, dl, vtx), (t2, f2, dist2, dl2) in zip(ob['lods'], mo['lods']):
                # Same list bytes except pointer words (compare instruction words and opcode bytes).
                a, b = dl, dl2 - SHIFT
                ea = list_end(data, a)
                for x in range(a, ea + 8, 8):
                    y = x - a + dl2
                    if data[x:x + 4] != out[y:y + 4] or (data[x] not in (0x01, 0xDE, 0xDA, 0xDC, 0xFD, 0xE0, 0xE1, 0xDD)
                                                          and data[x + 4:x + 8] != out[y + 4:y + 8]):
                        if not (data[x] == 0xDF and out[y] == 0xE3):
                            e.append('%s: list differs at +%#x' % (mo['name'], x - a)); break
                if dist != dist2 or f2 != f & LOD_FLAGS_KEPT:
                    e.append('lod mismatch in %s' % mo['name'])
        # The embedded 2049 directory must still describe the (shifted) chunks.
        o, cc = rep['2049 directory'], {}
        while out[o:o + 4].isalpha():
            cc[out[o:o + 4].decode()] = struct.unpack_from('>II', out, o + 4)
            o += 12
        if {t: (a - SHIFT, n) for t, (a, n) in cc.items()} != src.c:
            e.append('embedded 2049 directory wrong')
        leaks = tlut_leaks(out, [l[3] for mm in m.models for l in mm['lods']])
        if leaks:
            e.append('%d lists still leave TLUT != RGBA16' % leaks)
        show('file %d -> %#x bytes' % (k, len(out)), e, st)
        print('       %s' % ', '.join('%s=%d' % kv for kv in sorted(rep.items())))
    print('== PVS tables (main data): diagonal set, 2049 -> Rush 2 encoding round trip')
    for t in range(12):
        p = pvs_rush2(r2, t)
        diag = sum((m >> i) & 1 for i, m in enumerate(p))
        print('  Rush 2 track %2d: %3d regions, self-visible %3d, avg visible %.1f' %
              (t, len(p), diag, sum(bin(m).count('1') for m in p) / len(p)))
    for k in range(1, 7):
        p = pvs_2049(q, k)
        b = pvs_rush2_bytes(p)
        back = [struct.unpack_from('>QQ', b, i * 16) for i in range(len(p))]
        rt = all((hi | lo << 64) == m for (hi, lo), m in zip(back, p))
        diag = sum((m >> i) & 1 for i, m in enumerate(p))
        print('  2049 track %d:    %3d regions, self-visible %3d, avg visible %.1f, round trip %s' %
              (k, len(p), diag, sum(bin(m).count('1') for m in p) / len(p), 'OK' if rt else 'BAD'))
        bad += not rt
    print('FAILED' if bad else 'all OK')
    return not bad


if __name__ == '__main__':
    sys.exit(0 if selfcheck() else 1)
