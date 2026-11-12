"""Renders models of a Rush 2 model container (file-offset addresses, as the converters write them: e.g. the track select
art tools/rush1/cpp_test/out/asset3_rush1.bin) to PNG, for checking converted dioramas without the game.

    python tools/menu_model_view.py CONTAINER OUT.png MODEL [MODEL ...] [--yaw DEG] [--pitch DEG] [--size N] [--list]

Each model is a panel, side by side. The F3DEX2 list is run with a small RSP/RDP model: G_VTX into a 64-entry vertex
buffer, G_TRI1/G_TRI2, G_DL calls into the container, G_TEXTURE on/off and scale, and the texture loads
(SETTIMG / SETTILE / LOADTLUT / LOADBLOCK / SETTILESIZE, read back from the loaded address rather than TMEM's layout);
CI4 / CI8 through their RGBA16 TLUT, RGBA16, IA8/IA16, I4/I8. Textured triangles are TEXEL0 x SHADE, others SHADE;
texturing starts on, as the game draws models (lists that draw untextured say so with G_TEXTURE).
Uses tools/glb_view.py's rasterizer (numpy, Pillow).
"""
import argparse, os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb_view


def cname(d, o):
    b = d[o:o + 16]
    return b.split(b'\0')[0].decode('latin1')


def models(d):
    h = struct.unpack_from('>10I', d, 0)
    out = {}
    for i in range(h[4]):
        out[cname(d, h[1] + i * 0x18)] = struct.unpack_from('>I', d, h[0] + i * 0x34 + 12)[0]
    return out


def rgba16(c):
    return ((c >> 11) & 31) / 31, ((c >> 6) & 31) / 31, ((c >> 1) & 31) / 31, float(c & 1)


def decode(d, addr, fmt, siz, w, h, tlut):
    img = np.zeros((h, w, 4), np.float32)
    for y in range(h):
        for x in range(w):
            i = y * w + x
            if siz == 0:
                b = d[addr + i // 2] if addr + i // 2 < len(d) else 0
                v = (b >> 4) if i % 2 == 0 else b & 15
            elif siz == 1:
                v = d[addr + i] if addr + i < len(d) else 0
            else:
                v = struct.unpack_from('>H', d, addr + 2 * i)[0] if addr + 2 * i + 2 <= len(d) else 0
            if fmt == 2:          # CI
                c = tlut[v] if v < len(tlut) else 0
                img[y, x] = rgba16(c)
            elif fmt == 0:        # RGBA16
                img[y, x] = rgba16(v)
            elif fmt == 3:        # IA
                if siz == 0:
                    ii, a = (v >> 1) / 7, v & 1
                elif siz == 1:
                    ii, a = (v >> 4) / 15, (v & 15) / 15
                else:
                    ii, a = (v >> 8) / 255, (v & 255) / 255
                img[y, x] = (ii, ii, ii, a)
            else:                 # I
                ii = v / 15 if siz == 0 else v / 255
                img[y, x] = (ii, ii, ii, ii)
    return img


def run(d, dl):
    """(P, U, C, T) triangles and texture list of the list at dl."""
    vb = [None] * 64
    P, U, C, T = [], [], [], []
    texs = []; tex_cache = {}
    st = dict(on=True, ss=1.0, ts=1.0, timg=(0, 0, 0), tiles={}, blocks={}, tluts={})

    def texture():
        t0 = st['tiles'].get(0)
        if not st['on'] or t0 is None:
            return -1, 1, 1
        fmt, siz, line, tmem, pal, size = t0['fmt'], t0['siz'], t0['line'], t0['tmem'], t0['pal'], t0.get('size')
        src = st['blocks'].get(tmem)
        if src is None or size is None:
            return -1, 1, 1
        bits = (4, 8, 16, 32)[siz]
        w = max(1, line * 64 // bits) if line else int(size[2] - size[0] + 1)
        hgt = int(size[3] - size[1] + 1)
        tl = []
        if fmt == 2:
            base = 256 + (pal * 16 if siz == 0 else 0)
            for k in range(256 if siz == 1 else 16):
                tl.append(st['tluts'].get(base + k, 0))
        key = (src, fmt, siz, w, hgt, tuple(tl))
        if key not in tex_cache:
            tex_cache[key] = len(texs)
            texs.append((decode(d, src, fmt, siz, w, hgt, tl), 10497, 10497))
        return tex_cache[key], w, hgt

    def tri(w):
        idx = [((w >> s) & 0xFF) // 2 for s in (16, 8, 0)]
        vs = [vb[i] for i in idx]
        if None in vs:
            return
        ti, tw, th = texture()
        P.append([v[0] for v in vs])
        C.append([v[2] for v in vs])
        U.append([(v[1][0] * st['ss'] / tw, v[1][1] * st['ts'] / th) for v in vs])
        T.append(ti)

    def walk(o, depth):
        while o + 8 <= len(d):
            w0, w1 = struct.unpack_from('>II', d, o)
            op = w0 >> 24
            if op == 0xDF:
                return
            if op == 0xDE:
                walk(w1, depth + 1)
                if (w0 >> 16) & 0xFF == 1:
                    return
            elif op == 0x01:
                n = (w0 >> 12) & 0xFF; end = (w0 >> 1) & 0x7F
                for k in range(n):
                    a = w1 + 16 * k
                    x, y, z, _, s, t = struct.unpack_from('>6h', d, a)
                    r, g, b, al = d[a + 12:a + 16]
                    vb[end - n + k] = ((x, y, z), (s / 32, t / 32), (r / 255, g / 255, b / 255, al / 255))
            elif op == 0x05:
                tri(w0 & 0xFFFFFF)
            elif op == 0x06:
                tri(w0 & 0xFFFFFF); tri(w1 & 0xFFFFFF)
            elif op == 0xD7:
                st['on'] = bool(w0 & 2)
                st['ss'] = ((w1 >> 16) & 0xFFFF) / 65536 or 1.0
                st['ts'] = (w1 & 0xFFFF) / 65536 or 1.0
            elif op == 0xFD:
                st['timg'] = ((w0 >> 21) & 7, (w0 >> 19) & 3, w1)
            elif op == 0xF5:
                tile = (w1 >> 24) & 7
                st['tiles'][tile] = dict(st['tiles'].get(tile, {}), fmt=(w0 >> 21) & 7, siz=(w0 >> 19) & 3,
                                         line=(w0 >> 9) & 0x1FF, tmem=w0 & 0x1FF, pal=(w1 >> 20) & 15)
            elif op == 0xF2:
                tile = (w1 >> 24) & 7
                st['tiles'].setdefault(tile, {})['size'] = (((w0 >> 12) & 0xFFF) / 4, (w0 & 0xFFF) / 4,
                                                           ((w1 >> 12) & 0xFFF) / 4, (w1 & 0xFFF) / 4)
            elif op == 0xF3:
                tile = (w1 >> 24) & 7
                st['blocks'][st['tiles'].get(tile, {}).get('tmem', 0)] = st['timg'][2]
            elif op == 0xF0:
                tile = (w1 >> 24) & 7
                n = ((w1 >> 14) & 0x3FF) + 1
                base = st['tiles'].get(tile, {}).get('tmem', 256)
                for k in range(n):
                    a = st['timg'][2] + 2 * k
                    if a + 2 <= len(d):
                        st['tluts'][base + k] = struct.unpack_from('>H', d, a)[0]
            o += 8

    walk(dl, 0)
    if not P:
        return None, texs
    return (np.array(P, np.float32), np.array(U, np.float32), np.array(C, np.float32), np.array(T)), texs


if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('container'); ap.add_argument('out'); ap.add_argument('models', nargs='*')
    ap.add_argument('--yaw', type=float, default=30); ap.add_argument('--pitch', type=float, default=35)
    ap.add_argument('--size', type=int, default=480); ap.add_argument('--list', action='store_true')
    a = ap.parse_args()
    d = open(a.container, 'rb').read()
    lists = models(d)
    if a.list:
        print(' '.join(sorted(lists)))
        sys.exit()
    panels = []
    for m in a.models:
        tris, texs = run(d, lists[m])
        if tris is None:
            print(m, 'draws nothing'); continue
        P = tris[0].copy(); P[..., 0] = -P[..., 0]     # the game's x runs the other way from glTF's
        panels.append(glb_view.render((P, tris[1], tris[2], tris[3]), texs, a.yaw, a.pitch, a.size))
    if panels:
        img = Image.new('RGB', (a.size * len(panels), a.size))
        for i, p in enumerate(panels):
            img.paste(p, (i * a.size, 0))
        img.save(a.out)
        print('wrote', a.out)
