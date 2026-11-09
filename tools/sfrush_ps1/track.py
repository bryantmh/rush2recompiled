"""PS1 SF Rush track geometry (research reader and viewer, docs/sfrush_ps1.md#track-geometry).

T<n>ROAD/T<n>ROAD2.DAT (road) and T<n>SCN2.DAT (scenery) are chunks (.SIZ: s32 sizes) of:
  +0x00 s32 1, +0x04 s32 origin x, y, z (1/256 ft), +0x10 u32 vertex count, +0x14 u32 face count,
  +0x18 / +0x1C RAM pointers (rewritten by the loader func_800369B8), +0x20 vertices (3 x s32, 16.16 ft),
  then faces (0x28 bytes):
  +0x00 u8[3] flags, +0x03 s8 CLUT index (into 0x800A7B08), +0x04 u16[4] vertex indices (a ring, not PS1's zigzag),
  +0x0C (u16 v, u16 u) x4, +0x1C u32 texture (an entry of the per-track texture table, vram.py), +0x20 u16[4]
  (+0x22 kept by the loader).
PS1 axes: x and y across the ground, z up?; feet in the game's frame: (game x, y, z) = (ps1 y, ps1 z, ps1 x).

  python track.py glb TRACK OUT.glb [--road-only] [--scenery-only]   textured glb (glTF meters) for tools/glb_view.py
"""
import io, math, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
from ps1 import Disc
from disps1 import Exe
import vram

FT_PER_M = 3.2808399


def chunk_mesh(c):
    one, ox, oy, oz, nv, nf = struct.unpack_from('<4i2I', c, 0)
    verts = []
    for i in range(nv):
        x, y, z = struct.unpack_from('<3i', c, 0x20 + 12 * i)
        verts.append((ox / 256 + x / 65536, oy / 256 + y / 65536, oz / 256 + z / 65536))
    faces = []
    fo = 0x20 + 12 * nv
    for i in range(nf):
        f = c[fo + 0x28 * i:fo + 0x28 * (i + 1)]
        idx = struct.unpack_from('<4H', f, 4)
        vu = struct.unpack_from('<8H', f, 0xC)
        uv = [(vu[2 * k + 1], vu[2 * k]) for k in range(4)]
        faces.append(dict(flags=f[0] | f[1] << 8 | f[2] << 16, clut=struct.unpack_from('<b', f, 3)[0], idx=idx, uv=uv,
                          tex=struct.unpack_from('<I', f, 0x1C)[0], tail=struct.unpack_from('<4H', f, 0x20)))
    return verts, faces


def track_meshes(disc, track, kinds=('ROAD2', 'SCN2')):
    out = []
    for k in kinds:
        name = 'T%dROAD/T%d%s' % (track, track, k)
        for i, c in enumerate(disc.chunks(name)):
            out.append(('%s_%d' % (k, i),) + chunk_mesh(c))
    return out


def game_pos(p):
    return (p[1], p[2], p[0])


def to_gltf(p):
    g = game_pos(p)
    return (-g[0] / FT_PER_M, g[1] / FT_PER_M, g[2] / FT_PER_M)


def build_glb(track, out, kinds):
    from stock_track_glb import Glb
    from PIL import Image
    disc = Disc(); e = Exe()
    vr = vram.Vram(disc, e, track)
    table = vram.tex_table(e, track); cluts = vram.clut_ids(e)
    g = Glb()
    images = {}

    def material(tex, clut):
        tx, ty, _, voff = table[tex][:4]
        base = (tx & ~63, ty & 256)
        cid = cluts[clut] if 0 <= clut < len(cluts) else 0
        key = (base, cid)
        if key not in images:
            img = Image.new('RGBA', (256, 256)); px = img.load()
            for v in range(256):
                for u in range(256):
                    px[u, v] = vr.texel4(base[0], base[1], cid, u, v)
            b = io.BytesIO(); img.save(b, 'PNG')
            images[key] = g.material('p%d_%d_c%d' % (base[0], base[1], cid), g.image('p', b.getvalue()), 'MASK', True)
        return images[key], voff

    for name, verts, faces in track_meshes(disc, track, kinds):
        prims = {}
        for f in faces:
            m, voff = material(f['tex'], f['clut'])
            pr = prims.setdefault(m, ([], [], [], []))
            ring = [k for k in range(4) if k == 0 or f['idx'][k] != f['idx'][k - 1]]
            if len(ring) == 4 and f['idx'][3] == f['idx'][0]:
                ring = ring[:3]
            base = len(pr[0])
            for k in ring:
                pr[0].append(to_gltf(verts[f['idx'][k]]))
                u, v = f['uv'][k]
                pr[2].append(((u + 0.5) / 256, (v + voff + 0.5) / 256))
            for a in range(1, len(ring) - 1):
                pr[3].append((base, base + a + 1, base + a))   # mirrored axes flip the winding
        g.textured_mesh(name, [(m, p[0], [(0, 1, 0)] * len(p[0]), p[2], None, p[3]) for m, p in prims.items()])
    open(out, 'wb').write(g.bytes())


if __name__ == '__main__':
    if sys.argv[1] == 'glb':
        kinds = ('ROAD2',) if '--road-only' in sys.argv else ('SCN2',) if '--scenery-only' in sys.argv else ('ROAD2', 'SCN2')
        build_glb(int(sys.argv[2]), sys.argv[3], kinds)
