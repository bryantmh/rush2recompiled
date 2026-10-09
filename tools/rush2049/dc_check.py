"""Checks the N64 files the Dreamcast source makes (cpp_test/dc_test.exe output) against the N64 ROM's.

    dc_check.py OUTDIR [indices...]

Models: parse and validate as the game's loader will see them (model.M49.check), list object names missing on either
side. Placement: record counts and how many records sit at the same place with the same name. Collision: header
counts and sizes. Paths: byte equality. RUSH2049_ROM is the N64 ROM (roms.py). See docs/rush2049_research/dreamcast.md.
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(__file__))
from roms import Rush2049
from model import M49


def placement(d):
    o, n = struct.unpack_from('>II', d, 0)
    chunks = {}
    for i in range(n):
        tag = d[o + i * 12:o + i * 12 + 4].decode()
        chunks[tag] = struct.unpack_from('>II', d, o + i * 12 + 4)
    wo, wn = chunks['WOBJ']
    recs = set()
    for i in range(wn):
        r = wo + i * 0x68
        name = d[r:r + 16].split(b'\0')[0].decode('latin1')
        pos = struct.unpack_from('>3f', d, r + 0x34)
        recs.add((name, tuple(round(v) for v in pos)))
    return chunks, recs


def main():
    out = sys.argv[1]
    rom = Rush2049()
    want = [int(a) for a in sys.argv[2:]]
    bad = 0
    for f in sorted(os.listdir(out)):
        i = int(f[:3])
        if want and i not in want:
            continue
        d = open(os.path.join(out, f), 'rb').read()
        n = rom.file(i)
        if 158 <= i <= 182:
            print('%d path: %s' % (i, 'same' if d == n else 'DIFFERENT (%d vs %d bytes)' % (len(d), len(n))))
            bad += d != n
        elif 120 <= i <= 138:
            cd, rd = placement(d)
            cn, rn = placement(n)
            print('%d placement: %d records (N64 %d), %d at the same place and name' % (i, len(rd), len(rn), len(rd & rn)))
        elif 139 <= i <= 157:
            hd, hn = struct.unpack_from('>6HI', d, 0), struct.unpack_from('>6HI', n, 0)
            ok = len(d) == 0x10 + hd[0] * 0x84 + hd[1] * 0x14 + hd[2] * 0x18 + hd[3] * 8 + hd[4] * 0x20 + hd[5] + hd[6]
            print('%d collision: %s (N64 %s) %s' % (i, hd, hn, 'sizes add up' if ok else 'SIZES WRONG'))
            bad += not ok
        else:
            try:
                m = M49(d)
                errors, stats = m.check()
            except Exception as e:
                print('%d model: FAILS TO PARSE: %s' % (i, e))
                bad += 1
                continue
            nm = M49(n)
            names, nnames = set(o['name'] for o in m.objects), set(o['name'] for o in nm.objects)
            miss = sorted(nnames - names)
            print('%d model: %d objects (N64 %d), %d textures, %d errors%s%s' % (
                i, len(m.objects), len(nm.objects), len(m.textures), len(errors),
                (' first: ' + errors[0]) if errors else '', (' missing: ' + ' '.join(miss[:12])) if miss else ''))
            bad += bool(errors)
    print('%d problems' % bad)


if __name__ == '__main__':
    main()
