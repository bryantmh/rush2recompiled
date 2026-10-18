"""Reader for San Francisco Rush 2049 arcade (Special Edition, sf2049se) track data, as unpacked from the arcade
hard drive by ProjectR (%LOCALAPPDATA%\\ProjectR\\Data\\sf2049se). Formats: docs/arcade2049.md.

Every file is little endian and uncompressed. A track folder (TRACKS/TRACK<n>) holds:
  objects.rom   models (per object up to 4 LODs of vertices + triangles) and the texture descriptors
  textures.rom  texel data (3dfx Voodoo formats with mip chains) addressed by the descriptors
  worlds.rom    placed instances of objects, by object name
  dobs.rom      scripted ("dynamic") objects: name, a text script, a placement
  dpath.rom     the drones' path
  roaddat.rom   road / collision data

Usage: python arcade.py [TRACKn]   prints a summary of a track's files
"""
import os, struct, sys

DATA = os.environ.get('ARCADE2049_DATA', os.path.join(os.environ.get('LOCALAPPDATA', ''), 'ProjectR', 'Data', 'sf2049se'))

# Voodoo textureMode format field (bits 8-11)
FMT_YIQ422, FMT_AYIQ8422, FMT_RGB565 = 1, 9, 10


# The files use three axis orders for one world. World = the N64 port's frame (feet, y up), the frame
# tools/stock_track_glb.py's to_gltf takes: objects.rom vertices and worlds.rom placements are already in it;
# dpath.rom and roaddat.rom are (z, x, -y) of it; dobs.rom placements are (x, z, y).
def to_world_path(p):
    return (p[1], -p[2], p[0])


def to_world_dobs(p):
    return (p[0], p[2], p[1])


def _s9(v):
    return v - 512 if v & 256 else v


class Texture:
    """A texture descriptor (0x50 bytes in objects.rom): u8 first LOD, u8 last LOD (6: mips stop at 4 texels on the
    long side), u8 format, u8 0; u16 width, height; u32 offset into textures.rom; s32 LOD bias; u32 Voodoo
    textureMode; u32 tLOD; 8 bytes 0; the NCC table as Voodoo registers: Y[16] u8, I[4] and Q[4] u32 (each a packed
    signed 9-bit RGB: red bits 18-26, green 9-17, blue 0-8)."""

    def __init__(self, name, rec):
        self.name = name
        self.lod_min, self.lod_max, self.fmt = rec[0], rec[1], rec[2]
        self.w, self.h, self.offset = struct.unpack_from('<HHI', rec, 4)
        self.texture_mode, self.tlod = struct.unpack_from('<II', rec, 0x10)
        self.y = list(rec[0x20:0x30])
        unpack = lambda v: (_s9((v >> 18) & 511), _s9((v >> 9) & 511), _s9(v & 511))
        self.i = [unpack(struct.unpack_from('<I', rec, 0x30 + 4 * k)[0]) for k in range(4)]
        self.q = [unpack(struct.unpack_from('<I', rec, 0x40 + 4 * k)[0]) for k in range(4)]

    @property
    def clamp_s(self):
        return bool(self.texture_mode & 0x40)

    @property
    def clamp_t(self):
        return bool(self.texture_mode & 0x80)

    def rgba(self, texels):
        """Level 0 as w*h RGBA bytes (row 0 first). YIQ422: Y index bits 4-7, I bits 2-3, Q bits 0-1, color =
        Y + I + Q clamped; AYIQ8422 adds alpha in the high byte."""
        import numpy as np
        w, h = self.w, self.h
        if self.fmt == FMT_RGB565:
            v = np.frombuffer(texels, '<u2', w * h, self.offset).reshape(h, w).astype(np.int32)
            px = [((v >> 11) & 31) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31, np.full((h, w), 255)]
            return np.dstack(px).astype(np.uint8)
        if self.fmt == FMT_YIQ422:
            p = np.frombuffer(texels, np.uint8, w * h, self.offset).reshape(h, w).astype(np.int32)
            a = np.full((h, w), 255)
        elif self.fmt == FMT_AYIQ8422:
            v = np.frombuffer(texels, '<u2', w * h, self.offset).reshape(h, w).astype(np.int32)
            p, a = v & 255, v >> 8
        else:
            raise ValueError('%s: texture format %d' % (self.name, self.fmt))
        y, i, q = np.array(self.y), np.array(self.i), np.array(self.q)
        c = y[p >> 4][..., None] + i[(p >> 2) & 3] + q[p & 3]
        return np.dstack([np.clip(c, 0, 255), a]).astype(np.uint8)

    def uv_scale(self):
        """Voodoo s/t run 0-256 across the long side and 256 x aspect across the short one."""
        long = max(self.w, self.h)
        return 256.0 * self.w / long, 256.0 * self.h / long


class Lod:
    """An object LOD slot (0x20 bytes): f32 squared distance it is drawn to, u32 1, u32 vertex count, u32 vertex
    offset, u32 triangle count, u32 triangle offset, u32 0, s32 -1. Vertex (28 bytes): f32 x, y, z, f32 s, t (Voodoo
    texel space), u16 flags (0, 0xFFFF, 0xFFFE), u8 r, g, b, a, u16 0xDEAD. Triangle (8 bytes): u16 v0, v1, v2,
    u16 texture index."""

    def __init__(self, d, o):
        self.dist2, _, nv, ov, nt, ot = struct.unpack_from('<fIIIII', d, o)
        self.verts = []
        for k in range(nv):
            x, y, z, s, t, flags = struct.unpack_from('<5fH', d, ov + 28 * k)
            r, g, b, a = d[ov + 28 * k + 22:ov + 28 * k + 26]
            self.verts.append(((x, y, z), (s, t), flags, (r, g, b, a)))
        self.tris = [struct.unpack_from('<4H', d, ot + 8 * k) for k in range(nt)]


class Object:
    """An object header (0x8C bytes from 0x68): u32 0, f32 radius, u32 LOD count, 4 LOD slots."""

    def __init__(self, name, d, o):
        self.name = name
        _, self.radius, self.lod_count = struct.unpack_from('<IfI', d, o)
        self.lods = [Lod(d, o + 0xC + 0x20 * k) for k in range(max(1, min(4, self.lod_count)))]


class Track:
    def __init__(self, folder):
        self.folder = folder if os.path.isdir(folder) else os.path.join(DATA, 'TRACKS', folder)
        d = self.read('objects.rom')
        # Header at 0x50: u32 object count, u32 0, u32 object name table, u32 texture name table; 0x60: u32 object
        # count, u32 texture count, u32 0; object headers from 0x68, then the texture descriptors, then geometry.
        n_obj, _, obj_names, tex_names = struct.unpack_from('<4I', d, 0x50)
        n_tex = struct.unpack_from('<I', d, 0x64)[0]
        name = lambda o: d[o:o + 16].split(b'\0')[0].decode('latin1')
        # Name tables: objects 0x18 bytes (name[16], f32 radius, u32 index); textures 0x14 (name[16], u16 w, h).
        self.objects = [Object(name(obj_names + 0x18 * k), d, 0x68 + 0x8C * k) for k in range(n_obj)]
        tex_desc = 0x68 + 0x8C * n_obj
        self.textures = [Texture(name(tex_names + 0x14 * k), d[tex_desc + 0x50 * k:tex_desc + 0x50 * (k + 1)])
                         for k in range(n_tex)]
        self.by_name = {o.name: o for o in self.objects}
        self._texels = None

    def object_named(self, name):
        """The object a placement names: worlds.rom keeps 8 characters of a name and dobs.rom's names can be longer
        than the object table's 15; a flipbook's frames are <name>_1..n (the first is returned)."""
        if name in self.by_name:
            return self.by_name[name]
        for cut in (15, 8):
            if len(name) >= cut:
                hits = [o for o in self.objects if o.name.startswith(name[:cut]) and (cut == 15 or len(name) == 8)]
                if len(hits) == 1 or (hits and cut == 15):
                    return hits[0]
        return self.by_name.get(name + '_1')

    def read(self, name):
        with open(os.path.join(self.folder, name), 'rb') as f:
            return f.read()

    @property
    def texels(self):
        if self._texels is None:
            self._texels = self.read('textures.rom')
        return self._texels

    def worlds(self):
        """worlds.rom: u32 1, u32 0x18, a 16-byte root record (TEMP), then 0x4C-byte instances: char name[8] (the
        object's name cut to 8 characters, see object_named; bytes after its NUL are leftovers), u8 flags[8], f32 matrix[9], f32 position[3], u32 0,
        s16 next record (-1 ends), s16 -1, u32 0. Returns (name, flags bytes, matrix, position)."""
        w = self.read('worlds.rom')
        first = struct.unpack_from('<I', w, 4)[0]
        out = []
        for o in range(first, len(w) - 0x4B, 0x4C):
            out.append((w[o:o + 8].split(b'\0')[0].decode('latin1'), w[o + 8:o + 16],
                        struct.unpack_from('<9f', w, o + 16), struct.unpack_from('<3f', w, o + 52)))
        return out

    def dobs(self):
        """dobs.rom: u8 1, then records: u8 name length (with NUL), name, u16 script length, script (CRLF lines such
        as 'spinz 16', 'alpha 0', 'soundsphere prisonsiren 900 0.5', 'coin'), f32 matrix[9], f32 position[3].
        Returns (name, script lines, matrix, position)."""
        b = self.read('dobs.rom')
        out, o = [], 1
        while o < len(b):
            n = b[o]; o += 1
            if n == 0 or o + n > len(b):
                break
            nm = b[o:o + n].split(b'\0')[0].decode('latin1'); o += n
            sl = struct.unpack_from('<H', b, o)[0]; o += 2
            script = [ln.strip() for ln in b[o:o + sl].split(b'\0')[0].decode('latin1').splitlines() if ln.strip()]
            o += sl
            m, p = struct.unpack_from('<9f', b, o), struct.unpack_from('<3f', b, o + 36); o += 48
            out.append((nm, script, m, p))
        return out

    def collision(self, with_segment=False):
        """roaddat.rom, the source of the N64 port's collision files (tools/rush2049/collision.py) in its own layout:
        header u32 size, node count, polygon count, vertex count, segment count, leaf u16 count, vertex-list u16
        count; 0x1C: f32[3] + 0x18 bytes (not decoded); 0x40: nodes (0x24: s32 parent, s32 x0, x1, z0, z1 bounds,
        s32 child[4], negative = leaf); polygons (0x1C: u32 flags, u16 info, s16 m[9] (x 16384), u32 vertex-list
        offset in u16 units); vertices (s32 x, y, z, 1/40 ft); segments (0xA4, the road splines); leaves (u16);
        vertex lists (u16 count, u16 index...). As on N64, a polygon's vertex 0 is its origin and the others are in its
        local frame (local z = 0): point = v0 + M local, M's columns the local axes, minus column 2 the face normal. All in
        the path frame (see to_world_path). The 1/40 ft unit was fitted (it puts every polygon on the scenery, and
        arcade Marina's vertices on the N64 port's collision to 0.04 ft). Flags: bits 0-7 = the N64 port's u16 flags'
        low byte (0-3 surface type: 0 road, 1 dirt, 2 grass, 3 water, 5/6 wall, 7 kill; 4-7 wheel material; walls
        are 0x55 here, 0x05 on N64), 0x1000/0x2000 spline road; bits 16-31 the port dropped. A spline polygon's
        vertex list is followed by its u16 segment index (segments()).
        Returns (flags, [world xyz...], world normal) per polygon, plus the segment index (None unless 0x3000) with
        with_segment."""
        d = self.read('roaddat.rom')
        size, nn, npl, nv, ns, nleaf, nvl = struct.unpack_from('<7I', d, 0)
        po = 0x40 + nn * 0x24
        vo = po + npl * 0x1C
        vlo = size - nvl * 2
        verts = [struct.unpack_from('<3i', d, vo + 12 * k) for k in range(nv)]
        out = []
        for i in range(npl):
            flags, info = struct.unpack_from('<IH', d, po + 0x1C * i)
            m = [v / 16384.0 for v in struct.unpack_from('<9h', d, po + 0x1C * i + 6)]
            vl = vlo + 2 * struct.unpack_from('<I', d, po + 0x1C * i + 24)[0]
            cnt = struct.unpack_from('<H', d, vl)[0]
            idx = struct.unpack_from('<%dH' % cnt, d, vl + 2)
            if cnt < 3:
                continue
            o = [c / 40.0 for c in verts[idx[0]]]
            pts = [tuple(o)]
            for k in idx[1:]:
                l = [c / 40.0 for c in verts[k]]
                pts.append(tuple(o[a] + l[0] * m[3 * a] + l[1] * m[3 * a + 1] + l[2] * m[3 * a + 2] for a in range(3)))
            # The face (the side cars drive on, walls' track side) is against column 2: road faces then point up.
            poly = (flags, [to_world_path(p) for p in pts], to_world_path((-m[2], -m[5], -m[8])))
            if with_segment:
                seg = struct.unpack_from('<H', d, vl + 2 + 2 * cnt)[0] if flags & 0x3000 else None
                poly += (seg,)
            out.append(poly)
        return out

    def segments(self):
        """roaddat.rom's road spline segments (0xA4 bytes each, after the vertices) as Rush 2 / N64 2049 SEG records
        (33 floats, game space; tools/rush2049/collision.py seg_fields / seg_surface). The arcade record: f32 origin[3]
        and matrix[9] (path frame, the matrix stored transposed), across '30', edge along vectors '3C' / '48', then
        f32 54..78 as the N64's, 8 words not read here. Its across parameter spans twice the road (the N64's 0..1 is
        its 0.25..0.75), so the widths (54, 64..70) halve and the edge vectors and slopes (3C / 48, 74 / 78) are
        taken a quarter and three quarters across. Checked against arcade Marina's N64 port (file 139): every field of
        the segments both keep equal to float precision; on Alcatraz every spline polygon corner is within 0.05 ft of
        its segment's surface. The next segment is the following record, as on N64."""
        d = self.read('roaddat.rom')
        size, nn, npl, nv, ns = struct.unpack_from('<5I', d, 0)
        at = 0x40 + nn * 0x24 + npl * 0x1C + nv * 12
        q = lambda v: [v[1], -v[2], v[0]]       # path frame -> world (to_world_path, for directions)
        out = []
        for i in range(ns):
            a = struct.unpack_from('<31f', d, at + 0xA4 * i)
            m = [[a[3 + 3 * r + c] for c in range(3)] for r in range(3)]
            # world matrix = Q A^T Q^T, Q the path -> world axis swap (q): rows q(A's column 1), -q(column 2),
            # q(column 0).
            cols = [q([m[0][c], m[1][c], m[2][c]]) for c in range(3)]
            mat = [cols[1], [-x for x in cols[2]], cols[0]]

            def mix(f, g, w):
                v = [f[k] * (1 - w) + g[k] * w for k in range(3)]
                n = sum(x * x for x in v) ** 0.5 or 1.0
                return [x / n for x in v]
            v3c, v48 = a[15:18], a[18:21]
            out.append(q(a[0:3]) + [x for r in mat for x in r] + q(a[12:15]) + q(mix(v3c, v48, 0.25)) +
                       q(mix(v3c, v48, 0.75)) + [a[21] / 2, a[22], a[23], a[24], a[25] / 2, a[26] / 2, a[27] / 2,
                                                 a[28] / 2, a[29] * 0.75 + a[30] * 0.25, a[29] * 0.25 + a[30] * 0.75,
                                                 0.0, 0.0])
        return out

    def dpath_records(self):
        """dpath.rom: header (0x18: u32 lap point count, u32 1, u32, u16 count - 1, u16 count, 4 x u16), then 0x18-byte
        records until a zero one: f32 x, y, z (path frame), f32 speed (ft/s, 100-280), u32 steering behaviour (the
        N64 lanes' code byte: 2 follow the line, 5 damped, 1 blended, 3/4 variants, 0 default), u16 index, u16 link. The first `count` are the drones' lap in race order; later runs are other routes (on Alcatraz a
        route off the island). A large second structure follows (not decoded). Positions returned in world."""
        p = self.read('dpath.rom')
        out = []
        o = 0x18
        while o + 0x18 <= len(p):
            x, y, z, speed, kind, a, b = struct.unpack_from('<4fIHH', p, o)
            if kind == 0 and x == 0 and y == 0:
                break
            out.append((to_world_path((x, y, z)), speed, kind, a, b))
            o += 0x18
        return out

    def path_tables(self):
        """dpath.rom is 8 tables of 2000 0x18-byte slots (384,000 bytes); slot 0 of each is its header (u32 lap point
        count, u32 1 or 0, ...), the rest records as dpath_records describes. Table 0 is the centre line, tables 1-7
        the drones' racing lines (on most tracks all seven within 10-17 ft of it; on Alcatraz only 1 and 2, the others
        stale data far off the course). Returns per table (lap point count, [(world xyz, speed ft/s, behaviour, index,
        link)...]) with every record up to the first empty one."""
        p = self.read('dpath.rom')
        out = []
        for k in range(len(p) // 48000):
            base = k * 48000
            n = struct.unpack_from('<I', p, base)[0]
            recs = []
            for i in range(1, 2000):
                x, y, z, speed, kind, a, b = struct.unpack_from('<4fIHH', p, base + 0x18 * i)
                if not (x or y or z) and kind == 0:
                    break
                recs.append((to_world_path((x, y, z)), speed, kind, a, b))
            out.append((n, recs))
        return out

    def dpath(self):
        """The drones' lap, world coordinates."""
        return self.lap()

    def lap(self):
        n = struct.unpack_from('<I', self.read('dpath.rom'), 0)[0]
        return [r[0] for r in self.dpath_records()[:n]]


def main():
    t = Track(sys.argv[1] if len(sys.argv) > 1 else 'TRACK4')
    import collections
    print('%d objects, %d textures' % (len(t.objects), len(t.textures)))
    print('texture formats', collections.Counter(x.fmt for x in t.textures))
    print('worlds %d, dobs %d, dpath %d' % (len(t.worlds()), len(t.dobs()), len(t.dpath())))
    print('dobs scripts', collections.Counter(s[0] if s else '' for _, s, _, _ in t.dobs()))
    print('triangles at LOD 0: %d' % sum(len(o.lods[0].tris) for o in t.objects))


if __name__ == '__main__':
    main()
