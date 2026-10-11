"""Builds a car model mod (docs/car_mods.md): each Rush 2 car replaced by one mesh of any detail, drawn by the game in
place of the car's panels and dented like a Rush 2049 car's body (src/car_mods.cpp).

    python tools/car_mod.py build MAPPING.json OUT.rtz [--preview PREVIEW.png]
    python tools/car_mod.py textures MESH.dae TEXTURE_DIR      (which texture each material's UVs fit best)
    python tools/car_mod.py preview MAPPING.json PREVIEW.png    (side and top views next to the Rush 2 cars)
    python tools/car_mod.py overlay MAPPING.json OUT.png [CAR...] (the mod over the Rush 2 car and its wheels)

MAPPING.json:
    {"id": "my_cars", "name": "My Cars", "author": "...", "description": "...",
     "cars": {"PICKUP": {"meshes": ["Pickup.dae", "Pickup_Undertray.dae"],
                         "textures": {"*": "stock_base (9).png", "Undertray": "undertray.png"},
                         "paint": "auto"}, ...},
     "base": "folder the paths are relative to"}
Car keys are Rush 2's part prefixes (PICKUP, INTEG, VETTE, SLED, BMW, CAMARO, ...: tools/rush2049/roms.py CARS).
"textures" maps a material name, node name or mesh file name (without extension), or "*" for the rest, to an image
or a color ("#203040"); without one the image the mesh names is used.
"paint": "auto" makes the paint mask from the texture (light, unsaturated texels take MAIN COLOR), "none" leaves the
car unpainted, or an image path (red = MAIN COLOR, green = ACCENT COLOR). "flip": ["front"] / ["up"] overrides the
orientation the tool picks; "fit": "axes" (default: each axis scaled to the Rush 2 car's) or "uniform".

Meshes can be Collada (.dae) or glTF binary (.glb). The output space is the model dump's (docs/asset_catalog.md,
Model dump): vertex units (1/16 ft), y up, +z the front, x negated (glTF's right-handed space), so a car made in
Blender against a dumped car's .glb lines up. Needs the Rush 2 ROM (RUSH2_ROM, tools/rush2049/roms.py) to fit the
meshes to the cars they replace.
"""
import argparse, io, json, math, os, struct, sys, zipfile
import xml.etree.ElementTree as ET
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rush1'))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rush2049'))

NS = {'c': 'http://www.collada.org/2005/11/COLLADASchema'}


class Mesh:
    """One material's triangles: positions (N, 3), normals (N, 3), uvs (N, 2, glTF's: v down), tris (M, 3)."""
    def __init__(self, name, material, image, positions, normals, uvs, tris):
        self.name, self.material, self.image = name, material, image
        self.positions, self.normals, self.uvs, self.tris = positions, normals, uvs, tris
        self.source = name


# ---- Collada ----------------------------------------------------------------------------------------------------
def _floats(el):
    return np.array([float(x) for x in (el.text or '').split()], dtype=np.float64)


def load_dae(path):
    root = ET.parse(path).getroot()
    sources = {}
    for s in root.iter('{%s}source' % NS['c']):
        fa = s.find('c:float_array', NS)
        acc = s.find('.//c:accessor', NS)
        if fa is None or acc is None:
            continue
        stride = int(acc.get('stride', '1'))
        a = _floats(fa)
        sources[s.get('id')] = a.reshape(-1, stride) if stride > 1 and len(a) % stride == 0 else a.reshape(-1, 1)
    vertices = {}
    for v in root.iter('{%s}vertices' % NS['c']):
        for i in v.findall('c:input', NS):
            if i.get('semantic') == 'POSITION':
                vertices[v.get('id')] = i.get('source')[1:]
    # material id -> image file, through effect -> sampler -> surface -> image
    images = {i.get('id'): (i.find('c:init_from', NS).text or '') for i in root.iter('{%s}image' % NS['c'])
              if i.find('c:init_from', NS) is not None}
    effect_image = {}
    for e in root.iter('{%s}effect' % NS['c']):
        surf = {}
        for p in e.iter('{%s}newparam' % NS['c']):
            s = p.find('c:surface/c:init_from', NS)
            if s is not None:
                surf[p.get('sid')] = s.text
            s2 = p.find('c:sampler2D/c:source', NS)
            if s2 is not None:
                surf[p.get('sid')] = surf.get(s2.text, s2.text)
        t = e.find('.//c:diffuse/c:texture', NS)
        if t is not None:
            img = surf.get(t.get('texture'), t.get('texture'))
            effect_image[e.get('id')] = images.get(img, img)
    material_image, material_name = {}, {}
    for m in root.iter('{%s}material' % NS['c']):
        ie = m.find('c:instance_effect', NS)
        if ie is not None:
            material_image[m.get('id')] = effect_image.get(ie.get('url')[1:])
        material_name[m.get('id')] = m.get('name') or m.get('id')
    geometries = {g.get('id'): g for g in root.iter('{%s}geometry' % NS['c'])}

    meshes = []

    def node(n, parent):
        m = parent
        for child in n:
            tag = child.tag.split('}')[1]
            if tag == 'matrix':
                m = m @ _floats(child).reshape(4, 4)
            elif tag == 'translate':
                t = np.eye(4); t[:3, 3] = _floats(child); m = m @ t
            elif tag == 'scale':
                s = np.eye(4); s[0, 0], s[1, 1], s[2, 2] = _floats(child); m = m @ s
            elif tag == 'rotate':
                ax, ay, az, deg = _floats(child)
                a = math.radians(deg); c, s = math.cos(a), math.sin(a)
                u = np.array([ax, ay, az]); u = u / (np.linalg.norm(u) or 1)
                r = np.eye(4)
                r[:3, :3] = c * np.eye(3) + s * np.array([[0, -u[2], u[1]], [u[2], 0, -u[0]], [-u[1], u[0], 0]]) + (1 - c) * np.outer(u, u)
                m = m @ r
        for child in n:
            tag = child.tag.split('}')[1]
            if tag == 'instance_geometry':
                bind = {}
                for im in child.iter('{%s}instance_material' % NS['c']):
                    bind[im.get('symbol')] = im.get('target')[1:]
                g = geometries.get(child.get('url')[1:])
                if g is not None:
                    meshes.extend(_geometry(g, m, bind, sources, vertices, material_image, material_name,
                                            n.get('name') or g.get('name')))
            elif tag == 'node':
                node(child, m)

    for scene in root.iter('{%s}visual_scene' % NS['c']):
        for n in scene.findall('c:node', NS):
            node(n, np.eye(4))
    return meshes


def _geometry(g, matrix, bind, sources, vertices, material_image, material_name, name):
    out = []
    mesh = g.find('c:mesh', NS)
    if mesh is None:
        return out
    normal_matrix = np.linalg.inv(matrix[:3, :3]).T
    for prim in list(mesh.findall('c:triangles', NS)) + list(mesh.findall('c:polylist', NS)) + list(mesh.findall('c:polygons', NS)):
        inputs = prim.findall('c:input', NS)
        stride = max(int(i.get('offset')) for i in inputs) + 1
        p = prim.find('c:p', NS)
        if p is None:
            continue
        idx = np.array([int(x) for x in p.text.split()], dtype=np.int64).reshape(-1, stride)
        if prim.tag.endswith('polylist'):
            vc = [int(x) for x in prim.find('c:vcount', NS).text.split()]
            corners, at = [], 0
            for n in vc:
                for k in range(1, n - 1):
                    corners += [at, at + k, at + k + 1]
                at += n
            idx = idx[corners]
        sem = {}
        for i in inputs:
            s = i.get('semantic')
            if s == 'TEXCOORD' and i.get('set', '0') not in ('0', None) and 'TEXCOORD' in sem:
                continue
            src = i.get('source')[1:]
            if s == 'VERTEX':
                src = vertices.get(src, src)
            sem[s] = (int(i.get('offset')), sources.get(src))
        if 'VERTEX' not in sem or sem['VERTEX'][1] is None:
            continue
        # One output vertex per distinct index tuple.
        keys, inverse = np.unique(idx, axis=0, return_inverse=True)
        inverse = inverse.reshape(-1)
        pos = sem['VERTEX'][1][keys[:, sem['VERTEX'][0]], :3]
        pos = (np.c_[pos, np.ones(len(pos))] @ matrix.T)[:, :3]
        if 'NORMAL' in sem and sem['NORMAL'][1] is not None:
            nrm = sem['NORMAL'][1][keys[:, sem['NORMAL'][0]], :3] @ normal_matrix.T
            nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
        else:
            nrm = np.zeros_like(pos)
        if 'TEXCOORD' in sem and sem['TEXCOORD'][1] is not None:
            uv = sem['TEXCOORD'][1][keys[:, sem['TEXCOORD'][0]], :2].copy()
            uv[:, 1] = 1.0 - uv[:, 1] # Collada's v points up
        else:
            uv = np.zeros((len(pos), 2))
        tris = inverse.reshape(-1, 3)
        if np.linalg.det(matrix[:3, :3]) < 0:
            tris = tris[:, ::-1]
        mat = bind.get(prim.get('material'), prim.get('material'))
        image = material_image.get(mat)
        out.append(Mesh(name, material_name.get(mat, mat or ''), image, pos, nrm, uv, tris.copy()))
    return out


# ---- glTF binary ------------------------------------------------------------------------------------------------
COMPONENT = {5120: np.int8, 5121: np.uint8, 5122: np.int16, 5123: np.uint16, 5125: np.uint32, 5126: np.float32}
WIDTH = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}


def read_glb(path):
    data = open(path, 'rb').read()
    magic, version, length = struct.unpack_from('<III', data, 0)
    assert magic == 0x46546C67, path + ': not a glb'
    o, js, binary = 12, None, b''
    while o < length:
        n, kind = struct.unpack_from('<II', data, o)
        chunk = data[o + 8:o + 8 + n]
        if kind == 0x4E4F534A:
            js = json.loads(chunk)
        elif kind == 0x004E4942:
            binary = chunk
        o += 8 + n
    return js, binary


def _accessor(js, binary, i):
    a = js['accessors'][i]
    v = js['bufferViews'][a['bufferView']]
    dtype = np.dtype(COMPONENT[a['componentType']])
    w = WIDTH[a['type']]
    start = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = v.get('byteStride', 0) or dtype.itemsize * w
    out = np.zeros((a['count'], w), dtype=np.float64)
    for k in range(a['count']):
        out[k] = np.frombuffer(binary, dtype=dtype, count=w, offset=start + k * stride)
    if a.get('normalized') and dtype.kind in 'iu':
        out /= float(np.iinfo(dtype).max)
    return out


def load_glb(path):
    js, binary = read_glb(path)
    meshes = []

    def image_name(tex):
        img = js['images'][js['textures'][tex]['source']]
        return img.get('uri') or img.get('name') or ('image%d' % js['textures'][tex]['source'])

    def node(i, parent):
        n = js['nodes'][i]
        m = np.eye(4)
        if 'matrix' in n:
            m = np.array(n['matrix']).reshape(4, 4).T
        else:
            t = np.eye(4); t[:3, 3] = n.get('translation', [0, 0, 0])
            x, y, z, w = n.get('rotation', [0, 0, 0, 1])
            r = np.eye(4)
            r[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
                         [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
                         [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]]
            s = np.diag(list(n.get('scale', [1, 1, 1])) + [1])
            m = t @ r @ s
        m = parent @ m
        if 'mesh' in n:
            for p in js['meshes'][n['mesh']]['primitives']:
                if p.get('mode', 4) != 4:
                    continue
                at = p['attributes']
                pos = _accessor(js, binary, at['POSITION'])[:, :3]
                pos = (np.c_[pos, np.ones(len(pos))] @ m.T)[:, :3]
                nrm = _accessor(js, binary, at['NORMAL'])[:, :3] @ np.linalg.inv(m[:3, :3]) if 'NORMAL' in at else np.zeros_like(pos)
                uv = _accessor(js, binary, at['TEXCOORD_0'])[:, :2] if 'TEXCOORD_0' in at else np.zeros((len(pos), 2))
                tris = (_accessor(js, binary, p['indices'])[:, 0].astype(np.int64) if 'indices' in p
                        else np.arange(len(pos))).reshape(-1, 3)
                if np.linalg.det(m[:3, :3]) < 0:
                    tris = tris[:, ::-1]
                mat = js['materials'][p['material']] if 'material' in p else {}
                tex = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture')
                meshes.append(Mesh(n.get('name', ''), mat.get('name', ''), image_name(tex['index']) if tex else None,
                                   pos, nrm, uv, tris))
        for c in n.get('children', []):
            node(c, m)

    for i in js['scenes'][js.get('scene', 0)]['nodes']:
        node(i, np.eye(4))
    return meshes


def load_mesh(path):
    return load_glb(path) if path.lower().endswith('.glb') else load_dae(path)


# ---- the Rush 2 car a mesh replaces --------------------------------------------------------------------------------
INTACT = ('FRAME1', 'D0_FL1', 'D0_FR1', 'D0_RL1', 'D0_RR1', 'D0_TOP1', 'H0_L', 'H0_R')


def rush2_car(name):
    """The intact car's triangles, in the output space (vertex units, x negated): (M, 3, 3)."""
    import cartex
    from roms import Rush2
    r2 = Rush2()
    d = r2.asset(0x1D + Rush2.CARS.index(name))
    tris = []
    for model, ts in cartex.meshes(d, 2).items():
        if model.startswith(name) and model[len(name):] in INTACT:
            tris += [[(-v[0], v[1], v[2]) for v in t] for t, _ in ts]
    return np.array(tris, dtype=np.float64)


def rush2_wheels(name):
    """Where Rush 2 draws the car's four wheels, in the output space: [(x, y, z, radius)] front right, front left,
    rear right, rear left. The wheel draw (carselect_preview_update_8005A598) puts each at the physics descriptor's
    wheel point (descriptor +0x7C, 0x800C07BC[0x800C07E8[type]], feet) plus the tire radius (wheel struct +0x424:
    1 ft) in y, x 16 units per foot; the wheel model (WHEELWHEEL, asset 0x12) is 15 units round, scaled by the type's
    wheel scale (0x800C0A6C front, 0x800C0B74 rear). The FORMULA's rear wheels are drawn at z -85.25
    (rush2_fix_formula_rear_wheels). x is negated like the rest of the output space."""
    from roms import Rush2
    r2 = Rush2()
    t = Rush2.CARS.index(name)
    desc = r2.w(0x800C07BC + r2.read(0x800C07E8 + t, 1)[0] * 4)
    out = []
    for k in range(4):
        x, y, z = (r2.f(desc + 0x7C + k * 12 + j * 4) for j in range(3))
        scale = r2.f((0x800C0A6C if k < 2 else 0x800C0B74) + t * 4)
        z = -85.25 / 16 if name == 'FORM1' and k >= 2 else z
        out.append((-x * 16, (y + 1.0) * 16, z * 16, 15.0 * scale))
    return out


def surface_points(tris, n=6000, seed=1):
    """Points spread over the triangles by area."""
    a, b, c = tris[:, 0], tris[:, 1], tris[:, 2]
    area = np.linalg.norm(np.cross(b - a, c - a), axis=1) / 2
    if area.sum() <= 0:
        return tris.reshape(-1, 3)
    rng = np.random.default_rng(seed)
    pick = rng.choice(len(tris), size=n, p=area / area.sum())
    u, v = rng.random(n), rng.random(n)
    flip = u + v > 1
    u[flip], v[flip] = 1 - u[flip], 1 - v[flip]
    return a[pick] + (b[pick] - a[pick]) * u[:, None] + (c[pick] - a[pick]) * v[:, None]


def rotations():
    """The 24 rotations that map axes onto axes."""
    out = []
    for perm in ((0, 1, 2), (0, 2, 1), (1, 0, 2), (1, 2, 0), (2, 0, 1), (2, 1, 0)):
        for sx in (1, -1):
            for sy in (1, -1):
                for sz in (1, -1):
                    r = np.zeros((3, 3))
                    r[0, perm[0]], r[1, perm[1]], r[2, perm[2]] = sx, sy, sz
                    if np.linalg.det(r) > 0:
                        out.append(r)
    return out


def fit_box(points, lo, hi, uniform=False):
    """Scale and offset taking points' bounding box onto [lo, hi] (per axis, or by the length alone)."""
    plo, phi = points.min(0), points.max(0)
    scale = (hi - lo) / np.maximum(phi - plo, 1e-9)
    if uniform:
        scale[:] = scale[2]
    offset = (lo + hi) / 2 - (plo + phi) / 2 * scale
    return scale, offset


def occupancy(points, lo, hi, grid=(10, 8, 20)):
    g = np.array(grid)
    cells = np.clip(((points - lo) / np.maximum(hi - lo, 1e-9) * g).astype(int), 0, g - 1)
    occ = np.zeros(grid, dtype=bool)
    occ[cells[:, 0], cells[:, 1], cells[:, 2]] = True
    return occ


def orient(meshes, ref, flips=(), uniform=False):
    """The rotation (no mirroring) and fit that best lay the meshes over the Rush 2 car: the longest axis becomes the
    length, and of the ways round that leaves, the one whose shape covers the Rush 2 car's best."""
    tris = np.concatenate([m.positions[m.tris] for m in meshes])
    pts = surface_points(tris)
    rpts = surface_points(ref)
    lo, hi = ref.reshape(-1, 3).min(0), ref.reshape(-1, 3).max(0)
    ref_occ = occupancy(rpts, lo, hi)
    ext = pts.max(0) - pts.min(0)
    best = None
    for r in rotations():
        e = np.abs(r) @ ext
        if e[2] < max(e[0], e[1]):
            continue # the length has to be the longest
        p = pts @ r.T
        s, o = fit_box(p, lo, hi, uniform)
        occ = occupancy(p * s + o, lo, hi)
        score = (occ & ref_occ).sum() / max(1, (occ | ref_occ).sum())
        if best is None or score > best[0]:
            best = (score, r, s, o)
    score, r, s, o = best
    if 'front' in flips:
        r = np.diag([-1, 1, -1]) @ r
    if 'up' in flips:
        r = np.diag([-1, -1, 1]) @ r
    if flips:
        s, o = fit_box(pts @ r.T, lo, hi, uniform)
    return r, s, o, score


# ---- textures -----------------------------------------------------------------------------------------------------
def uv_mask(meshes, size=64):
    im = Image.new('L', (size, size), 0)
    dr = ImageDraw.Draw(im)
    for m in meshes:
        for t in m.tris:
            uv = m.uvs[t] % 1.0 if (m.uvs[t].max() > 1.001 or m.uvs[t].min() < -0.001) else m.uvs[t]
            dr.polygon([(float(u) * size, float(v) * size) for u, v in uv], fill=255)
    return np.array(im) > 0


def content_mask(path, size=64):
    im = Image.open(path).convert('RGBA').resize((size, size), Image.BILINEAR)
    a = np.array(im).astype(np.float64)
    return (a[:, :, 3] > 32) & (a[:, :, :3].max(2) > 12)


def best_textures(meshes, folder, top=3):
    want = uv_mask(meshes)
    scores = []
    for f in sorted(os.listdir(folder)):
        if not f.lower().endswith(('.png', '.jpg', '.bmp', '.tga')):
            continue
        have = content_mask(os.path.join(folder, f))
        inside = (want & have).sum() / max(1, want.sum())        # the UVs land on painted texels
        spill = (have & ~want).sum() / max(1, have.sum())        # little of the image goes unused
        scores.append((inside - 0.5 * spill, inside, spill, f))
    scores.sort(reverse=True)
    return scores[:top]


def paint_mask(image):
    """Paint mask as the game reads it (red = MAIN COLOR weight, green = ACCENT COLOR weight): light, unsaturated
    texels (the white or grey body) take MAIN COLOR; glass, lights, trim and dark texels keep their color."""
    a = np.array(image.convert('RGB')).astype(np.float64) / 255.0
    mx, mn = a.max(2), a.min(2)
    sat = (mx - mn) / np.maximum(mx, 1e-6)
    w = np.clip((0.22 - sat) / 0.10, 0, 1) * np.clip((mx - 0.30) / 0.15, 0, 1)
    r = (w * 255).astype(np.uint8)
    return Image.fromarray(np.dstack([r, np.zeros_like(r), np.zeros_like(r)]), 'RGB')


def bleed(image, mask, uv_tris, steps=24):
    """Spreads the colors of the texels the UVs cover outward into the rest of the image (and the paint mask with
    them), `steps` texels: filtering and mipmaps sample past a triangle's edge, and the black between an atlas's
    islands would show as dark seams along every panel edge."""
    from PIL import ImageDraw
    w, h = image.size
    cov = Image.new('L', (w, h), 0)
    dr = ImageDraw.Draw(cov)
    for uv in uv_tris:
        if uv.min() < -0.01 or uv.max() > 1.01:
            uv = uv - np.floor(uv.min(0))
        dr.polygon([(float(u) * w, float(v) * h) for u, v in uv], fill=255, outline=255)
    known = np.array(cov) > 0
    layers = [np.array(image.convert('RGBA')).astype(np.float64)]
    if mask is not None:
        layers.append(np.array(mask.convert('RGB').resize((w, h))).astype(np.float64))
    for _ in range(steps):
        if known.all():
            break
        k = known.astype(np.float64)
        pad = lambda a: np.pad(a, [(1, 1), (1, 1)] + [(0, 0)] * (a.ndim - 2), mode='edge')
        kp = pad(k)
        count = sum(kp[1 + dy:h + 1 + dy, 1 + dx:w + 1 + dx] for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)))
        grow = (~known) & (count > 0)
        if not grow.any():
            break
        for i, layer in enumerate(layers):
            lp = pad(layer * k[..., None])
            total = sum(lp[1 + dy:h + 1 + dy, 1 + dx:w + 1 + dx] for dy, dx in ((-1, 0), (1, 0), (0, -1), (0, 1)))
            layer[grow] = total[grow] / count[grow][:, None]
        known = known | grow
    out = Image.fromarray(np.clip(layers[0], 0, 255).astype(np.uint8), 'RGBA')
    out_mask = Image.fromarray(np.clip(layers[1], 0, 255).astype(np.uint8), 'RGB') if mask is not None else None
    return out, out_mask


def mask_file(path):
    """A paint mask from a file: an RGB one as is (red MAIN, green ACCENT); a grayscale one is all MAIN."""
    m = Image.open(path)
    if m.mode in ('1', 'L', 'LA', 'I', 'I;16'):
        r = np.array(m.convert('L'))
        return Image.fromarray(np.dstack([r, np.zeros_like(r), np.zeros_like(r)]), 'RGB')
    return m.convert('RGB')


# ---- output -------------------------------------------------------------------------------------------------------
def pad4(b, fill=b'\0'):
    return b + fill * ((4 - len(b) % 4) % 4)


def png_bytes(image, max_size=2048):
    if max(image.size) > max_size:
        k = max_size / max(image.size)
        image = image.resize((max(1, int(image.width * k)), max(1, int(image.height * k))), Image.LANCZOS)
    buf = io.BytesIO()
    image.save(buf, 'PNG', optimize=True)
    return buf.getvalue()


def write_glb(parts, title):
    """parts: [(Mesh in the output space, base color Image, paint mask Image or None)] -> glb bytes. One mesh, a
    primitive per part; materials carry the paint mask as extras.rush2_paint (a texture index)."""
    binary = b''
    js = {'asset': {'version': '2.0', 'generator': 'rush2-recomp tools/car_mod.py'}, 'scene': 0,
          'scenes': [{'nodes': [0]}], 'nodes': [{'name': title, 'mesh': 0}], 'meshes': [{'name': title, 'primitives': []}],
          'accessors': [], 'bufferViews': [], 'buffers': [], 'materials': [], 'textures': [], 'images': [],
          'samplers': [{'magFilter': 9729, 'minFilter': 9987, 'wrapS': 10497, 'wrapT': 10497}]}

    def view(data, target=None):
        nonlocal binary
        binary = pad4(binary)
        js['bufferViews'].append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': len(data)})
        if target:
            js['bufferViews'][-1]['target'] = target
        binary += data
        return len(js['bufferViews']) - 1

    def accessor(arr, kind, ctype, target, minmax=False):
        a = {'bufferView': view(arr.tobytes(), target), 'componentType': ctype, 'count': len(arr), 'type': kind}
        if minmax:
            a['min'] = arr.min(0).tolist(); a['max'] = arr.max(0).tolist()
        js['accessors'].append(a)
        return len(js['accessors']) - 1

    images = {}

    def texture(image):
        key = id(image)
        if key not in images:
            js['images'].append({'bufferView': view(png_bytes(image)), 'mimeType': 'image/png'})
            js['textures'].append({'sampler': 0, 'source': len(js['images']) - 1})
            images[key] = len(js['textures']) - 1
        return images[key]

    for mesh, image, mask in parts:
        mat = {'name': mesh.material or 'body', 'pbrMetallicRoughness': {'metallicFactor': 0.0, 'roughnessFactor': 1.0}}
        if image is not None:
            mat['pbrMetallicRoughness']['baseColorTexture'] = {'index': texture(image)}
        if mask is not None:
            mat['extras'] = {'rush2_paint': texture(mask)}
        js['materials'].append(mat)
        prim = {'attributes': {
            'POSITION': accessor(mesh.positions.astype(np.float32), 'VEC3', 5126, 34962, True),
            'NORMAL': accessor(mesh.normals.astype(np.float32), 'VEC3', 5126, 34962),
            'TEXCOORD_0': accessor(mesh.uvs.astype(np.float32), 'VEC2', 5126, 34962)},
            'indices': accessor(mesh.tris.reshape(-1).astype(np.uint32), 'SCALAR', 5125, 34963),
            'material': len(js['materials']) - 1}
        js['meshes'][0]['primitives'].append(prim)
    js['buffers'].append({'byteLength': len(pad4(binary))})
    j = pad4(json.dumps(js, separators=(',', ':')).encode(), b' ')
    b = pad4(binary)
    return (struct.pack('<III', 0x46546C67, 2, 12 + 8 + len(j) + 8 + len(b)) + struct.pack('<II', len(j), 0x4E4F534A) + j +
            struct.pack('<II', len(b), 0x004E4942) + b)


def manifest(cfg):
    return json.dumps({'game_id': 'rush2', 'id': cfg['id'], 'display_name': cfg.get('name', cfg['id']),
                       'description': cfg.get('description', ''), 'short_description': cfg.get('name', cfg['id']),
                       'version': cfg.get('version', '1.0.0'), 'authors': [cfg.get('author', 'Unknown')],
                       'minimum_recomp_version': '0.0.0'}, indent=4)


def resolve(cfg, path):
    base = cfg.get('base', '.')
    return path if os.path.isabs(path) else os.path.join(base, path)


def box_iou(lo1, hi1, lo2, hi2):
    inter = np.prod(np.clip(np.minimum(hi1, hi2) - np.maximum(lo1, lo2), 0, None))
    union = np.prod(np.maximum(hi1 - lo1, 1e-6)) + np.prod(np.maximum(hi2 - lo2, 1e-6)) - inter
    return inter / union


def align_file(extra, body):
    """An extra file (an undertray, suspension) exported in another frame than the body's (a Blender node rotated
    the other way): rotated so its box best fits the body's, when its own fits poorly."""
    pts = np.concatenate([m.positions for m in extra])
    bpts = np.concatenate([m.positions for m in body])
    blo, bhi = bpts.min(0), bpts.max(0)
    # Which way is up isn't known yet (orient decides), but the body's thinnest axis is its height: the extra part
    # belongs where its footprint across the other two axes covers the body's, no taller than the body.
    up = int(np.argmin(bhi - blo))
    flat = [a for a in range(3) if a != up]
    def score(p):
        lo, hi = p.min(0), p.max(0)
        s = box_iou(lo[flat], hi[flat], blo[flat], bhi[flat])
        return s if hi[up] - lo[up] <= (bhi[up] - blo[up]) * 1.05 else s * 0.1
    margin = (bhi - blo) * 0.15
    if np.all(pts.min(0) >= blo - margin) and np.all(pts.max(0) <= bhi + margin):
        return # it sits within the body as it is
    best = max(rotations(), key=lambda r: score(pts @ r.T))
    for m in extra:
        m.positions = m.positions @ best.T
        m.normals = m.normals @ best.T


def convert_car(cfg, name, car, ref):
    """The car's parts in the output space, with their images and masks."""
    meshes = []
    for i, path in enumerate(car['meshes']):
        file_meshes = []
        for m in load_mesh(resolve(cfg, path)):
            m.source = os.path.splitext(os.path.basename(path))[0]
            file_meshes.append(m)
        if i > 0 and meshes:
            align_file(file_meshes, meshes)
        meshes += file_meshes
    meshes = [m for m in meshes if len(m.tris)]
    r, s, o, score = orient(meshes, ref, car.get('flip', ()), car.get('fit') == 'uniform')
    textures = car.get('textures', {})
    loaded = {}
    parts = []
    for m in meshes:
        p = (m.positions @ r.T) * s + o
        n = (m.normals @ r.T) / np.where(s == 0, 1, s)
        n /= np.maximum(np.linalg.norm(n, axis=1, keepdims=True), 1e-9)
        file = next((textures[k] for k in (m.material, m.name, m.source) if k in textures), None)
        if file is None and m.image and os.path.exists(resolve(cfg, os.path.basename(m.image.replace('%20', ' ')))):
            file = os.path.basename(m.image.replace('%20', ' '))
        if file is None:
            file = textures.get('*')
        image = mask = None
        if file:
            full = resolve(cfg, file)
            if full not in loaded:
                im = Image.new('RGBA', (8, 8), file) if file.startswith('#') else Image.open(full).convert('RGBA')
                paint = car.get('paint', 'auto')
                pm = car.get('paint_masks', {}).get(file, paint)
                mk = paint_mask(im) if pm == 'auto' else None if pm == 'none' else mask_file(resolve(cfg, pm))
                loaded[full] = (im, mk)
            image, mask = loaded[full]
        parts.append((Mesh(m.name, m.material, file, p, n, m.uvs, m.tris), image, mask))
    # Bleed each image past the edges of the triangles that use it (one image can serve several parts).
    users = {}
    for q in parts:
        if q[1] is not None and q[1].size[0] > 8:
            users.setdefault(id(q[1]), []).append(q)
    for qs in users.values():
        uv_tris = [q[0].uvs[t] for q in qs for t in q[0].tris]
        image, mask = bleed(qs[0][1], qs[0][2], uv_tris)
        for i, q in enumerate(parts):
            if any(q is u for u in qs):
                parts[i] = (q[0], image, mask)
    # Faces wound clockwise from outside (meshes saved left-handed, as Unity's are) would be culled as back faces:
    # a closed car's signed volume says which way it is wound.
    volume = sum(np.einsum('ij,ij->i', q[0].positions[q[0].tris][:, 0],
                           np.cross(q[0].positions[q[0].tris][:, 1], q[0].positions[q[0].tris][:, 2])).sum() for q in parts)
    if volume < 0:
        for q in parts:
            q[0].tris = q[0].tris[:, ::-1].copy()
    return parts, score


def build(cfg, out, preview=None):
    entries = {}
    previews = []
    for name, car in cfg['cars'].items():
        ref = rush2_car(name)
        parts, score = convert_car(cfg, name, car, ref)
        entries['rush2/cars/%s.glb' % name] = write_glb(parts, name)
        tris = sum(len(p[0].tris) for p in parts)
        print('%-8s %4d triangles, fit %.2f, %s' % (name, tris, score, ', '.join(sorted(set(str(p[0].image) for p in parts)))))
        previews.append((name, parts, ref))
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('mod.json', manifest(cfg))
        for path, data in entries.items():
            z.writestr(path, data)
    print('wrote', out)
    if preview:
        render_preview(previews, preview)


def silhouette(tris, axes, size, lo, hi):
    """Filled outline of triangles (M, 3, 3) looking down axis axes[2]: a bool image; axes[3] flips the horizontal."""
    from PIL import ImageDraw
    w, h = size
    ax, ay = axes[0], axes[1]
    k = min((w - 8) / max(hi[ax] - lo[ax], 1e-9), (h - 8) / max(hi[ay] - lo[ay], 1e-9))
    cx, cy = (lo[ax] + hi[ax]) / 2, (lo[ay] + hi[ay]) / 2
    im = Image.new('L', size, 0)
    dr = ImageDraw.Draw(im)
    for t in tris:
        dr.polygon([(w / 2 + (p[ax] - cx) * k * axes[3], h / 2 - (p[ay] - cy) * k) for p in t], fill=255)
    return np.array(im) > 0, (k, cx, cy)


def render_overlay(cars, path, cell=(560, 220)):
    """Per car: the Rush 2 car (red) and the mod (green) over each other, yellow where both are, from the side (front
    to the right, Rush 2's wheels drawn as circles), the top and the front. Agreement is the yellow share."""
    from PIL import ImageDraw
    rows = []
    for name, parts, ref in cars:
        mod = np.concatenate([q[0].positions[q[0].tris] for q in parts])
        allp = np.concatenate([ref.reshape(-1, 3), mod.reshape(-1, 3)])
        lo, hi = allp.min(0) - 4, allp.max(0) + 4
        wheels = rush2_wheels(name)
        lo[1] = min(lo[1], min(wy - wr for _, wy, _, wr in wheels) - 2)
        row = Image.new('RGB', (cell[0] * 3, cell[1] + 16), (20, 20, 20))
        dr = ImageDraw.Draw(row)
        notes = []
        for i, (axes, label) in enumerate((((2, 1, 0, 1), 'side'), ((2, 0, 1, -1), 'top'), ((0, 1, 2, 1), 'front'))):
            a, (k, cx, cy) = silhouette(ref, axes, cell, lo, hi)
            b, _ = silhouette(mod, axes, cell, lo, hi)
            img = np.zeros(cell[::-1] + (3,), np.uint8)
            img[a & ~b] = (200, 40, 40)
            img[b & ~a] = (40, 200, 40)
            img[a & b] = (220, 210, 60)
            tile = Image.fromarray(img)
            if label == 'side':
                td = ImageDraw.Draw(tile)
                for wx, wy, wz, wr in wheels[1::2]:
                    px, py = cell[0] / 2 + (wz - cx) * k, cell[1] / 2 - (wy - cy) * k
                    td.ellipse((px - wr * k, py - wr * k, px + wr * k, py + wr * k), outline=(255, 255, 255))
            row.paste(tile, (i * cell[0], 16))
            notes.append('%s %.2f' % (label, (a & b).sum() / max(1, (a | b).sum())))
        dr.text((4, 2), '%s  red Rush 2, green mod, yellow both; circles: Rush 2 wheels.  %s' % (name, ', '.join(notes)),
                fill=(255, 255, 255))
        rows.append(row)
    sheet = Image.new('RGB', (rows[0].width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y)); y += r.height
    sheet.save(path)
    print('wrote', path)


# ---- preview ------------------------------------------------------------------------------------------------------
def render(parts, axes, size, lo, hi, light=None):
    """Orthographic view down the third axis of `axes` (0 x, 1 y, 2 z, sign = which end is near), textured, with a
    z-buffer. Returns an RGB image."""
    w, h = size
    img = np.full((h, w, 3), 40, dtype=np.float64)
    zbuf = np.full((h, w), -1e18)
    (ax, ay, az), sz = axes[:3], axes[3]
    span = max(hi[ax] - lo[ax], (hi[ay] - lo[ay]) * w / h)
    k = (w - 8) / span
    cx, cy = (lo[ax] + hi[ax]) / 2, (lo[ay] + hi[ay]) / 2
    for mesh, image, mask in parts:
        tex = np.array(image.convert('RGB')).astype(np.float64) if image is not None else None
        for t in mesh.tris:
            p = mesh.positions[t]
            x = (p[:, ax] - cx) * k * axes[4] + w / 2
            y = h / 2 - (p[:, ay] - cy) * k
            z = p[:, az] * sz
            x0, x1 = int(max(0, np.floor(x.min()))), int(min(w - 1, np.ceil(x.max())))
            y0, y1 = int(max(0, np.floor(y.min()))), int(min(h - 1, np.ceil(y.max())))
            if x0 > x1 or y0 > y1:
                continue
            d = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0])
            if abs(d) < 1e-9:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            b1 = ((gx - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (gy - y[0])) / d
            b2 = ((x[1] - x[0]) * (gy - y[0]) - (gx - x[0]) * (y[1] - y[0])) / d
            b0 = 1 - b1 - b2
            inside = (b0 >= 0) & (b1 >= 0) & (b2 >= 0)
            if not inside.any():
                continue
            zz = b0 * z[0] + b1 * z[1] + b2 * z[2]
            sub = zbuf[y0:y1 + 1, x0:x1 + 1]
            draw = inside & (zz > sub)
            if not draw.any():
                continue
            sub[draw] = zz[draw]
            if tex is not None:
                uv = mesh.uvs[t]
                u = (b0 * uv[0, 0] + b1 * uv[1, 0] + b2 * uv[2, 0]) % 1.0
                v = (b0 * uv[0, 1] + b1 * uv[1, 1] + b2 * uv[2, 1]) % 1.0
                col = tex[(v * (tex.shape[0] - 1)).astype(int), (u * (tex.shape[1] - 1)).astype(int)]
            else:
                col = np.full(gx.shape + (3,), 200.0)
            nrm = np.cross(p[1] - p[0], p[2] - p[0])
            nrm /= max(np.linalg.norm(nrm), 1e-9)
            shade = 0.55 + 0.45 * abs(nrm[az])
            img[y0:y1 + 1, x0:x1 + 1][draw] = col[draw] * shade
    return Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))


def render_preview(cars, path, cell=(300, 110)):
    rows = []
    for name, parts, ref in cars:
        lo, hi = ref.reshape(-1, 3).min(0), ref.reshape(-1, 3).max(0)
        ref_parts = [(Mesh('ref', '', None, ref.reshape(-1, 3), np.zeros((len(ref) * 3, 3)), np.zeros((len(ref) * 3, 2)),
                           np.arange(len(ref) * 3).reshape(-1, 3)), None, None)]
        # side (from the car's right: game +x is glTF -x, front to the right), top (front to the right)
        side = (2, 1, 0, -1, 1)
        top = (2, 0, 1, 1, 1)
        row = Image.new('RGB', (cell[0] * 4, cell[1] + 14), (20, 20, 20))
        dr = ImageDraw.Draw(row)
        for i, (ps, view) in enumerate([(parts, side), (ref_parts, side), (parts, top), (ref_parts, top)]):
            row.paste(render(ps, view, cell, lo, hi), (i * cell[0], 14))
        dr.text((4, 1), name + '  (mod side | Rush 2 side | mod top | Rush 2 top; front to the right)', fill=(255, 255, 255))
        rows.append(row)
    sheet = Image.new('RGB', (rows[0].width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y)); y += r.height
    sheet.save(path)
    print('wrote', path)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    b = sub.add_parser('build'); b.add_argument('mapping'); b.add_argument('out'); b.add_argument('--preview')
    t = sub.add_parser('textures'); t.add_argument('mesh'); t.add_argument('folder')
    p = sub.add_parser('preview'); p.add_argument('mapping'); p.add_argument('out')
    o = sub.add_parser('overlay'); o.add_argument('mapping'); o.add_argument('out'); o.add_argument('cars', nargs='*')
    a = ap.parse_args()
    if a.cmd == 'textures':
        meshes = load_mesh(a.mesh)
        by_mat = {}
        for m in meshes:
            by_mat.setdefault(m.material, []).append(m)
        for mat, ms in by_mat.items():
            print('%s [%s]: %s' % (os.path.basename(a.mesh), mat, ', '.join('%s (%.2f in, %.2f unused)' % (f, i, s)
                                                                          for _, i, s, f in best_textures(ms, a.folder))))
        return
    cfg = json.load(open(a.mapping))
    cfg.setdefault('base', os.path.dirname(os.path.abspath(a.mapping)))
    if a.cmd == 'build':
        build(cfg, a.out, a.preview)
    else:
        cars = []
        for name, car in cfg['cars'].items():
            if a.cmd == 'overlay' and a.cars and name not in a.cars:
                continue
            ref = rush2_car(name)
            parts, score = convert_car(cfg, name, car, ref)
            cars.append((name, parts, ref))
        (render_overlay if a.cmd == 'overlay' else render_preview)(cars, a.out)


if __name__ == '__main__':
    main()
