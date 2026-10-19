#!/usr/bin/env python3
"""California Speed's extended scenery outside the game: runs src/calspeed/calspeed_scenery.cpp on tracks' files
(tools/calspeed/scenery_test.cpp, built by scenery_test.bat), checks the collision it adds, and draws views of the
result with tools/glb_view.py (the track as placed through tools/trackview/track_glb.cpp).

    python tools/calspeed/scenery.py [TRACK ...] [--views back,top,far,sea] [--out DIR] [--no-render] [--stock]

Views (images in OUT, default tmp/scenery/):
  back  from the start line looking back down the road (where the barricades and the bend are)
  top   from above the start, looking down
  far   from the start line at 6 ft, looking back over 60 degrees either side (wide)
  sea   from high over the track toward the far side of the sea
  sign  close on the barricades and their ROAD CLOSED sign
--stock draws the same views of the unchanged track (stock_<view>_N.png), to tell the new geometry from the old.
Tracks are California Speed's 0-15 (default: every point-to-point track). docs/calspeed_research.md#extended-scenery.
"""
import argparse, math, os, struct, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(REPO, 'tools', 'rush2049'))
sys.path.insert(0, os.path.join(REPO, 'tools', 'trackview'))
if 'CALSPEED_ROM' not in os.environ and os.path.exists(os.path.join(REPO, 'build', 'calspeed.z64')):
    os.environ['CALSPEED_ROM'] = os.path.join(REPO, 'build', 'calspeed.z64')
from cs import CalSpeed  # noqa: E402
import paths  # noqa: E402
import stock  # noqa: E402

EXE = os.path.join(REPO, 'tmp', 'scenery_test', 'scenery_test.exe')
BAT = os.path.join(REPO, 'tools', 'calspeed', 'scenery_test.bat')
GLB_EXE = os.path.join(REPO, 'tmp', 'track_glb', 'track_glb.exe')
VIEW = os.path.join(REPO, 'tools', 'glb_view.py')
POINT_TO_POINT = [0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11]


def build():
    srcs = [os.path.join(REPO, p) for p in ('tools/calspeed/scenery_test.cpp', 'src/calspeed/calspeed_scenery.cpp',
                                            'include/calspeed_scenery.h')]
    if os.path.exists(EXE) and all(os.path.getmtime(s) < os.path.getmtime(EXE) for s in srcs):
        return
    r = subprocess.run(['cmd', '/c', BAT], cwd=REPO, capture_output=True, text=True, errors='replace')
    if r.returncode != 0 or not os.path.exists(EXE):
        sys.exit('build failed:\n' + r.stdout[-3000:] + r.stderr[-3000:])


def start_frame(path):
    """Spine point 0 and the road's direction there (x, z unit vector)."""
    s = paths.parse(path)['spine']
    a = s[0]
    b = next(q for q in s if math.hypot(q[0] - a[0], q[2] - a[2]) > 40)
    dx, dz = b[0] - a[0], b[2] - a[2]
    d = math.hypot(dx, dz)
    return a, (dx / d, dz / d)


def render(glb, out, eye, at, fov=75, w=640, h=360):
    # Back faces culled and textures filtered, as the game draws them (a surface facing the wrong way is a hole).
    m = lambda p: [str(-p[0]), str(p[1]), str(p[2])]   # the placed glb mirrors x
    subprocess.run([sys.executable, VIEW, glb, out, '--eye', *m(eye), '--at', *m(at), '--fov', str(fov),
                    '--width', str(w), '--height', str(h), '--cull', '1', '--bilinear'], check=True, capture_output=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tracks', nargs='*', type=int)
    ap.add_argument('--views', default='back,top')
    ap.add_argument('--out', default=os.path.join(REPO, 'tmp', 'scenery'))
    ap.add_argument('--no-render', action='store_true')
    ap.add_argument('--stock', action='store_true')
    a = ap.parse_args()
    build()
    c = CalSpeed()
    os.makedirs(a.out, exist_ok=True)
    failed = False
    for t in a.tracks or POINT_TO_POINT:
        work = os.path.join(a.out, 't%d' % t)
        os.makedirs(work, exist_ok=True)
        files = {'g': c.asset(62 + t), 'p': c.asset(78 + t), 'c': stock.remap_cs(c.asset(94 + t)), 'path': c.asset(110 + t)}
        for k, v in files.items():
            with open(os.path.join(work, k + '.bin'), 'wb') as f:
                f.write(v)
        r = subprocess.run([EXE, str(t)] + [os.path.join(work, k + '.bin') for k in ('g', 'p', 'c', 'path')] + [work],
                           capture_output=True, text=True, errors='replace')
        print('--- track %d (%s)' % (t, stock.CS_NAMES[t]))
        print(r.stdout.strip())
        if r.returncode != 0:
            failed = True
            continue
        if a.no_render:
            continue
        glb = os.path.join(work, 'stock.glb' if a.stock else 'track.glb')
        src = ('g.bin', 'p.bin') if a.stock else ('geometry.bin', 'placement.bin')
        g = subprocess.run([GLB_EXE, os.path.join(work, src[0]), os.path.join(work, src[1]), glb, 'track'],
                           capture_output=True, text=True)
        if g.returncode != 0:
            print('track_glb failed', g.stderr)
            failed = True
            continue
        s0, (dx, dz) = start_frame(files['path'])
        views = a.views.split(',')
        pre = 'stock_' if a.stock else ''
        if 'back' in views:
            eye = (s0[0] + dx * 30, s0[1] + 6, s0[2] + dz * 30)
            render(glb, os.path.join(a.out, pre + 'back_%d.png' % t), eye, (s0[0] - dx * 100, s0[1] + 3, s0[2] - dz * 100))
        if 'top' in views:
            cx, cz = s0[0] - dx * 300, s0[2] - dz * 300
            render(glb, os.path.join(a.out, pre + 'top_%d.png' % t), (cx, s0[1] + 1400, cz), (cx + 0.01, s0[1], cz + 1), 70, 480, 480)
        if 'far' in views:
            eye = (s0[0] + dx * 30, s0[1] + 6, s0[2] + dz * 30)
            render(glb, os.path.join(a.out, pre + 'far_%d.png' % t), eye, (s0[0] - dx * 100, s0[1] + 6, s0[2] - dz * 100), 120, 960, 360)
        if 'sign' in views:
            # Close on the barricades (about 45 ft past the cut) from 25 ft behind the spine's start.
            eye = (s0[0] - dx * 25, s0[1] + 5, s0[2] - dz * 25)
            render(glb, os.path.join(a.out, pre + 'sign_%d.png' % t), eye, (s0[0] - dx * 70, s0[1] + 3.5, s0[2] - dz * 70), 40, 640, 360)
            render(glb, os.path.join(a.out, pre + 'signclose_%d.png' % t), eye, (s0[0] - dx * 70, s0[1] + 3.5, s0[2] - dz * 70), 12, 640,
                   360)
        if 'sea' in views:
            eye = (s0[0] + dx * 300, s0[1] + 400, s0[2] + dz * 300)
            render(glb, os.path.join(a.out, pre + 'sea_%d.png' % t), eye, (s0[0] - dx * 3000, s0[1], s0[2] - dz * 3000), 90, 960, 400)
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
