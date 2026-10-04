"""Cross-reference finder for Rush 2 and Rush 2049 code (research aid for cars.md).

Resolves lui/addiu/addu chains (lui r,hi; addu r,r,idx; lw x,lo(r)) to absolute addresses, which the plain
annotated listings miss for indexed table reads.
Usage: python xref.py r2|49 LO HI         list every reference into [LO, HI)
       python xref.py r2|49 -i IMM        list instructions with this immediate (slti/addiu/ori), e.g. 0x16
"""
import struct, sys
import rabbitizer
import roms, decomp

_cache = {}

def segments(game):
    if game in _cache: return _cache[game]
    if game == 'r2':
        r = roms.Rush2()
        segs = [(0x80000400, r.rom[0x1000:0x1000 + (0x800539E0 - 0x80000400)]),
                (0x800539E0, r.rom[0x01000000:0x01000000 + (0x800BCBB0 - 0x800539E0)]),
                (0x803AA800, r.rom[0x01080000:0x01080000 + 0x1C190])]
    else:
        r = roms.Rush2049()
        segs = [(r.MAIN_VRAM, r.main)]
    _cache[game] = segs
    return segs

LOADSTORE = {0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2B, 0x31, 0x35, 0x39, 0x3D}

def scan(game, text_hi=None):
    """Yield (addr, word, resolved address or None, func start)."""
    bounds = set(decomp.bounds(game))
    for base, data in segments(game):
        hi = {}; func = base
        n = len(data) // 4
        for i in range(n):
            a = base + i * 4
            w = struct.unpack('>I', data[i * 4:i * 4 + 4])[0]
            if a in bounds: hi = {}; func = a
            op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; rd = (w >> 11) & 31
            imm = w & 0xFFFF; simm = imm - 0x10000 if imm & 0x8000 else imm
            ref = None
            if op == 0x0F:
                hi[rt] = imm << 16; yield a, w, None, func; continue
            if op in LOADSTORE and rs in hi:
                ref = (hi[rs] + simm) & 0xFFFFFFFF
                if op not in (0x28, 0x29, 0x2B, 0x39, 0x3D, 0x31, 0x35): hi.pop(rt, None)
            elif op in (0x09, 0x0D) and rs in hi:
                ref = (hi[rs] + (imm if op == 0x0D else simm)) & 0xFFFFFFFF
                hi[rt] = ref if op == 0x09 and False else hi.get(rt) if False else None
                hi.pop(rt, None)
            elif op == 0 and (w & 0x3F) == 0x21 and (rs in hi) != (rt in hi):
                src = rs if rs in hi else rt
                h = hi[src]; hi.pop(rd, None); hi[rd] = h
            else:
                if op == 0:
                    if rd: hi.pop(rd, None)
                elif op not in (0x28, 0x29, 0x2B, 0x39, 0x3D, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17, 0x01, 0x02, 0x03, 0x11):
                    hi.pop(rt, None)
            if w == 0x03E00008: hi = {}
            yield a, w, ref, func

def refs(game, lo, hi_):
    out = []
    for a, w, ref, func in scan(game):
        if ref is not None and lo <= ref < hi_:
            out.append((ref, a, func, rabbitizer.Instruction(w, vram=a).disassemble()))
    return sorted(out)

def imms(game, val):
    out = []
    for a, w, ref, func in scan(game):
        op = w >> 26
        if op in (0x08, 0x09, 0x0A, 0x0B, 0x0D) and (w & 0xFFFF) == val:
            out.append((a, func, rabbitizer.Instruction(w, vram=a).disassemble()))
    return out

if __name__ == '__main__':
    g = sys.argv[1]
    if sys.argv[2] == '-i':
        for a, f, t in imms(g, int(sys.argv[3], 0)): print('%08X func_%08X  %s' % (a, f, t))
    else:
        lo = int(sys.argv[2], 16); hi_ = int(sys.argv[3], 16)
        for ref, a, f, t in refs(g, lo, hi_): print('%08X  %08X func_%08X  %s' % (ref, a, f, t))
