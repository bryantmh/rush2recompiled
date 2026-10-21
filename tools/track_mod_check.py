"""Checks a custom track's converted files (tools/track_mod_test.bat output) with the Rush 2 format validators.

    python tools/track_mod_check.py OUTDIR [--stock]

--stock: OUTDIR is a converted stock track (Rush 2049 or SF Rush, e.g. tools/rush2049/cpp_test output): the geometry is
also checked as a Rush 2 container (model.R2Model.check), the path with paths.validate, and placement records may name
Rush 2's shared models (assets 0x12 and 0x14; the geometry must not define them too) or records Rush 2's own classifier gives a class of their own (keys,
breakable glass and flags: placement.R2Rules), which the converters leave to Rush 2.

geometry.bin: emulated Rush 2 loads (1P/2P x normal/mirror) and an RSP walk of every list (model.load_test).
collision.bin: parses and validates (collision.Collision), then probes the ground under every AI lane point with the
game's own quadtree walk (descend by func_8006CD60's quadrant rule) to prove the tree finds the polygons.
path.bin: parses (paths.parse) and recomputes the load-time checkpoint crossings.
placement.bin: parses (placement.parse_r2) and checks every record's model is in the geometry's name table.
"""
import os, struct, sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'rush2049'))
import collision, model, paths, placement, roms  # noqa: E402


def names_of(geo):
    mt, nt, tt, pt, n = struct.unpack_from('>5I', geo, 0)
    return [geo[nt + i * 0x18:nt + i * 0x18 + 16].split(b'\0')[0].decode('latin1') for i in range(n)]


def leaf_for(c, x, z):
    """func_8006CD60: descend from the root; returns the leaf's polygon list. A file tagged in the root's byte +2
    (0x80: src/rush2049/track2049_collision.cpp, the form the converter takes past 64 KB of leaves) splits toward zero."""
    n = c.nodes[0]
    toward_zero = bool(n['pad'] & 0x80)
    while True:
        x0, x1, z0, z1 = n['bounds']
        if toward_zero:
            mx, mz = int((x0 + x1) / 2), int((z0 + z1) / 2)
        else:
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


def main(outdir, stock=False):
    problems = []
    geo = open(os.path.join(outdir, 'geometry.bin'), 'rb').read()
    errs = model.load_test(geo)
    if stock:
        errs = model.R2Model(geo).check()[0] + errs
    problems += ['geometry: ' + e for e in errs[:20]]
    names = set(names_of(geo))
    print('geometry: %d bytes, %d models, load test %s' % (len(geo), len(names), 'OK' if not errs else '%d problems' % len(errs)))

    col = open(os.path.join(outdir, 'collision.bin'), 'rb').read()
    c = collision.Collision.parse(col, collision.R2)
    errs = c.validate()
    problems += ['collision: ' + e for e in errs[:20]]
    print('collision: %d polys, %d nodes, %d leaves, validate %s' % (len(c.polys), len(c.nodes), len(c.leaves),
                                                                      'OK' if not errs else '%d problems' % len(errs)))
    # Surfaces: polygon count per flags value (type = low 4 bits, material slot = bits 4-7).
    census = {}
    for poly in c.polys:
        census[poly['flags']] = census.get(poly['flags'], 0) + 1
    print('  surfaces: ' + ', '.join('flags 0x%04X (type %d, material %d) x%d' % (f, f & 0xF, (f >> 4) & 0xF, n)
                                     for f, n in sorted(census.items())))

    p = paths.parse(open(os.path.join(outdir, 'path.bin'), 'rb').read())
    print('path: %d checkpoints, spine %d, lanes %s, ends at %d of %d bytes' % (
        p['n_cp'], len(p['spine']), [ln['count'] for ln in p['lanes']], p['end'], len(p['raw'])))
    if p['end'] != len(p['raw']):
        problems.append('path: parse ends at %d, file is %d bytes' % (p['end'], len(p['raw'])))
    if stock:
        problems += ['path: ' + e for e in paths.validate(p)]
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
    special = lambda name: False
    if stock:
        r2 = roms.Rush2()
        shared = set()
        for a in (0x12, 0x14):
            shared |= {m['name'] for m in model.R2Model(r2.asset(a)).models}
        if names & shared:
            problems.append('geometry names also in shared assets (shared slot may win): %s' % sorted(names & shared))
        names |= shared
        rules = placement.R2Rules(r2)
        special = lambda name: rules.classify(name)[0] != 0
    bad = [r.name for r in recs if r.name not in names and not special(r.name)]
    problems += ['placement: record %s has no model' % n for n in bad]
    print('placement: %d records, %d without a model' % (len(recs), len(bad)))

    print('\n'.join(problems) if problems else 'ALL OK')
    return 1 if problems else 0


if __name__ == '__main__':
    args = [a for a in sys.argv[1:] if a != '--stock']
    sys.exit(main(args[0], stock='--stock' in sys.argv))
