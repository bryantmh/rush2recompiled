"""Rebuilds the N64 Rush 2049 code-segment tables the recomp reads from the Dreamcast executable, and checks them.

    dc_tables.py [--emit src/rush2049_dc_tables.inc] [--report]

The recomp reads tables out of the N64 game's boot and main segments and battle overlay (car setup, the dynamic-object
type table, texture animation lists, PVS, fog, record seeds, battle tuning). A Dreamcast source builds those segments
from 1ST_READ.BIN (little endian, loaded at 0x8C010000): every table is copied to its N64 address in the N64's layout
and with the N64's row order and index numbering, its values taken from the disc. A program of copy operations does it
(src/rush2049_dc_tables.cpp runs the .inc this writes); this tool also runs it on the disc and compares the result
with the N64 segments field by field (--report), which shows where the versions really differ.

Schemas (layout tokens, space separated; DC bytes read -> N64 bytes written):
  w  4->4 32-bit value          r  4->4 32-bit value stored as two swapped halfwords on the disc
  h  2->2 16-bit value          b  1->1 byte
  s  4->4 string pointer: the string goes to the heap after the segment, renamed to the N64's object name
  f  4->4 function pointer, through the function map (code compares a few type-table callbacks)
  Hm 2->2 s16 through value map m (the N64's numbering; -1 stays -1)
  Vm 4->4 s32 through value map m (map 1: sound effect ids, tools/rush2049/dc_sounds.py; unpaired ones become -1)
  Pk,n  4->4 pointer to n elements of schema k (heap)    Qk  pointer to a list of schema k up to its end mark
  Fk 4->4 pointer to as many elements of schema k as this element's first s16 says
  X  4->0 disc-only word        z  0->4 N64-only zero word
A schema's end mark: 'h0' a first s16 of 0, 'w0' a first word of 0 (the mark element is copied too).
Needs RUSH2049_ROM (roms.py) and the disc's files (DC_EXE, default tmp/dc/files/1ST_READ.BIN). See
docs/rush2049_research/dreamcast.md.
"""
import os, re, struct, sys, zlib

sys.path.insert(0, os.path.dirname(__file__))
from roms import Rush2049

DC_BASE = 0x8C010000
EXE = os.environ.get('DC_EXE', os.path.join(os.path.dirname(__file__), '..', '..', 'tmp', 'dc', 'files', '1ST_READ.BIN'))
BOOT, MAIN, BATTLE = 0, 1, 2
VRAM = {BOOT: 0x80000400, MAIN: 0x80086A50, BATTLE: 0x8038A400}
SEG_NAMES = {BOOT: 'Boot', MAIN: 'Main', BATTLE: 'Battle'}

# ---------------------------------------------------------------------------------------------------------------
# Schemas: name -> (layout, end mark or None)

SCHEMAS = {
    # Dynamic-object types (N64 0x30 bytes, disc 0x34): name prefix, model, init, update, flags, anim (model handle),
    # kind, sub-kind, parameter, sounds[3], sound flags, sound range; the disc adds a float.
    'type': ('s s f f r H0 b b w V1 V1 V1 w w X', None),
    'string': ('s', None),
    'word': ('w', None),
    'half': ('h', None),
    'byte': ('b', None),
    # Kind parameters: a pointer per kind to its per-sub-kind parameters.
    'kind_params': ('P2,18 P2,3 P2,6 w w w w w', None),
    # Flip-books: s16 count (0 ends), s16 start, s16 forward, s16 current, f32 timer, f32 period, frames
    # (12 bytes each: name, then two words set up at run time).
    'flip': ('h h h h w w F4', 'h0'),
    'frame': ('s X X z z', None),
    # Scrolls: name (0 ends), s16 position, s16 wrap, s8 speed, u8 kind, s16 rate, f32 timer, data.
    'scroll': ('s h h b b h w w', 'w0'),
    'flip_list': ('Q5', None),
    'scroll_list': ('Q7', None),
    # Demo start spine indices: up to 5 per track direction.
    'demo_list': ('P3,5', None),
    # Car descriptors (0xB4 bytes of words).
    'desc': (' '.join(['w'] * 45), None),
    'desc_ptr': ('P12,1', None),
    # Engine sound layers (N64 0x20 bytes, disc 0x1C): sound, base rpm, span, rpm points[3], pad, volumes[3].
    'engine_layer': ('V1 h h h h h h w w w z', None),
}
# Numeric references in layouts (P2, F4, Q5, Q7, P3, P12) name schemas by index in this order:
ORDER = ['type', 'string', 'word', 'half', 'frame', 'flip', 'byte', 'scroll', 'kind_params', 'flip_list',
         'scroll_list', 'demo_list', 'desc', 'desc_ptr', 'engine_layer']
assert sorted(ORDER) == sorted(SCHEMAS)


def sizes(layout):
    dc = n64 = 0
    for t in layout.split():
        c = t[0]
        if c in 'wrsfPQFV':
            dc += 4
            n64 += 4
        elif c in 'hH':
            dc += 2
            n64 += 2
        elif c == 'b':
            dc += 1
            n64 += 1
        elif c == 'X':
            dc += 4
        elif c == 'z':
            n64 += 4
    return dc, n64


# ---------------------------------------------------------------------------------------------------------------
# Data

class Exe:
    def __init__(self):
        self.d = open(EXE, 'rb').read()

    def ok(self, a, n=1):
        return DC_BASE <= a and a - DC_BASE + n <= len(self.d)

    def w(self, a): return struct.unpack_from('<I', self.d, a - DC_BASE)[0]
    def h(self, a): return struct.unpack_from('<H', self.d, a - DC_BASE)[0]
    def b(self, a): return self.d[a - DC_BASE]

    def s(self, a):
        o = a - DC_BASE
        return self.d[o:self.d.index(b'\0', o)].decode('latin1')


def n64_segments():
    rom = Rush2049()
    boot = rom.rom[0x1000:0x1000 + 0x86650]
    main = zlib.decompressobj(-15).decompress(rom.rom[0xB0CB10:0xB0CB10 + 0x100000])
    battle = zlib.decompressobj(-15).decompress(rom.rom[0xB6FEC4:0xB6FEC4 + 0x40000])
    return {BOOT: boot, MAIN: main, BATTLE: battle}


def n32(seg, s, a): return struct.unpack_from('>I', seg[s], a - VRAM[s])[0]
def nstr(seg, s, a):
    o = a - VRAM[s]
    return seg[s][o:seg[s].index(b'\0', o)].decode('latin1')


# ---------------------------------------------------------------------------------------------------------------
# The program

def build_program(exe, seg):
    """[(op, ...)]: ('copy', seg, n64, dc, schema, count), ('str', seg, n64, text), ('word', seg, n64, value),
    plus value maps {map: {dc: n64}} and the function map {dc: n64}."""
    ops = []
    maps = {0: {}, 1: {}}
    funcs = {}

    # Sound effect ids: the disc's to the N64's (an N64 id the disc shares with another keeps the lowest).
    import dc_sounds
    mapping, _ = dc_sounds.resolve(dc_sounds.table_pairs(exe, seg))
    for n, c in sorted(mapping.items(), reverse=True):
        maps[1][c] = n

    def copy(s, n64, dc, schema, count=1):
        ops.append(('copy', s, n64, dc, schema, count))

    # Model handle names (N64 0x8011AD68, 0x200 entries; disc 0x8C0AD8E4): by name, renamed to the N64's.
    n_handles, dc_handles = 0x8011AD68, 0x8C0AD8E4
    renames = load_renames()
    dc_by_n64 = {}
    for j in range(0x400):
        a = dc_handles + j * 4
        if not exe.ok(a, 4):
            break
        p = exe.w(a)
        if not exe.ok(p, 1):
            continue
        try:
            name = exe.s(p)
        except ValueError:
            continue
        if name and name.isprintable() and len(name) < 32:
            dc_by_n64.setdefault(renames.get(name, name), j)
    for i in range(0x200):
        p = n32(seg, MAIN, n_handles + i * 4)
        if not (VRAM[MAIN] <= p < VRAM[MAIN] + len(seg[MAIN])):
            continue
        name = nstr(seg, MAIN, p)
        j = dc_by_n64.get(name)
        if j is not None:
            copy(MAIN, n_handles + i * 4, dc_handles + j * 4, 'string')
            maps[0][j] = i

    # Type table (N64 0x80117530, 122 rows of 0x30; disc 0x8C0BFD80, rows of 0x34): rows by name. Rows the disc
    # lacks get a name no object starts with.
    n_types, dc_types = 0x80117530, 0x8C0BFD80
    dc_rows = {}
    j = 0
    while True:
        a = dc_types + j * 0x34
        try:
            name = exe.s(exe.w(a))
        except (ValueError, struct.error):
            break
        if not name or not name.isprintable():
            break
        dc_rows.setdefault(name, j)
        j += 1
    for k in range(122):
        a = n_types + k * 0x30
        name = nstr(seg, MAIN, n32(seg, MAIN, a))
        j = dc_rows.get(name)
        if j is None:
            ops.append(('str', MAIN, a, '~'))
            ops.append(('word', MAIN, a + 0x14, 0xFFFF0000))
            continue
        d = dc_types + j * 0x34
        copy(MAIN, a, d, 'type')
        for off in (8, 12):
            funcs.setdefault(exe.w(d + off), set()).add(n32(seg, MAIN, a + off))

    # Kind parameters: pointers (N64 0x80118DDC, disc 0x8C0C1744) and entry sizes (0x80117510, 0x8C0BFD60).
    copy(MAIN, 0x80118DDC, 0x8C0C1744, 'kind_params')
    copy(MAIN, 0x80117510, 0x8C0BFD60, 'byte', 8)

    # Texture animation: flip-books (0x8011A31C, disc 0x8C0C3170) and scrolls (0x8011A840, disc 0x8C0C38C4), a list
    # per track id.
    copy(MAIN, 0x8011A31C, 0x8C0C3170, 'flip_list', 19)
    copy(MAIN, 0x8011A840, 0x8C0C38C4, 'scroll_list', 19)

    # Demo starts: lists (0x801173D8, disc 0x8C0BFC94) and counts (0x80117408, disc 0x8C0BFCC4).
    copy(MAIN, 0x801173D8, 0x8C0BFC94, 'demo_list', 12)
    copy(MAIN, 0x80117408, 0x8C0BFCC4, 'half', 12)

    # Fog colors (3 bytes per track id) and PVS: the per-track tables, battle and obstacle course ones and counts form
    # one block in both, at one offset.
    copy(MAIN, 0x80114658, 0x8C0BDD56, 'byte', 19 * 3)
    copy(MAIN, 0x8011B898, 0x8C0C49B0, 'word', (0x8011E748 - 0x8011B898) // 4)
    copy(MAIN, 0x8011E748, 0x8C0C7860, 'byte', 19)

    # Cars: descriptors through their pointer table, per-car tables, setup tables, constants.
    copy(MAIN, 0x80110D08, 0x8C0BB894, 'desc_ptr', 13)
    for n64, dc, schema, count in [
        (0x80110DA4, 0x8C0BB930, 'word', 13),    # preload
        (0x80110DD8, 0x8C0BB964, 'word', 13),    # mass
        (0x80110E0C, 0x8C0BB998, 'word', 13),    # inertia
        (0x80110E44, 0x8C0BB9D0, 'word', 13),    # yaw inertia
        (0x80110EBC, 0x8C0BBA48, 'word', 6),     # gear ratios
        (0x801110C4, 0x8C0BBC4C, 'word', 27),    # torque [ENGINE 9][HANDLING 3]
        (0x8011121C, 0x8C0BBDA4, 'word', 5),     # rear grip per tire
        (0x80111230, 0x8C0BBDB8, 'byte', 13),    # setup E
        (0x80111274, 0x8C0BBDFC, 'word', 6),     # frame weights
        (0x801112DC, 0x8C0BBE64, 'word', 65),    # front wheel model scale [5][13]
        (0x801113E0, 0x8C0BBF68, 'word', 65),    # rear wheel model scale [5][13]
        (0x801114E4, 0x8C0BC06C, 'byte', 13),    # drive
        (0x8011157C, 0x8C0BC101, 'byte', 13),    # rims
        (0x801116D0, 0x8C0BC1C4, 'word', 33),    # handling [3][11]
        (0x8011F814, 0x8C0A8B1C, 'word', 6),     # front tire curve
        (0x8011F82C, 0x8C0A8B34, 'word', 6),     # rear tire curve
        (0x8011F844, 0x8C0A8F84, 'word', 52),    # boxes [13][4]
        (0x8010FD80, 0x8C0B92A0, 'engine_layer', 18),  # engine sounds [ENGINE 9][2]
        (0x80124150, 0x8C039EA8, 'word', 1),     # mass per frame weight (code constant)
        (0x80124154, 0x8C018788, 'word', 1),     # frame weight offsets (code constants, 0.4)
        (0x80124158, 0x8C018788, 'word', 1),
        (0x8012415C, 0x8C039968, 'word', 1),     # yaw inertia per frame weight (code constant, 100000)
        (0x801245AC, 0x8C0A8A10, 'word', 1),     # speedometer over true speed (1.2)
    ]:
        copy(MAIN, n64, dc, schema, count)

    # Record seeds (boot segment 0x8002E870, 38 floats).
    copy(BOOT, 0x8002E870, 0x8C0A7840, 'word', 38)

    # Battle tuning: weapon mounts [8][13][3], muzzles [9][3], shield sizes [13].
    copy(BATTLE, 0x803943A4, 0x8C0D1DF0, 'word', 8 * 13 * 3)
    copy(BATTLE, 0x80394B08, 0x8C0B34A0, 'word', 9 * 3)
    copy(BATTLE, 0x80394358, 0x8C0D1DAC, 'word', 13)

    return ops, maps, funcs


def load_renames():
    path = os.path.join(os.path.dirname(__file__), '..', '..', 'src', 'rush2049_dc_names.inc')
    out = {}
    for line in open(path):
        m = re.match(r'RENAME\("([^"]*)", "([^"]*)"\)', line)
        if m:
            out[m[1]] = m[2]
    return out


# ---------------------------------------------------------------------------------------------------------------
# Running the program (mirrors src/rush2049_dc_tables.cpp)

class Builder:
    def __init__(self, exe, sizes_, maps, funcs, renames):
        self.exe = exe
        self.seg = {s: bytearray(n) for s, n in sizes_.items()}
        self.maps, self.funcs, self.renames = maps, funcs, renames
        self.strings = {}

    def heap(self, s, data, align=4):
        b = self.seg[s]
        while len(b) % align:
            b.append(0)
        at = VRAM[s] + len(b)
        b.extend(data)
        return at

    def string(self, s, text):
        text = self.renames.get(text, text)
        key = (s, text)
        if key not in self.strings:
            self.strings[key] = self.heap(s, text.encode('latin1') + b'\0', 1)
        return self.strings[key]

    def put(self, s, a, data):
        o = a - VRAM[s]
        self.seg[s][o:o + len(data)] = data

    def element(self, s, n64, dc, name):
        """Copies one element; returns the N64 and disc addresses after it."""
        layout = SCHEMAS[name][0]
        e = self.exe
        start = dc
        for t in layout.split():
            c, arg = t[0], t[1:]
            if c == 'w':
                self.put(s, n64, struct.pack('>I', e.w(dc))); n64 += 4; dc += 4
            elif c == 'r':
                v = e.w(dc)
                self.put(s, n64, struct.pack('>I', ((v >> 16) | (v << 16)) & 0xFFFFFFFF)); n64 += 4; dc += 4
            elif c == 'h':
                self.put(s, n64, struct.pack('>H', e.h(dc))); n64 += 2; dc += 2
            elif c == 'H':
                v = struct.unpack('<h', struct.pack('<H', e.h(dc)))[0]
                v = v if v < 0 else self.maps[int(arg)].get(v, -1)
                self.put(s, n64, struct.pack('>h', v)); n64 += 2; dc += 2
            elif c == 'V':
                v = struct.unpack('<i', struct.pack('<I', e.w(dc)))[0]
                v = v if v < 0 else self.maps[int(arg)].get(v, -1)
                self.put(s, n64, struct.pack('>i', v)); n64 += 4; dc += 4
            elif c == 'b':
                self.put(s, n64, bytes([e.b(dc)])); n64 += 1; dc += 1
            elif c == 's':
                p = e.w(dc)
                self.put(s, n64, struct.pack('>I', self.string(s, e.s(p)) if p else 0)); n64 += 4; dc += 4
            elif c == 'f':
                self.put(s, n64, struct.pack('>I', self.funcs.get(e.w(dc), 0))); n64 += 4; dc += 4
            elif c in 'PQF':
                p = e.w(dc)
                target = 0
                if p:
                    k, _, n = arg.partition(',')
                    if c == 'P':
                        count = int(n)
                    elif c == 'F':
                        count = max(0, struct.unpack('<h', struct.pack('<H', e.h(start)))[0])
                    else:
                        count = None
                    target = self.array(s, p, ORDER[int(k)], count)
                self.put(s, n64, struct.pack('>I', target)); n64 += 4; dc += 4
            elif c == 'X':
                dc += 4
            elif c == 'z':
                self.put(s, n64, b'\0\0\0\0'); n64 += 4
        return n64, dc

    def array(self, s, dc, name, count):
        layout, mark = SCHEMAS[name]
        dsz, nsz = sizes(layout)
        if count is None:
            count = 0
            while True:
                a = dc + count * dsz
                count += 1
                if (mark == 'h0' and self.exe.h(a) == 0) or (mark == 'w0' and self.exe.w(a) == 0):
                    break
        at = self.heap(s, bytes(nsz * max(count, 0)))
        for i in range(max(count, 0)):
            self.element(s, at + i * nsz, dc + i * dsz, name)
        return at

    def run(self, ops):
        for op in ops:
            if op[0] == 'copy':
                _, s, n64, dc, name, count = op
                dsz, nsz = sizes(SCHEMAS[name][0])
                for i in range(count):
                    self.element(s, n64 + i * nsz, dc + i * dsz, name)
            elif op[0] == 'str':
                self.put(op[1], op[2], struct.pack('>I', self.string(op[1], op[3])))
            elif op[0] == 'word':
                self.put(op[1], op[2], struct.pack('>I', op[3]))


# ---------------------------------------------------------------------------------------------------------------
# Checking against the N64

def compare(seg, built, ops):
    """Per copy op: how many of its N64 bytes the disc's data reproduces (pointers compared by what they point at
    is beyond this check; their words are skipped)."""
    lines = []
    totals = {}
    for op in ops:
        if op[0] == 'copy' and op[4] in ('type', 'string') and op[5] == 1:
            # Row-mapped tables: one line for all their rows.
            _, s, n64, dc, name, count = op
            nsz = sizes(SCHEMAS[name][0])[1]
            o = n64 - VRAM[s]
            t = totals.setdefault(name, [0, 0, 0])
            t[0] += 1
            t[1] += nsz
            t[2] += sum(1 for x, y in zip(seg[s][o:o + nsz], built[s][o:o + nsz]) if x == y)
            continue
        if op[0] != 'copy':
            continue
        _, s, n64, dc, name, count = op
        dsz, nsz = sizes(SCHEMAS[name][0])
        n = nsz * count
        o = n64 - VRAM[s]
        a, b = seg[s][o:o + n], bytes(built[s][o:o + n])
        same = sum(1 for x, y in zip(a, b) if x == y)
        lines.append('%-6s %08X <- %08X %-12s x%-4d %5.1f%% same' % (SEG_NAMES[s], n64, dc, name, count, 100.0 * same / max(n, 1)))
    for name, (rows, n, same) in totals.items():
        lines.append('rows of %-12s x%-4d %5.1f%% same' % (name, rows, 100.0 * same / max(n, 1)))
    return lines


def main():
    exe = Exe()
    seg = n64_segments()
    ops, maps, funcs = build_program(exe, seg)
    conflicts = {hex(k): sorted(hex(x) for x in v) for k, v in funcs.items() if len(v) > 1}
    # A disc callback that some N64 rows have and others leave empty maps to the N64's callback.
    func_map = {k: max(v) for k, v in funcs.items()}
    renames = load_renames()
    b = Builder(exe, {s: len(seg[s]) for s in seg}, maps, func_map, renames)
    b.run(ops)
    if '--report' in sys.argv:
        print('\n'.join(compare(seg, b.seg, ops)))
        print('function map conflicts: %s' % conflicts)
        print('heap: %s' % {SEG_NAMES[s]: len(b.seg[s]) - len(seg[s]) for s in seg})
    if '--compare' in sys.argv:
        # Checks the C++ build (cpp_test/dc_test.exe writes segN.bin) against this one.
        d = sys.argv[sys.argv.index('--compare') + 1]
        for s in seg:
            got = open(os.path.join(d, 'seg%d.bin' % s), 'rb').read()
            print('%s: %s' % (SEG_NAMES[s], 'same as the tool' if got == bytes(b.seg[s]) else
                              'DIFFERENT (%d vs %d bytes, first at %#x)' % (len(got), len(b.seg[s]),
                              next((i for i, (x, y) in enumerate(zip(got, b.seg[s])) if x != y), min(len(got), len(b.seg[s]))))))
    if '--emit' in sys.argv:
        emit(sys.argv[sys.argv.index('--emit') + 1], seg, ops, maps, func_map)


def emit(path, seg, ops, maps, funcs):
    with open(path, 'w', newline='\n') as f:
        f.write('// Generated by tools/rush2049/dc_tables.py from the N64 ROM and the disc; do not edit.\n')
        for mac in ('SEGMENT', 'SCHEMA', 'COPY', 'STR', 'WORD', 'VALUE', 'FUNC'):
            f.write('#ifndef %s\n#define %s(...)\n#endif\n' % (mac, mac))
        f.write('// SEGMENT(segment, N64 size)\n')
        for s in sorted(seg):
            f.write('SEGMENT(%d, %d)\n' % (s, len(seg[s])))
        f.write('// SCHEMA(id, layout, end mark: 0 none, 1 first s16 is 0, 2 first word is 0)\n')
        for i, name in enumerate(ORDER):
            layout, mark = SCHEMAS[name]
            f.write('SCHEMA(%d, "%s", %d)  // %s\n' % (i, layout, {None: 0, 'h0': 1, 'w0': 2}[mark], name))
        f.write('// COPY(segment, N64 address, disc address, schema, count), STR(segment, N64 address, text), WORD(...)\n')
        for op in ops:
            if op[0] == 'copy':
                f.write('COPY(%d, 0x%08X, 0x%08X, %d, %d)\n' % (op[1], op[2], op[3], ORDER.index(op[4]), op[5]))
            elif op[0] == 'str':
                f.write('STR(%d, 0x%08X, "%s")\n' % (op[1], op[2], op[3]))
            else:
                f.write('WORD(%d, 0x%08X, 0x%08X)\n' % (op[1], op[2], op[3]))
        f.write('// VALUE(map, disc value, N64 value): index numbering (map 0: model handles, 1: sound effects)\n')
        for m, d in sorted(maps.items()):
            for k, v in sorted(d.items()):
                f.write('VALUE(%d, %d, %d)\n' % (m, k, v))
        f.write('// FUNC(disc function, N64 function): type-table callbacks\n')
        for k, v in sorted(funcs.items()):
            f.write('FUNC(0x%08X, 0x%08X)\n' % (k, v))
        for mac in ('SEGMENT', 'SCHEMA', 'COPY', 'STR', 'WORD', 'VALUE', 'FUNC'):
            f.write('#undef %s\n' % mac)


if __name__ == '__main__':
    main()
