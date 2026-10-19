"""Renders a .glb from Dump Models (docs/asset_catalog.md) to PNG with a small software rasterizer: textured, vertex
colored, z-buffered, alpha-tested. For checking model dumps without a 3D app.

    python tools/glb_view.py MODEL.glb OUT.png [--yaw DEG] [--pitch DEG] [--size N] [--node NAME ...] [--views N] [--scene N]

Draws scene N (default 0: a track as placed) with its node matrices; LODs other than the first are scaled to 0 in
the dumps, so they are skipped. --max-edge M leaves out triangles with an edge longer than M glb units (a track's water
or ground sheets, to frame the rest). --node keeps
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('glb'); ap.add_argument('out')
    ap.add_argument('--yaw', type=float, default=35); ap.add_argument('--pitch', type=float, default=25)
    ap.add_argument('--size', type=int, default=512); ap.add_argument('--node', nargs='*', default=[])
    ap.add_argument('--views', type=int, default=1); ap.add_argument('--scene', type=int, default=0)
    ap.add_argument('--max-edge', type=float, default=0)
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
    panels = [render(tris, texs, a.yaw + i * 360 / a.views, a.pitch, a.size) for i in range(a.views)]
    out = Image.new('RGB', (a.size * len(panels), a.size))
    for i, p in enumerate(panels):
        out.paste(p, (i * a.size, 0))
    out.save(a.out)
    print(f'{len(tris[0])} triangles, {len(texs)} textures -> {a.out}')


if __name__ == '__main__':
    main()
