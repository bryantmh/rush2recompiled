"""Writes a Rush 2 car panel as the derived replacements see it, to test recoloring a paint base's upscale
(rush2::upscale::recolor / recolor_paint, docs/texture_upscaling.md) on real data with tools/texture_upscale_test.cpp:

    python tools/rush1/paint_case.py CAR TEXTURE OUT_DIR [MAIN r,g,b] [ACCENT r,g,b]
    tmp/upscale_test/upscale_test.exe --paint OUT_DIR UPSCALE.png

e.g. `paint_case.py BMW BMD0_1 tmp/paint 248,40,40 255,255,255`. Writes OUT_DIR/gray.png (the catalog's image: entry i
as the 5-bit gray i & 31, what Dump Textures wrote before it wrote previews), preview.png (what it writes now:
rush2::catalog::paint_preview, MAIN violet and ACCENT yellow), painted.png (CARPALETTE with MAIN and ACCENT painted: each ramp
entry the color times CARPALETTE's gray over 230, an approximation of car_build_textures_8008582C) and indices.bin (the
palette indices, one byte per texel). Needs the Rush 2 ROM (RUSH2_ROM, tools/rush2049/roms.py).
"""
import os, sys
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cartex

car, name, out = sys.argv[1], sys.argv[2], sys.argv[3]
main = tuple(int(v) for v in (sys.argv[4] if len(sys.argv) > 4 else '248,40,40').split(','))
accent = tuple(int(v) for v in (sys.argv[5] if len(sys.argv) > 5 else '255,255,255').split(','))
d = cartex.load(2, car)
_, textures, _ = cartex.parse(d, 2)
t = next(t for t in textures if t['name'] == name)
w, h = t['w'], t['h']
assert t['siz'] == 2, 'not a CI8 texture'
indices = d[t['data']:t['data'] + w * h]
pal = cartex.carpalette()
os.makedirs(out, exist_ok=True)
gray, painted, preview = Image.new('RGB', (w, h)), Image.new('RGB', (w, h)), Image.new('RGB', (w, h))
for k, i in enumerate(indices):
    g = ((i & 31) << 3) | ((i & 31) >> 2)
    gray.putpixel((k % w, k // w), (g, g, g))
    if 1 <= i <= 31 or 33 <= i <= 63:
        paint, level = (main if i < 32 else accent), pal[i][0] / 230
        c = tuple(min(255, int(v * level)) for v in paint)
    else:
        c = pal[i][:3]
    painted.putpixel((k % w, k // w), c)
    if 1 <= i <= 31 or 33 <= i <= 63:
        paint = (160, 80, 255) if i < 32 else (255, 215, 40)
        c = tuple(min(255, v * pal[i][0] // 230) for v in paint)
    preview.putpixel((k % w, k // w), c)
gray.save(os.path.join(out, 'gray.png'))
painted.save(os.path.join(out, 'painted.png'))
preview.save(os.path.join(out, 'preview.png'))
open(os.path.join(out, 'indices.bin'), 'wb').write(bytes(indices))
print(f'{car} {name} {w}x{h} -> {out}')
