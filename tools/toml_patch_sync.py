#!/usr/bin/env python3
"""Checks generated `[[patches.instruction]]` entries against us.toml and, with --apply, updates the values of the
matching entries in place (same func and vram), leaving everything else in the file as it is.

For generators whose output is spread through us.toml rather than in a marked block (tools/rush2049/cartypes.py toml):
after changing a layout, regenerate and sync instead of hand-editing hundreds of entries.

    python tools/rush2049/cartypes.py toml > tmp/cartypes.toml
    python tools/toml_patch_sync.py tmp/cartypes.toml            # report: changed, missing, duplicated entries
    python tools/toml_patch_sync.py tmp/cartypes.toml --apply    # write the changed values

Entries the generator emits that us.toml lacks are listed (add them by hand), and nothing is removed.
"""
import argparse
import os
import re
import sys

REPO = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
ENTRY = re.compile(r'\[\[patches\.instruction\]\]\s*\nfunc = "([^"]+)"\s*\nvram = (0x[0-9A-Fa-f]+)\s*\nvalue = (0x[0-9A-Fa-f]+)([^\r\n]*)')


def entries(text):
    return [(m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4), m) for m in ENTRY.finditer(text)]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('generated')
    ap.add_argument('--toml', default=os.path.join(REPO, 'us.toml'))
    ap.add_argument('--apply', action='store_true')
    args = ap.parse_args()
    gen = entries(open(args.generated, encoding='utf-8').read())
    with open(args.toml, encoding='utf-8', newline='') as f:
        text = f.read()
    have = {}
    for func, vram, value, comment, m in entries(text):
        have.setdefault((func, vram), []).append(m)
    changes, missing, dup = [], [], []
    for func, vram, value, comment, _ in gen:
        found = have.get((func, vram), [])
        if not found:
            missing.append((func, vram, value, comment))
        elif len(found) > 1:
            dup.append((func, vram))
        elif int(found[0].group(3), 16) != value or found[0].group(4) != comment:
            changes.append((found[0], value, comment))
    print('%d generated, %d to change, %d missing, %d duplicated' % (len(gen), len(changes), len(missing), len(dup)))
    for func, vram, value, comment in missing:
        print('missing: %s 0x%08X = 0x%08X%s' % (func, vram, value, comment))
    for func, vram in dup:
        print('duplicated: %s 0x%08X' % (func, vram))
    if not args.apply:
        for m, value, comment in changes[:40]:
            print('change: %s %s: %s%s -> 0x%08X%s' % (m.group(1), m.group(2), m.group(3), m.group(4), value, comment))
        return
    out, at = [], 0
    for m, value, comment in sorted(changes, key=lambda c: c[0].start(3)):
        out.append(text[at:m.start(3)])
        out.append('0x%08X%s' % (value, comment))
        at = m.end(4)
    out.append(text[at:])
    with open(args.toml, 'w', encoding='utf-8', newline='') as f:
        f.write(''.join(out))
    print('applied', len(changes))


if __name__ == '__main__':
    sys.exit(main())
