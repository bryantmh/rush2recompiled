"""Reader for San Francisco Rush (USA, NSFE) ROM data (research aid for the Rush 1 track port).

The main code is LZ-compressed (Rush 2's variant) at ROM 0x7A7930 and runs at 0x8005BB10; the boot segment is raw at
ROM 0x1000 / 0x80000400. Assets are listed by 72 u32 ROM offsets at 0x800C7C1C, all LZ. Path defaults to the
app-data copy; override with RUSH1_ROM.
"""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
from roms import lz

RUSH1_ROM = os.environ.get('RUSH1_ROM', os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Rush2Recompiled', 'rush1.z64'))
if not os.path.exists(RUSH1_ROM):
    RUSH1_ROM = r'E:\Emulation\roms\n64\San Francisco Rush - Extreme Racing.z64'

MAIN_ROM = 0x7A7930
MAIN_VRAM = 0x8005BB10
BOOT_VRAM = 0x80000400
ASSETS = 0x800C7C1C
ASSET_COUNT = 72


class Rush1:
    def __init__(self, path=RUSH1_ROM):
        self.rom = open(path, 'rb').read()
        self.main = lz(self.rom, MAIN_ROM, False)
        self._cache = {}

    def read(self, vram, n):
        if vram >= MAIN_VRAM:
            o = vram - MAIN_VRAM; return self.main[o:o + n]
        o = 0x1000 + vram - BOOT_VRAM; return self.rom[o:o + n]

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
