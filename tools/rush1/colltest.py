"""Compares Rush 1's polygon ground test on original collision data with Rush 2's on the converted data."""
import struct, random, math
import track1
from r1 import Rush1
from collision1 import parse as parse1, decode_vlist, vval

def r1_polys(d):
    c = parse1(d); out = []
    for p in c['polys']:
        fl, info = struct.unpack('>HH', p[:4]); m = struct.unpack('>9h', p[6:24]); vo = struct.unpack('>H', p[24:26])[0]
        idx, _, _ = decode_vlist(c['vlist'], vo, info & 0xF)
        out.append((fl, [[m[3*b+a]/16384 for b in range(3)] for a in range(3)], [vval(c['verts'][k]) for k in idx]))
    return out

def r2_polys(d):
    ns, nn, np_, nv, lb, vb = struct.unpack('>6H', d[:12])
    o = 0xC + ns * 0x84 + nn * 0x14; po = o; vo_ = po + np_ * 0x18; vl = d[vo_ + nv * 8 + lb:]
    verts = [struct.unpack('>hhhH', d[vo_ + i * 8:vo_ + i * 8 + 8]) for i in range(nv)]
    out = []
    for i in range(np_):
        p = d[po + i * 0x18:po + i * 0x18 + 0x18]
        fl, info = struct.unpack('>HH', p[:4]); m = struct.unpack('>9h', p[4:22]); voff = struct.unpack('>H', p[22:24])[0]
        idx, _, _ = decode_vlist(vl, voff, info & 0xF)
        out.append((fl, [[m[3*a+b]/16384 for b in range(3)] for a in range(3)], [vval(verts[k]) for k in idx]))
    return out

def mv(M, v): return [sum(M[i][j] * v[j] for j in range(3)) for i in range(3)]

def test1(polys, P):
    best = -1e9; hit = None
    for i, (fl, M, V) in enumerate(polys):
        if fl & 0xF not in (0, 1, 2): continue
        L = mv(M, [P[k] - V[0][k] for k in range(3)])
        if L[2] >= 5.0 or L[2] < best: continue
        x, y = L[0], L[1]; vs = V[1:]
        vl = vs[-1]
        if (vl[0] - x) * vl[1] - (vl[1] - y) * vl[0] < 0: continue
        v1 = vs[0]
        if x * v1[1] - y * v1[0] < 0: continue
        ok = True
        for a, b in zip(vs, vs[1:]):
            if (x - a[0]) * (b[1] - a[1]) - (b[0] - a[0]) * (y - a[1]) < 0: ok = False; break
        if ok: best, hit = L[2], i
    return hit, best

def test2(polys, P):
    best = 1e9; hit = None
    for i, (fl, M, V) in enumerate(polys):
        if fl & 0xF not in (0, 1, 2): continue
        L = mv(M, [P[k] - V[0][k] for k in range(3)])
        if L[1] <= -5.0 or L[1] > best: continue
        x, z = L[0], L[2]; vs = V[1:]
        vl = vs[-1]
        if (vl[2] - z) * vl[0] - (vl[0] - x) * vl[2] < 0: continue
        v1 = vs[0]
        if z * v1[0] - x * v1[2] < 0: continue
        ok = True
        for a, b in zip(vs, vs[1:]):
            if (z - a[2]) * (b[0] - a[0]) - (b[2] - a[2]) * (x - a[0]) < 0: ok = False; break
        if ok: best, hit = L[1], i
    return hit, best

r = Rush1(); random.seed(1)
for t in range(7):
    d1 = r.asset(44 + t); p1 = r1_polys(d1); p2 = r2_polys(track1.convert_collision(d1))
    L = track1.lanes(r.asset(58 + t))
    same = diff = miss = 0
    for _ in range(150):
        ln = random.choice(L)['points']; q = random.choice(ln)
        P = [q[0] + random.uniform(-8, 8), q[1] + 3.0, q[2] + random.uniform(-8, 8)]   # render, 3 ft up
        Pc = [P[2], P[0], -P[1]]                                                         # Rush 1 collision space
        h1, d1v = test1(p1, Pc); h2, d2v = test2(p2, P)
        if h1 is None and h2 is None: miss += 1
        elif h1 == h2 and abs(d1v + d2v) < 1e-3: same += 1
        else: diff += 1; print('  differ', t + 1, h1, h2, round(d1v, 3), round(d2v, 3))
    print('track', t + 1, 'same', same, 'differ', diff, 'no ground', miss)
