"""Reader for Dreamcast Rush 2049 model containers (decompressed .LZS: tracks, cars, objects, HUD art).

    dc_model.py info FILE                 chunks, objects, textures
    dc_model.py obj FILE NAME             decode one object's strips (vertices, texture commands)
    dc_model.py tex FILE OUTDIR [NAME]    decode textures to PNG (needs Pillow)

Container (little endian; the N64 one is big endian, docs/rush2049_research/geometry.md section 3): u32 size, u32
chunk count, then a directory of {char tag[4] (reversed: DHBO = OBHD), u32 offset, u32 size or count} at the end.
Objects (DHBO) are 0x48 bytes: name[16], f32 radius, u16 kind, s16 LOD count, 4 x {u16 texture handle, u16 flags,
f32 distance, u32 stream offset}. The N64 LOD entry has a vertex pointer too (0x58 records).

An object's stream is a list of 32-bit commands (decoded by the DC loader's 0x8C026104 and drawn by e.g.
0x8C077F00), the top 3 bits the kind:
  0xE0000000  end
  0xA0000000  texture: low 16 bits = texture index (s16, -1 none); one more word (the loader stores a pointer there)
  0x80000000  conditional: bit 0 = drawn only when not mirrored, bit 1 = only when mirrored; one more word, the
              byte length of the block the condition covers (the loader zeroes it when the block is drawn)
  0x00000000 / 0x20000000  vertex; 0x20000000 ends the strip. Bits 16-23 / 8-15 / 0-7: position / uv / colour cache
              slot. A clear 0x10000000 bit means 3 floats of position follow (stored in the slot), else the slot's;
              clear 0x08000000: 2 floats of uv; clear 0x04000000: a colour word (ARGB), or 3 floats of normal when
              the object is lit (LOD flag 0x10).
See docs/rush2049_research/dreamcast.md.
"""
import struct, sys

END, TEXTURE, COND, STRIP_END = 0xE0000000, 0xA0000000, 0x80000000, 0x20000000


def chunks(d):
    n = struct.unpack_from('<I', d, 4)[0]
    t = len(d) - n * 12
    out = {}
    for i in range(n):
        tag, off, cnt = struct.unpack_from('<4sII', d, t + i * 12)
        out[tag[::-1].decode()] = (off, cnt)
    return out


# Texture records (DHXT, 0x30 bytes): name[16], u16 w, u16 h, u8 2, u8 pixel format, s16 -1, u32 texel offset (from
# IMAG), u32 surface flags (0x04000000 = mipmapped), u32 layout (0x2000 VQ, 0x4000 twiddled, neither = linear rows),
# then the runtime surface pointer. Pixel formats below; mipmapped texel data starts with the smallest level.
PIXEL_FORMATS = {0: 'ARGB4444', 1: 'RGB565', 2: 'ARGB1555'}
# Offset of the largest mip level: VQ in index bytes after the 2 KB codebook, others in texels (Flycast's tables).
VQ_MIP = [0x0, 0x1, 0x2, 0x6, 0x16, 0x56, 0x156, 0x556, 0x1556, 0x5556, 0x15556]
MIP16 = [0x3, 0x4, 0x8, 0x18, 0x58, 0x158, 0x558, 0x1558, 0x5558, 0x15558, 0x55558]


def untwiddle(x, y):
    """Texel index of (x, y) in a twiddled square: bits interleave y (even) and x (odd)."""
    o, b = 0, 0
    while (1 << b) <= max(x, y):
        o |= ((y >> b) & 1) << (2 * b) | ((x >> b) & 1) << (2 * b + 1)
        b += 1
    return o


def to_rgba(v, fmt):
    if fmt == 2:
        return ((v >> 10) & 31) * 255 // 31, ((v >> 5) & 31) * 255 // 31, (v & 31) * 255 // 31, 255 if v & 0x8000 else 0
    if fmt == 1:
        return ((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, 255
    return ((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 15) * 17  # 0: ARGB4444


def texture_rgba(d, imag, t):
    """Largest level of a texture record as [h][w] (r, g, b, a)."""
    w, h, fmt = t['w'], t['h'], t['siz']
    at, flags, layout = imag + t['words'][0], t['words'][1], t['words'][2]
    mip = bool(flags & 0x04000000)
    lw = w.bit_length() - 1
    s = min(w, h)
    out = [[None] * w for _ in range(h)]
    if layout & 0x2000:
        book = [struct.unpack_from('<4H', d, at + i * 8) for i in range(256)]
        idx = at + 2048 + (VQ_MIP[lw] if mip else 0)
        hs = s // 2
        for y in range(0, h, 2):
            for x in range(0, w, 2):
                bx, by = x // 2, y // 2
                blk = (bx // hs + by // hs) * hs * hs
                c = book[d[idx + blk + untwiddle(bx % hs, by % hs)]]
                for k, (dx, dy) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
                    out[y + dy][x + dx] = to_rgba(c[k], fmt)
    elif layout & 0x4000:
        base = at + (MIP16[lw] * 2 if mip else 0)
        for y in range(h):
            for x in range(w):
                blk = (x // s + y // s) * s * s
                out[y][x] = to_rgba(struct.unpack_from('<H', d, base + 2 * (blk + untwiddle(x % s, y % s)))[0], fmt)
    else:
        for y in range(h):
            for x in range(w):
                out[y][x] = to_rgba(struct.unpack_from('<H', d, at + 2 * (y * w + x))[0], fmt)
    return out


def cstr(b):
    return b.split(b'\0')[0].decode('latin1')


class DCModel:
    OBHD = 0x48
    TXHD = 0x30

    def __init__(self, d):
        self.d = d
        self.c = chunks(d)
        o, n = self.c.get('OBHD', (0, 0))
        self.objects = []
        for i in range(n):
            r = o + i * self.OBHD
            radius, kind, nlod = struct.unpack_from('<fHh', d, r + 16)
            lods = [struct.unpack_from('<HHfI', d, r + 0x18 + j * 12) for j in range(4)]
            self.objects.append(dict(name=cstr(d[r:r + 16]), radius=radius, kind=kind, n=nlod, lods=lods[:nlod], rec=r))
        o, n = self.c.get('TXHD', (0, 0))
        self.textures = []
        for i in range(n):
            r = o + i * self.TXHD
            w, h, fmt, siz, pal = struct.unpack_from('<HHBBh', d, r + 16)
            words = struct.unpack_from('<6I', d, r + 24)
            self.textures.append(dict(name=cstr(d[r:r + 16]), w=w, h=h, fmt=fmt, siz=siz, pal=pal, words=words))

    def obj(self, name):
        for o in self.objects:
            if o['name'] == name:
                return o
        return None

    def decode(self, at, lit=False):
        """Strips of the stream at file offset at: [(texture, cond, [vertex (pos, uv, colour or normal)])]."""
        d = self.d
        pos, uv, col = {}, {}, {}
        strips = []
        cur = []
        tex, cond = None, None
        o = at
        while True:
            w = struct.unpack_from('<I', d, o)[0]
            o += 4
            kind = w & 0xE0000000
            if kind == END:
                break
            if kind == TEXTURE:
                t = w & 0xFFFF
                tex = None if t == 0xFFFF else t
                o += 4
                continue
            if kind == COND:
                cond = (w & 3, struct.unpack_from('<I', d, o)[0])
                o += 4
                continue
            if kind not in (0, STRIP_END):
                raise ValueError('unknown command %08X at %X' % (w, o - 4))
            pi, ui, ci = (w >> 16) & 0xFF, (w >> 8) & 0xFF, w & 0xFF
            if not w & 0x10000000:
                pos[pi] = struct.unpack_from('<3f', d, o); o += 12
            if not w & 0x08000000:
                uv[ui] = struct.unpack_from('<2f', d, o); o += 8
            if not w & 0x04000000:
                if lit:
                    col[ci] = struct.unpack_from('<3f', d, o); o += 12
                else:
                    col[ci] = struct.unpack_from('<I', d, o)[0]; o += 4
            cur.append((pos.get(pi), uv.get(ui), col.get(ci)))
            if kind == STRIP_END:
                strips.append((tex, cond, cur))
                cur = []
        if cur:
            strips.append((tex, cond, cur))
        return strips, o

    @staticmethod
    def triangles(strips):
        out = []
        for tex, cond, vs in strips:
            for i in range(2, len(vs)):
                a, b, c = (vs[i - 2], vs[i - 1], vs[i]) if i % 2 == 0 else (vs[i - 1], vs[i - 2], vs[i])
                out.append((tex, a, b, c))
        return out


def main():
    d = open(sys.argv[2], 'rb').read()
    m = DCModel(d)
    if sys.argv[1] == 'info':
        print(m.c)
        for o in m.objects:
            print('%-16s r=%.1f kind=%d lods=%s' % (o['name'], o['radius'], o['kind'],
                                                   ['%x:%x:%.0f@%x' % l for l in o['lods']]))
        for i, t in enumerate(m.textures):
            print(i, t)
    elif sys.argv[1] == 'obj':
        o = m.obj(sys.argv[3])
        for l in o['lods']:
            strips, end = m.decode(l[3], bool(l[1] & 0x10))
            print('LOD %s: %d strips, %d triangles, %d bytes' % (l, len(strips), len(m.triangles(strips)), end - l[3]))
            for s in strips[:int(sys.argv[4]) if len(sys.argv) > 4 else 8]:
                print('  tex=%s cond=%s' % (s[0], s[1]))
                for v in s[2]:
                    print('    ', v)
    elif sys.argv[1] == 'tex':
        from PIL import Image
        import os
        os.makedirs(sys.argv[3], exist_ok=True)
        imag = m.c['IMAG'][0]
        for t in m.textures:
            if len(sys.argv) > 4 and t['name'] != sys.argv[4]:
                continue
            px = texture_rgba(d, imag, t)
            im = Image.new('RGBA', (t['w'], t['h']))
            im.putdata([p for row in px for p in row])
            im.save(os.path.join(sys.argv[3], '%s_%s.png' % (t['name'], PIXEL_FORMATS.get(t['siz'], t['siz']))))


if __name__ == '__main__':
    main()
