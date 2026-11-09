"""Annotated disassembly of the PS1 SF Rush program (EXE/RUSH.EXE, loaded at 0x80010000). Research aid,
docs/sfrush_ps1.md.

  python disps1.py asm [OUT]          whole program to OUT (default tools/sfrush_ps1/out/rush.asm): functions start at
                                   jal targets, lui/addiu (lw/sw...) pairs are resolved and strings they point at shown
  python disps1.py func ADDR          one function (to the next jal target)
  python disps1.py refs ADDR          instructions whose resolved address (lui pair) is ADDR (or within +-0x40 with --near)
  python disps1.py str TEXT           addresses of strings containing TEXT, and the code that references them
"""
import os, re, struct, sys
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps1 import Disc, EXE_ADDR

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out')


class Exe:
    def __init__(self):
        self.data = Disc().exe()
        self.end = EXE_ADDR + len(self.data)
        md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS32 + capstone.CS_MODE_LITTLE_ENDIAN)
        self.md = md
        self.words = struct.unpack('<%dI' % (len(self.data) // 4), self.data[:len(self.data) // 4 * 4])
        self.targets = set()
        for i, w in enumerate(self.words):
            if (w >> 26) == 3:   # jal
                self.targets.add(((EXE_ADDR + i * 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))

    def w(self, a): return self.words[(a - EXE_ADDR) // 4]
    def h(self, a): return struct.unpack_from('<h', self.data, a - EXE_ADDR)[0]

    def string(self, a):
        if not EXE_ADDR <= a < self.end:
            return None
        o = a - EXE_ADDR; e = self.data.find(b'\0', o, o + 80)
        s = self.data[o:e] if e > o else b''
        if len(s) >= 3 and all(32 <= c < 127 for c in s):
            return s.decode()
        return None

    def resolve(self, lo, hi):
        """(address, text) per instruction in [lo, hi): lui-pair addresses resolved per register, reset at jumps."""
        regs = {}
        out = []
        for a in range(lo, hi, 4):
            w = self.w(a)
            ins = next(self.md.disasm(struct.pack('<I', w), a), None)
            text = '%s %s' % (ins.mnemonic, ins.op_str) if ins else '.word 0x%08x' % w
            op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; imm = w & 0xFFFF
            simm = imm - 0x10000 if imm & 0x8000 else imm
            note = ''; ref = None
            if op == 0x0F:
                regs[rt] = imm << 16
            elif op in (0x09, 0x0D) and rs in regs:   # addiu / ori
                ref = (regs[rs] + simm) & 0xFFFFFFFF if op == 9 else regs[rs] | imm
                regs[rt] = ref
            elif op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x22, 0x26, 0x2A, 0x2E, 0x32, 0x3A) and rs in regs:
                ref = (regs[rs] + simm) & 0xFFFFFFFF
                if op in (0x20, 0x21, 0x23, 0x24, 0x25):
                    regs.pop(rt, None)
            elif op == 3:
                t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2); note = 'func_%08X' % t
            else:
                # any other write to rt/rd drops it
                if op == 0:
                    regs.pop((w >> 11) & 31, None)
                elif op not in (2, 4, 5, 6, 7, 1, 0x2B, 0x28, 0x29):
                    regs.pop(rt, None)
            if ref is not None:
                s = self.string(ref)
                note = '0x%08X' % ref + (' "%s"' % s if s else '')
            out.append((a, w, text, note, ref))
            if op in (2, 3) or (op == 0 and (w & 0x3F) in (8, 9)):
                regs = {}
        return out

    def func_end(self, a):
        later = [t for t in self.targets if t > a]
        return min(later) if later else self.end

    def lines(self, lo, hi):
        for a, w, text, note, _ in self.resolve(lo, hi):
            head = '\nfunc_%08X:\n' % a if a in self.targets else ''
            yield '%s%08X: %08x  %-40s %s' % (head, a, w, text, ('; ' + note) if note else '')


def main():
    e = Exe()
    cmd = sys.argv[1]
    if cmd == 'asm':
        out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(OUT, 'rush.asm')
        os.makedirs(os.path.dirname(out), exist_ok=True)
        code_end = max(e.targets)
        with open(out, 'w') as f:
            for l in e.lines(EXE_ADDR, min(e.func_end(code_end), e.end)):
                f.write(l + '\n')
        print(out)
    elif cmd == 'func':
        a = int(sys.argv[2], 16)
        for l in e.lines(a, e.func_end(a)):
            print(l)
    elif cmd == 'refs':
        t = int(sys.argv[2], 16); near = '--near' in sys.argv
        for a, w, text, note, ref in e.resolve(EXE_ADDR, e.end):
            if ref is not None and (ref == t or (near and abs(ref - t) <= 0x40)):
                f = max([x for x in e.targets if x <= a], default=0)
                print('%08X (func_%08X): %s ; %s' % (a, f, text, note))
    elif cmd == 'str':
        pat = sys.argv[2].encode()
        hits = [m.start() + EXE_ADDR for m in re.finditer(re.escape(pat), e.data, re.I)]
        refs = {}
        for a, w, text, note, ref in e.resolve(EXE_ADDR, e.end):
            if ref is not None:
                refs.setdefault(ref, []).append(a)
        for h in hits:
            # the string's start
            s = h
            while s > EXE_ADDR and 32 <= e.data[s - 1 - EXE_ADDR] < 127:
                s -= 1
            print('%08X "%s"' % (s, e.string(s)))
            for a in refs.get(s, []):
                f = max([x for x in e.targets if x <= a], default=0)
                print('    ref %08X in func_%08X' % (a, f))


if __name__ == '__main__':
    main()
