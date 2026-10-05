"""Finds global arrays Rush 2 indexes by player or view number (candidates for tools/players4/relocate.py).

Within each function, registers holding a player or view index are tracked through the instructions in order
(control flow is ignored, so this over-approximates). Seeds:
- a car's view number, lb/lbu 0x347(car)
- loop counters compared against the number of players or views (D_8010C3E2, D_8010C3EC)
- the first argument ($a0) of functions known to take a view or player index (ARG_FUNCS)
Indices flow through arithmetic. An address formed from a lui and a tainted index, then dereferenced or kept as
a pointer, is reported with its base address.

    python taint.py [function ...]    report arrays (all functions by default)
"""
import collections
import os
import re
import sys

ASM = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'analysis', 'out_disasm', 'r2.asm')
line_re = re.compile(r'^([0-9A-F]{8}) (\w+(?:\.\w+)?)\s*(.*)$')

COUNTS = {0x8010C3E2, 0x8010C3EC}
# Functions whose $a0 is a view or player index.
ARG_FUNCS = {
    'func_8007C624',  # draws view a0
    'func_80081074',  # view fog
    'func_80054A50',  # sets up view a0
    'func_800546B0',  # view a0 viewport/scissor
    'func_80054430',  # view a0 pointer
    'func_80093DC0',  # port a0
}
# Race HUD element callbacks (layout table 0x800C2DB4): the element's parameter (+0x2C) holds its player.
HUD_CALLBACKS = {
    'func_800B7B1C', 'func_800B7E88', 'func_800BA2A8', 'func_800B9754', 'func_800B9978', 'func_800B94C0',
    'func_800BA608', 'func_800B92F4', 'func_800BA868', 'func_800B9C44', 'func_800B9CC0', 'func_800B920C',
    'func_800B8CC8', 'func_800B7654', 'func_80060E34', 'func_80060F24',
}
ARITH = {'sll', 'sra', 'srl', 'addu', 'subu', 'addiu', 'or', 'andi', 'ori', 'mflo', 'sllv', 'srav', 'xori', 'xor'}


def funcs():
    out, cur = {}, None
    for line in open(ASM):
        line = line.rstrip()
        if line.startswith('func_'):
            cur = line[:-1]
            out[cur] = []
            continue
        m = line_re.match(line)
        if m and cur:
            out[cur].append(m.groups())
    return out


def regs(args):
    return [x.strip() for x in args.split(',')] if args else []


def scan(name, ins, arg_taint=None, calls=None):
    """Arrays indexed by a tainted register in one function. arg_taint: argument registers holding an index on entry.
    calls, if given, collects (callee, tainted argument registers) for each call made with an index."""
    found = []
    # Pass 1: registers loaded with a player/view count.
    count_regs_at = set()
    hi = {}
    for addr, op, args in ins:
        a = regs(args)
        if op == 'lui':
            hi[a[0]] = int(a[1], 16) << 16
            continue
        mm = re.match(r'(-?0x[0-9A-F]+|-?\d+)\((\$\w+)\)', a[-1]) if a else None
        if op in ('lh', 'lhu') and mm and mm.group(2) in hi:
            if (hi[mm.group(2)] + int(mm.group(1), 0)) & 0xFFFFFFFF in COUNTS:
                count_regs_at.add(a[0])
    # Pass 2: taint.
    tainted = set(arg_taint or ())
    if name in ARG_FUNCS:
        tainted.add('$a0')
    pending_call = None
    pending_left = 0
    hi = {}       # reg -> lui high part
    based = {}    # reg -> (hi base) for regs = lui + tainted index
    counts = set()
    stack = {}  # stack slot -> scale, for slots holding an index
    scale = collections.defaultdict(lambda: 1)  # tainted reg -> multiple of the index it holds
    consts = {}  # reg -> small constant (for multiplies)
    based_scale = {}  # based reg -> stride
    # Two passes: loop counters are recognized at the compare that ends the loop, after the body's accesses.
    for addr, op, args in ins + [('PASS', 'pass', '')] + ins:
        if op == 'pass':
            found.clear()
            if calls is not None:
                calls.clear()
            hi, based, pending_call = {}, {}, None
            continue
        if pending_call is not None:
            pending_left -= 1
            if pending_left < 0:
                if calls is not None:
                    args_t = {r for r in ('$a0', '$a1', '$a2', '$a3') if r in tainted}
                    if args_t:
                        calls.append((pending_call, args_t))
                for r in ('$v0', '$v1'):
                    tainted.discard(r)
                pending_call = None
        a = regs(args)
        if not a:
            continue
        dst = a[0]
        if op == 'jal':
            pending_call = 'func_' + a[0].replace('0x', '').upper()
            pending_left = 1
            continue
        mm = re.match(r'(-?0x[0-9A-F]+|-?\d+)\((\$\w+)\)', a[-1])
        if mm and mm.group(2) == '$sp':
            slot = int(mm.group(1), 0)
            if op in ('sw', 'sh', 'sb') and dst in tainted:
                stack[slot] = scale[dst]
            elif op in ('lw', 'lh', 'lhu', 'lb', 'lbu') and slot in stack:
                tainted.add(dst)
                scale[dst] = stack[slot]
                hi.pop(dst, None)
                based.pop(dst, None)
                continue
        if op == 'lui':
            hi[dst] = int(a[1], 16) << 16
            tainted.discard(dst)
            based.pop(dst, None)
            continue
        # Memory access through a based register.
        if mm and mm.group(2) in based:
            base = (based[mm.group(2)] + int(mm.group(1), 0)) & 0xFFFFFFFF
            found.append((addr, op, base, based_scale.get(mm.group(2), 1)))
        if op in ('lh', 'lhu') and mm and mm.group(2) in hi and \
                (hi[mm.group(2)] + int(mm.group(1), 0)) & 0xFFFFFFFF in COUNTS:
            counts.add(dst)
            tainted.discard(dst)
            hi.pop(dst, None)
            continue
        if name in HUD_CALLBACKS and op in ('lw', 'lh', 'lhu', 'lb', 'lbu') and mm and int(mm.group(1), 0) in (0x2C, 0x2D, 0x2E, 0x2F)                 and mm.group(2) != '$sp':
            tainted.add(dst)
            scale[dst] = 1
            hi.pop(dst, None)
            based.pop(dst, None)
            continue
        if op in ('lb', 'lbu') and mm and int(mm.group(1), 0) == 0x347:
            tainted.add(dst)
            scale[dst] = 1
            hi.pop(dst, None)
            based.pop(dst, None)
            continue
        if op in ('slt', 'sltu') and len(a) == 3 and a[2] in counts:
            tainted.add(a[1])
            scale[a[1]] = 1
            continue
        if op in ('bne', 'beq', 'bnel', 'beql') and len(a) >= 2 and (a[0] in counts or a[1] in counts):
            other = a[1] if a[0] in counts else a[0]
            tainted.add(other)
            scale[other] = 1
            continue
        if op == 'addu' and len(a) == 3:
            x, y = a[1], a[2]
            if (x in hi and y in tainted) or (y in hi and x in tainted):
                based[dst] = hi[x] if x in hi else hi[y]
                based_scale[dst] = scale[y] if y in tainted else scale[x]
                tainted.discard(dst)
                hi.pop(dst, None)
                continue
            if (x in based and y in tainted) or (y in based and x in tainted):
                based[dst] = based[x] if x in based else based[y]
                based_scale[dst] = scale[y] if y in tainted else scale[x]
                continue
        if op == 'addiu' and len(a) == 3 and a[1] in based:
            based[dst] = based[a[1]] + int(a[2], 0)
            based_scale[dst] = based_scale.get(a[1], 1)
            # A pointer kept for later use.
            found.append((addr, 'ptr', based[dst] & 0xFFFFFFFF, based_scale[dst]))
            continue
        if op in ('addiu', 'ori') and len(a) == 3 and a[1] == '$zero':
            consts[dst] = int(a[2], 0)
        if op in ('mult', 'multu'):
            t = a[0] if a[0] in tainted else (a[1] if a[1] in tainted else None)
            if t is not None:
                other = a[1] if t == a[0] else a[0]
                scale['$lo'] = scale[t] * consts.get(other, 1)
                tainted.add('$lo')
            continue
        if op in ('mult', 'multu'):
            if a[0] in tainted or a[1] in tainted:
                tainted.add('$lo')
            continue
        if op == 'mflo':
            if '$lo' in tainted:
                tainted.add(dst)
                scale[dst] = scale['$lo']
            else:
                tainted.discard(dst)
            hi.pop(dst, None)
            based.pop(dst, None)
            continue
        if op in ARITH and len(a) >= 2:
            srcs = a[1:]
            if any(s in tainted for s in srcs):
                if op == 'sll':
                    scale[dst] = scale[a[1]] << int(a[2], 0)
                elif op in ('sra', 'srl'):
                    scale[dst] = max(1, scale[a[1]] >> int(a[2], 0))
                elif op == 'andi':
                    scale[dst] = 1 if scale[a[1]] > 0xF else scale[a[1]]
                elif op in ('addu', 'subu') and a[1] in tainted and a[2] in tainted:
                    scale[dst] = scale[a[1]] + scale[a[2]] if op == 'addu' else scale[a[1]] - scale[a[2]]
                elif op in ('addu', 'subu'):
                    scale[dst] = scale[a[1]] if a[1] in tainted else scale[a[2]]
                else:
                    scale[dst] = scale[a[1]] if a[1] in tainted else 1
                tainted.add(dst)
                hi.pop(dst, None)
                based.pop(dst, None)
                continue
            if op == 'addiu' and a[1] in hi:
                hi[dst] = hi[a[1]] + int(a[2], 0)
                based.pop(dst, None)
                tainted.discard(dst)
                continue
        # Any other write kills the destination's address parts. Taint sticks (control flow is ignored, so a register
        # reused on another path keeps it): this over-approximates, and the report is reviewed by hand.
        if op not in ('sw', 'sh', 'sb', 'swc1', 'sdc1', 'beq', 'bne', 'beql', 'bnel', 'blez', 'bgtz', 'bltz', 'bgez',
                      'jr', 'jal', 'j', 'b', 'bc1t', 'bc1f', 'bc1tl', 'bc1fl', 'mtc1', 'ctc1', 'c.lt.s', 'c.le.s',
                      'c.eq.s', 'div', 'divu', 'nop'):
            hi.pop(dst, None)
            based.pop(dst, None)
            counts.discard(dst)
    return found


def propagate(fs):
    """Argument registers holding an index for each function, through calls, to a fixpoint."""
    arg_taint = collections.defaultdict(set)
    changed = True
    while changed:
        changed = False
        for n, ins in fs.items():
            calls = []
            scan(n, ins, arg_taint.get(n), calls)
            for callee, regs_t in calls:
                if callee in fs and not regs_t <= arg_taint[callee]:
                    arg_taint[callee] |= regs_t
                    changed = True
    return arg_taint


def main():
    fs = funcs()
    arg_taint = propagate(fs)
    names = sys.argv[1:] or sorted(fs)
    by_base = collections.defaultdict(set)
    for n in names:
        for addr, op, base, stride in scan(n, fs[n], arg_taint.get(n)):
            by_base[base].add((n, stride))
    for base in sorted(by_base):
        strides = sorted({st for f, st in by_base[base]})
        print('%08X  stride %s  %s' % (base, ','.join(hex(x) for x in strides), ' '.join(sorted({f for f, st in by_base[base]}))))


if __name__ == '__main__':
    main()
