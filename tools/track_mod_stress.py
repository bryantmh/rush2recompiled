#!/usr/bin/env python3
"""Builds a custom-track stress mod: COUNT copies of one track, for testing the track select, circuit screen and
anything else that has to cope with many custom tracks (the game takes up to 195, ids 38-63 and 80-248).

    python tools/track_mod_stress.py OUT_DIR --track TRACK_DIR [--count N] [--diorama GLB] [--id ID]

TRACK_DIR is a custom track folder (track.glb and track.toml; a small one converts fast, e.g. tmp/cruisn/mod1/tracks/
germany). Each copy tracks/stress_NNN gets its own numbered logo; even copies get --diorama as their authored
diorama.glb (a big one, e.g. the PlayStation SF Rush mod's tracks/chinatown/diorama.glb), odd ones a route preview in
front of a numbered 512 x 256 preview.png (the largest picture the converter keeps). Link the result into a test run
with tools/test_race.py --mod-dir OUT_DIR (docs/custom_tracks.md#size-limits).
"""
import argparse, json, os, re, shutil, sys

from PIL import Image, ImageDraw, ImageFont


def font(size):
    for name in ('arialbd.ttf', 'DejaVuSans-Bold.ttf'):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def picture(w, h, text, bg, size):
    im = Image.new('RGBA', (w, h), bg)
    d = ImageDraw.Draw(im)
    f = font(size)
    box = d.textbbox((0, 0), text, font=f)
    d.text(((w - (box[2] - box[0])) // 2 - box[0], (h - (box[3] - box[1])) // 2 - box[1]), text, font=f,
           fill=(255, 255, 255, 255))
    return im


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--track', required=True, help='custom track folder to copy (track.glb, track.toml)')
    ap.add_argument('--count', type=int, default=172)
    ap.add_argument('--diorama', help='diorama.glb for the even copies (none: every copy a route preview)')
    ap.add_argument('--id', default='track_stress', help="the mod's id (mod.json)")
    a = ap.parse_args()

    toml = open(os.path.join(a.track, 'track.toml'), encoding='utf-8').read()
    toml = re.sub(r'(?ms)^\[menu\].*?(?=^\[|\Z)', '', toml)          # the copies' own [menu] is written below
    if os.path.exists(a.out):
        shutil.rmtree(a.out)
    os.makedirs(os.path.join(a.out, 'tracks'))
    with open(os.path.join(a.out, 'mod.json'), 'w', encoding='utf-8') as f:
        json.dump({'game_id': 'rush2', 'id': a.id, 'display_name': 'Custom track stress test',
                   'description': 'tools/track_mod_stress.py', 'short_description': 'stress test', 'version': '0.1.0',
                   'authors': ['test'], 'minimum_recomp_version': '0.0.0'}, f, indent=4)
    for n in range(1, a.count + 1):
        folder = os.path.join(a.out, 'tracks', 'stress_%03d' % n)
        os.makedirs(folder)
        shutil.copy(os.path.join(a.track, 'track.glb'), folder)
        hue = (n * 47) % 360
        bg = tuple(int(c) for c in Image.new('HSV', (1, 1), (hue * 255 // 360, 200, 150)).convert('RGB').getpixel((0, 0)))
        picture(128, 32, 'STRESS %03d' % n, bg + (255,), 22).save(os.path.join(folder, 'logo.png'))
        menu = ['[menu]', 'logo = "logo.png"']
        if a.diorama and n % 2 == 0:
            shutil.copy(a.diorama, os.path.join(folder, 'diorama.glb'))
            menu.append('diorama = "diorama.glb"')
        else:
            picture(512, 256, '%03d' % n, bg + (255,), 160).save(os.path.join(folder, 'preview.png'))
            menu.append('preview = "preview.png"')
        text = re.sub(r'(?m)^name\s*=.*$', 'name = "Stress %03d"' % n, toml).rstrip() + '\n\n' + '\n'.join(menu) + '\n'
        with open(os.path.join(folder, 'track.toml'), 'w', encoding='utf-8', newline='\n') as f:
            f.write(text)
    print('%d tracks in %s' % (a.count, a.out))


if __name__ == '__main__':
    sys.exit(main())
