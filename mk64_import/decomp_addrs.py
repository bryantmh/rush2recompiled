#!/usr/bin/env python3
"""Finds where the mk64 decomp's named course data (spawn tables, actor models and their display lists) sits in a
course's segment 6, so courses.toml can give mk64_track.py the address (the decomp names them; the ROM holds them in
build order with no table pointing at them).

    python mk64_import/decomp_addrs.py <mk64 decomp> <course> [name filter ...] [--rom "Mario Kart 64.n64"]

Prints `name = 0x06xxxxxx` for every array of courses/<course>/course_data.c matching a filter (substring; default
spawn, model and dl_ names that aren't offset-named): ActorSpawnData and Vtx arrays by their bytes, Gfx lists by
their command opcodes (gbi macro expansions below) with their gsSPVertex / gsSPDisplayList targets checked against the
arrays already found. A name found more than once, or not at all, is printed as a comment.
"""
import argparse
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mk64  # noqa: E402

# F3DEX gbi macro -> the opcodes it expands to (include/PR/gbi.h).
MACROS = {
    'gsSPDisplayList': [0x06], 'gsSPEndDisplayList': [0xB8], 'gsSPVertex': [0x04], 'gsSP1Triangle': [0xBF],
    'gsSP2Triangles': [0xB1], 'gsSPTexture': [0xBB], 'gsSPClearGeometryMode': [0xB6], 'gsSPSetGeometryMode': [0xB7],
    'gsSPNumLights': [0xBC], 'gsSPLight': [0x03], 'gsSPSetLights1': [0xBC, 0x03, 0x03], 'gsSPFogFactor': [0xBC],
    'gsDPSetTile': [0xF5], 'gsDPTileSync': [0xE8], 'gsDPSetTextureImage': [0xFD], 'gsDPLoadSync': [0xE6],
    'gsDPLoadBlock': [0xF3], 'gsDPSetTileSize': [0xF2], 'gsDPPipeSync': [0xE7], 'gsDPSetCombineMode': [0xFC],
    'gsDPSetRenderMode': [0xB9], 'gsDPSetAlphaCompare': [0xB9], 'gsDPSetTextureLUT': [0xBA],
    'gsDPSetCycleType': [0xBA], 'gsDPSetTexturePersp': [0xBA], 'gsDPSetTextureFilter': [0xBA], 'gsDPNoOp': [0x00],
    'gsDPSetFogColor': [0xF8], 'gsDPLoadTLUT_pal256': [0xFD, 0xE8, 0xF5, 0xE6, 0xF0, 0xE7],
    'gsDPLoadTextureBlock': [0xFD, 0xF5, 0xE6, 0xF3, 0xE7, 0xF5, 0xF2],
}
ARRAY = re.compile(r'^(struct \w+|\w+)\s+(\w+)\[[^\]]*\]\s*=\s*\{(.*?)^\};', re.S | re.M)
OFFSET_NAMED = re.compile(r'_(?:packed_)?dl_[0-9A-F]+$|_[0-9A-F]{8}$')


def macros(body):
    """(macro, first argument) for each gbi macro in a Gfx initializer."""
    out = []
    for m in re.finditer(r'(gs\w+)\s*\(', body):
        depth, i = 1, m.end()
        while depth:
            depth += {'(': 1, ')': -1}.get(body[i], 0)
            i += 1
        out.append((m.group(1), body[m.end():i - 1].split(',')[0].strip()))
    return out


def ints(body):
    return [int(x, 16) if 'x' in x else int(x) for x in
            re.findall(r'(?<![\w])-?(?:0x[0-9A-Fa-f]+|\d+)(?![\w])', re.sub(r'//.*', '', body))]


def find_all(blob, pattern, step):
    out, i = [], blob.find(pattern)
    while i >= 0:
        if i % step == 0:
            out.append(i)
        i = blob.find(pattern, i + 1)
    return out


def locate(seg6, arrays):
    """name -> [offsets] for the arrays; Gfx last so they can check their pointers against the rest."""
    found = {}
    for kind, name, body in arrays:
        v = ints(body) if kind in ('struct ActorSpawnData', 'Vtx') else []
        if kind == 'struct ActorSpawnData' and len(v) % 4 == 0:
            data = b''.join(struct.pack('>3hH', a, b, c, d & 0xFFFF) for a, b, c, d in zip(*[iter(v)] * 4))
            found[name] = find_all(seg6, data, 2)
        elif kind == 'Vtx' and len(v) % 10 == 0:
            data = b''.join(struct.pack('>3hH2h4B', *(x & 0xFFFF if k == 3 else x for k, x in enumerate(e)))
                            for e in zip(*[iter(v)] * 10))
            found[name] = find_all(seg6, data, 8)
    in_order(arrays, found)
    for kind, name, body in arrays:
        if kind != 'Gfx':
            continue
        cmds = []
        for mac, arg in macros(body):
            ops = MACROS.get(mac)
            if ops is None:
                cmds = None
                break
            target = None
            if mac in ('gsSPVertex', 'gsSPDisplayList'):
                if len(found.get(arg, [])) == 1:
                    target = 0x06000000 + found[arg][0]
                elif re.search(r'_packed_dl_([0-9A-F]+)$', arg):
                    target = 0x07000000 + int(re.search(r'_([0-9A-F]+)$', arg).group(1), 16)
                elif re.search(r'_dl_([0-9A-F]{3,})$', arg):
                    target = 0x06000000 + int(re.search(r'_([0-9A-F]+)$', arg).group(1), 16)
                elif re.fullmatch(r'0x0[0-9A-Fa-f]{7}', arg):
                    target = int(arg, 16)
            cmds += [(op, target if k == 0 else None) for k, op in enumerate(ops)]
        if not cmds:
            continue
        hits = []
        for off in range(0, len(seg6) - 8 * len(cmds) + 1, 8):
            if seg6[off] != cmds[0][0]:
                continue
            for k, (op, target) in enumerate(cmds):
                w0, w1 = struct.unpack_from('>II', seg6, off + 8 * k)
                if w0 >> 24 != op or (target is not None and w1 != target):
                    break
            else:
                hits.append(off)
        found[name] = hits
    in_order(arrays, found)
    return found


def in_order(arrays, found):
    """Arrays with identical bytes match each other's places: the decomp lists them in ROM order, so each takes its
    first match after the array before it."""
    last = -1
    for _, name, _ in arrays:
        hits = found.get(name)
        if not hits:
            continue
        later = [h for h in hits if h > last]
        if later:
            found[name] = later[:1]
            last = later[0]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('decomp')
    ap.add_argument('course', choices=mk64.COURSE_NAMES)
    ap.add_argument('filters', nargs='*')
    ap.add_argument('--rom', default=os.environ.get('MK64_ROM', r'E:\Emulation\roms\n64\Mario Kart 64.n64'))
    a = ap.parse_args()
    src = open(os.path.join(a.decomp, 'courses', a.course, 'course_data.c')).read()
    arrays = [(m.group(1), m.group(2), m.group(3)) for m in ARRAY.finditer(src)]
    seg6 = mk64.Course(mk64.load_rom(a.rom), mk64.COURSE_NAMES.index(a.course)).seg6
    found = locate(seg6, arrays)
    for kind, name, _ in arrays:
        if a.filters:
            if not any(f in name for f in a.filters):
                continue
        elif OFFSET_NAMED.search(name) or not re.search(r'spawn|model|_dl_', name):
            continue
        hits = found.get(name)
        if hits is None:
            print('# %s (%s): not searchable' % (name, kind))
        elif len(hits) == 1:
            print('%s = 0x%08X   # %s' % (name, 0x06000000 + hits[0], kind))
        else:
            print('# %s (%s): %d matches %s' % (name, kind, len(hits), ' '.join('0x%X' % (0x06000000 + h)
                                                                              for h in hits[:6])))


if __name__ == '__main__':
    main()
