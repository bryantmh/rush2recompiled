"""List the textures/palettes/models of Rush 1 car files (table index 25-35) next to the same cars in Rush 2.

usage: python cartex.py [car name, e.g. BMW]   (no argument = all eleven)
Both games use the same container: u32 names ptr, model count, 0, 0, textures ptr, count, palettes ptr, count; name
records 0x18, texture records 0x20 (name[16], w, h, fmt, siz, pal idx, data, flags), palette records 0x18.
"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
from r1 import Rush1
from roms import Rush2

R1_CARS = ['BMW', 'CAMARO', 'SUPRA', 'BUGATTI', 'VWBUS', 'VIPER', 'VWBUG', 'CONCEPT', 'TAXI', 'HOTROD', 'FORM1']
R2_TYPE = {'BMW': 4, 'CAMARO': 5, 'SUPRA': 6, 'BUGATTI': 7, 'VWBUS': 8, 'VIPER': 9, 'VWBUG': 10, 'CONCEPT': 11,
           'TAXI': 16, 'HOTROD': 17, 'FORM1': 18}


def u32(d, o): return struct.unpack_from('>I', d, o)[0]
def u16(d, o): return struct.unpack_from('>H', d, o)[0]
def cs(b): return b.split(b'\0')[0].decode('latin1')


def parse(d, game=1):
    """Return (models, textures, palettes). Rush 1 header: names, count, 0, 0, tex, count, pal, count (segment
    pointers). Rush 2 header (10 words): models, names, textures, palettes, count, tex count, pal count, ..."""
    m = lambda p: p & 0xFFFFFF
    if game == 1:
        names, nm, tex, nt, pal, npal = u32(d, 0), u32(d, 4), u32(d, 16), u32(d, 20), u32(d, 24), u32(d, 28)
    else:
        names, nm, tex, nt, pal, npal = u32(d, 4), u32(d, 16), u32(d, 8), u32(d, 20), u32(d, 12), u32(d, 24)
    models = [cs(d[m(names) + i * 0x18:][:16]) for i in range(nm)] if names else []
    texs = []
    for i in range(nt):
        o = m(tex) + i * 0x20
        texs.append(dict(name=cs(d[o:o + 16]), w=u16(d, o + 16), h=u16(d, o + 18), fmt=d[o + 20], siz=d[o + 21],
                         pal=struct.unpack_from('>h', d, o + 22)[0], data=u32(d, o + 24) & 0xFFFFFF, flags=u32(d, o + 28)))
    pals = [cs(d[m(pal) + i * 0x18:][:16]) for i in range(npal)]
    return models, texs, pals


def load(game, car):
    if game == 1:
        return Rush1().asset(25 + R1_CARS.index(car))
    return Rush2().asset(0x1D + R2_TYPE[car])


if __name__ == '__main__':
    cars = sys.argv[1:] or R1_CARS
    for c in cars:
        for g in (1, 2):
            d = load(g, c)
            mo, te, pa = parse(d, g)
            print(f'== {c} Rush {g}: {len(d):#x} bytes, {len(mo)} models, {len(te)} named textures, {len(pa)} palettes')
            print('  models :', ' '.join(mo).encode('ascii','replace').decode())
            for t in te:
                print(f"  tex {t['name']:16s} {t['w']}x{t['h']} fmt{t['fmt']}/{t['siz']} pal{t['pal']} data={t['data']:#x} fl={t['flags']:#x}")
            print('  pals   :', ' '.join(pa))


# ---- texture sheets -------------------------------------------------------------------------------------------
def rgba16(v):
    r, g, b, a = (v >> 11) & 31, (v >> 6) & 31, (v >> 1) & 31, v & 1
    return (r * 255 // 31, g * 255 // 31, b * 255 // 31, 255 if a else 0)


def r1_textures(d):
    """Walk every Rush 1 car model list; return {(addr,w,h,siz): set(model names)} for CI textures loaded by
    SETTIMG + SETTILE(render tile) + SETTILESIZE (F3DEX1: 06 = DL call, B8 = end)."""
    mo, _, _ = parse(d, 1)
    out = {}
    seen = set(); seg = u32(d, 0) >> 24   # each car file has its own segment (BMW 7, CAMARO 8, ...)
    for i, name in enumerate(mo):
        start = u32(d, 0x20 + i * 0x34 + 12)
        def walk(a):
            o = a & 0xFFFFFF; img = None; siz = None
            while True:
                if o + 8 > len(d): return
                w0, w1 = struct.unpack_from('>II', d, o); op = w0 >> 24
                if op == 0xFD: img = w1
                elif op == 0xF5 and ((w1 >> 24) & 7) == 0:
                    siz = (w0 >> 19) & 3; fmt = (w0 >> 21) & 7
                elif op == 0xF2 and img is not None and siz is not None and fmt == 2:
                    w = (((w1 >> 12) & 0xFFF) - ((w0 >> 12) & 0xFFF)) // 4 + 1; h = ((w1 & 0xFFF) - (w0 & 0xFFF)) // 4 + 1
                    out.setdefault((img & 0xFFFFFF, w, h, siz), set()).add(name)
                elif op == 0x06 and w1 not in seen and (w1 >> 24) == seg:
                    seen.add(w1); walk(w1)
                elif op == 0xB8: return
                o += 8
        walk(start)
    return out


def sheet(car, outdir, palname='RED', scale=3, cols=5, min_w=1024):
    from PIL import Image
    d1 = load(1, car)
    pals = parse(d1, 1)[2]
    po = u32(d1, 24) & 0xFFFFFF
    pi = next((i for i, n in enumerate(pals) if palname in n), 0)
    pal = [rgba16(u16(d1, (u32(d1, po + pi * 0x18 + 20) & 0xFFFFFF) + i * 2)) for i in range(256)]
    texs = r1_textures(d1)
    items = [it for it in sorted(texs.items()) if it[0][1] * it[0][2] >= min_w]
    cell = 64 * scale + 8; rowh = 64 * scale + 8
    img = Image.new('RGBA', (cols * cell, max(1, (len(items) + cols - 1) // cols) * rowh), (40, 40, 40, 255))
    for n, ((addr, w, h, siz), models) in enumerate(items):
        t = Image.new('RGBA', (w, h))
        for y in range(h):
            for x in range(w):
                if siz == 1:
                    idx = d1[addr + y * w + x]
                else:
                    b = d1[addr + (y * w + x) // 2]; idx = b >> 4 if x % 2 == 0 else b & 15
                t.putpixel((x, y), pal[idx])
        k = scale
        t = t.resize((w * k, h * k), Image.NEAREST)
        img.paste(t, ((n % cols) * cell, (n // cols) * rowh))
        print(f'  {addr:#x} {w}x{h} siz{siz} <- {sorted(models)[:3]}')
    os.makedirs(outdir, exist_ok=True)
    img.save(os.path.join(outdir, f'{car}_r1.png'))


def carpalette():
    """Rush 2's base car palette (texture CARPALETTE in asset 0x1C, 256 x RGBA16): grey ramp 1-31, green = accent
    marker (0x20 and rows), red main-colour ramps, magenta stripe-colour marker, etc. func_8008582C recolours it."""
    d = Rush2().asset(0x1C)
    po = u32(d, 12); o = po + 84 * 0x18
    assert cs(d[o:o + 16]) == 'CARPALETTE'
    data = u32(d, o + 20)
    return [rgba16(u16(d, data + i * 2)) for i in range(256)]


def sheet2(car, outdir, scale=3, cols=4):
    """Render Rush 2's 64x32/32x64/32x32 panel textures of a car with CARPALETTE."""
    from PIL import Image
    d = load(2, car); pal = carpalette()
    _, te, _ = parse(d, 2)
    items = [t for t in te if not t['name'].endswith('_4') and t['fmt'] == 1 and t['w'] * t['h'] >= 1024]
    cw, ch = 64 * scale + 6, 64 * scale + 6
    img = Image.new('RGBA', (cols * cw, ((len(items) + cols - 1) // cols) * ch), (40, 40, 40, 255))
    for n, t in enumerate(items):
        im = Image.new('RGBA', (t['w'], t['h']))
        for y in range(t['h']):
            for x in range(t['w']):
                im.putpixel((x, y), pal[d[t['data'] + y * t['w'] + x]])
        img.paste(im.resize((t['w'] * scale, t['h'] * scale), Image.NEAREST), ((n % cols) * cw, (n // cols) * ch))
    os.makedirs(outdir, exist_ok=True)
    img.save(os.path.join(outdir, f'{car}_r2.png'))
    return [t['name'] for t in items]


def r1_palettes(d):
    """Rush 1 car file palettes: list of (name, [256 RGBA16 words]) (10 paint colours: BLK BLU GRN GRY ORG PNK PUR RED
    TIL YEL). Entries that are identical in all ten are fixed colours (lights, glass, decals); the others are the
    paint ramps."""
    _, _, pals = parse(d, 1)
    po = u32(d, 24) & 0xFFFFFF
    out = []
    for i, n in enumerate(pals):
        pd = u32(d, po + i * 0x18 + 20) & 0xFFFFFF
        out.append((n, [u16(d, pd + j * 2) for j in range(256)]))
    return out


def r1_panels(d):
    """Distinct full-size (not 16x8 mip) CI8 panel textures of a Rush 1 car: {addr: (w, h, [model names])}."""
    return {a: (w, h, sorted(m)) for (a, w, h, s), m in r1_textures(d).items() if w * h >= 1024}


R1_BODY = set(range(1, 26)) | {30} | set(range(65, 87))   # Rush 1 paint-ramp palette indices (vary across the 10 palettes)
R2_BODY = set(range(1, 32)) | set(range(33, 64))          # Rush 2 main (1-31) and accent (33-63) ramps (table 0x800C5670)


def r2_panels(d2, stage='D0'):
    """Rush 2 car panel textures of one damage stage, full size only: {name: (w, h, data offset)}."""
    _, te, _ = parse(d2, 2)
    return {t['name']: (t['w'], t['h'], t['data']) for t in te
            if f'_{stage}_' in t['name'] and not re.search(r'_\d_4$', t['name'])}


def pair_panels(car, stage='D0', verbose=True):
    """Match each Rush 2 panel texture to the Rush 1 panel with the most similar body/fixed-colour layout."""
    d1, d2 = load(1, car), load(2, car)
    p1 = r1_panels(d1); p2 = r2_panels(d2, stage)
    res = {}
    for n2, (w2, h2, o2) in sorted(p2.items()):
        best = None
        for a1, (w1, h1, m1) in p1.items():
            if (w1, h1) != (w2, h2): continue
            same = sum((d1[a1 + i] in R1_BODY) == (d2[o2 + i] in R2_BODY) for i in range(w2 * h2)) / (w2 * h2)
            if best is None or same > best[0]: best = (same, a1, m1)
        res[n2] = best
        if verbose: print(f'  {n2:12s} {w2}x{h2} -> R1 {best[1]:#x} {best[2]} match {best[0]:.3f}' if best else f'  {n2} no match')
    return res


# ---- meshes -------------------------------------------------------------------------------------------------
def s16(d, o): return struct.unpack_from('>h', d, o)[0]


def meshes(d, game):
    """Walk every model's display list of a car file; return {model name: [triangle]} where a triangle is
    (verts, tex) with verts = 3 x (x, y, z, s, t) (s/t in texels = value / 32) and tex = (data offset, w, h, uls, ult) of the
    largest CI texture loaded when it was drawn (None if none). Rush 1 = F3DEX1 (VTX 04, DL 06, TRI1 BF, TRI2 B1,
    vertex indices x2), Rush 2 = F3DEX2 (VTX 01, DL DE, TRI1 05, TRI2 06, indices x2)."""
    seg = (u32(d, 0) >> 24) if game == 1 else 0
    if game == 1:
        models = parse(d, 1)[0]
        recs = [u32(d, 0x20 + i * 0x34 + 12) for i in range(len(models))]
    else:
        models, nm = [], u32(d, 16)
        no = u32(d, 4); mo = u32(d, 0)
        models = [cs(d[no + i * 0x18:][:16]) for i in range(nm)]
        recs = [u32(d, mo + i * 0x34 + 12) for i in range(nm)]
    VTX, DL, END = (0x04, 0x06, 0xB8) if game == 1 else (0x01, 0xDE, 0xDF)
    out = {}
    for name, start in zip(models, recs):
        tris = []
        vbuf = [None] * 64
        st = dict(img=None, fmt=None, best=None)
        seen = set()

        def walk(a, depth=0):
            o = a & 0xFFFFFF
            while o + 8 <= len(d):
                w0, w1 = struct.unpack_from('>II', d, o); op = w0 >> 24
                if op == VTX:
                    if game == 1:
                        n = (w0 >> 10) & 0x3F; v0 = (w0 >> 17) & 0x7F
                        v0 = (w0 >> 16 & 0xFF) // 2
                    else:
                        n = (w0 >> 12) & 0xFF; v0 = ((w0 >> 1) & 0x7F) - n
                    src = w1 & 0xFFFFFF
                    for k in range(n):
                        if src + k * 16 + 16 <= len(d) and v0 + k < 64:
                            q = src + k * 16
                            vbuf[v0 + k] = (s16(d, q), s16(d, q + 2), s16(d, q + 4), s16(d, q + 8) / 32.0, s16(d, q + 10) / 32.0)
                elif op == 0xFD:
                    st['img'] = w1 & 0xFFFFFF
                elif op == 0xF2 and st['img'] is not None:
                    w = (((w1 >> 12) & 0xFFF) - ((w0 >> 12) & 0xFFF)) // 4 + 1
                    h = ((w1 & 0xFFF) - (w0 & 0xFFF)) // 4 + 1
                    if st['best'] is None or w * h >= st['best'][1] * st['best'][2]:
                        st['best'] = (st['img'], w, h, ((w0 >> 12) & 0xFFF) / 4.0, (w0 & 0xFFF) / 4.0)
                elif (game == 1 and op in (0xBF, 0xB1)) or (game == 2 and op in (0x05, 0x06)):
                    if game == 1 and op == 0xBF or game == 2 and op == 0x05:
                        ids = [[(w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF]] if game == 1 else \
                              [[(w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF]]
                    else:
                        ids = [[(w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF], [(w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF]]
                    for t in ids:
                        vs = [vbuf[i // 2] if i // 2 < 64 else None for i in t]
                        if all(vs):
                            tris.append((vs, st['best']))
                elif op == DL and (w1 >> 24) == (seg if game == 1 else 0) and depth < 6:
                    # Rush 1 car panels pick their texture with B4/B0 (RDPHALF_1 + BRANCH_Z) between a 16x8 far list and
                    # a 64x32 near one; walking both and keeping the larger texture takes the near one.
                    walk(w1, depth + 1)
                elif op == END:
                    return
                o += 8
        # reset per-model texture state between models
        walk(start)
        out[name] = tris
    return out
