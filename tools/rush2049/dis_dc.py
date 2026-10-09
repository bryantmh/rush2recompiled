"""SH-4 disassembler for the Dreamcast Rush 2049 executable (1ST_READ.BIN, loaded at 0x8C010000).

    dis_dc.py dis ADDR [COUNT]      disassemble COUNT instructions (default 64) from ADDR
    dis_dc.py func ADDR             disassemble the function at ADDR, up to the rts that no branch jumps past
    dis_dc.py refs VALUE            literal-pool words equal to VALUE and the instructions that load them
    dis_dc.py calls ADDR            call sites (jsr through a pool word, bsr) of the function at ADDR
    dis_dc.py str TEXT              addresses of a string and the instructions that load its address

PC-relative loads (mov.l/mov.w @(disp,pc)) are annotated with the value they load, and with the string at that
address when there is one. The executable is DC_EXE (default tmp/dc/files/1ST_READ.BIN, from dc_cdi.py all).
See docs/rush2049_research/dreamcast.md.
"""
import os, re, struct, sys

import capstone

BASE = 0x8C010000
EXE = os.environ.get('DC_EXE', os.path.join(os.path.dirname(__file__), '..', '..', 'tmp', 'dc', 'files', '1ST_READ.BIN'))
_d = None
_md = None


def data():
    global _d
    if _d is None:
        _d = open(EXE, 'rb').read()
    return _d


def md():
    global _md
    if _md is None:
        _md = capstone.Cs(capstone.CS_ARCH_SH, capstone.CS_MODE_SH4 | capstone.CS_MODE_SHFPU | capstone.CS_MODE_LITTLE_ENDIAN)
    return _md


def u32(a):
    o = a - BASE
    return struct.unpack_from('<I', data(), o)[0] if 0 <= o <= len(data()) - 4 else None


def u16(a):
    o = a - BASE
    return struct.unpack_from('<H', data(), o)[0] if 0 <= o <= len(data()) - 2 else None


def cstring(a):
    o = a - BASE
    d = data()
    if not 0 <= o < len(d):
        return None
    e = d.find(b'\0', o)
    s = d[o:e]
    if 2 <= len(s) < 80 and all(32 <= c < 127 for c in s):
        return s.decode()
    return None


def insn(a):
    """(mnemonic, operands, size) of the instruction at a."""
    o = a - BASE
    for i in md().disasm(data()[o:o + 2], a):
        return i.mnemonic, i.op_str
    return '.word', '0x%04x' % u16(a)


def pool_target(a):
    """Address a PC-relative load at a reads (mov.l / mov.w @(disp,pc), mova), with its size, or (None, 0)."""
    op = u16(a)
    if op >> 12 == 0xD:
        return (a & ~3) + 4 + (op & 0xFF) * 4, 4
    if op >> 12 == 0x9:
        return a + 4 + (op & 0xFF) * 2, 2
    if op >> 8 == 0xC7:
        return (a & ~3) + 4 + (op & 0xFF) * 4, 0
    return None, 0


def load_value(a):
    t, size = pool_target(a)
    if t is None:
        return None
    return u32(t) if size == 4 else u16(t) if size == 2 else t


def line(a):
    mn, ops = insn(a)
    s = '%08X: %04x  %-8s %s' % (a, u16(a), mn, ops)
    v = load_value(a)
    if v is not None:
        s += '    ; =0x%X' % v
        if u16(a) >> 12 == 0x9 and v & 0x8000:
            s += ' (%d)' % (v - 0x10000)
        st = cstring(v) if u16(a) >> 12 != 0x9 else None
        if st:
            s += ' "%s"' % st
    return s


def branch_target(a, mn, ops):
    if mn in ('bra', 'bsr', 'bt', 'bf', 'bt/s', 'bf/s'):
        m = re.search(r'0x[0-9a-f]+', ops)
        if m:
            return int(m.group(0), 16)
    return None


def func_range(a):
    """[a, end) of the function at a: ends at the rts (plus delay slot) past every forward branch."""
    far = a
    p = a
    while p - BASE < len(data()) - 2:
        mn, ops = insn(p)
        t = branch_target(p, mn, ops)
        if t is not None and mn != 'bsr' and t > far:
            far = t
        if mn == 'rts' and p >= far:
            return a, p + 4
        p += 2
    return a, p


def calls(target):
    d = data()
    out = []
    for p in range(0, len(d) - 2, 2):
        a = BASE + p
        mn, ops = insn(a)
        if mn == 'bsr' and branch_target(a, mn, ops) == target:
            out.append(a)
        elif u16(a) >> 12 == 0xD and load_value(a) == target:
            out.append(a)
    return out


def main():
    cmd = sys.argv[1]
    if cmd == 'dis':
        a = int(sys.argv[2], 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 64
        for i in range(n):
            print(line(a + 2 * i))
    elif cmd == 'func':
        a, e = func_range(int(sys.argv[2], 16))
        for p in range(a, e, 2):
            print(line(p))
    elif cmd == 'refs':
        v = int(sys.argv[2], 16)
        d = data()
        pools = [BASE + p for p in range(0, len(d) - 4, 4) if struct.unpack_from('<I', d, p)[0] == v]
        print('pool words:', ' '.join('%08X' % p for p in pools))
        for a in calls(v):
            print('  load at %08X' % a)
    elif cmd == 'calls':
        for a in calls(int(sys.argv[2], 16)):
            print('%08X' % a)
    elif cmd == 'str':
        d = data()
        p = d.find(sys.argv[2].encode() + b'\0')
        while p >= 0:
            a = BASE + p
            print('string at %08X; loads:' % a, ' '.join('%08X' % c for c in calls(a)))
            p = d.find(sys.argv[2].encode() + b'\0', p + 1)


if __name__ == '__main__':
    main()
