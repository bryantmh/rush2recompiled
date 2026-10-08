"""Decompile Rush 2 / Rush 2049 functions with m2c (research aid).

Usage: python decomp.py r2|49|ovl FUNCADDR [more...]
  ovl = the in-race mode overlay (stunt/battle HUD, weapons; ROM 0xB6FEC4 raw deflate, loaded at 0x8038A400), read from
  the ROM (tools/rush2049/roms.py); function bounds come from scanning it for jr $ra.
Needs m2c (git clone https://github.com/matt-kempster/m2c) at $M2C or ./m2c. Function bounds come from
analysis/out_disasm/{r2,d49}.asm labels. lui/addiu pairs are emitted as %hi/%lo of D_XXXXXXXX symbols.
"""
import os, re, struct, subprocess, sys, tempfile
import rabbitizer
import roms

REPO = roms.REPO
M2C = os.environ.get('M2C', os.path.join(os.path.dirname(__file__), 'm2c', 'm2c.py'))
_bounds = {}

OVL_BASE = 0x8038A400
OVL_ROM = 0xB6FEC4
_ovl = []

def ovl_bytes():
    if not _ovl:
        import zlib
        _ovl.append(zlib.decompressobj(-15).decompress(roms.Rush2049().rom[OVL_ROM:OVL_ROM + 0x80000]))
    return _ovl[0]

def ovl_bounds():
    """Function starts of the overlay: the instruction after each jr $ra delay slot that no branch jumps past."""
    d = ovl_bytes(); starts = [OVL_BASE]; maxbr = OVL_BASE; a = OVL_BASE
    while a < OVL_BASE + len(d):
        w = struct.unpack('>I', d[a - OVL_BASE:a - OVL_BASE + 4])[0]
        ins = rabbitizer.Instruction(w, vram=a)
        if ins.isBranch(): maxbr = max(maxbr, ins.getBranchVramGeneric())
        if w == 0x03E00008 and a >= maxbr:
            starts.append(a + 8); maxbr = a + 8
            a += 8; continue
        a += 4
    return starts

def bounds(game):
    if game == 'ovl':
        if game not in _bounds: _bounds[game] = ovl_bounds()
        return _bounds[game]
    if game in _bounds: return _bounds[game]
    fn = os.path.join(REPO, 'analysis', 'out_disasm', 'r2.asm' if game == 'r2' else 'd49.asm')
    starts = []; prev = 0; second = False
    for ln in open(fn):
        m = re.match(r'^([0-9A-F]{8}) ', ln)
        if m:
            a = int(m.group(1), 16)
            if a < prev - 0x10000: second = True   # d49.asm: boot dump overlaps main; trust main only
            prev = a; continue
        m = re.match(r'^func_([0-9A-F]{8}):', ln)
        if m:
            a = int(m.group(1), 16)
            if game != 'r2' and not second and a >= 0x80086A50: continue
            starts.append(a)
    starts = sorted(set(starts)); _bounds[game] = starts; return starts

def reader(game):
    if game == 'ovl':
        d = ovl_bytes()
        return lambda a: struct.unpack('>I', d[a - OVL_BASE:a - OVL_BASE + 4])[0] if OVL_BASE <= a < OVL_BASE + len(d) else 0
    if game == 'r2':
        r = roms.Rush2(); return lambda a: struct.unpack('>I', r.read(a, 4))[0]
    r = roms.Rush2049()
    def rd(a):
        if a >= r.MAIN_VRAM: return r.w(a)
        return struct.unpack('>I', r.rom[0x1000 + a - 0x80000400:0x1000 + a - 0x80000400 + 4])[0]
    return rd

def func_end(game, start, rd):
    st = bounds(game); i = st.index(start) if start in st else None
    nxt = st[i + 1] if i is not None and i + 1 < len(st) else start + 0x4000
    # stop at last jr ra + delay slot before nxt, scanning forward for branches beyond
    a = start; maxbr = start; end = nxt
    while a < nxt:
        w = rd(a)
        ins = rabbitizer.Instruction(w, vram=a)
        if ins.isBranch():
            maxbr = max(maxbr, ins.getBranchVramGeneric())
        if w == 0x03E00008 and a >= maxbr:
            return a + 8
        a += 4
    return end

def emit(game, start):
    rd = reader(game); end = func_end(game, start, rd)
    ins_list = [(a, rd(a)) for a in range(start, end, 4)]
    labels = set()
    for a, w in ins_list:
        ins = rabbitizer.Instruction(w, vram=a)
        if ins.isBranch() or (ins.isJump() and not ins.isJumpWithAddress() is False and w >> 26 == 2):
            try: labels.add(ins.getBranchVramGeneric())
            except Exception: pass
    # resolve lui pairs
    hi = {}; lui = {}
    lines = ['glabel func_%08X' % start]
    for a, w in ins_list:
        ins = rabbitizer.Instruction(w, vram=a)
        op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        txt = ins.disassemble()
        if a in labels: lines.append('.L%08X:' % a)
        if op == 0x0F:
            # look ahead for a pairing use
            lui[rt] = (imm << 16, len(lines))
        elif rs in lui and op in (0x09, 0x20, 0x21, 0x23, 0x24, 0x25, 0x27, 0x28, 0x29, 0x2B, 0x31, 0x35, 0x39, 0x3D):
            base, li = lui[rs]
            addr = (base + simm) & 0xFFFFFFFF
            if 0x80000000 <= addr < 0x80800000:
                sym = 'D_%08X' % addr
                lines[li] = re.sub(r'0x[0-9A-Fa-f]+$', '%%hi(%s)' % sym, lines[li])
                txt = re.sub(r'(-?0x[0-9A-Fa-f]+|\b-?\d+)(\(\$\w+\))?$', lambda m: '%%lo(%s)%s' % (sym, m.group(2) or ''), txt)
        if op not in (0x0F, 0x28, 0x29, 0x2B, 0x39, 0x3D, 0x04, 0x05, 0x14, 0x15, 0x01, 0x31, 0x35, 0x11) and op != 0:
            if rt in lui and not (rs == rt and op == 0x09 and False):
                lui.pop(rt, None)
        if op == 0:
            lui.pop((w >> 11) & 31, None)
        if ins.isBranch():
            t = ins.getBranchVramGeneric()
            txt = re.sub(r'\. \+ 4 \+ \(.*\)$|0x[0-9A-Fa-f]+$|L_[0-9A-F]+$', '.L%08X' % t, txt)
        if w >> 26 == 2:
            t = ins.getBranchVramGeneric(); txt = 'j .L%08X' % t
        if w >> 26 == 3:
            t = ((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2); txt = 'jal func_%08X' % t
        lines.append('/* %08X */ ' % a + txt)
    return '\n'.join(lines) + '\n'

if __name__ == '__main__':
    game = sys.argv[1]
    for f in sys.argv[2:]:
        a = int(f.replace('func_', ''), 16)
        asm = emit(game, a)
        with tempfile.NamedTemporaryFile('w', suffix='.s', delete=False) as t:
            t.write('.set noat\n.set noreorder\n' + asm.replace('/* ', '/* ').replace('*/ ', '*/ '))
            path = t.name
        res = subprocess.run([sys.executable, M2C, '--valid-syntax', path], capture_output=True, text=True)
        print(res.stdout or res.stderr)
        if res.returncode: print(asm)
