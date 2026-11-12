#!/usr/bin/env python3
"""Builds a new-car stress mod: COUNT cars cloned from a mod's new cars, for testing the car select, races and online
play with every mod car slot full (the game takes 64 new cars, types 57-120, include/car_types.h).

    python tools/car_mod_stress.py OUT_DIR --from MOD.rtz [--count N] [--id ID]

MOD.rtz is a mod with rush2/new_cars (e.g. build/mods/sfrush_ps1.rtz, whose cars are small). Copy i (STRESS01, ...)
takes source car i % (its cars), renamed "STRESS 01" and so on, without its logo image so the car select letters the
number. Link the result into a test run with tools/test_race.py --mod-dir OUT_DIR (docs/car_mods.md#in-the-game).
The bundled mods' cars come first in mod order, so with them on the copies past the free slots are left out (the log
says which).
"""
import argparse, json, os, shutil, zipfile


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('out')
    ap.add_argument('--from', dest='source', required=True, help='a mod .rtz with rush2/new_cars')
    ap.add_argument('--count', type=int, default=64)
    ap.add_argument('--id', default='zz_car_stress')
    a = ap.parse_args()

    z = zipfile.ZipFile(a.source)
    names = z.namelist()
    cars = sorted(n[len('rush2/new_cars/'):-5] for n in names
                  if n.startswith('rush2/new_cars/') and n.endswith('.json'))
    if not cars:
        raise SystemExit('no rush2/new_cars/*.json in ' + a.source)
    if os.path.isdir(a.out):
        shutil.rmtree(a.out)
    dest = os.path.join(a.out, 'rush2', 'new_cars')
    os.makedirs(dest)
    for i in range(a.count):
        car = cars[i % len(cars)]
        new = 'STRESS%02d' % (i + 1)
        spec = json.loads(z.read('rush2/new_cars/%s.json' % car))
        spec['name'] = 'STRESS %02d' % (i + 1)
        spec.pop('logo', None)
        spec['source'] = 'tools/car_mod_stress.py copy of %s' % car
        with open(os.path.join(dest, new + '.json'), 'w', encoding='utf-8') as f:
            json.dump(spec, f, indent=4)
        # The car's model files: <ID>.glb, or its schemes <ID>_COLn.glb with their _HOOD / _BRAKE parts.
        for n in names:
            base = n[len('rush2/new_cars/'):]
            if n.startswith('rush2/new_cars/') and base.endswith('.glb') and (base == car + '.glb' or base.startswith(car + '_')):
                with open(os.path.join(dest, new + base[len(car):]), 'wb') as f:
                    f.write(z.read(n))
    manifest = {
        'authors': ['rush2-recomp'],
        'description': '%d new cars cloned from %s (tools/car_mod_stress.py), for testing every mod car slot.' %
                       (a.count, os.path.basename(a.source)),
        'display_name': 'Car Slot Stress Test',
        'game_id': 'rush2',
        'id': a.id,
        'minimum_recomp_version': '0.0.0',
        'short_description': 'Car slot stress test',
        'version': '1.0.0',
    }
    with open(os.path.join(a.out, 'mod.json'), 'w', encoding='utf-8') as f:
        json.dump(manifest, f, indent=4)
    print('%d cars from %d sources in %s' % (a.count, len(cars), a.out))


if __name__ == '__main__':
    main()
