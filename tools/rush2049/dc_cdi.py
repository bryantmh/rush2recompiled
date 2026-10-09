"""Lists and extracts files from the Dreamcast Rush 2049 DiscJuggler image (.cdi).

    dc_cdi.py ls IMAGE [PATTERN]          list files (optionally only paths containing PATTERN)
    dc_cdi.py get IMAGE NAME OUT          extract one file (NAME as listed, e.g. TRACK1.LZS)
    dc_cdi.py get IMAGE NAME OUT --lz     ... and LZ-decompress it (.LZS files)
    dc_cdi.py all IMAGE OUTDIR            extract every game file, .LZS ones decompressed (skips PWBROWSER, .STR, .SFD)
    dc_cdi.py pack PACK OUTDIR [GLOB]     extract files from the app's rush2049_dc.pak (src/rush2049_dc.cpp), .LZS ones
                                          decompressed (songs stay the pack's ADPCM)

The image stores raw 2336-byte mode 2 sectors (8-byte subheader, 2048 bytes of data). It holds several ISO9660
volumes; the game's is the one labelled SFR20491 whose files include the .LZS level data (the other with the same
label is a low-density copy that has no game data). See docs/rush2049_research/dreamcast.md.
"""
import mmap, os, struct, sys

sys.path.insert(0, os.path.dirname(__file__))
from roms import lz

SECTOR = 2336


def open_image(path):
    fh = open(path, 'rb')
    return mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)


def volumes(m):
    """Offsets of every primary volume descriptor in the image."""
    pos = 0
    while True:
        p = m.find(b'\x01CD001', pos)
        if p < 0:
            return
        if (p - 8) % SECTOR == 0:
            yield p
        pos = p + 1


def read_sectors(m, base, lba, count):
    return b''.join(m[base + (lba + i) * SECTOR + 8:base + (lba + i) * SECTOR + 8 + 2048] for i in range(count))


def walk(m, base, lba, size, path=''):
    data = read_sectors(m, base, lba, (size + 2047) // 2048)
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // 2048 + 1) * 2048
            continue
        rec = data[i:i + n]
        i += n
        name = rec[33:33 + rec[32]]
        if name in (b'\0', b'\1'):
            continue
        l, s = struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]
        full = path + '/' + name.decode('latin1').split(';')[0]
        if rec[25] & 2:
            yield from walk(m, base, l, s, full)
        else:
            yield full, l, s


def game_volume(m):
    """(base offset, files) of the volume that holds the most files."""
    best = None
    for pvd in volumes(m):
        base = pvd - 8 - 16 * SECTOR
        root = read_sectors(m, base, 16, 1)[156:190]
        try:
            files = list(walk(m, base, struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0]))
        except Exception:
            continue
        # Files past the end of the image can't be read, so only count ones inside it.
        ok = [f for f in files if base + (f[1] + (f[2] + 2047) // 2048) * SECTOR <= len(m)]
        if best is None or len(ok) > len(best[1]):
            best = (base, ok)
    return best


def pack_entries(path):
    """{name: (offset, size, kind)} of a rush2049_dc.pak ("R49DCPAK", u32 version, count, path length, path, entries)."""
    with open(path, 'rb') as f:
        if f.read(8) != b'R49DCPAK':
            sys.exit('not a Rush 2049 Dreamcast pack: ' + path)
        _, count, path_len = struct.unpack('<III', f.read(12))
        f.read(path_len)
        out = {}
        for _ in range(count):
            name = f.read(f.read(1)[0]).decode()
            out[name] = struct.unpack('<QQI', f.read(20))
        return out


def extract_pack(path, outdir, pattern):
    import fnmatch
    os.makedirs(outdir, exist_ok=True)
    with open(path, 'rb') as f:
        for name, (off, size, kind) in sorted(pack_entries(path).items()):
            if pattern and not fnmatch.fnmatch(name.upper(), pattern.upper()):
                continue
            f.seek(off)
            data = f.read(size)
            if name.upper().endswith('.LZS'):
                data = lz(data, 0, True)
            open(os.path.join(outdir, name), 'wb').write(data)
            print('%s: %d bytes' % (name, len(data)))


if __name__ == '__main__':
    if sys.argv[1] == 'pack':
        extract_pack(sys.argv[2], sys.argv[3], sys.argv[4] if len(sys.argv) > 4 else None)
        sys.exit()
    m = open_image(sys.argv[2])
    base, files = game_volume(m)
    if sys.argv[1] == 'ls':
        for p, l, s in files:
            if len(sys.argv) < 4 or sys.argv[3].lower() in p.lower():
                print('%-40s lba=%d size=%d' % (p, l, s))
    elif sys.argv[1] == 'all':
        os.makedirs(sys.argv[3], exist_ok=True)
        for p, l, s in files:
            if p.startswith('/PWBROWSER') or p.endswith(('.STR', '.SFD')):
                continue
            data = read_sectors(m, base, l, (s + 2047) // 2048)[:s]
            if p.endswith('.LZS'):
                data = lz(data, 0, True)
            open(os.path.join(sys.argv[3], p.lstrip('/')), 'wb').write(data)
    else:
        want = '/' + sys.argv[3].lstrip('/')
        for p, l, s in files:
            if p.upper() == want.upper():
                data = read_sectors(m, base, l, (s + 2047) // 2048)[:s]
                if '--lz' in sys.argv:
                    data = lz(data, 0, True)
                open(sys.argv[4], 'wb').write(data)
                print('%s: %d bytes' % (p, len(data)))
                break
        else:
            sys.exit('not found: ' + sys.argv[3])
