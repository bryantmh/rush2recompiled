#!/usr/bin/env python3
"""California Speed's billboard trees outside the game: runs src/calspeed/calspeed_billboards.cpp on tracks' files
(tools/calspeed/billboards_test.cpp, built by billboards_test.bat), checks the lists it writes, and draws contact
sheets of every card it looked at, so what it turns to the camera (and what it leaves flat) can be checked by eye.

    python tools/calspeed/billboards.py [TRACK ...] [--out DIR] [--review]

Writes to OUT (default tmp/billboards/): chosen.png (every card turned to the camera, by track), flat_<reason>.png
(the cards left flat, by reason), t<N>/ (each track's files and cards.tsv). --review takes every card that passes the
shape tests whatever its texture and draws textures.png instead: one card per (track, texture), with how many there are,
to pick each track's tree textures (calspeed_billboards.cpp `trees`). Tracks are California Speed's 0-15
(default all). docs/calspeed_research.md#billboard-trees.
"""
import argparse, os, subprocess, sys
from collections import defaultdict

from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'trackview'))
if 'CALSPEED_ROM' not in os.environ and os.path.exists(os.path.join(REPO, 'build', 'calspeed.z64')):
    os.environ['CALSPEED_ROM'] = os.path.join(REPO, 'build', 'calspeed.z64')
from cs import CalSpeed  # noqa: E402
import stock  # noqa: E402

EXE = os.path.join(REPO, 'tmp', 'billboards_test', 'billboards_test.exe')
BAT = os.path.join(REPO, 'tools', 'calspeed', 'billboards_test.bat')


def build():
    srcs = [os.path.join(REPO, p) for p in ('tools/calspeed/billboards_test.cpp', 'src/calspeed/calspeed_billboards.cpp',
                                            'include/calspeed_billboards.h')]
    if os.path.exists(EXE) and all(os.path.getmtime(s) < os.path.getmtime(EXE) for s in srcs):
        return
    r = subprocess.run(['cmd', '/c', BAT], cwd=REPO, capture_output=True, text=True, errors='replace')
    if r.returncode != 0 or not os.path.exists(EXE):
        sys.exit('build failed:\n' + r.stdout[-3000:] + r.stderr[-3000:])


def read_cards(work):
    rows = []
    with open(os.path.join(work, 'cards.tsv')) as f:
        head = f.readline().rstrip('\n').split('\t')
        for line in f:
            rows.append(dict(zip(head, line.rstrip('\n').split('\t'))))
    data = open(os.path.join(work, 'crops.bin'), 'rb').read()
    at = 0
    for r in rows:
        w, h = int(r['crop_w']), int(r['crop_h'])
        img = Image.frombytes('RGBA', (w, h), data[at:at + w * h * 4])
        at += w * h * 4
        bg = Image.new('RGBA', (w, h), (255, 0, 255, 255))
        bg.alpha_composite(img)
        r['img'] = bg.convert('RGB')
    return rows


def sheet(groups, path, width=1800):
    """groups: [(title, [(label, image)])] -> one image, a labelled block per group."""
    pad, lab = 4, 11
    blocks = []
    for title, items in groups:
        rows, row, x, rh = [], [], 0, 0
        for label, img in items:
            w = max(img.width, 6 * len(label))
            if x + w + pad > width and row:
                rows.append((row, rh))
                row, x, rh = [], 0, 0
            row.append((x, label, img))
            x += w + pad
            rh = max(rh, img.height + lab)
        if row:
            rows.append((row, rh))
        blocks.append((title, rows))
    total = sum(16 + sum(rh + pad for _, rh in rows) for _, rows in blocks)
    out = Image.new('RGB', (width, max(total, 20)), 'white')
    d = ImageDraw.Draw(out)
    y = 0
    for title, rows in blocks:
        d.text((2, y + 2), title, fill='black')
        y += 16
        for row, rh in rows:
            for x, label, img in row:
                out.paste(img, (x, y + lab))
                d.text((x, y), label, fill=(60, 60, 60))
            y += rh + pad
    out.save(path)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tracks', nargs='*', type=int)
    ap.add_argument('--out', default=os.path.join(REPO, 'tmp', 'billboards'))
    ap.add_argument('--review', action='store_true')
    a = ap.parse_args()
    build()
    c = CalSpeed()
    os.makedirs(a.out, exist_ok=True)
    chosen, flat = [], defaultdict(list)
    failed = False
    for t in a.tracks or range(16):
        work = os.path.join(a.out, 't%d' % t)
        os.makedirs(work, exist_ok=True)
        for k, v in (('g', c.asset(62 + t)), ('p', c.asset(78 + t))):
            with open(os.path.join(work, k + '.bin'), 'wb') as f:
                f.write(v)
        r = subprocess.run([EXE, str(t), os.path.join(work, 'g.bin'), os.path.join(work, 'p.bin'), work] +
                           (['--any-texture'] if a.review else []),
                           capture_output=True, text=True, errors='replace')
        print('--- track %d (%s)' % (t, stock.CS_NAMES[t]))
        print(r.stdout.strip())
        if r.returncode != 0:
            failed = True
        rows = read_cards(work)
        name = stock.CS_NAMES[t]
        if a.review:
            seen = {}
            for x in rows:
                if x['chosen'] == '1':
                    seen.setdefault(x['texture'], []).append(x)
            chosen.append(('%d %s' % (t, name), [('%s x%d %.0fft' % (tex, len(xs), float(xs[0]['height'])), xs[0]['img'])
                                                  for tex, xs in sorted(seen.items())]))
            continue
        chosen.append(('%d %s: %d' % (t, name, sum(1 for x in rows if x['chosen'] == '1')),
                       [('%s %.0f' % (x['texture'][-5:], float(x['height'])), x['img']) for x in rows if x['chosen'] == '1']))
        for x in rows:
            if x['chosen'] != '1':
                flat[x['why']].append(('%d %.0fx%.0f g%.2f c%.2f' % (t, float(x['width']), float(x['height']),
                                                                    float(x['green']), float(x['clear'])), x['img']))
    if a.review:
        sheet(chosen, os.path.join(a.out, 'textures.png'))
        print('sheet in', a.out)
        sys.exit(1 if failed else 0)
    sheet(chosen, os.path.join(a.out, 'chosen.png'))
    for why, items in flat.items():
        sheet([('%s: %d' % (why, len(items)), items)], os.path.join(a.out, 'flat_%s.png' % why.replace(' ', '_')))
    print('sheets in', a.out)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
