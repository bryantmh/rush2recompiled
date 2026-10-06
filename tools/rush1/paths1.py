"""Rush 1 AI lane files (assets 58-64 forward, 65-71 backward) and per-track checkpoint tables (research aid).

Lane file: 4 x { s16 count, s16 loop_index, s16 a, s16 b; count x { s16 x, y, z (collision space: render z, x, -y);
u8 speed (ft/s: the driver func_8007CDD4 compares lane x 1.05 with the car speed); u8 behaviour; s16 index } }. Checkpoint tables: 0x800C7B6C (forward) / 0x800C7B88 (backward), ptr per
track to 0x18-byte records { f32 x, y, z (render space), f32 0; s16 flags (-1 ends); s16 time[3] }.
"""
import struct, math
from r1 import Rush1

def lanes(d):
    o = 0; out = []
    for l in range(4):
        h = struct.unpack('>4h', d[o:o + 8]); n = h[0]
        pts = []
        for k in range(n):
            c0, c1, c2, sp, be, ix = struct.unpack('>hhhBBh', d[o + 8 + k * 10:o + 18 + k * 10])
            pts.append(((c1, -c2, c0), sp, be, ix))
        out.append((h, pts)); o += 8 + n * 10
    assert o == len(d), (o, len(d))
    return out

def checkpoints(r, t, backward):
    p = r.w((0x800C7B88 if backward else 0x800C7B6C) + t * 4); out = []
    for i in range(13):
        d = r.read(p + i * 0x18, 0x18)
        x, y, z, w = struct.unpack('>4f', d[:16]); fl, a, b, c = struct.unpack('>4h', d[16:24])
        out.append(((x, y, z), fl, (a, b, c)))
        if fl == -1: break
    return out

def dist(a, b): return math.dist(a, b)

if __name__ == '__main__':
    r = Rush1()
    for t in range(7):
        for b in (0, 1):
            L = lanes(r.asset(58 + t + 7 * b)); cps = checkpoints(r, t, b)
            print('track', t + 1, 'bwd' if b else 'fwd', 'cps', [(c[1], c[2][0]) for c in cps])
            for li, (h, pts) in enumerate(L):
                length = sum(dist(pts[k][0], pts[k + 1][0]) for k in range(len(pts) - 1))
                close = dist(pts[-1][0], pts[h[1]][0]) if 0 <= h[1] < len(pts) else -1
                near = []
                for c in cps[:-1]:
                    k = min(range(len(pts)), key=lambda k: dist(pts[k][0], c[0])); near.append((k, round(dist(pts[k][0], c[0]))))
                print('  lane', li, 'hdr', h, 'len', round(length), 'end->loop', round(close), 'cp nearest', near)
