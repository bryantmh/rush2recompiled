#!/usr/bin/env python3
"""Loudness of each sound source in a game run, from the streams RUSH2_AUDIO_LEVELS=<folder> writes (src/main.cpp:
log_audio_levels; each source mixed apart at 100% main volume, raw interleaved stereo f32, rate in rate.txt).

    python tools/audio_loudness.py DIR [DIR ...] [--skip SECONDS] [--seconds SECONDS]
    python tools/audio_loudness.py DIR --sweep RUN_LOG [--csv OUT]

For each folder and source (rush2: Rush 2's own music and effects, rush2049: everything the Rush 2049 sound engine
plays, wings, car_engines) prints the integrated loudness (LUFS, ITU-R BS.1770: K-weighted and gated, so a source that
is silent most of the time is measured over the time it sounds), the loudest 3 s (short-term max), the loudest 400 ms
(momentary max) and the sample peak in dBFS. --skip drops the start of the run (boot, menus) and --seconds limits the
length measured. --sweep reads the "[sweep]" lines a RUSH2_TEST_SFX_SWEEP run printed (src/sfx_sweep.cpp) and gives
each effect's loudest 400 ms (momentary max, LUFS) in the window it played in, from its game's stream, then the power
mean of the engine sounds and of the other effects (silent ids left out). Test aid for balancing Rush 2049's sound against Rush 2's (docs/rush2049_research/audio.md).
Needs numpy and pyloudnorm (pip install pyloudnorm).
"""
import argparse, os, sys

import numpy as np
import pyloudnorm


def k_weighted_power(meter, x):
    """Per-sample K-weighted power summed over both channels (the filters integrated_loudness uses)."""
    y = x.copy()
    for f in meter._filters.values():
        y = f.apply_filter(y)
    return (y ** 2).sum(axis=1)


def window_max(power, rate, seconds):
    """The loudest `seconds` window (stepped by a tenth of it), in LUFS without gating."""
    n = int(rate * seconds)
    if len(power) < n:
        return float('-inf')
    c = np.concatenate(([0.0], np.cumsum(power)))
    step = max(1, n // 10)
    z = (c[n::step] - c[:len(c) - n:step]) / n
    z = z[z > 0]
    return float(-0.691 + 10 * np.log10(z.max())) if len(z) else float('-inf')


def sweep(d, log, csv):
    import re
    rate = int(open(os.path.join(d, 'rate.txt')).read().strip())
    meter = pyloudnorm.Meter(rate)
    starts, engines, done = [], {'rush2': set(), '2049': set()}, None
    for line in open(log, errors='ignore'):
        m = re.match(r'\[sweep\] (rush2|2049) engine 0x([0-9A-F]+)', line)
        if m:
            engines[m.group(1)].add(int(m.group(2), 16))
            continue
        m = re.match(r'\[sweep\] (rush2|2049) 0x([0-9A-F]+) frame (\d+)', line)
        if m:
            starts.append((m.group(1), int(m.group(2), 16), int(m.group(3))))
        m = re.match(r'\[sweep\] done frame (\d+)', line)
        if m:
            done = int(m.group(1))
    if not starts:
        sys.exit('no [sweep] lines in ' + log)
    game = starts[0][0]
    x = np.fromfile(os.path.join(d, ('rush2' if game == 'rush2' else 'rush2049') + '.f32'), dtype=np.float32)
    x = x.reshape(-1, 2).astype(np.float64)
    power = k_weighted_power(meter, x)
    rows = []
    for i, (g, sid, f) in enumerate(starts):
        end = starts[i + 1][2] if i + 1 < len(starts) else (done or f + int(1.5 * rate))
        seg = power[f:min(end, len(power))]
        mm = window_max(seg, rate, 0.4)
        rows.append((sid, mm, sid in engines[g]))
    out = open(csv, 'w') if csv else None
    for sid, mm, eng in rows:
        line = f'{sid:#04x},{mm:.2f},{"engine" if eng else "effect"}'
        print(line)
        if out:
            out.write(line + '\n')
    for label, sel in (('engines', True), ('effects', False)):
        v = [mm for sid, mm, eng in rows if eng == sel and mm > -70]
        if v:
            mean = 10 * np.log10(np.mean([10 ** (m / 10) for m in v]))
            print(f'{game} {label}: {len(v)} sounding, power mean of momentary max {mean:.2f} LUFS')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('dirs', nargs='+')
    ap.add_argument('--skip', type=float, default=0.0)
    ap.add_argument('--seconds', type=float, default=0.0)
    ap.add_argument('--sweep', help='run log of a RUSH2_TEST_SFX_SWEEP run (per-effect levels)')
    ap.add_argument('--csv', help='with --sweep: also write id,LUFS,kind lines here')
    a = ap.parse_args()
    if a.sweep:
        sweep(a.dirs[0], a.sweep, a.csv)
        return
    for d in a.dirs:
        try:
            rate = int(open(os.path.join(d, 'rate.txt')).read().strip())
        except OSError:
            print(f'{d}: no rate.txt (run with RUSH2_AUDIO_LEVELS=<folder>)', file=sys.stderr)
            continue
        meter = pyloudnorm.Meter(rate)
        print(d)
        for name in ('rush2', 'rush2049', 'wings', 'car_engines'):
            path = os.path.join(d, name + '.f32')
            if not os.path.exists(path):
                continue
            x = np.fromfile(path, dtype=np.float32).reshape(-1, 2).astype(np.float64)
            x = x[int(a.skip * rate):]
            if a.seconds > 0:
                x = x[:int(a.seconds * rate)]
            if len(x) < rate or not np.any(x):
                print(f'  {name:12s} silent')
                continue
            lufs = meter.integrated_loudness(x)
            power = k_weighted_power(meter, x)
            peak = 20 * np.log10(np.max(np.abs(x)))
            print(f'  {name:12s} {lufs:6.1f} LUFS  short-term max {window_max(power, rate, 3.0):6.1f}  '
                  f'momentary max {window_max(power, rate, 0.4):6.1f}  peak {peak:5.1f} dBFS  ({len(x) / rate:.0f} s)')


if __name__ == '__main__':
    main()
