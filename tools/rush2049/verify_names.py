"""Verify the function registries (syms/names/*.toml) against the actual ROMs (research aid).

Checks, for every rush2, rush2049 and rush1 entry (games are r2 / 49 / r1; set RUSH1_ROM for Rush 1):
  - the address is word aligned, inside the game's code, and looks like a function start (reached by a jal, or begins
    with a stack frame setup `addiu sp,sp,-N`, or is a leaf that ends in jr ra);
  - for entries with a `rush2 =` link: how similar the two functions are (mnemonic sequence ratio and size ratio),
    which is how the 2049 <-> Rush 2 pairings in the docs were or were not confirmed.
Also `show GAME ADDR [ADDR...]` prints a function's disassembly with jal targets resolved (and registry names).

Also: `match [--apply]` (find functions whose address-masked code equals a Rush 2 function's in Rush 1 and 2049 and link them; ambiguous groups go to tmp/names_match.txt unlinked), `links` (classify each 2049->Rush 2 link as same-code or role in the registry), `rank` (where each 2049->Rush 2 link ranks among all Rush 2 functions by 4-gram Jaccard) and
`best r2|49 ADDR` (best matches in the other game), `refs r2|49 LO HI` (code that forms an address in [LO,HI)).
Usage:  RUSH2_ROM=rush2.us.recomp.z64 RUSH2049_ROM=<2049 .z64> RUSH1_ROM=<SF Rush .z64> python verify_names.py [check|show r2|49 ADDR...]
        (check writes tmp/names_verify.txt at the repo root and prints only the suspicious rows)
Needs rabbitizer (pip install rabbitizer). Default Rush 2 ROM is the repo copy made by tools/extract.py.
"""
import difflib, os, struct, sys, tomllib
import rabbitizer
import roms

REPO = roms.REPO
_cache = {}


def segments(game):
    """[(base_vram, bytes)] of the code of a game."""
    if game in _cache:
        return _cache[game]
    if game == 'r2':
        r = roms.Rush2()
        segs = [(0x80000400, r.rom[0x1000:0x1000 + (0x800539E0 - 0x80000400)]),
                (0x800539E0, r.rom[0x01000000:0x01000000 + (0x800BCBB0 - 0x800539E0)]),
                (0x803AA800, r.rom[0x01080000:0x01080000 + 0x1C190])]
    elif game == 'r1':
        sys.path.insert(0, os.path.join(REPO, 'tools', 'rush1'))
        import r1
        r = r1.Rush1()
        segs = [(0x80000400, r.rom[0x1000:0x1000 + (0x8001E2E0 - 0x80000400)]), (r1.MAIN_VRAM, r.main)]
    else:
        r = roms.Rush2049()
        segs = [(0x80000400, r.rom[0x1000:0x1000 + (r.MAIN_VRAM - 0x80000400)]), (r.MAIN_VRAM, r.main)]
    _cache[game] = segs
    return segs


def word(game, a):
    for base, d in segments(game):
        if base <= a < base + len(d) - 3:
            return struct.unpack('>I', d[a - base:a - base + 4])[0]
    return None


def in_code(game, a):
    return word(game, a) is not None


def jal_targets(game):
    key = (game, 'jal')
    if key in _cache:
        return _cache[key]
    t = {}
    for base, d in segments(game):
        for i in range(0, len(d) - 3, 4):
            w = struct.unpack('>I', d[i:i + 4])[0]
            if w >> 26 == 3:
                tgt = ((base + i + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                t[tgt] = t.get(tgt, 0) + 1
    _cache[key] = t
    return t


def branch_target(w, a):
    op = w >> 26
    if op in (4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17) or (op == 1 and ((w >> 16) & 0x1F) < 0x14) or \
            (op == 0x11 and (w >> 21) & 0x1F == 8):
        off = w & 0xFFFF
        off = off - 0x10000 if off & 0x8000 else off
        return a + 4 + off * 4
    return None


def func_words(game, start, limit=0x4000):
    """Instruction words of the function at `start`: up to the jr ra (past the furthest branch) and its delay slot."""
    out = []
    furthest = start
    a = start
    while a < start + limit:
        w = word(game, a)
        if w is None:
            break
        out.append(w)
        t = branch_target(w, a)
        if t is not None and t > furthest and t < start + limit:
            furthest = t
        if w >> 26 == 0 and w & 0x3F == 8 and (w >> 21) & 0x1F == 31 and a >= furthest:
            d = word(game, a + 4)
            if d is not None:
                out.append(d)
            break
        a += 4
    return out


def mnems(ws, base):
    return [rabbitizer.Instruction(w, vram=base + 4 * i).getOpcodeName() if w else 'nop' for i, w in enumerate(ws)]


def looks_like_start(game, a):
    w = word(game, a)
    if w is None:
        return 'outside code'
    called = jal_targets(game).get(a, 0)
    prologue = (w >> 26) == 9 and (w >> 16) & 0x1F == 29 and (w >> 21) & 0x1F == 29 and w & 0x8000
    if called or prologue:
        return None
    ws = func_words(game, a, 0x200)
    if ws and ws[-2:-1] and ws[-2] >> 26 == 0 and ws[-2] & 0x3F == 8:
        return 'leaf, never jal-called (maybe a callback)'
    return 'no jal caller and no stack prologue'


def starts(game):
    """Function starts: jal targets plus every `addiu sp,sp,-N` (prologue) in the code."""
    key = (game, 'starts')
    if key in _cache:
        return _cache[key]
    st = set(jal_targets(game))
    for base, d in segments(game):
        for i in range(0, len(d) - 3, 4):
            w = struct.unpack('>I', d[i:i + 4])[0]
            if (w >> 16) == 0x27BD and w & 0x8000:
                st.add(base + i)
    st = sorted(a for a in st if in_code(game, a))
    _cache[key] = st
    return st


def grams(game, n=4):
    """{start: set of mnemonic n-grams} for every function of the game."""
    key = (game, 'grams')
    if key in _cache:
        return _cache[key]
    out = {}
    for a in starts(game):
        ws = func_words(game, a, 0x3000)
        m = mnems(ws, a)
        if len(m) >= n:
            out[a] = set(tuple(m[i:i + n]) for i in range(len(m) - n + 1))
    _cache[key] = out
    return out


def best_matches(src_game, addr, dst_game='r2', top=3):
    """Rank the functions of dst_game by 4-gram Jaccard similarity to the function at addr in src_game."""
    g = grams(src_game).get(addr)
    if not g:
        return []
    res = []
    for b, gb in grams(dst_game).items():
        j = len(g & gb) / len(g | gb)
        if j > 0.05:
            res.append((j, b))
    res.sort(reverse=True)
    return res[:top]


def rank_links(game='rush2049', key='49'):
    """For every entry of `game` with a rush2 link: where the link ranks among all Rush 2 functions."""
    for e in load_reg(game):
        if 'rush2' not in e:
            continue
        v, t = e['vram'], e['rush2']
        top = best_matches(key, v, 'r2', 5)
        tj = None
        g, gb = grams(key).get(v), grams('r2').get(t)
        if g and gb:
            tj = len(g & gb) / len(g | gb)
        rank = next((i + 1 for i, (j, b) in enumerate(top) if b == t), None)
        print('%08X %-28s ->%08X  jaccard %s  rank %s  top: %s' % (
            v, e.get('name', ''), t, '%.2f' % tj if tj is not None else '-', rank or '>5',
            ' '.join('%08X:%.2f' % (b, j) for j, b in top[:3])))


def refs(game, lo, hi):
    """Instructions that form an address in [lo, hi) from lui + addiu/ori/load/store (same base register, linear scan).
    Prints pc, the enclosing function start, and the instruction."""
    import bisect
    st = starts(game)
    for base, d in segments(game):
        lui = {}
        for i in range(0, len(d) - 3, 4):
            w = struct.unpack('>I', d[i:i + 4])[0]
            pc = base + i
            op, rs, rt, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, w & 0xFFFF
            if op == 0xF:
                lui[rt] = imm << 16
                continue
            if op in (9, 0xD, 0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x31, 0x35, 0x39, 0x3D) and rs in lui:
                off = imm - 0x10000 if imm & 0x8000 else imm
                a = (lui[rs] + off) & 0xFFFFFFFF if op != 0xD else lui[rs] | imm
                if lo <= a < hi:
                    f = st[bisect.bisect_right(st, pc) - 1]
                    print('%08X in func_%08X  %08X  %s   -> %08X' % (
                        pc, f, w, rabbitizer.Instruction(w, vram=pc).disassemble(), a))
            if op == 3 or (w >> 26 == 0 and w & 0x3F == 8):
                lui = {}


def set_links():
    """Write `link = "same-code"|"role"` on every rush2049 entry with a rush2 link, from the measured similarity
    (mnemonic sequence ratio >= 0.75 and size ratio >= 0.6 -> same-code). Entries that already have a link keep it."""
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    import names
    n = 0
    for game, key in (('rush2049', '49'), ('rush1', 'r1')):
      funcs = names.load(game)
      for e in funcs:
        if 'rush2' in e and 'link' not in e:
            wa, wb = func_words(key, e['vram']), func_words('r2', e['rush2'])
            if not wa or not wb:
                continue
            sim = difflib.SequenceMatcher(None, mnems(wa, e['vram']), mnems(wb, e['rush2']), autojunk=False).ratio()
            size = min(len(wa), len(wb)) / max(len(wa), len(wb))
            e['link'] = 'same-code' if sim >= 0.75 and size >= 0.6 else 'role'
            n += 1
      names.dump(game, funcs)
    print('%d links classified' % n)


# ---- cross-game identical-function search ----------------------------------------------------------------------------

def reader(game):
    """word reader over code AND data (data beyond the text segments, for resolving float constants)."""
    key = (game, 'rdr')
    if key in _cache:
        return _cache[key]
    if game == 'r2':
        r = roms.Rush2()
        rd = lambda a: struct.unpack('>I', r.read(a, 4))[0]
    elif game == '49':
        r = roms.Rush2049()
        def rd(a):
            if a >= r.MAIN_VRAM:
                return struct.unpack('>I', r.main[a - r.MAIN_VRAM:a - r.MAIN_VRAM + 4])[0]
            return struct.unpack('>I', r.rom[0x1000 + a - 0x80000400:0x1000 + a - 0x80000400 + 4])[0]
    else:
        sys.path.insert(0, os.path.join(REPO, 'tools', 'rush1'))
        import r1
        r = r1.Rush1()
        rd = lambda a: struct.unpack('>I', r.read(a, 4))[0]
    _cache[key] = rd
    return rd


def norm_key(game, ws, base):
    """Address-independent signature of a function: jal/j targets and address immediates (lui of an address plus the
    low half in the paired addiu/ori/load/store) are masked; float constants loaded through such addresses (lwc1/ldc1)
    are replaced by their VALUE, so two copies of the same code with different tuning constants do not match."""
    rd = reader(game)
    marked = set()
    out = []
    for i, w in enumerate(ws):
        op, rs, rt, rd_, imm = w >> 26, (w >> 21) & 31, (w >> 16) & 31, (w >> 11) & 31, w & 0xFFFF
        if op in (2, 3):
            out.append(('j', op))
            continue
        if op == 0xF:
            if imm >= 0x8000:
                out.append(('lui', rt)); marked.add(rt); hi = {}
            else:
                out.append(w); marked.discard(rt)
            continue
        if op in (9, 0xD) and rs in marked:
            out.append((op, rs, rt)); marked.add(rt)
            continue
        if rs in marked and op in (0x20, 0x21, 0x23, 0x24, 0x25, 0x28, 0x29, 0x2B, 0x31, 0x35, 0x39, 0x3D):
            val = None
            if op in (0x31, 0x35):
                # find the matching lui to resolve the address
                a = None
                for k in range(i - 1, max(i - 40, -1), -1):
                    wk = ws[k]
                    if wk >> 26 == 0xF and (wk >> 16) & 31 == rs:
                        off = imm - 0x10000 if imm & 0x8000 else imm
                        a = ((wk & 0xFFFF) << 16) + off
                        break
                if a is not None:
                    try:
                        val = (rd(a), rd(a + 4)) if op == 0x35 else rd(a)
                    except Exception:
                        val = 'unreadable'
            out.append((op, rs, rt, val))
            if op in (0x20, 0x21, 0x23, 0x24, 0x25):
                marked.discard(rt)
            continue
        out.append(w)
        if op == 0:
            marked.discard(rd_)
        elif op not in (0x2B, 0x28, 0x29, 0x31, 0x35, 0x39, 0x3D, 4, 5, 6, 7, 0x14, 0x15, 0x16, 0x17, 1, 0x11):
            marked.discard(rt)
    return tuple(out)


def clean_starts(game):
    """starts() minus prologue-looking addresses that lie inside another function (kept if jal-called)."""
    key = (game, 'clean')
    if key in _cache:
        return _cache[key]
    jt = jal_targets(game)
    res, end = [], 0
    for a in starts(game):
        if a < end and a not in jt:
            continue
        ws = func_words(game, a, 0x4000)
        res.append(a)
        end = max(end, a + 4 * len(ws))
    _cache[key] = res
    return res


def r2_symbol_names():
    """vram -> name for functions that the signature matcher already named in syms/rush2.us.syms.toml."""
    import re
    out = {}
    p = os.path.join(REPO, 'syms', 'rush2.us.syms.toml')
    for ln in open(p, encoding='utf-8'):
        m = re.search(r'name = "([^"]+)", vram = 0x([0-9A-Fa-f]+)', ln)
        if m and not m.group(1).startswith('func_'):
            out[int(m.group(2), 16)] = m.group(1)
    return out


def match_games(min_insns=8, apply=False):
    """Find functions of Rush 1 / Rush 2049 whose address-masked code (and float constants) equals a Rush 2 function's.
    Unambiguous matches (one Rush 2 function per signature) are linked: `rush2 = <addr>`, `link = "same-code"`.
    Writes tmp/names_match.txt (all groups, including the ambiguous ones that are NOT linked)."""
    sys.path.insert(0, os.path.join(REPO, 'tools'))
    import names
    sigs = {}
    for game in ('r2', '49', 'r1'):
        d = {}
        for a in clean_starts(game):
            ws = func_words(game, a, 0x4000)
            if len(ws) < min_insns or (len(ws) > 3 and all(x == 0 for x in ws[:-2])):
                continue
            d.setdefault(norm_key(game, ws, a), []).append(a)
        sigs[game] = d
    r2sym = r2_symbol_names()
    reg = {g: {e['vram']: e for e in names.load(g)} for g in ('rush2', 'rush2049', 'rush1')}
    report, linked, ambiguous, conflicts = [], {}, 0, 0
    for game, gname in (('49', 'rush2049'), ('r1', 'rush1')):
        for key, addrs in sigs[game].items():
            r2a = sigs['r2'].get(key)
            if not r2a:
                continue
            pairs = None
            if len(r2a) == 1:
                pairs = [(a, r2a[0], False) for a in addrs]
            elif len(r2a) == len(addrs):
                # N identical copies on each side (library duplicates, compiler-generated helpers): pair by address order
                pairs = [(a, t, True) for a, t in zip(sorted(addrs), sorted(r2a))]
                report.append('%s %s -> paired by order with Rush 2 %s' % (
                    gname, ' '.join('%08X' % a for a in sorted(addrs)), ' '.join('%08X' % a for a in sorted(r2a))))
            else:
                ambiguous += len(addrs)
                report.append('%s %s -> %d Rush 2 candidates %s (not linked)' % (
                    gname, ' '.join('%08X' % a for a in addrs), len(r2a), ' '.join('%08X' % a for a in r2a)))
                continue
            for a, t, by_order in pairs:
                n = len(func_words('r2', t, 0x4000))
                e = reg[gname].get(a)
                if e is not None and 'rush2' in e and e['rush2'] != t:
                    conflicts += 1
                    report.append('CONFLICT %s %08X is linked to %08X but its code matches Rush 2 %08X' % (gname, a, e['rush2'], t))
                    continue
                if e is None:
                    e = {'vram': a}
                    reg[gname][a] = e
                    r2e = reg['rush2'].get(t, {})
                    sym = r2sym.get(t) or ('%s_%08X' % (r2e['name'], t) if 'name' in r2e else 'func_%08X' % t)
                    e['desc'] = 'Same code as Rush 2 %s (%d instructions, address-masked match, float constants equal).%s' % (
                        sym, n, (' ' + r2e['desc'].split(' | ')[0]) if r2e.get('desc') else '')
                    e['status'] = 'inferred'
                    if by_order:
                        e['desc'] += ' One of %d identical copies; paired with this Rush 2 copy by address order, so which copy is which is a guess.' % len(r2a)
                    if r2e.get('area'):
                        e['area'] = r2e['area']
                r2e = reg['rush2'].get(t, {})
                if e.get('desc', '').startswith('Same code as Rush 2 '):
                    # auto-made entry: refresh it from the Rush 2 knowledge (names propagate after naming Rush 2)
                    sym = '%s_%08X' % (r2e['name'], t) if 'name' in r2e else (r2sym.get(t) or 'func_%08X' % t)
                    e['desc'] = 'Same code as Rush 2 %s (%d instructions, address-masked match, float constants equal).%s' % (
                        sym, n, (' ' + r2e['desc'].split(' | ')[0]) if r2e.get('desc') else '')
                    if by_order:
                        e['desc'] += ' One of %d identical copies; paired with this Rush 2 copy by address order, so which copy is which is a guess.' % len(r2a)
                    for k in ('area', 'sig'):
                        if r2e.get(k) and k not in e:
                            e[k] = r2e[k]
                if r2e.get('name') and not by_order and 'name' not in e:
                    e['name'] = r2e['name']
                    if 'sig' not in e and r2e.get('sig'):
                        e['sig'] = r2e['sig']
                e['rush2'] = t
                e.setdefault('link', 'same-code')
                linked.setdefault(gname, set()).add(a)
    for g in ('rush2049', 'rush1'):
        print('%-9s %4d functions linked to a Rush 2 function' % (g, len(linked.get(g, ()))))
    print('%d ambiguous (not linked), %d conflicts with existing links' % (ambiguous, conflicts))
    os.makedirs(os.path.join(REPO, 'tmp'), exist_ok=True)
    open(os.path.join(REPO, 'tmp', 'names_match.txt'), 'w').write('\n'.join(report) + '\n')
    if apply:
        for g in ('rush2049', 'rush1'):
            names.dump(g, list(reg[g].values()))
        print('registries updated')


def load_reg(game):
    p = os.path.join(REPO, 'syms', 'names', game + '.toml')
    return tomllib.load(open(p, 'rb')).get('func', []) if os.path.exists(p) else []


def check():
    rows = []
    r2 = {e['vram']: e for e in load_reg('rush2')}
    for g, key in (('rush2', 'r2'), ('rush2049', '49'), ('rush1', 'r1')):
        for e in load_reg(g):
            v = e['vram']
            msgs = []
            m = looks_like_start(key, v)
            if m:
                msgs.append(m)
            sim = None
            if 'rush2' in e:
                if e['rush2'] not in r2:
                    msgs.append('linked rush2 %08X has no rush2 entry' % e['rush2'])
                wa, wb = func_words(key, v), func_words('r2', e['rush2'])
                if wa and wb:
                    ma, mb = mnems(wa, v), mnems(wb, e['rush2'])
                    sim = difflib.SequenceMatcher(None, ma, mb, autojunk=False).ratio()
                    size = min(len(wa), len(wb)) / max(len(wa), len(wb))
                    flag = '' if sim >= 0.75 and size >= 0.6 else '  <-- WEAK'
                    msgs.append('link->%08X mnemonic sim %.2f size ratio %.2f (%d vs %d insns)%s' % (
                        e['rush2'], sim, size, len(wa), len(wb), flag))
                else:
                    msgs.append('link->%08X unreadable' % e['rush2'])
            rows.append((g, v, e.get('name', ''), msgs))
    outp = os.path.join(REPO, 'tmp', 'names_verify.txt')
    os.makedirs(os.path.dirname(outp), exist_ok=True)
    bad = 0
    with open(outp, 'w') as o:
        for g, v, n, msgs in rows:
            line = '%-8s %08X %-34s %s' % (g, v, n, '; '.join(msgs))
            o.write(line + '\n')
            if any(('outside' in m or 'no jal' in m or 'WEAK' in m or 'no rush2' in m or 'unreadable' in m or 'leaf' in m)
                   for m in msgs):
                print(line)
                bad += 1
    print('%d entries checked, %d suspicious; full table in %s' % (len(rows), bad, outp))


def show(game, addrs):
    names = {}
    for g, k in (('rush2', 'r2'), ('rush2049', '49'), ('rush1', 'r1')):
        if k == game:
            for e in load_reg(g):
                if 'name' in e:
                    names[e['vram']] = '%s_%08X' % (e['name'], e['vram'])
    for a in addrs:
        a = int(a, 16)
        print('# %08X %s' % (a, names.get(a, '')))
        for i, w in enumerate(func_words(game, a)):
            pc = a + 4 * i
            ins = rabbitizer.Instruction(w, vram=pc)
            txt = ins.disassemble()
            if w >> 26 == 3:
                t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                txt += '   ; %s' % names.get(t, 'func_%08X' % t)
            print('%08X  %08X  %s' % (pc, w, txt))


if __name__ == '__main__':
    if len(sys.argv) >= 2 and sys.argv[1] == 'match':
        match_games(apply='--apply' in sys.argv)
    elif len(sys.argv) >= 2 and sys.argv[1] == 'links':
        set_links()
    elif len(sys.argv) >= 2 and sys.argv[1] == 'rank':
        rank_links()
        rank_links('rush1', 'r1')
    elif len(sys.argv) >= 5 and sys.argv[1] == 'refs':
        refs(sys.argv[2], int(sys.argv[3], 16), int(sys.argv[4], 16))
    elif len(sys.argv) >= 4 and sys.argv[1] == 'best':
        for j, b in best_matches(sys.argv[2], int(sys.argv[3], 16), 'r2' if sys.argv[2] != 'r2' else '49', 8):
            print('%08X %.2f' % (b, j))
    elif len(sys.argv) >= 4 and sys.argv[1] == 'show':
        show(sys.argv[2], sys.argv[3:])
    else:
        check()
