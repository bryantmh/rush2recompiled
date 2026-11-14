"""Road-level renders of a converted SF Rush track along its AI spine, for checking a conversion (the N64 night
versions' recolor and lit windows, docs/sfrush_ps1.md#n64-night-versions) without the game.

    python tools/rush1/spine_views.py TRACK.glb PATH.bin OUT_PREFIX I [I ...]

TRACK.glb: the geometry as tools/asset_catalog_test.exe --file GEOMETRY PLACEMENT writes it (the placed scene, world
units). PATH.bin: the track's Rush 2 path file (tools/rush1/cpp_test/out/track1_test.exe . --night OUT writes
path0<t>.bin). Each I is a spine point: the camera sits 8 units above it looking at the point 6 ahead, written to
OUT_PREFIX_<I>.png (640 x 400, back faces culled as the game culls them). The glb's x is the path's -x.
"""
import os, subprocess, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', 'rush2049'))
import paths


def main():
    if len(sys.argv) < 5:
        print(__doc__)
        return 1
    glb, path, out = sys.argv[1:4]
    spine = paths.parse(open(path, 'rb').read())['spine']
    for i in (int(a) for a in sys.argv[4:]):
        a, b = spine[i % len(spine)], spine[(i + 6) % len(spine)]
        eye = [-a[0], a[1] + 8, a[2]]
        at = [-b[0], b[1] + 6, b[2]]
        subprocess.run([sys.executable, os.path.join(HERE, '..', 'glb_view.py'), glb, '%s_%d.png' % (out, i),
                        '--eye', *map(str, eye), '--at', *map(str, at), '--width', '640', '--height', '400',
                        '--fov', '60', '--sky', '40', '40', '40', '--cull', '1'], check=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
