# For reading the Rush 2049 arcade code in ProjectR's native port (project-r.exe; docs/arcade2049.md events).
# Finds references to a string in an x86-64 PE: RIP-relative instructions that point at it and 8-byte pointers to it.
# python tools/arcade2049/pe_xref.py <exe> <string> [--near N]   (prints the referencing instructions with N instructions around)
import struct, sys
import capstone

exe, text = sys.argv[1], sys.argv[2].encode()
near = int(sys.argv[sys.argv.index('--near') + 1]) if '--near' in sys.argv else 6
b = open(exe, 'rb').read()
pe = struct.unpack_from('<I', b, 0x3C)[0]
nsec = struct.unpack_from('<H', b, pe + 6)[0]
opt = pe + 24
base = struct.unpack_from('<Q', b, opt + 24)[0]
secs = []
so = opt + struct.unpack_from('<H', b, pe + 20)[0]
for i in range(nsec):
    o = so + 40 * i
    name = b[o:o + 8].rstrip(b'\0').decode()
    vsz, va, rsz, raw = struct.unpack_from('<IIII', b, o + 8)
    secs.append((name, va, vsz, raw, rsz))

def off_to_rva(off):
    for n, va, vsz, raw, rsz in secs:
        if raw <= off < raw + rsz:
            return va + off - raw

targets = []
start = 0
while True:
    i = b.find(text, start)
    if i < 0:
        break
    # whole token: NUL or non-letter before and after
    if (i == 0 or not chr(b[i - 1]).isalpha()) and not chr(b[i + len(text)]).isalpha():
        targets.append(off_to_rva(i))
    start = i + 1
print('string rvas', [hex(t) for t in targets if t])
tset = set(t for t in targets if t)
# pointers in data
for n, va, vsz, raw, rsz in secs:
    if n == '.text':
        continue
    d = b[raw:raw + rsz]
    for k in range(0, len(d) - 8, 8):
        p = struct.unpack_from('<Q', d, k)[0] - base
        if p in tset:
            print('pointer in %s at rva %#x (len after: %d)' % (n, va + k, struct.unpack_from('<Q', d, k + 8)[0]))
# code refs
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
md.detail = False
for n, va, vsz, raw, rsz in secs:
    if n != '.text':
        continue
    code = b[raw:raw + rsz]
    # cheap scan: any 4-byte disp at position k such that rva(k+4+extra)+disp hits a target; then disassemble around
    hits = []
    for k in range(len(code) - 4):
        disp = struct.unpack_from('<i', code, k)[0]
        for extra in (0, 1, 4):
            if va + k + 4 + extra + disp in tset:
                hits.append(k)
    for k in hits:
        for s in range(max(0, k - 8), k):
            insns = list(md.disasm(code[s:s + 16], va + s + base, 1))
            if insns and s + insns[0].size > k and 'rip' in insns[0].op_str:
                ins = insns[0]
                tgt = None
                print('ref at %#x: %s %s' % (ins.address, ins.mnemonic, ins.op_str))
                break
