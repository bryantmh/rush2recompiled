"""Rush 2 menu research helpers (track-select screen, track-indexed UI tables, widget lists).

Usage (from tools/rush2049):
    python menus.py tables        dump the per-track UI tables (names, logos, diorama objects, option labels)
    python menus.py widgets       dump every static widget list passed to the screen builder func_800604FC
    python menus.py refs ADDR..   find aligned data words equal to ADDR (pointer tables / widget callbacks)
    python menus.py logos OUTDIR  write the 13 Rush 2 track-name logos (assets 4..0x10, CI8 128x32, rows stored bottom-up) as PNG
    python menus.py diorama       list the objects in asset 3 (track-select dioramas) with sizes

Findings are in docs/rush2049_research/menus.md.
"""
import os, re, struct, sys, zlib
import roms

r2 = None


def R():
    global r2
    if r2 is None:
        r2 = roms.Rush2()
    return r2


def o2v(i):
    if 0x1000 <= i < 0x1000 + 0x539E0 - 0x400: return i - 0x1000 + 0x80000400
    if 0x1000000 <= i < 0x1080000: return i - 0x1000000 + 0x800539E0
    if 0x1080000 <= i < 0x10C0000: return i - 0x1080000 + 0x803AA800
    return None


def sptr(v):
    r = R()
    if 0x80000400 <= v < 0x803E0000:
        try:
            s = r.s(v)
        except Exception:
            return None
        if s and len(s) < 80 and all(32 <= ord(c) < 127 or c == '\n' for c in s):
            return s
    return None


# Per-track tables found in the menu code. (address, count, kind, description)
TABLES = [
    (0x800C25AC, 12, 'str', 'logo texture names for the high-score screen func_800606A8 (widget 800C2304)'),
    (0x800C182C, 12, 'str', 'track file prefixes (func_8008F080 <prefix>FINISH, func_800A5110)'),
    (0x800C4C40, 12, 'str', 'full display names (LAS VEGAS..STUNT 1); entries 12+ are Controller Pak strings'),
    (0x803C91E0, 12, 'str', 'track-select diorama object names in asset 3 (func_803AB294)'),
    (0x803C9180, 12, 'f32', 'track-select diorama scale (func_803ABE0C draw loop)'),
    (0x803C91B0, 12, 'f32', 'track-select cloud height above diorama (func_803AB294)'),
    (0x803C9668, 12, 'str', 'track-select name logo texture names (widget callback func_803C6208)'),
    (0x803C900C, 13, 'str', 'records-screen logo names, 12 tracks + CIRCUIT (func_803C5468)'),
    (0x803C7D6C, 7, 'str', 'race-track logo names, tracks 0-6 (func_803BE528)'),
    (0x803CAC10, 11, 'ptr', 'track-select option jump table (func_803ABE0C), option ids 0..10'),
    (0x800C48F8, 11, 'str', 'option labels, English (index lang*11 + option)'),
    (0x800C4B94, 6, 'str', 'screen titles (SELECT TRACK, SELECT CAR, RECORDS, ...)'),
]


def tables():
    r = R()
    for a, n, kind, desc in TABLES:
        print('%08X  %s' % (a, desc))
        for i in range(n):
            v = r.w(a + 4 * i)
            if kind == 'f32':
                print('    %2d  %g' % (i, r.f(a + 4 * i)))
            elif kind == 'str':
                print('    %2d  %08X %r' % (i, v, sptr(v)))
            else:
                print('    %2d  %08X' % (i, v))


def widgets_at(a, n):
    """Widget record (0x28 bytes): char *texture, s16 x, s16 y, 3 x u32 -1, u32 0, u32 0, u32 0xFF,
    callback (per-frame update/draw, called with the live widget), u32 arg (the live widget keeps it at +0x2C)."""
    r = R(); out = []
    for i in range(n):
        o = a + i * 0x28
        tex, xy = r.w(o), r.w(o + 4)
        cb, arg = r.w(o + 0x20), r.w(o + 0x24)
        out.append((o, arg, sptr(tex) if tex != 0xFFFFFFFF else None, xy >> 16, xy & 0xFFFF, cb))
    return out


def screen_lists():
    """Static lists passed as a2 to jal 0x800604FC (count in a3), found in analysis/out_disasm/r2.asm."""
    asm = os.path.join(roms.REPO, 'analysis', 'out_disasm', 'r2.asm')
    lines = open(asm).read().splitlines(); res = []; fn = None
    for k, ln in enumerate(lines):
        if ln.startswith('func_'): fn = ln[:-1]
        if 'jal         0x800604FC' in ln:
            hi = a2 = a3 = None
            for w in lines[max(0, k - 12):k + 2]:
                m = re.search(r'lui\s+\$a2, (0x[0-9A-F]+)', w)
                if m: hi = int(m.group(1), 16) << 16
                m = re.search(r'addiu\s+\$a2, \$a2, (-?0x[0-9A-F]+)', w)
                if m and hi is not None: a2 = (hi + int(m.group(1), 16)) & 0xFFFFFFFF
                m = re.search(r'addiu\s+\$a3, \$zero, (0x[0-9A-F]+)', w)
                if m: a3 = int(m.group(1), 16)
            if a2 and a3: res.append((fn, a2, a3))
    return res


def widgets():
    for fn, a, n in screen_lists():
        print('%s  list %08X  %d widgets' % (fn, a, n))
        for o, arg, tex, x, y, cb in widgets_at(a, n):
            print('    %08X arg=%08X %-16s (%3d,%3d) cb=%08X' % (o, arg, tex, x, y, cb))


def refs(targets):
    r = R(); rom = r.rom
    for t in targets:
        b = struct.pack('>I', t); i = 0x1000
        while True:
            i = rom.find(b, i)
            if i < 0 or i > 0x10C0000: break
            if i % 4 == 0 and o2v(i): print('%08X referenced at %08X' % (t, o2v(i)))
            i += 1


def png(path, w, h, rgba):
    raw = b''.join(b'\0' + bytes(rgba[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xFFFFFFFF)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0))
                           + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))


def rgba5551(c):
    return [((c >> 11) & 31) * 255 // 31, ((c >> 6) & 31) * 255 // 31, ((c >> 1) & 31) * 255 // 31, 255 * (c & 1)]


def logos(outdir):
    import model
    r = R(); os.makedirs(outdir, exist_ok=True)
    for idx in range(4, 0x11):
        d = r.asset(idx); m = model.R2Model(d)
        t = m.textures[0]; p = m.palettes[0]
        w, h = t['w'], t['h']; tex = d[t['texels']:t['texels'] + w * h]
        pal = [struct.unpack_from('>H', d, p['pal'] + 2 * i)[0] for i in range(256)]
        rgba = []
        for y in range(h - 1, -1, -1):  # stored bottom row first, like 2049's TPIC thumbnails
            for b in tex[y * w:(y + 1) * w]: rgba += rgba5551(pal[b])
        fn = os.path.join(outdir, 'logo_%02X_%s.png' % (idx, t['name']))
        png(fn, w, h, rgba); print(fn, w, h, 'fmt %08X' % t['fmt'])


def diorama():
    import model
    r = R(); d = r.asset(3); m = model.R2Model(d)
    print('asset 3: %d bytes, %d models, %d textures' % (len(d), len(m.models), len(m.textures)))
    starts = sorted(set(l[3] for x in m.models for l in x['lods'])) + [m.h[0]]
    for x in m.models:
        dl = x['lods'][0][3]
        nxt = min([s for s in starts if s > dl] or [len(d)])
        print('  %-14s radius %8.1f  lod0 tex %04X flags %04X dl %06X (~%d bytes to next dl)' % (
            x['name'], x['radius'], x['lods'][0][0], x['lods'][0][1], dl, nxt - dl))
    for t in m.textures:
        print('  tex %-16s %dx%d fmt %08X' % (t['name'], t['w'], t['h'], t['fmt']))


if __name__ == '__main__':
    cmd = sys.argv[1] if len(sys.argv) > 1 else 'tables'
    if cmd == 'tables': tables()
    elif cmd == 'widgets': widgets()
    elif cmd == 'refs': refs([int(x, 16) for x in sys.argv[2:]])
    elif cmd == 'logos': logos(sys.argv[2])
    elif cmd == 'diorama': diorama()
    else: print(__doc__)
