"""Writes the inputs and Python results of the car decal test (see car_decals_main.cpp): out/cars/<CAR>_r1.bin, _r2.bin,
stripe.bin (Rush 1 car file, Rush 2 car file, Rush 2 asset 0x1C) and out/cars/<CAR>_ref.bin (panels 1-6: w, h, colours,
quarter-size colours; w = 0 for a panel without decal)."""
import os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))
sys.path.insert(0, os.path.join(HERE, '..', '..', 'rush2049'))
import cardecal
from cartex import load, Rush2

out = os.path.join(HERE, 'out', 'cars'); os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'stripe.bin'), 'wb').write(Rush2().asset(0x1C))
for car in cardecal.CARS:
    open(os.path.join(out, f'{car}_r1.bin'), 'wb').write(load(1, car))
    open(os.path.join(out, f'{car}_r2.bin'), 'wb').write(load(2, car))
    res = cardecal.project(car); by_n = {int(n[-1]): v for n, v in res.items()}
    ref = bytearray()
    for n in range(1, 7):
        if n in by_n:
            w, h, colours, _ = by_n[n]
            ref += bytes([w, h]) + bytes(colours) + bytes(cardecal.lod_tile(colours, w, h))
        else:
            ref += bytes([0, 0])
    open(os.path.join(out, f'{car}_ref.bin'), 'wb').write(ref)
print('wrote', out)
