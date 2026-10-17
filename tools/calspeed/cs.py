"""Reader for California Speed (USA) N64 ROM data (research aid for the California Speed track port, docs/calspeed_research.md).

California Speed N64 is built on the Rush 2 N64 engine. The boot segment is raw at ROM 0x1000 / 0x80000400 and the
main code is raw (not compressed) at ROM 0x1CD60 / 0x80053710. Assets are listed by 128 u32 ROM offsets at 0x8008DFBC
(ROM 0x5760C), all compressed with Rush 2's LZ variant, and are Rush 2 formats.
Path defaults to the app-data copy (calspeed.z64); override with CALSPEED_ROM. A byte-swapped (.n64/.v64) or
little-endian dump is converted on load.

Usage: python cs.py            lists the assets (size, kind, first model names)
"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
from roms import lz

CALSPEED_ROM = os.environ.get('CALSPEED_ROM', os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Rush2Recompiled', 'calspeed.z64'))

BOOT_VRAM = 0x80000400
MAIN_ROM = 0x1CD60
MAIN_VRAM = 0x80053710
MAIN_END = 0x800D20B0       # past the last function (data follows in the same raw segment)
SEG_END_ROM = 0xB1C70       # first asset
ASSETS = 0x8008DFBC
ASSET_COUNT = 128

TRACK_COUNT = 16
# Asset index of track t's files (t 0-15): geometry, placement, collision, AI/checkpoint path.
GEOMETRY, PLACEMENT, COLLISION, PATH = 62, 78, 94, 110
PREFIXES = ['MONTEREY', 'SILICON', 'HWY1', 'SF', 'CENTRAL', 'SC', 'SD', 'CLOVER', 'LA', 'SHASTA', 'YOSEMITE',
            'MOJAVE', 'FUJI', 'OVAL', 'CALI', 'SA']


def to_big_endian(d):
    d = bytearray(d)
    if d[:4] == b'\x37\x80\x40\x12':      # byte-swapped
        d[0::2], d[1::2] = d[1::2], d[0::2]
    elif d[:4] == b'\x40\x12\x37\x80':    # little-endian
        for i in range(0, len(d), 4):
            d[i:i + 4] = d[i:i + 4][::-1]
    return bytes(d)


class CalSpeed:
    def __init__(self, path=CALSPEED_ROM):
        if not os.path.exists(path):
            alt = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tmp', 'calspeed', 'calspeed.z64')
            path = alt if os.path.exists(alt) else path
        self.rom = to_big_endian(open(path, 'rb').read())
        self._cache = {}

    def off(self, vram):
        if vram >= MAIN_VRAM:
            return MAIN_ROM + vram - MAIN_VRAM
        return 0x1000 + vram - BOOT_VRAM

    def read(self, vram, n):
        o = self.off(vram); return self.rom[o:o + n]

    def w(self, a): return struct.unpack('>I', self.read(a, 4))[0]
    def h(self, a): return struct.unpack('>h', self.read(a, 2))[0]
    def f(self, a): return struct.unpack('>f', self.read(a, 4))[0]

    def s(self, a):
        d = self.read(a, 64); return d[:d.index(b'\0')].decode('latin1') if b'\0' in d else d.decode('latin1')

    def asset_rom(self, i): return self.w(ASSETS + i * 4)

    def asset(self, i):
        if i not in self._cache:
            self._cache[i] = lz(self.rom, self.asset_rom(i), False)
        return self._cache[i]


if __name__ == '__main__':
    import model
    c = CalSpeed()
    for i in range(ASSET_COUNT):
        a = c.asset(i)
        try:
            m = model.R2Model(a)
            print('%3d %#8x %7d models %3d: %s' % (i, c.asset_rom(i), len(a), len(m.models),
                                                 ' '.join(x['name'] for x in m.models[:6])))
        except ValueError:
            print('%3d %#8x %7d %s' % (i, c.asset_rom(i), len(a), a[:16].hex()))
