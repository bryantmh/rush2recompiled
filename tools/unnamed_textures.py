# Classifies a texture dump's unnamed images (asset_dump/textures/<game>/unnamed/*.png) against its named ones: the
# same pixels as a named image, a crop of one (any sub-rectangle, also of its flips; "top rows" when only the rows above
# a garbled tail match), the same crop ignoring alpha, or none (made at run time). Prints counts and examples per class;
# --sheet writes a contact sheet per class, --list writes "<unnamed file> <class> <match>" lines.
# Usage: python tools/unnamed_textures.py [ASSET_DUMP_DIR] [--game rush2] [--sheet OUT_PREFIX] [--list OUT.txt]
import argparse, bisect, collections, os
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument('dump', nargs='?', default='build/asset_dump')
ap.add_argument('--game', action='append')
ap.add_argument('--sheet')
ap.add_argument('--list')
args = ap.parse_args()


def load(path):
    return np.asarray(Image.open(path).convert('RGBA'))


class Index:
    """Every row of every named image (and of its flips) in one byte string, to find where a row of pixels occurs."""

    def __init__(self, named, channels):
        parts, self.starts, self.where = [], [], []
        pos = 0
        for name, a in named.items():
            for tag, b in (('', a), ('flipped S', a[:, ::-1]), ('flipped T', a[::-1]), ('flipped S+T', a[::-1, ::-1])):
                b = np.ascontiguousarray(b[:, :, :channels])
                for y in range(b.shape[0]):
                    row = b[y].tobytes()
                    self.starts.append(pos)
                    self.where.append((name, tag, b, y))
                    parts.append(row + b'\xff' * 7)
                    pos += len(row) + 7
        self.blob = b''.join(parts)
        self.channels = channels

    def find(self, a, need_rows):
        """A named image (or flip) holding a's first need_rows rows as a crop: (name, tag, x, y) or None."""
        a = np.ascontiguousarray(a[:, :, :self.channels])
        h, w = a.shape[:2]
        # Probe with the row of most distinct pixels among the rows that must match (fewest false hits).
        probe = max(range(need_rows), key=lambda y: len({a[y, x].tobytes() for x in range(w)}))
        needle = a[probe].tobytes()
        start, hits = 0, 0
        while hits < 4000:
            at = self.blob.find(needle, start)
            if at < 0:
                return None
            start = at + 1
            i = bisect.bisect_right(self.starts, at) - 1
            off = at - self.starts[i]
            if off % self.channels:
                continue
            hits += 1
            name, tag, b, y = self.where[i]
            x, y0 = off // self.channels, y - probe
            if y0 < 0 or y0 + need_rows > b.shape[0] or x + w > b.shape[1]:
                continue
            if np.array_equal(b[y0:y0 + need_rows, x:x + w], a[:need_rows]):
                return name, tag, x, y0
        return None


for game in args.game or ['rush2', 'sfrush', 'rush2049']:
    base = os.path.join(args.dump, 'textures', game)
    udir = os.path.join(base, 'unnamed')
    if not os.path.isdir(udir):
        continue
    named = {}
    for line in open(os.path.join(base, 'names.txt')):
        p = line.split()
        if not p or p[0].startswith('#') or p[0].startswith('unnamed/'):
            continue
        f = os.path.join(base, p[0] + '.png')
        if os.path.exists(f):
            named[p[0]] = load(f)
    rgba, rgb = Index(named, 4), Index(named, 3)
    classes = collections.defaultdict(list)
    for f in sorted(os.listdir(udir)):
        a = load(os.path.join(udir, f))
        h, w = a.shape[:2]
        hit, cls = None, None
        for index, label in ((rgba, ''), (rgb, ' (alpha differs)')):
            hit = index.find(a, h)
            if hit:
                cls = 'crop of a named image' + label
                break
        if not hit:
            hit = rgba.find(a, max(1, h // 2)) or rgb.find(a, max(1, h // 2))
            cls = 'top rows match a named image'
        if hit:
            name, tag, x, y = hit
            b = named[name]
            whole = (x, y, w, h) == (0, 0, b.shape[1], b.shape[0])
            desc = '%s%s%s' % (name, ' ' + tag if tag else '', '' if whole else ' at %d,%d %dx%d of %dx%d' % (x, y, w, h, b.shape[1], b.shape[0]))
            if whole and cls.startswith('crop'):
                cls = ('mirror' if tag else 'same as') + ' a named image' + cls[len('crop of a named image'):]
        else:
            cls, desc = 'made at run time (no match)', '%dx%d' % (w, h)
        classes[cls].append((f, desc))
    total = sum(len(v) for v in classes.values())
    print('%s: %d unnamed' % (game, total))
    for c, v in sorted(classes.items(), key=lambda kv: -len(kv[1])):
        print('  %-42s %5d   e.g. %s' % (c, len(v), '; '.join('%s = %s' % e for e in v[:2])))
    if args.list:
        with open(args.list, 'a') as out:
            for c, v in classes.items():
                for f, desc in v:
                    out.write('%s/%s\t%s\t%s\n' % (game, f, c, desc))
    if args.sheet:
        for c, v in classes.items():
            cell, cols = 64, 20
            sel = v[:400]
            sheet = Image.new('RGBA', (cols * cell, max(1, (len(sel) + cols - 1) // cols) * cell), (40, 40, 40, 255))
            for i, (f, _) in enumerate(sel):
                t = Image.open(os.path.join(udir, f)).convert('RGBA')
                t.thumbnail((cell, cell))
                sheet.paste(t, ((i % cols) * cell, (i // cols) * cell))
            sheet.save('%s_%s_%s.png' % (args.sheet, game, c.split()[0]))
