#!/usr/bin/env python3
"""Crops each split-screen view's own car out of a 2-4 player shot (tools/test_race.py --cars a,b,c,d --shots with
RUSH2_TEST_LOOK_BACK=1: the look-back camera puts the car in the middle of its view, a little below center), for the Car
Lights review: one image per view, or all side by side with --sheet.

    python tools/quad_crops.py SHOT.png OUT_PREFIX [--labels a,b,c,d] [--box 0.28,0.30,0.72,0.88] [--sheet OUT.png]

The box is the part of a view kept, as fractions of the view (x0, y0, x1, y1).
"""
import argparse
from PIL import Image, ImageDraw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('shot')
    ap.add_argument('prefix')
    ap.add_argument('--labels', default='')
    ap.add_argument('--box', default='0.28,0.30,0.72,0.88')
    ap.add_argument('--sheet')
    ap.add_argument('--views', type=int, default=4)
    a = ap.parse_args()
    im = Image.open(a.shot).convert('RGB')
    W, H = im.size
    vw, vh = (W, H // 2) if a.views == 2 else (W // 2, H // 2)
    f = [float(v) for v in a.box.split(',')]
    labels = a.labels.split(',') if a.labels else []
    crops = []
    for v in range(a.views):
        ox, oy = ((0, v * vh) if a.views == 2 else ((v % 2) * vw, (v // 2) * vh))
        c = im.crop((ox + int(f[0] * vw), oy + int(f[1] * vh), ox + int(f[2] * vw), oy + int(f[3] * vh)))
        name = labels[v] if v < len(labels) else str(v)
        c.save('%s_%s.png' % (a.prefix, name.replace(':', '_').replace('/', '_')))
        crops.append((name, c))
    if a.sheet:
        w = sum(c.width for _, c in crops)
        s = Image.new('RGB', (w, max(c.height for _, c in crops)))
        x = 0
        for name, c in crops:
            s.paste(c, (x, 0))
            ImageDraw.Draw(s).text((x + 6, 6), name, fill=(255, 255, 0))
            x += c.width
        s.save(a.sheet)


if __name__ == '__main__':
    main()
