"""Bakes a custom track folder (track.glb + track.toml) into the JSON the trackview page draws.

Runs the game's own converter outside the game (tools/track_mod_test.cpp, built by tools/track_mod_test.bat), then
reads what it wrote with the Rush 2 format readers (tools/rush2049/collision.py, paths.py), so the page shows the
collision, AI path and checkpoints exactly as the game loads them. The result is cached under tmp/trackview/, keyed
on the track's files, the converter exe and this file.

    python tools/trackview/bake.py TRACK_FOLDER [OUT.json]

Coordinates in the JSON are glTF meters (the frame track.glb is in, so the page can draw the glb as is). The game
works in feet with x mirrored: game = (-x, y, z) * 3.2808399 (to_game in src/track_mod/track_mod_convert.cpp). The
report quotes Blender coordinates (x, y, z up) = glTF (x, -z, y).

See: docs/custom_tracks.md#track-previewer
"""
import hashlib, json, math, os, re, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, 'tools', 'rush2049'))
import collision, paths  # noqa: E402

FT = 3.2808399
EXE = os.path.join(REPO, 'tmp', 'track_mod_test', 'track_mod_test.exe')
BAT = os.path.join(REPO, 'tools', 'track_mod_test.bat')
CACHE = os.path.join(REPO, 'tmp', 'trackview')
#: Sources the converter exe is built from; a newer one rebuilds it.
SOURCES = ['tools/track_mod_test.cpp', 'src/track_mod/track_mod_convert.cpp', 'src/gltf_scene.cpp',
           'include/track_mod.h']

#: Stock surfaces by collision flags & 0xFF (`known` in track_mod_convert.cpp), then by type alone.
STOCK = {0x00: 'ROAD', 0x11: 'DIRT', 0x22: 'GRASS', 0x05: 'WALL', 0x03: 'WATER', 0x07: 'KILL'}
TYPE_NAMES = {0: 'ROAD', 1: 'DIRT', 2: 'GRASS', 3: 'WATER', 4: 'WATER', 5: 'WALL', 6: 'WALL', 7: 'KILL'}
#: A custom surface's `like` -> its collision type (the material slot goes in bits 4-7).
LIKE_TYPE = {'road': 0, 'dirt': 1, 'grass': 2}


def g2m(p):
    """Game feet -> glTF meters."""
    return [round(-p[0] / FT, 3), round(p[1] / FT, 3), round(p[2] / FT, 3)]


def converter_stale():
    if not os.path.exists(EXE):
        return True
    t = os.path.getmtime(EXE)
    return any(os.path.getmtime(os.path.join(REPO, s)) > t for s in SOURCES)


def convert(folder, outdir):
    """Runs the converter (rebuilding it first when its sources changed). Returns (ok, console text)."""
    os.makedirs(outdir, exist_ok=True)
    if converter_stale():
        cmd = ['cmd', '/c', BAT, folder, outdir]
    else:
        cmd = [EXE, folder, outdir]
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, errors='replace')
    return r.returncode == 0, r.stdout + r.stderr


def cache_key(folder):
    h = hashlib.sha1()
    for name in ('track.glb', 'track.toml'):
        p = os.path.join(folder, name)
        if os.path.exists(p):
            with open(p, 'rb') as f:
                h.update(f.read())
    for p in [EXE, os.path.abspath(__file__)] + [os.path.join(REPO, s) for s in SOURCES]:
        if os.path.exists(p):
            h.update(str(os.path.getmtime(p)).encode())
    return h.hexdigest()[:16]


BLENDER_AT = re.compile(r'\((-?[\d.]+), (-?[\d.]+), (-?[\d.]+)\)')


def report_messages(text):
    """The converter's report lines as {level, text, at[]} with each quoted Blender position in glTF meters."""
    out = []
    for line in text.splitlines():
        if not line.strip() or line.startswith('OK: wrote') or 'vswhere' in line or line.startswith('operable'):
            continue
        level = 'error' if line.startswith('Error') or line == 'FAILED' else \
            'warning' if line.startswith('Warning') else 'info'
        at = [[float(x), float(z), -float(y)] for x, y, z in BLENDER_AT.findall(line)]
        out.append(dict(level=level, text=line, at=at))
    return out


def custom_surfaces(report_text):
    """{flags & 0xFF: name} for the custom surfaces, from the report's "Surface COL_X: like dirt, ... (slot N)." lines."""
    out = {}
    for name, like, slot in re.findall(r'Surface COL_(\w+): like (\w+),.*?\(slot (\d+)\)', report_text):
        out[LIKE_TYPE.get(like, 0) | int(slot) << 4] = name
    return out


def bake_collision(data, custom):
    c = collision.Collision.parse(data, collision.R2)
    names = []
    index = {}
    pos, surf = [], []
    for p in c.polys:
        key = p['flags'] & 0xFF
        name = custom.get(key) or STOCK.get(key) or TYPE_NAMES.get(key & 0xF, 'TYPE_%d' % (key & 0xF))
        if name not in index:
            index[name] = len(names)
            names.append(name)
        vs = [g2m(v) for v in collision.world_polygon(c, p)]
        for i in range(1, len(vs) - 1):
            # glTF is the game mirrored, so the fan's winding flips with it
            for v in (vs[0], vs[i + 1], vs[i]):
                pos.extend(v)
            surf.append(index[name])
    kinds = {n: ('custom' if n in custom.values() else 'stock') for n in names}
    return dict(surfaces=[dict(name=n, kind=kinds[n]) for n in names], pos=pos, surf=surf, polys=len(c.polys))


def bake_path(data):
    p = paths.parse(data)
    rc = paths.recompute(p)
    spine = [g2m(q) for q in p['spine']]
    lanes = [dict(pts=[g2m(q[:3]) for q in ln['points']], speed=[q[3] for q in ln['points']],
                  behavior=[q[4] for q in ln['points']]) for ln in p['lanes']]
    cps = []
    for i, c in enumerate(p['cps']):
        # A custom track keeps the crossings the converter stored; -1 leaves a gate to the game's own search.
        cross = [s if s >= 0 else r for s, r in zip(c['cross'][:5], rc['cross'][i][:5])]
        d = [-x for x in c['dir']]  # stored pointing back against the race
        cps.append(dict(pos=g2m(c['pos']), dir=[-d[0], d[1], d[2]], radius=round(math.sqrt(c['r2']) / FT, 2),
                        flags=c['flags'], cross=cross, dist=round(c['dist'] / FT, 1)))
    return dict(spine=spine, lanes=lanes, checkpoints=cps,
                branches=[[g2m(q) for q in b['points']] for b in p['branches']])


def bake(folder, force=False):
    """Returns the baked dict for a track folder (from the cache when nothing changed)."""
    folder = os.path.abspath(folder)
    key = cache_key(folder)
    tag = re.sub(r'\W', '_', os.path.basename(folder))
    cached = os.path.join(CACHE, '%s-%s.json' % (tag, key))
    if not force and os.path.exists(cached):
        with open(cached, encoding='utf-8') as f:
            return json.load(f)
    outdir = os.path.join(CACHE, tag + '-out')
    for n in ('geometry.bin', 'collision.bin', 'path.bin', 'path_backward.bin', 'placement.bin'):
        try:
            os.remove(os.path.join(outdir, n))
        except OSError:
            pass
    ok, text = convert(folder, outdir)
    try:
        with open(os.path.join(outdir, 'report.txt'), encoding='utf-8', errors='replace') as f:
            report = f.read()
    except OSError:
        report = text
    res = dict(name=os.path.basename(folder), folder=folder, ok=ok, report=report_messages(report))
    if not ok and not os.path.exists(os.path.join(outdir, 'collision.bin')):
        res['console'] = text[-4000:]
        return res
    with open(os.path.join(outdir, 'collision.bin'), 'rb') as f:
        res['collision'] = bake_collision(f.read(), custom_surfaces(report))
    with open(os.path.join(outdir, 'path.bin'), 'rb') as f:
        res['forward'] = bake_path(f.read())
    if os.path.exists(os.path.join(outdir, 'path_backward.bin')):
        with open(os.path.join(outdir, 'path_backward.bin'), 'rb') as f:
            res['backward'] = bake_path(f.read())
    res['has_glb'] = os.path.exists(os.path.join(folder, 'track.glb'))
    for old in os.listdir(CACHE):
        if old.startswith(tag + '-') and old.endswith('.json'):
            os.remove(os.path.join(CACHE, old))
    with open(cached, 'w', encoding='utf-8') as f:
        json.dump(res, f, separators=(',', ':'))
    return res


if __name__ == '__main__':
    r = bake(sys.argv[1], force=True)
    if len(sys.argv) > 2:
        with open(sys.argv[2], 'w', encoding='utf-8') as f:
            json.dump(r, f)
    for m in r['report']:
        print(m['level'][0].upper(), m['text'])
    if 'collision' in r:
        c = r['collision']
        print('%d polygons, %d triangles, surfaces %s' % (c['polys'], len(c['surf']), [s['name'] for s in c['surfaces']]))
        fw = r['forward']
        print('spine %d points, lanes %s, %d checkpoints' % (len(fw['spine']), [len(l['pts']) for l in fw['lanes']],
                                                             len(fw['checkpoints'])))
