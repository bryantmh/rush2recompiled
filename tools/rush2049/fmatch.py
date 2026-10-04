"""Match Rush 2 functions to Rush 2049 functions by mnemonic n-gram similarity (research aid).

Usage: python fmatch.py [r2func ...]   (no args: write tools/rush2049/out/fmatch.txt for all r2 functions)
"""
import os, re, sys, struct, pickle
import rabbitizer, roms, decomp

def funcs(game):
    rd = decomp.reader(game); st = decomp.bounds(game); out = {}
    lo, hi = (0x800539E0, 0x803D0000) if game == 'r2' else (0x80086A50, 0x80120000)
    st = [a for a in st if lo <= a < hi]
    for i, a in enumerate(st):
        e = st[i + 1] if i + 1 < len(st) else a + 0x400
        e = min(e, a + 0x4000)
        ops = []
        for x in range(a, e, 4):
            try: w = rd(x)
            except Exception: break
            ops.append(rabbitizer.Instruction(w, vram=x).getOpcodeName() if w else 'nop')
        out[a] = ops
    return out

def grams(ops, n=4):
    return set(tuple(ops[i:i + n]) for i in range(len(ops) - n + 1))

def build():
    cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out', 'fmatch.pkl')
    if os.path.exists(cache): return pickle.load(open(cache, 'rb'))
    a = funcs('r2'); b = funcs('49')
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
        best = sorted(((c / len(g | gb[f]), f) for f, c in cnt.items()), reverse=True)[:3]
        res[k] = best
    pickle.dump(res, open(cache, 'wb'))
    return res

if __name__ == '__main__':
    res = build()
    if len(sys.argv) > 1:
        for f in sys.argv[1:]:
            a = int(f, 16); print('%08X' % a, ['%08X %.2f' % (f2, s) for s, f2 in res.get(a, [])])
    else:
        with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out', 'fmatch.txt'), 'w') as o:
            for a in sorted(res):
                o.write('%08X ' % a + ' '.join('%08X:%.2f' % (f2, s) for s, f2 in res[a]) + '\n')
