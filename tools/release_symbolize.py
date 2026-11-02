#!/usr/bin/env python3
"""Name code addresses in a release Rush2Recompiled.exe (no PDB) using a local build that has one.

Player crash dumps come from release builds whose PDB isn't shipped. For each RVA given (e.g. the 0xab571f of
"Rush2Recompiled+0xab571f" in a cdb stack), this cuts 16-byte windows of code around it from the release exe, finds
each window in the local exe, and votes on the shift between the two builds; the shifted RVA is then named with cdb
against the local PDB. Windows holding call/rip-relative displacements simply fail to match, so the vote survives
builds that moved code around.

  python tools/release_symbolize.py RELEASE_EXE 0xab571f 0x87eae1 ...  [--local build/Rush2Recompiled.exe]

Get the release exe with `gh release download vX.Y.Z -p "*.zip"` (into tmp/). Matches are close but not proof:
check the disassembly when it matters (cdb -z dump -i <release dir> -c "u Rush2Recompiled+0x...").
"""
import argparse
import collections
import os
import subprocess
import sys

import pefile

CDB = r"C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe"


def image(path):
    pe = pefile.PE(path, fast_load=True)
    return pe.get_memory_mapped_image()


def match(rel, loc, rva, width=16):
    votes = collections.Counter()
    for start in range(rva - 64, rva + 48, 4):
        window = rel[start:start + width]
        if len(window) < width or window.count(0xCC) > 4:
            continue
        hit = loc.find(window)
        if hit < 0 or loc.find(window, hit + 1) >= 0:
            continue
        votes[hit - start] += 1
    if not votes:
        return None, 0
    shift, n = votes.most_common(1)[0]
    return rva + shift, n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("release_exe")
    ap.add_argument("rvas", nargs="+")
    ap.add_argument("--local", default="build/Rush2Recompiled.exe")
    a = ap.parse_args()
    rel, loc = image(a.release_exe), image(a.local)
    found = []
    for s in a.rvas:
        rva = int(s, 16)
        local_rva, votes = match(rel, loc, rva)
        found.append((rva, local_rva, votes))
    cmds = "; ".join(f"ln Rush2Recompiled+{l:#x}" for _, l, _ in found if l is not None) + "; q"
    names = {}
    if any(l is not None for _, l, _ in found):
        out = subprocess.run([CDB, "-z", a.local, "-y", os.path.dirname(os.path.abspath(a.local)), "-c", cmds],
                             capture_output=True, text=True).stdout
        order = [l for _, l, _ in found if l is not None]
        lines = [ln for ln in out.splitlines() if ln.startswith("(")]
        for l, line in zip(order, lines):
            names[l] = line.split(")", 1)[1].split("|")[0].strip()
    for rva, l, votes in found:
        if l is None:
            print(f"{rva:#010x}  (no match)")
        else:
            print(f"{rva:#010x} -> {l:#010x}  votes {votes:2}  {names.get(l, '?')}")


if __name__ == "__main__":
    sys.exit(main())
