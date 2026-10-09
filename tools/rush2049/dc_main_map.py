"""Maps Rush 2049's N64 code-segment data onto the Dreamcast executable (1ST_READ.BIN).

    dc_main_map.py DC_1ST_READ [--emit src/rush2049_dc_map.inc] [--report]

The recomp reads tables out of the N64 game's main segment (and the battle overlay): car setup, PVS, fog, sounds...
A Dreamcast disc has the same tables in 1ST_READ.BIN (loaded at 0x8C010000), little endian, at other addresses. This
tool lines the two up: every 8-byte N64 window that matches DC bytes under a 32-bit swap (words, floats), a 16-bit
swap (halfwords) or as is (bytes, strings) at one place only votes for that place. Runs of words that keep one offset
become map entries, and each word gets the swap of the matches around it. Pointer words (N64 segment address on one
side, DC executable address on the other) are marked for translation.

With --emit it writes the map as C++ (rush2::rom2049 builds the N64 segments of a Dreamcast source from it, see
src/rush2049_dc.cpp). The N64 ROM is RUSH2049_ROM (roms.py). See docs/rush2049_research/dreamcast.md.
"""
import bisect, collections, struct, sys, zlib

sys.path.insert(0, __import__('os').path.dirname(__file__))
from roms import Rush2049

DC_BASE = 0x8C010000
# N64 segments the recomp reads: (name, vram, ROM offset of the raw-deflate data)
SEGMENTS = [('main', 0x80086A50, 0xB0CB10), ('battle', 0x8038A400, 0xB6FEC4)]

SWAP32, SWAP16, BYTES, POINTER = 'w', 'h', 'b', 'p'


def transform(b, mode):
    if mode == SWAP32:
        return b''.join(b[i:i + 4][::-1] for i in range(0, len(b), 4))
    if mode == SWAP16:
        return b''.join(b[i:i + 2][::-1] for i in range(0, len(b), 2))
    return b


def distinctive(b):
    # Windows of zeros, one repeated byte or mostly zero say nothing about where they are.
    return len(set(b)) >= 4 and b.count(0) <= 4


def index_dc(dc):
    idx = collections.defaultdict(list)
    for p in range(0, len(dc) - 8, 2):
        w = dc[p:p + 8]
        if distinctive(w):
            l = idx[w]
            if len(l) < 3:
                l.append(p)
    return idx


def map_segment(seg, base, dc, idx):
    """Per N64 word: (delta, mode) votes, then runs."""
    words = len(seg) // 4
    votes = [None] * words
    for o in range(0, words * 4 - 8, 4):
        win = seg[o:o + 8]
        if not distinctive(win):
            continue
        for mode in (SWAP32, SWAP16, BYTES):
            hits = idx.get(transform(win, mode))
            if hits and len(hits) == 1 and hits[0] % 4 == 0:
                votes[o // 4] = (hits[0] - o, mode)
                votes[o // 4 + 1] = votes[o // 4 + 1] or (hits[0] - o, mode)
                break
    # Fill: a word between two anchors with the same delta takes that delta; its mode is the nearest anchor's.
    anchors = [i for i, v in enumerate(votes) if v]
    out = [None] * words
    for a, b in zip(anchors, anchors[1:]):
        out[a] = votes[a]
        if votes[a][0] == votes[b][0] and b - a <= 64:
            for i in range(a + 1, b):
                mode = votes[a][1] if i - a <= b - i else votes[b][1]
                out[i] = (votes[a][0], mode)
    if anchors:
        out[anchors[-1]] = votes[anchors[-1]]
    # Pointers and words whose value differs: decide the mode from the actual bytes.
    for i, v in enumerate(out):
        if not v:
            continue
        d, mode = v
        n = seg[i * 4:i * 4 + 4]
        p = i * 4 + d
        if p < 0 or p + 4 > len(dc):
            out[i] = None
            continue
        c = dc[p:p + 4]
        nv = struct.unpack('>I', n)[0]
        cv = struct.unpack('<I', c)[0]
        if base <= nv < base + len(seg) and DC_BASE <= cv < DC_BASE + len(dc):
            out[i] = (d, POINTER)
        elif transform(n, SWAP32) == c:
            out[i] = (d, SWAP32) if mode != SWAP16 or transform(n, SWAP16) != c else (d, mode)
        elif transform(n, SWAP16) == c:
            out[i] = (d, SWAP16)
        elif n == c:
            out[i] = (d, BYTES)
    return out


def runs(out):
    """[(start word, count, delta, modes string)] of consecutive mapped words with one delta."""
    res = []
    i = 0
    while i < len(out):
        if not out[i]:
            i += 1
            continue
        j = i
        while j < len(out) and out[j] and out[j][0] == out[i][0]:
            j += 1
        res.append((i, j - i, out[i][0], ''.join(o[1] for o in out[i:j])))
        i = j
    return res


def rle(modes):
    parts = []
    for k, g in __import__('itertools').groupby(modes):
        parts.append('%d%s' % (len(list(g)), k))
    return ''.join(parts)


def main():
    dc = open(sys.argv[1], 'rb').read()
    rom = Rush2049()
    idx = index_dc(dc)
    emit = sys.argv[sys.argv.index('--emit') + 1] if '--emit' in sys.argv else None
    lines = []
    for name, vram, rom_off in SEGMENTS:
        seg = zlib.decompressobj(-15).decompress(rom.rom[rom_off:rom_off + 0x100000])
        out = map_segment(seg, vram, dc, idx)
        rs = [r for r in runs(out) if r[1] >= 2]
        mapped = sum(r[1] for r in rs)
        print('%s: %d of %d words mapped in %d runs' % (name, mapped, len(seg) // 4, len(rs)))
        if '--report' in sys.argv:
            for s, n, d, m in rs:
                print('  %08X..%08X -> %08X %s' % (vram + s * 4, vram + (s + n) * 4, DC_BASE + s * 4 + d, rle(m)))
        lines.append('    // %s segment: N64 0x%08X, %d bytes' % (name, vram, len(seg)))
        lines.append('    { "%s", 0x%08X, %d, {' % (name, vram, len(seg)))
        for s, n, d, m in rs:
            lines.append('        { 0x%08X, %d, 0x%08X, "%s" },' % (vram + s * 4, n, DC_BASE + s * 4 + d, rle(m)))
        lines.append('    } },')
    if emit:
        with open(emit, 'w', newline='\n') as f:
            f.write('// Generated by tools/rush2049/dc_main_map.py from the N64 and Dreamcast Rush 2049 executables. Do not edit.\n')
            f.write('// { N64 segment, vram, size, { { N64 address, words, DC address, word modes (count + w swap32, h swap16,\n')
            f.write('//   b bytes, p pointer) } } }\n')
            f.write('\n'.join(lines) + '\n')


if __name__ == '__main__':
    main()
