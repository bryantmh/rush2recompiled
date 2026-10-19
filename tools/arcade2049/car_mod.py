#!/usr/bin/env python3
"""Converts the San Francisco Rush 2049 arcade (SE) cars into new cars for a mod
(docs/car_mods.md#new-cars; the arcade's formats: docs/arcade2049.md#cars).

    python tools/arcade2049/car_mod.py OUT_MOD_DIR [--cars CAR7,CAR8] [--mod-id ID] [--sides SIDES.png]

Reads the arcade data ProjectR unpacks (ARCADE2049_DATA, default %LOCALAPPDATA%\\ProjectR\\Data\\sf2049se) and writes
OUT_MOD_DIR/rush2/new_cars/<ID>.glb + <ID>.json per car, and OUT_MOD_DIR/mod.json if it has none (its own mod,
rush2049_arcade_cars, apart from tools/arcade2049/track_mod.py's tracks).

Per car: every color scheme (CARS/CARn/COLn: whole liveries with their own texture sets and parts) as its own glb,
<ID>_COLn.glb, listed in the .json's "schemes" (the car select's STYLE picks one); of each, the body's full-detail LOD (CARBODY LOD 0) in the model dump's space (1/16 ft, x negated), one material per
arcade texture, drawn as authored (no paint mask: each scheme keeps the arcade's colors), and its hood camera hood
(HOOD, as Rush 2049's N64 cars draw theirs) as <ID>_COLn_HOOD.glb, listed in "hoods". The .json takes the car's
name, the Rush 2 car it starts from (engine sound, options), the wheel centers (the arcade's, the same for every car:
--sides draws them over the side views) and the arcade's collision box; it drives as its base car. The brake lights (BRAKE) and wheels
(Rush 2's are drawn) are left out.

The game builds this mod itself (Games tab, src/arcade2049/arcade_cars.cpp, a C++ port of this file): change both
together and compare them with tools/arcade2049/cpp_test/build.bat (docs/arcade2049.md, "Building it in the game").
"""
import argparse, importlib.util, json, os, sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), 'rush2049'))
import arcade  # noqa: E402

# tools/car_mod.py (this file shares its name): its glb writer.
_spec = importlib.util.spec_from_file_location('rush2_car_mod', os.path.join(os.path.dirname(HERE), 'car_mod.py'))
_car_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_car_mod)
Mesh, write_glb = _car_mod.Mesh, _car_mod.write_glb

# The arcade cars added as new cars, each driving as the Rush 2 car of its kind (`base`: its physics, engine sound and
# options). The arcade can't tell them apart: it gives every car one physics descriptor (docs/arcade2049.md#car-physics),
# so its values made the Deuce and the Caddy drive alike, and harder than any stock car. The arcade gives every car
# the same wheels (docs/arcade2049.md#cars: its one car descriptor's wheel block, copied into each race car, and its
# WBLUR wheels placed there): the right wheels at x 2.75, front z 3.925 / rear z -3.925, 0.8 / 0.75 ft below the
# body's origin (Rush 2's descriptor: y -0.8 / -0.75, the drawn center 1 ft higher). Every arcade wheel is 1.01 ft
# round (WHEEL), so the wheel model (15/16 ft at scale 1) is drawn at 1.08.
CARS = {
    'CAR8': dict(id='TRUCK', name='TRUCK', base='PICKUP'),
    'CAR10': dict(id='DEUCE', name='DEUCE', base='HOTROD'),
    'CAR11': dict(id='CADDY', name='CADDY', base='CADDY'),
}
# Arcade color schemes added to Rush 2049 (N64) cars as liveries (docs/car_mods.md#liveries): the N64 car, its arcade
# folder, the COL folders, and how far forward (ft) the N64 car's wheels sit from the arcade's (the center of its
# descriptor's wheel points: the Bruiser's 0x80110984, 4.425 / -3.425; the others' 0x801108D0, the arcade's own). The
# arcade body moves by that, so it sits on the N64 car's wheels (a side and top silhouette fit agrees).
LIVERIES = {
    'GX-2': dict(folder='CAR2', cols=[6], dz=0.0),
    'MAGNUM': dict(folder='CAR4', cols=[4, 6, 7], dz=0.0),
    'SUPER GT': dict(folder='CAR5', cols=[5, 7], dz=0.0),
    '8-BALL': dict(folder='CAR6', cols=[1, 3], dz=0.0),
    'BRUISER': dict(folder='CAR7', cols=[1, 2, 3, 5, 6, 7, 8], dz=0.5),
}
FRONT, REAR = (2.75, 3.925), (2.75, -3.925)
WHEEL_Y = (0.2, 0.25)       # drawn centers above the origin: Rush 2's descriptor y + 1 (-0.8, -0.75)
SCALE = (1.08, 1.08)
UNITS = 16.0                # vertex units per foot (the model dump's space)


# The arcade's collision boxes (project-r.exe VA 0x140664130, 12 x {front, rear, half width, height}, by car index =
# CARn - 1; docs/arcade2049.md#car-physics). Height: the N64 port of this code gives every car 3.5 ft; the arcade's
# table has 2 for all (and its drone box, VA 0x1406EF824), which isn't checked against Rush 2's box use, so the N64's
# is kept.
ARCADE_BOX = [(5.75, 6.5, 3, 2), (6.29, 6.58, 2.9, 2), (6.05, 6.65, 2.9, 2), (6.25, 5.75, 3, 2), (6.44, 6.64, 3, 2),
              (6.25, 6.75, 3, 2), (5.75, 6, 3, 2), (6.7, 6.97, 2.8, 2), (6.25, 6, 3, 2), (6.75, 7, 3, 2),
              (6.5, 6.5, 3, 2), (6.25, 5.5, 3, 2)]


def box(folder):
    front, rear, half, _ = ARCADE_BOX[int(folder[3:]) - 1]
    return [front, rear, half, 3.5]


def mod_manifest(mod_id):
    return {'game_id': 'rush2', 'id': mod_id, 'display_name': 'Rush 2049 Arcade Cars',
            'description': 'San Francisco Rush 2049 Special Edition (arcade): the Truck, Deuce and Caddy as new cars, '
                           'and arcade liveries for the GX-2, Magnum, Super GT, 8-Ball and Bruiser (STYLE row), '
                           'converted with tools/arcade2049/car_mod.py.',
            'short_description': 'Rush 2049 arcade cars', 'version': '1.0.0',
            'authors': ['rush2-recomp'], 'minimum_recomp_version': '0.0.0'}


def body_textures(t, obj='CARBODY'):
    lod = t.by_name[obj].lods[0]
    used = sorted({ti for *_, ti in lod.tris if ti < len(t.textures)})
    return lod, {ti: t.textures[ti].rgba(t.texels) for ti in used}


def smooth_normals(P, crease=60.0):
    """Per corner of the (M, 3, 3) triangles: the face normals of the triangles sharing the corner's point within
    `crease` degrees of this one, averaged."""
    fn = np.cross(P[:, 1] - P[:, 0], P[:, 2] - P[:, 0])
    fn /= np.maximum(np.linalg.norm(fn, axis=1, keepdims=True), 1e-9)
    keys = {}
    for f in range(len(P)):
        for k in range(3):
            keys.setdefault(tuple(np.round(P[f, k], 3)), []).append(f)
    cos = np.cos(np.radians(crease))
    out = np.zeros_like(P)
    for f in range(len(P)):
        for k in range(3):
            n = np.zeros(3)
            for g in keys[tuple(np.round(P[f, k], 3))]:
                if fn[g] @ fn[f] >= cos:
                    n += fn[g]
            out[f, k] = n / max(np.linalg.norm(n), 1e-9)
    return out


def color_schemes(folder):
    """The car's color schemes: its COL folders, in order."""
    d = os.path.join(arcade.DATA, 'CARS', folder)
    return sorted((c for c in os.listdir(d) if c.upper().startswith('COL')), key=lambda c: int(c[3:]))


def convert(folder, col, dz=0.0, obj='CARBODY'):
    """Color scheme `col` of the car (object `obj`: the body, or HOOD, the hood camera's hood) as a glb, and its
    triangle count (None without that object); dz moves it forward (ft)."""
    t = arcade.Track(os.path.join(arcade.DATA, 'CARS', folder, col))
    if obj not in t.by_name:
        return None, 0
    lod, images = body_textures(t, obj)
    parts = []
    tri_count = 0
    for ti, rgba in images.items():
        tex = t.textures[ti]
        su, sv = tex.uv_scale()
        tris = [tr for tr in lod.tris if tr[3] == ti]
        if not tris:
            continue
        # Mirroring x (into glTF's space) flips the winding: b and c swap to keep each face's front.
        P = np.array([[lod.verts[k][0] for k in (a, c, b)] for a, b, c, _ in tris], float)
        G = (P + np.array([0.0, 0.0, dz])) * np.array([-UNITS, UNITS, UNITS])
        UV = np.array([[(lod.verts[k][1][0] / su, lod.verts[k][1][1] / sv) for k in (a, c, b)] for a, b, c, _ in tris])
        N = smooth_normals(G)
        mesh = Mesh(tex.name, tex.name, None, G.reshape(-1, 3), N.reshape(-1, 3), UV.reshape(-1, 2),
                    np.arange(len(tris) * 3).reshape(-1, 3))
        parts.append((mesh, Image.fromarray(rgba, 'RGBA'), None))
        tri_count += len(tris)
    return write_glb(parts, '%s_%s_%s' % (folder, col, obj)), tri_count


def write_hood(out, folder, col, name, dz=0.0):
    """The scheme's hood camera hood as glb `name` in `out`; its name, or "" without one."""
    glb, tris = convert(folder, col, dz, 'HOOD')
    if glb is None:
        return ''
    with open(os.path.join(out, name), 'wb') as fp:
        fp.write(glb)
    print('%s/%s HOOD -> %s: %d triangles' % (folder, col, name, tris))
    return name


def render_side(folder, car, S=48):
    """The body's right side (textured, +z to the left) on a 1 ft grid with the wheels drawn at their centers."""
    t = arcade.Track(os.path.join(arcade.DATA, 'CARS', folder, 'COL1'))
    lod, images = body_textures(t)
    W, H = 17 * S, 6 * S
    col = np.full((H, W, 3), 255, np.uint8)
    depth = np.full((H, W), -1e9)
    for a, b, c, ti in lod.tris:
        if ti not in images:
            continue
        im = images[ti]
        su, sv = t.textures[ti].uv_scale()
        P = np.array([lod.verts[k][0] for k in (a, b, c)])
        UV = np.array([lod.verts[k][1] for k in (a, b, c)]) / np.array([su, sv])
        X, Y = (8.5 - P[:, 2]) * S, (5.0 - P[:, 1]) * S
        x0, x1 = int(max(0, np.floor(X.min()))), int(min(W - 1, np.ceil(X.max())))
        y0, y1 = int(max(0, np.floor(Y.min()))), int(min(H - 1, np.ceil(Y.max())))
        den = (Y[1] - Y[2]) * (X[0] - X[2]) + (X[2] - X[1]) * (Y[0] - Y[2])
        if abs(den) < 1e-9 or x1 < x0 or y1 < y0:
            continue
        gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
        w0 = ((Y[1] - Y[2]) * (gx - X[2]) + (X[2] - X[1]) * (gy - Y[2])) / den
        w1 = ((Y[2] - Y[0]) * (gx - X[2]) + (X[0] - X[2]) * (gy - Y[2])) / den
        inside = (w0 >= 0) & (w1 >= 0) & (1 - w0 - w1 >= 0)
        if not inside.any():
            continue
        ww = np.stack([w0, w1, 1 - w0 - w1], -1)[inside]
        px, uv = ww @ P[:, 0], ww @ UV
        ys, xs = np.nonzero(inside)
        ys, xs = ys + y0, xs + x0
        near = px > depth[ys, xs]
        ys, xs, px, uv = ys[near], xs[near], px[near], uv[near]
        depth[ys, xs] = px
        h, w = im.shape[:2]
        col[ys, xs] = im[np.floor(uv[:, 1] * h).astype(int) % h, np.floor(uv[:, 0] * w).astype(int) % w][:, :3]
    img = Image.fromarray(col)
    d = ImageDraw.Draw(img)
    for y in range(6):
        d.line([(0, y * S), (W, y * S)], fill=(200, 200, 255))
    for z in range(-8, 9):
        d.line([((8.5 - z) * S, 0), ((8.5 - z) * S, H)], fill=(255, 0, 0) if z == 0 else (200, 200, 255))
    d.text((4, 4), '%s %s' % (folder, car['name']), fill=(0, 0, 0))
    for (x, z), y, r in ((FRONT, WHEEL_Y[0], SCALE[0]), (REAR, WHEEL_Y[1], SCALE[1])):
        r *= 15.0 / 16.0
        d.ellipse([((8.5 - z - r) * S, (5 - y - r) * S), ((8.5 - z + r) * S, (5 - y + r) * S)], outline=(0, 160, 0),
                  width=2)
    return img


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out_dir')
    ap.add_argument('--cars', default=','.join(CARS), help='arcade car folders (default: every one in CARS)')
    ap.add_argument('--mod-id', default='rush2049_arcade_cars')
    ap.add_argument('--sides', help='also draw the side views with the wheels to this PNG')
    a = ap.parse_args()
    folders = [c.strip().upper() for c in a.cars.split(',') if c.strip()]
    for f in folders:
        if f not in CARS:
            sys.exit('%s: not one of %s' % (f, ', '.join(CARS)))
    out = os.path.join(a.out_dir, 'rush2', 'new_cars')
    os.makedirs(out, exist_ok=True)
    for f in folders:
        car = CARS[f]
        for old in os.listdir(out):  # schemes of an earlier run
            if old.upper().startswith(car['id'] + '_') or old.upper() == car['id'] + '.GLB':
                os.remove(os.path.join(out, old))
        files, hoods = [], []
        for col in color_schemes(f):
            glb, tris = convert(f, col)
            name = '%s_%s.glb' % (car['id'], col)
            with open(os.path.join(out, name), 'wb') as fp:
                fp.write(glb)
            files.append(name)
            print('%s/%s -> %s: %d triangles' % (f, col, name, tris))
            hoods.append(write_hood(out, f, col, '%s_%s_HOOD.glb' % (car['id'], col)))
        spec = {'name': car['name'], 'base': car['base'], 'schemes': files, 'hoods': hoods, 'box': box(f),
                'wheels': {'front': [FRONT[0] * UNITS, WHEEL_Y[0] * UNITS, FRONT[1] * UNITS],
                           'rear': [REAR[0] * UNITS, WHEEL_Y[1] * UNITS, REAR[1] * UNITS]},
                'wheel_scale': list(SCALE),
                'source': 'San Francisco Rush 2049 SE (arcade) %s, tools/arcade2049/car_mod.py' % f}
        with open(os.path.join(out, car['id'] + '.json'), 'w', newline='\n') as fp:
            json.dump(spec, fp, indent=4)
    # Liveries: rush2/liveries/<ID>.json + <ID>_COLn.glb per N64 car.
    lout = os.path.join(a.out_dir, 'rush2', 'liveries')
    os.makedirs(lout, exist_ok=True)
    for old in os.listdir(lout):
        os.remove(os.path.join(lout, old))
    for car, l in LIVERIES.items():
        lid = car.replace(' ', '').replace('-', '')
        files, hoods = [], []
        for c in l['cols']:
            glb, tris = convert(l['folder'], 'COL%d' % c, l['dz'])
            name = '%s_COL%d.glb' % (lid, c)
            with open(os.path.join(lout, name), 'wb') as fp:
                fp.write(glb)
            files.append(name)
            print('%s/COL%d -> livery %s: %d triangles' % (l['folder'], c, name, tris))
            hoods.append(write_hood(lout, l['folder'], 'COL%d' % c, '%s_COL%d_HOOD.glb' % (lid, c), l['dz']))
        with open(os.path.join(lout, lid + '.json'), 'w', newline='\n') as fp:
            json.dump({'car': car, 'styles': files, 'hoods': hoods,
                       'source': 'San Francisco Rush 2049 SE (arcade) %s, tools/arcade2049/car_mod.py' % l['folder']},
                      fp, indent=4)
    mod_json = os.path.join(a.out_dir, 'mod.json')
    if not os.path.exists(mod_json):
        with open(mod_json, 'w', newline='\n') as fp:
            json.dump(mod_manifest(a.mod_id), fp, indent=4)
    if a.sides:
        rows = [render_side(f, CARS[f]) for f in folders]
        sheet = Image.new('RGB', (rows[0].width, sum(r.height for r in rows)))
        y = 0
        for r in rows:
            sheet.paste(r, (0, y))
            y += r.height
        sheet.save(a.sides)
        print('wrote', a.sides)


if __name__ == '__main__':
    main()
