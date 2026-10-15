#!/usr/bin/env python3
"""Checks Rush 2's Controller Pak save (build/saves/rush2.n64.us.mpk): finds the game's note (NR2E), follows its page
chain and prints each player profile with whether its checksummed section is valid. A bad section is what makes the
game call the note damaged and wipe it at the next boot (docs/online.md#the-race).

    python tools/pak_check.py [PAK ...]

Profile records are 0x6C0 bytes from the note's offset 0x40 (func_80097FCC); +0 is the byte sum of +1..+0x33, +2 the
name. Exits 1 if any profile's section is bad.
"""
import sys

PAGE = 0x100
INODE_PAGE = 1
DIR_PAGE = 3
RECORD_START = 0x40
RECORD_SIZE = 0x6C0
RECORDS = 4
SECTION_END = 0x33


def note_data(pak):
    for i in range(16):
        entry = pak[DIR_PAGE * PAGE + i * 32:DIR_PAGE * PAGE + i * 32 + 32]
        if entry[4:8] == b'NR2E' or entry[:4] == b'NR2E':
            break
    else:
        return None
    page = int.from_bytes(entry[6:8], 'big')
    data = b''
    seen = set()
    while 5 <= page < 128 and page not in seen:
        seen.add(page)
        data += pak[page * PAGE:(page + 1) * PAGE]
        nxt = int.from_bytes(pak[INODE_PAGE * PAGE + page * 2:INODE_PAGE * PAGE + page * 2 + 2], 'big')
        if nxt == 1:
            break
        page = nxt & 0xFF
    return data


def main():
    paths = sys.argv[1:] or ['build/saves/rush2.n64.us.mpk']
    bad = False
    for path in paths:
        pak = open(path, 'rb').read()
        data = note_data(pak)
        if data is None:
            print(f'{path}: no Rush 2 note')
            continue
        print(f'{path}:')
        for k in range(RECORDS):
            r = RECORD_START + k * RECORD_SIZE
            if r + SECTION_END >= len(data):
                break
            stored, summed = data[r], sum(data[r + 1:r + SECTION_END + 1]) & 0xFF
            name = data[r + 2:r + 8].split(b'\0')[0].decode('latin-1')
            ok = stored == summed
            bad |= not ok
            print(f'  profile {k} {name!r:10} car byte {data[r + 0x31]:02X} checksum {stored:02X} '
                  f'sum {summed:02X} {"ok" if ok else "BAD"}')
    sys.exit(1 if bad else 0)


main()
