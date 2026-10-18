"""Trackview's built-in tracks from the other games: San Francisco Rush's 7 and California Speed's 16, read from the
user's ROMs and shown with the same collision, AI path and checkpoint layers as a custom track.

They aren't track folders, so the viewer lists them under the virtual folder names `stock:sf:<0-6>` and
`stock:cs:<0-15>` (`is_stock`). What is drawn is what the game loads, built without the game running:
- SF Rush: tools/rush1/track1.py's convert_collision / convert_path (byte-identical with src/rush1/track1_convert.cpp).
  Rush 1 has a collision and a path per direction, so these have a Backward view.
- California Speed: assets 94 + t (collision) and 110 + t (path) as they are, with the surface remap of
  src/calspeed/calspeed_convert.cpp remap_surfaces (verges become grass, the other walls 5). The game also splits
  the lanes' long steps and moves checkpoint 0 on point-to-point tracks (not drawn here). No backward races.
The visible-model layer is the track as placed (tools/trackview/track_glb.cpp, the model dump's write_glb on the
game's geometry and placement files; glb_path), in feet where custom tracks' glbs are meters (`GLB_SCALE`). AI runs use the game's track select id (`menu_id`).

ROMs: build/rush1.z64, build/calspeed.z64 (RUSH1_ROM / CALSPEED_ROM override).

    python tools/trackview/stock.py stock:sf:0 [OUT.json]
"""
import hashlib, json, os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
BUILD = os.path.join(REPO, 'build')
for var, name in (('RUSH1_ROM', 'rush1.z64'), ('CALSPEED_ROM', 'calspeed.z64')):
    if var not in os.environ and os.path.exists(os.path.join(BUILD, name)):
        os.environ[var] = os.path.join(BUILD, name)
for d in ('rush2049', 'rush1', 'calspeed', 'trackview'):
    sys.path.insert(0, os.path.join(REPO, 'tools', d))
import bake  # noqa: E402

SF_NAMES = ['Golden Gate', 'Embarcadero', 'Market', 'Downtown', 'The Heights', 'Sunset', 'The Rock']
CS_NAMES = ['Monterey', 'Silicon Valley', 'Highway 1', 'San Francisco', 'Central Valley', 'Santa Cruz', 'San Diego',
            'Clover', 'Los Angeles', 'Mt. Shasta', 'Yosemite', 'Mojave Desert', 'Fuji', 'Oval', 'California',
            'San Andreas']
GLB_SCALE = 1 / bake.FT   # the dump's placed glb is in game feet (x already mirrored as bake.g2m does)
SF_FIRST_ID, CS_FIRST_ID = 18, 64   # track select ids (include/track1.h, include/calspeed.h)
ROMS = {'sf': 'RUSH1_ROM', 'cs': 'CALSPEED_ROM'}
SOURCES = [__file__, bake.__file__, os.path.join(REPO, 'tools/rush1/track1.py'),
           os.path.join(REPO, 'tools/rush2049/collision.py'), os.path.join(REPO, 'tools/rush2049/paths.py')]


def is_stock(folder):
    return str(folder).startswith('stock:')


def parse(folder):
    """('sf' | 'cs', track index) of a virtual folder name."""
    _, game, t = folder.split(':')
    return game, int(t)


def menu_id(folder):
    game, t = parse(folder)
    return (SF_FIRST_ID if game == 'sf' else CS_FIRST_ID) + t


def run_name(folder):
    """A file-name-safe name for the folder's recordings and logs."""
    return folder.replace(':', '_')


def listing():
    """[{folder, name, group}] for the tracks whose ROM is present."""
    out = []
    for game, names, group in (('sf', SF_NAMES, 'San Francisco Rush'), ('cs', CS_NAMES, 'California Speed')):
        if os.path.exists(os.environ.get(ROMS[game], '')):
            out += [dict(folder='stock:%s:%d' % (game, t), name=n, group=group) for t, n in enumerate(names)]
    return out


def remap_cs(col):
    """src/calspeed/calspeed_convert.cpp remap_surfaces on a collision file."""
    col = bytearray(col)
    segs, nodes, polys = (int.from_bytes(col[i:i + 2], 'big') for i in (0, 2, 4))
    at = 12 + segs * 0x84 + nodes * 0x14
    for _ in range(polys):
        low = col[at + 1]
        typ, mat = low & 0xF, low >> 4
        typ = 2 if typ == 3 else 5 if typ == 4 or typ >= 7 else typ
        col[at + 1] = mat << 4 | typ
        at += 0x18
    return bytes(col)


def cache_key(folder):
    h = hashlib.sha1(folder.encode())
    for p in [os.environ.get(ROMS[parse(folder)[0]], '')] + SOURCES:
        if os.path.exists(p):
            st = os.stat(p)
            h.update(('%s%d%d' % (os.path.basename(p), st.st_size, int(st.st_mtime))).encode())
    return h.hexdigest()[:16]


R49_NAMES = ['Marina', 'Haight', 'Civic', 'Metro', 'Mission', 'Presidio']   # include/track2049.h track_names


def dump_glb(folder):
    """The model dump's textured placed glb (build/asset_dump/models, Settings > Dump Models) of a Rush 2 or Rush 2049
    track exported to tmp/stock_tracks (tools/stock_track_glb.py: r2_<slot>, r49_race<k>), or None."""
    name = os.path.basename(os.path.abspath(folder))
    if name.startswith('r2_'):
        sub = 'rush2/tracks/' + name[3:].upper()
    elif name.startswith('r49_race') and name[8:].isdigit() and 1 <= int(name[8:]) <= 6:
        sub = 'rush2049/tracks/' + R49_NAMES[int(name[8:]) - 1]
    else:
        return None
    path = os.path.join(BUILD, 'asset_dump', 'models', sub, '_placed.glb')
    return path if os.path.exists(path) else None


def track_files(folder):
    """(geometry, placement) of a virtual folder's track as the game loads it."""
    game, t = parse(folder)
    if game == 'sf':
        import track1
        from r1 import Rush1
        r = Rush1()
        g, names, shifts = track1.convert_geometry(r, t, 'HAWAII')
        return g, track1.convert_placement(r, t, 'HAWAII', names, None, shifts)
    from cs import CalSpeed
    c = CalSpeed()
    return c.asset(62 + t), c.asset(78 + t)


EXE = os.path.join(REPO, 'tmp', 'track_glb', 'track_glb.exe')
BAT = os.path.join(REPO, 'tools', 'trackview', 'track_glb.bat')


def glb_path(folder):
    """The track's placed glb (built once per ROM and exporter; None if the exporter fails)."""
    game, t = parse(folder)
    out = os.path.join(bake.CACHE, 'stock_%s_%d-%s.glb' % (game, t, cache_key(folder)))
    if os.path.exists(out):
        return out
    src = [os.path.join(REPO, p) for p in ('tools/trackview/track_glb.cpp', 'src/asset_catalog_walk.cpp',
                                           'include/asset_catalog.h')]
    work = os.path.join(bake.CACHE, 'stock_%s_%d-work' % (game, t))
    os.makedirs(work, exist_ok=True)
    g, p = track_files(folder)
    with open(os.path.join(work, 'g.bin'), 'wb') as f:
        f.write(g)
    with open(os.path.join(work, 'p.bin'), 'wb') as f:
        f.write(p)
    stale = not os.path.exists(EXE) or any(os.path.getmtime(s) > os.path.getmtime(EXE) for s in src)
    args = [os.path.join(work, 'g.bin'), os.path.join(work, 'p.bin'), out + '.tmp', SF_NAMES[t] if game == 'sf' else CS_NAMES[t]]
    cmd = ['cmd', '/c', BAT] + args if stale else [EXE] + args
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, errors='replace')
    if r.returncode != 0 or not os.path.exists(out + '.tmp'):
        print('track_glb failed:', r.stdout[-2000:], r.stderr[-2000:], file=sys.stderr)
        return None
    for old in os.listdir(bake.CACHE):
        if old.startswith('stock_%s_%d-' % (game, t)) and old.endswith('.glb'):
            os.remove(os.path.join(bake.CACHE, old))
    os.replace(out + '.tmp', out)
    return out


def bake_stock(folder, force=False):
    """The baked dict for a virtual folder (the same shape as bake.bake's), cached under tmp/trackview/."""
    game, t = parse(folder)
    names = SF_NAMES if game == 'sf' else CS_NAMES
    cached = os.path.join(bake.CACHE, 'stock_%s_%d-%s.json' % (game, t, cache_key(folder)))
    if not force and os.path.exists(cached):
        with open(cached, encoding='utf-8') as f:
            return json.load(f)
    res = dict(name=names[t], folder=folder, ok=True, report=[], has_glb=True, glb_scale=GLB_SCALE)
    if game == 'sf':
        import track1
        from r1 import Rush1
        r = Rush1()
        for key, back in (('forward', False), ('backward', True)):
            col = track1.convert_collision(r.asset((track1.COLLISION_BACK if back else track1.COLLISION) + t))
            path, _ = track1.convert_path(r, t, back)
            if not back:
                res['collision'] = bake.bake_collision(col, {})
            res[key] = bake.bake_path(path)
    else:
        from cs import CalSpeed
        c = CalSpeed()
        res['collision'] = bake.bake_collision(remap_cs(c.asset(94 + t)), {})
        res['forward'] = bake.bake_path(c.asset(110 + t))
        res['report'] = [dict(level='info', at=[], text='California Speed track as the game loads it before its own '
                              'lane fixes (long steps split, checkpoint 0 moved on point-to-point tracks).')]
    os.makedirs(bake.CACHE, exist_ok=True)
    for old in os.listdir(bake.CACHE):
        if old.startswith('stock_%s_%d-' % (game, t)):
            os.remove(os.path.join(bake.CACHE, old))
    with open(cached, 'w', encoding='utf-8') as f:
        json.dump(res, f, separators=(',', ':'))
    return res


if __name__ == '__main__':
    r = bake_stock(sys.argv[1], force=True)
    if len(sys.argv) > 2:
        with open(sys.argv[2], 'w', encoding='utf-8') as f:
            json.dump(r, f)
    fw = r['forward']
    print('%s: %d polygons, %d triangles, surfaces %s; spine %d points, %d lanes, %d checkpoints%s' % (
        r['name'], r['collision']['polys'], len(r['collision']['surf']), [s['name'] for s in r['collision']['surfaces']],
        len(fw['spine']), len(fw['lanes']), len(fw['checkpoints']), ', backward' if 'backward' in r else ''))
