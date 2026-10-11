"""Writes a Rush 2 car panel as the derived replacements see it, to test recoloring a paint base's upscale
(rush2::upscale::recolor and clamp_chroma, docs/texture_upscaling.md) on real data with tools/texture_upscale_test.cpp:

    python tools/rush1/paint_case.py CAR TEXTURE OUT_DIR [MAIN r,g,b] [ACCENT r,g,b]
    tmp/upscale_test/upscale_test.exe --paint OUT_DIR UPSCALE.png

e.g. `paint_case.py BMW BMD0_1 tmp/paint 248,40,40 255,255,255`. Writes OUT_DIR/preview.png (what Dump Textures writes:
rush2::catalog::paint_preview, MAIN violet and ACCENT yellow) and painted.png (CARPALETTE with MAIN and ACCENT painted:
each ramp entry the color times CARPALETTE's gray over 230, as car_build_textures_8008582C shades a paint). Needs the
Rush 2 ROM (RUSH2_ROM, tools/rush2049/roms.py).
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
painted, preview = Image.new('RGB', (w, h)), Image.new('RGB', (w, h))
for k, i in enumerate(indices):
    ramp = 1 <= i <= 31 or 33 <= i <= 63
    for image, paints in ((painted, (main, accent)), (preview, ((160, 80, 255), (255, 215, 40)))):
        if ramp:
            c = tuple(min(255, v * pal[i][0] // 230) for v in paints[0 if i < 32 else 1])
        else:
            c = pal[i][:3]
        image.putpixel((k % w, k // w), c)
painted.save(os.path.join(out, 'painted.png'))
preview.save(os.path.join(out, 'preview.png'))
print(f'{car} {name} {w}x{h} -> {out}')
