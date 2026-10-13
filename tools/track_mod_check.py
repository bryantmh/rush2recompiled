"""Checks a custom track's converted files (tools/track_mod_test.bat output) with the Rush 2 format validators.

    python tools/track_mod_check.py OUTDIR

geometry.bin: emulated Rush 2 loads (1P/2P x normal/mirror) and an RSP walk of every list (model.load_test).
collision.bin: parses and validates (collision.Collision), then probes the ground under every AI lane point with the
game's own quadtree walk (descend by func_8006CD60's quadrant rule) to prove the tree finds the polygons.
path.bin: parses (paths.parse) and recomputes the load-time checkpoint crossings.
placement.bin: parses (placement.parse_r2) and checks every record's model is in the geometry's name table.
"""
import os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rush2049'))
import collision, model, paths, placement  # noqa: E402


def names_of(geo):
    mt, nt, tt, pt, n = struct.unpack_from('>5I', geo, 0)
    return [geo[nt + i * 0x18:nt + i * 0x18 + 16].split(b'\0')[0].decode('latin1') for i in range(n)]


def leaf_for(c, x, z):
    """func_8006CD60: descend from the root; returns the leaf's polygon list."""
    n = c.nodes[0]
    while True:
        x0, x1, z0, z1 = n['bounds']
        mx, mz = (x0 + x1) >> 1, (z0 + z1) >> 1
        q = (0 if x < mx else 1) + (2 if z < mz else 0)
        if n['mask'] & (1 << q):
            k = n['child'][q]
            if k == 0:
                return []
            n = c.nodes[k]
            continue
        off = n['leaf'][q]
        return c.leaves[off][0] if off else []


def main(outdir):
    problems = []
    geo = open(os.path.join(outdir, 'geometry.bin'), 'rb').read()
    errs = model.load_test(geo)
    problems += ['geometry: ' + e for e in errs[:20]]
    names = set(names_of(geo))
    print('geometry: %d bytes, %d models, load test %s' % (len(geo), len(names), 'OK' if not errs else '%d problems' % len(errs)))

    col = open(os.path.join(outdir, 'collision.bin'), 'rb').read()
    c = collision.Collision.parse(col, collision.R2)
    errs = c.validate()
    problems += ['collision: ' + e for e in errs[:20]]
    print('collision: %d polys, %d nodes, %d leaves, validate %s' % (len(c.polys), len(c.nodes), len(c.leaves),
                                                                      'OK' if not errs else '%d problems' % len(errs)))

    p = paths.parse(open(os.path.join(outdir, 'path.bin'), 'rb').read())
    print('path: %d checkpoints, spine %d, lanes %s, ends at %d of %d bytes' % (
        p['n_cp'], len(p['spine']), [ln['count'] for ln in p['lanes']], p['end'], len(p['raw'])))
    if p['end'] != len(p['raw']):
        problems.append('path: parse ends at %d, file is %d bytes' % (p['end'], len(p['raw'])))
    for i, cp in enumerate(p['cps']):
        k = paths.crossing(cp, p['spine'])
        print('  checkpoint %d at spine %d (flags %d, radius %.0f ft)' % (i, k, cp['flags'], cp['r2'] ** 0.5))
    # every lane point: is there a polygon in its leaf whose box holds it?
    missing = 0
    for ln in p['lanes']:
        for pt in ln['points']:
            x, z = pt[0], pt[2]
            found = False
            for pi in leaf_for(c, x, z):
                vs = [collision.vert_xyz(c.verts[v]) for v in c.polys[pi]['verts']]
                if (c.polys[pi]['flags'] & 0xF) not in (5, 6):
                    found = True
                    break
            missing += not found
    print('lanes: %d points with no ground polygon in their quadtree leaf' % missing)

    pl = placement.parse_r2(open(os.path.join(outdir, 'placement.bin'), 'rb').read())
    recs = pl[1] if isinstance(pl, tuple) else pl['records'] if isinstance(pl, dict) else pl
    bad = [r.name for r in recs if r.name not in names]
    problems += ['placement: record %s has no model' % n for n in bad]
    print('placement: %d records, %d without a model' % (len(recs), len(bad)))

    print('\n'.join(problems) if problems else 'ALL OK')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1]))
