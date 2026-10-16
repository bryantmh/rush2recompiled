"""A small glTF 2.0 binary writer for custom-track exports: textured meshes with vertex colors, plain meshes, Empties,
child nodes and animations (MOVE_ objects).

Writes what Blender's glTF exporter writes for the objects docs/custom_tracks.md asks for, so the result goes through
the game's track converter the same way a modder's file does: PNG images embedded in the binary chunk, one material
per image (base color texture, a sampler with the image's wrap modes, an alpha mode), and every object a node named as
the converter expects (top level unless given a parent).
"""
import io
import json
import struct

FLOAT, UINT = 5126, 5125
WRAP = {'repeat': 10497, 'clamp': 33071, 'mirror': 33648}


class Glb:
    def __init__(self, generator):
        self.js = {'asset': {'version': '2.0', 'generator': generator}, 'scene': 0, 'scenes': [{'nodes': []}],
                   'nodes': [], 'meshes': [], 'accessors': [], 'bufferViews': [], 'buffers': [], 'materials': [],
                   'images': [], 'textures': [], 'samplers': []}
        self.bin = bytearray()

    def _view(self, data, target=None):
        while len(self.bin) % 4:
            self.bin.append(0)
        v = {'buffer': 0, 'byteOffset': len(self.bin), 'byteLength': len(data)}
        if target:
            v['target'] = target
        self.js['bufferViews'].append(v)
        self.bin += data
        return len(self.js['bufferViews']) - 1

    def _accessor(self, values, kind, width, ctype, target, minmax=False):
        fmt = '<%d%s' % (len(values), 'f' if ctype == FLOAT else 'I')
        a = {'bufferView': self._view(struct.pack(fmt, *values), target), 'componentType': ctype,
             'count': len(values) // width, 'type': kind}
        if minmax:
            a['min'] = [min(values[k::width]) for k in range(width)]
            a['max'] = [max(values[k::width]) for k in range(width)]
        self.js['accessors'].append(a)
        return len(self.js['accessors']) - 1

    def material(self, name, png=None, wrap=('repeat', 'repeat'), alpha='OPAQUE', color=(1.0, 1.0, 1.0, 1.0),
                 double_sided=False):
        """A material, textured when png (PNG bytes) is given. Returns its index."""
        pbr = {'baseColorFactor': list(color), 'metallicFactor': 0.0, 'roughnessFactor': 1.0}
        if png is not None:
            self.js['images'].append({'name': name, 'mimeType': 'image/png', 'bufferView': self._view(png)})
            self.js['samplers'].append({'magFilter': 9729, 'minFilter': 9987, 'wrapS': WRAP[wrap[0]],
                                        'wrapT': WRAP[wrap[1]]})
            self.js['textures'].append({'source': len(self.js['images']) - 1, 'sampler': len(self.js['samplers']) - 1})
            pbr['baseColorTexture'] = {'index': len(self.js['textures']) - 1}
        m = {'name': name, 'pbrMetallicRoughness': pbr, 'alphaMode': alpha, 'doubleSided': double_sided}
        if alpha == 'MASK':
            m['alphaCutoff'] = 0.5
        self.js['materials'].append(m)
        return len(self.js['materials']) - 1

    def mesh(self, name, prims, matrix=None, parent=None):
        """prims: list of dicts with 'pos' [(x,y,z)], 'tris' [(a,b,c)] (counter-clockwise front) and optional
        'nrm', 'uv', 'col' (RGBA floats 0-1) and 'material'. parent: a node index to put it under. Returns the
        node index."""
        out = []
        for p in prims:
            at = {'POSITION': self._accessor([c for v in p['pos'] for c in v], 'VEC3', 3, FLOAT, 34962, True)}
            if p.get('nrm'):
                at['NORMAL'] = self._accessor([c for v in p['nrm'] for c in v], 'VEC3', 3, FLOAT, 34962)
            if p.get('uv'):
                at['TEXCOORD_0'] = self._accessor([c for v in p['uv'] for c in v], 'VEC2', 2, FLOAT, 34962)
            if p.get('col'):
                at['COLOR_0'] = self._accessor([c for v in p['col'] for c in v], 'VEC4', 4, FLOAT, 34962)
            prim = {'attributes': at, 'indices': self._accessor([i for t in p['tris'] for i in t], 'SCALAR', 1, UINT,
                                                               34963)}
            if p.get('material') is not None:
                prim['material'] = p['material']
            out.append(prim)
        self.js['meshes'].append({'name': name, 'primitives': out})
        node = {'name': name, 'mesh': len(self.js['meshes']) - 1}
        if matrix:
            node['matrix'] = matrix
        return self._node(node, parent)

    def empty(self, name, matrix=None, translation=None, parent=None):
        """An Empty placed by a matrix, or by a translation (an animated node can't have a matrix)."""
        node = {'name': name}
        if matrix:
            node['matrix'] = matrix
        if translation:
            node['translation'] = list(translation)
        return self._node(node, parent)

    def animation(self, name, node, times, rotations=None, translations=None):
        """A LINEAR animation of a node: rotations as (x, y, z, w) quaternions and/or translations at the times (s)."""
        t = self._accessor(list(times), 'SCALAR', 1, FLOAT, None, True)
        channels, samplers = [], []
        for path, values, kind, width in (('rotation', rotations, 'VEC4', 4), ('translation', translations, 'VEC3', 3)):
            if values:
                samplers.append({'input': t, 'output': self._accessor([c for v in values for c in v], kind, width,
                                                                      FLOAT, None), 'interpolation': 'LINEAR'})
                channels.append({'sampler': len(samplers) - 1, 'target': {'node': node, 'path': path}})
        self.js.setdefault('animations', []).append({'name': name, 'channels': channels, 'samplers': samplers})

    def _node(self, n, parent=None):
        self.js['nodes'].append(n)
        i = len(self.js['nodes']) - 1
        if parent is None:
            self.js['scenes'][0]['nodes'].append(i)
        else:
            self.js['nodes'][parent].setdefault('children', []).append(i)
        return i

    def bytes(self):
        for k in ('images', 'textures', 'samplers', 'materials'):
            if not self.js[k]:
                del self.js[k]
        while len(self.bin) % 4:
            self.bin.append(0)
        self.js['buffers'] = [{'byteLength': len(self.bin)}]
        j = json.dumps(self.js, separators=(',', ':')).encode()
        j += b' ' * (-len(j) % 4)
        return (struct.pack('<III', 0x46546C67, 2, 28 + len(j) + len(self.bin)) + struct.pack('<II', len(j), 0x4E4F534A)
                + j + struct.pack('<II', len(self.bin), 0x004E4942) + bytes(self.bin))


def png_bytes(image):
    """PNG bytes of a PIL image."""
    b = io.BytesIO()
    image.save(b, 'PNG')
    return b.getvalue()
