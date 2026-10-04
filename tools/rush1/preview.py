"""Top-down preview of a converted Rush 1 track (test aid): runs the converted F3DEX2 model lists (vertex cache and
triangles) for every placed section, shades by height and overlays the converted AI spine and checkpoint gates.

Usage: python preview.py OUT_DIR [track 1-7 ...]
"""
import math, os, struct, sys
from PIL import Image, ImageDraw
import track1
from r1 import Rush1
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
import paths as r2paths


def model_lists(g):
    h = struct.unpack_from('>10I', g, 0)
    out = {}
    for i in range(h[4]):
        name = track1.cname(g, h[1] + i * 0x18)
        out[name] = struct.unpack_from('>I', g, h[0] + i * 0x34 + 12)[0]
    return out


def triangles(g, dl):
    cache = [None] * 32
    o = dl
    while True:
        w0, w1 = struct.unpack_from('>II', g, o)
        op = w0 >> 24
        if op == 0xDF:
            return
        if op == 0x01:
            n = (w0 >> 12) & 0xFF
            end = (w0 >> 1) & 0x7F
            for k in range(n):
                cache[end - n + k] = struct.unpack_from('>3h', g, w1 + k * 16)
        elif op == 0x05:
            yield [cache[((w0 >> s) & 0xFF) // 2] for s in (16, 8, 0)]
        elif op == 0x06:
            yield [cache[((w0 >> s) & 0xFF) // 2] for s in (16, 8, 0)]
            yield [cache[((w1 >> s) & 0xFF) // 2] for s in (16, 8, 0)]
        o += 8


def render(r, t, out_dir, size=1400):
    res = track1.convert(r, t)
    g, pl = res['geometry'], res['placement']
    lists = model_lists(g)
    base = struct.unpack_from('>I', pl, 4)[0]
    tris = []
    for o in range(base, len(pl), 0x64):
        name = track1.cname(pl, o)
        if name not in lists:
            continue
        pos = struct.unpack_from('>3f', pl, o + 0x34)
        child = o >= base and struct.unpack_from('>h', pl, o + 0x46)[0]
        for tri in triangles(g, lists[name]):
            if None in tri:
                continue
            tris.append([(v[0] + pos[0], v[1] + pos[1], v[2] + pos[2]) for v in tri])
    p = r2paths.parse(res['path'])
    xs = [q[0] for q in p['spine']]; zs = [q[2] for q in p['spine']]; ys = [v[1] for tr in tris for v in tr]
    x0, x1, z0, z1 = min(xs) - 300, max(xs) + 300, min(zs) - 300, max(zs) + 300
    y0, y1 = sorted(ys)[len(ys) // 50], sorted(ys)[-len(ys) // 50]
    sc = size / max(x1 - x0, z1 - z0)
    img = Image.new('RGB', (int((x1 - x0) * sc) + 1, int((z1 - z0) * sc) + 1), (10, 10, 30))
    d = ImageDraw.Draw(img)
    for tr in sorted(tris, key=lambda tr: sum(v[1] for v in tr)):
        c = int(40 + 200 * min(max((sum(v[1] for v in tr) / 3 - y0) / max(y1 - y0, 1), 0), 1))
        d.polygon([((v[0] - x0) * sc, (v[2] - z0) * sc) for v in tr], fill=(c, c, c))
    sp = [((q[0] - x0) * sc, (q[2] - z0) * sc) for q in p['spine']]
    d.line(sp, fill=(230, 30, 30), width=2)
    for c in p['cps']:
        cx, cz = (c['pos'][0] - x0) * sc, (c['pos'][2] - z0) * sc
        rad = math.sqrt(c['r2']) * sc
        dx, dz = -c['dir'][2], c['dir'][0]
        d.line([(cx - dx * rad, cz - dz * rad), (cx + dx * rad, cz + dz * rad)], fill=(40, 220, 60), width=3)
    path = os.path.join(out_dir, 'track%d.png' % (t + 1))
    img.save(path)
    print(path, len(tris), 'triangles')


if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    r = Rush1()
    for t in ([int(a) - 1 for a in sys.argv[2:]] or range(7)):
        render(r, t, out)
