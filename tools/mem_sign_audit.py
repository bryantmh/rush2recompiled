#!/usr/bin/env python3
"""Lists MEM_* accesses in C++ sources whose address argument may be unsigned (the uint32 offset trap).

MEM_W(offset, reg) indexes rdram with (reg + offset) - 0xFFFFFFFF80000000. When reg + offset is a 32-bit unsigned
value (a uint32_t variable or constant on either side), 0x80xxxxxx isn't sign-extended and the access lands 4 GB past
RDRAM's start: past the reservation, in the native heap, where a write silently corrupts whatever lives there.
This flags each MEM_ call whose address isn't wrapped in (int32_t)/(gpr)/(int64_t) and whose arguments use a name
whose nearest declaration before the call is uint32_t/unsigned/size_t, a function returning uint32_t, or a 0x8...
literal. Heuristic: read each hit.

  python tools/mem_sign_audit.py [paths...]   (default src/ include/)
"""
import os
import re
import sys

CALL = re.compile(r'\bMEM_(?:W|H|HU|B|BU|D|F|WU)\s*\(')
SIGNED = re.compile(r'^\s*(?:\((?:int32_t|gpr|int64_t|int|s32)\)|static_cast<int32_t>)')
DECL = re.compile(r'\b((?:const\s+|constexpr\s+|static\s+)*'
                  r'(?:u?int(?:8|16|32|64)_t|unsigned(?:\s+int)?|int|size_t|gpr|auto|s32|u32))'
                  r'\s+&?\s*(\w+)\s*(?==|;|,|\)|\{|\[)')
UNSIGNED = re.compile(r'.*\b(uint32_t|unsigned|size_t|u32)\b')
UFUNC = re.compile(r'\buint32_t\s+(?:[\w:]+::)?(\w+)\s*\([^;{)]*\)\s*(?:const\s*)?\{')


def args_of(text, i):
    depth, j, start, out = 0, i, i, []
    while j < len(text):
        c = text[j]
        if c == '(':
            depth += 1
        elif c == ')':
            if depth == 0:
                out.append(text[start:j])
                return out
            depth -= 1
        elif c == ',' and depth == 0:
            out.append(text[start:j])
            start = j + 1
        j += 1
    return out


def audit(path):
    text = open(path, encoding='utf-8', errors='replace').read()
    decls = [(d.start(), d.group(2), d.group(1)) for d in DECL.finditer(text)]
    ufuncs = set(UFUNC.findall(text))
    for m in CALL.finditer(text):
        a = args_of(text, m.end())
        if len(a) < 2:
            continue
        off, addr = a[0].strip(), a[1].strip()
        if SIGNED.match(addr):
            continue
        names = set(re.findall(r'\b[A-Za-z_]\w*\b', off + ' ' + addr))
        hit = set()
        for n in names:
            last = None
            for pos, name, typ in decls:
                if pos >= m.start():
                    break
                if name == n:
                    last = typ
            if last is not None and UNSIGNED.match(last):
                hit.add(n)
            elif n in ufuncs and re.search(r'\b' + n + r'\s*\(', off + addr):
                hit.add(n + '()')
        if not hit and not re.search(r'\b0x8[0-9A-Fa-f]{7}\b', addr):
            continue
        line = text.count('\n', 0, m.start()) + 1
        print(f'{path}:{line}: {text[m.start():m.end()]}{off}, {addr})  [{", ".join(sorted(hit)) or "literal"}]')


def main():
    for root in sys.argv[1:] or ['src', 'include']:
        for dp, _, fs in os.walk(root):
            for f in sorted(fs):
                if f.endswith(('.cpp', '.h', '.hpp', '.c')):
                    audit(os.path.join(dp, f))


if __name__ == '__main__':
    main()
