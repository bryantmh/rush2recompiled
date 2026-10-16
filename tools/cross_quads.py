#!/usr/bin/env python3
"""Finds cross-quad scenery (trees and other flat "X" billboards: two or more vertical alpha-cutout quads crossing at a
shared vertical axis) in model-dump .glb files, and writes each one's textures to a folder.

    python tools/cross_quads.py OUT_DIR GLB [GLB ...]

GLB files come from the asset catalog (docs/asset_catalog.md), e.g. the Rush 2 tracks:
    cmd //c "tools\\asset_catalog_test.bat"
    tmp/catalog_test/asset_catalog_test.exe tmp/trees/dump 14 33 34 35 36 37 38 39 3A 3B 3C 3D 3E

Writes every texture a cross uses once (identical pictures across files are kept under the first name), turned the way
it stands in the world (half pictures the game mirrors across the card are written whole), to OUT_DIR/trees/ (mostly green to yellow texels: foliage) or OUT_DIR/other/ (fences, rails,
poles...), a contact sheet of each (OUT_DIR/sheet_trees.png, sheet_other.png) and OUT_DIR/crosses.csv: one row per
cross (file, model, its textures, card count, center, height and width in the glb's model units).
"""
import csv, io, json, math, os, re, struct, sys
from collections import defaultdict

import numpy as np
from PIL import Image, ImageDraw

COMP = {5120: 'b', 5121: 'B', 5122: 'h', 5123: 'H', 5125: 'I', 5126: 'f'}
NCOMP = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}


def load_glb(path):
    d = open(path, 'rb').read()
    n = struct.unpack_from('<I', d, 12)[0]
    j = json.loads(d[20:20 + n])
    off = 20 + n
    blen = struct.unpack_from('<I', d, off)[0]
    return j, d[off + 8:off + 8 + blen]


def accessor(j, b, i):
    a = j['accessors'][i]
    bv = j['bufferViews'][a['bufferView']]
    nc = NCOMP[a['type']]
    dt = np.dtype('<' + COMP[a['componentType']])
    start = bv.get('byteOffset', 0) + a.get('byteOffset', 0)
    stride = bv.get('byteStride', 0) or dt.itemsize * nc
    raw = np.frombuffer(b, dtype=np.uint8, count=stride * (a['count'] - 1) + dt.itemsize * nc, offset=start)
    out = np.lib.stride_tricks.as_strided(raw, shape=(a['count'], nc * dt.itemsize), strides=(stride, 1))
    return np.ascontiguousarray(out).view(dt).reshape(a['count'], nc).astype(np.float64)


def image_of(j, b, tex_index, cache):
    if tex_index in cache:
        return cache[tex_index]
    img = j['images'][j['textures'][tex_index]['source']]
    bv = j['bufferViews'][img['bufferView']]
    o = bv.get('byteOffset', 0)
    im = Image.open(io.BytesIO(b[o:o + bv['byteLength']])).convert('RGBA')
    cache[tex_index] = (img.get('name', 'tex%d' % tex_index), im)
    return cache[tex_index]


def has_cutout(im):
    a = np.asarray(im)[:, :, 3]
    return (a < 128).mean() > 0.05 and (a >= 128).mean() > 0.05


def quads(vtx):
    """Pairs the triangles of a triangle list (rows x y z u v) into planar quads (two triangles sharing an edge, same
    plane); a lone triangle stays a card of its own. Returns [(rows Nx5, normal)]."""
    tris = vtx.reshape(-1, 3, 5)[:, :, :3]
    rows = vtx.reshape(-1, 3, 5)
    normals = []
    for t in tris:
        n = np.cross(t[1] - t[0], t[2] - t[0])
        ln = np.linalg.norm(n)
        normals.append(n / ln if ln > 1e-9 else n)
    key = lambda p: tuple(np.round(p, 2))
    edges = defaultdict(list)
    for i, t in enumerate(tris):
        for a, c in ((0, 1), (1, 2), (2, 0)):
            edges[frozenset((key(t[a]), key(t[c])))].append(i)
    used, out = set(), []
    for i, t in enumerate(tris):
        if i in used:
            continue
        used.add(i)
        pts = [rows[i]]
        for a, c in ((0, 1), (1, 2), (2, 0)):
            for k in edges[frozenset((key(t[a]), key(t[c])))]:
                if k not in used and abs(abs(np.dot(normals[i], normals[k])) - 1) < 1e-3:
                    used.add(k)
                    pts.append(rows[k])
                    break
            if len(pts) > 1:
                break
        out.append((np.concatenate(pts), normals[i]))
    return out


def find_crosses(j, b):
    """Returns [(model name, [texture index...], cards, center xyz, height, width)]."""
    cache, cut = {}, {}
    found = []
    for mesh in j['meshes']:
        cards = []  # (texture, points, normal)
        for prim in mesh['primitives']:
            mat = j['materials'][prim.get('material', 0)]
            tex = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture', {}).get('index')
            if tex is None:
                continue
            if tex not in cut:
                cut[tex] = mat.get('alphaMode') in ('MASK', 'BLEND') or has_cutout(image_of(j, b, tex, cache)[1])
            if not cut[tex]:
                continue
            vtx = np.c_[accessor(j, b, prim['attributes']['POSITION']), accessor(j, b, prim['attributes']['TEXCOORD_0'])]
            if 'indices' in prim:
                vtx = vtx[accessor(j, b, prim['indices']).astype(int).ravel()]
            for rows, n in quads(vtx):
                pts = rows[:, :3]
                # Image rows run down the card when v grows as y falls (glTF v is down the image).
                upright = np.corrcoef(rows[:, 1], rows[:, 4])[0, 1] < 0 if np.ptp(rows[:, 4]) > 0 and np.ptp(rows[:, 1]) > 0 else True
                # A half picture shown twice, mirrored, across the card (mirrored-repeat wrap with u spanning 0..2).
                sampler = j['samplers'][j['textures'][tex].get('sampler', 0)] if j.get('samplers') else {}
                mirror = sampler.get('wrapS') == 33648 and np.ptp(rows[:, 3]) > 1.5
                if abs(n[1]) > 0.25:  # not upright (glTF y is up)
                    continue
                h = pts[:, 1].max() - pts[:, 1].min()
                horiz = np.array([n[2], -n[0]])  # the card's horizontal direction
                xz = pts[:, [0, 2]]
                w = (xz @ horiz).max() - (xz @ horiz).min()
                if h < 1e-3 or w < 1e-3:
                    continue
                cards.append((tex, pts, n, xz.mean(0), h, w, pts[:, 1].min(), (upright, mirror)))
        # Group cards whose ground centers sit on each other's line, at the same base, crossing at an angle.
        n = len(cards)
        parent = list(range(n))

        def root(i):
            while parent[i] != i:
                parent[i] = parent[parent[i]]
                i = parent[i]
            return i
        for a in range(n):
            ta, pa, na, ca, ha, wa, ya = cards[a][:7]
            for c in range(a + 1, n):
                tc, pc, nc_, cc, hc, wc, yc = cards[c][:7]
                if abs(np.dot(na[[0, 2]], nc_[[0, 2]])) > 0.85:  # parallel cards don't cross
                    continue
                size = max(wa, wc)
                if np.linalg.norm(ca - cc) > 0.2 * size or abs(ya - yc) > 0.2 * max(ha, hc):
                    continue
                parent[root(a)] = root(c)
        groups = defaultdict(list)
        for i in range(n):
            groups[root(i)].append(i)
        for g in groups.values():
            if len(g) < 2:
                continue
            pts = np.concatenate([cards[i][1] for i in g])
            texs = {}
            for i in g:
                texs.setdefault(cards[i][0], []).append(cards[i][7])
            # texture -> (drawn upright as stored, mirrored across the card), as most of its cards draw it
            texs = {t: (sum(u for u, _ in v) * 2 >= len(v), sum(m for _, m in v) * 2 > len(v)) for t, v in texs.items()}
            found.append((mesh.get('name', '?'), texs, len(g), pts.mean(0),
                          pts[:, 1].max() - pts[:, 1].min(), max(cards[i][5] for i in g)))
    return found, cache


def safe(name):
    return re.sub(r'[^A-Za-z0-9_.-]', '_', name)


def sheet(images, path):
    if not images:
        return
    cell, cols = 136, 8
    rows = (len(images) + cols - 1) // cols
    out = Image.new('RGBA', (cols * cell, rows * cell), (60, 60, 70, 255))
    d = ImageDraw.Draw(out)
    for k, (name, im) in enumerate(images):
        x, y = (k % cols) * cell, (k // cols) * cell
        t = im.copy()
        t.thumbnail((cell - 8, cell - 20), Image.NEAREST if max(im.size) < cell else Image.LANCZOS)
        if max(im.size) * 2 <= cell - 20:
            s = (cell - 20) // max(im.size)
            t = im.resize((im.width * s, im.height * s), Image.NEAREST)
        out.alpha_composite(t, (x + (cell - t.width) // 2, y + 2))
        d.text((x + 3, y + cell - 16), name[:20], fill=(255, 255, 255, 255))
    out.save(path)


def foliage(im):
    """True when most of the opaque texels are green to yellow (leaves) rather than gray, brown or red (fences, rails,
    poles, lights)."""
    a = np.asarray(im.convert('RGBA')).astype(np.float64) / 255
    px = a[a[:, :, 3] >= 0.5][:, :3]
    if len(px) == 0:
        return False
    mx, mn = px.max(1), px.min(1)
    colored = (mx - mn) / np.maximum(mx, 1e-9) > 0.15  # the rest is gray: trunks, shadow, metal
    if colored.mean() < 0.5:
        return False
    r, g, b = px[colored].T
    return ((g >= b) & (g >= 0.6 * r)).mean() >= 0.7  # green through autumn yellow-orange, not red or brown


# Catalog dump labels (tools/asset_catalog_test.cpp) -> folder names.
LABELS = {'asset_14': 'race_objects'}


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    rows, seen, sheets = [], {}, {'trees': [], 'other': []}
    for path in sys.argv[2:]:
        j, b = load_glb(path)
        label = os.path.splitext(os.path.basename(path))[0]
        crosses, cache = find_crosses(j, b)
        names_here = {}
        for model, texs, ncards, center, h, w in crosses:
            names = []
            for t, (upright, mirror) in texs.items():
                if (t, upright, mirror) not in names_here:
                    name, im = image_of(j, b, t, cache)
                    if not upright:
                        im = im.transpose(Image.FLIP_TOP_BOTTOM)
                    if mirror:
                        full = Image.new('RGBA', (im.width * 2, im.height))
                        full.paste(im, (0, 0))
                        full.paste(im.transpose(Image.FLIP_LEFT_RIGHT), (im.width, 0))
                        im = full
                    key = im.tobytes() + bytes(im.size[0:1])
                    if key in seen:  # the same picture in another file or texture: keep the first name
                        fn = seen[key]
                    else:
                        kind = 'trees' if foliage(im) else 'other'
                        fn = '%s/%s.png' % (kind, safe(name))
                        k = 2
                        while fn in seen.values():  # two different pictures under one name
                            fn = '%s/%s_%d.png' % (kind, safe(name), k)
                            k += 1
                        os.makedirs(os.path.join(out, kind), exist_ok=True)
                        im.save(os.path.join(out, fn))
                        seen[key] = fn
                        sheets[kind].append((safe(name), im))
                    names_here[(t, upright, mirror)] = fn
                names.append(names_here[(t, upright, mirror)])
            rows.append([LABELS.get(label, label), model, ' '.join(sorted(set(names))), ncards,
                         '%.0f %.0f %.0f' % tuple(center), '%.0f' % h, '%.0f' % w])
        print('%s: %d crosses, %d textures' % (label, len(crosses), len(set(names_here.values()))))
    for kind, ims in sheets.items():
        sheet(ims, os.path.join(out, 'sheet_%s.png' % kind))
        print('%s: %d textures' % (kind, len(ims)))
    with open(os.path.join(out, 'crosses.csv'), 'w', newline='') as f:
        w = csv.writer(f)
        w.writerow(['file', 'model', 'textures', 'cards', 'center', 'height', 'width'])
        w.writerows(rows)
    return 0


if __name__ == '__main__':
    sys.exit(main())
