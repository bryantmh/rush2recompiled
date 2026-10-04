"""Match Rush 1 functions to Rush 2 functions by mnemonic 4-gram similarity (research aid).

Usage: python fmatch1.py            writes out/fmatch1.txt (every Rush 2 function -> best Rush 1 matches)
       python fmatch1.py r2 ADDR..  best Rush 1 matches for Rush 2 functions
       python fmatch1.py r1 ADDR..  best Rush 2 matches for Rush 1 functions
"""
import os, re, sys, struct, pickle
import rabbitizer
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'rush2049'))
import decomp
from r1 import Rush1, MAIN_VRAM

HERE = os.path.dirname(os.path.abspath(__file__))

def r1_funcs():
    starts = sorted(int(m.group(1), 16) for m in re.finditer(r'^func_([0-9A-F]{8}):', open(os.path.join(HERE, 'out', 'r1.asm')).read(), re.M))
    r = Rush1(); out = {}
    for i, a in enumerate(starts):
        e = min(starts[i + 1] if i + 1 < len(starts) else a + 0x400, a + 0x4000)
        ops = []
        for x in range(a, e, 4):
            d = r.read(x, 4)
            if len(d) < 4: break
            w = struct.unpack('>I', d)[0]
            ops.append(rabbitizer.Instruction(w, vram=x).getOpcodeName() if w else 'nop')
        out[a] = ops
    return out

def r2_funcs():
    rd = decomp.reader('r2'); st = [a for a in decomp.bounds('r2') if 0x80000400 <= a < 0x803D0000]; out = {}
    for i, a in enumerate(st):
        e = min(st[i + 1] if i + 1 < len(st) else a + 0x400, a + 0x4000)
        ops = []
        for x in range(a, e, 4):
            try: w = rd(x)
            except Exception: break
            ops.append(rabbitizer.Instruction(w, vram=x).getOpcodeName() if w else 'nop')
        out[a] = ops
    return out

def grams(ops, n=4): return set(tuple(ops[i:i + n]) for i in range(len(ops) - n + 1))

def match(a, b):
    ga = {k: grams(v) for k, v in a.items()}; gb = {k: grams(v) for k, v in b.items()}
    inv = {}
    for k, g in gb.items():
        for x in g: inv.setdefault(x, []).append(k)
    res = {}
    for k, g in ga.items():
        if len(g) < 3: continue
        cnt = {}
        for x in g:
            l = inv.get(x, [])
            if len(l) > 200: continue
            for f in l: cnt[f] = cnt.get(f, 0) + 1
        res[k] = sorted(((c / len(g | gb[f]), f) for f, c in cnt.items()), reverse=True)[:3]
    return res

def build():
    cache = os.path.join(HERE, 'out', 'fmatch1.pkl')
    if os.path.exists(cache): return pickle.load(open(cache, 'rb'))
    a = r2_funcs(); b = r1_funcs()
    res = (match(a, b), match(b, a))
    pickle.dump(res, open(cache, 'wb'))
    return res

if __name__ == '__main__':
    r2r1, r1r2 = build()
    if len(sys.argv) > 2:
        tab = r2r1 if sys.argv[1] == 'r2' else r1r2
        for f in sys.argv[2:]:
            a = int(f, 16); print('%08X' % a, ['%08X %.2f' % (f2, s) for s, f2 in tab.get(a, [])])
    else:
        with open(os.path.join(HERE, 'out', 'fmatch1.txt'), 'w') as o:
            for a in sorted(r2r1):
                o.write('%08X ' % a + ' '.join('%08X:%.2f' % (f2, s) for s, f2 in r2r1[a]) + '\n')
