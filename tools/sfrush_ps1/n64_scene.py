"""Writes the N64 SF Rush course a model dump placed (build/asset_dump/models/sfrush/tracks/<Name>/_placed.glb, Dump
Models) as the binary tools/sfrush_ps1/cpp_test/main.cpp reads with SFRUSH_PS1_N64_SCENE: what the game's install
(src/sfrush_ps1/ps1_install.cpp n64_embarcadero) hands the PS1 converter from the ROM, for offline runs.

  python tools/sfrush_ps1/n64_scene.py Embarcadero tmp/ps1fix/n64_embarcadero.bin

Format (little-endian): u32 image count, then per image u32 width, height, wrap_s, wrap_t and width*height*4 RGBA;
u32 triangle count, then per triangle 3 x (3 f32 position in the game's world feet, 2 f32 uv, 4 u8 color), i32 image.
"""
import io, os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..'))
import glb_view as G

name, out = sys.argv[1], sys.argv[2]
g, b = G.load(f'build/asset_dump/models/sfrush/tracks/{name}/_placed.glb')
imgs = []
for t in g.get('textures', []):
    im = g['images'][t['source']]; v = g['bufferViews'][im['bufferView']]; o = v.get('byteOffset', 0)
    rgba = Image.open(io.BytesIO(b[o:o + v['byteLength']])).convert('RGBA')
    s = g['samplers'][t['sampler']] if 'sampler' in t else {}
    imgs.append((rgba.width, rgba.height, s.get('wrapS', 10497), s.get('wrapT', 10497), rgba.tobytes()))
tris = bytearray(); count = 0
def walk(ni, M):
    global count, tris
    n = g['nodes'][ni]
    L = np.array(n['matrix']).reshape(4, 4).T if 'matrix' in n else np.eye(4)
    M = M @ L
    if n.get('scale') == [0, 0, 0]: return
    if 'mesh' in n and (n.get('name', '').startswith('TRACK') or n.get('name', '').endswith('FINISH')):
        for p in g['meshes'][n['mesh']]['primitives']:
            P = G.accessor(g, b, p['attributes']['POSITION']); P = (np.c_[P, np.ones(len(P))] @ M.T)[:, :3]
            U = G.accessor(g, b, p['attributes']['TEXCOORD_0']) if 'TEXCOORD_0' in p['attributes'] else np.zeros((len(P), 2))
            C = G.accessor(g, b, p['attributes']['COLOR_0']) if 'COLOR_0' in p['attributes'] else np.ones((len(P), 4))
            if C.shape[1] == 3: C = np.c_[C, np.ones(len(C))]
            I = G.accessor(g, b, p['indices']).astype(int).reshape(-1, 3) if 'indices' in p else np.arange(len(P)).reshape(-1, 3)
            mat = g['materials'][p['material']] if 'material' in p else {}
            tex = mat.get('pbrMetallicRoughness', {}).get('baseColorTexture', {}).get('index', -1)
            for tri in I:
                for k in tri:
                    x, y, z = P[k]
                    tris += struct.pack('<5f4B', -x, y, z, U[k][0], U[k][1], *[int(round(min(1, max(0, c)) * 255)) for c in C[k]])
                tris += struct.pack('<i', tex); count += 1
    for c in n.get('children', []): walk(c, M)
for ni in g['scenes'][0]['nodes']: walk(ni, np.eye(4))
with open(out, 'wb') as f:
    f.write(struct.pack('<I', len(imgs)))
    for w, h, ws, wt, px in imgs: f.write(struct.pack('<4I', w, h, ws, wt)); f.write(px)
    f.write(struct.pack('<I', count)); f.write(tris)
print(out, len(imgs), 'images', count, 'triangles')
