#!/usr/bin/env python3
"""Lists California Speed main-code instructions matching a regex, with the function start they fall in (research
aid; a function start = the nearest preceding `addiu $sp, $sp, -N`).

    python tools/calspeed/find_ins.py REGEX          e.g. "^sh .*, 0x10e[(]" (capstone syntax: "sh $v0, 0x10e($s1)")
"""
import os, re, sys
import capstone
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cs

pat = re.compile(sys.argv[1])
c = cs.CalSpeed()
md = capstone.Cs(capstone.CS_ARCH_MIPS, capstone.CS_MODE_MIPS64 + capstone.CS_MODE_BIG_ENDIAN)
md.skipdata = True
start = cs.MAIN_VRAM
for ins in md.disasm(c.read(cs.MAIN_VRAM, cs.MAIN_END - cs.MAIN_VRAM), cs.MAIN_VRAM):
    text = '%s %s' % (ins.mnemonic, ins.op_str)
    if ins.mnemonic == 'addiu' and ins.op_str.startswith('$sp, $sp, -'):
        start = ins.address
    if pat.search(text):
        print('%08X  (in %08X)  %s' % (ins.address, start, text))
