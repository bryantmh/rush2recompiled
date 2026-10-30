# Checks an RT64 disk shader cache (build/shader_cache/rt64_shaders_*.bin, written by
# lib/rt64/src/render/rt64_raster_shader_cache.cpp): walks the records and reports any whose DXIL
# containers are malformed (bad header size, part offsets out of range, unreadable part names) and
# descriptions stored more than once. --extract N writes record N's shaders next to the cache file.
import argparse
import struct
import sys

RECORD_MAGIC = 0x43455253  # "SREC"


def container_ok(blob):
    if len(blob) < 32 or blob[:4] != b'DXBC' or struct.unpack_from('<I', blob, 24)[0] != len(blob):
        return False
    count = struct.unpack_from('<I', blob, 28)[0]
    if 32 + count * 4 > len(blob):
        return False
    for off in struct.unpack_from('<%dI' % count, blob, 32):
        if off + 8 > len(blob) or not blob[off:off + 4].isalnum():
            return False
        if off + 8 + struct.unpack_from('<I', blob, off + 4)[0] > len(blob):
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('cache')
    ap.add_argument('--extract', type=int)
    args = ap.parse_args()
    data = open(args.cache, 'rb').read()
    pos, index, bad, seen = 16, 0, 0, {}
    while pos + 32 <= len(data):
        if struct.unpack_from('<I', data, pos)[0] != RECORD_MAGIC:
            print(f'record {index} at {pos}: bad record magic, stopping')
            bad += 1
            break
        desc = data[pos + 4:pos + 24]
        vs_size, ps_size = struct.unpack_from('<II', data, pos + 24)
        vs = data[pos + 32:pos + 32 + vs_size]
        ps = data[pos + 32 + vs_size:pos + 32 + vs_size + ps_size]
        if len(ps) < ps_size:
            print(f'record {index} at {pos}: truncated')
            break
        vs_ok, ps_ok = container_ok(vs), container_ok(ps)
        if not (vs_ok and ps_ok):
            bad += 1
            print(f'record {index} at {pos}: desc {desc.hex()} vs {"ok" if vs_ok else "BAD"} ({vs_size}) ps {"ok" if ps_ok else "BAD"} ({ps_size})')
        seen.setdefault(desc, []).append(index)
        if args.extract == index:
            open(args.cache + f'.{index}.vs.dxil', 'wb').write(vs)
            open(args.cache + f'.{index}.ps.dxil', 'wb').write(ps)
        pos += 32 + vs_size + ps_size
        index += 1
    dups = sum(1 for v in seen.values() if len(v) > 1)
    print(f'{index} records, {len(seen)} descriptions, {dups} stored more than once, {bad} bad')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
