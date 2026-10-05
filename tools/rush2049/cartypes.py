"""Widening Rush 2's per-car-type tables from 22 to 36 types (Rush 2049 cars as types 23-35; 22 keeps its meaning as
Rush 2's "no car" marker).

Each table moves to a 35-wide copy in free RDRAM (NEW_BASE, one 64 KB page so every relocated `lui` gets the same
high half). This script finds the instructions that address the old tables (lui + addiu/load/store pairs, through
indexed `addu`), and the row-stride idioms (r * 22 as shift/subtract sequences) next to them, and prints us.toml
`[[patches.instruction]]` entries plus a review list of everything it couldn't decide.

    python cartypes.py scan        sites per function, with idioms and conflicts (review)
    python cartypes.py layout      old -> new table addresses (for src/car2049.cpp)
"""
import re, sys, os
import refscan

N_OLD, N_NEW = 22, 36
NEW_BASE = 0x80200000

# (old address, element size, rows, meaning). Rows are: 0 = defaults/drones, 1 = player 1, 2 = player 2. The copies
# of the per-player tables get two more rows, for players 3 and 4 (src/players4.cpp): the rows are indexed by human
# car + 1.
TABLES = [
    (0x800C06B4, 4, 1, 'steer/yaw force base'),
    (0x800C070C, 4, 1, 'yaw damping base'),
    (0x800C0764, 4, 1, 'car name pointers'),
    (0x800C07E8, 1, 1, 'type -> physics descriptor index'),
    (0x800C0800, 4, 1, 'suspension preload'),
    (0x800C0858, 4, 1, 'mass'),
    (0x800C08B0, 4, 1, 'pitch/roll inertia'),
    (0x800C090C, 4, 1, 'yaw inertia'),
    (0x800C0964, 4, 3, 'setup weight value'),
    (0x800C0A6C, 4, 3, 'front wheel scale'),
    (0x800C0B74, 4, 3, 'rear wheel scale'),
    (0x800C0C7C, 1, 3, 'u8 setting'),
    (0x800C0CD0, 1, 3, 'engine torque map index'),
    (0x800C0D14, 1, 3, 'drive flags'),
    (0x800C0D68, 1, 3, 'setting (yaw damping)'),
    (0x800C0DAC, 1, 3, 'handling 0..10'),
    (0x800C0E48, 1, 3, 'car+0x59E'),
    (0x800C0E8C, 1, 3, 'u8 setting'),
    (0x800C0ED0, 1, 3, 'u8 setting'),
    (0x800C0F14, 1, 3, 'u8 setting'),
    (0x800C0F58, 1, 3, 'u8 setting'),
    (0x800C0F9C, 1, 3, 'u8 setting'),
]


NEW_PLAYER_ROWS = 5


def new_rows(rows):
    return NEW_PLAYER_ROWS if rows == 3 else rows


def layout():
    out, at = [], NEW_BASE
    for old, size, rows, meaning in TABLES:
        out.append((old, at, size, rows, meaning))
        at += (N_NEW * size * new_rows(rows) + 7) & ~7
    return out


# Sites left unrelocated: func_800A4210 builds 0x800C08B0 only to read 0x58 past it (0x800C0908, a constant).
EXCLUDE = {'800A4490', '800A4494', '800A44E8'}


def table_of(addr):
    """(old base, new base, size, rows, row, index) for an address inside an old table, or None."""
    for old, new, size, rows, _ in layout():
        span = N_OLD * size
        if old <= addr < old + span * rows:
            row, rem = divmod(addr - old, span)
            return old, new, size, rows, row, rem // size
    return None


def translate(addr):
    t = table_of(addr)
    if t is None:
        return None
    old, new, size, rows, row, idx = t
    return new + row * N_NEW * size + (addr - old - row * N_OLD * size)


R2 = None


def word(vram):
    global R2
    if R2 is None:
        import roms
        R2 = roms.Rush2()
    return int.from_bytes(R2.read(vram, 4), 'big')


def lines_by_func():
    funcs, cur = {}, None
    for line in open(refscan.ASM):
        line = line.rstrip()
        if line.startswith('func_'):
            cur = line[:-1]
            funcs[cur] = []
            continue
        m = refscan.line_re.match(line)
        if m and cur:
            funcs[cur].append(m.groups())
    return funcs


def regs(args):
    return [x.strip() for x in args.split(',')]


def find_idioms(ins):
    """r*22 (bytes, final sll 1) and r*88 (f32 rows, final sll 3) as sll 2 / subu / sll 2 / subu / sll k."""
    out = []
    for i, (addr, op, args) in enumerate(ins):
        a = regs(args)
        if op != 'sll' or len(a) != 3 or int(a[2], 0) != 2 or a[0] == a[1]:
            continue
        t, r = a[0], a[1]
        want = [('subu', [t, t, r]), ('sll', [t, t, 2]), ('subu', [t, t, r]), ('sll', [t, t, None])]
        seq = [(addr, 'sll')]
        j = i + 1
        k = 0
        while j < len(ins) and k < len(want) and j - i < 16:
            a2, op2, args2 = ins[j]
            b = regs(args2)
            wop, wargs = want[k]
            if op2 == wop and b[:2] == wargs[:2] and (wargs[2] is None or (op2 == 'sll' and int(b[2], 0) == wargs[2])
                                                       or (op2 != 'sll' and b[2] == wargs[2])):
                seq.append((a2, op2, b[2]))
                k += 1
            elif t in b:
                break
            j += 1
        if k == len(want) and int(seq[-1][2], 0) in (1, 3):
            out.append((t, r, [s[0] for s in seq], 1 if int(seq[-1][2], 0) == 1 else 4))
    return out


def enc_r(funct, rs, rt, rd, sa):
    return (rs << 21) | (rt << 16) | (rd << 11) | (sa << 6) | funct


REG = {n: i for i, n in enumerate(['zero', 'at', 'v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5',
                                    't6', 't7', 's0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 't8', 't9', 'k0', 'k1',
                                    'gp', 'sp', 'fp', 'ra'])}


def stride35(t, r, size):
    """r*36*size in the idiom's 5 slots: sll t,r,3; addu t,t,r; sll t,t,(2|4); then two no-op shifts."""
    T, R = REG[t.strip('$')], REG[r.strip('$')]
    return [enc_r(0x00, 0, R, T, 3), enc_r(0x21, T, R, T, 0), enc_r(0x00, 0, T, T, 4 if size == 4 else 2),
            enc_r(0x00, 0, T, T, 0), enc_r(0x00, 0, T, T, 0)]


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'scan'
    if cmd == 'layout':
        for old, new, size, rows, m in layout():
            print('0x%08X -> 0x%08X  %d x %d x %d  %s' % (old, new, rows, N_NEW, size, m))
        return
    allrefs = refscan.scan([(0x80000000, 0x81000000)])
    uses_of_lui = {}
    for f, addr, op, args, val, lui in allrefs:
        uses_of_lui.setdefault(lui, []).append((addr, val))
    by_func = {}
    for f, addr, op, args, val, lui in allrefs:
        if table_of(val) is not None:
            by_func.setdefault(f, []).append((addr, op, args, val, lui))
    funcs = lines_by_func()
    patches, review = {}, []
    for f, sites in by_func.items():
        for addr, op, args, val, lui in sites:
            if addr in EXCLUDE:
                continue
            new = translate(val)
            w = word(int(addr, 16))
            patches[addr] = ((w & 0xFFFF0000) | (new & 0xFFFF), '%s %s -> %08X' % (op, args, new))
            others = [hex(v) for a, v in uses_of_lui.get(lui, []) if table_of(v) is None]
            if others:
                review.append('%s: lui %s also used for %s' % (f, lui, others))
            elif lui:
                lw = word(int(lui, 16))
                patches[lui] = ((lw & 0xFFFF0000) | (new >> 16), 'lui -> %04X' % (new >> 16))
        for t, r, slots, size in find_idioms(funcs[f]):
            for slot, v in zip(slots, stride35(t, r, size)):
                patches[slot] = (v, 'row stride 22 -> 36 (%s = %s * 36 * %d)' % (t, r, size))
    tp, notes = taint_patches(funcs, sorted(by_func))
    for addr, v in tp.items():
        if addr not in EXCLUDE:
            patches[addr] = v
    review += notes
    if cmd == 'scan':
        for f, sites in sorted(by_func.items()):
            print('==', f, len(sites), 'sites, idioms:', [(t, r, s[0], z) for t, r, s, z in find_idioms(funcs[f])])
        print('REVIEW:')
        print('\n'.join(review))
        print(len(patches), 'patches')
    if cmd == 'toml':
        for addr in sorted(patches):
            v, why = patches[addr]
            f = [fn for fn, ins in funcs.items() if any(i[0] == addr for i in ins)][0]
            print('[[patches.instruction]]\nfunc = "%s"\nvram = 0x%s\nvalue = 0x%08X # %s\n' % (f, addr, v, why))




def candidates():
    """Row-offset immediates and 22-type loop bounds in the functions that address the tables (manual review)."""
    allrefs = refscan.scan([(0x80000000, 0x81000000)])
    fn = sorted({f for f, addr, op, args, val, lui in allrefs if table_of(val) is not None})
    funcs = lines_by_func()
    imm = re.compile(r'(?:^|[ ,])(-?0x(?:16|2C|58|B0))(?:\(|$)')
    for f in fn:
        hits = [(a, op, args) for a, op, args in funcs[f] if imm.search(args)]
        print('==', f, len(hits))
        for a, op, args in hits:
            print('   ', a, op, args)


def row_imm(o, size):
    """Re-strides an immediate o that reaches across rows of a table with element size `size`: o = row * 22 * size + d
    with |d| small becomes row * 35 * size + d. Returns None when o doesn't cross a row."""
    old_row, new_row = N_OLD * size, N_NEW * size
    row = round(o / old_row)
    d = o - row * old_row
    if row == 0 or abs(d) > 8 * size:
        return None
    return row * new_row + d


def taint_patches(funcs, fnames):
    """Linear scan per function: registers (and stack slots) holding pointers into a relocated table; row-crossing
    immediates on them (loads, stores, addiu) are re-strided. Returns {addr: (new word, why)} and notes."""
    load_store = re.compile(r'^(l[bhw]u?|lwc1|s[bhw]|swc1|ldc1|sdc1)$')
    patches, notes = {}, []
    for f in fnames:
        hi = {}
        taint, slot = {}, {}
        for addr, op, args in funcs[f]:
            a = regs(args)
            if op == 'lui':
                hi[a[0]] = int(a[1], 16) << 16
                taint.pop(a[0], None)
                continue
            m = re.match(r'(-?0x[0-9A-Fa-f]+|-?\d+)\((\$\w+)\)', a[-1]) if a else None
            if op == 'addiu' and len(a) == 3 and a[1] in hi:
                t = table_of((hi[a[1]] + int(a[2], 0)) & 0xFFFFFFFF)
                if t and addr not in EXCLUDE:
                    taint[a[0]] = (t[2], t[3])
                elif a[0] in taint:
                    taint.pop(a[0])
                if a[0] != a[1]:
                    hi.pop(a[0], None)
                continue
            if op == 'addu' and len(a) == 3:
                src = a[1] if a[1] in taint else a[2] if a[2] in taint else None
                if a[1] in hi and a[2] not in hi:
                    hi[a[0]] = hi[a[1]]
                elif a[2] in hi and a[1] not in hi:
                    hi[a[0]] = hi[a[2]]
                else:
                    hi.pop(a[0], None)
                if src:
                    taint[a[0]] = taint[src]
                else:
                    taint.pop(a[0], None)
                continue
            if op == 'addiu' and len(a) == 3 and a[1] in taint:
                size, rows = taint[a[1]]
                imm = int(a[2], 0)
                n = row_imm(imm, size) if rows > 1 else None
                if rows == 1 and abs(imm) >= N_OLD * size:
                    notes.append('%s %s: step %s past a one-row table' % (f, addr, hex(imm)))
                if n is not None:
                    w = word(int(addr, 16))
                    patches[addr] = ((w & 0xFFFF0000) | (n & 0xFFFF), 'row step %s -> %s' % (hex(imm), hex(n)))
                taint[a[0]] = (size, rows)
                continue
            if m and load_store.match(op):
                base = m.group(2)
                imm = int(m.group(1), 0)
                if base == '$sp' and op == 'sw' and a[0] in taint:
                    slot[imm] = taint[a[0]]
                elif base == '$sp' and op == 'lw':
                    if imm in slot:
                        taint[a[0]] = slot[imm]
                    else:
                        taint.pop(a[0], None)
                    hi.pop(a[0], None)
                    continue
                elif base in taint:
                    size, rows = taint[base]
                    n = row_imm(imm, size) if rows > 1 else None
                    if rows == 1 and abs(imm) >= N_OLD * size:
                        notes.append('%s %s: offset %s past a one-row table' % (f, addr, hex(imm)))
                    if n is not None:
                        w = word(int(addr, 16))
                        patches[addr] = ((w & 0xFFFF0000) | (n & 0xFFFF), 'row offset %s -> %s' % (hex(imm), hex(n)))
                if op.startswith('l') and a[0] not in ('$sp',):
                    taint.pop(a[0], None)
                    hi.pop(a[0], None)
                continue
            if a and a[0].startswith('$') and op not in ('sw', 'sh', 'sb', 'swc1', 'beq', 'bne', 'bnel', 'beql', 'jr', 'jal'):
                taint.pop(a[0], None)
                if op != 'addiu':
                    hi.pop(a[0], None)
    return patches, notes


if __name__ == '__main__':
    if sys.argv[1:2] == ['cand']:
        candidates()
    else:
        main()


# ---------------------------------------------------------------------------------------------------------------
# Car preview ids: the car select shows car type t for player p with preview "car" id = p * 22 + t (44 ids), which
# indexes the race per-car arrays below. With 36 types the ids run to 72, so these arrays move to 72-entry copies in
# one 64 KB window (one lui value, 0x8022) and every instruction addressing them is relocated (strides unchanged).
ID_OLD, ID_NEW = 44, 72
ID_WINDOW = 0x80218000
ID_ARRAYS = [
    (0x8010C480, 0x4E, 'part handles (func_80086700)'),
    (0x8010F330, 0x1C, 'per-car callbacks'),
    (0x80113F90, 0x134, 'car state (body node index at +0)'),
    (0x801174F8, 0x7C, 'preview state (+1 loaded)'),
    (0x800D9D40, 0x2, 'per-car s16'),
    (0x800D97E8, 0x18, 'car slot loads (func_80087CE4)'),
    (0x80119858, 0x1, 'per-car byte (func_8008582C)'),
]


def id_layout():
    out, at = [], ID_WINDOW
    for old, stride, m in ID_ARRAYS:
        out.append((old, at, stride, m))
        at += (ID_NEW * stride + 15) & ~15
    assert at <= ID_WINDOW + 0x10000
    return out


def id_translate(addr):
    """Only addresses within the first 8 entries (race cars 0-7, and the base an id index is added to) move: the car
    select over-indexes these race arrays up to id 43 into memory that holds other variables, which stay put."""
    for old, new, stride, m in id_layout():
        if old <= addr < old + 8 * stride:
            return new + (addr - old)
    return None


def id_patches():
    allrefs = refscan.scan([(0x80000000, 0x81000000)])
    uses_of_lui = {}
    for f, addr, op, args, val, lui in allrefs:
        uses_of_lui.setdefault(lui, []).append((addr, val))
    funcs = lines_by_func()
    patches, review = {}, []
    for f, addr, op, args, val, lui in allrefs:
        new = id_translate(val)
        if new is None or addr in ID_EXCLUDE:
            continue
        hi = (new + 0x8000) >> 16
        lo = (new - (hi << 16)) & 0xFFFF
        w = word(int(addr, 16))
        patches[addr] = ((w & 0xFFFF0000) | lo, '%s %s -> %08X' % (op, args, new))
        others = sorted({hex(v) for a, v in uses_of_lui.get(lui, []) if id_translate(v) is None})
        his = {(id_translate(v) + 0x8000) >> 16 for a, v in uses_of_lui.get(lui, []) if id_translate(v) is not None}
        if others or len(his) > 1:
            review.append('%s %s: lui %s also used for %s' % (f, addr, lui, others))
        elif lui and lui not in ID_EXCLUDE:
            lw = word(int(lui, 16))
            patches[lui] = ((lw & 0xFFFF0000) | hi, 'lui -> %04X' % hi)
    return patches, review, funcs


# Preview-id arithmetic: id = player * 22 + type, type = id % 22 -> 36 (the neighbour-prefetch wrap in
# func_8008813C at 0x800882C4 / 0x80088350 stays 22 so prefetching only walks Rush 2's cars), and the two reset
# loops that stop at an id array's end.
ID_IMM = {
    '803B8E74': 0x24,  # func_803B81F0: carousel entry id offset for player 1
    '803B93A4': 0x24,  # func_803B81F0: id -> type
    '80088148': 0x24,  # func_8008813C: id -> player
    '80088264': 0x24, '800882B0': 0x24, '800884D0': 0x24, '80088548': 0x24, '800885CC': 0x24,  # id -> type
    '800882D8': 0x24, '8008835C': 0x24,  # neighbour type -> id
    '8005B9E0': 0x24, '8005BC0C': 0x24,  # func_8005B978: id -> type
    '80084488': 0x24,  # func_800843EC: id -> type
    '800878D8': 0x24,  # func_800878C8: id -> type
    '8008656C': 0x24,  # func_80085FD0: id -> type
    '80086E54': 0x24,  # func_80086CA4: type -> id
}


# func_800A00DC builds 0x801174F8 as the end of a fill loop over the array before it, not as the preview array.
# The part-handle table 0x8010C480 continues past the 44 cars (index 1716+) with track-object, debris and smoke
# handles; the object and particle code (func_80059450, func_8005AEA4, func_8005E918/F718/F900/FC08, func_8008A01C)
# indexes it from the same base, so those sites keep the old base.
ID_EXCLUDE = {'800A0458', '800A0460', '800595D4', '800595E8', '8005B3A8', '8005B3C0', '8005B600', '8005B604',
              '8005B718', '8005B71C', '8005EA10', '8005EA18', '8005F7E8', '8005F7F4', '8005F95C', '8005F9AC',
              '8005FC1C', '8005FC98', '8008A068', '8008A074'}


def id_end(old, stride):
    for o, n, s, m in id_layout():
        if o == old:
            return n + ID_NEW * s


def id_toml():
    patches, review, funcs = id_patches()
    for addr, imm in ID_IMM.items():
        w = word(int(addr, 16))
        assert (w & 0xFFFF) == 0x16, addr
        patches[addr] = ((w & 0xFFFF0000) | imm, 'preview id: 22 -> 36')
    for lui_addr, lo_addr, old, stride, end_index in (('8008A408', '8008A40C', 0x8010C480, 0x4E, None),
                                                      ('8008A3D8', '8008A3E4', 0x80119858, 1, None),
                                                      ('800A388C', '800A3890', 0x800D9D40, 2, 22)):
        # end_index: a fill loop that stops at a fixed entry (func_800A37F4 writes entries 2-21) rather than the end.
        end = id_end(old, stride) if end_index is None else id_translate(old) + end_index * stride
        hi, lo = (end + 0x8000) >> 16, (end - (((end + 0x8000) >> 16) << 16)) & 0xFFFF
        patches[lui_addr] = ((word(int(lui_addr, 16)) & 0xFFFF0000) | hi, 'loop end lui -> %04X' % hi)
        patches[lo_addr] = ((word(int(lo_addr, 16)) & 0xFFFF0000) | lo, 'loop end -> %08X' % end)
    assert not review, review
    out = []
    for addr in sorted(patches):
        v, why = patches[addr]
        f = [fn for fn, ins in funcs.items() if any(i[0] == addr for i in ins)][0]
        out.append('[[patches.instruction]]\nfunc = "%s"\nvram = 0x%s\nvalue = 0x%08X # %s\n' % (f, addr, v, why))
    return '\n'.join(out)
