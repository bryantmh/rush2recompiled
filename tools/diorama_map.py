#!/usr/bin/env python3
"""Top-down authoring map of a track .glb, for placing a hand-made track select diorama's parts (buildings, landmarks,
outline) by coordinate: every triangle seen from above, shaded by height (or textured with --tex), the AI path
(AIPATH_main) in red with its point numbers and heights, and a grid labeled in glb units (glTF meters, x right,
z down the picture).

    python tools/diorama_map.py TRACK.glb OUT.png [--px N] [--grid M] [--window X0 Z0 X1 Z1] [--tex]
                                [--overlay DIORAMA.glb]

--window crops to that glb rectangle (zoom in on one district). --overlay draws another glb's triangles (the diorama
being built, in the same frame) outlined in cyan over the track, to check its parts sit where they should.
Needs numpy and Pillow; reads with tools/glb_view.py.
"""
import argparse, math, os, sys
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import glb_view  # noqa: E402


def world_tris(path, tex=False):
    """(triangles [n,3,3] in world space, node name per triangle, colors [n,3] or None)."""
    gltf, bin_ = glb_view.load(path)
    texs = glb_view.textures(gltf, bin_) if tex else []
    tris, names, cols = [], [], []

    def walk(ni, parent):
        node = gltf['nodes'][ni]
        m = parent @ glb_view.node_matrix(node)
        if 'mesh' in node:
            for prim in gltf['meshes'][node['mesh']]['primitives']:
                pos = glb_view.accessor(gltf, bin_, prim['attributes']['POSITION'])
                idx = (glb_view.accessor(gltf, bin_, prim['indices']).astype(np.int64).ravel() if 'indices' in prim
                       else np.arange(len(pos)))
                p = np.c_[pos, np.ones(len(pos))] @ m.T
                t = p[idx[:len(idx) // 3 * 3], :3].reshape(-1, 3, 3)
                tris.append(t)
                names.extend([node.get('name', '')] * len(t))
                c = np.full((len(t), 3), 200.0)
                if tex and 'TEXCOORD_0' in prim['attributes'] and 'material' in prim:
                    mat = gltf['materials'][prim['material']]
                    ti = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture', {}).get('index')
                    if ti is not None:
                        im = texs[ti][0][:, :, :3] * 255.0
                        uv = glb_view.accessor(gltf, bin_, prim['attributes']['TEXCOORD_0'])[idx[:len(idx) // 3 * 3]]
                        uv = uv.reshape(-1, 3, 2).mean(1)
                        u = (np.mod(uv[:, 0], 1) * (im.shape[1] - 1)).astype(int)
                        v = (np.mod(uv[:, 1], 1) * (im.shape[0] - 1)).astype(int)
                        c = im[v, u]
                cols.append(c)
        for ch in node.get('children', []):
            walk(ch, m)

    scene = gltf['scenes'][gltf.get('scene', 0)]
    for ni in scene['nodes']:
        walk(ni, np.eye(4))
    return np.concatenate(tris), names, np.concatenate(cols)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('glb')
    ap.add_argument('out')
    ap.add_argument('--px', type=float, default=1.0, help='pixels per glb unit')
    ap.add_argument('--grid', type=float, default=50.0)
    ap.add_argument('--window', type=float, nargs=4)
    ap.add_argument('--tex', action='store_true')
    ap.add_argument('--overlay')
    a = ap.parse_args()

    tris, names, cols = world_tris(a.glb, a.tex)
    keep = [not n.startswith(('AIPATH', 'FINISH', 'CHECKPOINT', 'COL_')) for n in names]
    keep = np.array(keep)
    vis, vcol = tris[keep], cols[keep]
    route = None
    for i, n in enumerate(names):
        if n == 'AIPATH_main':
            route = tris[np.array([x == 'AIPATH_main' for x in names])]
            break
    if a.window:
        x0, z0, x1, z1 = a.window
    else:
        x0, z0 = vis[:, :, 0].min() - 20, vis[:, :, 2].min() - 20
        x1, z1 = vis[:, :, 0].max() + 20, vis[:, :, 2].max() + 20
    W, H = int((x1 - x0) * a.px), int((z1 - z0) * a.px)
    img = Image.new('RGB', (W, H), (40, 40, 48))
    d = ImageDraw.Draw(img)
    ylo, yhi = vis[:, :, 1].min(), vis[:, :, 1].max()
    # Low first, so the tops of buildings cover the streets.
    order = np.argsort(vis[:, :, 1].max(1))
    for k in order:
        t = vis[k]
        pts = [((p[0] - x0) * a.px, (p[2] - z0) * a.px) for p in t]
        if a.tex:
            c = tuple(int(v) for v in vcol[k])
        else:
            h = (t[:, 1].max() - ylo) / max(1e-6, yhi - ylo)
            c = (int(60 + 195 * h), int(60 + 150 * h), int(90 + 60 * (1 - h)))
        d.polygon(pts, fill=c)
    # Grid.
    g = a.grid
    gx = math.ceil(x0 / g) * g
    while gx < x1:
        X = (gx - x0) * a.px
        d.line([(X, 0), (X, H)], fill=(90, 90, 110) if gx % (g * 4) else (150, 150, 170))
        d.text((X + 2, 2), f'{gx:.0f}', fill=(255, 255, 0))
        gx += g
    gz = math.ceil(z0 / g) * g
    while gz < z1:
        Z = (gz - z0) * a.px
        d.line([(0, Z), (W, Z)], fill=(90, 90, 110) if gz % (g * 4) else (150, 150, 170))
        d.text((2, Z + 2), f'{gz:.0f}', fill=(255, 255, 0))
        gz += g
    # The AI path: the ribbon's centers, numbered every 25 points with their height.
    if route is not None:
        c = route.reshape(-1, 2, 3, 3)[:, 0].mean(1)   # first triangle of each quad: near its start
        pts = [((p[0] - x0) * a.px, (p[2] - z0) * a.px) for p in c]
        d.line(pts + pts[:1], fill=(255, 40, 40), width=2)
        for i in range(0, len(c), 25):
            d.text(pts[i], f'{i}:{c[i][1]:.0f}', fill=(255, 160, 160))
    if a.overlay:
        ot, _, _ = world_tris(a.overlay)
        for t in ot:
            pts = [((p[0] - x0) * a.px, (p[2] - z0) * a.px) for p in t]
            d.polygon(pts, outline=(0, 255, 255))
    img.save(a.out)
    print(f'{len(vis)} triangles, heights {ylo:.0f}..{yhi:.0f}, {W}x{H} -> {a.out}')


if __name__ == '__main__':
    main()
