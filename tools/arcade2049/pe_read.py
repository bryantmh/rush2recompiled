# For reading the Rush 2049 arcade code in ProjectR's native port (project-r.exe; docs/arcade2049.md events).
# Reads values at virtual addresses of a PE: python tools/arcade2049/pe_read.py <exe> <va> <struct fmt, e.g. <4f> [more va fmt pairs]
import struct, sys

b = open(sys.argv[1], 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
n = struct.unpack_from('<H', b, pe + 6)[0]
so = pe + 24 + struct.unpack_from('<H', b, pe + 20)[0]
base = struct.unpack_from('<Q', b, pe + 24 + 24)[0]


def va2off(va):
    rva = va - base
    for i in range(n):
        o = so + 40 * i
        vsz, v, rsz, raw = struct.unpack_from('<IIII', b, o + 8)
        if v <= rva < v + rsz:
            return raw + rva - v


args = sys.argv[2:]
for va, fmt in zip(args[0::2], args[1::2]):
    o = va2off(int(va, 16))
    print(va, fmt, None if o is None else struct.unpack_from(fmt, b, o))
