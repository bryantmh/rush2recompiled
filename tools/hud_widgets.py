#!/usr/bin/env python3
"""Lists the race HUD's 2D widgets from an RDRAM dump (tools/live_ram.py PID --dump FILE): each image widget's slot,
image name and record address, position, size, hidden flag and source rectangle, and the record's own size and texel
pointer, plus the model containers' texture tables (slot, records, count, first and last name). Shows which container
a widget's image came from (docs/hud_layout.md#image-name-clashes-with-the-track).

    python tools/hud_widgets.py DUMP [NAME ...]     only widgets whose image is one of NAME (default: all)
"""
import struct
import sys


def main():
    d = open(sys.argv[1], 'rb').read()
    only = {n.encode() for n in sys.argv[2:]}
    w32 = lambda a: struct.unpack('>I', d[a - 0x80000000:a - 0x80000000 + 4])[0]
    s16 = lambda a: struct.unpack('>h', d[a - 0x80000000:a - 0x80000000 + 2])[0]
    name = lambda a: d[a - 0x80000000:a - 0x80000000 + 16].split(b'\0')[0]
    count = d[0x800D5788 - 0x80000000]
    tables = []
    for s in range(count):
        r, n = w32(0x80119220 + s * 8), w32(0x80119224 + s * 8)
        tables.append((r, n))
        if r and n:
            print(f'container {s:2}: {r:08X} {n:4}  {name(r).decode("latin1")} .. {name(r + (n - 1) * 0x20).decode("latin1")}')

    def slot_of(rec):
        for s, (r, n) in enumerate(tables):
            if r and r <= rec < r + n * 0x20:
                return s
        return -1

    for i in range(400):  # widget array moved to 0x802F6C00 (src/players4.cpp), 0x20 each
        w = 0x802F6C00 + i * 0x20
        img = w32(w + 4)
        if d[w - 0x80000000 + 0x15] & 0x80 or not 0x80000000 <= img < 0x81000000:
            continue
        n = name(img)
        if only and n not in only:
            continue
        print(f'widget {i:3} {n.decode("latin1"):12} record {img:08X} (container {slot_of(img)}) at ({s16(w + 0xA)},'
              f'{s16(w + 0xC)}) {s16(w + 0x10)}x{s16(w + 0x12)} hidden {d[w - 0x80000000 + 0x16]} src '
              f'{[s16(w + k) for k in (0x18, 0x1A, 0x1C, 0x1E)]} image {s16(img + 0x10)}x{s16(img + 0x12)} '
              f'texels {w32(img + 0x18):08X}')


if __name__ == '__main__':
    main()
