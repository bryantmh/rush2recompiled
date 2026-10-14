"""Writes analysis/out_disasm/r2.asm, the plain Rush 2 disassembly read by tools/rush2049/refscan.py, annot2.py,
decomp.py and tools/players4/ (taint.py, triage.py, relocate.py), from the recomp ROM and syms/rush2.us.syms.toml.

    python tools/gen_disasm.py [out.asm]

Each function starts with a "func_XXXXXXXX:" line, then one "ADDRESS mnemonic operands" line per instruction.
Reads RUSH2_ROM (default rush2.us.recomp.z64 in the project root, made by tools/extract.py). Needs rabbitizer.
"""
import os
import re
import sys

import rabbitizer

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
ROM = os.environ.get('RUSH2_ROM', os.path.join(REPO, 'rush2.us.recomp.z64'))


def sections():
    out, cur = [], None
    for line in open(os.path.join(REPO, 'syms', 'rush2.us.syms.toml'), encoding='utf-8'):
        if line.startswith('[[section]]'):
            cur = {'functions': []}
            out.append(cur)
            continue
        m = re.match(r'(rom|vram|size) = (0x[0-9A-Fa-f]+)', line)
        if m and cur is not None and m.group(1) not in cur:
            cur[m.group(1)] = int(m.group(2), 16)
            continue
        m = re.search(r'vram = (0x[0-9A-Fa-f]+), size = (0x[0-9A-Fa-f]+)', line)
        if m and cur is not None:
            cur['functions'].append((int(m.group(1), 16), int(m.group(2), 16)))
    return out


def main():
    out_path = sys.argv[1] if len(sys.argv) > 1 else os.path.join(REPO, 'analysis', 'out_disasm', 'r2.asm')
    rom = open(ROM, 'rb').read()
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    count = 0
    with open(out_path, 'w') as f:
        for sec in sections():
            for vram, size in sorted(sec['functions']):
                f.write('func_%08X:\n' % vram)
                base = sec['rom'] + (vram - sec['vram'])
                for i in range(0, size, 4):
                    word = int.from_bytes(rom[base + i:base + i + 4], 'big')
                    ins = rabbitizer.Instruction(word, vram + i)
                    text = ins.disassemble()
                    parts = text.split(None, 1)
                    f.write('%08X %s %s\n' % (vram + i, parts[0], parts[1].strip() if len(parts) > 1 else ''))
                count += 1
    print('%d functions -> %s' % (count, out_path))


if __name__ == '__main__':
    main()
