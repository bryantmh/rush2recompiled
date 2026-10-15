# Copies a custom track mod folder to a new mod, optionally renaming objects in its track.glb (test variants: put a
# [surfaces] material on a track's dirt, say). The copy gets its own mod id, folder and track name.
#
# Usage: python tools/track_mod_copy.py SRC_MOD_DIR DST_MOD_DIR [--name "Track name"] [--rename OLD=NEW ...]
#   python tools/track_mod_copy.py build/mods/wario_stadium2 build/mods/wario_stadium_fastdirt \
#       --name "Wario Stadium (fastdirt)" --rename COL_DIRT_main=COL_FASTDIRT_main
import argparse, json, os, shutil, struct, sys


def rename_glb_nodes(path, renames):
    data = open(path, 'rb').read()
    if data[:4] != b'glTF':
        sys.exit(f'{path}: not a .glb')
    json_len = struct.unpack('<I', data[12:16])[0]
    gltf = json.loads(data[20:20 + json_len])
    found = set()
    for node in gltf.get('nodes', []):
        if node.get('name') in renames:
            found.add(node['name'])
            node['name'] = renames[node['name']]
    for old in renames:
        if old not in found:
            print(f'warning: no object named {old} in {path}')
    chunk = json.dumps(gltf, separators=(',', ':')).encode()
    chunk += b' ' * (-len(chunk) % 4)   # JSON chunks pad with spaces to 4 bytes
    body = struct.pack('<II', len(chunk), 0x4E4F534A) + chunk + data[20 + json_len:]
    open(path, 'wb').write(b'glTF' + struct.pack('<II', 2, 12 + len(body)) + body)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--name', help='track name shown on track select')
    ap.add_argument('--rename', action='append', default=[], metavar='OLD=NEW', help='rename a track.glb object')
    args = ap.parse_args()
    if os.path.exists(args.dst):
        sys.exit(f'{args.dst} already exists')
    renames = dict(r.split('=', 1) for r in args.rename)
    mod_id = os.path.basename(os.path.normpath(args.dst))
    shutil.copytree(args.src, args.dst)

    meta_path = os.path.join(args.dst, 'mod.json')
    meta = json.load(open(meta_path, encoding='utf-8'))
    meta['id'] = mod_id
    if args.name:
        meta['display_name'] = args.name
    with open(meta_path, 'w', encoding='utf-8', newline='') as f:
        json.dump(meta, f, indent=4)

    tracks = os.path.join(args.dst, 'tracks')
    for folder in os.listdir(tracks):
        track = os.path.join(tracks, mod_id if len(os.listdir(tracks)) == 1 else folder)
        os.rename(os.path.join(tracks, folder), track)
        glb = os.path.join(track, 'track.glb')
        if renames and os.path.exists(glb):
            rename_glb_nodes(glb, renames)
        toml = os.path.join(track, 'track.toml')
        if args.name and os.path.exists(toml):
            lines = open(toml, encoding='utf-8', newline='').read().splitlines(keepends=True)
            for i, line in enumerate(lines):
                if line.split('=')[0].strip() == 'name':
                    lines[i] = f'name        = "{args.name}"' + line[len(line.rstrip('\r\n')):]
                    break
            open(toml, 'w', encoding='utf-8', newline='').write(''.join(lines))
    print(f'{args.dst}: mod id {mod_id}')


if __name__ == '__main__':
    main()
