"""Generate the N64Recomp symbol file for Rush 2 (USA).

Function boundaries come from spimdisasm; libultra/libaudio names come from
matching n64sym's signature database (ref/n64sym/src/builtin_signatures.sig)
against the boot segment, plus a few manual disambiguations.

Usage: py tools/gen_syms.py   (after tools/extract.py)
Writes syms/rush2.us.syms.toml
"""
import csv
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SIG_PATH = ROOT / "ref/n64sym/src/builtin_signatures.sig"

# name, rom (fake rom for compressed segments), vram, size of .text
SECTIONS = [
    ("boot", 0x00001000, 0x80000400, 0x18400),  # text ends where rspboot starts (0x80018800)
    ("main", 0x01000000, 0x800539E0, 0x691D0),  # text ends at 0x800BCBB0 (osInvalICache range in boot)
    ("ovl",  0x01080000, 0x803AA800, 0x1C190),
]

# Signature matches that are ambiguous (identical code, different relocated data).
MANUAL_NAMES = {
    0x800071A0: "osViGetCurrentFramebuffer",  # reads __osViCurr (0x8001BAA0)
    0x800071E0: "osViGetNextFramebuffer",     # reads __osViNext (0x8001BAA4)
    0x8000C8F0: "osMotorStop",                # uses zero-filled motor data
    0x8000CA90: "osMotorStart",               # uses one-filled motor data
    0x80010290: "__ll_rem",
    0x80010700: "__osPiCreateAccessQueue",
    0x80010750: "__osPiGetAccess",
    0x80010794: "__osPiRelAccess",
    0x80011EF0: "__osSiCreateAccessQueue",
    0x80011F40: "__osSiGetAccess",
    0x80011F84: "__osSiRelAccess",
    0x80013360: "alMainBusParam",
    0x80014F60: "alAuxBusParam",
    0x80008580: "__osContGetInitData",
    0x800166A0: "__osPfsGetInitData",
    # The game stops/starts *other* threads around critical sections. Route these through wrappers with
    # libultra semantics instead of the runtime's reimplementations (see src/threads.cpp).
    0x80005FD0: "rush2_osStartThread",
    0x8000F640: "rush2_osStopThread",
    # The runtime's osPfs* functions always report no Controller Pak. Route these to the file-backed
    # implementation in src/pak.cpp.
    0x8000C650: "rush2_osPfsInitPak",
    0x8000D2F0: "rush2_osPfsFindFile",
    0x8000D4B0: "rush2_osPfsFileState",
    0x8000D7A0: "rush2_osPfsDeleteFile",
    0x8000DDC0: "rush2_osPfsAllocateFile",
    0x8000E66C: "rush2_osPfsReadWriteFile",
    0x8000EA90: "rush2_osPfsFreeBlocks",
    0x8000EBE0: "rush2_osPfsChecker",
}
# Function starts spimdisasm misses (only reached via computed jumps).
EXTRA_FUNCTION_STARTS = {
    0x80000450,  # boot main; the entrypoint clears bss then does `jr $t2` here
}

# Names that the signature scan finds for several unrelated tiny stubs; don't trust them.
UNTRUSTED_NAMES = {"ptstart", "__dummy", "osSyncPrintf", "__osGetId", "osScGetCmdQ"}


def load_sigs():
    syms, cur = [], None
    for line in SIG_PATH.read_text().splitlines():
        p = line.split()
        if not p or p[0].startswith("#"):
            continue
        if p[0].startswith("."):
            cur["relocs"] += [(int(o, 16), p[0], p[1]) for o in p[2:]]
        else:
            cur = {"name": p[0], "size": int(p[1], 16), "a": int(p[2], 16), "b": int(p[3], 16), "relocs": []}
            syms.append(cur)
    by_a = {}
    for s in syms:
        s["relocs"].sort()
        by_a.setdefault(s["a"], []).append(s)
    return by_a


def stripped(buf, off, sig):
    data = bytearray(buf[off:off + sig["size"]])
    if len(data) < sig["size"]:
        return None
    for o, typ, _ in sig["relocs"]:
        w = struct.unpack_from(">I", data, o)[0]
        w &= 0xFFFF0000 if typ in (".hi16", ".lo16") else 0xFC000000
        struct.pack_into(">I", data, o, w)
    return bytes(data)


def match_libultra(buf, vram, func_addrs, sigs_by_a):
    """Return {vram: name} for functions in buf matching a library signature."""
    names = {}
    for addr in func_addrs:
        off = addr - vram
        best = None
        for sigs in sigs_by_a.values():
            for s in sigs:
                if s["size"] < 12:
                    continue
                d = stripped(buf, off, s)
                if d is None or zlib.crc32(d[:8]) != s["a"] or zlib.crc32(d) != s["b"]:
                    continue
                if best is None or s["size"] > best["size"]:
                    best = s
        if best is not None:
            names[addr] = best["name"]
            # jal targets inside a matched function are named by the signature too
            for o, typ, target in best["relocs"]:
                if typ == ".targ26" and "_text_" not in target:
                    w = struct.unpack_from(">I", buf, off + o)[0]
                    names.setdefault(((w & 0x3FFFFFF) << 2) | 0x80000000, target)
    return names


def spimdisasm_functions(buf, vram, size, tmp):
    binpath = Path(tmp) / f"{vram:08X}.bin"
    binpath.write_bytes(buf)
    info = Path(tmp) / f"{vram:08X}.csv"
    subprocess.run([sys.executable, "-m", "spimdisasm", "singleFileDisasm", str(binpath), str(Path(tmp) / "asm"),
                    "--start", "0", "--end", hex(size), "--vram", hex(vram), "--function-info", str(info)],
                   check=True, capture_output=True)
    funcs = []
    for r in csv.DictReader(info.open()):
        funcs.append((int(r["address"], 16), int(r["length"], 16)))
    return sorted(funcs)


def main():
    rom = (ROOT / "rush2.us.recomp.z64").read_bytes()
    sigs = load_sigs()
    out = ["# Generated by tools/gen_syms.py - do not edit by hand.", ""]
    used_names = set()
    with tempfile.TemporaryDirectory() as tmp:
        for sec_name, sec_rom, sec_vram, sec_size in SECTIONS:
            buf = rom[sec_rom:sec_rom + sec_size]
            funcs = spimdisasm_functions(buf, sec_vram, sec_size, tmp)
            known = {a for a, _ in funcs}
            funcs = sorted(funcs + [(a, 0) for a in EXTRA_FUNCTION_STARTS
                                    if sec_vram <= a < sec_vram + sec_size and a not in known])
            names = {}
            if sec_name == "boot":
                names = match_libultra(buf, sec_vram, [a for a, _ in funcs], sigs)
                names = {a: n for a, n in names.items() if n not in UNTRUSTED_NAMES}
                names.update(MANUAL_NAMES)
            out += ["[[section]]", f'name = "{sec_name}"', f"rom = 0x{sec_rom:08X}", f"vram = 0x{sec_vram:08X}",
                    f"size = 0x{sec_size:X}", "", "functions = ["]
            named = 0
            for i, (addr, length) in enumerate(funcs):
                end = funcs[i + 1][0] if i + 1 < len(funcs) else sec_vram + sec_size
                length = min(length, end - addr) if length else end - addr
                name = names.get(addr)
                if name is None or name in used_names:
                    name = f"func_{addr:08X}"
                else:
                    named += 1
                used_names.add(name)
                out.append(f'    {{ name = "{name}", vram = 0x{addr:08X}, size = 0x{length:X} }},')
            out += ["]", ""]
            print(f"{sec_name}: {len(funcs)} functions, {named} named")
    dest = ROOT / "syms" / "rush2.us.syms.toml"
    dest.parent.mkdir(exist_ok=True)
    dest.write_text("\n".join(out))
    print(f"Wrote {dest}")


if __name__ == "__main__":
    main()
