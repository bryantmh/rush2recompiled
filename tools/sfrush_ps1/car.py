"""PS1 SF Rush car models (CARS/*.PSV, EXTRA/UFO.PSV; research reader, docs/sfrush_ps1.md#cars).

PSV: +0x38 u32 vertex count, face count, vertex offset, face offset. Vertices 3 x s32 (16.16, 1/64 ft?). Faces:
u32 flags, u32 n (3 or 4), u32 index[n], (u32 u, u32 v) << 16 x n, u32 texture, u32 [n] (normals?).
Texture 2 is EXTRA/WHEELS.GFX (the underside and rims); the car's own index is its CARSGFX/<name>01.GFX.

  python car.py glb NAME OUT.glb         a car as a glb (glTF meters, y up) for tools/glb_view.py
  python car.py stats
  python car.py size                    the PS1 cars' size against Rush 2's: SF Rush's eight cars are in both games
                                        (Rush 2 types 4-11 are the N64 SF Rush's), so each PS1 model's length at
                                        car_load's scale (12 / 640 ft a unit) against the same car's in Rush 2 gives
                                        the scale src/sfrush_ps1/ps1_cars.cpp draws the PS1 cars at (car_size).
                                        Needs the Rush 2 ROM (tools/rush2049/roms.py).
"""
import io, os, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps1 import Disc
import vram

# The PS1-only cars: model, texture, name.
CARS = {
    'STARSKY': ('CARS/STARSKY.PSV', 'CARSGFX/HUTCH01.GFX'),
    'JEEP': ('CARS/JEEP.PSV', 'CARSGFX/JEP01.GFX'),
    'JETCAR': ('CARS/JETCAR.PSV', 'CARSGFX/JET01.GFX'),
    'MINI': ('CARS/MINI.PSV', 'CARSGFX/MINI01.GFX'),
    'BBUS': ('CARS/BBUS.PSV', 'CARSGFX/BBUS.GFX'),
    'PIGGY': ('CARS/PIGGY.PSV', 'CARSGFX/PIGGY14.GFX'),
    'UFO': ('EXTRA/UFO.PSV', 'EXTRA/UFO.GFX'),
}


def psv(data):
    nv, nf, vo, fo = struct.unpack_from('<4I', data, 0x38)
    verts = [tuple(v / 65536 for v in struct.unpack_from('<3i', data, vo + 12 * i)) for i in range(nv)]
    faces = []; o = fo
    for i in range(nf):
        flags, n = struct.unpack_from('<2I', data, o)
        idx = struct.unpack_from('<%dI' % n, data, o + 8)
        uv = struct.unpack_from('<%dI' % (2 * n), data, o + 8 + 4 * n)
        tex = struct.unpack_from('<I', data, o + 8 + 12 * n)[0]
        faces.append(dict(flags=flags, idx=idx, uv=[(uv[2 * k] >> 16, uv[2 * k + 1] >> 16) for k in range(n)], tex=tex))
        o += 12 + 16 * n
    return verts, faces


def gfx_rgba(data):
    """A GFX file: 8 bytes, 16-color CLUT, 4-bit texels 256 wide. Returns (w, h, RGBA bytes)."""
    clut = struct.unpack_from('<16H', data, 8); px = data[40:]
    w = 256; h = len(px) * 2 // w
    out = bytearray()
    for y in range(h):
        for x in range(w):
            out += bytes(vram.rgba5551(clut[(px[(y * w + x) // 2] >> ((x & 1) * 4)) & 15]))
    return w, h, bytes(out)


def build_glb(name, out):
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
    from stock_track_glb import Glb
    from PIL import Image
    d = Disc()
    model, tex = CARS[name]
    verts, faces = psv(d.read(model))
    g = Glb(); mats = {}
    own = max(set(f['tex'] for f in faces) - {2}, default=2)
    for key, f in (('own', tex), ('wheels', 'EXTRA/WHEELS.GFX')):
        w, h, rgba = gfx_rgba(d.read(f))
        b = io.BytesIO(); Image.frombytes('RGBA', (w, h), rgba).save(b, 'PNG')
        mats[key] = (g.material(key, g.image(key, b.getvalue()), 'MASK', True), w, h)
    prims = {}
    S = 1 / 64 / 3.2808399
    for f in faces:
        m, w, h = mats['wheels' if f['tex'] == 2 else 'own']
        p = prims.setdefault(m, ([], [], []))
        base = len(p[0])
        for k, i in enumerate(f['idx']):
            x, y, z = verts[i]
            p[0].append((-y * S, z * S, -x * S))   # the PSV's front is -x; so mapped the lettering reads right
            u, v = f['uv'][k]
            p[1].append(((u + 0.5) / w, (v + 0.5) / h))
        for a in range(1, len(f['idx']) - 1):
            p[2].append((base, base + a, base + a + 1))   # front faces as the game culls (glb_view --cull 1)
    g.textured_mesh(name, [(m, p[0], [(0, 1, 0)] * len(p[0]), p[1], None, p[2]) for m, p in prims.items()])
    open(out, 'wb').write(g.bytes())


if __name__ == '__main__':
    if sys.argv[1] == 'glb':
        build_glb(sys.argv[2], sys.argv[3])
    elif sys.argv[1] == 'size':
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
        sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
        import car_mod
        d = Disc()
        pairs = [('BMW', 'BMW'), ('BUS', 'VWBUS'), ('CAM', 'CAMARO'), ('VIPER', 'VIPER'), ('SUPRA', 'SUPRA'),
                 ('BUG', 'VWBUG'), ('BUGETTI', 'BUGAT'), ('CONCEPT', 'CONCPT')]
        ratios = []
        for ps1, r2 in pairs:
            data = d.read('CARS/%s.PSV' % ps1)   # only its vertices (BMW.PSV has faces psv doesn't read)
            nv, _, vo, _ = struct.unpack_from('<4I', data, 0x38)
            verts = [struct.unpack_from('<3i', data, vo + 12 * i) for i in range(nv)]
            verts = [tuple(c / 65536 for c in v) for v in verts]
            length = (max(v[0] for v in verts) - min(v[0] for v in verts)) * 12 / 640
            tris = car_mod.rush2_car(r2).reshape(-1, 3) / 16
            r2_length = tris[:, 2].max() - tris[:, 2].min()
            ratios.append(r2_length / length)
            print('%-8s PS1 %.2f ft  Rush 2 %s %.2f ft  x%.4f' % (ps1, length, r2, r2_length, ratios[-1]))
        print('mean x%.4f' % (sum(ratios) / len(ratios)))
    elif sys.argv[1] == 'stats':
        d = Disc()
        for n, (model, tex) in CARS.items():
            verts, faces = psv(d.read(model))
            under = [verts[i] for f in faces if f['tex'] == 2 for i in f['idx']]
            zs = [v[2] for v in verts]
            print(n, 'z range', min(zs), max(zs), 'underside mean z', sum(v[2] for v in under) / max(len(under), 1))
