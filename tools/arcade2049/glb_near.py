# Lists the triangles of a converted track.glb near a point, per mesh (scenery and COL_*), in the arcade's world feet
# (the converter's to_gltf undone: x = -x_m, all / 0.3048). For checking collision against what is drawn.
# python -I tools/arcade2049/glb_near.py <track.glb> x y z [radius_ft] [--mesh SUBSTR]
import json, math, struct, sys
FT = 3.280839895
path = sys.argv[1]; P = [float(v) for v in sys.argv[2:5]]
R = float(sys.argv[5]) if len(sys.argv) > 5 and not sys.argv[5].startswith('--') else 15.0
want = sys.argv[sys.argv.index('--mesh') + 1] if '--mesh' in sys.argv else None
b = open(path, 'rb').read()
jl = struct.unpack_from('<I', b, 12)[0]; j = json.loads(b[20:20 + jl]); bin0 = 20 + jl + 8
def acc(i):
    a = j['accessors'][i]; v = j['bufferViews'][a['bufferView']]
    o = bin0 + v.get('byteOffset', 0) + a.get('byteOffset', 0)
    n = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4}[a['type']]
    f = {5126: 'f', 5125: 'I', 5123: 'H', 5121: 'B'}[a['componentType']]
    st = v.get('byteStride', n * struct.calcsize(f))
    return [struct.unpack_from('<%d%s' % (n, f), b, o + st * k) for k in range(a['count'])]
def walk(ni, t):
    n = j['nodes'][ni]; t = [t[k] + n.get('translation', [0, 0, 0])[k] for k in range(3)]
    if 'mesh' in n: yield n.get('name', ''), n['mesh'], t
    for c in n.get('children', []): yield from walk(c, t)
for root in j['scenes'][0]['nodes']:
    for name, mi, t in walk(root, [0, 0, 0]):
        mesh = j['meshes'][mi]; label = name or mesh.get('name', '')
        if want and want not in label: continue
        for pr in mesh['primitives']:
            pos = acc(pr['attributes']['POSITION']); idx = acc(pr['indices']) if 'indices' in pr else [(k,) for k in range(len(pos))]
            idx = [i[0] for i in idx]
            w = [(-(p[0] + t[0]) * FT, (p[1] + t[1]) * FT, (p[2] + t[2]) * FT) for p in pos]
            hits = []
            for k in range(0, len(idx) - 2, 3):
                tri = [w[idx[k]], w[idx[k + 1]], w[idx[k + 2]]]
                c = [sum(q[a] for q in tri) / 3 for a in range(3)]
                if math.dist(c, P) < R: hits.append(tri)
            if hits:
                ys = [q[1] for tri in hits for q in tri]
                print('%-40s %4d tris  y %.1f..%.1f' % (label[:40], len(hits), min(ys), max(ys)))
