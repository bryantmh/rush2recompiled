#!/usr/bin/env python3
"""Lines up a California Speed function against its Rush 2 counterpart by what it reads, writes, calls and the float
constants it loads (research aid; the two games were built by different compilers, so the instructions themselves
don't line up, but the struct accesses and constants do).

    python tools/calspeed/physdiff.py CS_ADDR R2_ADDR [--all]

Prints both token streams side by side, aligned by kind (load/store width, call, constant), marking rows that differ;
--all also prints the equal rows (default: only the differing runs with one row of context). Ends with the struct
offset pairs seen in matching rows (CS offset -> Rush 2 offset), which is how the car struct's shifts were mapped.
Needs RUSH2_ROM (default the repo copy) and CALSPEED_ROM (default build/calspeed.z64).
"""
import collections, difflib, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'rush2049'))
os.environ.setdefault('CALSPEED_ROM', os.path.join(REPO, 'build', 'calspeed.z64'))
import rabbitizer
import verify_names as vn

LOADS = {'lwc1': 'f', 'swc1': 'F', 'lw': 'w', 'sw': 'W', 'lh': 'h', 'lhu': 'h', 'sh': 'H', 'lb': 'b', 'lbu': 'b',
         'sb': 'B', 'ldc1': 'd', 'sdc1': 'D'}


def tokens(game, addr):
    ws = vn.func_words(game, addr)
    out = []
    hi = {}
    ptr = {4: 0}  # registers holding the first argument (the struct) + an offset
    spill = {}    # stack slots holding it
    for i, w in enumerate(ws):
        pc = addr + 4 * i
        ins = rabbitizer.Instruction(w, vram=pc)
        op = ins.getOpcodeName()
        rs, rt, rd = (w >> 21) & 0x1F, (w >> 16) & 0x1F, (w >> 11) & 0x1F
        if op == 'lui':
            hi[rt] = (w & 0xFFFF) << 16
            ptr.pop(rt, None)
            continue
        if op in ('addu', 'or', 'move', 'daddu') and rs in ptr and rt == 0:
            ptr[rd] = ptr[rs]
            continue
        if op in ('addu', 'or', 'move', 'daddu') and rt in ptr and rs == 0:
            ptr[rd] = ptr[rt]
            continue
        if op == 'addiu' and rs in ptr and rt != 29:
            imm = w & 0xFFFF
            ptr[rt] = ptr[rs] + (imm - 0x10000 if imm & 0x8000 else imm)
            continue
        if op == 'sw' and rs == 29 and rt in ptr:
            spill[w & 0xFFFF] = ptr[rt]  # the struct pointer saved on the stack
            continue
        if op == 'lw' and rs == 29 and (w & 0xFFFF) in spill:
            ptr[rt] = spill[w & 0xFFFF]
            continue
        if op not in LOADS or LOADS[op].islower():
            # A write to a register drops what it held (loads into an integer register, ALU results).
            dst = rt if op in ('addiu', 'andi', 'ori', 'lw', 'lh', 'lhu', 'lb', 'lbu', 'slti', 'sltiu') else rd
            if op not in ('jal', 'mtc1', 'swc1', 'sw', 'sh', 'sb', 'sdc1') and not op.startswith('b') and \
                    not op.endswith('.s') and not op.startswith('c.') and dst in ptr and dst not in (16, 17):
                ptr.pop(dst, None)
        if op == 'mtc1':
            rt = (w >> 16) & 0x1F
            if rt in hi:
                f = struct.unpack('>f', struct.pack('>I', hi[rt]))[0]
                out.append(('k', '%g' % f, pc))
            elif rt == 0:
                out.append(('k', '0', pc))
            continue
        if op == 'jal':
            t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            out.append(('c', '%08X' % t, pc))
            continue
        if op in LOADS:
            base = (w >> 21) & 0x1F
            off = w & 0xFFFF
            off = off - 0x10000 if off & 0x8000 else off
            if base == 29:
                continue  # the stack
            if base in hi and base != 0:
                a = (hi[base] + off) & 0xFFFFFFFF
                if LOADS[op] in 'fd' and 0x80000000 <= a < 0x80400000:
                    v = vn.word(game, a)
                    if v is not None:
                        f = struct.unpack('>f', struct.pack('>I', v))[0] if LOADS[op] == 'f' else None
                        if f is not None:
                            out.append(('k', '%g' % f, pc))
                            continue
                out.append((LOADS[op], '@%08X' % a, pc))
                continue
            if base in ptr:
                off += ptr[base]
                out.append((LOADS[op], '+%X' % off if off >= 0 else '-%X' % -off, pc))
            else:
                out.append((LOADS[op], 'r%d%+X' % (base, off), pc))
            continue
        if op.startswith('c.') or op in ('div.s', 'sqrt.s', 'cvt.d.s', 'cvt.s.d', 'cvt.s.w', 'trunc.w.s'):
            out.append(('o', op, pc))
    return out


SAME_BELOW = 0x224  # the car struct's fields below this are at the same offsets in both games


def key(t):
    """What two tokens must share to line up: constants, calls and float ops exactly, struct fields below SAME_BELOW
    by offset, others by access kind only (their offsets differ between the games)."""
    if t[0] in 'kco':
        return t[0] + t[1]
    if t[1][0] == '+' and int(t[1][1:], 16) < SAME_BELOW:
        return t[0] + t[1]
    return t[0]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    show_all = '--all' in sys.argv
    cs_addr, r2_addr = int(args[0], 16), int(args[1], 16)
    a = tokens('cs', cs_addr)
    b = tokens('r2', r2_addr)
    sm = difflib.SequenceMatcher(None, [key(t) for t in a], [key(t) for t in b], autojunk=False)
    pairs = collections.Counter()
    rows = []
    for tag, i1, i2, j1, j2 in sm.get_opcodes():
        n = max(i2 - i1, j2 - j1)
        for k in range(n):
            x = a[i1 + k] if i1 + k < i2 else None
            y = b[j1 + k] if j1 + k < j2 else None
            same = tag == 'equal' and x[1] == y[1]
            if tag == 'equal' and x[0] not in 'kco' and x[1] != y[1]:
                pairs[(x[1], y[1])] += 1
            if tag == 'equal' and x[0] not in 'kco':
                pairs[(x[1], y[1])] += 0
            rows.append((same, tag, x, y))
    fmt = lambda t: '' if t is None else '%08X %s %s' % (t[2], t[0], t[1])
    keep = set()
    for i, r in enumerate(rows):
        if show_all or r[1] != 'equal':
            keep.update((i - 1, i, i + 1))
    last = -2
    print('CS %08X (%d tokens)  vs  Rush 2 %08X (%d tokens)' % (cs_addr, len(a), r2_addr, len(b)))
    for i, (same, tag, x, y) in enumerate(rows):
        if i not in keep:
            continue
        if i != last + 1:
            print('   ...')
        last = i
        mark = ' ' if tag == 'equal' else {'replace': '|', 'delete': '<', 'insert': '>'}[tag]
        print('%-34s %s %s' % (fmt(x), mark, fmt(y)))
    print('\nstruct offsets in matching rows (CS -> Rush 2):')
    seen = collections.defaultdict(collections.Counter)
    for (x, y), n in pairs.items():
        seen[x][y] += max(n, 1)
    for x in sorted(seen, key=lambda s: (s[0], int(s[1:], 16) if s[1:].isalnum() else 0)):
        print('  %s -> %s' % (x, ', '.join('%s' % y for y in seen[x])))


if __name__ == '__main__':
    main()
