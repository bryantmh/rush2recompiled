"""Finds the data arrays Rush 2 indexes by a value (e.g. the car preview id): taints a register at a function's entry,
follows it through arithmetic, stack spills and calls (interprocedurally, by argument register), and reports every
lui-based address the tainted value is added to, with the multiplier when it can be read off.

    python idscan.py func_8008813C:a0 func_8005B978:s3 ...
"""
import re, sys
import refscan

line_re = refscan.line_re


def load():
    funcs, cur = {}, None
    for line in open(refscan.ASM):
        line = line.rstrip()
        if line.startswith('func_'):
            cur = line[:-1]
            funcs[cur] = []
            continue
        m = line_re.match(line)
        if m and cur:
            funcs[cur].append(m.groups())
    return funcs


def regs(args):
    return [x.strip() for x in args.split(',')] if args else []


ARGS = ['$a0', '$a1', '$a2', '$a3']


def scan(funcs, fname, tainted, depth, seen, out):
    key = (fname, tuple(sorted(tainted)))
    if key in seen or fname not in funcs or depth > 6:
        return
    seen.add(key)
    taint = set(tainted)
    slots = set()
    hi = {}
    mul = {}  # tainted reg -> scale factor estimate
    for addr, op, args in funcs[fname]:
        a = regs(args)
        m = re.match(r'(-?0x[0-9A-Fa-f]+|-?\d+)\((\$\w+)\)', a[-1]) if a else None
        if op == 'lui':
            hi[a[0]] = int(a[1], 16) << 16
            taint.discard(a[0])
            continue
        if op in ('jal',):
            callee = 'func_' + a[0][2:].upper().rjust(8, '0') if a[0].startswith('0x') else None
            t_args = [r for r in ARGS if r in taint]
            if callee and t_args:
                scan(funcs, callee, set(t_args), depth + 1, seen, out)
            continue
        if m and op in ('sw', 'sh') and m.group(2) == '$sp':
            if a[0] in taint:
                slots.add(int(m.group(1), 0))
            continue
        if m and op in ('lw', 'lh', 'lhu') and m.group(2) == '$sp':
            if int(m.group(1), 0) in slots:
                taint.add(a[0])
            else:
                taint.discard(a[0])
            hi.pop(a[0], None)
            continue
        if m and m.group(2) in taint and m.group(2) in hi:
            out.setdefault(hi[m.group(2)] + int(m.group(1), 0), set()).add((fname, addr, mul.get(m.group(2))))
        if op in ('sll', 'sra', 'srl') and len(a) == 3 and a[1] in taint:
            taint.add(a[0])
            mul[a[0]] = mul.get(a[1], 1) * (2 ** int(a[2], 0) if op == 'sll' else 1)
            continue
        if op in ('addu', 'subu', 'or') and len(a) == 3:
            src_t = [r for r in a[1:] if r in taint]
            src_h = [r for r in a[1:] if r in hi]
            if src_t and src_h:
                base = hi[src_h[0]]
                out.setdefault(base, set()).add((fname, addr, None))
                taint.add(a[0])
                hi[a[0]] = base
                continue
            if src_t:
                taint.add(a[0])
                hi.pop(a[0], None)
                continue
            taint.discard(a[0])
            if op == 'addu' and src_h and a[1] != a[2]:
                hi[a[0]] = hi[src_h[0]]
            else:
                hi.pop(a[0], None)
            continue
        if op == 'addiu' and len(a) == 3:
            if a[1] in hi and a[1] not in taint:
                hi[a[0]] = hi[a[1]] + int(a[2], 0)
                taint.discard(a[0])
            elif a[1] in taint:
                taint.add(a[0])
                if a[1] in hi:
                    hi[a[0]] = hi[a[1]] + int(a[2], 0)
            else:
                taint.discard(a[0])
                hi.pop(a[0], None)
            continue
        if op in ('multu', 'mult') and any(r in taint for r in a):
            taint.add('$lo')
            continue
        if op == 'mflo':
            if '$lo' in taint:
                taint.add(a[0])
            else:
                taint.discard(a[0])
            hi.pop(a[0], None)
            continue
        if op in ('div', 'divu'):
            continue
        if op in ('mfhi',):
            taint.discard(a[0])
            continue
        if a and a[0].startswith('$') and op not in ('sw', 'sh', 'sb', 'swc1', 'sdc1', 'beq', 'bne', 'beql', 'bnel',
                                                      'blez', 'bgez', 'bltz', 'bgtz', 'jr', 'jalr'):
            taint.discard(a[0])
            hi.pop(a[0], None)


def main():
    funcs = load()
    out = {}
    seen = set()
    for spec in sys.argv[1:]:
        f, r = spec.split(':')
        scan(funcs, f, {'$' + r}, 0, seen, out)
    for base in sorted(out):
        print('%08X' % base, sorted({(f[5:], a) for f, a, _ in out[base]})[:6])


if __name__ == '__main__':
    main()
