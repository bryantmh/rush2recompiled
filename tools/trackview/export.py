"""Trackview's Export: a track as a custom-track folder (track.glb + track.toml + its menu art), zipped, that the
game's converter reads back to the same track (docs/custom_tracks.md#track-previewer).

- A custom track (its own track.glb): the folder's files as they are, byte for byte.
- A stock track (Rush 2's and Rush 2049's exported to tmp/stock_tracks by tools/stock_track_glb.py, San Francisco
  Rush's and California Speed's virtual `stock:` folders): a track.glb built from the game's collision and path files
  as a modder would author it (tools/stock_track_glb.py build_glb: COL_<surface>_stock meshes, FINISH / CHECKPOINT_n
  Empties with the arrow along the race, AIPATH_main, and the four lanes as AIPATH_lane1..4 carrying their _SPEED and
  _AI_MODE), with the track's textured model merged in (the model dump's placed glb, in feet, scaled to meters), and a track.toml with the game's settings for that track (Rush 2049 gravity, California Speed's road hold).

    python tools/trackview/export.py FOLDER OUT.zip
"""
import io, json, os, struct, sys, zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools'))
# Rush 2049's ROM for the tmp/stock_tracks r49_ folders, from build/ unless set (tools/rush2049/roms.py; Rush 2's is
# its decompressed rush2.us.recomp.z64 at the repo root).
if 'RUSH2049_ROM' not in os.environ and os.path.exists(os.path.join(REPO, 'build', 'rush2049.z64')):
    os.environ['RUSH2049_ROM'] = os.path.join(REPO, 'build', 'rush2049.z64')
import bake, stock  # noqa: E402
import stock_track_glb  # noqa: E402


def read_glb(data):
    """(json, bin) of a glb file."""
    magic, _, total = struct.unpack_from('<III', data, 0)
    if magic != 0x46546C67:
        raise ValueError('not a glb')
    at, js, bin_ = 12, None, b''
    while at < total:
        length, kind = struct.unpack_from('<II', data, at)
        chunk = data[at + 8:at + 8 + length]
        if kind == 0x4E4F534A:
            js = json.loads(chunk)
        elif kind == 0x004E4942:
            bin_ = chunk
        at += 8 + length
    return js, bin_


def merge_glb(g, data, scale):
    """Appends a glb's scene to the Glb writer `g`, scaled by `scale` about the origin (its root nodes are scaled
    in place: a wrapping node without a mesh would be reported by the converter as an unknown marker)."""
    src, src_bin = read_glb(data)
    js = g.js
    while len(g.bin) % 4:
        g.bin.append(0)
    bin_base = len(g.bin)
    g.bin += src_bin
    base = {k: len(js.get(k, [])) for k in ('bufferViews', 'accessors', 'images', 'samplers', 'textures',
                                             'materials', 'meshes', 'nodes', 'animations')}

    def add(kind, items):
        js.setdefault(kind, []).extend(items)

    views = []
    for v in src.get('bufferViews', []):
        v = dict(v, buffer=0, byteOffset=v.get('byteOffset', 0) + bin_base)
        views.append(v)
    add('bufferViews', views)
    accessors = []
    for a in src.get('accessors', []):
        a = dict(a)
        if 'bufferView' in a:
            a['bufferView'] += base['bufferViews']
        if 'sparse' in a:
            a['sparse'] = json.loads(json.dumps(a['sparse']))
            a['sparse']['indices']['bufferView'] += base['bufferViews']
            a['sparse']['values']['bufferView'] += base['bufferViews']
        accessors.append(a)
    add('accessors', accessors)
    add('images', [dict(i, bufferView=i['bufferView'] + base['bufferViews']) if 'bufferView' in i else dict(i)
                   for i in src.get('images', [])])
    add('samplers', [dict(s) for s in src.get('samplers', [])])
    textures = []
    for t in src.get('textures', []):
        t = dict(t)
        if 'source' in t:
            t['source'] += base['images']
        if 'sampler' in t:
            t['sampler'] += base['samplers']
        textures.append(t)
    add('textures', textures)

    def retex(m):
        if isinstance(m, dict):
            out = {}
            for k, v in m.items():
                out[k] = retex(v)
                if k.endswith('Texture') and isinstance(v, dict) and 'index' in v:
                    out[k]['index'] = v['index'] + base['textures']
            return out
        return m
    add('materials', [retex(m) for m in src.get('materials', [])])
    meshes = []
    for m in src.get('meshes', []):
        m = json.loads(json.dumps(m))
        for p in m['primitives']:
            p['attributes'] = {k: v + base['accessors'] for k, v in p['attributes'].items()}
            if 'indices' in p:
                p['indices'] += base['accessors']
            if 'material' in p:
                p['material'] += base['materials']
            for t in p.get('targets', []):
                for k in t:
                    t[k] += base['accessors']
        meshes.append(m)
    add('meshes', meshes)
    nodes = []
    for n in src.get('nodes', []):
        n = dict(n)
        if 'mesh' in n:
            n['mesh'] += base['meshes']
        if 'children' in n:
            n['children'] = [c + base['nodes'] for c in n['children']]
        n.pop('camera', None)
        n.pop('skin', None)
        nodes.append(n)
    add('nodes', nodes)
    anims = []
    for a in src.get('animations', []):
        a = json.loads(json.dumps(a))
        for s in a['samplers']:
            s['input'] += base['accessors']
            s['output'] += base['accessors']
        for c in a['channels']:
            c['target']['node'] += base['nodes']
        anims.append(a)
    if anims:
        add('animations', anims)
    for key in ('extensionsUsed', 'extensionsRequired'):
        for e in src.get(key, []):
            if e not in js.setdefault(key, []):
                js[key].append(e)
    roots = src['scenes'][src.get('scene', 0)]['nodes'] if src.get('scenes') else []
    for r in roots:
        n = js['nodes'][r + base['nodes']]
        if 'matrix' in n:
            n['matrix'] = [v * scale for v in n['matrix'][:15]] + [n['matrix'][15]]
        else:
            n['scale'] = [v * scale for v in n.get('scale', [1, 1, 1])]
            n['translation'] = [v * scale for v in n.get('translation', [0, 0, 0])]
        js['scenes'][0]['nodes'].append(r + base['nodes'])


def toml_text(name, author, description, gravity=None, road_hold=False):
    def q(s):
        return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    lines = ['[track]', 'name = ' + q(name), 'author = ' + q(author), 'description = ' + q(description)]
    if gravity:
        lines.append('gravity = ' + q(gravity))
    if road_hold:
        lines.append('road_hold = true')
    return '\n'.join(lines) + '\n'


def stock_sources(folder):
    """(zip folder name, collision bytes, path bytes, model glb path, track.toml text) of a stock track."""
    if stock.is_stock(folder):
        game, t = stock.parse(folder)
        if game == 'sf':
            import track1
            from r1 import Rush1
            r = Rush1()
            col = track1.convert_collision(r.asset(track1.COLLISION + t))
            path, _ = track1.convert_path(r, t, False)
            name = stock.SF_NAMES[t]
            toml = toml_text(name, 'Atari Games', name + ' from San Francisco Rush, exported by trackview')
            zip_name = 'sfrush_' + name
        else:
            from cs import CalSpeed
            c = CalSpeed()
            col, path = stock.remap_cs(c.asset(94 + t)), c.asset(110 + t)
            name = stock.CS_NAMES[t]
            toml = toml_text(name, 'Atari Games', name + ' from California Speed, exported by trackview',
                             road_hold=True)
            zip_name = 'calspeed_' + name
        return zip_name, col, path, stock.glb_path(folder), toml
    # tmp/stock_tracks/r2_<slot> and r49_race<k>: the exported track's own files.
    base = os.path.basename(os.path.abspath(folder))
    import roms, collision
    if base.startswith('r2_'):
        t = stock_track_glb.RUSH2_TRACKS.index(base[3:].upper())
        r2 = roms.Rush2()
        col, path = r2.asset(0x4B + t), r2.asset(0x57 + t)
        name = base[3:].upper()
        toml = toml_text(name, 'Atari Games', name + ' from San Francisco Rush 2, exported by trackview')
    else:
        k = int(base[8:])
        q = roms.Rush2049()
        col, path = collision.convert(q.file(138 + k)), q.file(157 + k)
        name = stock.R49_NAMES[k - 1]
        toml = toml_text(name, 'Atari Games', name + ' from San Francisco Rush 2049, exported by trackview',
                         gravity='rush2049')
    return base, col, path, stock.dump_glb(folder), toml


def export(folder):
    """(file name, zip bytes) of the track in `folder` (a track folder or a virtual stock: folder)."""
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        if not stock.is_stock(folder) and not stock.dump_glb(folder):
            base = os.path.basename(os.path.abspath(folder))
            for root, _, files in os.walk(folder):
                for f in sorted(files):
                    p = os.path.join(root, f)
                    z.write(p, base + '/' + os.path.relpath(p, folder).replace('\\', '/'))
        else:
            base, col, path, model, toml = stock_sources(folder)
            base = ''.join(ch if ch.isalnum() or ch in '_-' else '_' for ch in base).lower()
            g, _, _, layout = stock_track_glb.build_glb(col, path, visual=False, lanes=True)
            if layout != 'circuit':   # point_to_point (California Speed's courses) or run_in (New York One, ...)
                toml += layout + ' = true\n'
            if model:
                with open(model, 'rb') as f:
                    merge_glb(g, f.read(), stock.GLB_SCALE)
            z.writestr(base + '/track.glb', g.bytes())
            z.writestr(base + '/track.toml', toml)
    return base + '.zip', out.getvalue()


if __name__ == '__main__':
    if len(sys.argv) < 3:
        print(__doc__)
        sys.exit(1)
    name, data = export(sys.argv[1] if stock.is_stock(sys.argv[1]) else os.path.abspath(sys.argv[1]))
    with open(sys.argv[2], 'wb') as f:
        f.write(data)
    print('wrote %s (%s, %d bytes)' % (sys.argv[2], name, len(data)))
