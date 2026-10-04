"""Readers for the Rush 2 and Rush 2049 asset tables.

Rush 2 assets come from the recomp ROM (main code uncompressed at 0x01000000) plus the original ROM for data.
Rush 2049 files come from the user's 2049 ROM. Paths default to the repo copy of Rush 2 and the app-data copy of
2049; override with the RUSH2_ROM / RUSH2049_ROM environment variables.
"""
import os, struct, zlib

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
RUSH2_ROM = os.environ.get('RUSH2_ROM', os.path.join(REPO, 'rush2.us.recomp.z64'))
RUSH2049_ROM = os.environ.get('RUSH2049_ROM',
    os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Rush2Recompiled', 'rush2049.z64'))


def lz(data, offset, relative):
    """4 KB-window LZSS. relative=False: Rush 2 (absolute ring positions). relative=True: Rush 2049."""
    out = bytearray(); i = offset; pos = 1
    while True:
        flags = data[i]; i += 1
        for bit in range(8):
            if flags & (1 << bit):
                out.append(data[i]); i += 1; pos = (pos + 1) & 0xFFF
                continue
            b1, b2 = data[i], data[i + 1]; i += 2
            off = (((b1 & 0xF0) << 4) | b2) & 0xFFF
            length = (b1 & 0xF) + 2
            if off == 0 and length == 2:
                return bytes(out)
            if relative:
                dist = off
            else:
                dist = pos - off
                if dist <= 0:
                    dist += 0x1000
            for _ in range(length):
                out.append(out[-dist] if dist <= len(out) else 0)
            pos = (pos + length) & 0xFFF


def inflate(data, offset):
    z = zlib.decompressobj(-15)
    return z.decompress(data[offset:offset + 0x400000])


class Rush2:
    MAIN_VRAM = 0x800539E0
    OVL_VRAM = 0x803AA800
    LZ_INDICES = {0x12, 0x13, 0x15, 0x16, 0x17, 0x18} | set(range(0x1D, 0x33))
    CARS = ['PICKUP', 'INTEG', 'VETTE', 'SLED', 'BMW', 'CAMARO', 'SUPRA', 'BUGAT', 'VWBUS', 'VIPER', 'VWBUG',
            'CONCPT', 'CIVIC', 'CADDY', 'MUST', 'SUV', 'TAXI', 'HOTROD', 'FORM1', 'GT90', 'ROCKET', 'DEW']

    def __init__(self, path=RUSH2_ROM):
        self.rom = open(path, 'rb').read()

    def off(self, vram):
        if 0x80000400 <= vram < self.MAIN_VRAM:
            return 0x1000 + vram - 0x80000400
        if self.MAIN_VRAM <= vram < self.OVL_VRAM:
            return 0x01000000 + vram - self.MAIN_VRAM
        return 0x01080000 + vram - self.OVL_VRAM

    def read(self, vram, n):
        o = self.off(vram); return self.rom[o:o + n]

    def w(self, vram): return struct.unpack('>I', self.read(vram, 4))[0]
    def h(self, vram): return struct.unpack('>h', self.read(vram, 2))[0]
    def f(self, vram): return struct.unpack('>f', self.read(vram, 4))[0]

    def s(self, vram):
        o = self.off(vram); e = self.rom.index(b'\0', o); return self.rom[o:e].decode('latin1')

    def asset_rom(self, index): return self.w(0x800C185C + index * 4)
    def asset_size(self, index): return self.w(0x8001CD64 + index * 4)

    def asset(self, index):
        o = self.asset_rom(index)
        data = lz(self.rom, o, False) if index in self.LZ_INDICES else inflate(self.rom, o)
        return data


class Rush2049:
    MAIN_VRAM = 0x80086A50
    MAIN_ROM = 0xB0CB10

    def __init__(self, path=RUSH2049_ROM):
        self.rom = open(path, 'rb').read()
        self.main = zlib.decompressobj(-15).decompress(self.rom[self.MAIN_ROM:self.MAIN_ROM + 0x60000])
        t = 0x8011B5BC - self.MAIN_VRAM
        self.offsets = []
        while True:
            v = struct.unpack('>I', self.main[t:t + 4])[0]
            if self.offsets and (v <= self.offsets[-1] or v > self.MAIN_ROM):
                break
            self.offsets.append(v); t += 4
        self.offsets.append(self.MAIN_ROM)
        self._cache = {}

    def w(self, vram): return struct.unpack('>I', self.main[vram - self.MAIN_VRAM:vram - self.MAIN_VRAM + 4])[0]

    def file(self, k):
        if k in self._cache:
            return self._cache[k]
        o, e = self.offsets[k], self.offsets[k + 1]
        try:
            z = zlib.decompressobj(-15); r = z.decompress(self.rom[o:e + 16])
            if not z.eof: raise ValueError
        except Exception:
            try: r = lz(self.rom, o, True)
            except Exception: r = self.rom[o:e]
        self._cache[k] = r
        return r

    def chunks(self, data):
        """2049 chunk directory: word 0 = offset of {tag, offset, size} entries."""
        d = struct.unpack('>I', data[:4])[0]; out = {}
        while d + 12 <= len(data):
            tag = data[d:d + 4]
            if not tag.isalpha(): break
            off, size = struct.unpack('>II', data[d + 4:d + 12])
            out[tag.decode()] = (off, size); d += 12
        return out
