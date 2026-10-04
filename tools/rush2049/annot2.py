"""Annotate analysis/out_disasm/r2.asm with resolved lui pair addresses (research aid).

Writes tools/rush2049/out/r2a.asm (same lines + ' ; XXXXXXXX') and prints nothing.
Usage: python annot2.py [in.asm] [out.asm]
"""
import re, sys, os
REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
src = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'analysis', 'out_disasm', 'r2.asm')
dst = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(__file__), 'out', 'r2a.asm')
line_re = re.compile(r'^([0-9A-F]{8}) (\S+)\s+(.*)$')
mem_re = re.compile(r'^(\$\w+), (-?0x[0-9A-Fa-f]+|-?\d+)\((\$\w+)\)$')
lui = {}
out = open(dst, 'w')
def num(s): return int(s, 0)
for ln in open(src):
    ln = ln.rstrip('\n')
    if ln.endswith(':'):
        lui = {}; out.write(ln + '\n'); continue
    m = line_re.match(ln)
    if not m:
        out.write(ln + '\n'); continue
    op, args = m.group(2), m.group(3)
    a = [x.strip() for x in args.split(',')]
    note = ''
    if op == 'lui':
        lui[a[0]] = num(a[1]) << 16
    else:
        mm = mem_re.match(args)
        if mm and mm.group(3) in lui:
            note = ' ; %08X' % ((lui[mm.group(3)] + num(mm.group(2))) & 0xFFFFFFFF)
            if not op.startswith('s') and not op.startswith('lwc') and not op.startswith('ldc'):
                lui.pop(mm.group(1), None)
            elif op.startswith('lwc') or op.startswith('ldc'):
                pass
        elif op in ('addiu', 'ori', 'addi') and len(a) == 3 and a[1] in lui:
            v = lui[a[1]] + (num(a[2]) if op != 'ori' else num(a[2]) & 0xFFFF)
            note = ' ; %08X' % (v & 0xFFFFFFFF)
            lui.pop(a[0], None)
            if op != 'ori' or True:
                pass
        elif op == 'addu' and len(a) == 3 and (a[1] in lui or a[2] in lui):
            # base + index: keep the base as 'lui' value for following loads
            base = lui.get(a[1], lui.get(a[2]))
            if a[0] not in (a[1], a[2]) or True:
                lui[a[0]] = base
            out.write(ln + '\n'); continue
        else:
            if a and a[0].startswith('$') and not op.startswith('s') and not op.startswith('b') and op not in ('jr', 'jalr', 'mtc1', 'dmtc1', 'ctc1', 'mult', 'multu', 'div', 'divu'):
                lui.pop(a[0], None)
    if op == 'jr' and args == '$ra':
        pass
    out.write(ln + note + '\n')
