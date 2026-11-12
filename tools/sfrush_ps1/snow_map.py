"""How the PlayStation SF Rush recolors its tracks for snow and night, from its own textures (docs/sfrush_ps1.md#snow-and-night).

The snow (S) and night (N) page and CLUT sets load at the day set's VRAM places, so every texel a track face shows by
day has a snow and a night texel at the same (page, CLUT, u, v). This pairs them over every road and scenery face of
tracks 1-4 (each face's texels inside its UV polygon, with the face's tilt) and asks whether a lookup table keyed on
the day color (optionally plus whether the face looks up) predicts the other condition's color: the question behind
recoloring the N64 track select dioramas for SNOW / NIGHT versions (docs/sfrush_ps1.md#track-select-diorama).

  python snow_map.py check [S|N] [--bits B]   consistency report: per-bin spread, LUT error against simple models,
                                              leave-one-track-out error, by face tilt and by day-color class
  python snow_map.py lut [S|N] OUT.json [--bits B] [--up]   write the table (all four tracks)

SFRUSH_PS1_DISC points at the disc (ps1.py).
"""
import json, os, sys
import numpy as np
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ps1 import Disc
from disps1 import Exe
import vram, track


def vram_array(disc, e, t, variant):
    return np.array(vram.Vram(disc, e, t, variant).v, dtype=np.uint16).reshape(512, 1024)


def decode_page(v, base, cid):
    """256x256 RGB (uint8) and opaque mask of the 4-bit page at base drawn with CLUT id cid."""
    tx, ty = base
    words = v[ty:ty + 256, tx:tx + 64].astype(np.uint32)                       # 256 rows x 64 halfwords
    idx = np.stack([(words >> (4 * k)) & 15 for k in range(4)], axis=2).reshape(256, 256)
    cx, cy = (cid & 63) * 16, cid >> 6
    clut = v[cy, cx:cx + 16].astype(np.uint32)
    c = clut[idx]
    rgb = np.stack([(c & 31) << 3, ((c >> 5) & 31) << 3, ((c >> 10) & 31) << 3], axis=2)
    rgb = rgb | (rgb >> 5)
    return rgb.astype(np.uint8), c != 0


def face_texels(uv, voff):
    """(rows, cols) of the texel centers inside the face's UV polygon (convex, 3 or 4 corners)."""
    us = np.array([p[0] for p in uv], float); vs = np.array([p[1] for p in uv], float)
    u0, u1 = int(np.floor(us.min())), int(np.ceil(us.max())); v0, v1 = int(np.floor(vs.min())), int(np.ceil(vs.max()))
    if u1 - u0 > 256 or v1 - v0 > 256:
        return None
    gu, gv = np.meshgrid(np.arange(u0, u1 + 1) + 0.5, np.arange(v0, v1 + 1) + 0.5)
    n = len(uv); inside_pos = np.ones(gu.shape, bool); inside_neg = np.ones(gu.shape, bool)
    for k in range(n):
        ax, ay = us[k], vs[k]; bx, by = us[(k + 1) % n], vs[(k + 1) % n]
        cr = (bx - ax) * (gv - ay) - (by - ay) * (gu - ax)
        inside_pos &= cr >= -1e-6; inside_neg &= cr <= 1e-6
    m = inside_pos | inside_neg
    if not m.any():
        return None
    cols = gu[m].astype(int) & 255
    rows = (gv[m].astype(int) + voff) & 255
    return rows, cols


def pairs(variant):
    """{track: (day rgb N x 3, other rgb N x 3, up N bool)} over every opaque texel of every face. Cached in
    tmp/snow_map_<variant>.npz (the disc doesn't change; delete it after changing the pairing)."""
    cache = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tmp', 'snow_map_%s.npz' % variant)
    if os.path.exists(cache):
        z = np.load(cache)
        return {t: (z['d%d' % t], z['o%d' % t], z['u%d' % t]) for t in (1, 2, 3, 4)}
    out = _pairs(variant)
    os.makedirs(os.path.dirname(cache), exist_ok=True)
    np.savez_compressed(cache, **{'%s%d' % (n, t): out[t][i] for t in out for i, n in enumerate('dou')})
    return out


def _pairs(variant):
    disc = Disc(); e = Exe(); out = {}
    cluts = vram.clut_ids(e)
    for t in (1, 2, 3, 4):
        vd, vo = vram_array(disc, e, t, ''), vram_array(disc, e, t, variant)
        table = vram.tex_table(e, t); pages = {}
        D, O, U = [], [], []; cover = {}
        for name, verts, faces in track.track_meshes(disc, t):
            for f in faces:
                if not 0 <= f['tex'] < len(table):
                    continue
                tx, ty, _, voff = table[f['tex']][:4]
                base = (tx & ~63, ty & 256); cid = cluts[f['clut']] if 0 <= f['clut'] < len(cluts) else 0
                key = (base, cid)
                if key not in pages:
                    pages[key] = decode_page(vd, base, cid) + decode_page(vo, base, cid)
                ring = [k for k in range(4) if k == 0 or f['idx'][k] != f['idx'][k - 1]]
                if len(ring) == 4 and f['idx'][3] == f['idx'][0]:
                    ring = ring[:3]
                if len(ring) < 3:
                    continue
                rc = face_texels([f['uv'][k] for k in ring], voff)
                if rc is None:
                    continue
                r, c = rc
                # Tilt: ps1 z is up (track.game_pos); a face is "up" when its normal is within 45 degrees of vertical.
                p = [np.array(verts[f['idx'][k]], float) for k in ring]
                nrm = np.cross(p[1] - p[0], p[2] - p[0]); ln = np.linalg.norm(nrm)
                up = bool(ln > 0 and abs(nrm[2]) / ln > 0.707)
                # Each texel counts once per tilt, however many faces tile it.
                cover.setdefault((key, up), np.zeros((256, 256), bool))[r, c] = True
        for (key, up), m in cover.items():
            dr, dm, orr, om = pages[key]
            m = m & dm & om
            D.append(dr[m]); O.append(orr[m]); U.append(np.full(int(m.sum()), up))
        out[t] = (np.concatenate(D).astype(float), np.concatenate(O).astype(float), np.concatenate(U))
    return out


def bin_of(rgb, bits, up=None):
    q = (rgb.astype(int) >> (8 - bits))
    k = (q[:, 0] << (2 * bits)) | (q[:, 1] << bits) | q[:, 2]
    return k if up is None else k * 2 + up.astype(int)


def fit(d, o, keys):
    """Mean other color per key (dict key -> rgb), and pixel counts."""
    uk, inv, cnt = np.unique(keys, return_inverse=True, return_counts=True)
    sums = np.zeros((len(uk), 3))
    np.add.at(sums, inv, o)
    return dict(zip(uk.tolist(), (sums / cnt[:, None]).tolist())), dict(zip(uk.tolist(), cnt.tolist()))


def err(a, b):
    return np.sqrt(((a - b) ** 2).sum(axis=1))


def predict(lut, keys, fallback):
    out = np.array(fallback, float).copy()
    ks = np.array(sorted(lut)); vs = np.array([lut[k] for k in ks.tolist()])
    pos = np.clip(np.searchsorted(ks, keys), 0, len(ks) - 1)
    hit = ks[pos] == keys
    out[hit] = vs[pos[hit]]
    return out


def seen(lut, keys):
    ks = np.array(sorted(lut)); pos = np.clip(np.searchsorted(ks, keys), 0, len(ks) - 1)
    return ks[pos] == keys


def classify(d):
    """Day-color classes for the report: green (grass, trees), grey (road, concrete), brown/tan, blue (water, sky), other."""
    r, g, b = d[:, 0], d[:, 1], d[:, 2]
    mx, mn = d.max(axis=1), d.min(axis=1); sat = (mx - mn) / np.maximum(mx, 1)
    cls = np.full(len(d), 'other', object)
    cls[sat < 0.15] = 'grey'
    cls[(sat >= 0.15) & (g >= r) & (g >= b)] = 'green'
    cls[(sat >= 0.15) & (r > g) & (g > b)] = 'brown/tan'
    cls[(sat >= 0.15) & (b > r) & (b >= g)] = 'blue'
    return cls


def check(variant, bits):
    data = pairs(variant)
    gains = np.array([1.08, 1.09, 1.27]) if variant == 'S' else np.array([0.49, 0.49, 0.67])
    D = np.concatenate([v[0] for v in data.values()]); O = np.concatenate([v[1] for v in data.values()])
    U = np.concatenate([v[2] for v in data.values()])
    print('%s: %d texel pairs, %.0f%% on up-facing faces; unchanged texels %.0f%%' % (
        'snow' if variant == 'S' else 'night', len(D), 100 * U.mean(), 100 * (err(D, O) < 1).mean()))
    print('error = RGB distance (0-441) of the prediction from the PS1\'s own %s texel' % ('snow' if variant == 'S' else 'night'))
    print('  model                         mean   median  <24   <48')

    def row(label, pred, mask=None):
        e_ = err(pred, O) if mask is None else err(pred[mask], O[mask])
        print('  %-28s %6.1f %7.1f %4.0f%% %4.0f%%' % (label, e_.mean(), np.median(e_), 100 * (e_ < 24).mean(), 100 * (e_ < 48).mean()))

    row('no change', D)
    row('channel gains', np.clip(D * gains, 0, 255))
    for b in sorted({bits, 3, 4, 5}):
        lut, _ = fit(D, O, bin_of(D, b)); row('LUT %d bits (in-sample)' % b, predict(lut, bin_of(D, b), D))
        lutu, _ = fit(D, O, bin_of(D, b, U)); row('LUT %d bits + up (in-sample)' % b, predict(lutu, bin_of(D, b, U), D))
    # Spread inside a bin: how far each pixel is from its bin's mean (the floor any color-only table hits).
    keys = bin_of(D, bits); lut, cnt = fit(D, O, keys)
    pred = predict(lut, keys, D); e_ = err(pred, O)
    uk, inv = np.unique(keys, return_inverse=True)
    binerr = np.zeros(len(uk)); np.add.at(binerr, inv, e_); binerr /= np.bincount(inv)
    w = np.bincount(inv)
    order = np.argsort(-w)[:12]
    print('\nbusiest %d-bit day bins: day color -> mean %s color, pixel share, mean spread' % (bits, 'snow' if variant == 'S' else 'night'))
    for i in order:
        k = int(uk[i]); q = ((k >> (2 * bits)) & ((1 << bits) - 1), (k >> bits) & ((1 << bits) - 1), k & ((1 << bits) - 1))
        day = tuple(int((x + 0.5) * (256 >> bits)) for x in q)
        print('  %-15s -> %-15s %5.1f%%  %5.1f' % (day, tuple(int(x) for x in lut[k]), 100 * w[i] / len(D), binerr[i]))
    print('\nleave one track out (LUT %d bits from the other three, gains where a bin is unseen):' % bits)
    print('  track  model          mean  median  <24   <48   unseen')
    for t in data:
        tr = [x for x in data if x != t]
        d_tr = np.concatenate([data[x][0] for x in tr]); o_tr = np.concatenate([data[x][1] for x in tr])
        u_tr = np.concatenate([data[x][2] for x in tr])
        d, o, u = data[t]
        for label, kt, kk in (('color', bin_of(d_tr, bits), bin_of(d, bits)), ('color+up', bin_of(d_tr, bits, u_tr), bin_of(d, bits, u))):
            lut, _ = fit(d_tr, o_tr, kt)
            pred = predict(lut, kk, np.clip(d * gains, 0, 255)); e_ = err(pred, o)
            unseen = 1 - seen(lut, kk).mean()
            print('  %d      %-12s %5.1f %6.1f %4.0f%% %4.0f%%  %4.0f%%' % (t, label, e_.mean(), np.median(e_), 100 * (e_ < 24).mean(), 100 * (e_ < 48).mean(), 100 * unseen))
        ge = err(np.clip(d * gains, 0, 255), o)
        print('  %d      %-12s %5.1f %6.1f %4.0f%% %4.0f%%' % (t, 'gains', ge.mean(), np.median(ge), 100 * (ge < 24).mean(), 100 * (ge < 48).mean()))
    print('\nby day-color class and tilt: mean luminance day -> %s, LUT(color+up) in-sample error' % ('snow' if variant == 'S' else 'night'))
    lum = lambda x: x @ np.array([0.299, 0.587, 0.114])
    lutu, _ = fit(D, O, bin_of(D, bits, U)); pu = predict(lutu, bin_of(D, bits, U), D); eu = err(pu, O)
    cls = classify(D)
    for c in ('green', 'grey', 'brown/tan', 'blue', 'other'):
        for up in (True, False):
            m = (cls == c) & (U == up)
            if m.sum() < 200:
                continue
            whiter = (lum(O[m]) - lum(D[m]) > 40).mean()
            print('  %-10s %-5s %5.1f%% of pixels  lum %5.1f -> %5.1f  turned much lighter %3.0f%%  err %5.1f' % (
                c, 'up' if up else 'wall', 100 * m.mean(), lum(D[m]).mean(), lum(O[m]).mean(), 100 * whiter, eu[m].mean()))


def write_lut(variant, out, bits, use_up):
    data = pairs(variant)
    D = np.concatenate([v[0] for v in data.values()]); O = np.concatenate([v[1] for v in data.values()])
    U = np.concatenate([v[2] for v in data.values()])
    lut, cnt = fit(D, O, bin_of(D, bits, U if use_up else None))
    json.dump({'variant': variant, 'bits': bits, 'up': use_up,
               'bins': {str(k): [round(x, 1) for x in v] + [cnt[k]] for k, v in lut.items()}}, open(out, 'w'))
    print('%d bins -> %s' % (len(lut), out))


if __name__ == '__main__':
    a = sys.argv[1:]
    bits = int(a[a.index('--bits') + 1]) if '--bits' in a else 4
    variant = a[1] if len(a) > 1 and a[1] in ('S', 'N') else 'S'
    if a[0] == 'check':
        check(variant, bits)
    elif a[0] == 'lut':
        write_lut(variant, a[2], bits, '--up' in a)
