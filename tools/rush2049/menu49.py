"""Rush 2049 front-end art (file 54): the menu button models, their texture load lists and an offline render of the
button flip (research aid for the Rush 2049 Menus option, src/rush2049/menus2049.cpp; docs/rush2049_research/menus.md
section 11).

Usage:  python menu49.py models                 every model of file 54: bounds, vertex and triangle counts
        python menu49.py dl NAME                 a model's display list, decoded
        python menu49.py txld NAME               a texture's load list (TXHD +0x18), decoded
        python menu49.py flip OUT.png [MODEL]    renders MODEL (default BUTTON_S1G1) at flip angles 0..pi as 2049 draws a
                                                 menu button: rotX(angle - pi/2) at (0, 0, 300), 90 degree horizontal field
                                                 of view at 320x240, BUTTON below pi/2 and BUTTON_SELECT above

Model display lists are F3DEX2 with file-offset addresses (2049 relocates them at load): G_VTX (01) loads n vertices
of 16 bytes {s16 x, y, z, u16 flag, s16 s, t, u8 r, g, b, a} into the cache at v0, G_TRI1 (05) / G_TRI2 (06) draw
cache indices x 2.
"""
import math, os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import roms, model

FILE = 54


def load():
    q = roms.Rush2049()
    d = q.file(FILE)
    return d, model.M49(d)


def u32(d, o):
    return struct.unpack_from('>I', d, o)[0]


def walk(d, start):
    """Commands of a display list from start to G_ENDDL: [(offset, w0, w1)]."""
    out, o = [], start
    while o + 8 <= len(d):
        w0, w1 = u32(d, o), u32(d, o + 4)
        out.append((o, w0, w1))
        if w0 >> 24 == 0xDF:
            break
        o += 8
    return out


def triangles(d, start):
    """The triangles a model list draws: [((x, y, z, s, t, rgba) x 3)] in model units (1/16) and 10.5 texels."""
    cache = [None] * 64
    tris = []
    for o, w0, w1 in walk(d, start):
        op = w0 >> 24
        if op == 0x01:
            n = (w0 >> 12) & 0xFF
            end = (w0 >> 1) & 0x7F
            addr = w1 & 0xFFFFFF
            for i in range(n):
                x, y, z, fl, s, t = struct.unpack_from('>hhhHhh', d, addr + i * 16)
                cache[end - n + i] = (x, y, z, s, t, u32(d, addr + i * 16 + 12))
        elif op == 0x05:
            tris.append(tuple(cache[((w0 >> k) & 0xFF) // 2] for k in (16, 8, 0)))
        elif op == 0x06:
            tris.append(tuple(cache[((w0 >> k) & 0xFF) // 2] for k in (16, 8, 0)))
            tris.append(tuple(cache[((w1 >> k) & 0xFF) // 2] for k in (16, 8, 0)))
    return tris


def find_object(m, name):
    for o in m.objects:
        if o['name'] == name:
            return o
    sys.exit('no model ' + name)


def model_list(o):
    # OBHD LOD record: u16, u16, f32, u32 display list, u32 vertices (file offsets).
    return o['lods'][0][3]


def find_texture(m, name):
    for i, t in enumerate(m.textures):
        if t['name'] == name:
            return i, t
    sys.exit('no texture ' + name)


def cmd_models(d, m):
    for o in m.objects:
        tris = triangles(d, model_list(o))
        pts = [v for t in tris for v in t]
        if not pts:
            print('%-16s no triangles' % o['name'])
            continue
        lo = [min(p[k] for p in pts) / 16 for k in range(3)]
        hi = [max(p[k] for p in pts) / 16 for k in range(3)]
        print('%-16s %3d tris  x %7.2f..%7.2f  y %6.2f..%6.2f  z %6.2f..%6.2f' %
              (o['name'], len(tris), lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))


def cmd_dl(d, m, name):
    for o, w0, w1 in walk(d, model_list(find_object(m, name))):
        print('%06X  %08X %08X' % (o, w0, w1))


def cmd_txld(d, m, name):
    i, t = find_texture(m, name)
    rec = m.c['TXHD'][0] + i * model.M49.TXHD
    print(name, t, 'record', hex(rec))
    start = u32(d, rec + 0x18)
    for o, w0, w1 in walk(d, start):
        print('%06X  %08X %08X' % (o, w0, w1))


FILE_BASE = 0x80400000      # where texload puts the container (relocated as 2049's loader does)
DL_BASE = 0x80600000        # the display list texload's loader call writes
CURSOR = 0x805FFFF0         # the cursor word ($s4 points at it)
STOP = 0x80000180           # return address: the run ends when it gets there


def texload(name, rom=None):
    """Runs Rush 2049's texture loader func_80099BFC (the one its polygon draw calls when the texture changes) on
    file 54's record `name` in an emulated N64 and returns the commands it writes: [(w0, w1)]. Inputs, from its call
    in func_8009F058 (0x8009FF04-0x8009FF60): $s1 = the TXHD record, $s4 = &display list cursor, $s5 = the
    container's palette (PLHD) table, $s0 = $s3 = 0. The container is relocated as loading does: TXHD +0x18 and
    PLHD +0x14 become RDRAM addresses (IMAG-relative in the file)."""
    from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_BIG_ENDIAN
    from unicorn.mips_const import (UC_MIPS_REG_PC, UC_MIPS_REG_RA, UC_MIPS_REG_SP, UC_MIPS_REG_S0, UC_MIPS_REG_S1,
                                    UC_MIPS_REG_S3, UC_MIPS_REG_S4, UC_MIPS_REG_S5)
    q = rom or roms.Rush2049()
    d = bytearray(q.file(FILE))
    m = model.M49(bytes(d))
    imag = m.imag
    txhd, ntx = m.c['TXHD']
    plhd, npl = m.c['PLHD']
    for i in range(ntx):
        r = txhd + i * model.M49.TXHD
        struct.pack_into('>I', d, r + 0x18, FILE_BASE + imag + u32(d, r + 0x18))
    for i in range(npl):
        r = plhd + i * model.M49.PLHD
        struct.pack_into('>I', d, r + 0x14, FILE_BASE + imag + u32(d, r + 0x14))
    index, _ = find_texture(m, name)
    uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_BIG_ENDIAN)
    # kseg0 is unmapped: unicorn takes 0x80000000-0x9FFFFFFF to physical 0.
    uc.mem_map(0, 0x800000)
    boot = q.rom[0x1000:0x1000 + (q.MAIN_VRAM - 0x80000400)]
    uc.mem_write(0x400, boot)
    uc.mem_write(q.MAIN_VRAM & 0x7FFFFF, q.main)
    uc.mem_write(FILE_BASE & 0x7FFFFF, bytes(d))
    uc.mem_write(CURSOR & 0x7FFFFF, struct.pack('>I', DL_BASE))
    uc.reg_write(UC_MIPS_REG_S1, FILE_BASE + txhd + index * model.M49.TXHD)
    uc.reg_write(UC_MIPS_REG_S4, CURSOR)
    uc.reg_write(UC_MIPS_REG_S5, FILE_BASE + plhd)
    uc.reg_write(UC_MIPS_REG_S0, 0)
    uc.reg_write(UC_MIPS_REG_S3, 0)
    uc.reg_write(UC_MIPS_REG_SP, 0x807F0000)
    uc.reg_write(UC_MIPS_REG_RA, STOP)
    uc.emu_start(0x80099BFC, STOP, count=2000000)
    end = struct.unpack('>I', uc.mem_read(CURSOR & 0x7FFFFF, 4))[0]
    raw = bytes(uc.mem_read(DL_BASE & 0x7FFFFF, end - DL_BASE))
    return [struct.unpack_from('>II', raw, o) for o in range(0, len(raw), 8)], FILE_BASE, imag


DL2D_CURSOR = 0x80149438    # 2049's 2D display list cursor (the widget draw writes there)


def widget_draw(name, x=0, y=0, rows=None, rom=None):
    """Runs Rush 2049's 2D widget image draw on file 54's texture `name` at (x, y), as func_800A04C4's widget loop
    does for a textured widget with flags +0x15 = 0 and alpha 0xFF (the menu background's records 0x801141D4):
    func_8008705C(0x20); func_8008A148(palette, fmt, siz, 0); then per strip func_80087A08 (load rows, bottom up:
    2049's images are stored flipped) and func_80087110 (the rectangle). Returns the commands written: [(w0, w1)]."""
    from unicorn import Uc, UC_ARCH_MIPS, UC_MODE_MIPS32, UC_MODE_BIG_ENDIAN
    from unicorn.mips_const import (UC_MIPS_REG_RA, UC_MIPS_REG_SP, UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2,
                                    UC_MIPS_REG_A3)
    q = rom or roms.Rush2049()
    d = bytearray(q.file(FILE))
    m = model.M49(bytes(d))
    imag = m.imag
    index, t = find_texture(m, name)
    rec = m.c['TXHD'][0] + index * model.M49.TXHD
    texels = FILE_BASE + imag + u32(d, rec + 0x18)
    pal_index = t['idx'] & 0xFFFF
    prec = m.c['PLHD'][0] + pal_index * model.M49.PLHD
    palette = FILE_BASE + imag + u32(d, prec + 0x14)
    fmt, siz = d[rec + 0x15], d[rec + 0x14]
    w, h = t['w'], t['h']
    uc = Uc(UC_ARCH_MIPS, UC_MODE_MIPS32 + UC_MODE_BIG_ENDIAN)
    uc.mem_map(0, 0x800000)
    uc.mem_write(0x400, q.rom[0x1000:0x1000 + (q.MAIN_VRAM - 0x80000400)])
    uc.mem_write(q.MAIN_VRAM & 0x7FFFFF, q.main)
    uc.mem_write(FILE_BASE & 0x7FFFFF, bytes(d))
    uc.mem_write(DL2D_CURSOR & 0x7FFFFF, struct.pack('>I', DL_BASE))

    def call(func, *args):
        regs = (UC_MIPS_REG_A0, UC_MIPS_REG_A1, UC_MIPS_REG_A2, UC_MIPS_REG_A3)
        sp = 0x807F0000
        for i, v in enumerate(args):
            if i < 4:
                uc.reg_write(regs[i], v & 0xFFFFFFFF)
            else:
                uc.mem_write((sp + 0x10 + (i - 4) * 4) & 0x7FFFFF, struct.pack('>I', v & 0xFFFFFFFF))
        uc.reg_write(UC_MIPS_REG_SP, sp)
        uc.reg_write(UC_MIPS_REG_RA, STOP)
        uc.emu_start(func, STOP, count=2000000)

    # Rows a load holds: the table 0x8011ED0C by width (as the widget draw reads it), halved for formats 2 and 5.
    per = struct.unpack_from('>H', q.main, 0x8011ED0C - q.MAIN_VRAM + ((w >> (4 - siz)) * 2))[0]
    if fmt in (2, 5):
        per >>= 1
    rows = rows or h
    # The widget loop for a widget with flags +0x15 = 0 and alpha 0xFF: flag 8 on (rows drawn bottom up), 0x20 off,
    # the clip rect over the image.
    call(0x800878E0, 8)
    call(0x8008705C, 0x20)
    call(0x8008A3E4, x, y, x + w - 1, y + rows - 1)
    call(0x8008A148, palette, fmt, siz, 0)
    r = 0
    while r < rows:
        n = min(per, rows - r)
        call(0x80087A08, texels, fmt, siz, w, h, 0, h - r - n, w - 1, h - r - 1, 0, 0)
        call(0x80087110, x, y + r, x + w - 1, y + r + n - 1, 0, h - r - n)
        r += n
    end = struct.unpack('>I', uc.mem_read(DL2D_CURSOR & 0x7FFFFF, 4))[0]
    raw = bytes(uc.mem_read(DL_BASE & 0x7FFFFF, end - DL_BASE))
    return [struct.unpack_from('>II', raw, o) for o in range(0, len(raw), 8)], per


def cmd_widget(name, rows):
    cmds, per = widget_draw(name, rows=rows)
    print('rows a load:', per)
    for w0, w1 in cmds:
        print('%08X %08X' % (w0, w1))


def cmd_texload(name):
    cmds, base, imag = texload(name)
    for w0, w1 in cmds:
        note = ''
        if w0 >> 24 in (0xFD,) and base <= w1 < base + 0x100000:
            note = '  file +%#x (IMAG +%#x)' % (w1 - base, w1 - base - imag)
        print('%08X %08X%s' % (w0, w1, note))


def cmd_flip(d, m, out, name):
    from PIL import Image, ImageDraw
    import ui49
    tris = triangles(d, model_list(find_object(m, name)))
    images = {}
    for tex in ('BUTTON', 'BUTTON_SELECT'):
        i, t = find_texture(m, tex)
        kind, rgba = ui49.decode(m, t)
        images[tex] = (t['w'], t['h'], rgba)
    tan_h = 1.0                      # 90 degree horizontal field of view
    tan_v = tan_h * 240 / 320
    frames = 9
    sheet = Image.new('RGBA', (320, 60 * frames), (20, 20, 60, 255))
    for f in range(frames):
        angle = math.pi * f / (frames - 1)
        rot = angle - math.pi / 2
        c, s = math.cos(rot), math.sin(rot)
        w, h, rgba = images['BUTTON_SELECT' if angle > math.pi / 2 else 'BUTTON']
        oy = 60 * f + 30
        # Painter's order by depth; point-sampled texture, shade = vertex color.
        projected = []
        for tri in tris:
            pts = []
            for (x, y, z, ts, tt, col) in tri:
                X, Y, Z = x / 16, y / 16, z / 16
                Y, Z = Y * c - Z * s, Y * s + Z * c       # rotX
                Z += 300
                pts.append((160 + X / Z / tan_h * 160, oy - Y / Z / tan_v * 120, Z, ts / 32, tt / 32))
            projected.append(pts)
        projected.sort(key=lambda p: -sum(q[2] for q in p))
        layer = Image.new('RGBA', sheet.size)
        px = layer.load()
        for pts in projected:
            (x0, y0, _, s0, t0), (x1, y1, _, s1, t1), (x2, y2, _, s2, t2) = pts
            den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2)
            if abs(den) < 1e-9:
                continue
            for yy in range(int(min(y0, y1, y2)), int(max(y0, y1, y2)) + 1):
                for xx in range(int(min(x0, x1, x2)), int(max(x0, x1, x2)) + 1):
                    a = ((y1 - y2) * (xx + .5 - x2) + (x2 - x1) * (yy + .5 - y2)) / den
                    b = ((y2 - y0) * (xx + .5 - x2) + (x0 - x2) * (yy + .5 - y2)) / den
                    g = 1 - a - b
                    if a < 0 or b < 0 or g < 0 or not (0 <= xx < 320 and 0 <= yy < sheet.size[1]):
                        continue
                    u = int(a * s0 + b * s1 + g * s2) % w
                    v = int(a * t0 + b * t1 + g * t2) % h
                    k = (v * w + u) * 4
                    px[xx, yy] = tuple(rgba[k:k + 4])
        sheet.alpha_composite(layer)
    sheet.resize((640, sheet.size[1] * 2), Image.NEAREST).save(out)


def main():
    a = sys.argv[1:]
    if not a:
        print(__doc__)
        sys.exit(1)
    d, m = load()
    if a[0] == 'models':
        cmd_models(d, m)
    elif a[0] == 'dl':
        cmd_dl(d, m, a[1])
    elif a[0] == 'txld':
        cmd_txld(d, m, a[1])
    elif a[0] == 'texload':
        cmd_texload(a[1])
    elif a[0] == 'widget':
        cmd_widget(a[1], int(a[2]) if len(a) > 2 else None)
    elif a[0] == 'flip':
        cmd_flip(d, m, a[1], a[2] if len(a) > 2 else 'BUTTON_S1G1')
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == '__main__':
    main()
