"""Rush 1 collision files (assets 44-57) and their conversion to Rush 2's layout (research aid / prototype).

Rush 1: 0x20-byte header (u16 nSeg, nNode, nPoly, nVert, leafBytes, vlistBytes, ..., +0x18/+0x1A/+0x1C globals), then
SEG (0x84), NODE (0x14), POLY (0x1A), VERT (8), LEAF, VLIST. A polygon is Rush 2's with an extra u16 at +4, and its
local frame is (x, y) = surface plane, z = depth (Rush 2: (z, x) plane, y = height): Rush 2 local = (y, -z, x).
"""
import struct, sys
from r1 import Rush1

def parse(d):
    h = struct.unpack('>16H', d[:0x20])
    ns, nn, np_, nv, lb, vb = h[:6]
    o = 0x20
    segs = d[o:o + ns * 0x84]; o += ns * 0x84
    nodes = d[o:o + nn * 0x14]; o += nn * 0x14
    polys = [d[o + i * 0x1A:o + i * 0x1A + 0x1A] for i in range(np_)]; o += np_ * 0x1A
    verts = [struct.unpack('>hhhH', d[o + i * 8:o + i * 8 + 8]) for i in range(nv)]; o += nv * 8
    leaf = d[o:o + lb]; o += lb
    vlist = d[o:o + vb]; o += vb
    assert o == len(d), (hex(o), hex(len(d)))
    return dict(h=h, segs=segs, nodes=nodes, polys=polys, verts=verts, leaf=leaf, vlist=vlist)

def decode_vlist(buf, off, count):
    out = []
    while count > 0:
        v = (buf[off] << 8) | buf[off + 1]; off += 2
        run = 0
        if count >= 2 and buf[off] >= 0xC0:
            run = buf[off] & 0x3F; off += 1
        count -= run + 1
        out += list(range(v, v + run + 1))
    trailer = (buf[off] << 8) | buf[off + 1] if off + 2 <= len(buf) else 0
    return out, off, trailer

def vval(v):
    x, y, z, f = v
    return (x + ((f >> 10) & 31) / 32, y + ((f >> 5) & 31) / 32, z + (f & 31) / 32)

if __name__ == '__main__':
    r = Rush1()
    import collections
    for i in range(44, 58):
        c = parse(r.asset(i))
        roles = collections.defaultdict(set); dets = collections.Counter(); f4 = collections.Counter()
        nonplanar = 0
        for p in c['polys']:
            fl, info, x4 = struct.unpack('>HHH', p[:6]); m = struct.unpack('>9h', p[6:24]); vo = struct.unpack('>H', p[24:26])[0]
            f4[x4] += 1
            idx, _, _ = decode_vlist(c['vlist'], vo, info & 0xF)
            roles[idx[0]].add('w')
            for k in idx[1:]:
                roles[k].add('l')
                if abs(vval(c['verts'][k])[2]) > 0.05: nonplanar += 1
            M = [[m[3 * a + b] / 16384 for b in range(3)] for a in range(3)]
            det = (M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0])
                   + M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]))
            dets[round(det, 1)] += 1
        both = sum(1 for k, s in roles.items() if len(s) > 1)
        print(i, 'hdr', ' '.join('%x' % x for x in c['h'][6:15]), 'polys', len(c['polys']), 'both-role verts', both,
              'nonplanar', nonplanar, 'dets', dict(dets), 'x4', dict(f4.most_common(4)))
