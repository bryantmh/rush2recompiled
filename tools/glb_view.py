"""Renders a .glb from Dump Models (docs/asset_catalog.md) to PNG with a small software rasterizer: textured, vertex
colored, z-buffered, alpha-tested. For checking model dumps without a 3D app.

    python tools/glb_view.py MODEL.glb OUT.png [--yaw DEG] [--pitch DEG] [--size N] [--node NAME ...] [--views N] [--scene N]

Draws scene N (default 0: a track as placed) with its node matrices; LODs other than the first are scaled to 0 in
the dumps, so they are skipped. --eye X Y Z --at X Y Z draws a perspective view from a point instead (glb units, e.g.
standing on a track's start line and looking back; --fov degrees across, --width/--height pixels, --sky R G B). --cull 1|-1 leaves out triangles wound one way on screen (back faces, as the game does), --bilinear filters
textures as the game does. --max-edge M
leaves out triangles with an edge longer than M glb units (a track's water or ground sheets, to frame the rest). --node keeps
only nodes whose names start with one of the given texts. --views N draws N panels around the model, side by side.
Needs numpy and Pillow.
"""
import argparse, io, json, math, struct
import numpy as np
from PIL import Image


def load(path):
    data = open(path, 'rb').read()
    magic, version, length = struct.unpack_from('<III', data, 0)
    assert magic == 0x46546C67, 'not a glb'
    jlen, _ = struct.unpack_from('<II', data, 12)
    gltf = json.loads(data[20:20 + jlen])
    blen, _ = struct.unpack_from('<II', data, 20 + jlen)
    bin_ = data[28 + jlen:28 + jlen + blen]
    return gltf, bin_


def accessor(gltf, bin_, index):
    a = gltf['accessors'][index]
    v = gltf['bufferViews'][a['bufferView']]
    comps = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
    dtype = {5126: np.float32, 5121: np.uint8, 5123: np.uint16, 5125: np.uint32}[a['componentType']]
    off = v.get('byteOffset', 0) + a.get('byteOffset', 0)
    arr = np.frombuffer(bin_, dtype=dtype, count=a['count'] * comps, offset=off).reshape(a['count'], comps)
    if a.get('normalized'):
        arr = arr.astype(np.float32) / 255.0
    return arr.astype(np.float32)


def textures(gltf, bin_):
    out = []
    for t in gltf.get('textures', []):
        img = gltf['images'][t['source']]
        v = gltf['bufferViews'][img['bufferView']]
        o = v.get('byteOffset', 0)
        im = Image.open(io.BytesIO(bin_[o:o + v['byteLength']])).convert('RGBA')
        s = gltf['samplers'][t['sampler']]
        out.append((np.asarray(im, dtype=np.float32) / 255.0, s.get('wrapS', 10497), s.get('wrapT', 10497)))
    return out


def wrap(c, n, mode):
    if mode == 33071:
        return np.clip(c, 0, n - 1)
    if mode == 33648:
        p = np.mod(c, 2 * n)
        return np.where(p >= n, 2 * n - 1 - p, p)
    return np.mod(c, n)


def node_matrix(node):
    if 'matrix' in node:
        return np.array(node['matrix'], dtype=np.float64).reshape(4, 4).T
    m = np.eye(4)
    if 'scale' in node:
        m[:3, :3] = np.diag(node['scale'])
    if 'translation' in node:
        m[:3, 3] = node['translation']
    return m


def triangles(gltf, bin_, keep, scene=0):
    """(positions N x 3 x 3, uvs N x 3 x 2, colors N x 3 x 4, texture per triangle) of the visible nodes of scene,
    transformed by their node matrices."""
    nodes = gltf['nodes']
    P, U, C, T = [], [], [], []
    cache = {}

    def mesh(index):
        if index not in cache:
            parts = []
            for prim in gltf['meshes'][index]['primitives']:
                at = prim['attributes']
                p = accessor(gltf, bin_, at['POSITION'])
                uv = accessor(gltf, bin_, at['TEXCOORD_0'])
                col = accessor(gltf, bin_, at['COLOR_0'])
                mat = gltf['materials'][prim['material']]
                tex = mat['pbrMetallicRoughness'].get('baseColorTexture', {}).get('index', -1)
                parts.append((p, uv, col, tex))
            cache[index] = parts
        return cache[index]

    def visit(i, parent):
        node = nodes[i]
        m = parent @ node_matrix(node)
        if node.get('scale') == [0, 0, 0]:
            return
        if 'mesh' in node and (not keep or any(node.get('name', '').startswith(k) for k in keep)):
            for p, uv, col, tex in mesh(node['mesh']):
                n = len(p) // 3
                q = p[:n * 3] @ m[:3, :3].T + m[:3, 3]
                P.append(q.reshape(n, 3, 3)); U.append(uv[:n * 3].reshape(n, 3, 2))
                C.append(col[:n * 3].reshape(n, 3, 4)); T.append(np.full(n, tex))
        for c in node.get('children', []):
            visit(c, m)

    for i in gltf['scenes'][scene]['nodes']:
        visit(i, np.eye(4))
    if not P:
        return None
    return np.concatenate(P), np.concatenate(U), np.concatenate(C), np.concatenate(T)


def render(tris, texs, yaw, pitch, size):
    P, U, C, T = tris
    lo, hi = P.reshape(-1, 3).min(0), P.reshape(-1, 3).max(0)
    center = (lo + hi) / 2
    radius = max(np.linalg.norm(hi - lo) / 2, 1e-3)
    cy, sy, cp, sp = math.cos(math.radians(yaw)), math.sin(math.radians(yaw)), math.cos(math.radians(pitch)), math.sin(math.radians(pitch))
    rot_y = np.array([[cy, 0, sy], [0, 1, 0], [-sy, 0, cy]])
    rot_x = np.array([[1, 0, 0], [0, cp, -sp], [0, sp, cp]])
    V = (P - center) @ (rot_x @ rot_y).T
    scale = size / (2.1 * radius)
    # Right-handed, as glTF viewers are: the camera looks down -z, so nearer points have larger z.
    sx = V[..., 0] * scale + size / 2
    sy_ = size / 2 - V[..., 1] * scale
    z = -V[..., 2]
    color = np.zeros((size, size, 3), np.float32) + 0.25
    depth = np.full((size, size), np.inf, np.float32)
    for i in range(len(P)):
        x0, x1, x2 = sx[i]; y0, y1, y2 = sy_[i]
        minx, maxx = int(max(min(x0, x1, x2), 0)), int(min(max(x0, x1, x2) + 1, size))
        miny, maxy = int(max(min(y0, y1, y2), 0)), int(min(max(y0, y1, y2) + 1, size))
        if minx >= maxx or miny >= maxy:
            continue
        area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
        if abs(area) < 1e-9:
            continue
        gx, gy = np.meshgrid(np.arange(minx, maxx) + 0.5, np.arange(miny, maxy) + 0.5)
        w0 = ((x1 - gx) * (y2 - gy) - (x2 - gx) * (y1 - gy)) / area
        w1 = ((x2 - gx) * (y0 - gy) - (x0 - gx) * (y2 - gy)) / area
        w2 = 1 - w0 - w1
        inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
        if not inside.any():
            continue
        zz = w0 * z[i, 0] + w1 * z[i, 1] + w2 * z[i, 2]
        sub = depth[miny:maxy, minx:maxx]
        vis = inside & (zz < sub)
        if not vis.any():
            continue
        col = (w0[..., None] * C[i, 0] + w1[..., None] * C[i, 1] + w2[..., None] * C[i, 2])
        if T[i] >= 0 and T[i] < len(texs):
            img, ws, wt = texs[T[i]]
            h, w = img.shape[:2]
            u = w0 * U[i, 0, 0] + w1 * U[i, 1, 0] + w2 * U[i, 2, 0]
            v = w0 * U[i, 0, 1] + w1 * U[i, 1, 1] + w2 * U[i, 2, 1]
            tx = wrap(np.floor(u * w).astype(np.int64), w, ws)
            ty = wrap(np.floor(v * h).astype(np.int64), h, wt)
            texel = img[ty, tx]
            vis &= texel[..., 3] > 0.3
            col = col * texel
        sub[vis] = zz[vis]
        color[miny:maxy, minx:maxx][vis] = col[vis][:, :3]
    return Image.fromarray((np.clip(color, 0, 1) * 255).astype(np.uint8))


def clip_near(p, near):
    """A triangle (3 camera-space points, rows [x, y, z, attrs...], visible z > near) clipped to z >= near: 0-2 triangles."""
    out = []
    n = len(p)
    for i in range(n):
        a, b = p[i], p[(i + 1) % n]
        ia, ib = a[2] >= near, b[2] >= near
        if ia:
            out.append(a)
        if ia != ib:
            t = (near - a[2]) / (b[2] - a[2])
            out.append(a + (b - a) * t)
    return [np.array([out[0], out[k], out[k + 1]]) for k in range(1, len(out) - 1)]


def sample(img, ws, wt, u, v, bilinear):
    """Texels at (u, v): nearest, or bilinear (as the game draws them, the N64's filtering)."""
    h, w = img.shape[:2]
    if not bilinear:
        return img[wrap(np.floor(v * h).astype(np.int64), h, wt), wrap(np.floor(u * w).astype(np.int64), w, ws)]
    x, y = u * w - 0.5, v * h - 0.5
    x0, y0 = np.floor(x), np.floor(y)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    x0, y0 = x0.astype(np.int64), y0.astype(np.int64)
    xa, xb = wrap(x0, w, ws), wrap(x0 + 1, w, ws)
    ya, yb = wrap(y0, h, wt), wrap(y0 + 1, h, wt)
    top = img[ya, xa] * (1 - fx) + img[ya, xb] * fx
    bot = img[yb, xa] * (1 - fx) + img[yb, xb] * fx
    return top * (1 - fy) + bot * fy


def render_eye(tris, texs, eye, at, fov, width, height, sky, cull=0, bilinear=False):
    """Perspective view from eye toward at (y up), perspective-correct textures, alpha-tested like render(). cull: 1
    leaves out triangles wound counterclockwise on screen, -1 clockwise (the game culls back faces), 0 none."""
    P, U, C, T = tris
    eye, at = np.array(eye, np.float64), np.array(at, np.float64)
    f = at - eye; f /= np.linalg.norm(f)
    r = np.cross(f, [0.0, 1.0, 0.0]); r /= np.linalg.norm(r)
    u = np.cross(r, f)
    V = np.stack([(P - eye) @ r, (P - eye) @ u, (P - eye) @ f], axis=-1)   # x right, y up, z forward
    focal = width / 2 / math.tan(math.radians(fov) / 2)
    near = 0.5
    zmax = V[..., 2].max(1)
    keep = zmax > near
    keep &= ~(np.all(V[..., 0] > V[..., 2] * width / 2 / focal, 1) | np.all(-V[..., 0] > V[..., 2] * width / 2 / focal, 1))
    keep &= ~(np.all(V[..., 1] > V[..., 2] * height / 2 / focal, 1) | np.all(-V[..., 1] > V[..., 2] * height / 2 / focal, 1))
    color = np.zeros((height, width, 3), np.float32) + np.array(sky, np.float32) / 255.0
    depth = np.full((height, width), np.inf, np.float32)
    for i in np.nonzero(keep)[0]:
        rows = np.concatenate([V[i], U[i], C[i]], axis=1)    # x y z u v r g b a
        for tri in clip_near(rows, near):
            z = tri[:, 2]
            xs = width / 2 + tri[:, 0] * focal / z
            ys = height / 2 - tri[:, 1] * focal / z
            x0, x1, x2 = xs; y0, y1, y2 = ys
            minx, maxx = int(max(min(xs), 0)), int(min(max(xs) + 1, width))
            miny, maxy = int(max(min(ys), 0)), int(min(max(ys) + 1, height))
            if minx >= maxx or miny >= maxy:
                continue
            area = (x1 - x0) * (y2 - y0) - (x2 - x0) * (y1 - y0)
            if abs(area) < 1e-9 or cull * area > 0:
                continue
            gx, gy = np.meshgrid(np.arange(minx, maxx) + 0.5, np.arange(miny, maxy) + 0.5)
            w0 = ((x1 - gx) * (y2 - gy) - (x2 - gx) * (y1 - gy)) / area
            w1 = ((x2 - gx) * (y0 - gy) - (x0 - gx) * (y2 - gy)) / area
            w2 = 1 - w0 - w1
            inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
            if not inside.any():
                continue
            iz = w0 / z[0] + w1 / z[1] + w2 / z[2]
            zz = 1 / np.maximum(iz, 1e-12)
            sub = depth[miny:maxy, minx:maxx]
            vis = inside & (zz < sub)
            if not vis.any():
                continue
            def attr(k):
                return (w0 * tri[0, k] / z[0] + w1 * tri[1, k] / z[1] + w2 * tri[2, k] / z[2]) * zz
            col = np.stack([attr(5), attr(6), attr(7)], axis=-1)
            if T[i] >= 0 and T[i] < len(texs):
                img, ws, wt = texs[T[i]]
                texel = sample(img, ws, wt, attr(3), attr(4), bilinear)
                vis &= texel[..., 3] > 0.3
                col = col * texel[..., :3]
            sub[vis] = zz[vis]
            color[miny:maxy, minx:maxx][vis] = col[vis]
    return Image.fromarray((np.clip(color, 0, 1) * 255).astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('glb'); ap.add_argument('out')
    ap.add_argument('--yaw', type=float, default=35); ap.add_argument('--pitch', type=float, default=25)
    ap.add_argument('--size', type=int, default=512); ap.add_argument('--node', nargs='*', default=[])
    ap.add_argument('--views', type=int, default=1); ap.add_argument('--scene', type=int, default=0)
    ap.add_argument('--eye', type=float, nargs=3); ap.add_argument('--at', type=float, nargs=3)
    ap.add_argument('--fov', type=float, default=70); ap.add_argument('--width', type=int, default=640)
    ap.add_argument('--height', type=int, default=360); ap.add_argument('--sky', type=int, nargs=3, default=[110, 150, 200])
    ap.add_argument('--max-edge', type=float, default=0)
    ap.add_argument('--cull', type=int, default=0, choices=[-1, 0, 1])
    ap.add_argument('--bilinear', action='store_true')
    a = ap.parse_args()
    gltf, bin_ = load(a.glb)
    tris = triangles(gltf, bin_, a.node, a.scene)
    if tris is not None and a.max_edge > 0:
        P = tris[0]
        keep = np.max(np.linalg.norm(P - np.roll(P, 1, axis=1), axis=2), axis=1) <= a.max_edge
        tris = tuple(x[keep] for x in tris) if keep.any() else None
    if tris is None:
        print('nothing to draw'); return
    texs = textures(gltf, bin_)
    if a.eye:
        render_eye(tris, texs, a.eye, a.at, a.fov, a.width, a.height, a.sky, a.cull, a.bilinear).save(a.out)
        print(f'{len(tris[0])} triangles, {len(texs)} textures -> {a.out}')
        return
    panels = [render(tris, texs, a.yaw + i * 360 / a.views, a.pitch, a.size) for i in range(a.views)]
    out = Image.new('RGB', (a.size * len(panels), a.size))
    for i, p in enumerate(panels):
        out.paste(p, (i * a.size, 0))
    out.save(a.out)
    print(f'{len(tris[0])} triangles, {len(texs)} textures -> {a.out}')


if __name__ == '__main__':
    main()
