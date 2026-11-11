"""PS1 VRAM placement of a track's textures, read from the game's own loaders (docs/sfrush_ps1.md#textures).

Each track has a page loader (T<n>PAGE: LoadImage func_80092C58 of chunk k at a hard-coded RECT) and a CLUT loader
(T<n>CLUT: func_80014F38(x, y, chunk) puts the chunk's 16-color CLUT at (x, y) and 31 fog-shaded copies under it;
func_80015090 does the same with copies that always darken, for track 2's tunnel).
The loaders are straight-line code, so a linear scan that tracks constant registers and the RECT on the stack gives
the placements in chunk order.

  python vram.py rects                  every track's page RECTs and CLUT positions
  python vram.py png TRACK OUT.png      track TRACK's (1-4) VRAM as an image (pages drawn with their first CLUT)
"""
import os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from disps1 import Exe
from ps1 import Disc

LOAD_IMAGE = 0x80092C58
LOAD_CLUT = 0x80014F38       # clut_upload_shaded: fog-shaded copies (lighter by day, darker at night)
LOAD_CLUT_DARK = 0x80015090  # clut_upload_dark: copies always darker, color 0 not kept (track 2's tunnel)
PAGE_LOADERS = {1: 0x80037C2C, 2: 0x80038644, 3: 0x80039048, 4: 0x80039A88}
CLUT_LOADERS = {1: 0x8003A4A0, 2: 0x8003AF18, 3: 0x8003B990, 4: 0x8003C408}
TEX_TABLE_A = 0x800A7478     # track index 1 (track 2) when 0x800B34F8 == 1 (func_800369B8)
TEX_TABLE_B = 0x800A77C0     # every other track
TEX_ENTRIES = 35
CLUT_IDS = 0x800A7B08        # face byte 3 -> CLUT id ((y << 6) | (x >> 4))


def scan(e, start, end):
    """[(kind, args, chunk)] for LoadImage (x, y, w, h) and CLUT (x, y) calls between start and end. chunk is the
    .DAT chunk the call gets: the loaders step their chunk pointer by the .SIZ sizes (lw $v0, 4k($s1) adds chunk
    k's; the last into $a2), so after the size of chunk k the next call gets chunk k + 1. Track 2's CLUT loader uploads
    chunks 10 and 26 (the tunnel's CLUTs) with LOAD_CLUT_DARK, so both functions count."""
    regs = {0: 0}; stack = {}; out = []; chunk = 0
    a = start
    while a < end:
        w = e.w(a); op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        nxt = None
        if op == 9 and rs in regs: regs[rt] = (regs[rs] + simm) & 0xFFFFFFFF
        elif op == 0xD and rs in regs: regs[rt] = regs[rs] | imm
        elif op == 0xF: regs[rt] = imm << 16
        elif op == 0x23 and rs == 17:
            chunk = simm // 4 + 1
            regs.pop(rt, None)
        elif op == 0x29 and rs == 29 and rt in regs: stack[simm] = regs[rt] & 0xFFFF
        elif op == 3:
            t = (a & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
            # the delay slot runs first
            d = e.w(a + 4); dop = d >> 26; drs = (d >> 21) & 31; drt = (d >> 16) & 31; dimm = d & 0xFFFF
            dsimm = dimm - 0x10000 if dimm & 0x8000 else dimm
            if dop == 0x29 and drs == 29 and drt in regs: stack[dsimm] = regs[drt] & 0xFFFF
            if dop == 9 and drs in regs: regs[drt] = (regs[drs] + dsimm) & 0xFFFFFFFF
            if t == LOAD_IMAGE:
                out.append(('image', tuple(stack.get(o, -1) for o in (0x10, 0x12, 0x14, 0x16)), chunk))
            elif t in (LOAD_CLUT, LOAD_CLUT_DARK):
                out.append(('clut', (regs.get(4, -1), regs.get(5, -1)), chunk))
            for r in (2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 24, 25):
                regs.pop(r, None)
            a += 8; continue
        elif op == 0 and (w & 0x3F) in (0x21, 0x25) and ((w >> 16) & 31) == 0 and rs in regs:
            regs[(w >> 11) & 31] = regs[rs]
        elif op == 0:
            regs.pop((w >> 11) & 31, None)
        elif op not in (1, 2, 4, 5, 6, 7, 0x28, 0x29, 0x2B):
            regs.pop(rt, None)
        regs[0] = 0
        a += 4
    return out


def placements(e, track, chunks=False):
    """The page RECTs and CLUT positions in call order; with chunks, also each call's .DAT chunk index."""
    p = PAGE_LOADERS[track]; c = CLUT_LOADERS[track]
    pages = [(x, n) for k, x, n in scan(e, p, e.func_end(p)) if k == 'image']
    cluts = [(x, n) for k, x, n in scan(e, c, e.func_end(c)) if k == 'clut']
    if chunks:
        return pages, cluts
    return [x for x, n in pages], [x for x, n in cluts]


def tex_table(e, track):
    base = TEX_TABLE_A if track == 2 else TEX_TABLE_B
    return [struct.unpack_from('<6i', e.data, base + 24 * i - 0x80010000) for i in range(TEX_ENTRIES)]


def clut_ids(e, n=64):
    return list(struct.unpack_from('<%dI' % n, e.data, CLUT_IDS - 0x80010000))


class Vram:
    """1024x512 halfwords, with a track's pages and CLUTs uploaded as the game does."""
    def __init__(self, disc, e, track, variant=''):
        self.v = [0] * (1024 * 512)
        pages, cluts = placements(e, track, chunks=True)
        pc = disc.chunks('T%dPAGE/T%dPAGE%s.DAT' % (track, track, variant))
        cc = disc.chunks('T%dCLUT/T%dCLUT%s.DAT' % (track, track, variant))
        self.pages, self.cluts = [x for x, n in pages], [x for x, n in cluts]
        # The page loader reads the .DAT as one buffer and uploads w x h from each chunk's start whatever its size
        # (track 4's night chunk 28 is 4 rows short and runs into chunk 29).
        data = b''.join(pc); at = [sum(len(c) for c in pc[:k]) for k in range(len(pc))]
        for (x, y, w, h), n in pages:
            px = struct.unpack_from('<%dH' % (w * h), data, at[n] + 0x28)
            for r in range(h):
                self.v[(y + r) * 1024 + x:(y + r) * 1024 + x + w] = px[r * w:(r + 1) * w]
        for (x, y), n in cluts:
            self.v[y * 1024 + x:y * 1024 + x + 16] = struct.unpack_from('<16H', cc[n], 8)

    def texel4(self, tx, ty, clut, u, v):
        """RGBA of the 4-bit texel (u, v) of the texture page at (tx, ty) with CLUT id clut."""
        h = self.v[(ty + (v & 255)) * 1024 + tx + ((u & 255) >> 2)]
        i = (h >> ((u & 3) * 4)) & 15
        cx, cy = (clut & 63) * 16, clut >> 6
        c = self.v[cy * 1024 + cx + i]
        return rgba5551(c)


def rgba5551(c):
    """PS1 15-bit color to RGBA; 0x0000 is transparent, any other value (even black 0x8000) is drawn."""
    r, g, b = (c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3
    return (r | r >> 5, g | g >> 5, b | b >> 5, 0 if c == 0 else 255)


if __name__ == '__main__':
    e = Exe()
    if sys.argv[1] == 'rects':
        for t in (1, 2, 3, 4):
            pages, cluts = placements(e, t)
            print('track', t, 'pages', len(pages), pages)
            print('   cluts', len(cluts), cluts)
    elif sys.argv[1] == 'png':
        from PIL import Image
        t = int(sys.argv[2]); vr = Vram(Disc(), e, t)
        img = Image.new('RGBA', (2048, 512))
        px = img.load()
        for (x, y, w, h), (cx, cy) in zip(vr.pages, vr.cluts + [(0, 0)] * 64):
            clut = (cy << 6) | (cx >> 4)
            for yy in range(h):
                for u in range(w * 4):
                    px[x * 2 + u, y + yy] = vr.texel4(x, y, clut, u, yy)
        img.save(sys.argv[3])
