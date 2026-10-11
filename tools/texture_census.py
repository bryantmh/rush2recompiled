"""Runs one race per track step with the texture census on (RUSH2_TEXTURE_CENSUS, src/texture_census.cpp) and keeps
each report: which drawn textures the catalog can't name and where their texels come from.

Usage: python tools/texture_census.py OUT_DIR STEP [STEP ...]
STEP is how many times the track select is stepped right from the saved track before the race (0 = the saved one).
Set RUSH2_CENSUS_CAR=N to step the car select left N times from the saved car, right for a negative N (to race a
given car's textures).
Set RUSH2_EXE to the build to run (it runs in its own folder, portable, with that folder's saves and ROMs); use a copy
of build/ in tmp/ so the race doesn't touch your own saves. Each race holds the gas for about 75 s. Writes
OUT_DIR/step<N>.txt (the report), step<N>.txt_images/ (the drawn images) and step<N>_track.png (the track select).
"""
import os, subprocess, sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..'))
out = os.path.abspath(sys.argv[1])
os.makedirs(out, exist_ok=True)
for step in [int(s) for s in sys.argv[2:]]:
    # Title, START GAME > ONE RACE, JUST PLAY, then the track select (about 16 s in).
    presses = ['8:START', '11:A', '14:A']
    t = 16.5
    for _ in range(step):
        presses.append('%.1f:DR' % t)
        t += 0.6
    shot = t + 0.5
    presses.append('%.1f:A' % (t + 1.5))
    t += 4.5  # The car select.
    car_steps = int(os.environ.get('RUSH2_CENSUS_CAR', '0'))
    for _ in range(abs(car_steps)):
        presses.append('%.1f:%s' % (t, 'DL' if car_steps > 0 else 'DR'))
        t += 0.6
    presses += ['%.1f:A' % t, '%.1f:A' % (t + 3), '%.1f:A:75' % (t + 6)]
    report = os.path.join(out, 'step%d.txt' % step)
    env = dict(os.environ, RUSH2_TEXTURE_CENSUS=report, RUSH2_TEXTURE_DUMP='%d' % int(t + 90))
    # The census is written at RUSH2_TEXTURE_DUMP; the dump that follows is cut short when the game is closed.
    shots = os.path.join(out, 'step%d_shots' % step)
    subprocess.run([sys.executable, os.path.join(REPO, 'tools', 'rush1', 'shots.py'), shots, ','.join(presses),
                    '%.1f' % shot, '%.1f' % (t + 40), '%.1f' % (t + 100)], env=env, check=False)
    src = os.path.join(shots, 'shot_%05.1f.png' % shot)
    if os.path.exists(src):
        os.replace(src, os.path.join(out, 'step%d_track.png' % step))
    print('step', step, 'report', report if os.path.exists(report) else 'missing')
