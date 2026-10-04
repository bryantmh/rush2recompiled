"""Finds instructions in Rush 2's disassembly that address given data ranges (lui + addiu/load/store pairs).

Usage: python refscan.py START END [START END ...]   (hex vram ranges; prints function, instruction, resolved address)
"""
import re, sys

import os
ASM = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'analysis', 'out_disasm', 'r2.asm')
line_re = re.compile(r'^([0-9A-F]{8}) (\w+(?:\.\w+)?)\s+(.*)$')

def scan(ranges):
    func = None
    hi = {}
    lui_at = {}
    out = []
    for line in open(ASM):
        line = line.rstrip()
        if line.startswith('func_'):
            func = line[:-1]
            hi = {}
            lui_at = {}
            continue
        m = line_re.match(line)
        if not m:
            continue
        addr, op, args = m.groups()
        a = [x.strip() for x in args.split(',')]
        if op == 'lui':
            hi[a[0]] = int(a[1], 16) << 16
            lui_at[a[0]] = addr
            continue
        mm = re.match(r'(-?0x[0-9A-Fa-f]+|-?\d+)\((\$\w+)\)', a[-1]) if a else None
        val = None
        src = None
        if op in ('addiu', 'ori') and len(a) == 3 and a[1] in hi:
            src = a[1]
            imm = int(a[2], 16) if 'x' in a[2] else int(a[2])
            val = hi[a[1]] + (imm if op == 'addiu' else imm & 0xFFFF)
            if op == 'addiu' and a[0] == a[1]:
                pass
        elif mm and mm.group(2) in hi:
            src = mm.group(2)
            val = hi[mm.group(2)] + int(mm.group(1), 16 if 'x' in mm.group(1) else 10)
        if val is not None:
            val &= 0xFFFFFFFF
            for lo, hi_ in ranges:
                if lo <= val < hi_:
                    out.append((func, addr, op, args, val, lui_at.get(src)))
        if op == 'addu' and len(a) == 3 and (a[1] in hi) != (a[2] in hi):
            r = a[1] if a[1] in hi else a[2]
            hi[a[0]] = hi[r]
            lui_at[a[0]] = lui_at.get(r)
            continue
        # a write to a register clears its lui value (except the addiu-to-self case keeps the full address)
        if a and a[0] in hi and op not in ('sw', 'sh', 'sb', 'swc1', 'sdc1'):
            del hi[a[0]]
    return out

if __name__ == '__main__':
    v = [int(x, 16) for x in sys.argv[1:]]
    for f, addr, op, args, val, lui in scan(list(zip(v[::2], v[1::2]))):
        print(f'{f:14s} {addr} {op:6s} {args:30s} -> {val:08X}  (lui {lui})')
