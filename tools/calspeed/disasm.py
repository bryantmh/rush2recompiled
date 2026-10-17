#!/usr/bin/env python3
"""Disassembles California Speed's main code (N64 ROM, see cs.py) at an address (research aid).

    python tools/calspeed/disasm.py ADDR [COUNT]      COUNT instructions from N64 address ADDR (default 64)
    (code that addresses a data range: tools/rush2049/verify_names.py refs cs LO HI)
"""
import os, sys
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cs

addr = int(sys.argv[1], 16)
count = int(sys.argv[2]) if len(sys.argv) > 2 else 64
c = cs.CalSpeed()
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_BIG_ENDIAN)
for ins in md.disasm(c.read(addr, count * 4), addr):
    print('%08X: %-8s %s' % (ins.address, ins.mnemonic, ins.op_str))
