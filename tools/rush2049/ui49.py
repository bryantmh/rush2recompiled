"""Dumps Rush 2049 UI textures (default: the track-select file 60 and the menu-title file 59) to PNG, and renders
the flat TRK_* track-select meshes of file 60 as top-down silhouettes.

Usage:  python ui49.py OUTDIR [--raw] [FILE ...]   e.g.  python ui49.py out            (files 60 and 59)
                                                          python ui49.py out 54 57 58   (other UI files)

The track-select thumbnails (file 60 TPICn race, DPICn battle, SPICn stunt, OPIC1 obstacle) are stored upside
down; they are flipped vertically on output unless --raw is given. All are drawn by the same menu-overlay call
(func_800B3704(name, 0xB0, 0x20, 0) at 0x8038AF24), so they share one orientation convention.

2049 texture records (TXHD, 0x24 bytes): name[16], u16 w, u16 h, u32 idx, u32 texels (IMAG-relative), u32 fmt,
u32 extra. Observed encoding (verified by texel/palette spacing and the decoded images):
    (idx >> 16) & 0xFF == 2       colour-indexed, palette in PLHD (same name), TLUT = RGBA5551
    PLHD record: name[16], u32 a, u32 pal (IMAG-relative); (a >> 16) & 0xFF = entry count - 1
    16-entry palette = CI4, 256-entry = CI8 (fmt & 0x40000000 usually agrees, but not for TRACKFAN)
Other types (fonts, sheen, effects) are written as intensity images (I4/I8 guess) for inspection only.
"""
import os, struct, sys, zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roms, model

try:
    from PIL import Image
except ImportError:
    Image = None


def write_png(path, w, h, rgba):
    if Image is not None:
        Image.frombytes('RGBA', (w, h), bytes(rgba)).save(path)
        return
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))

    def chunk(t, b):
        return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xFFFFFFFF)
    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                + chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b''))


def rgba5551(v):
    r, g, b = (v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31
    return (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2, 255 if v & 1 else 0)


def texel_indices(d, o, w, h, bpp):
    if bpp == 8:
        return list(d[o:o + w * h])
    out = []
    for b in d[o:o + (w * h + 1) // 2]:
        out += (b >> 4, b & 15)
    return out[:w * h]


def decode(m, t):
    """Returns (kind, rgba bytes) for one TXHD record."""
    d, base = m.d, m.imag
    w, h = t['w'], t['h']
    bpp = 8 if t['fmt'] & 0x40000000 else 4
    kind = (t['idx'] >> 16) & 0xFF
    pal = next((p for p in m.palettes if p['name'] == t['name'] and p['pal']), None)
    rgba = bytearray()
    if kind == 2 and pal is not None:
        n = ((pal['a'] >> 16) & 0xFF) + 1
        bpp = 4 if n <= 16 else 8      # TRACKFAN has fmt bit 0x40000000 set but a 16-entry palette and CI4 size
        idx = texel_indices(d, base + t['texels'], w, h, bpp)
        lut = [rgba5551(struct.unpack_from('>H', d, base + pal['pal'] + 2 * i)[0]) for i in range(n)]
        for i in idx:
            rgba += bytes(lut[i] if i < n else (255, 0, 255, 255))
        return 'CI%d' % bpp, rgba
    idx = texel_indices(d, base + t['texels'], w, h, bpp)
    s = 255 // ((1 << bpp) - 1)
    for i in idx:
        v = i * s
        rgba += bytes((v, v, v, 255))
    return 'I%d?' % bpp, rgba


def mesh_triangles(d, ob):
    """Top-level F3DEX2 walk of one LOD: G_VTX (01) loads and G_TRI1/G_TRI2 (05/06). Returns [(x, z) x 3]."""
    vbuf = [None] * 64
    o = ob['lods'][0][3]
    tris = []
    while d[o] != 0xDF:
        w0, w1 = struct.unpack_from('>II', d, o)
        op = w0 >> 24
        if op == 0x01:
            n = (w0 >> 12) & 0xFF
            v0 = ((w0 >> 1) & 0x7F) - n
            a = w1 & 0xFFFFFF
            for i in range(n):
                x, y, z = struct.unpack_from('>hhh', d, a + 16 * i)
                vbuf[v0 + i] = (x, z)
        elif op in (0x05, 0x06):
            tris.append(tuple(vbuf[((w0 >> s) & 0xFF) // 2] for s in (16, 8, 0)))
            if op == 0x06:
                tris.append(tuple(vbuf[((w1 >> s) & 0xFF) // 2] for s in (16, 8, 0)))
        o += 8
    return tris


def render_mesh(tris, size=160):
    xs = [p[0] for t in tris for p in t]; zs = [p[1] for t in tris for p in t]
    x0, x1, z0, z1 = min(xs), max(xs), min(zs), max(zs)
    sc = (size - 8) / max(x1 - x0, z1 - z0, 1)
    img = bytearray(size * size * 4)
    for i in range(size * size):
        img[i * 4:i * 4 + 4] = b'\x10\x10\x18\xff'
    for t in tris:
        pts = [((p[0] - x0) * sc + 4, (p[1] - z0) * sc + 4) for p in t]
        ymin, ymax = int(min(p[1] for p in pts)), int(max(p[1] for p in pts)) + 1
        xmin, xmax = int(min(p[0] for p in pts)), int(max(p[0] for p in pts)) + 1
        (ax, ay), (bx, by), (cx, cy) = pts
        area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
        if area == 0:
            continue
        for y in range(max(ymin, 0), min(ymax, size)):
            for x in range(max(xmin, 0), min(xmax, size)):
                px, py = x + .5, y + .5
                w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area
                w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area
                if w0 >= 0 and w1 >= 0 and w0 + w1 <= 1:
                    img[(y * size + x) * 4:(y * size + x) * 4 + 4] = b'\xe0\xe0\x40\xff'
    return size, img


def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    out = sys.argv[1]
    raw = '--raw' in sys.argv[2:]
    files = [int(a, 0) for a in sys.argv[2:] if a != '--raw'] or [60, 59]
    os.makedirs(out, exist_ok=True)
    q = roms.Rush2049()
    for k in files:
        m = model.M49(q.file(k))
        for t in m.textures:
            kind, rgba = decode(m, t)
            if k == 60 and not raw and t['name'][1:4] == 'PIC':
                row = t['w'] * 4
                rgba = b''.join(rgba[y * row:(y + 1) * row] for y in reversed(range(t['h'])))
            fn = '%d_%s.png' % (k, t['name'])
            write_png(os.path.join(out, fn), t['w'], t['h'], rgba)
            print('%-28s %3dx%-3d %-4s idx=%08x fmt=%08x' % (fn, t['w'], t['h'], kind, t['idx'], t['fmt']))
        if k == 60:
            for ob in m.objects:
                tris = mesh_triangles(m.d, ob)
                size, img = render_mesh(tris)
                fn = '%d_mesh_%s.png' % (k, ob['name'])
                write_png(os.path.join(out, fn), size, size, img)
                print('%-28s %d triangles' % (fn, len(tris)))


if __name__ == '__main__':
    main()
