"""Lists the Rush 2 instructions that load or store a global's bytes, and writes the us.toml patches that turn its
signed byte loads (lb) into unsigned ones (lbu).

Registers holding constants (lui, then addiu/ori on it) are followed through each function in order (control flow is
ignored, so a value set on one path is assumed on the others: read each hit). A load or store whose address works
out to ADDR is listed; so is a place the address itself escapes (passed to a call in $a0-$a3, or stored), since the
callee or the reader of the stored pointer then accesses it out of sight.

    python tools/byte_reads.py ADDR [ADDR ...]          list the accesses
    python tools/byte_reads.py ADDR --lbu [--skip VRAM,...] [--also VRAM,...]
        print us.toml patches turning each lb into lbu (--also: more lb instructions to turn, found by hand, such as
        loads through a pointer into a copy of the value)

Used for the track select id byte 0x8010C3F0 (issue #55: custom tracks past id 127 need it read unsigned).
Reads RUSH2_ROM (default rush2.us.recomp.z64) and syms/rush2.us.syms.toml. Needs rabbitizer.
"""
import argparse
import os
import re
import sys

import rabbitizer

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_disasm  # noqa: E402

REPO = gen_disasm.REPO

LOADS = {'lb', 'lbu', 'lh', 'lhu', 'lw', 'lwu', 'lwc1', 'ld', 'ldc1', 'lwl', 'lwr'}
STORES = {'sb', 'sh', 'sw', 'swc1', 'sd', 'sdc1', 'swl', 'swr'}
SIZE = {'lb': 1, 'lbu': 1, 'sb': 1, 'lh': 2, 'lhu': 2, 'sh': 2}


def names():
    out = {}
    for line in open(os.path.join(REPO, 'syms', 'rush2.us.syms.toml'), encoding='utf-8'):
        m = re.search(r'name = "([^"]+)", vram = (0x[0-9A-Fa-f]+)', line)
        if m:
            out[int(m.group(2), 16)] = m.group(1)
    return out


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def scan(rom, targets):
    """Yields (function vram, instruction vram, word, kind, text) for each access to a target address."""
    for sec in gen_disasm.sections():
        for fvram, size in sorted(sec['functions']):
            base = sec['rom'] + (fvram - sec['vram'])
            known = {}      # register -> 32-bit value
            # Each register's lui values anywhere in the function: a load at a branch target can use one set on
            # another path (the in-order scan has lost it there), so a base with one of them counts too.
            luis = {}
            for i in range(0, size, 4):
                ins = rabbitizer.Instruction(int.from_bytes(rom[base + i:base + i + 4], 'big'), fvram + i)
                if ins.getOpcodeName() == 'lui':
                    luis.setdefault(ins.rt.name, set()).add((ins.getProcessedImmediate() & 0xFFFF) << 16)
            for i in range(0, size, 4):
                vram = fvram + i
                word = int.from_bytes(rom[base + i:base + i + 4], 'big')
                ins = rabbitizer.Instruction(word, vram)
                op = ins.getOpcodeName()
                text = ins.disassemble()
                rs = ins.rs.name if ins.hasOperandAlias(rabbitizer.OperandType.cpu_rs) else None
                rt = ins.rt.name if ins.hasOperandAlias(rabbitizer.OperandType.cpu_rt) else None
                if op in LOADS or op in STORES:
                    n = SIZE.get(op, 4)
                    bases = {known[rs]} if rs in known else luis.get(rs, set())
                    if any(addr <= t < addr + n for t in targets
                           for addr in ((v + s16(word)) & 0xFFFFFFFF for v in bases)):
                        yield fvram, vram, word, op, text
                    if rs in known:
                        if op in STORES and rt in known and known[rt] in targets:
                            yield fvram, vram, word, 'escape (stored)', text
                    if op in LOADS and rt is not None:
                        known.pop(rt, None)
                    continue
                if op == 'lui':
                    known[rt] = (word & 0xFFFF) << 16
                    continue
                if op in ('addiu', 'daddiu') and rs in known:
                    known[rt] = (known[rs] + s16(word)) & 0xFFFFFFFF
                    continue
                if op == 'ori' and rs in known:
                    known[rt] = known[rs] | (word & 0xFFFF)
                    continue
                if op in ('jal', 'jalr'):
                    for a in ('a0', 'a1', 'a2', 'a3'):
                        if known.get(a) in targets:
                            yield fvram, vram, word, 'escape (call arg $%s)' % a, text
                    # The call clobbers the caller-saved registers.
                    for r in list(known):
                        if r not in ('s0', 's1', 's2', 's3', 's4', 's5', 's6', 's7', 'fp', 'sp', 'gp'):
                            del known[r]
                    continue
                # Any other write to a register forgets it.
                if ins.modifiesRd():
                    known.pop(ins.rd.name, None)
                if ins.modifiesRt():
                    known.pop(ins.rt.name, None)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addrs', nargs='+')
    ap.add_argument('--lbu', action='store_true', help='print us.toml patches turning each lb into lbu')
    ap.add_argument('--skip', default='', help='comma-separated instruction addresses to leave out of --lbu')
    ap.add_argument('--also', default='', help='comma-separated lb instruction addresses to turn as well')
    a = ap.parse_args()
    targets = {int(x, 16) for x in a.addrs}
    skip = {int(x, 16) for x in a.skip.split(',') if x}
    also = {int(x, 16) for x in a.also.split(',') if x}
    rom = open(gen_disasm.ROM, 'rb').read()
    sym = names()
    hits = list(scan(rom, targets))
    if also:
        for sec in gen_disasm.sections():
            for fvram, size in sec['functions']:
                for vram in also:
                    if fvram <= vram < fvram + size:
                        at = sec['rom'] + (vram - sec['vram'])
                        word = int.from_bytes(rom[at:at + 4], 'big')
                        ins = rabbitizer.Instruction(word, vram)
                        if ins.getOpcodeName() != 'lb':
                            sys.exit('%08X is %s, not lb' % (vram, ins.disassemble()))
                        hits.append((fvram, vram, word, 'lb', ins.disassemble()))
        hits.sort(key=lambda h: h[1])
    if not a.lbu:
        for fvram, vram, word, kind, text in hits:
            print('%08X %-40s %-22s %s' % (vram, sym.get(fvram, 'func_%08X' % fvram), kind, text))
        return
    for fvram, vram, word, kind, text in hits:
        if kind != 'lb' or vram in skip:
            continue
        new = word | 0x10000000     # lb (0x20) -> lbu (0x24)
        new_text = rabbitizer.Instruction(new, vram).disassemble()
        print('[[patches.instruction]]')
        print('func = "%s"' % sym.get(fvram, 'func_%08X' % fvram))
        print('vram = 0x%08X' % vram)
        print('value = 0x%08X # %s' % (new, re.sub(r'\s+', ' ', new_text)))
        print()


if __name__ == '__main__':
    main()
