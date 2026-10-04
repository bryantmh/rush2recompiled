"""Labeled disassembly of Rush 1's boot and main segments (research aid). Usage: python dis1.py [out.asm]"""
import struct, sys, os
import rabbitizer
from r1 import Rush1, MAIN_VRAM, BOOT_VRAM

r = Rush1()
BOOT_END = 0x8001E2E0
segments = [(BOOT_VRAM, r.rom[0x1000:0x1000 + BOOT_END - BOOT_VRAM]), (MAIN_VRAM, r.main)]

def words(seg):
    base, data = seg
    for i in range(0, len(data) - 3, 4):
        yield base + i, struct.unpack('>I', data[i:i + 4])[0]

funcs = set()
for seg in segments:
    for a, w in words(seg):
        if w >> 26 == 3:
            funcs.add(((a + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2))
out = open(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), 'out', 'r1.asm'), 'w')
for seg in segments:
    lui = {}
    for a, w in words(seg):
        if a in funcs:
            out.write('\nfunc_%08X:\n' % a); lui = {}
        ins = rabbitizer.Instruction(w, vram=a)
        t = ins.disassemble(); note = ''
        op = w >> 26; rs = (w >> 21) & 31; rt = (w >> 16) & 31; imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op == 0x0F:
            lui[rt] = imm << 16
        elif rs in lui and op in (0x09, 0x0D, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x31, 0x39, 0x35, 0x3D):
            addr = (lui[rs] + (imm if op == 0x0D else simm)) & 0xFFFFFFFF
            note = ' ; %08X' % addr
        if op not in (0x0F, 0x28, 0x29, 0x2B, 0x39, 0x3D, 0x04, 0x05, 0x14, 0x15, 0x01) and op != 0:
            if rt in lui and op not in (0x31, 0x35):
                lui.pop(rt, None)
        if op == 0 and ((w >> 11) & 31) in lui:
            lui.pop((w >> 11) & 31, None)
        if ins.isBranch() or ins.isJump():
            try: note += ' -> %08X' % ins.getBranchVramGeneric()
            except Exception: pass
        if w == 0x03E00008:
            lui = {}
        out.write('%08X %08X %s%s\n' % (a, w, t, note))
