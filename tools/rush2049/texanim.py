"""Rush 2049's animated track textures, as patch sites in a converted Rush 2 track geometry file.

    python texanim.py [K ...]      prints the animations of converted tracks K (default 1-6) from out/trackK

Spec: docs/rush2049_research/texanim.md. Two per-track tables in 2049's main data, indexed by 2049 track id (race
track K = id K - 1), drive them, both with 0x14-byte records:
- 0x8011A31C flip-books (func_800BDAA8 set-up, func_800BD2C8 per frame): s16 frame count (0 ends the list), s16 start
  (= the target frame), s16 forward, s16 current, f32 timer, f32 period, frame table (12 bytes per frame: name, record,
  texels). Each step the target texture's load list gets the current frame's texels in its first G_SETTIMG.
- 0x8011A840 scrolls and palette cycles: name (0 ends), s16 position, s16 wrap, s8 speed, u8 kind, s16 rate, f32
  timer, data. Kinds 9 and 10 set the tile origin s or t of every G_SETTILESIZE in the target's load list.

describe() returns the same text as the C++ converter's ConvertedTrack::tex_anims (cpp_test compares them).
"""
import os, struct, sys

import roms
from model import u32

FLIP_LISTS = 0x8011A31C
SCROLL_LISTS = 0x8011A840
TEXTURE_DIRECT = 0x08000000   # texture record flag: data points at texels, not a load list


def s16(d, o):
    return struct.unpack_from('>h', d, o)[0]


def main_string(q, a):
    o = a - q.MAIN_VRAM
    return q.main[o:q.main.index(b'\0', o)].decode('latin1')


def find_texture(g, name):
    """(data, flags) of the converted geometry's texture record named name (15 characters compared), or None."""
    table, count = u32(g, 8), u32(g, 20)
    for i in range(count):
        r = table + i * 0x20
        if g[r:r + 16].split(b'\0')[0].decode('latin1')[:15] == name[:15]:
            return u32(g, r + 0x18), u32(g, r + 0x1C)
    return None


def load_list_commands(g, lst, op):
    """Offsets of the commands with opcode op in the texture-load list at lst, up to its G_ENDDL."""
    start, end = u32(g, 28), u32(g, 32)
    if lst < start or lst >= end or lst & 7:
        return []
    out = []
    for o in range(lst, end, 8):
        if g[o] == op:
            out.append(o)
        if g[o] == 0xDF:
            return out
    return []


def tex_anims(q, k, g):
    """(flipbooks, scrolls) of 2049 race track k in converted geometry g."""
    flips, scrolls = [], []
    m, base = q.main, q.MAIN_VRAM
    lst = u32(m, FLIP_LISTS - base + (k - 1) * 4)
    o = lst - base
    while lst and s16(m, o):
        count, start, fwd = s16(m, o), s16(m, o + 2), s16(m, o + 4) != 0
        period = u32(m, o + 0xC)
        frames_tab = u32(m, o + 0x10)
        ok = count > 0 and 0 <= start < count
        f = dict(target=None, settimg=0, frames=[], start=start, forward=fwd, period=period)
        for i in range(count):
            if not ok:
                break
            name = main_string(q, u32(m, frames_tab - base + i * 12))
            t = find_texture(g, name)
            if t is None:
                ok = False
                break
            data, flags = t
            if i == start:
                st = load_list_commands(g, data, 0xFD)
                ok = not (flags & TEXTURE_DIRECT) and bool(st)
                f['target'] = name
                f['settimg'] = st[0] if ok else 0
            if flags & TEXTURE_DIRECT:
                f['frames'].append(data)
            else:
                st = load_list_commands(g, data, 0xFD)
                ok = ok and bool(st)
                f['frames'].append(u32(g, st[0] + 4) if ok else 0)
        if ok:
            flips.append(f)
        o += 0x14
    lst = u32(m, SCROLL_LISTS - base + (k - 1) * 4)
    o = lst - base
    while lst and u32(m, o):
        kind = m[o + 9]
        name = main_string(q, u32(m, o))
        t = find_texture(g, name)
        if kind in (9, 10) and t is not None and not (t[1] & TEXTURE_DIRECT):
            tiles = [(c, u32(g, c + 4)) for c in load_list_commands(g, t[0], 0xF2)]
            if tiles:
                scrolls.append(dict(target=name, tiles=tiles, t=kind == 10, position=s16(m, o + 4),
                                    wrap=s16(m, o + 6), speed=struct.unpack_from('>b', m, o + 8)[0],
                                    rate=s16(m, o + 0xA)))
        o += 0x14
    return flips, scrolls


def describe(q, k, g):
    flips, scrolls = tex_anims(q, k, g)
    lines = []
    for f in flips:
        lines.append('flip %s settimg %x start %d %s period %08x frames %s' % (
            f['target'], f['settimg'], f['start'], 'forward' if f['forward'] else 'backward', f['period'],
            ','.join('%x' % v for v in f['frames'])))
    for s in scrolls:
        lines.append('scroll %s %s position %d wrap %d speed %d rate %d tiles %s' % (
            s['target'], 't' if s['t'] else 's', s['position'], s['wrap'], s['speed'], s['rate'],
            ','.join('%x:%08x' % c for c in s['tiles'])))
    return ''.join(l + '\n' for l in lines)


if __name__ == '__main__':
    q = roms.Rush2049()
    for k in [int(a) for a in sys.argv[1:]] or range(1, 7):
        g = open(os.path.join('out', 'track%d' % k, 'geometry.bin'), 'rb').read()
        print('track %d' % k)
        sys.stdout.write(describe(q, k, g))
