#!/usr/bin/env python3
"""Counts how much of each track's geometry the game draws with its own texture mipmapping (research aid).

Walks every model display list of the stock Rush 2 tracks (F3DEX2) and the San Francisco Rush tracks (F3DEX) and
sorts the triangles by the texture state they're drawn with:

    mipmapped   texture LOD on (G_SETOTHERMODE_H TEXTLOD = LOD) and G_TEXTURE asks for more than one level
    lod, 1 lvl  texture LOD on but G_TEXTURE has one level, so the RDP has nothing to blend down to
    plain       texture LOD off: no minification filtering at all

Each list is walked from the state Rush 2 leaves between models (LOD off, one level), so state that leaks in from
another model isn't seen. The recomp's Distant Textures option (RT64's sampleGeneratedMipmaps) filters all three
with mipmaps it generates, taking the place of the game's own in the first column (RasterPS.hlsl).

    python tools/texlod_scan.py [rush2] [rush1] [-v]     -v lists the models with LOD-on triangles

ROM paths are tools/rush2049/roms.py's and tools/rush1/r1.py's.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'rush2049'))
sys.path.insert(0, os.path.join(HERE, 'rush1'))

# Opcodes of the two microcodes: (vtx/skip set, tri -> triangle count, dl, enddl, texture, othermode_h, branch_z)
F3DEX = dict(tris={0xBF: 1, 0xB1: 2}, dl=0x06, enddl=0xB8, texture=0xBB, othermode_h=0xBA, branch_z=0xB0,
             rdphalf_1=0xB4)
F3DEX2 = dict(tris={0x05: 1, 0x06: 2, 0x07: 2}, dl=0xDE, enddl=0xDF, texture=0xD7, othermode_h=0xE3, branch_z=0x04,
              rdphalf_1=0xE1)
G_MDSFT_TEXTLOD = 16


def lod_write(gbi, w0, w1):
    """The TEXTLOD bit a G_SETOTHERMODE_H writes, or None if it leaves the bit alone."""
    if gbi is F3DEX:
        sft, length = (w0 >> 8) & 0xFF, w0 & 0xFF
    else:
        length = (w0 & 0xFF) + 1
        sft = 32 - ((w0 >> 8) & 0xFF) - length
    if sft <= G_MDSFT_TEXTLOD < sft + length:
        return bool(w1 >> G_MDSFT_TEXTLOD & 1)
    return None


def walk(d, o, gbi, state, counts, depth=0):
    """Walks the list at file offset o. state: [lod on, levels - 1]. counts: triangles per (lod on, levels > 1)."""
    half = 0
    for _ in range(65536):
        if depth > 16 or o < 0 or o + 8 > len(d):
            return
        w0, w1 = struct.unpack_from('>II', d, o)
        op = w0 >> 24
        if op == gbi['enddl']:
            return
        if op in gbi['tris']:
            key = (state[0], state[1] > 0)
            counts[key] = counts.get(key, 0) + gbi['tris'][op]
        elif op == gbi['texture']:
            state[1] = (w0 >> 11) & 7
        elif op == gbi['othermode_h']:
            lod = lod_write(gbi, w0, w1)
            if lod is not None:
                state[0] = lod
        elif op == gbi['rdphalf_1']:
            half = w1
        elif op == gbi['branch_z']:
            # Both branches are drawn at some distance: the far one is the rest of this list.
            walk(d, half & 0xFFFFFF, gbi, list(state), counts, depth + 1)
        elif op == gbi['dl']:
            walk(d, w1 & 0xFFFFFF, gbi, state, counts, depth + 1)
            if (w0 >> 16) & 0xFF == 1:
                return
        o += 8


def report(label, d, models, gbi, verbose):
    """models: [(name, [list offsets])]."""
    total = {}
    lod_models = []
    for name, lists in models:
        counts = {}
        for o in lists:
            walk(d, o, gbi, [False, 0], counts)
        for k, v in counts.items():
            total[k] = total.get(k, 0) + v
        if counts.get((True, True)) or counts.get((True, False)):
            lod_models.append((name, counts.get((True, True), 0), counts.get((True, False), 0),
                               counts.get((False, True), 0) + counts.get((False, False), 0)))
    plain = total.get((False, True), 0) + total.get((False, False), 0)
    print('%-22s %4d models  mipmapped %6d   lod, 1 lvl %6d   plain %6d' % (
        label, len(models), total.get((True, True), 0), total.get((True, False), 0), plain))
    if verbose:
        for name, mip, one, rest in lod_models:
            print('      %-16s mipmapped %5d   lod, 1 lvl %5d   plain %5d' % (name, mip, one, rest))


def rush2(verbose):
    import roms
    import model
    r = roms.Rush2()
    for t in range(12):
        d = r.asset(0x33 + t)
        m = model.R2Model(d)
        models = [(mm['name'], [l[3] & 0xFFFFFF for l in mm['lods'] if l[3]]) for mm in m.models]
        report('Rush 2 track %d' % t, d, models, F3DEX2, verbose)


def rush1(verbose):
    import r1
    import track1
    r = r1.Rush1()
    for t in range(7):
        c = track1.Container(r.asset(track1.GEOMETRY + t) + r.asset(track1.TEXTURE_BANK))
        models = [(c.name(i), [l[2] for l in c.model(i)[1] if l[2]]) for i in range(c.n_models)]
        report('SF Rush track %d' % t, c.d, models, F3DEX, verbose)


if __name__ == '__main__':
    args = sys.argv[1:]
    verbose = '-v' in args
    games = [a for a in args if a != '-v'] or ['rush2', 'rush1']
    for g in games:
        {'rush2': rush2, 'rush1': rush1}[g](verbose)
