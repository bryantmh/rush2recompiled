"""Builds a labeled contact sheet PNG from screenshots: one row per --row, each image with its caption (test and mod
packaging aid, e.g. a car mod's cars and liveries from car select captures).

    python tools/contact_sheet.py OUT.png [--title TEXT] [--width 480] [--crop L,T,R,B] [--no-captions]
        --row "ROW LABEL" IMG[=CAPTION] IMG[=CAPTION] ... [--row ...]

--width is each image's width in the sheet (height keeps the aspect; text and spacing scale with it). --crop takes fractions of each image
(0.1,0.1,0.9,0.8) to keep before scaling; the default is the whole image. --no-captions leaves out the text line
under each image. An OUT ending in .jpg is saved as JPEG at quality 85 (far smaller than PNG for screenshots).
"""
import argparse, os, sys
from PIL import Image, ImageDraw, ImageFont


def font(size):
    for name in ('arialbd.ttf', 'arial.ttf', 'DejaVuSans-Bold.ttf', 'DejaVuSans.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def parse_rows(argv):
    rows, cur = [], None
    rest = []
    i = 0
    while i < len(argv):
        if argv[i] == '--row':
            cur = (argv[i + 1], [])
            rows.append(cur)
            i += 2
            continue
        if cur is None:
            rest.append(argv[i])
        else:
            path, _, cap = argv[i].partition('=')
            cur[1].append((path, cap or os.path.splitext(os.path.basename(path))[0]))
        i += 1
    return rest, rows


def main():
    rest, rows = parse_rows(sys.argv[1:])
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('out')
    ap.add_argument('--title', default='')
    ap.add_argument('--width', type=int, default=480)
    ap.add_argument('--crop', default='')
    ap.add_argument('--no-captions', action='store_true')
    a = ap.parse_args(rest)
    if not rows:
        sys.exit('no --row given')
    crop = [float(x) for x in a.crop.split(',')] if a.crop else None
    tiles = []
    for label, items in rows:
        row = []
        for path, cap in items:
            im = Image.open(path).convert('RGB')
            if crop:
                w, h = im.size
                im = im.crop((int(crop[0] * w), int(crop[1] * h), int(crop[2] * w), int(crop[3] * h)))
            im = im.resize((a.width, round(im.height * a.width / im.width)), Image.LANCZOS)
            row.append((im, cap))
        tiles.append((label, row))
    k = a.width / 480  # text and spacing scale with the image width
    pad, cap_h, label_h = round(12 * k), 0 if a.no_captions else round(30 * k), round(44 * k)
    tile_h = max(im.height for _, r in tiles for im, _ in r)
    cols = max(len(r) for _, r in tiles)
    title_h = round(64 * k) if a.title else 0
    W = pad + cols * (a.width + pad)
    H = title_h + len(tiles) * (label_h + tile_h + cap_h + pad) + pad
    sheet = Image.new('RGB', (W, H), (16, 16, 20))
    d = ImageDraw.Draw(sheet)
    if a.title:
        d.text((pad, round(14 * k)), a.title, font=font(round(36 * k)), fill=(255, 255, 255))
    y = title_h
    for label, row in tiles:
        d.text((pad, y + round(8 * k)), label, font=font(round(30 * k)), fill=(255, 210, 60))
        y += label_h
        for i, (im, cap) in enumerate(row):
            x = pad + i * (a.width + pad)
            sheet.paste(im, (x, y))
            if not a.no_captions:
                d.text((x + 2, y + tile_h + round(4 * k)), cap, font=font(round(20 * k)), fill=(220, 220, 220))
        y += tile_h + cap_h + pad
    sheet.save(a.out, quality=85, subsampling=0) if a.out.lower().endswith(('.jpg', '.jpeg')) else sheet.save(a.out)
    print('%s: %dx%d, %d rows' % (a.out, W, H, len(tiles)))


if __name__ == '__main__':
    main()
