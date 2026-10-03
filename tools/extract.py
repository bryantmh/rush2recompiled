"""Prepare Rush 2 (USA) ROM images for recompilation.

Outputs (next to the repo root):
  rush2.us.z64         - big-endian copy of the input ROM
  rush2.us.recomp.z64  - the same ROM with the deflate-compressed code segments
                         appended uncompressed at fake ROM addresses, so N64Recomp
                         can read their instructions.

Usage: py tools/extract.py "Rush 2 - Extreme Racing.n64"
"""
import hashlib
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

EXPECTED_SHA1 = "01b0a31b1accc18061e8c85f6c5edf0dbd79f097"  # big-endian (.z64) image

# Compressed code segments: (name, real rom offset of raw deflate stream, fake rom address)
# The game inflates these itself at runtime via func_800059D4(rom, dest, flag).
CODE_SEGMENTS = [
    ("main", 0xAFD0C0, 0x01000000),  # -> 0x800539E0
    ("ovl",  0xB3D20E, 0x01080000),  # -> 0x803AA800
]
RECOMP_ROM_SIZE = 0x01100000


def to_big_endian(data: bytes) -> bytes:
    magic = data[:4]
    if magic == b"\x80\x37\x12\x40":
        return data
    if magic == b"\x37\x80\x40\x12":  # byteswapped (.v64 / this .n64)
        out = bytearray(data)
        out[0::2], out[1::2] = data[1::2], data[0::2]
        return bytes(out)
    if magic == b"\x40\x12\x37\x80":  # little-endian
        out = bytearray(len(data))
        for i in range(4):
            out[i::4] = data[3 - i::4]
        return bytes(out)
    raise SystemExit("Unrecognized ROM format")


def inflate_at(rom: bytes, offset: int) -> bytes:
    z = zlib.decompressobj(-15)
    out = z.decompress(rom[offset:])
    if not z.eof:
        raise SystemExit(f"Deflate stream at 0x{offset:X} did not terminate")
    return out


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    rom = to_big_endian(Path(sys.argv[1]).read_bytes())
    sha1 = hashlib.sha1(rom).hexdigest()
    if sha1 != EXPECTED_SHA1:
        raise SystemExit(f"Unexpected ROM (sha1 {sha1}); expected Rush 2 USA {EXPECTED_SHA1}")
    (ROOT / "rush2.us.z64").write_bytes(rom)

    recomp_rom = bytearray(rom) + bytes(RECOMP_ROM_SIZE - len(rom))
    for name, src, dst in CODE_SEGMENTS:
        seg = inflate_at(rom, src)
        recomp_rom[dst:dst + len(seg)] = seg
        print(f"{name}: rom 0x{src:X} -> 0x{len(seg):X} bytes at fake rom 0x{dst:X}")
    (ROOT / "rush2.us.recomp.z64").write_bytes(recomp_rom)
    print("Wrote rush2.us.z64 and rush2.us.recomp.z64")


if __name__ == "__main__":
    main()
