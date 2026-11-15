#!/usr/bin/env python3
"""Car Lights review aid: a player's screenshot of a car beside the car's gridded view from a lamp dump
(tools/car_lamps.py sheet), one row per pair, so a lamp entry can be checked against what the player saw.

    python tools/lamp_compare.py OUT.png DUMP_DIR SHOT.png:KEY:VIEW[:x0,y0,x1,y1] ... [--size 360] [--table T]

VIEW is front or back; the optional box crops the screenshot (pixels) to the car.
"""
import argparse, os, subprocess, sys
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('dump')
    ap.add_argument('pairs', nargs='+')
    ap.add_argument('--size', type=int, default=360)
    ap.add_argument('--table', default=os.path.join(os.path.dirname(HERE), 'assets', 'car_lamps.json'))
    a = ap.parse_args()
    S = a.size
    rows = []
    for p in a.pairs:
        parts = p.split('|')
        shot, key, view = parts[0], parts[1], parts[2]
        box = tuple(int(v) for v in parts[3].split(',')) if len(parts) > 3 else None
        tmp = a.out + '.part.png'
        r = subprocess.run([sys.executable, os.path.join(HERE, 'car_lamps.py'), 'sheet', a.dump, tmp, '--only', key,
                            '--views', view, '--size', str(S), '--grid', '--table', a.table], capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(tmp):
            sys.exit('sheet failed for %s: %s' % (key, r.stderr))
        sheet = Image.open(tmp).convert('RGB').crop((230, 0, 230 + S, S))
        os.remove(tmp)
        im = Image.open(shot).convert('RGB')
        if box:
            im = im.crop(box)
        im = im.resize((int(S * im.width / im.height), S))
        row = Image.new('RGB', (im.width + S, S))
        row.paste(im, (0, 0))
        row.paste(sheet, (im.width, 0))
        ImageDraw.Draw(row).text((4, 4), '%s  %s' % (os.path.basename(shot), key), fill=(255, 255, 0))
        rows.append(row)
    W = max(r.width for r in rows)
    out = Image.new('RGB', (W, S * len(rows)))
    for i, r in enumerate(rows):
        out.paste(r, (0, i * S))
    out.save(a.out)
    print('wrote', a.out)


if __name__ == '__main__':
    main()
