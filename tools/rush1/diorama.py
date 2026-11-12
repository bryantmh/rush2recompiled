"""SF Rush (N64) track select dioramas (asset 10: TRK1O2 ... TRK7O5), as the Rush 1 port shows them
(src/rush1/track1_convert.cpp extend_menu_container; docs/sfrush_ps1.md#track-select-diorama).

  python diorama.py list                 every model in asset 10
  python diorama.py dump MODEL           the model's LOD 0 list, flattened (F3DEX), with its texture loads and vertex
                                         color statistics
  python diorama.py png MODEL OUTDIR     each texture the model loads as a PNG (CI4/CI8 through its TLUT, RGBA16, IA)

RUSH1_ROM points at the ROM (r1.py).
"""
import os, struct, sys
from collections import Counter
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from r1 import Rush1

ASSET = 10


class Container:
    def __init__(self, d):
        self.d = d
        self.seg = struct.unpack_from('>I', d, 0)[0] >> 24
        self.n = struct.unpack_from('>I', d, 4)[0]
        self.names_at = self.off(struct.unpack_from('>I', d, 0)[0])

    def off(self, a):
        return a & 0xFFFFFF

    def name(self, i):
        b = self.d[self.names_at + i * 0x18:self.names_at + i * 0x18 + 16]
        return b.split(b'\0')[0].decode('latin1')

    def find(self, name):
        for i in range(self.n):
            if self.name(i) == name:
                return i
        raise SystemExit('no model ' + name)

    def lod_list(self, i, l=0):
        o = 0x20 + i * 0x34 + 4 + l * 12
        dl = struct.unpack_from('>I', self.d, o + 8)[0]
        return self.off(dl) if dl else 0

    def cmd(self, o):
        return struct.unpack_from('>II', self.d, o)

    def walk(self, o, depth=0):
        """Yields (offset, w0, w1) of the flattened list (G_DL followed, BRANCH_Z taken)."""
        for _ in range(65536):
            w0, w1 = self.cmd(o); op = w0 >> 24
            if op == 0xB8:
                return
            if op == 0x06:
                yield from self.walk(self.off(w1), depth + 1)
                if (w0 >> 16) & 0xFF == 1:
                    return
                o += 8; continue
            if op == 0xB4:
                yield from self.walk(self.off(w1), depth + 1)
                return
            yield o, w0, w1
            o += 8


def loads(c, i):
    """[(settimg w0, address offset, kind, texel/entry count, uses)] and vertex color stats for model i."""
    out = []; timg = None; colors = Counter(); nverts = 0; tris = 0
    for o, w0, w1 in c.walk(c.lod_list(i)):
        op = w0 >> 24
        if op == 0xFD:
            timg = (w0, c.off(w1))
        elif op == 0xF0 and timg:      # LOADTLUT: count in w1 bits 14-23
            out.append(('tlut', timg[1], ((w1 >> 14) & 0x3FF) + 1, timg[0]))
        elif op == 0xF3 and timg:      # LOADBLOCK: texel count - 1 in w1 bits 12-23
            out.append(('block', timg[1], ((w1 >> 12) & 0xFFF) + 1, timg[0]))
        elif op == 0xF4 and timg:      # LOADTILE
            out.append(('tile', timg[1], (w0, w1), timg[0]))
        elif op == 0x04:
            n = (w0 >> 10) & 0x3F; a = c.off(w1); nverts += n
            for k in range(n):
                r, g, b, al = c.d[a + k * 16 + 12:a + k * 16 + 16]
                colors[(r, g, b)] += 1
        elif op in (0xBF,):
            tris += 1
        elif op == 0xB1:
            tris += 2
    return out, colors, nverts, tris


if __name__ == '__main__':
    r = Rush1(); c = Container(r.asset(ASSET))
    a = sys.argv[1:]
    if a[0] == 'list':
        for i in range(c.n):
            print(i, c.name(i))
    elif a[0] == 'dump':
        i = c.find(a[1])
        ops = Counter()
        for o, w0, w1 in c.walk(c.lod_list(i)):
            ops['%02X' % (w0 >> 24)] += 1
            if '-v' in a:
                print('%06X %08X %08X' % (o, w0, w1))
        print('ops', dict(ops))
        ld, colors, nv, tris = loads(c, i)
        print('vertices', nv, 'triangles', tris, 'distinct colors', len(colors))
        for col, n in colors.most_common(15):
            print('   ', col, n)
        seen = set()
        for kind, addr, n, timg in ld:
            if (kind, addr) in seen:
                continue
            seen.add((kind, addr))
            fmt, siz = (timg >> 21) & 7, (timg >> 19) & 3
            print('  %-5s at %06X fmt %d siz %d count %s' % (kind, addr, fmt, siz, n))
