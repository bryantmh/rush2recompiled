"""AI path ("race route") files of Rush 2 (assets 0x57-0x6E) and Rush 2049 (files 158-182).

Both games use the same file format and near-identical loader code (Rush 2 func_80093300 + func_80093048,
2049 func_800BADE0 + func_800BAAA0). See docs/rush2049_research/race.md for field meanings and evidence.

Layout (big-endian):
  0x000  header, 12 bytes: u16 base_time (ignored, runtime forces 90), s16 loop_cp, s16 finish_cp, s16 arm_cp
         (all three recomputed from checkpoint flags), s16 n_cp, s16 pad
  0x00C  10 checkpoint records x 0x50 (first n_cp used):
         +00 f32 pos[3]  +0C f32 dir[3] (direction of travel; crossing plane normal)  +18 u32 radius^2
         +1C s16 flags (1 = finish/lap line, 2 = lap counting armed after passing, 4 = loop start / wrap target)
         +1E s16 time bonus lap 1, +20 s16 time bonus later laps (both overwritten with 45 at load)
         +22 s16 cross[20] (runtime: crossing index on spine, lanes 0-3, branches 0-14)
         +4A s16 pad  +4C f32 distance to next checkpoint along the spine (runtime)
  0x32C  route header 16 bytes: u16 n_spine, u16 pad, u32 ptr (runtime), u8 n_branch, 3 pad, u32 ptr (runtime)
  0x33C  n_branch x 0x10: u8 alt_flag, s8 from_path, u16 from_idx, s8 to_path, u8 pad, u16 to_idx, s8 cp,
         u8 pad, u16 count, u32 ptr (runtime). from/to/cp are recomputed at load (func_80092060).
         path id -1 = spine, else branch index.
  then   u16 n_points_total (= n_spine + sum(branch counts)); spine n_spine x 6 (s16 x,y,z);
         branch points in branch order (count x 6 each)
  then   4 lanes: 8-byte header (u16 count, u8 unk2, u8 unk3, u32 ptr runtime) + count x 8
         (s16 x,y,z, u8 target speed in mph, u8 behaviour code)

Usage: python paths.py          -> self-check over every Rush 2 and Rush 2049 path file
"""
import math, struct, sys

CP_BASE, CP_SIZE, CP_MAX = 0xC, 0x50, 10
ROUTE = 0x32C
MAX_BRANCH = 15          # cross[20] = spine + 4 lanes + 15 branches


def _s16(d, o): return struct.unpack_from('>h', d, o)[0]
def _u16(d, o): return struct.unpack_from('>H', d, o)[0]


def parse(data):
    d = bytes(data)
    p = {'raw': d}
    p['base_time'], p['loop_cp'], p['finish_cp'], p['arm_cp'], p['n_cp'], p['hdr_pad'] = struct.unpack_from('>Hhhhhh', d, 0)
    cps = []
    for i in range(p['n_cp']):
        o = CP_BASE + i * CP_SIZE
        pos = struct.unpack_from('>3f', d, o); dr = struct.unpack_from('>3f', d, o + 0xC)
        r2, = struct.unpack_from('>I', d, o + 0x18)
        flags, t1, t2 = struct.unpack_from('>hhh', d, o + 0x1C)
        cross = list(struct.unpack_from('>20h', d, o + 0x22))
        dist, = struct.unpack_from('>f', d, o + 0x4C)
        cps.append(dict(pos=pos, dir=dr, r2=r2, flags=flags, time1=t1, time2=t2, cross=cross, dist=dist))
    p['cps'] = cps
    n_spine = _u16(d, ROUTE); n_br = d[ROUTE + 8]
    o = ROUTE + 16
    brs = []
    for i in range(n_br):
        flag, frm, fidx, to, _, tidx, cp, _, cnt = struct.unpack_from('>BbHbBHbBH', d, o + i * 16)
        brs.append(dict(alt=flag, from_path=frm, from_idx=fidx, to_path=to, to_idx=tidx, cp=cp, count=cnt,
                        rec=d[o + i * 16:o + i * 16 + 16]))
    o += 16 * n_br
    p['n_total'] = _u16(d, o); o += 2
    p['spine'] = [struct.unpack_from('>3h', d, o + 6 * i) for i in range(n_spine)]; o += 6 * n_spine
    for b in brs:
        b['points'] = [struct.unpack_from('>3h', d, o + 6 * i) for i in range(b['count'])]; o += 6 * b['count']
    p['branches'] = brs
    lanes = []
    for _ in range(4):
        cnt, u2, u3 = struct.unpack_from('>HBB', d, o); hdr = d[o:o + 8]; o += 8
        pts = [struct.unpack_from('>3hBB', d, o + 8 * i) for i in range(cnt)]; o += 8 * cnt
        lanes.append(dict(count=cnt, unk2=u2, unk3=u3, points=pts, hdr=hdr))
    p['lanes'] = lanes
    p['end'] = o
    return p


def build(p, clear_runtime=False):
    """Serialise a parsed path. With clear_runtime the load-time-recomputed fields are zeroed/-1."""
    out = bytearray(p['raw'][:ROUTE]) if not clear_runtime else bytearray(ROUTE)
    hdr = (p['base_time'], p['loop_cp'], p['finish_cp'], p['arm_cp'], p['n_cp'], p['hdr_pad'])
    if clear_runtime: hdr = (p['base_time'], -1, -1, -1, p['n_cp'], 0)
    struct.pack_into('>Hhhhhh', out, 0, *hdr)
    for i, c in enumerate(p['cps']):
        o = CP_BASE + i * CP_SIZE
        struct.pack_into('>3f3fIhhh', out, o, *c['pos'], *c['dir'], c['r2'], c['flags'], c['time1'], c['time2'])
        if clear_runtime:
            struct.pack_into('>20hhf', out, o + 0x22, *([-1] * 20), 0, 0.0)
    route = bytearray(16)
    struct.pack_into('>H', route, 0, len(p['spine'])); route[8] = len(p['branches'])
    if not clear_runtime: route = bytearray(p['raw'][ROUTE:ROUTE + 16])
    out += route
    for b in p['branches']:
        if clear_runtime:
            out += struct.pack('>BbHbBHbBHI', b['alt'], -1, 0, -1, 0, 0, -1, 0, len(b['points']), 0)
        else:
            out += b['rec']
    out += struct.pack('>H', len(p['spine']) + sum(len(b['points']) for b in p['branches']))
    for pt in p['spine']: out += struct.pack('>3h', *pt)
    for b in p['branches']:
        for pt in b['points']: out += struct.pack('>3h', *pt)
    for ln in p['lanes']:
        out += struct.pack('>HBBI', len(ln['points']), ln['unk2'], ln['unk3'], 0) if clear_runtime else ln['hdr']
        for pt in ln['points']: out += struct.pack('>3hBB', *pt)
    return bytes(out)


# --- load-time computations, reimplemented from Rush 2 code ------------------------------------------------------

def crossing(cp, pts):
    """func_80092BC4 / func_80092884: first index where the path crosses the checkpoint plane inside the radius
    (2D, x/z); falls back to the closest point."""
    n = len(pts); best = 3.4e38; best_i = 0; prev_side = 0; prev_d2 = 0.0
    i = 0; k = -1
    while True:
        if i == n: i = 0
        dx = pts[i][0] - cp['pos'][0]; dz = pts[i][2] - cp['pos'][2]
        side = -1 if dz * cp['dir'][2] + dx * cp['dir'][0] < 0 else 1
        d2 = dx * dx + dz * dz
        if d2 < best: best, best_i = d2, i
        if k >= 0 and d2 <= cp['r2'] and prev_d2 <= cp['r2'] and side != prev_side:
            return i
        k += 1; i += 1; prev_d2 = d2; prev_side = side
        if k >= n: return best_i


def crossing_branch(cp, pts):
    """func_80092A38: index of the first side change among points inside the radius, else -1."""
    first = True; prev = 0
    for i, pt in enumerate(pts):
        dx = pt[0] - cp['pos'][0]; dz = pt[2] - cp['pos'][2]
        if dx * dx + dz * dz <= cp['r2']:
            side = -1 if dz * cp['dir'][2] + dx * cp['dir'][0] < 0 else 1
            if first: prev, first = side, False
            elif side != prev: return i
    return -1


def nearest(p, pt, exclude):
    """Simplified func_80091E54: closest point on the spine or another branch (3D)."""
    best = (3.4e38, -1, 0)
    for i, q in enumerate(p['spine']):
        d = sum((a - b) ** 2 for a, b in zip(q, pt))
        if d < best[0]: best = (d, -1, i)
    for bi, b in enumerate(p['branches']):
        if bi == exclude: continue
        for i, q in enumerate(b['points']):
            d = sum((a - b2) ** 2 for a, b2 in zip(q, pt))
            if d < best[0]: best = (d, bi, i)
    return best[1], best[2]


def recompute(p):
    """Return the runtime fields the game derives at load: header indices, cross[] per checkpoint, branch links."""
    loop = finish = arm = -1
    for i, c in enumerate(p['cps']):
        if finish < 0 and c['flags'] & 1: finish = i
        if loop < 0 and c['flags'] & 4: loop = i
        if arm < 0 and c['flags'] & 2: arm = i
    hdr = (max(loop, 0), max(finish, 0), max(arm, 0))
    cross = []
    for c in p['cps']:
        row = [crossing(c, p['spine'])] + [crossing(c, [q[:3] for q in ln['points']]) for ln in p['lanes']]
        row += [crossing_branch(c, b['points']) for b in p['branches']]
        cross.append(row)
    links = [(nearest(p, b['points'][0], bi), nearest(p, b['points'][-1], bi)) for bi, b in enumerate(p['branches'])]
    return dict(hdr=hdr, cross=cross, links=links)


def validate(p):
    """Structural checks. Returns a list of error strings (empty = OK)."""
    e = []
    if p['end'] != len(p['raw']): e.append('trailing/short data: parsed %#x of %#x' % (p['end'], len(p['raw'])))
    if not 1 <= p['n_cp'] <= CP_MAX: e.append('checkpoint count %d not in 1..10' % p['n_cp'])
    if len(p['branches']) > MAX_BRANCH: e.append('%d branches > 15' % len(p['branches']))
    tot = len(p['spine']) + sum(b['count'] for b in p['branches'])
    if tot != p['n_total']: e.append('point total %d != %d' % (p['n_total'], tot))
    if len(p['spine']) < 2: e.append('spine too short')
    for bi, b in enumerate(p['branches']):
        for nm, path, idx in (('from', b['from_path'], b['from_idx']), ('to', b['to_path'], b['to_idx'])):
            if path >= len(p['branches']) or path < -1:
                e.append('branch %d %s path %d out of range' % (bi, nm, path)); continue
            lim = len(p['spine']) if path < 0 else p['branches'][path]['count']
            if not 0 <= idx < lim: e.append('branch %d %s idx %d out of range (%d)' % (bi, nm, idx, lim))
        if not -1 <= b['cp'] < p['n_cp']: e.append('branch %d checkpoint %d out of range' % (bi, b['cp']))
        if b['count'] < 2: e.append('branch %d has %d points' % (bi, b['count']))
    for li, ln in enumerate(p['lanes']):
        if ln['count'] < 2: e.append('lane %d has %d points' % (li, ln['count']))
    for i, c in enumerate(p['cps']):
        n = math.sqrt(sum(x * x for x in c['dir']))
        if abs(n - 1) > 0.02: e.append('checkpoint %d dir not unit (%.3f)' % (i, n))
    # Runtime-derived values recomputed at load (cross[], branch from/to/cp) must land in range.
    rc = recompute(p)
    for i, row in enumerate(rc['cross']):
        for j, v in enumerate(row):
            lim = len(p['spine']) if j == 0 else (p['lanes'][j - 1]['count'] if j < 5 else p['branches'][j - 5]['count'])
            if not -1 <= v < lim: e.append('checkpoint %d cross[%d]=%d out of range' % (i, j, v))
    for bi, (f, t) in enumerate(rc['links']):
        for path, idx in (f, t):
            lim = len(p['spine']) if path < 0 else p['branches'][path]['count']
            if not 0 <= idx < lim: e.append('branch %d recomputed link %s out of range' % (bi, (path, idx)))
    return e


def warnings(p):
    """Non-fatal observations (stale editor values that the loader overwrites, defaults that apply)."""
    w = []
    if not any(c['flags'] & 1 for c in p['cps']): w.append('no flag-1 checkpoint: finish defaults to cp 0')
    if not any(c['flags'] & 4 for c in p['cps']): w.append('no flag-4 checkpoint: loop start defaults to cp 0')
    if not any(c['flags'] & 2 for c in p['cps']): w.append('no flag-2 checkpoint: arm defaults to cp 0')
    for i, c in enumerate(p['cps']):
        for j, v in enumerate(c['cross'][:5 + len(p['branches'])]):
            lim = len(p['spine']) if j == 0 else (p['lanes'][j - 1]['count'] if j < 5 else p['branches'][j - 5]['count'])
            if not -1 <= v < lim: w.append('stale stored cp%d cross[%d]=%d' % (i, j, v))
    return w


def convert(path_2049):
    """Rush 2049 path file -> Rush 2 path file. The formats and loaders are identical and every derived field is
    recomputed at load, so the bytes are returned unchanged after validation."""
    p = parse(path_2049)
    err = validate(p)
    if err: raise ValueError('; '.join(err))
    return bytes(path_2049)


# Rush 2049 file -> (track index 0..18, direction). Track index = file - 158 (forward) or file - 177 (backward).
FILES_2049 = {158 + i: ('race%d' % (i + 1), 'fwd') for i in range(6)}
FILES_2049.update({177 + i: ('race%d' % (i + 1), 'back') for i in range(6)})
FILES_2049.update({164: ('dm1', 'fwd'), 165: ('dm6', 'fwd'), 166: ('dm5', 'fwd'), 167: ('dm8', 'fwd'), 168: ('dm3', 'fwd'),
                   169: ('dm7', 'fwd'), 170: ('dm4', 'fwd'), 171: ('dm2', 'fwd'), 172: ('stunt4', 'fwd'),
                   173: ('stunt3', 'fwd'), 174: ('stunt2', 'fwd'), 175: ('stunt1', 'fwd'), 176: ('obstacle1', 'fwd')})


def summary(name, p, rc):
    st = [c['cross'] for c in p['cps']]
    same_cross = sum(1 for a, b in zip(st, rc['cross']) for x, y in zip(a, b) if x == y)
    tot_cross = sum(len(r) for r in rc['cross'])
    same_link = sum(1 for b, (f, t) in zip(p['branches'], rc['links'])
                    if (b['from_path'], b['from_idx']) == f and (b['to_path'], b['to_idx']) == t)
    hdr_ok = rc['hdr'] == (p['loop_cp'], p['finish_cp'], p['arm_cp'])
    flags = ''.join('%d' % c['flags'] for c in p['cps'])
    spd = [q[3] for ln in p['lanes'] for q in ln['points']]
    beh = sorted(set(q[4] for ln in p['lanes'] for q in ln['points']))
    return ('%-14s cp=%d flags=%-6s spine=%4d br=%2d lanes=%s speed=%d-%d mph beh=%s | stored==recomputed: '
            'hdr %s, cross %d/%d, links %d/%d' % (
                name, p['n_cp'], flags, len(p['spine']), len(p['branches']), [l['count'] for l in p['lanes']],
                min(spd), max(spd), beh, 'yes' if hdr_ok else 'no', same_cross, tot_cross, same_link,
                len(p['branches'])))


def selfcheck():
    import roms
    ok = True
    r2 = roms.Rush2()
    names = roms.Rush2.__dict__.get('TRACKS') or ['VEGAS', 'NYONE', 'HAWAII', 'NYTWO', 'ALCATRAZ', 'LA', 'SEATTLE',
                                                  'HALFPIPE', 'CRASH', 'PIPE', 'ATARI', 'STUNT1']
    print('Rush 2 (asset 0x57+t forward, 0x63+t backward)')
    for i in range(24):
        d = r2.asset(0x57 + i); p = parse(d); e = validate(p)
        ok &= not e
        print('  %#04x %s' % (0x57 + i, summary(names[i % 12] + ('/b' if i >= 12 else ''), p, recompute(p))),
              ('ERR ' + '; '.join(e)) if e else 'OK', '; '.join(warnings(p)))
        assert build(p) == d
    q = roms.Rush2049()
    print('Rush 2049 (files 158-182)')
    for k in range(158, 183):
        d = q.file(k); p = parse(d); e = validate(p)
        ok &= not e
        nm = '%s/%s' % FILES_2049[k]
        print('  %3d  %s' % (k, summary(nm, p, recompute(p))), ('ERR ' + '; '.join(e)) if e else 'OK',
              '; '.join(warnings(p)))
        if k in FILES_2049 and FILES_2049[k][0].startswith('race'):
            assert convert(d) == d
            # round trip of the structural part
            assert parse(build(p, clear_runtime=True))['spine'] == p['spine']
    print('ALL OK' if ok else 'ERRORS FOUND')
    return ok


if __name__ == '__main__':
    sys.exit(0 if selfcheck() else 1)
