# For reading ProjectR's native port (project-r.exe) of the arcade games: disassembles whole x86-64 functions.
# A function's bounds come from the PE's .pdata (x64 unwind) table. Strings a function points at (RIP-relative) are
# shown beside the instruction, and its direct callees are listed at the end.
#
#   python tools/arcade2049/pe_func.py <exe> <va> [<va> ...]        disassemble the functions holding these addresses
#   python tools/arcade2049/pe_func.py <exe> --callers <va>          list the functions that call <va> directly
#   python tools/arcade2049/pe_func.py <exe> --summary <va>          size, callees and strings only
import struct, sys
import capstone

exe = sys.argv[1]
b = open(exe, 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
nsec = struct.unpack_from('<H', b, pe + 6)[0]
opt = pe + 24
base = struct.unpack_from('<Q', b, opt + 24)[0]
secs = []
so = opt + struct.unpack_from('<H', b, pe + 20)[0]
for i in range(nsec):
    o = so + 40 * i
    vsz, va, rsz, raw = struct.unpack_from('<IIII', b, o + 8)
    secs.append((b[o:o + 8].rstrip(b'\0').decode(), va, vsz, raw, rsz))


def rva2off(rva):
    for _, va, vsz, raw, rsz in secs:
        if va <= rva < va + max(vsz, rsz):
            return raw + rva - va if rva - va < rsz else None
    return None


# .pdata: the exception directory (data directory 3): RUNTIME_FUNCTION {begin, end, unwind} RVAs.
exc_rva, exc_size = struct.unpack_from('<II', b, opt + 112 + 3 * 8)
funcs = []
o = rva2off(exc_rva)
for k in range(exc_size // 12):
    s, e, u = struct.unpack_from('<III', b, o + k * 12)
    funcs.append((s, e))
funcs.sort()


def func_at(rva):
    lo, hi = 0, len(funcs)
    while lo < hi:
        mid = (lo + hi) // 2
        if funcs[mid][0] <= rva:
            lo = mid + 1
        else:
            hi = mid
    if lo and funcs[lo - 1][0] <= rva < funcs[lo - 1][1]:
        return funcs[lo - 1]
    return None


def cstring(rva):
    off = rva2off(rva)
    if off is None:
        return None
    s = bytearray()
    while off < len(b) and b[off] and len(s) < 80:
        c = b[off]
        if c < 0x20 or c > 0x7E:
            return None
        s.append(c)
        off += 1
    return s.decode() if len(s) >= 4 else None


md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = True


def insns(start, end):
    off = rva2off(start)
    return md.disasm(b[off:off + end - start], base + start)


def rip_target(i):
    for op in i.operands:
        if op.type == capstone.x86.X86_OP_MEM and op.mem.base == capstone.x86.X86_REG_RIP:
            return i.address + i.size + op.mem.disp
    return None


def show(va, summary=False):
    f = func_at(va - base)
    if f is None:
        print('no function at', hex(va))
        return
    s, e = f
    print('== function %#x-%#x (%d bytes)' % (base + s, base + e, e - s))
    callees, strings = [], []
    for i in insns(s, e):
        note = ''
        t = rip_target(i)
        if t is not None:
            st = cstring(t - base)
            if st:
                note = '  ; "%s"' % st
                strings.append(st)
        if i.mnemonic == 'call' and i.operands and i.operands[0].type == capstone.x86.X86_OP_IMM:
            callees.append(i.operands[0].imm)
        if not summary:
            print('%x  %-7s %s%s' % (i.address, i.mnemonic, i.op_str, note))
    print('callees:', ' '.join(hex(c) for c in dict.fromkeys(callees)))
    if strings:
        print('strings:', ' | '.join(dict.fromkeys(strings)))


def callers(va):
    found = set()
    for s, e in funcs:
        for i in insns(s, e):
            if i.mnemonic in ('call', 'jmp') and i.operands and i.operands[0].type == capstone.x86.X86_OP_IMM \
                    and i.operands[0].imm == va:
                found.add((base + s, i.address))
    for f, at in sorted(found):
        print('%#x  (call at %#x)' % (f, at))


args = sys.argv[2:]
if args and args[0] == '--callers':
    callers(int(args[1], 16))
elif args and args[0] == '--summary':
    for a in args[1:]:
        show(int(a, 16), True)
else:
    for a in args:
        show(int(a, 16))
