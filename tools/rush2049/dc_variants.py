# Builds test disc images from the game volume of the Rush 2049 USA .cdi (the 2336-byte-sector rip the importer was
# made with), each in a layout other rips use, to check src/rush2049dc/rush2049_dc.cpp's import against them:
#   python3 tools/rush2049/dc_variants.py IMAGE.cdi OUTDIR   (about 4 GB written; delete afterwards)
#   tools/rush2049/cpp_test/out/dc_test.exe OUTDIR/<variant> OUTDIR/<variant>.pak OUTDIR/out 0   ("import failed: N" = ImportResult N; "0: no file" after it is normal)
#   rel.iso        2048-byte sectors, positions relative to the file (control: should import)
#   abs45000.iso   2048-byte sectors, positions + 45000 (a GD track03 pulled out as .iso)
#   abs11702.cdi   2336-byte sectors, positions + 11702 (self-boot second session)
#   gd.gdi         .gdi with track03.bin raw 2352 at LBA 45000, positions + 45000 (control: should import)
#   scrambled.iso  rel.iso with 1ST_READ.BIN scrambled the way CD-R self-boot copies do
#   selfboot.cdi   abs11702.cdi with the scrambled 1ST_READ.BIN (a BootDreams-style self-boot copy)
#   redump.cue     Redump-style .cue: tracks 1-2 single density, then REM HIGH-DENSITY AREA and track03.bin
#   selfboot.cue   a CD .cue (no high-density area) over selfboot.cdi
import mmap, struct, sys, os

SRC, OUT = sys.argv[1], sys.argv[2]
SZ, HD, BASE = 2336, 8, 150  # the game volume, labelled CDROM (see tools/rush2049/dc_cdi.py)
fh = open(SRC, 'rb'); m = mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ)
count = (len(m) - BASE * SZ) // SZ
vol = bytearray()
for i in range(count):
    o = (BASE + i) * SZ + HD
    vol += m[o:o + 2048]
print('volume sectors', count)

def sec(buf, n): return buf[n * 2048:(n + 1) * 2048]

def walk(buf):
    """Yields (byte offset of a directory record, is_dir) for every record reachable from the root."""
    pvd = sec(buf, 16)
    root, rsize = struct.unpack_from('<I', pvd, 158)[0], struct.unpack_from('<I', pvd, 166)[0]
    todo, seen = [(root, rsize)], set()
    while todo:
        lba, size = todo.pop()
        if lba in seen: continue
        seen.add(lba)
        start, i = lba * 2048, 0
        while i < size:
            n = buf[start + i]
            if n == 0:
                i = (i // 2048 + 1) * 2048; continue
            rec = start + i
            nl = buf[rec + 32]
            yield rec, nl == 1 and buf[rec + 33] in (0, 1)
            if buf[rec + 25] & 2 and not (nl == 1 and buf[rec + 33] in (0, 1)):
                todo.append((struct.unpack_from('<I', buf, rec + 2)[0], struct.unpack_from('<I', buf, rec + 10)[0]))
            i += n

def find(buf, name):
    for rec, dot in walk(buf):
        nl = buf[rec + 32]
        if bytes(buf[rec + 33:rec + 33 + nl]).split(b';')[0].upper() == name:
            return struct.unpack_from('<I', buf, rec + 2)[0], struct.unpack_from('<I', buf, rec + 10)[0]

def shifted(off):
    b = bytearray(vol)
    recs = list(walk(b))  # walk before changing anything
    for rec, _ in recs + [(16 * 2048 + 156, False)]:
        l = struct.unpack_from('<I', b, rec + 2)[0] + off
        struct.pack_into('<I', b, rec + 2, l); struct.pack_into('>I', b, rec + 6, l)
    p = 16 * 2048
    for o, fmt in ((140, '<I'), (144, '<I'), (148, '>I'), (152, '>I')):
        v = struct.unpack_from(fmt, b, p + o)[0]
        if v: struct.pack_into(fmt, b, p + o, v + off)
    return b

def write(name, data, size=2048, header=0, raw_header=None):
    with open(os.path.join(OUT, name), 'wb') as f:
        for i in range(len(data) // 2048):
            if size == 2048:
                f.write(data[i * 2048:(i + 1) * 2048]); continue
            hdr = raw_header(i) if raw_header else bytes(header)
            f.write(hdr + data[i * 2048:(i + 1) * 2048] + bytes(size - header - 2048))
    print('wrote', name)

# Comstedt's scramble (the inverse of the boot ROM's descramble of 1ST_READ.BIN on a CD-R).
def scramble(data):
    seed = len(data) & 0xffff
    def rnd():
        nonlocal seed
        seed = (seed * 2109 + 9273) & 0x7fff
        return (seed + 0xc000) & 0xffff
    out, pos, left, chunk = bytearray(), 0, len(data), 2048 * 1024
    while chunk >= 32:
        while left >= chunk:
            n = chunk // 32
            idx = list(range(n))
            for i in range(n - 1, -1, -1):
                x = (rnd() * i) >> 16
                idx[i], idx[x] = idx[x], idx[i]
                out += data[pos + 32 * idx[i]:pos + 32 * idx[i] + 32]
            pos += chunk; left -= chunk
        chunk >>= 1
    out += data[pos:]
    return out

def descramble(data):
    seed = len(data) & 0xffff
    def rnd():
        nonlocal seed
        seed = (seed * 2109 + 9273) & 0x7fff
        return (seed + 0xc000) & 0xffff
    out, src, left, chunk = bytearray(len(data)), 0, len(data), 2048 * 1024
    base = 0
    while chunk >= 32:
        while left >= chunk:
            n = chunk // 32
            idx = list(range(n))
            for i in range(n - 1, -1, -1):
                x = (rnd() * i) >> 16
                idx[i], idx[x] = idx[x], idx[i]
                out[base + 32 * idx[i]:base + 32 * idx[i] + 32] = data[src:src + 32]; src += 32
            base += chunk; left -= chunk
        chunk >>= 1
    out[base:] = data[src:]
    return out

write('rel.iso', vol)
write('abs45000.iso', shifted(45000))
write('abs11702.cdi', shifted(11702), 2336, 8)

def mode1(i):
    lba = 45000 + i + 150
    mm, ss, ff = lba // 4500, lba // 75 % 60, lba % 75
    bcd = lambda v: (v // 10) << 4 | v % 10
    return b'\x00' + b'\xff' * 10 + b'\x00' + bytes((bcd(mm), bcd(ss), bcd(ff), 1))
write('track03.bin', shifted(45000), 2352, 16, mode1)
with open(os.path.join(OUT, 'gd.gdi'), 'w', newline='\r\n') as f:
    f.write('1\n3 45000 4 2352 track03.bin 0\n')
print('wrote gd.gdi')

lba, size = find(vol, b'1ST_READ.BIN')
exe = bytes(vol[lba * 2048:lba * 2048 + size])
s = scramble(exe)
assert descramble(s) == exe and s != exe
b = bytearray(vol); b[lba * 2048:lba * 2048 + size] = s
write('scrambled.iso', b)

# A self-boot CD-R as BootDreams-style copies make it: second-session positions and a scrambled executable.
b = shifted(11702); b[lba * 2048:lba * 2048 + size] = s
write('selfboot.cdi', b, 2336, 8)
del b

# Redump's layout: single-density tracks 1-2, then the GD-ROM part from LBA 45000 (track03.bin from above).
write('track01.bin', bytes(2048 * 300), 2352, 16)
with open(os.path.join(OUT, 'track02.bin'), 'wb') as f:
    f.write(bytes(2352 * 300))
with open(os.path.join(OUT, 'redump.cue'), 'w', newline='\r\n') as f:
    f.write('REM SINGLE-DENSITY AREA\nFILE "track01.bin" BINARY\n  TRACK 01 MODE1/2352\n    INDEX 01 00:00:00\n'
            'FILE "track02.bin" BINARY\n  TRACK 02 AUDIO\n    INDEX 00 00:00:00\n    INDEX 01 00:02:00\n'
            'REM HIGH-DENSITY AREA\nFILE "track03.bin" BINARY\n  TRACK 03 MODE1/2352\n    INDEX 01 00:00:00\n')
# A CD .cue (no high-density area) over the self-boot image.
with open(os.path.join(OUT, 'selfboot.cue'), 'w', newline='\r\n') as f:
    f.write('FILE "selfboot.cdi" BINARY\n  TRACK 01 MODE2/2336\n    INDEX 01 00:00:00\n')
print('wrote selfboot.cdi, redump.cue, selfboot.cue')
