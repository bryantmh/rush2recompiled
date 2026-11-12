#!/usr/bin/env python3
"""Car Lights' lamp review (src/car_lights.cpp, docs/graphics_effects.md#car-lights): draws every car the game dumped
with its lamps marked, so the lamps found on the bodies can be checked and corrected in assets/car_lamps.json.

    python tools/car_lamps.py sweep DUMP_DIR [--types 0-21,23-72] [--dreamcast] [--livery N] [--track 17]
    python tools/car_lamps.py sheet DUMP_DIR OUT.png [--only KEY ...] [--size 220] [--table assets/car_lamps.json]
                                  [--grid] [--views front back side top]
    python tools/car_lamps.py table DUMP_DIR [--only KEY ...]
    python tools/car_lamps.py pick DUMP_DIR KEY head|tail --x LO HI --y LO HI [--rel 0.6] [--min 0] [--color r,g,b]
                                 [--neutral] [--z LO HI] [--facing F] [--single | --mirror] [--size R] [--no-lens]
                                 [--save assets/car_lamps.json [--add]]
    python tools/car_lamps.py glb CAR.glb DUMP_DIR/NAME.json --key KEY [--lamps DUMP_DIR] [--n 24]

DUMP_DIR is what a race run with RUSH2_CAR_LIGHTS_DUMP=<folder> writes: one <car key>.json per car body met (its
triangles in the car's own axes, feet, with a color per quarter, and its lamps with where each came from).
`sweep` fills it: races on a night track (tools/test_race.py, hidden and silent) with the player's car and the seven
opponents' (npc_cars.json) set to the next eight types, until every type is covered; --dreamcast takes the Rush 2049
cars from the Dreamcast disc, --livery N shows STYLE N on every car with mod liveries (a livery is its own
car key, "<car>/<livery file>"). Types the run has no car for (mod slots nothing fills) are skipped by the game.

`sheet` draws one row per car: front, back, side and top views, the body flat shaded by its texels, each lamp as a
ring of its lens's radius with a cross at its middle, headlamps cyan and tail lamps yellow (dashed: from the body's
shape, a guess).
`table` prints the lamps as assets/car_lamps.json entries, to paste in and correct.
`pick` measures a lamp for a hand entry: the texel samples on the triangles facing the lamp's way (front for a
headlamp, back for a tail lamp, as the game's finder) inside the box (feet, the car's x and y), kept when their
brightest channel is at least --rel of the brightest there (and --min), or near --color; it prints the lamp and its
mirror (the box mirrored across the body's middle, measured on its own) as entries: the samples' middle and a radius of
sqrt 2 times their RMS distance from it, the game's own rule.
"""
import argparse, glob, json, math, os, re, subprocess, sys
from PIL import Image, ImageDraw, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))

VIEWS = [  # name, (horizontal axis, sign), (vertical axis, sign), depth axis and sign toward the viewer
    ('front', (0, -1), (1, 1), (2, 1)),
    ('back', (0, 1), (1, 1), (2, -1)),
    ('side', (2, 1), (1, 1), (0, 1)),
    ('top', (0, -1), (2, 1), (1, 1)),
]


def font(size):
    for name in ('arialbd.ttf', 'arial.ttf', 'DejaVuSans-Bold.ttf', 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def load(folder, only):
    cars = []
    for path in sorted(glob.glob(os.path.join(folder, '*.json'))):
        with open(path) as f:
            j = json.load(f)
        if only and j['key'] not in only:
            continue
        cars.append(j)
    return cars


def draw_view(car, view, size, grid=False):
    _, (ha, hs), (va, vs), (da, ds) = view
    img = Image.new('RGB', (size, size), (24, 26, 32))
    d = ImageDraw.Draw(img)
    pts = [car['tris'][i]['p'][k * 3:k * 3 + 3] for i in range(len(car['tris'])) for k in range(3)]
    if not pts:
        return img
    lo = [min(p[a] for p in pts) for a in range(3)]
    hi = [max(p[a] for p in pts) for a in range(3)]
    span = max(hi[ha] - lo[ha], hi[va] - lo[va], 1e-3)
    scale = (size - 16) / span
    cx, cy = (lo[ha] + hi[ha]) / 2, (lo[va] + hi[va]) / 2

    def to2d(p):
        return (size / 2 + hs * (p[ha] - cx) * scale, size / 2 - vs * (p[va] - cy) * scale)

    # Each triangle as the game sampled it: an n x n grid of sub-triangles, row a, step c: the one pointing up, then
    # (but for the last of the row) the one pointing down (src/car_lights.cpp dump), far ones first.
    subs = []
    for t in car['tris']:
        p = [t['p'][0:3], t['p'][3:6], t['p'][6:9]]
        n = t.get('n', 1)

        def g(a, c):
            return [p[0][k] + a / n * (p[1][k] - p[0][k]) + c / n * (p[2][k] - p[0][k]) for k in range(3)]
        quads = []
        for a in range(n):
            for c in range(n - a):
                quads.append((g(a, c), g(a + 1, c), g(a, c + 1)))
                if a + c < n - 1:
                    quads.append((g(a + 1, c), g(a + 1, c + 1), g(a, c + 1)))
        # Shade by how squarely the face looks at the viewer.
        e1 = [p[1][k] - p[0][k] for k in range(3)]
        e2 = [p[2][k] - p[0][k] for k in range(3)]
        n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
        ln = math.sqrt(sum(c * c for c in n)) or 1.0
        shade = 0.55 + 0.45 * abs(n[da] / ln)
        for q, c in zip(quads, t['c']):
            depth = sum(v[da] for v in q) / 3 * ds
            subs.append((depth, q, tuple(int(min(255, ch * shade)) for ch in c)))
    subs.sort(key=lambda s: s[0])
    for _, q, c in subs:
        d.polygon([to2d(v) for v in q], fill=c)
    if grid and view[0] in ('front', 'back'):
        # Half-foot lines in the car's x and y, labeled each foot, for `pick` boxes.
        g = (90, 90, 110)
        for k in range(-20, 21):
            v = k * 0.5
            x, _ = to2d([v if i == ha else 0 for i in range(3)])
            _, y = to2d([v if i == va else 0 for i in range(3)])
            if 0 <= x < size:
                d.line([x, 0, x, size], fill=g if k % 2 else (130, 130, 160))
                if k % 2 == 0:
                    d.text((x + 2, size - 14), '%g' % v, fill=(200, 200, 220), font=font(10))
            if 0 <= y < size:
                d.line([0, y, size, y], fill=g if k % 2 else (130, 130, 160))
                if k % 2 == 0:
                    d.text((2, y - 12), '%g' % v, fill=(200, 200, 220), font=font(10))
    for lamp in car['lamps']:
        # Only the lamps on the side the view looks at (a front view shows no tail lamps).
        if view[0] == 'front' and lamp['kind'] != 'head' or view[0] == 'back' and lamp['kind'] == 'head':
            continue
        x, y = to2d(lamp['at'])
        # A ring of the lamp's lens radius, and a cross at its middle (dashed ring: from the body's shape, a guess).
        r = max(3.0, lamp['size'] * scale)
        color = (40, 255, 255) if lamp['kind'] == 'head' else (255, 255, 40)
        guess = lamp.get('from') == 'body shape'
        if guess:
            for k in range(0, 360, 30):
                d.arc([x - r, y - r, x + r, y + r], k, k + 15, fill=color, width=2)
        else:
            d.ellipse([x - r, y - r, x + r, y + r], outline=color, width=1)
        d.line([x - 3, y, x + 3, y], fill=color)
        d.line([x, y - 3, x, y + 3], fill=color)
    d.text((4, 2), view[0], fill=(160, 160, 170), font=font(12))
    return img


def apply_table(cars, path):
    # The hand entries over what was found, kind by kind, as the game applies them.
    with open(path) as f:
        table = json.load(f).get('cars', {})
    for car in cars:
        hand = table.get(car['key'])
        if not hand:
            continue
        for kind in ('head', 'tail'):
            mine = [dict(l, **{'from': 'by hand'}) for l in hand if l.get('kind') == kind]
            if mine:
                car['lamps'] = [l for l in car['lamps'] if l['kind'] != kind] + mine


def sheet(a):
    cars = load(a.dump, a.only)
    if not cars:
        sys.exit('no cars in ' + a.dump)
    if a.table:
        apply_table(cars, a.table)
    size = a.size
    label_w = 230
    views = [v for v in VIEWS if not a.views or v[0] in a.views]
    out = Image.new('RGB', (label_w + size * len(views), size * len(cars)), (12, 12, 16))
    d = ImageDraw.Draw(out)
    f = font(15)
    small = font(12)
    for row, car in enumerate(cars):
        y = row * size
        d.text((8, y + 8), car['key'], fill=(240, 240, 240), font=f)
        d.text((8, y + 28), 'type %d, %d triangles' % (car['type'], len(car['tris'])), fill=(170, 170, 180), font=small)
        for k, lamp in enumerate(car['lamps']):
            d.text((8, y + 48 + k * 15), '%s (%.2f, %.2f, %.2f) %s' % (lamp['kind'], *lamp['at'], lamp.get('from', '')),
                   fill=(255, 220, 40) if lamp['kind'] == 'head' else (255, 90, 90), font=small)
        for v, view in enumerate(views):
            out.paste(draw_view(car, view, size, a.grid), (label_w + v * size, y))
    out.save(a.out)
    print('wrote', a.out, len(cars), 'cars')


def table(a):
    entries = {}
    for car in load(a.dump, a.only):
        entries[car['key']] = [{'kind': l['kind'], 'at': [round(c, 2) for c in l['at']], 'size': round(l['size'], 2)}
                               for l in car['lamps']]
    print(json.dumps(entries, indent=2))


def glb(a):
    # A car glb (a livery's body, a mod car's) as a dump, sampled from its full-size textures: the game's dump reads the
    # N64-size copies, too coarse to place a livery's lamps from. In the car's axes as a mod's "lights" are
    # (src/car_mods.cpp: x negated, 1/16 ft).
    sys.path.insert(0, HERE)
    import numpy as np
    import glb_view
    gltf, bin_ = glb_view.load(a.glb)
    texs = glb_view.textures(gltf, bin_)
    P, U, C, T = glb_view.triangles(gltf, bin_, [])
    n = a.n
    tris = []
    for p, uv, col, tex in zip(P, U, C, T):
        q = [[-p[k][0] / 16, p[k][1] / 16, p[k][2] / 16] for k in range(3)]
        colors = []
        for i in range(n):
            for c in range(n - i):
                for g in ((i + 1 / 3, c + 1 / 3),) + (((i + 2 / 3, c + 2 / 3),) if i + c < n - 1 else ()):
                    wb, wc = g[0] / n, g[1] / n
                    w = (1 - wb - wc, wb, wc)
                    u = sum(w[k] * uv[k][0] for k in range(3))
                    v = sum(w[k] * uv[k][1] for k in range(3))
                    shade = [sum(w[k] * col[k][j] for k in range(3)) for j in range(3)]
                    if tex >= 0:
                        img, ws, wt = texs[tex]
                        h, wd = img.shape[0], img.shape[1]
                        x = int(glb_view.wrap(np.array(math.floor(u * wd)), wd, ws))
                        y = int(glb_view.wrap(np.array(math.floor(v * h)), h, wt))
                        rgb = img[y, x, :3]
                    else:
                        rgb = (1, 1, 1)
                    colors.append([int(min(255, 255 * rgb[j] * shade[j])) for j in range(3)])
        tris.append({'p': [x for v3 in q for x in v3], 'n': n, 'c': colors})
    lamps = []
    if a.lamps:
        for car in load(a.lamps, [a.key]):
            lamps = car['lamps']
    out = {'key': a.key, 'type': -1, 'tris': tris, 'lamps': lamps}
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w') as f:
        json.dump(out, f)
    print('wrote', a.out, len(tris), 'triangles')


def facing(t, center):
    p = [t['p'][0:3], t['p'][3:6], t['p'][6:9]]
    e1 = [p[1][k] - p[0][k] for k in range(3)]
    e2 = [p[2][k] - p[0][k] for k in range(3)]
    n = [e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]]
    ln = math.sqrt(sum(c * c for c in n))
    if ln < 1e-9:
        return None
    n = [c / ln for c in n]
    mid = [(p[0][k] + p[1][k] + p[2][k]) / 3 for k in range(3)]
    if sum((mid[k] - center[k]) * n[k] for k in range(3)) < 0:
        n = [-c for c in n]
    return n


def samples(t):
    # The sub-triangles' middles in the dump's order (src/car_lights.cpp dump), with their colors.
    p = [t['p'][0:3], t['p'][3:6], t['p'][6:9]]
    n = t.get('n', 1)
    out = []
    for a in range(n):
        for c in range(n - a):
            for g in ((a + 1 / 3, c + 1 / 3),) + (((a + 2 / 3, c + 2 / 3),) if a + c < n - 1 else ()):
                wb, wc = g[0] / n, g[1] / n
                out.append([p[0][k] + wb * (p[1][k] - p[0][k]) + wc * (p[2][k] - p[0][k]) for k in range(3)])
    return list(zip(out, t['c']))


def pick(a):
    cars = load(a.dump, [a.key])
    if not cars:
        sys.exit('no car ' + a.key)
    car = cars[0]
    pts = [car['tris'][i]['p'][k * 3:k * 3 + 3] for i in range(len(car['tris'])) for k in range(3)]
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    center = [(lo[k] + hi[k]) / 2 for k in range(3)]
    head = a.kind == 'head'
    want = [float(c) for c in a.color.split(',')] if a.color else None

    # What the front (back) view sees: the frontmost (rearmost) z of every triangle's samples per 0.08 ft cell.
    cell = 0.08
    sign = 1 if head else -1
    seen = {}
    for t in car['tris']:
        for q, _ in samples(t):
            k = (int(math.floor(q[0] / cell)), int(math.floor(q[1] / cell)))
            seen[k] = max(seen.get(k, -1e9), sign * q[2])

    def visible(q):
        k = (int(math.floor(q[0] / cell)), int(math.floor(q[1] / cell)))
        return sign * q[2] >= seen[k] - 0.25

    def measure(x0, x1):
        got = []
        for t in car['tris']:
            n = facing(t, center)
            if n is None or (n[2] < a.facing if head else n[2] > -a.facing):
                continue
            for q, c in samples(t):
                if x0 <= q[0] <= x1 and a.y[0] <= q[1] <= a.y[1] and (not a.z or a.z[0] <= q[2] <= a.z[1]) and (a.hidden or visible(q)):
                    got.append((q, c))
        if a.neutral:
            # Gray or white texels only (channels within 30% of the brightest), as the game's headlamp rule.
            got = [(q, c) for q, c in got if max(c) - min(c) <= 0.3 * max(c) and max(c) >= max(a.min, 40)]
        if want:
            got = [(q, c) for q, c in got if math.dist(c, want) <= a.tolerance]
        else:
            top = max((max(c) for _, c in got), default=0)
            got = [(q, c) for q, c in got if max(c) >= max(a.rel * top, a.min)]
        if not got:
            return None
        m = [sum(q[k] for q, _ in got) / len(got) for k in range(3)]
        rms = math.sqrt(sum(sum((q[k] - m[k]) ** 2 for k in range(3)) for q, _ in got) / len(got))
        mean = [round(sum(c[k] for _, c in got) / len(got)) for k in range(3)]
        # The lens's color goes in too: the game takes the texels near it as the lens (not the brightest, which can be
        # the paint round a red lamp).
        return ({'kind': a.kind, 'at': [round(c, 2) for c in m], 'size': round(math.sqrt(2) * rms, 2), 'color': mean},
                '%d samples, mean color %s' % (len(got), mean))

    mirror = sorted((2 * center[0] - a.x[1], 2 * center[0] - a.x[0]))
    entries = []
    for x0, x1 in ([a.x] if a.single or a.mirror else [a.x, mirror]):
        r = measure(x0, x1)
        if r is None:
            print('nothing in x %.2f..%.2f' % (x0, x1))
            continue
        if a.size:
            r[0]['size'] = a.size
        if a.no_lens:
            r[0]['lens'] = False
            r[0].pop('color', None)
        entries.append(r[0])
        print(json.dumps(r[0]) + ',', '  #', r[1])
        if a.mirror:
            # The other side as this one mirrored across the body's middle (that side's texels take in paint).
            m = dict(r[0], at=[round(2 * center[0] - r[0]['at'][0], 2)] + r[0]['at'][1:])
            entries.append(m)
            print(json.dumps(m) + ',', '  # mirrored')
    if a.save and entries:
        # Into the table: this car's entries of this kind replaced (--add: added to), the rest kept.
        with open(a.save, newline='') as f:
            text = f.read()
        t = json.loads(text)
        cars = t.setdefault('cars', {})
        old = cars.get(a.key, [])
        if not a.add:
            old = [e for e in old if e.get('kind') != a.kind]
        cars[a.key] = old + entries
        t['cars'] = dict(sorted(cars.items(), key=lambda kv: [int(p) if p.isdigit() else p for p in re.split(r'(\d+)', kv[0])]))
        out = json.dumps(t, indent=2)
        # One lamp a line.
        out = re.sub(r'\{\s+"kind": ([^{}]*?)\s+\}', lambda m: '{"kind": ' + re.sub(r'\s+', ' ', m.group(1)) + '}', out)
        out = re.sub(r'\[\s+(-?[\d.]+),\s+(-?[\d.]+),\s+(-?[\d.]+)\s+\]', r'[\1, \2, \3]', out)
        eol = '\r\n' if '\r\n' in text else '\n'
        with open(a.save, 'w', newline='') as f:
            f.write(out.replace('\n', eol) + eol)
        print('saved', a.key, a.kind, 'in', a.save)


def parse_types(text):
    out = []
    for part in text.split(','):
        lo, _, hi = part.partition('-')
        out += list(range(int(lo), int(hi or lo) + 1))
    return [t for t in out if t != 22]


def sweep(a):
    dump = os.path.abspath(a.dump)
    types = parse_types(a.types)
    for n in range(0, len(types), 8):
        batch = types[n:n + 8]
        cmd = [sys.executable, os.path.join(HERE, 'test_race.py'), str(a.track), '--cars', str(batch[0]),
               '--seconds', str(a.seconds), '--out', os.path.join(dump, '_run'), '--env', 'RUSH2_CAR_LIGHTS_DUMP=' + dump]
        for k, t in enumerate(batch[1:]):
            cmd += ['--config', 'npc_cars.npc%d_car=%d' % (k + 1, t)]
        if a.dreamcast:
            cmd += ['--games', 'rush2049_source=Dreamcast']
        if a.livery:
            cmd += ['--env', 'RUSH2_TEST_LIVERY=%d' % a.livery]
        print('types', batch, flush=True)
        subprocess.run(cmd, timeout=a.seconds + 90)
    print(len(glob.glob(os.path.join(dump, '*.json'))), 'cars in', dump)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest='cmd', required=True)
    s = sub.add_parser('sheet')
    s.add_argument('dump')
    s.add_argument('out')
    s.add_argument('--only', nargs='*')
    s.add_argument('--size', type=int, default=220)
    s.add_argument('--views', nargs='*', help='just these views (front, back, side, top)')
    s.add_argument('--grid', action='store_true', help='half-foot lines on the front and back views')
    s.add_argument('--table', help='draw these hand entries (assets/car_lamps.json) over the lamps found')
    w = sub.add_parser('sweep')
    w.add_argument('dump')
    w.add_argument('--types', default='0-21,23-72')
    w.add_argument('--dreamcast', action='store_true')
    w.add_argument('--livery', type=int, help='every car with liveries shows this STYLE (RUSH2_TEST_LIVERY)')
    w.add_argument('--track', type=int, default=17)
    w.add_argument('--seconds', type=float, default=48)
    t = sub.add_parser('table')
    t.add_argument('dump')
    t.add_argument('--only', nargs='*')
    p = sub.add_parser('pick')
    p.add_argument('dump')
    p.add_argument('key')
    p.add_argument('kind', choices=['head', 'tail'])
    p.add_argument('--x', type=float, nargs=2, required=True)
    p.add_argument('--y', type=float, nargs=2, required=True)
    p.add_argument('--rel', type=float, default=0.6, help='keep samples at least this share of the brightest')
    p.add_argument('--min', type=float, default=0, help='and at least this bright (0-255, brightest channel)')
    p.add_argument('--color', help='keep samples near this color instead (r,g,b)')
    p.add_argument('--tolerance', type=float, default=40)
    p.add_argument('--facing', type=float, default=0.3, help='the least |normal z| of the triangles (the game: 0.35 heads, 0.25 tails)')
    p.add_argument('--z', type=float, nargs=2, help='only samples in this z range')
    p.add_argument('--neutral', action='store_true', help='gray or white samples only (before --rel)')
    p.add_argument('--hidden', action='store_true', help='also samples behind the frontmost (rearmost) surface')
    p.add_argument('--mirror', action='store_true', help='the other side as this box mirrored, not measured')
    p.add_argument('--single', action='store_true', help='just this box, no mirror (a center lamp)')
    p.add_argument('--size', type=float, help='this radius instead of the measured one')
    p.add_argument('--no-lens', action='store_true', help='"lens": false (a hidden lamp)')
    p.add_argument('--save', help='write the entries into this table (assets/car_lamps.json)')
    p.add_argument('--add', action='store_true', help='with --save: add to the car entries of this kind')
    g = sub.add_parser('glb')
    g.add_argument('glb')
    g.add_argument('out', help='the dump file to write (DIR/<name>.json)')
    g.add_argument('--key', required=True, help='its car key, e.g. rush2049:3/MAGNUM_COL4')
    g.add_argument('--lamps', help='a dump folder to take the key\'s lamps from (to draw them)')
    g.add_argument('--n', type=int, default=24, help='samples per triangle edge')
    a = ap.parse_args()
    {'sheet': sheet, 'table': table, 'sweep': sweep, 'pick': pick, 'glb': glb}[a.cmd](a)


if __name__ == '__main__':
    main()
