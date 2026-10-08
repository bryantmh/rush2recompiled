"""Compact per-function summaries of Rush 2 code, for naming functions without reading full disassembly.

For each function (bounds from syms/rush2.us.syms.toml) prints: callers, arguments read before written, whether
v0/f0 is returned, calls with the origin of their a0-a3/f12/f14 arguments, struct-field accesses grouped by base
(args followed through moves and loads, e.g. a0->0x24.w = word at offset 0x24 of the struct a0 points to),
globals touched (lui pairs resolved), integer and float constants, loops, jump tables, function pointers taken,
and the docs that mention the address. Names come from the rush2 registry, the syms file and any pending
naming batches in tmp/naming/*.jsonl ({"vram": "800xxxxx", "name": ...} per line).

Usage: python fsum.py ADDR [ADDR...]        summaries of these functions
       python fsum.py --order [--all]        unnamed functions in bottom-up call order (callees first), one per line
                                             with size and number of unnamed callees; --all includes named ones
       python fsum.py --globals              every global variable (address, number of functions using it)
       python fsum.py --gnext FILE N         summaries (writers, readers, widths, initial value) of the next N globals in
                                             FILE without an entry in tmp/naming/g*.jsonl; DONE when none is left
       python fsum.py --batch N K            summaries of the K-th (0-based) slice of N slices of that order
       python fsum.py --next FILE N          summaries of the next N addresses listed in FILE (one hex address per line)
                                             that have no entry yet in tmp/naming/*.jsonl; prints DONE when none is left
Width letters: b/h/w = 8/16/32-bit int, f = float, d = double; r = read, w = write.
"""
import glob, json, os, re, struct, sys, tomllib
import rabbitizer
import roms

REPO = roms.REPO
MAIN_END = 0x800D0110   # end of the inflated main segment (code to 0x800BCBB0, then data); bss follows
OVL_END = 0x803CAF60    # end of the inflated overlay (code and data)
_rom = None


def rom():
    global _rom
    if _rom is None:
        _rom = roms.Rush2()
    return _rom


def word(a):
    if not (0x80000400 <= a < MAIN_END or 0x803AA800 <= a < OVL_END):
        return None
    r = rom()
    o = r.off(a)
    if o + 4 > len(r.rom):
        return None
    return struct.unpack('>I', r.rom[o:o + 4])[0]


def funcs():
    d = tomllib.load(open(os.path.join(REPO, 'syms', 'rush2.us.syms.toml'), 'rb'))
    out = {}
    for s in d['section']:
        for f in s['functions']:
            out[f['vram']] = (f['name'], f['size'])
    return out


def data_registry():
    p = os.path.join(REPO, 'syms', 'names', 'rush2_data.toml')
    if not os.path.exists(p):
        return {}
    return {e['vram']: e for e in tomllib.load(open(p, 'rb')).get('var', [])}


def registry():
    p = os.path.join(REPO, 'syms', 'names', 'rush2.toml')
    return {e['vram']: e for e in tomllib.load(open(p, 'rb')).get('func', [])}


def pending():
    out = {}
    for fn in sorted(glob.glob(os.path.join(REPO, 'tmp', 'naming', '*.jsonl'))):
        if os.path.basename(fn).startswith('g'):
            continue   # global-variable batches
        for ln in open(fn, encoding='utf-8'):
            ln = ln.strip()
            if ln:
                try:
                    j = json.loads(ln)
                    out[int(str(j['vram']), 16)] = j
                except (ValueError, KeyError):
                    pass
    return out


DATA = data_registry()
_dkeys = None


def gfmt(a):
    """'800C2140' plus the global's name: '800C2140=race_mode', or '800C2144=race_state+0x4' inside a named
    struct/array (nearest named global below, within 0x200)."""
    global _dkeys
    if _dkeys is None:
        import bisect
        _dkeys = sorted(k for k, e in DATA.items() if 'name' in e)
    e = DATA.get(a)
    if e and 'name' in e:
        return '%08X=%s' % (a, e['name'])
    import bisect
    i = bisect.bisect_right(_dkeys, a) - 1
    if i >= 0 and a - _dkeys[i] < 0x200:
        b = DATA[_dkeys[i]]
        t = b.get('type', '')
        if '[' in t or 'struct' in t or t.endswith('*'):
            return '%08X=%s+0x%X' % (a, b['name'], a - _dkeys[i])
    return '%08X' % a


GUSE = {}   # global address -> {function: access tags}, filled by summarize()
FUNCS = funcs()
REG = registry()
PEND = pending()


def name(a):
    if a in PEND and PEND[a].get('name'):   # a pending batch's newer name wins
        return '%s_%08X' % (re.sub(r'_?%08x$' % a, '', PEND[a]['name'].lower()), a)
    e = REG.get(a)
    if e and 'name' in e:
        return '%s_%08X' % (e['name'], a)
    if a in FUNCS:
        return FUNCS[a][0]
    return 'func_%08X' % a


def is_named(a):
    return not name(a).startswith('func_')


def insns(a):
    n, size = FUNCS[a]
    return [rabbitizer.Instruction(word(a + i), vram=a + i) for i in range(0, size, 4)]


_graph = None


def graph():
    """callees[f] = set of called functions; callers[f] = set of calling functions (jal and tail j)."""
    global _graph
    if _graph:
        return _graph
    callees = {f: set() for f in FUNCS}
    callers = {f: set() for f in FUNCS}
    ptrs = {}
    for f, (n, size) in FUNCS.items():
        hi = {}
        for i in range(0, size, 4):
            w = word(f + i)
            op = w >> 26
            if op in (2, 3):
                t = ((f + i + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
                if t in FUNCS and (op == 3 or not (f <= t < f + size)):
                    callees[f].add(t)
                    callers[t].add(f)
            elif op == 0xF:
                hi[(w >> 16) & 31] = (w & 0xFFFF) << 16
            elif op == 9 and ((w >> 21) & 31) in hi:
                t = (hi[(w >> 21) & 31] + ((w & 0xFFFF) ^ 0x8000) - 0x8000) & 0xFFFFFFFF
                if t in FUNCS:
                    ptrs.setdefault(t, set()).add(f)
    _graph = (callees, callers, ptrs)
    return _graph


def docs_mentioning(a):
    key = re.compile(r'(?<![0-9A-Fa-f])%08X(?![0-9A-Fa-f])' % a, re.I)
    hits = []
    for fn in glob.glob(os.path.join(REPO, 'docs', '**', '*.md'), recursive=True):
        try:
            if key.search(open(fn, encoding='utf-8', errors='replace').read()):
                hits.append(os.path.relpath(fn, REPO))
        except OSError:
            pass
    return hits


_doc_cache = None


def doc_index():
    """address -> docs mentioning it, built once."""
    global _doc_cache
    if _doc_cache is None:
        _doc_cache = {}
        pat = re.compile(r'(?<![0-9A-Fa-f])(80[0-9A-Fa-f]{6})(?![0-9A-Fa-f])')
        for fn in glob.glob(os.path.join(REPO, 'docs', '**', '*.md'), recursive=True):
            if 'rush1' in fn.lower() or 'functions' in fn:
                continue   # SF Rush docs, and the generated registry tables
            rel = os.path.relpath(fn, REPO)
            for m in pat.finditer(open(fn, encoding='utf-8', errors='replace').read()):
                _doc_cache.setdefault(int(m.group(1), 16), set()).add(rel)
    return _doc_cache


WID = {'lb': 'b', 'lbu': 'b', 'sb': 'b', 'lh': 'h', 'lhu': 'h', 'sh': 'h', 'lw': 'w', 'sw': 'w', 'lwc1': 'f',
       'swc1': 'f', 'ldc1': 'd', 'sdc1': 'd', 'ld': 'D', 'sd': 'D', 'lwl': 'w', 'lwr': 'w', 'swl': 'w', 'swr': 'w'}
ARGS = ['a0', 'a1', 'a2', 'a3']
FARGS = ['f12', 'f14']


def cstr(a):
    """Printable ASCII string at a (in the boot/main/ovl image), or None."""
    if word(a) is None:
        return None
    r = rom()
    o = r.off(a)
    b = r.rom[o:o + 48]
    end = b.find(b'\0')
    if end < 2:
        return None
    t = b[:end]
    if all(32 <= c < 127 or c in (10, 9) for c in t):
        return '"%s"' % t.decode().replace('\n', '\\n')
    return None


def gname(v):
    s = cstr(v)
    return 'G%s%s' % (gfmt(v), s or '')


def fmt_float(v):
    if v != v or abs(v) > 1e9 or (v != 0 and abs(v) < 1e-6):
        return None
    s = ('%.6g' % v)
    return s


def rodata_float(a, dbl=False):
    w = word(a)
    if w is None:
        return None
    if dbl:
        w2 = word(a + 4)
        return struct.unpack('>d', struct.pack('>II', w, w2))[0]
    return struct.unpack('>f', struct.pack('>I', w))[0]


def summarize(f):
    n, size = FUNCS[f]
    callees, callers, ptrs = graph()
    ins = insns(f)
    st = State()
    read_first, written = [], set()
    acc = {}                        # base origin -> {off: set(rw+width)}
    glob_acc = {}                   # address -> set(rw+width)
    consts, floats, calls, fnptrs = [], [], [], []
    loops, jumptable = 0, False
    frame = 0
    stack_args = set()

    def rd(r):
        if r in ('f13', 'f15'):
            r = 'f12' if r == 'f13' else 'f14'
        if r in ARGS + FARGS and r not in written and r not in read_first:
            read_first.append(r)

    idx = 0
    while idx < len(ins):
        i = ins[idx]
        op = i.getOpcodeName()
        for chk, fld in (('readsRs', 'rs'), ('readsRt', 'rt'), ('readsRd', 'rd'),
                         ('readsFs', 'fs'), ('readsFt', 'ft'), ('readsFd', 'fd')):
            if getattr(i, chk)():
                rd(getattr(i, fld).name)
        if op == 'addiu' and i.rs.name == 'sp' and i.rt.name == 'sp' and i.getProcessedImmediate() < 0 and not frame:
            frame = -i.getProcessedImmediate()
        if i.isBranch() and i.getBranchOffsetGeneric() < 0:
            loops += 1
        if i.isJrNotRa():
            jumptable = True
        if op in WID:
            base, off = i.rs.name, i.getProcessedImmediate()
            rw = 'w' if i.doesStore() else 'r'
            tag = rw + WID[op]
            if base in st.hi:
                ga = (st.hi[base] + off) & 0xFFFFFFFF
                glob_acc.setdefault(ga, set()).add(tag)
                if op in ('lwc1', 'ldc1') and ga < 0x80400000:
                    v = rodata_float(ga, op == 'ldc1')
                    s = fmt_float(v) if v is not None else None
                    if s and s not in floats:
                        floats.append(s)
            elif base == 'sp':
                if rw == 'r' and frame and off >= frame + 0x10:
                    stack_args.add(off - frame)
            elif base in st.org and not st.org[base].startswith('#'):
                m = re.match(r'^&G([0-9A-F]{8})(?:=[\w+]*)?(?:"[^"]*")?(?:\+0x([0-9A-F]+))?$', st.org[base])
                if m:
                    ga = (int(m.group(1), 16) + int(m.group(2) or '0', 16) + off) & 0xFFFFFFFF
                    glob_acc.setdefault(ga, set()).add(tag)
                acc.setdefault(st.org[base], {}).setdefault(off, set()).add(tag)
        elif op == 'lui':
            pass
        elif op in ('addiu', 'ori') and i.rs.name in st.hi:
            v = (st.hi[i.rs.name] + i.getProcessedImmediate()) & 0xFFFFFFFF if op == 'addiu' else \
                st.hi[i.rs.name] | i.getProcessedImmediate()
            if v in FUNCS:
                fnptrs.append(name(v))
            elif not 0x80000000 <= v < 0x80800000:
                consts.append(v)
        elif op == 'mtc1' and i.rt.name in st.hi:
            s = fmt_float(struct.unpack('>f', struct.pack('>I', st.hi[i.rt.name]))[0])
            if s and s not in floats:
                floats.append(s)
        elif op in ('addiu', 'ori', 'addi', 'slti', 'sltiu', 'andi', 'xori') and i.rs.name != 'sp' and i.rt.name != 'sp':
            v = i.getProcessedImmediate()
            if not -2 <= v <= 2 and v not in (0xFF, 0xFFFF, -0x10, 0x10) or op in ('slti', 'sltiu') and v not in (0, 1):
                consts.append(v)
        call = None
        if i.isFunctionCall():
            call = name(i.getInstrIndexAsVram()) if i.isJumpWithAddress() else '(*%s)' % st.org.get(i.rs.name, i.rs.name)
        elif op == 'j' and not (f <= i.getInstrIndexAsVram() < f + size):
            call = 'TAIL ' + name(i.getInstrIndexAsVram())
        if call:
            if idx + 1 < len(ins):
                j = ins[idx + 1]
                for chk, fld in (('readsRs', 'rs'), ('readsRt', 'rt'), ('readsFs', 'fs'), ('readsFt', 'ft')):
                    if getattr(j, chk)():
                        rd(getattr(j, fld).name)
                st.step(j)
                note_writes(j, written)
            calls.append('%s(%s)' % (call, st.call_args()))
            written.update(('v0', 'v1', 'a0', 'a1', 'a2', 'a3', 'f0', 'f12', 'f13', 'f14', 'f15'))
            st.after_call(call)
            idx += 2
            continue
        st.step(i)
        note_writes(i, written)
        idx += 1
    for ga, tags in glob_acc.items():
        GUSE.setdefault(ga, {}).setdefault(f, set()).update(tags)
    for x in fnptrs:
        pass
    ret_v0 = ret_f0 = False
    for idx, i in enumerate(ins):
        if i.isJrRa():
            for j in ins[max(0, idx - 8):idx + 2]:
                if (j.modifiesRd() and j.rd.name == 'v0') or (j.modifiesRt() and j.rt.name == 'v0'):
                    ret_v0 = True
                if any(getattr(j, c)() and getattr(j, fl).name == 'f0'
                       for c, fl in (('modifiesFd', 'fd'), ('modifiesFt', 'ft'), ('modifiesFs', 'fs'))):
                    ret_f0 = True
    e = REG.get(f, {})
    p = PEND.get(f, {})
    out = []
    cl = sorted(callers.get(f, ()))
    pr = sorted(ptrs.get(f, ()))
    hdr = '## %08X %s size 0x%X  callers %d: %s' % (f, name(f), size, len(cl),
                                                    ', '.join(name(c) for c in cl[:8]) + (' ...' if len(cl) > 8 else ''))
    if pr:
        hdr += '  ptr-taken-by: ' + ', '.join(name(c) for c in pr[:4])
    out.append(hdr)
    args = [r for r in ARGS + FARGS if r in read_first]
    if stack_args:
        args.append('stack:' + ','.join('0x%X' % s for s in sorted(stack_args)))
    extra = ('  loops %d' % loops if loops else '') + ('  jumptable' if jumptable else '')
    out.append('  args: %s   ret: %s%s' % (' '.join(args) or '-',
                                           ' '.join(x for x, y in (('v0', ret_v0), ('f0', ret_f0)) if y) or '-', extra))
    if calls:
        out.append('  calls: ' + '; '.join(calls[:24]) + (' ...(%d more)' % (len(calls) - 24) if len(calls) > 24 else ''))
    for b in sorted(acc, key=lambda b: (b.count('->'), b))[:12]:
        fields = acc[b]
        s = ' '.join('0x%X.%s' % (o, '/'.join(sorted(fields[o]))) for o in sorted(fields)[:24])
        out.append('  %s: %s%s' % (b, s, ' ...(%d)' % len(fields) if len(fields) > 24 else ''))
    if glob_acc:
        g = sorted(glob_acc)
        out.append('  globals: ' + ' '.join('%s.%s' % (gfmt(a), '/'.join(sorted(glob_acc[a]))) for a in g[:24]) +
                   (' ...(%d)' % len(g) if len(g) > 24 else ''))
    cs = []
    for v in consts:
        if v not in cs:
            cs.append(v)
    if cs:
        out.append('  consts: ' + ' '.join(('0x%X' % v if v >= 0 else '-0x%X' % -v) for v in cs[:20]))
    if floats:
        out.append('  floats: ' + ' '.join(floats[:16]))
    if fnptrs:
        out.append('  fnptrs: ' + ' '.join(sorted(set(fnptrs))[:10]))
    d = sorted(doc_index().get(f, ()))
    if d:
        out.append('  docs (may mean another game): ' + ' '.join(d[:4]))
    if e.get('desc') or p.get('desc'):
        out.append('  known: ' + (p.get('desc') or e.get('desc'))[:400])
    return '\n'.join(out)


def note_writes(i, written):
    for chk, fld in (('modifiesRt', 'rt'), ('modifiesRd', 'rd'), ('modifiesFs', 'fs'), ('modifiesFt', 'ft'),
                     ('modifiesFd', 'fd')):
        if getattr(i, chk)():
            written.add(getattr(i, fld).name)


class State:
    """Linear value-origin tracking: what each register holds (arg, field of arg, global, constant, return)."""

    def __init__(self):
        self.org = {r: r for r in ARGS}
        self.forg = {'f12': 'f12', 'f14': 'f14'}
        self.hi = {}
        self.slot = {}

    def call_args(self):
        used = [self.org.get(r, '?') for r in ARGS]
        while used and (used[-1] == '?' or used[-1] == ARGS[len(used) - 1]):
            used.pop()
        fl = ['%s=%s' % (r, self.forg[r]) for r in FARGS if r in self.forg and self.forg[r] != r]
        return ', '.join(used + fl)

    def after_call(self, call):
        for r in ('v0', 'v1', 'a0', 'a1', 'a2', 'a3', 't0', 't1', 't2', 't3', 't4', 't5', 't6', 't7', 't8', 't9', 'at'):
            self.org.pop(r, None)
            self.hi.pop(r, None)
        self.org['v0'] = 'ret:' + call.split('(')[0].replace('TAIL ', '')
        self.forg = {k: v for k, v in self.forg.items() if k[1:].isdigit() and int(k[1:]) >= 20}
        self.forg['f0'] = self.org['v0']

    def step(self, i):
        org, forg, hi = self.org, self.forg, self.hi
        op = i.getOpcodeName()
        if op == 'lui':
            d = i.rt.name
            hi[d] = i.getProcessedImmediate() << 16 & 0xFFFFFFFF
            org[d] = '#0x%X' % hi[d]
            return
        if op in ('addiu', 'ori') and i.rs.name in hi:
            v = (hi[i.rs.name] + i.getProcessedImmediate()) & 0xFFFFFFFF if op == 'addiu' else \
                hi[i.rs.name] | i.getProcessedImmediate()
            d = i.rt.name
            hi.pop(d, None)
            if v in FUNCS:
                org[d] = '&' + name(v)
            elif 0x80000000 <= v < 0x80800000:
                org[d] = '&' + gname(v)
                hi[d] = v
            else:
                org[d] = '#0x%X' % v
            return
        if op in ('addiu', 'ori', 'addi') and i.rs.name == 'zero':
            v = i.getProcessedImmediate()
            org[i.rt.name] = '#%d' % v if -10 <= v <= 10 else '#0x%X' % v
            hi.pop(i.rt.name, None)
            return
        if op == 'addiu' and i.rs.name == 'sp':
            if i.rt.name != 'sp':
                org[i.rt.name] = '&local_%X' % i.getProcessedImmediate()
                hi.pop(i.rt.name, None)
            return
        if op == 'addiu' and i.rs.name in org:
            off = i.getProcessedImmediate()
            src = org[i.rs.name]
            if src.startswith('#'):
                org.pop(i.rt.name, None)
            else:
                org[i.rt.name] = src if off == 0 else ('%s+0x%X' % (src, off) if off > 0 else '%s-0x%X' % (src, -off))
            hi.pop(i.rt.name, None)
            return
        if op in ('addu', 'or', 'daddu') and (i.rt.name == 'zero' or i.rs.name == 'zero'):
            s = i.rs.name if i.rt.name == 'zero' else i.rt.name
            d = i.rd.name
            if s in org:
                org[d] = org[s]
            else:
                org.pop(d, None)
            if s in hi:
                hi[d] = hi[s]
            else:
                hi.pop(d, None)
            return
        if op in WID and i.doesStore() and i.rs.name == 'sp':
            r = i.ft.name if op in ('swc1', 'sdc1') else i.rt.name
            src = (forg if op in ('swc1', 'sdc1') else org).get(r)
            if src:
                self.slot[i.getProcessedImmediate()] = src
            else:
                self.slot.pop(i.getProcessedImmediate(), None)
            return
        if op in WID and i.doesLoad():
            base, off = i.rs.name, i.getProcessedImmediate()
            if base == 'sp':
                src = self.slot.get(off)
            elif base in hi:
                src = 'G' + gfmt((hi[base] + off) & 0xFFFFFFFF)
            elif base in org and not org[base].startswith('#'):
                src = '%s->0x%X' % (org[base], off)
            else:
                src = None
            if op in ('lwc1', 'ldc1'):
                d = i.ft.name
                if base in hi:
                    ga = (hi[base] + off) & 0xFFFFFFFF
                    v = rodata_float(ga, op == 'ldc1') if ga < 0x80400000 else None
                    s = fmt_float(v) if v is not None else None
                    forg[d] = s if s and word(ga) is not None else src
                elif src:
                    forg[d] = src
                else:
                    forg.pop(d, None)
            else:
                d = i.rt.name
                hi.pop(d, None)
                if src and src.count('->') < 3:
                    org[d] = src if op == 'lw' or base == 'sp' else '(%s)' % src if not src.startswith('(') else src
                else:
                    org.pop(d, None)
            return
        if op == 'mtc1':
            if i.rt.name in hi:
                forg[i.fs.name] = fmt_float(struct.unpack('>f', struct.pack('>I', hi[i.rt.name]))[0]) or '?'
            elif i.rt.name == 'zero':
                forg[i.fs.name] = '0.0'
            else:
                forg.pop(i.fs.name, None)
            return
        if op in ('mov.s', 'mov.d'):
            if i.fs.name in forg:
                forg[i.fd.name] = forg[i.fs.name]
            else:
                forg.pop(i.fd.name, None)
            return
        for chk, fld in (('modifiesRt', 'rt'), ('modifiesRd', 'rd')):
            if getattr(i, chk)():
                org.pop(getattr(i, fld).name, None)
                hi.pop(getattr(i, fld).name, None)
        for chk, fld in (('modifiesFs', 'fs'), ('modifiesFt', 'ft'), ('modifiesFd', 'fd')):
            if getattr(i, chk)():
                forg.pop(getattr(i, fld).name, None)


def gvars():
    """Global variables: data addresses some function touches, minus strings and read-only float constants."""
    out = []
    for a, v in GUSE.items():
        if a >= 0xA0000000 or cstr(a):
            continue
        tags = set().union(*v.values())
        if (a < MAIN_END or 0x803AA800 <= a < OVL_END) and tags <= {'rf', 'rd'}:
            continue
        out.append(a)
    return out


def PEND_DATA():
    out = set()
    for fn in glob.glob(os.path.join(REPO, 'tmp', 'naming', 'g*.jsonl')):
        for ln in open(fn, encoding='utf-8'):
            try:
                out.add(int(str(json.loads(ln)['vram']), 16))
            except (ValueError, KeyError):
                pass
    return out


def gsummary(g):
    users = GUSE.get(g, {})
    tags = sorted(set().union(*users.values())) if users else []
    init = ''
    if word(g) is not None:
        w = word(g)
        fl = fmt_float(struct.unpack('>f', struct.pack('>I', w))[0])
        init = '  init 0x%08X%s' % (w, (' (%s f)' % fl) if fl and w else '')
    elif g >= MAIN_END:
        init = '  bss'
    us = sorted(users, key=lambda f: name(f))
    rd = [name(f) for f in us if any(t[0] == 'r' for t in users[f])]
    wr = [name(f) for f in us if any(t[0] == 'w' for t in users[f])]
    s = '## %s %s%s\n  written by %d: %s\n  read by %d: %s' % (
        gfmt(g), '/'.join(tags), init, len(wr), ', '.join(wr[:10]) + (' ...' if len(wr) > 10 else ''),
        len(rd), ', '.join(rd[:10]) + (' ...' if len(rd) > 10 else ''))
    d = DATA.get(g, {})
    if d.get('desc'):
        s += '\n  known: ' + d['desc'][:300]
    docs = sorted(doc_index().get(g, ()))
    if docs:
        s += '\n  docs: ' + ' '.join(docs[:3])
    return s


def order(include_named=False):
    """Bottom-up: repeatedly take functions whose callees are all already taken (cycles broken by size)."""
    callees, callers, ptrs = graph()
    todo = set(FUNCS)
    done = []
    taken = set()
    while todo:
        ready = [f for f in todo if all(c in taken or c == f for c in callees[f])]
        if not ready:
            ready = [min(todo, key=lambda f: (len([c for c in callees[f] if c not in taken]), f))]
        for f in sorted(ready):
            todo.discard(f)
            taken.add(f)
            done.append(f)
    if not include_named:
        done = [f for f in done if not is_named(f)]
    return done


if __name__ == '__main__':
    a = sys.argv[1:]
    if not a:
        print(__doc__)
    elif a[0] == '--order':
        callees, _, _ = graph()
        for f in order('--all' in a):
            print('%08X %-40s 0x%-5X unnamed callees %d' % (f, name(f), FUNCS[f][1],
                                                            sum(1 for c in callees[f] if not is_named(c))))
    elif a[0] == '--next':
        todo = [int(x.split()[0], 16) for x in open(a[1]) if x.strip()]
        todo = [f for f in todo if f not in PEND]
        if not todo:
            print('DONE')
        for f in todo[:int(a[2])]:
            print(summarize(f))
        print('# %d left after these' % max(0, len(todo) - int(a[2])))
    elif a[0] in ('--globals', '--gnext'):
        for f in FUNCS:
            summarize(f)
        if a[0] == '--globals':
            for g in sorted(gvars()):
                print('%08X %d' % (g, len(GUSE[g])))
        else:
            todo = [int(x.split()[0], 16) for x in open(a[1]) if x.strip()]
            todo = [g for g in todo if g not in PEND_DATA()]
            if not todo:
                print('DONE')
            for g in todo[:int(a[2])]:
                print(gsummary(g))
            print('# %d left after these' % max(0, len(todo) - int(a[2])))
    elif a[0] == '--batch':
        nsl, k = int(a[1]), int(a[2])
        o = order()
        per = (len(o) + nsl - 1) // nsl
        for f in o[k * per:(k + 1) * per]:
            print(summarize(f))
    else:
        for x in a:
            print(summarize(int(x.replace('func_', '').split('_')[-1], 16)))
