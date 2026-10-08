"""Tiles screenshots into one PNG for a quick look: python montage.py OUT.png COLUMNS WIDTH image1 image2 ... (each scaled
to WIDTH pixels wide, in reading order). Test aid; used with tools/rush1/shots.py."""
import sys
from PIL import Image
out, cols, width = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
ims = []
for p in sys.argv[4:]:
    im = Image.open(p).convert('RGB'); ims.append(im.resize((width, im.height * width // im.width)))
h = max(i.height for i in ims); rows = (len(ims) + cols - 1) // cols
sheet = Image.new('RGB', (cols * width, rows * h))
for n, im in enumerate(ims): sheet.paste(im, ((n % cols) * width, (n // cols) * h))
sheet.save(out); print(out, sheet.size)
