"""Find tight busy-wait loops (short backward branches whose bodies only load memory and compare).

On hardware these loops wait for another thread or an interrupt to change memory; under the
recomp runtime game threads are cooperative, so each one needs a yield hook in us.toml.
Usage: py tools/find_spin_loops.py
"""
import struct
from pathlib import Path

import rabbitizer

ROOT = Path(__file__).resolve().parent.parent
SECTIONS = [("boot", 0x00001000, 0x80000400, 0x18400), ("main", 0x01000000, 0x800539E0, 0x691D0),
            ("ovl", 0x01080000, 0x803AA800, 0x1C190)]
MAX_LOOP_INSNS = 8
SAFE_PREFIXES = ("lw", "lh", "lb", "lbu", "lhu", "andi", "and", "sltu", "slt", "slti", "sltiu", "nop", "lui",
                 "addiu", "or", "ori", "sll", "srl", "sra", "xor", "xori", "beq", "bne", "beql", "bnel", "beqz",
                 "bnez", "beqzl", "bnezl", "b", "move")

rom = (ROOT / "rush2.us.recomp.z64").read_bytes()
for name, rom_addr, vram, size in SECTIONS:
    words = struct.unpack(f">{size // 4}I", rom[rom_addr:rom_addr + size])
    for i, w in enumerate(words):
        insn = rabbitizer.Instruction(w, vram=vram + i * 4)
        if not insn.isBranch():
            continue
        target = insn.getBranchVramGeneric()
        start = (target - vram) // 4
        if not (0 <= i - start < MAX_LOOP_INSNS):
            continue
        body = [rabbitizer.Instruction(words[j], vram=vram + j * 4) for j in range(start, i + 2)]
        if all(b.getOpcodeName() in SAFE_PREFIXES for b in body) and any(b.doesLoad() for b in body):
            print(f"{name} loop 0x{target:08X}..0x{vram + i * 4:08X}: " +
                  "; ".join(b.disassemble() for b in body))
