"""Renders objects of N64 Rush 2049 model containers to PNG (software, textured, z-buffered), to compare files.

    view49.py OUT.png FILE:OBJECT [FILE:OBJECT ...] [--yaw DEG] [--pitch DEG] [--size N]

FILE is a container (an N64 ROM file dumped by roms.py, or a Dreamcast source output from cpp_test/dc_test.exe); a
plain number reads that file from RUSH2049_ROM. Each object (its first LOD) is drawn in its own panel, side by side.
Follows the lists as the RSP would: VTX into the 32-entry buffer, TRI1/TRI2, G_DL calls into texture-load lists
(SETTIMG, LOADTLUT, the render tile's format and SETTILESIZE pick the texture), conditional ops taken as not mirrored.
Needs Pillow and numpy.
"""
import struct, sys

import numpy as np
from PIL import Image

sys.path.insert(0, __import__('os').path.dirname(__file__))
from model import M49


def u32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def rgba16(v):
    return ((v >> 11) & 31) * 255 // 31, ((v >> 6) & 31) * 255 // 31, ((v >> 1) & 31) * 255 // 31, 255 if v & 1 else 0


def decode_texture(d, imag, st):
    """st: texture state {timg, tlut, fmt, siz, line, w, h}. Returns an h x w x 4 uint8 array or None."""
    fmt, siz, w, h = st.get('fmt'), st.get('siz'), st.get('w'), st.get('h')
    if fmt is None or not w or not h or st.get('timg') is None:
        return None
    a = st['timg'] + imag
    line = st.get('line', 0) * 8 * (2 if siz == 3 else 1)  # 32-bit texels are split over TMEM's two halves
    out = np.zeros((h, w, 4), np.uint8)
    pal = None
    if fmt == 2 and st.get('tlut') is not None:
        p = st['tlut'] + imag
        pal = [rgba16(struct.unpack_from('>H', d, p + i * 2)[0]) for i in range(256)]
    for y in range(h):
        row = a + y * (line if line else w * (1 << siz) // 2)
        for x in range(w):
            try:
                if siz == 0:
                    b = d[row + x // 2]
                    v = (b >> 4) if x % 2 == 0 else (b & 15)
                    c = pal[v + st.get('palette', 0) * 16] if pal else (v * 17,) * 3 + (255,)
                elif siz == 1:
                    v = d[row + x]
                    if fmt == 2 and pal:
                        c = pal[v]
                    elif fmt == 3:
                        c = ((v >> 4) * 17,) * 3 + ((v & 15) * 17,)
                    else:
                        c = (v,) * 3 + (255,)
                elif siz == 2:
                    v = struct.unpack_from('>H', d, row + x * 2)[0]
                    c = rgba16(v) if fmt == 0 else (v >> 8,) * 3 + (v & 255,)
                else:
                    c = tuple(d[row + x * 4:row + x * 4 + 4])
            except (IndexError, struct.error, TypeError):
                c = (255, 0, 255, 255)
            out[y, x] = c
    return out


def walk(m, dl):
    """Triangles [(3 x (x, y, z, s, t, r, g, b, a), texture state)] of a list."""
    d = m.d
    buf = [None] * 64
    tris = []
    st = {}
    stack, o = [], dl
    for _ in range(500000):
        w0, w1 = u32(d, o), u32(d, o + 4)
        op = w0 >> 24
        if op == 0x01:
            n = (w0 >> 12) & 0xFF
            v0 = ((w0 >> 1) & 0x7F) - n
            for i in range(n):
                buf[v0 + i] = struct.unpack_from('>hhhHhhBBBB', d, (w1 & 0xFFFFFF) + i * 16)
        elif op in (0x05, 0x06):
            for word in ((w0,) if op == 0x05 else (w0, w1)):
                idx = [((word >> s) & 0xFF) // 2 for s in (16, 8, 0)]
                if all(buf[i] is not None for i in idx):
                    tris.append(([buf[i] for i in idx], dict(st)))
        elif op == 0xFD:
            st['last_timg'] = w1 & 0xFFFFFF
            if 'timg' not in st or st.get('_new'):
                st['timg'] = w1 & 0xFFFFFF
                st['_new'] = False
        elif op == 0xF0:
            st['tlut'] = st['last_timg']
        elif op == 0xF5:
            tile = (w1 >> 24) & 7
            if tile == 0:
                st['fmt'], st['siz'], st['line'] = (w0 >> 21) & 7, (w0 >> 19) & 3, (w0 >> 9) & 0x1FF
                st['palette'] = (w1 >> 20) & 15
        elif op == 0xF2:
            if (w1 >> 24) & 7 == 0:
                st['w'] = (((w1 >> 12) & 0xFFF) >> 2) + 1
                st['h'] = ((w1 & 0xFFF) >> 2) + 1
        elif op == 0xDE:
            if not (w0 >> 16) & 0xFF:
                stack.append(o + 8)
            st['_new'] = True
            st.pop('timg', None)
            o = w1 & 0xFFFFFF
            continue
        elif op == 0xE0 and (w0 >> 16) & 0xFF == 1:
            if (w0 & 0xFFFF) != 3:      # not mirrored: blocks for "mirrored only" are skipped
                o = w1 & 0xFFFFFF
                continue
        elif op == 0xDF:
            if not stack:
                break
            o = stack.pop()
            continue
        o += 8
    return tris


def render(m, name, size, yaw, pitch):
    ob = next((o for o in m.objects if o['name'] == name), None)
    img = np.zeros((size, size, 3), np.float32) + 40
    if ob is None or not ob['lods']:
        return img
    tris = walk(m, ob['lods'][0][3])
    if not tris:
        return img
    pts = np.array([v[:3] for t, _ in tris for v in t], np.float64)
    c = (pts.min(0) + pts.max(0)) / 2
    r = np.abs(pts - c).max() or 1
    cy, sy, cp, sp = np.cos(np.radians(yaw)), np.sin(np.radians(yaw)), np.cos(np.radians(pitch)), np.sin(np.radians(pitch))
    rot = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]]) @ np.eye(3)
    rot = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]]) @ rot
    zbuf = np.full((size, size), np.inf)
    cache = {}
    for t, st in tris:
        key = (st.get('timg'), st.get('tlut'), st.get('fmt'), st.get('siz'), st.get('w'), st.get('h'), st.get('palette'))
        if key not in cache:
            cache[key] = decode_texture(m.d, m.imag, st)
        tex = cache[key]
        P = (np.array([v[:3] for v in t], np.float64) - c) @ rot.T / r
        X = (P[:, 0] * 0.45 + 0.5) * size
        Y = (-P[:, 1] * 0.45 + 0.5) * size
        Z = P[:, 2]
        x0, x1 = int(max(0, np.floor(X.min()))), int(min(size - 1, np.ceil(X.max())))
        y0, y1 = int(max(0, np.floor(Y.min()))), int(min(size - 1, np.ceil(Y.max())))
        den = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2])
        if abs(den) < 1e-9 or x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        l0 = ((Y[1] - Y[2]) * (gx - X[2]) + (X[2] - X[1]) * (gy - Y[2])) / den
        l1 = ((Y[2] - Y[0]) * (gx - X[2]) + (X[0] - X[2]) * (gy - Y[2])) / den
        l2 = 1 - l0 - l1
        inside = (l0 >= 0) & (l1 >= 0) & (l2 >= 0)
        z = l0 * Z[0] + l1 * Z[1] + l2 * Z[2]
        zb = zbuf[y0:y1 + 1, x0:x1 + 1]
        draw = inside & (z < zb)
        if not draw.any():
            continue
        col = np.array([v[6:9] for v in t], np.float64)
        shade = l0[..., None] * col[0] + l1[..., None] * col[1] + l2[..., None] * col[2]
        if tex is not None:
            S = np.array([v[4] for v in t], np.float64) / 32 + 0.5
            T = np.array([v[5] for v in t], np.float64) / 32 + 0.5
            s = (l0 * S[0] + l1 * S[1] + l2 * S[2]).astype(np.int64) % tex.shape[1]
            tt = (l0 * T[0] + l1 * T[1] + l2 * T[2]).astype(np.int64) % tex.shape[0]
            texel = tex[tt, s].astype(np.float64)
            draw &= texel[..., 3] > 0
            rgb = texel[..., :3] * shade / 255
        else:
            rgb = shade
        zb[draw] = z[draw]
        img[y0:y1 + 1, x0:x1 + 1][draw] = rgb[draw]
    return img


def main():
    args = sys.argv[1:]
    opt = {'--yaw': 30.0, '--pitch': 20.0, '--size': 320}
    for k in list(opt):
        if k in args:
            i = args.index(k)
            opt[k] = type(opt[k])(args[i + 1])
            del args[i:i + 2]
    out, specs = args[0], args[1:]
    panels = []
    files = {}
    for spec in specs:
        f, name = spec.rsplit(':', 1)
        if f not in files:
            if f.isdigit():
                from roms import Rush2049
                files[f] = M49(Rush2049().file(int(f)))
            else:
                files[f] = M49(open(f, 'rb').read())
        panels.append(render(files[f], name, opt['--size'], opt['--yaw'], opt['--pitch']))
    sheet = np.concatenate(panels, axis=1).clip(0, 255).astype(np.uint8)
    Image.fromarray(sheet).save(out)


if __name__ == '__main__':
    main()
