"""Reader for the PlayStation San Francisco Rush disc (SLUS-00505, research aid for the PS1 content port,
docs/sfrush_ps1.md).

The disc is a .cue with a MODE2/2352 data track (Track 1) and CD audio tracks. Point SFRUSH_PS1_DISC at the .cue or
the data track's .bin. The game program is EXE/RUSH.EXE (a PS-X EXE loaded at 0x80010000).

  python ps1.py ls                       list the disc's files
  python ps1.py extract OUT_DIR          write every file under OUT_DIR
  python ps1.py chunks FILE.DAT          list the chunks of a .DAT/.SIZ pair (sizes)
"""
import os, re, struct, sys

DISC = os.environ.get('SFRUSH_PS1_DISC', os.path.expanduser(r'~\Downloads\ps1\San Francisco Rush - Extreme Racing (USA).cue'))
EXE_ADDR = 0x80010000


def data_track(path):
    """The .bin of the cue's first (MODE2/2352) track, or the path itself when it is a .bin."""
    if not path.lower().endswith('.cue'):
        return path
    text = open(path, 'r', errors='replace').read()
    m = re.search(r'FILE\s+"([^"]+)"', text)
    return os.path.join(os.path.dirname(path), m.group(1))


class Disc:
    def __init__(self, path=DISC):
        self.f = open(data_track(path), 'rb')
        self.files = {}
        pvd = self.sector(16)
        root = pvd[156:156 + 34]
        self._walk(struct.unpack_from('<I', root, 2)[0], struct.unpack_from('<I', root, 10)[0], '')

    def sector(self, lba):
        self.f.seek(lba * 2352 + 24)   # sync 12 + header 4 + subheader 8 (mode 2 form 1)
        return self.f.read(2048)

    def _read(self, lba, size):
        b = bytearray()
        while len(b) < size:
            b += self.sector(lba); lba += 1
        return bytes(b[:size])

    def _walk(self, lba, size, path):
        data = self._read(lba, size); i = 0
        while i < len(data):
            n = data[i]
            if n == 0:
                i = (i // 2048 + 1) * 2048; continue
            rec = data[i:i + n]; i += n
            elba, esz = struct.unpack_from('<I', rec, 2)[0], struct.unpack_from('<I', rec, 10)[0]
            name = rec[33:33 + rec[32]]
            if name in (b'\0', b'\1'):
                continue
            p = (path + '/' + name.decode('ascii', 'replace').split(';')[0]).lstrip('/')
            if rec[25] & 2:
                self._walk(elba, esz, p)
            else:
                self.files[p.upper()] = (elba, esz)

    def read(self, name):
        lba, size = self.files[name.upper().replace('\\', '/')]
        return self._read(lba, size)

    def chunks(self, name):
        """A .DAT file split by its .SIZ (a list of s32 chunk sizes)."""
        base = name[:-4] if name.upper().endswith('.DAT') else name
        d = self.read(base + '.DAT'); s = self.read(base + '.SIZ')
        out = []; o = 0
        for z in struct.unpack('<%di' % (len(s) // 4), s):
            out.append(d[o:o + z]); o += z
        return out

    def exe(self):
        """RUSH.EXE's text+data as loaded (from EXE_ADDR)."""
        d = self.read('EXE/RUSH.EXE')
        addr, size = struct.unpack_from('<II', d, 0x18)
        assert addr == EXE_ADDR
        return d[0x800:0x800 + size]


if __name__ == '__main__':
    d = Disc()
    if sys.argv[1] == 'ls':
        for k, (lba, size) in sorted(d.files.items()):
            print('%10d  %s' % (size, k))
    elif sys.argv[1] == 'extract':
        for k in d.files:
            p = os.path.join(sys.argv[2], k)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            open(p, 'wb').write(d.read(k))
    elif sys.argv[1] == 'chunks':
        for i, c in enumerate(d.chunks(sys.argv[2])):
            print(i, len(c))
