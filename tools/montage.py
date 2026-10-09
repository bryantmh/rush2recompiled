"""Tiles screenshots into one labeled contact sheet (test aid), so a run's captures can be checked in one image.

Usage: python tools/montage.py OUT.png [--width W] [--cols N] IMAGE...   (each tile is W pixels wide, default 480)
"""
import os, sys
from PIL import Image, ImageDraw

args = sys.argv[1:]
out = args.pop(0)
width, cols = 480, 3
while args and args[0].startswith('--'):
    flag = args.pop(0)
    if flag == '--width':
        width = int(args.pop(0))
    elif flag == '--cols':
        cols = int(args.pop(0))
images = [Image.open(p).convert('RGB') for p in args]
tiles = [im.resize((width, int(im.height * width / im.width))) for im in images]
th = max(t.height for t in tiles)
rows = (len(tiles) + cols - 1) // cols
sheet = Image.new('RGB', (cols * width, rows * (th + 16)), (40, 40, 40))
draw = ImageDraw.Draw(sheet)
for i, (t, p) in enumerate(zip(tiles, args)):
    x, y = (i % cols) * width, (i // cols) * (th + 16)
    sheet.paste(t, (x, y + 16))
    draw.text((x + 4, y + 2), os.path.basename(p), fill=(255, 255, 0))
sheet.save(out)
