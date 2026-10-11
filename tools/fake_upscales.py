"""Test aid: fills asset_dump/user_texture_pack with stand-in "upscales" of every dumped texture (each image scaled up
by nearest neighbor, optionally tinted so replaced textures stand out in screenshots), to test Install Upscaled
Textures and the derived replacements (docs/texture_upscaling.md) without a real upscaler.

Usage: python tools/fake_upscales.py ASSET_DUMP_DIR [--scale 2] [--tint] [--game rush2 ...] [--flat]
--flat writes each game's images into one folder (as Gigapixel does), else the dump's folders are kept.
"""
import argparse, os
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('dump')
ap.add_argument('--scale', type=int, default=2)
ap.add_argument('--tint', action='store_true', help='shift colors towards magenta')
ap.add_argument('--game', action='append')
ap.add_argument('--flat', action='store_true')
args = ap.parse_args()

count = 0
for game in args.game or ['rush2', 'sfrush', 'rush2049']:
    src = os.path.join(args.dump, 'textures', game)
    dst = os.path.join(args.dump, 'user_texture_pack', game)
    for d, _, files in os.walk(src):
        for f in files:
            if not f.endswith('.png'):
                continue
            im = Image.open(os.path.join(d, f)).convert('RGBA')
            im = im.resize((im.width * args.scale, im.height * args.scale), Image.NEAREST)
            if args.tint:
                r, g, b, a = im.split()
                r = r.point(lambda v: min(255, v + 60))
                b = b.point(lambda v: min(255, v + 60))
                im = Image.merge('RGBA', (r, g, b, a))
            rel = os.path.relpath(os.path.join(d, f), src)
            out = os.path.join(dst, os.path.basename(rel) if args.flat else rel)
            os.makedirs(os.path.dirname(out), exist_ok=True)
            im.save(out[:-4] + '-x%d.png' % args.scale)
            count += 1
print('wrote', count, 'stand-in upscales')
