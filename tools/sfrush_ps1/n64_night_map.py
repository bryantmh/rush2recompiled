"""Maps the N64 SF Rush building textures onto the PlayStation's own textures of the same buildings, so the N64 NIGHT
versions wear the PS1's night art (docs/sfrush_ps1.md#n64-night-versions, lit windows).

The PS1 remade three of the N64 courses (its tracks 1-3: Golden Gate, Embarcadero, Market) with its own textures and
a hand painted night set of them, and the N64 courses the PS1 lacks (Downtown, The Heights, Sunset) are built from
largely the same N64 textures. So for each N64 wall texture this lines the N64 course up with the PS1's
(n64_census.fit2d), projects every texel of every wall triangle onto the PS1 face at the same place, fits the affine
map N64 texel -> PS1 page texel each face gives, and keeps the map whose PS1 DAY texels look most like the N64
texture (normalized cross-correlation, refined by a small shift search). The result is per texture a page, a CLUT and
an affine: no PS1 pixels. The PS1 install (src/sfrush_ps1/ps1_track.cpp n64_night_textures) samples the night set
through it on the player's own disc, and the night conversion (src/rush1/track1_convert.cpp night_track) puts the
result in place of the texture.

  python n64_night_map.py scan COURSE PS1_TRACK OUT.npz [--all]  one course's candidates (Market 3, Embarcadero 2,
                                                                   Golden_Gate 1; a few minutes each); --all: every
                                                                   texture, not only the buildings' (the snow maps)
  python n64_night_map.py pick OUT.json SHEETDIR A.npz B.npz ...   the best map per texture key, contact sheets
                                                                   (N64 day | PS1 day | PS1 night, mapped)
  python n64_night_map.py search MAP.json BELOW [COURSE ...]     building textures of the courses (default the four
                                                                   the PS1 lacks) with no map scoring BELOW, looked for
                                                                   on every PS1 page (slow: ~1 min a texture; --shard I/N
                                                                   does every Nth from I, to run several at once)
  python n64_night_map.py publish MAP.json [REVIEWDIR]           writes the shipped map (MAP_OUT) with the entries
                                                                   that pass (ACCEPT) or the overrides accept; REVIEWDIR:
                                                                   sheets of the ones left for a look (N64 day | PS1 day |
                                                                   PS1 night, labeled score / color / key)
  python n64_night_map.py snowmaps MAP.json [REVIEWDIR]           the SNOW versions' maps of the other textures (from
                                                                   scan --all and pick) -> assets/sfrush_ps1/
                                                                   n64_snow_map.json; review sheets N64 day | PS1 day |
                                                                   PS1 snow (mark ... --snow records the review)
  python n64_night_map.py ground OUT.json SHEETDIR A.npz B.npz ... [--only KEY,KEY]  candidates for the noisy ground textures no map
                                                                   holds: the PS1 texture most of their faces land on
                                                                   (the PS1 drew its own grass, gravel, dirt); for
                                                                   review, mark OUT.json accept ... --snow
  python n64_night_map.py material OUT.json [SHEET.png] [--force KEY=REF,...]           candidates by material for the ground textures
                                                                   nothing maps: the nearest mapped ground texture's
                                                                   snow map; review, mark OUT.json accept ... --snow
  python n64_night_map.py snowcover [SHEET.png]                     the SNOW versions' share in PS1 snow art, and the
                                                                   largest textures still missing
  python n64_night_map.py snowclass COURSE PS1_TRACK OUT.json      every texture's PS1 snow: the share of its texels
                                                                   the PS1's snow set whitens where they land
  python n64_night_map.py snowpublish OUT.json A.json B.json C.json   those of the three courses -> OUT.json (texture
                                                                   key -> snow share, crops; research, not shipped)
  python n64_night_map.py cover OUT.json                           how many building textures of each N64 course
                                                                   the map covers

A map passes on its own when its day texels correlate (score, NCC) and its colors agree (color: median RGB distance of
the PS1's day texels from the N64's): the same art on both. A wrong map can correlate well (a grey wall's stripes on a
green one's); the PS1 retinted some art, so a right one can also differ in color. The rest are looked at by eye and
listed in assets/sfrush_ps1/n64_night_overrides.json with `mark`: "maps" (key -> the map looked at, shipped as it is
whatever the candidates later hold; a map written by hand goes here too) and "reject" (key -> the affine of the map
looked at, never shipped while the candidates hold that map).

  python n64_night_map.py mark MAP.json accept|reject KEYPREFIX ...   record the review of these maps (key prefixes as
                                                                   the review sheets print them)
  python n64_night_map.py windows KEYS.json OUTDIR                 each texture (a JSON list of keys) enlarged on a
                                                                   texel grid (labels every 4), to read windows off
  python n64_night_map.py light KEYPREFIX X,Y,W,H ...              lights those windows (texel rects, each lit whole:
                                                                   lit windows are rectangles) of that texture (a hand
                                                                   map, for one the PS1 has no art for)
  python n64_night_map.py clip KEYPREFIX X,Y,W,H                   keeps a map's PS1 art to that rect of the texture
                                                                   (and its palette variants'); the rest keeps its own
                                                                   night colors

Hand maps (overrides "hand": key -> size and the lit glass as texel runs [x, y, w, 1, window]) light picked windows of
a texture the PS1 has no art for, in the PS1's own lit-window colors (the PS1 art's lit texels cluster at pale
(214, 220, 221) and warm (216, 206, 126); the install picks one per window, ps1_track.cpp n64_night_art); the rest of
the texture keeps its own night colors. Keep them sparse: about a quarter to a third of a texture's windows.

Texture key: FNV-1a 64 of u16 width, u16 height and the level-0 texels as RGBA5551 (big endian), as
track1_convert.cpp night_key computes it. SFRUSH_PS1_DISC points at the disc (ps1.py); the N64 course models are the
asset dump's (build/asset_dump/models/sfrush/tracks/<Course>/_placed.glb, Settings > Dump Textures).
"""
import io, json, os, struct, sys
import numpy as np
import cv2
from scipy.spatial import cKDTree
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE); sys.path.insert(0, os.path.join(HERE, '..'))
import glb_view as G
from PIL import Image

DUMP = 'build/asset_dump/models/sfrush/tracks'
OVERRIDES = os.path.join(HERE, '..', '..', 'assets', 'sfrush_ps1', 'n64_night_overrides.json')
TALL_M = 12 / 3.2808      # a building wall triangle: 12 ft high or more (as night_track's building rule)


def tex_key(rgba):
    """FNV-1a 64 of (u16 w, u16 h, RGBA5551 texels big endian), as night_key in track1_convert.cpp."""
    h, w = rgba.shape[:2]
    a = rgba.astype(np.uint32)
    e = ((a[..., 0] >> 3) << 11) | ((a[..., 1] >> 3) << 6) | ((a[..., 2] >> 3) << 1) | (a[..., 3] >= 128)
    data = struct.pack('>HH', w, h) + e.astype('>u2').tobytes()
    x = 0xcbf29ce484222325
    for b in data:
        x = ((x ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return '%016x' % x


def n64_course(name):
    g, bn = G.load(f'{DUMP}/{name}/_placed.glb')
    T, U, C, X = G.triangles(g, bn, ['TRACK'])
    texs = []
    for tt in g['textures']:
        im = g['images'][tt['source']]; bv = g['bufferViews'][im['bufferView']]; o = bv.get('byteOffset', 0)
        texs.append(np.asarray(Image.open(io.BytesIO(bn[o:o + bv['byteLength']])).convert('RGBA')))
    return T / 3.2808, U, X, texs, [t['name'] for t in g['textures']]


def building_textures(T, X, n):
    """Textures drawn mostly on walls (70% of their triangles), a third of those tall: night_track's building rule."""
    nrm = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]); ln = np.linalg.norm(nrm, axis=1)
    wall = (np.abs(nrm[:, 1]) < 0.3 * np.maximum(ln, 1e-12)) & (ln > 1e-9)
    tall = wall & (np.ptp(T[:, :, 1], axis=1) >= TALL_M)
    out = set()
    for k in range(n):
        s = X == k
        if s.sum() and wall[s].sum() >= 0.7 * s.sum() and tall[s].sum() >= 0.3 * wall[s].sum():
            out.add(k)
    return out


def ps1_track(t):
    from ps1 import Disc; from disps1 import Exe
    import vram, track
    from snow_map import vram_array, decode_page
    disc = Disc(); e = Exe()
    vd, vn, vs = vram_array(disc, e, t, ''), vram_array(disc, e, t, 'N'), vram_array(disc, e, t, 'S')
    table = vram.tex_table(e, t); cluts = vram.clut_ids(e)
    PT, PUV, PK, keys = [], [], [], {}
    for name, verts, faces in track.track_meshes(disc, t):
        for f in faces:
            if not 0 <= f['tex'] < len(table): continue
            tx, ty, _, voff = table[f['tex']][:4]
            base = (tx & ~63, ty & 256); cid = cluts[f['clut']] if 0 <= f['clut'] < len(cluts) else 0
            k = keys.setdefault((base, cid), len(keys))
            ring = [i for i in range(4) if i == 0 or f['idx'][i] != f['idx'][i - 1]]
            if len(ring) == 4 and f['idx'][3] == f['idx'][0]: ring = ring[:3]
            if len(ring) < 3: continue
            # Vertex k takes the stored UV pair k ^ 1 (ps1_track.cpp read_chunk: the renderer's POLY_FT4 order).
            uv = {i: (f['uv'][i ^ 1][0] & 255, (f['uv'][i ^ 1][1] + voff) & 255) for i in ring}
            for tr in [(ring[0], ring[1], ring[2])] + ([(ring[0], ring[2], ring[3])] if len(ring) == 4 else []):
                PT.append([track.to_gltf(verts[f['idx'][i]]) for i in tr]); PUV.append([uv[i] for i in tr]); PK.append(k)
    pages = {k: (b, c, decode_page(vd, b, c), decode_page(vn, b, c), decode_page(vs, b, c)) for (b, c), k in keys.items()}
    return np.array(PT), np.array(PUV, float), np.array(PK), pages


def scan(course, t, out, snow=False, every=False):
    """snow: every textured triangle, and per texture the PS1 day and snow texels its texels land on (snowclass)."""
    from n64_census import fit2d, sample
    rng = np.random.default_rng(1)
    PT, PUV, PK, pages = ps1_track(t)
    NT, NU, NX, texs, names = n64_course(course)
    build = building_textures(NT, NX, len(texs))
    print(f'{course}: PS1 track {t} {len(PT)} triangles, N64 {len(NT)} triangles, {len(build)} building textures', flush=True)
    PP, _ = sample(PT, 2.0, rng); NP, _ = sample(NT, 2.0, rng)
    sub = NP[rng.choice(len(NP), min(len(NP), 6000), replace=False)][:, [0, 2]]
    err, mir, R, s, tt = fit2d(sub, PP[:, [0, 2]], cKDTree(PP[rng.choice(len(PP), min(len(PP), 20000), replace=False)][:, [0, 2]]))
    def xf(P):
        xz = s * ((P[..., [0, 2]] * np.array([mir, 1])) @ R.T) + tt
        return np.stack([xz[..., 0], P[..., 1] * s, xz[..., 1]], -1)
    NA = xf(NT)
    d, i = cKDTree(PP[:, [0, 2]]).query(xf(NP)[:, [0, 2]]); m = d < 2
    dy = np.median(PP[i[m], 1] - xf(NP)[m, 1]); NA[..., 1] += dy
    print(f'  fit median {err:.2f} m, mirror {mir}, dy {dy:.1f}', flush=True)
    PS_, PSo = sample(PT, 0.7, rng); pstree = cKDTree(PS_)
    pn = np.cross(PT[:, 1] - PT[:, 0], PT[:, 2] - PT[:, 0]); pn /= np.maximum(np.linalg.norm(pn, axis=1), 1e-9)[:, None]
    nn_ = np.cross(NA[:, 1] - NA[:, 0], NA[:, 2] - NA[:, 0]); nl = np.linalg.norm(nn_, axis=1); nn_ /= np.maximum(nl, 1e-9)[:, None]
    rows = []   # tex, page key, M00 M01 M10 M11 b0 b1 (local st -> page uv), samples, residual
    stats = {}   # snow: texture -> [samples, whitened]
    for j in np.where((np.isin(NX, list(build)) if not (snow or every) else NX >= 0) & (nl > 1e-6))[0]:
        tex = NX[j]; H, W = texs[tex].shape[:2]
        uv = NU[j] * [W, H]
        lo = np.floor(uv.min(0)).astype(int); hi = np.ceil(uv.max(0)).astype(int)
        if (hi - lo).prod() > 40000: continue
        gs, gt = np.meshgrid(np.arange(lo[0], hi[0]) + 0.5, np.arange(lo[1], hi[1]) + 0.5)
        q = np.c_[gs.ravel(), gt.ravel()]
        a, b, c = uv
        den = (b[1] - c[1]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[1] - c[1])
        if abs(den) < 1e-9: continue
        w0 = ((b[1] - c[1]) * (q[:, 0] - c[0]) + (c[0] - b[0]) * (q[:, 1] - c[1])) / den
        w1 = ((c[1] - a[1]) * (q[:, 0] - c[0]) + (a[0] - c[0]) * (q[:, 1] - c[1])) / den
        ins = (w0 >= -1e-6) & (w1 >= -1e-6) & (1 - w0 - w1 >= -1e-6)
        if ins.sum() < 8: continue
        q = q[ins]; X = np.c_[w0[ins], w1[ins], 1 - w0[ins] - w1[ins]] @ NA[j]
        near = pstree.query_ball_point(X[::max(1, len(X) // 64)], 2.0)
        cand = np.unique(PSo[np.concatenate([np.array(n, int) for n in near])])
        cand = cand[np.abs(pn[cand] @ nn_[j]) > 0.85] if len(cand) else cand
        if not len(cand): continue
        A = PT[cand]
        sd = np.einsum('kmd,md->km', X[:, None, :] - A[None, :, 0], pn[cand])
        Pp = X[:, None, :] - sd[..., None] * pn[cand][None]
        v0 = A[:, 1] - A[:, 0]; v1 = A[:, 2] - A[:, 0]; v2 = Pp - A[None, :, 0]
        d00 = (v0 * v0).sum(1); d01 = (v0 * v1).sum(1); d11 = (v1 * v1).sum(1)
        d20 = (v2 * v0[None]).sum(2); d21 = (v2 * v1[None]).sum(2)
        dn = d00 * d11 - d01 * d01; dn = np.where(np.abs(dn) < 1e-12, 1e-12, dn)
        bv = (d11 * d20 - d01 * d21) / dn; bw = (d00 * d21 - d01 * d20) / dn; bu = 1 - bv - bw
        score = np.where((bu > -0.02) & (bv > -0.02) & (bw > -0.02), np.abs(sd), np.inf)
        best = score.argmin(1); ar = np.arange(len(X)); ok = score[ar, best] < 1.5
        if ok.sum() < 8: continue
        if snow:
            # Each sample on its own face: the PS1 day and snow texels there, and whether the snow whitened it.
            kk = cand[best[ok]]
            B3 = np.c_[bu[ar, best], bv[ar, best], bw[ar, best]][ok]
            puv = np.floor(np.einsum('kc,kcd->kd', B3, PUV[kk])).astype(int) & 255
            pkk = PK[kk]
            day = np.array([pages[int(p)][2][0][v, u] for p, (u, v) in zip(pkk, puv)], float)
            sn = np.array([pages[int(p)][4][0][v, u] for p, (u, v) in zip(pkk, puv)], float)
            ld, ls = day @ [0.299, 0.587, 0.114], sn @ [0.299, 0.587, 0.114]
            white = (ls >= 150) & (ls - ld >= 30) & (sn.max(1) - sn.min(1) <= 60)
            st_ = stats.setdefault(int(tex), [0, 0]); st_[0] += len(white); st_[1] += int(white.sum())
            continue
        # One PS1 face's samples (the face most of them land on): its UV map is one affine.
        k = cand[best[ok]]
        top = np.bincount(np.searchsorted(cand, k)).argmax(); sel = k == cand[top]
        if sel.sum() < 8: continue
        B3 = np.c_[bu[ar, best], bv[ar, best], bw[ar, best]][ok][sel]
        puv = np.einsum('kc,kcd->kd', B3, PUV[k[sel]])
        st = q[ok][sel]
        # Local texel space: the tile of the texture the face's samples center on.
        origin = np.floor(st.mean(0) / [W, H]) * [W, H]
        D = np.c_[st - origin, np.ones(len(st))]
        sol, res, *_ = np.linalg.lstsq(D, puv, rcond=None)
        r = float(np.sqrt(((D @ sol - puv) ** 2).sum(1).mean()))
        rows.append([tex, PK[k[sel][0]], sol[0, 0], sol[1, 0], sol[0, 1], sol[1, 1], sol[2, 0], sol[2, 1], sel.sum(), r])
    keys = [tex_key(texs[k]) for k in range(len(texs))]
    if snow:
        json.dump({keys[k]: v for k, v in stats.items()}, open(out, 'w'))
        print(f'  {len(stats)} textures sampled -> {out}', flush=True)
        return
    rows = np.array(rows)
    pk = sorted(pages, key=lambda k: k)
    np.savez_compressed(out, rows=rows, keys=np.array(keys), names=np.array(names), build=np.array(sorted(build)),
                        track=t, course=course,
                        page_base=np.array([pages[k][0] for k in pk]), page_clut=np.array([pages[k][1] for k in pk]))
    print(f'  {len(rows)} face maps over {len(set(rows[:, 0].astype(int))) if len(rows) else 0} textures -> {out}', flush=True)


def snow_publish(out_path, files):
    """Each N64 texture's snow on the PS1 (the share of its sampled texels the snow set whitens, over the three
    courses), with its crop keys: research for snowing the textures no map covers (not shipped: the SNOW versions
    show the PS1's art only, user 2026-11-14)."""
    tot = {}
    for f in files:
        for k, (n, w) in json.load(open(f)).items():
            t = tot.setdefault(k, [0, 0]); t[0] += n; t[1] += w
    imgs, names = all_textures()
    out = {}
    for k, (n, w) in sorted(tot.items()):
        if n < 64 or k not in imgs: continue
        H = imgs[k].shape[0]
        out[k] = dict(snow=round(w / n, 3), samples=n, name=names[k],
                      crops=[[tex_key(imgs[k][:h]), h] for h in range(16, H, 16)])
    json.dump(out, open(out_path, 'w'), indent=1)
    print(f'{len(out)} textures -> {out_path}')


def sample_page(page, M, b, W, H):
    """The page (rgb, mask) sampled at the affine's texel centers over W x H (nearest texel, page coordinates wrap)."""
    gs, gt = np.meshgrid(np.arange(W) + 0.5, np.arange(H) + 0.5)
    u = M[0, 0] * gs + M[0, 1] * gt + b[0]; v = M[1, 0] * gs + M[1, 1] * gt + b[1]
    ui = np.floor(u).astype(int) & 255; vi = np.floor(v).astype(int) & 255
    return page[0][vi, ui], page[1][vi, ui]


def ncc(a, b, m):
    if m.sum() < 16: return -1.0
    x = a[m] - a[m].mean(); y = b[m] - b[m].mean()
    d = np.sqrt((x * x).sum() * (y * y).sum())
    return float((x * y).sum() / d) if d > 1e-9 else -1.0


def gray(rgb):
    return rgb[..., :3].astype(float) @ [0.299, 0.587, 0.114]


def pick(out, sheets, files):
    from snow_map import vram_array, decode_page as decode
    cache_p, cache_c = {}, {}
    def decode_page(v, base, cid):
        k = (id(v), base, cid)
        if k not in cache_p: cache_p[k] = decode(v, base, cid)
        return cache_p[k]
    def course_textures(name):
        if name not in cache_c: cache_c[name] = n64_course(name)[3]
        return cache_c[name]
    from ps1 import Disc; from disps1 import Exe
    disc = Disc(); e = Exe()
    vr = {}
    best = {}   # key -> dict
    for f in files:
        z = np.load(f, allow_pickle=True)
        t = int(z['track']); course = str(z['course'])
        if t not in vr: vr[t] = (vram_array(disc, e, t, ''), vram_array(disc, e, t, 'N'))
        bases, clut_ids = z['page_base'], z['page_clut']
        texs = course_textures(course); names = z['names']
        keys = z['keys']; rows = z['rows']
        for tex in sorted(set(rows[:, 0].astype(int))):
            R_ = rows[rows[:, 0] == tex]
            img = texs[tex]; H, W = img.shape[:2]; g0 = gray(img); m0 = img[..., 3] >= 128
            def score(page, M, b):
                rgb, msk = sample_page(page, M, b, W, H)
                return ncc(g0, gray(rgb), m0 & msk) * min(1.0, msk.mean() / 0.9)
            # Each face's map as it is, and (the courses line up only to a few meters, so a face may have landed on
            # the PS1's next building) its scale and turn with the shift found anywhere on its page: the N64 texture
            # warped into page texels, matched over the page (OpenCV normalized cross-correlation).
            scored, groups = [], {}
            for r in R_:
                pk = int(r[1]); base = tuple(int(x) for x in bases[pk]); cid = int(clut_ids[pk])
                M = np.array([[r[2], r[3]], [r[4], r[5]]]); b = np.array([r[6], r[7]])
                if abs(np.linalg.det(M)) < 0.05 or abs(np.linalg.det(M)) > 64: continue
                page = decode_page(vr[t][0], base, cid)
                scored.append((score(page, M, b), base, cid, M, b))
                gk = (base, cid) + tuple(np.round(M.ravel() * 10).astype(int))
                groups[gk] = groups.get(gk, 0) + r[8]
            for gk, _ in sorted(groups.items(), key=lambda kv: -kv[1])[:8]:
                base, cid = gk[0], gk[1]; M = np.array(gk[2:], float).reshape(2, 2) / 10
                corners = np.array([[0, 0], [W, 0], [0, H], [W, H]], float) @ M.T
                lo = np.floor(corners.min(0)); size = np.ceil(corners.max(0) - lo).astype(int)
                if (size < 3).any() or (size > 200).any(): continue
                A = np.c_[M, -lo]
                tmpl = cv2.warpAffine(g0.astype(np.float32), A, (int(size[0]), int(size[1])), flags=cv2.INTER_NEAREST)
                page = decode_page(vr[t][0], base, cid)
                pg = gray(page[0]).astype(np.float32)
                if tmpl.std() < 1: continue
                res = cv2.matchTemplate(pg, tmpl, cv2.TM_CCOEFF_NORMED)
                L = np.unravel_index(np.nanargmax(res), res.shape)
                b = np.array([L[1], L[0]], float) - lo
                scored.append((score(page, M, b), base, cid, M, b))
            scored.sort(key=lambda s_: -s_[0])
            for sc0, base, cid, M, b in scored[:3]:
                page = decode_page(vr[t][0], base, cid)
                top = (sc0, M, b)
                for dx in np.arange(-2, 2.01, 0.5):
                    for dy in np.arange(-2, 2.01, 0.5):
                        bb = b + [dx, dy]
                        sc = score(page, M, bb)
                        if sc > top[0]: top = (sc, M, bb)
                # Scale per page axis, about the texture's center.
                M1, b1 = top[1], top[2]; c = M1 @ [W / 2, H / 2] + b1
                for sx in (0.9, 0.95, 1.05, 1.1):
                    for sy in (0.9, 0.95, 1.0, 1.05, 1.1):
                        S = np.diag([sx, sy]); M2 = S @ M1; b2 = c - M2 @ [W / 2, H / 2]
                        sc = score(page, M2, b2)
                        if sc > top[0]: top = (sc, M2, b2)
                k = str(keys[tex])
                if k not in best or top[0] > best[k]['score']:
                    M, bb = top[1], top[2]
                    best[k] = dict(score=round(top[0], 3), track=t, page=list(base), clut=cid,
                                   affine=[round(float(x), 4) for x in (M[0, 0], M[0, 1], M[1, 0], M[1, 1], bb[0], bb[1])],
                                   size=[W, H], name=str(names[tex]), course=course)
        print(f'{course}: {len(best)} textures mapped so far', flush=True)
    json.dump(dict(sorted(best.items())), open(out, 'w'), indent=1)
    if sheets:
        os.makedirs(sheets, exist_ok=True)
        tiles = []
        for k, v in sorted(best.items(), key=lambda kv: -kv[1]['score']):
            t = v['track']; W, H = v['size']; a = v['affine']
            M = np.array([[a[0], a[1]], [a[2], a[3]]]); b = np.array(a[4:6])
            day = sample_page(decode_page(vr[t][0], tuple(v['page']), v['clut']), M, b, W, H)[0]
            night = sample_page(decode_page(vr[t][1], tuple(v['page']), v['clut']), M, b, W, H)[0]
            src = None
            for f in files:
                z = np.load(f, allow_pickle=True)
                if k in set(z['keys']):
                    src = course_textures(str(z['course']))[list(z['keys']).index(k)]; break
            sc = max(1, 64 // max(W, H))
            tile = Image.new('RGB', (W * sc * 3 + 8, H * sc + 12), (255, 0, 255))
            for c, im in enumerate((src[..., :3], day, night)):
                tile.paste(Image.fromarray(im.astype(np.uint8)).resize((W * sc, H * sc), Image.NEAREST), (c * (W * sc + 4), 12))
            from PIL import ImageDraw
            ImageDraw.Draw(tile).text((1, 0), f"{v['score']:.2f} {k[:6]} {v['name'][:18]}", fill=(255, 255, 255))
            tiles.append(tile)
        per = 24
        for s0 in range(0, len(tiles), per):
            T_ = tiles[s0:s0 + per]; w = max(x.width for x in T_)
            sheet = Image.new('RGB', (w * 2 + 6, sum(x.height + 4 for x in T_[::2])), (40, 40, 40))
            y = 0
            for i in range(0, len(T_), 2):
                sheet.paste(T_[i], (0, y))
                if i + 1 < len(T_): sheet.paste(T_[i + 1], (w + 6, y))
                y += max(T_[i].height, T_[i + 1].height if i + 1 < len(T_) else 0) + 4
            sheet.save(os.path.join(sheets, f'sheet_{s0 // per:02d}.png'))
    print(f'{len(best)} textures -> {out}')


def search(path, below, courses, shard=(0, 1)):
    """Building textures of `courses` with no map scoring `below` or better, looked for on every page and CLUT any
    face of PS1 tracks 1-4 draws with (the same art may sit on another building or track, Chinatown's too): the N64
    texture at 0.5, 1 and 2 page texels a texel on each axis, in all 8 turns and flips, matched over each page."""
    from snow_map import decode_page
    m = json.load(open(path)) if os.path.exists(path) else {}
    pages = []
    for t in (1, 2, 3, 4):
        PT, PUV, PK, pg = ps1_track(t)
        for k, (base, cid, day, night, snow) in pg.items():
            pages.append((t, base, cid, gray(day[0]).astype(np.float32), day))
    print(f'{len(pages)} PS1 pages', flush=True)
    todo = {}
    for course in courses:
        T, U, X, texs, names = n64_course(course)
        for k in building_textures(T, X, len(texs)):
            key = tex_key(texs[k])
            if key not in m or m[key]['score'] < below: todo[key] = (texs[k], names[k], course)
    todo = {k: v for i, (k, v) in enumerate(sorted(todo.items())) if i % shard[1] == shard[0]}
    print(f'{len(todo)} textures to look for', flush=True)
    turns = [np.array(a, float) for a in ([[1, 0], [0, 1]], [[-1, 0], [0, 1]], [[1, 0], [0, -1]], [[-1, 0], [0, -1]],
                                          [[0, 1], [1, 0]], [[0, -1], [1, 0]], [[0, 1], [-1, 0]], [[0, -1], [-1, 0]])]
    for n, (key, (img, name, course)) in enumerate(todo.items()):
        H, W = img.shape[:2]; g0 = gray(img); m0 = img[..., 3] >= 128
        if g0[m0].std() < 2 if m0.any() else True: continue
        top = (m[key]['score'] if key in m else -1.0, None)
        for sx in (0.5, 1, 2):
            for sy in (0.5, 1, 2):
                for R in turns:
                    M = R @ np.diag([sx, sy])
                    corners = np.array([[0, 0], [W, 0], [0, H], [W, H]], float) @ M.T
                    lo = np.floor(corners.min(0)); size = np.ceil(corners.max(0) - lo).astype(int)
                    if (size < 4).any() or (size > 255).any(): continue
                    tmpl = cv2.warpAffine(g0.astype(np.float32), np.c_[M, -lo], (int(size[0]), int(size[1])), flags=cv2.INTER_NEAREST)
                    for t, base, cid, pg, day in pages:
                        res = cv2.matchTemplate(pg, tmpl, cv2.TM_CCOEFF_NORMED)
                        v = float(np.nanmax(res))
                        if v <= top[0]: continue
                        L = np.unravel_index(np.nanargmax(res), res.shape)
                        b = np.array([L[1], L[0]], float) - lo
                        rgb, msk = sample_page(day, M, b, W, H)
                        sc = ncc(g0, gray(rgb), m0 & msk) * min(1.0, msk.mean() / 0.9)
                        if sc > top[0]:
                            top = (sc, dict(score=round(sc, 3), track=t, page=list(base), clut=cid,
                                            affine=[round(float(x), 4) for x in (M[0, 0], M[0, 1], M[1, 0], M[1, 1], b[0], b[1])],
                                            size=[W, H], name=str(name), course=course, found='search'))
        if top[1] is not None:
            m[key] = top[1]
        print(f'{n + 1}/{len(todo)} {name}: {top[0]:.2f}', flush=True)
        if n % 10 == 9: json.dump(dict(sorted(m.items())), open(path, 'w'), indent=1)
    json.dump(dict(sorted(m.items())), open(path, 'w'), indent=1)


LIT_LUMA = 110          # a PS1 night texel this bright (luma, 0-255) is a lit window (ps1_track.cpp n64_night_art)
VARIANT_MATCH = 0.9     # layout_match a palette variant needs (the house family da36b1ea: 0.92, its snow fringe)


def layout(img):
    """The texture's texels as color classes numbered by first appearance: equal for two TLUTs of one texture."""
    a = img.astype(np.uint32)
    c = ((a[..., 0] << 24) | (a[..., 1] << 16) | (a[..., 2] << 8) | a[..., 3]).ravel()
    _, first, inv = np.unique(c, return_index=True, return_inverse=True)
    order = np.argsort(np.argsort(first))
    return order[inv].reshape(img.shape[:2])


def layout_match(a, b):
    """How well one layout's classes predict the other's, both ways (1: the same texels, colors aside; a TLUT that
    gives two entries one color merges classes, so it isn't 1 then)."""
    def one(x, y):
        n = 0
        for v in np.unique(x):
            n += np.bincount(y[x == v]).max()
        return n / x.size
    return min(one(a.ravel(), b.ravel()), one(b.ravel(), a.ravel()))


MAP_OUT = os.path.join(HERE, '..', '..', 'assets', 'sfrush_ps1', 'n64_night_map.json')
COURSES = ('Downtown', 'The_Heights', 'Sunset', 'The_Rock', 'Market', 'Embarcadero', 'Golden_Gate')


def accept(score, color):
    return (score >= 0.6 and color <= 45) or (score >= 0.85 and color <= 60)


def publish(path, review=None):
    from snow_map import vram_array, decode_page
    from ps1 import Disc; from disps1 import Exe
    from PIL import ImageDraw
    m = json.load(open(path))
    ov = json.load(open(OVERRIDES)) if os.path.exists(OVERRIDES) else {}
    yes, no = ov.get('maps', {}), ov.get('reject', {})
    disc = Disc(); e = Exe(); vd, vn = {}, {}
    imgs, build, names_of = {}, set(), {}
    for c in COURSES:
        T, U, X, texs, names = n64_course(c)
        for t, nm in zip(texs, names):
            imgs.setdefault(tex_key(t), t); names_of.setdefault(tex_key(t), nm)
        build |= {tex_key(texs[k]) for k in building_textures(T, X, len(texs))}
    out, look = {}, []
    for k, v in sorted(m.items()):
        if k not in imgs or v['name'].startswith(('CHKPNT', 'FINISH')): continue
        t = v['track']
        if t not in vd: vd[t], vn[t] = vram_array(disc, e, t, ''), vram_array(disc, e, t, 'N')
        a = v['affine']; M = np.array([[a[0], a[1]], [a[2], a[3]]]); W, H = v['size']
        day, msk = sample_page(decode_page(vd[t], tuple(v['page']), v['clut']), M, np.array(a[4:6]), W, H)
        src = imgs[k]; mm = msk & (src[..., 3] >= 128)
        color = float(np.median(np.linalg.norm(src[..., :3][mm].astype(float) - day[mm].astype(float), axis=1))) if mm.any() else 999.0
        v = dict(v, color=round(color, 1))
        rejected = k in no and no[k] == v['affine']
        ok = not rejected and k not in yes and accept(v['score'], color)
        if ok: out[k] = {f: v[f] for f in ('track', 'page', 'clut', 'affine', 'size', 'name', 'score', 'color')}
        if not accept(v['score'], color) and v['score'] >= 0.45:
            night = sample_page(decode_page(vn[t], tuple(v['page']), v['clut']), M, np.array(a[4:6]), W, H)[0]
            look.append((k, v, src, day, night, 'Y' if k in yes and yes[k]['affine'] == v['affine'] else 'N' if rejected else '?'))
    for k, v in yes.items():
        out[k] = v
    for k, v in ov.get('hand', {}).items():
        if k not in out: out[k] = {f: v[f] for f in ('name', 'size', 'windows')}
    # Palette variants: a building texture with no map whose texels are laid out as a mapped one's (the same texture
    # with another TLUT) takes that map's lit windows only (variant_of: the PS1 install keeps the night texels that are
    # lit, LIT_LUMA, and leaves the rest to the texture's own night colors).
    variants = {}
    mapped = [k for k in out if k in imgs and 'windows' not in out[k]]
    canon = {k: layout(imgs[k]) for k in set(mapped) | (build - set(out))}
    for k in sorted(build - set(out)):
        if k not in imgs or k in no: continue
        best = (0.0, None)
        for b in mapped:
            if canon[b].shape != canon[k].shape: continue
            c = layout_match(canon[k], canon[b])
            if c > best[0]: best = (c, b)
        if best[0] < VARIANT_MATCH: continue
        v = out[best[1]]; t = v['track']
        if t not in vd: vd[t], vn[t] = vram_array(disc, e, t, ''), vram_array(disc, e, t, 'N')
        a = v['affine']; M = np.array([[a[0], a[1]], [a[2], a[3]]]); W, H = v['size']
        night, msk = sample_page(decode_page(vn[t], tuple(v['page']), v['clut']), M, np.array(a[4:6]), W, H)
        lit = msk & (gray(night) >= LIT_LUMA)
        if lit.mean() < 0.005: continue
        variants[k] = dict(variant_of=best[1], size=v['size'], name=str(names_of.get(k, '')), match=round(best[0], 3))
    out.update(variants)
    for k, v in out.items():
        c = ov.get('clip', {}).get(k) or ov.get('clip', {}).get(v.get('variant_of', ''))
        if c: v['clip'] = c
    # Crops: a list may load fewer rows than the texture is drawn with (a 48-row block drawn 64 rows tall, the rest
    # whatever TMEM held, as the dump's image shows it), so the conversion can only key the rows it loads. Each map
    # also carries the keys of its texture's top rows, every 16 (the install writes those rows as entries of their own).
    for k, v in out.items():
        if k in imgs:
            H = imgs[k].shape[0]
            v['crops'] = [[tex_key(imgs[k][:h]), h] for h in range(16, H, 16)]
    json.dump(out, open(MAP_OUT, 'w'), indent=1)
    print(f'{len(variants)} palette variants take a mapped texture\'s lit windows')
    print(f'{len(out)} maps -> {MAP_OUT}; {len(look)} left for a look')
    if review:
        os.makedirs(review, exist_ok=True)
        tiles = []
        for k, v, src, day, night, mark in sorted(look, key=lambda x: -x[1]['score']):
            W, H = v['size']; sc = max(1, 64 // max(W, H))
            tile = Image.new('RGB', (max(W * sc * 3 + 8, 150), H * sc + 12), (255, 0, 255) if mark == '?' else (0, 160, 0) if mark == 'Y' else (160, 0, 0))
            for c, im in enumerate((src[..., :3], day, night)):
                tile.paste(Image.fromarray(im.astype(np.uint8)).resize((W * sc, H * sc), Image.NEAREST), (c * (W * sc + 4), 12))
            ImageDraw.Draw(tile).text((1, 0), f"{v['score']:.2f} {v['color']:.0f} {k[:8]}", fill=(255, 255, 255))
            tiles.append(tile)
        per = 30
        for s0 in range(0, len(tiles), per):
            T_ = tiles[s0:s0 + per]; w = max(x.width for x in T_) + 6; cols = 3
            rows = [T_[i:i + cols] for i in range(0, len(T_), cols)]
            sheet = Image.new('RGB', (w * cols, sum(max(x.height for x in r) + 4 for r in rows)), (40, 40, 40)); y = 0
            for r in rows:
                for c, x in enumerate(r): sheet.paste(x, (c * w, y))
                y += max(x.height for x in r) + 4
            sheet.save(os.path.join(review, f'review_{s0 // per:02d}.png'))


def all_textures():
    imgs, names = {}, {}
    for c in COURSES:
        for t, nm in zip(*n64_course(c)[3:5]):
            imgs.setdefault(tex_key(t), t); names.setdefault(tex_key(t), nm)
    return imgs, names


def windows(keys_path, out):
    from PIL import ImageDraw
    os.makedirs(out, exist_ok=True)
    imgs, names = all_textures()
    for key in json.load(open(keys_path)):
        img = imgs[key]; H, W = img.shape[:2]; sc = max(6, 448 // max(W, H)); m = 18
        im = Image.new('RGB', (W * sc + m, H * sc + m), (20, 20, 20))
        im.paste(Image.fromarray(img[..., :3]).resize((W * sc, H * sc), Image.NEAREST), (m, m)); d = ImageDraw.Draw(im)
        for x in range(W + 1):
            d.line([(m + x * sc, m), (m + x * sc, m + H * sc)], fill=(255, 0, 255) if x % 4 == 0 else (90, 90, 90))
            if x % 4 == 0 and x < W: d.text((m + x * sc + 1, 2), str(x), fill=(255, 255, 0))
        for y in range(H + 1):
            d.line([(m, m + y * sc), (m + W * sc, m + y * sc)], fill=(255, 0, 255) if y % 4 == 0 else (90, 90, 90))
            if y % 4 == 0 and y < H: d.text((1, m + y * sc + 1), str(y), fill=(255, 255, 0))
        im.save(os.path.join(out, f'{key[:8]}_{names[key]}.png'))
        print(key[:8], names[key], f'{W}x{H}')


def light(prefix, rects_in):
    imgs, names = all_textures()
    keys = [k for k in imgs if k.startswith(prefix)]
    if len(keys) != 1:
        print(f'{prefix}: {len(keys)} keys match'); return
    k = keys[0]; img = imgs[k]; rects = []
    for wi, (x0, y0, w, h) in enumerate(rects_in):
        for y in range(y0, y0 + h):
            rects.append([x0, y, w, 1, wi])
    ov = json.load(open(OVERRIDES)) if os.path.exists(OVERRIDES) else {}
    hand = ov.setdefault('hand', {})
    hand[k] = dict(name=names[k], size=[img.shape[1], img.shape[0]], windows=rects, picked=[list(r) for r in rects_in])
    ov['hand'] = dict(sorted(hand.items()))
    json.dump(ov, open(OVERRIDES, 'w'), indent=1)
    print(f'{names[k]}: {len(rects_in)} windows, {sum(r[2] for r in rects)} texels lit')


def clip(prefix, rect):
    imgs, names = all_textures()
    keys = [k for k in imgs if k.startswith(prefix)]
    if len(keys) != 1:
        print(f'{prefix}: {len(keys)} keys match'); return
    ov = json.load(open(OVERRIDES)) if os.path.exists(OVERRIDES) else {}
    ov.setdefault('clip', {})[keys[0]] = rect
    json.dump(ov, open(OVERRIDES, 'w'), indent=1)
    print(f'{names[keys[0]]}: art kept to {rect}')


def mark(path, verdict, prefixes, snow=False):
    m = json.load(open(path))
    ov = json.load(open(OVERRIDES)) if os.path.exists(OVERRIDES) else {}
    yes_k, no_k = ('snow_maps', 'snow_reject') if snow else ('maps', 'reject')
    ov.setdefault(yes_k, {}); ov.setdefault(no_k, {})
    for p in prefixes:
        keys = [k for k in m if k.startswith(p)]
        if len(keys) != 1:
            print(f'{p}: {len(keys)} keys match'); continue
        k = keys[0]; v = m[k]
        if verdict == 'accept':
            ov[yes_k][k] = {f: v[f] for f in ('track', 'page', 'clut', 'affine', 'size', 'name', 'score')}
            ov[no_k].pop(k, None)
        else:
            ov[no_k][k] = v['affine']; ov[yes_k].pop(k, None)
    ov[yes_k] = dict(sorted(ov[yes_k].items())); ov[no_k] = dict(sorted(ov[no_k].items()))
    json.dump(ov, open(OVERRIDES, 'w'), indent=1)
    print(f"{len(ov[yes_k])} accepted, {len(ov[no_k])} rejected")


SNOW_MAP_OUT = os.path.join(HERE, '..', '..', 'assets', 'sfrush_ps1', 'n64_snow_map.json')


def publish_snow(path, review=None):
    """The SNOW versions' maps of the textures n64_night_map.json doesn't hold (roads, grass, roofs, trees: scan --all,
    pick): kept as the night's are (their DAY texels correlate and their colors agree, or accepted by eye: overrides
    "snow_maps" / "snow_reject", mark ... --snow), with their crop keys. The install samples the snow set through these
    and the night map's (ps1_track.cpp n64_snow_art)."""
    from snow_map import vram_array, decode_page
    from ps1 import Disc; from disps1 import Exe
    from PIL import ImageDraw
    m = json.load(open(path)); night = json.load(open(MAP_OUT))
    ov = json.load(open(OVERRIDES)) if os.path.exists(OVERRIDES) else {}
    yes, no = ov.get('snow_maps', {}), ov.get('snow_reject', {})
    disc = Disc(); e = Exe(); vd, vs = {}, {}
    imgs, names = all_textures()
    out, look = {}, []
    for k, v in sorted(m.items()):
        if k not in imgs or k in night or v['name'].startswith(('CHKPNT', 'FINISH')): continue
        t = v['track']
        if t not in vd: vd[t], vs[t] = vram_array(disc, e, t, ''), vram_array(disc, e, t, 'S')
        a = v['affine']; M = np.array([[a[0], a[1]], [a[2], a[3]]]); W, H = v['size']
        day, msk = sample_page(decode_page(vd[t], tuple(v['page']), v['clut']), M, np.array(a[4:6]), W, H)
        src = imgs[k]; mm = msk & (src[..., 3] >= 128)
        color = float(np.median(np.linalg.norm(src[..., :3][mm].astype(float) - day[mm].astype(float), axis=1))) if mm.any() else 999.0
        v = dict(v, color=round(color, 1))
        rejected = k in no and no[k] == v['affine']
        if not rejected and k not in yes and accept(v['score'], color):
            out[k] = {f: v[f] for f in ('track', 'page', 'clut', 'affine', 'size', 'name', 'score', 'color')}
        elif v['score'] >= 0.45:
            snow = sample_page(decode_page(vs[t], tuple(v['page']), v['clut']), M, np.array(a[4:6]), W, H)[0]
            look.append((k, v, src, day, snow, 'Y' if k in yes and yes[k]['affine'] == v['affine'] else 'N' if rejected else '?'))
    out.update(yes)
    for k, v in out.items():
        if k in imgs: v['crops'] = [[tex_key(imgs[k][:h]), h] for h in range(16, imgs[k].shape[0], 16)]
    json.dump(dict(sorted(out.items())), open(SNOW_MAP_OUT, 'w'), indent=1)
    print(f'{len(out)} snow maps -> {SNOW_MAP_OUT}; {len(look)} left for a look')
    if review:
        os.makedirs(review, exist_ok=True)
        tiles = []
        for k, v, src, day, snow, mark_ in sorted(look, key=lambda x: -x[1]['score']):
            W, H = v['size']; sc = max(1, 64 // max(W, H))
            tile = Image.new('RGB', (max(W * sc * 3 + 8, 150), H * sc + 12), (255, 0, 255) if mark_ == '?' else (0, 160, 0) if mark_ == 'Y' else (160, 0, 0))
            for c, im in enumerate((src[..., :3], day, snow)):
                tile.paste(Image.fromarray(im.astype(np.uint8)).resize((W * sc, H * sc), Image.NEAREST), (c * (W * sc + 4), 12))
            ImageDraw.Draw(tile).text((1, 0), f"{v['score']:.2f} {v['color']:.0f} {k[:8]}", fill=(255, 255, 255))
            tiles.append(tile)
        per = 30
        for s0 in range(0, len(tiles), per):
            T_ = tiles[s0:s0 + per]; w = max(x.width for x in T_) + 6; cols = 3
            rows = [T_[i:i + cols] for i in range(0, len(T_), cols)]
            sheet = Image.new('RGB', (w * cols, sum(max(x.height for x in r) + 4 for r in rows)), (40, 40, 40)); y = 0
            for r in rows:
                for c, x in enumerate(r): sheet.paste(x, (c * w, y))
                y += max(x.height for x in r) + 4
            sheet.save(os.path.join(review, f'review_{s0 // per:02d}.png'))


def ground(out_path, sheets, files, only=None):
    """Candidates for the textures no map holds (n64_night_map.json, n64_snow_map.json): noisy ground (grass, gravel,
    dirt, paving) whose art the PS1 drew anew, so no day texels match. Each takes the PS1 texture most of its faces
    land on (the face maps of scan --all, grouped by PS1 page and scale, the group with the most samples; its median
    affine), where its mean day color is within 40 of the texture's and the affine isn't degenerate. Written in the
    map's form (score -1, color the mean color distance) for mark ... --snow; sheets N64 day | PS1 day | PS1 snow.
    With `only` (key prefixes), just those textures, without the ground filters (trees: billboards over the PS1's own
    tree sprites, mostly transparent)."""
    from snow_map import vram_array, decode_page
    from ps1 import Disc; from disps1 import Exe
    from PIL import ImageDraw
    night = json.load(open(MAP_OUT)); snowm = json.load(open(SNOW_MAP_OUT)) if os.path.exists(SNOW_MAP_OUT) else {}
    imgs, names = all_textures(); disc = Disc(); e = Exe(); vd, vs = {}, {}
    best = {}
    for f in files:
        z = np.load(f, allow_pickle=True); t = int(z['track']); keys = list(z['keys']); R = z['rows']
        for tex in sorted(set(R[:, 0].astype(int))):
            key = str(keys[tex])
            if key in night or key in snowm or key not in imgs: continue
            if only and not any(key.startswith(o) for o in only): continue
            groups = {}
            for r in R[R[:, 0] == tex]:
                g = (int(r[1]),) + tuple(np.round(np.abs(r[2:6]) * 4).astype(int))
                groups.setdefault(g, []).append(r)
            g, rs = max(groups.items(), key=lambda kv: sum(r[8] for r in kv[1]))
            n = sum(r[8] for r in rs)
            if key not in best or n > best[key][0]:
                best[key] = (n, t, tuple(int(x) for x in z['page_base'][g[0]]), int(z['page_clut'][g[0]]),
                             np.median(np.array(rs), axis=0))
    found, tiles = {}, []
    for key, (n, t, base, cid, r) in sorted(best.items(), key=lambda kv: -kv[1][0]):
        M = np.array([[r[2], r[3]], [r[4], r[5]]]); b = r[6:8]
        if not 0.25 <= abs(np.linalg.det(M)) <= 16: continue
        if t not in vd: vd[t], vs[t] = vram_array(disc, e, t, ''), vram_array(disc, e, t, 'S')
        img = imgs[key]; H, W = img.shape[:2]
        day, msk = sample_page(decode_page(vd[t], base, cid), M, b, W, H)
        if not only and (msk.mean() < 0.9 or day.reshape(-1, 3).std(0).max() < 4): continue
        dc = float(np.linalg.norm(img[..., :3].reshape(-1, 3).mean(0) - day.reshape(-1, 3).mean(0)))
        if dc > 40 and not only: continue
        found[key] = dict(score=-1, color=round(dc, 1), track=t, page=list(base), clut=cid,
                          affine=[round(float(x), 4) for x in (M[0, 0], M[0, 1], M[1, 0], M[1, 1], b[0], b[1])],
                          size=[W, H], name=names[key], samples=int(n))
        snow = sample_page(decode_page(vs[t], base, cid), M, b, W, H)[0]
        sc = max(1, 64 // max(W, H))
        tile = Image.new('RGB', (max(W * sc * 3 + 8, 150), H * sc + 12), (255, 0, 255))
        for c, im in enumerate((img[..., :3], day, snow)):
            tile.paste(Image.fromarray(im.astype(np.uint8)).resize((W * sc, H * sc), Image.NEAREST), (c * (W * sc + 4), 12))
        ImageDraw.Draw(tile).text((1, 0), f"{dc:.0f} {key[:8]} {names[key][:14]}", fill=(255, 255, 255))
        tiles.append(tile)
    json.dump(found, open(out_path, 'w'), indent=1)
    print(f'{len(found)} ground candidates -> {out_path}')
    if sheets and tiles:
        os.makedirs(sheets, exist_ok=True)
        per = 30
        for s0 in range(0, len(tiles), per):
            T_ = tiles[s0:s0 + per]; w = max(x.width for x in T_) + 6; cols = 3
            rows = [T_[i:i + cols] for i in range(0, len(T_), cols)]
            sheet = Image.new('RGB', (w * cols, sum(max(x.height for x in r) + 4 for r in rows)), (40, 40, 40)); y = 0
            for r in rows:
                for c, x in enumerate(r): sheet.paste(x, (c * w, y))
                y += max(x.height for x in r) + 4
            sheet.save(os.path.join(sheets, f'ground_{s0 // per:02d}.png'))


def material(out_path, sheet, force=None, min_area=200.0):
    """Candidates by material for the four N64-only courses' ground textures no map holds (n64_night_map.py ground's
    leftovers: filler the N64 spreads everywhere, and The Rock, which the PS1 lacks): each takes the snow map of the
    mapped ground texture (n64_snow_map.json) nearest it in the N64 day art (mean color, then grain: per-channel
    spread), its affine scaled so the new texture covers the same PS1 texels. Ground = most of its area facing up,
    min_area m^2 over the four courses at least. Written in the map's form (score -2, color the mean color distance,
    ref the texture whose map it borrows) for mark ... --snow; the sheet shows N64 day | the borrowed texture's N64 day
    | PS1 day | PS1 snow through the new map. `force` {key prefix: reference key prefix} picks a texture's reference
    by hand (gravel nearest a road in color takes the gravel; a tree another tree's), and takes it whatever its use. Green textures (grass) count whatever way they face."""
    from snow_map import vram_array, decode_page
    from ps1 import Disc; from disps1 import Exe
    from PIL import ImageDraw
    night = json.load(open(MAP_OUT)); snowm = json.load(open(SNOW_MAP_OUT))
    have = {k for k, e in night.items() if 'windows' not in e and 'variant_of' not in e} | set(snowm)
    imgs, names = all_textures()
    def feat(img):
        px = img[..., :3].reshape(-1, 3).astype(float)
        return px.mean(0), px.std(0)
    # Ground use over the courses: of the missing textures (the four) and of the snow-mapped ones (all seven).
    use = {}
    for course in COURSES:
        T, U, X, texs, nm = n64_course(course)
        nrm = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]); area = np.linalg.norm(nrm, axis=1) / 2
        up = nrm[:, 1] > 0.7 * np.maximum(2 * area, 1e-12)
        for x in set(X):
            k = tex_key(texs[x]); sel = X == x
            if k not in have and course not in ('Downtown', 'The_Heights', 'Sunset', 'The_Rock'): continue
            u = use.setdefault(k, [0.0, 0.0]); u[0] += area[sel & up].sum(); u[1] += area[sel].sum()
    refs = [k for k in snowm if k in imgs and k in use and use[k][0] >= 0.5 * use[k][1]]
    rf = {k: feat(imgs[k]) for k in refs}
    disc = Disc(); e = Exe(); vd, vs = {}, {}
    found, tiles = {}, []
    def green(k):
        m_ = imgs[k][..., :3].reshape(-1, 3).mean(0)
        return m_[1] > m_[0] + 6 and m_[1] > m_[2] + 6
    forced = lambda k: any(k.startswith(f) for f in (force or {}))
    todo = sorted((k for k, u in use.items() if k not in have and k in imgs and
                   (forced(k) or (u[1] >= min_area and ((u[0] >= 0.5 * u[1] and u[0] >= min_area) or green(k))))),
                  key=lambda k: -use[k][0])
    for key in todo:
        img = imgs[key]; H, W = img.shape[:2]; m, sd = feat(img)
        if img[..., 3].mean() < 200 and not forced(key): continue    # see-through: not a ground texture
        if m[2] > m[0] * 1.3 + 8 and m[2] > m[1] * 1.05: continue    # water stays water
        best = min(refs, key=lambda r: np.linalg.norm(m - rf[r][0]) + 0.5 * np.linalg.norm(sd - rf[r][1]))
        for fk, fr in (force or {}).items():
            if key.startswith(fk): best = next(r for r in snowm if r.startswith(fr))
        r = snowm[best]; dc = float(np.linalg.norm(m - feat(imgs[best])[0]))
        a = r['affine']; W1, H1 = r['size']
        M = np.array([[a[0] * W1 / W, a[1] * H1 / H], [a[2] * W1 / W, a[3] * H1 / H]]); b = np.array(a[4:6])
        t = r['track']; base = tuple(r['page'])
        if t not in vd: vd[t], vs[t] = vram_array(disc, e, t, ''), vram_array(disc, e, t, 'S')
        day = sample_page(decode_page(vd[t], base, r['clut']), M, b, W, H)[0]
        snow = sample_page(decode_page(vs[t], base, r['clut']), M, b, W, H)[0]
        found[key] = dict(score=-2, color=round(dc, 1), track=t, page=list(base), clut=r['clut'],
                          affine=[round(float(x), 4) for x in (M[0, 0], M[0, 1], M[1, 0], M[1, 1], b[0], b[1])],
                          size=[W, H], name=names[key], ref=best, area=int(use[key][1]))
        sc = max(1, 64 // max(W, H)); ref_img = imgs[best]
        tile = Image.new('RGB', (W * sc * 4 + 16, H * sc + 12), (40, 40, 40))
        rimg = Image.fromarray(ref_img[..., :3]).resize((W * sc, H * sc), Image.NEAREST)
        for c, im in enumerate((Image.fromarray(img[..., :3]).resize((W * sc, H * sc), Image.NEAREST), rimg,
                                Image.fromarray(day.astype(np.uint8)).resize((W * sc, H * sc), Image.NEAREST),
                                Image.fromarray(snow.astype(np.uint8)).resize((W * sc, H * sc), Image.NEAREST))):
            tile.paste(im, (c * (W * sc + 4), 12))
        ImageDraw.Draw(tile).text((1, 0), f"{key[:8]} {names[key][:12]} {int(use[key][0])} <- {best[:8]} {dc:.0f}",
                                  fill=(255, 255, 255))
        tiles.append(tile)
    json.dump(found, open(out_path, 'w'), indent=1)
    print(f'{len(found)} material candidates -> {out_path}')
    for k, v in found.items(): print(f"  {k} {v['name'][:20]:20} {v['area']:8d} <- {v['ref']} {names[v['ref']][:20]} {v['color']}")
    if sheet and tiles:
        w = max(x.width for x in tiles) + 6; cols = 2
        rows = [tiles[i:i + cols] for i in range(0, len(tiles), cols)]
        sh = Image.new('RGB', (w * cols, sum(max(x.height for x in r_) + 4 for r_ in rows)), (20, 20, 20)); y = 0
        for r_ in rows:
            for c, x in enumerate(r_): sh.paste(x, (c * w, y))
            y += max(x.height for x in r_) + 4
        sh.save(sheet)


def snow_cover(sheet):
    """How much of each N64-only course the SNOW versions show in the PS1's snow art (n64_night_map.json's maps that
    aren't hand windows or palette variants, plus n64_snow_map.json), by triangle area, ground (facing up) and walls
    apart; with SHEET, a picture of the 48 largest textures still missing, by area over the four courses."""
    from PIL import ImageDraw
    night = json.load(open(MAP_OUT)); snowm = json.load(open(SNOW_MAP_OUT))
    have = {k for k, e in night.items() if 'windows' not in e and 'variant_of' not in e} | set(snowm)
    missing = {}
    for course in ('Downtown', 'The_Heights', 'Sunset', 'The_Rock'):
        T, U, X, texs, names = n64_course(course)
        nrm = np.cross(T[:, 1] - T[:, 0], T[:, 2] - T[:, 0]); area = np.linalg.norm(nrm, axis=1) / 2
        up = nrm[:, 1] > 0.7 * np.maximum(2 * area, 1e-12)
        keys = [tex_key(t) for t in texs]
        ok = np.array([keys[x] in have for x in X])
        g = lambda m: 100 * area[m & ok].sum() / max(area[m].sum(), 1e-9)
        print(f'{course}: {g(np.ones_like(ok))}% of the area in PS1 snow art, ground {g(up):.0f}%, walls {g(~up):.0f}%'.replace('%', '%', 1)
              .replace(f'{g(np.ones_like(ok))}', f'{g(np.ones_like(ok)):.0f}'))
        for x in set(X):
            if keys[x] in have: continue
            s_ = X == x
            e = missing.setdefault(keys[x], [0.0, 0.0, texs[x], names[x], set()])
            e[0] += area[s_ & up].sum(); e[1] += area[s_ & ~up].sum(); e[4].add(course[:4])
    top = sorted(missing.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))
    print('largest missing (m^2 ground / walls):')
    for k, (a_up, a_w, _, nm, cs) in top[:30]:
        print(f'  {k} {nm[:24]:24} {a_up:8.0f} {a_w:8.0f}  {",".join(sorted(cs))}')
    if sheet:
        tiles = []
        for k, (a_up, a_w, img, nm, cs) in top[:48]:
            H, W = img.shape[:2]; sc = max(1, 96 // max(W, H))
            t_ = Image.new('RGB', (150, 112), (40, 40, 40))
            t_.paste(Image.fromarray(img[..., :3]).resize((W * sc, H * sc), Image.NEAREST), (0, 14))
            ImageDraw.Draw(t_).text((1, 0), f"{k[:8]} {'G' if a_up > a_w else 'W'} {int(a_up + a_w)}", fill=(255, 255, 255))
            tiles.append(t_)
        sh = Image.new('RGB', (150 * 8, 112 * ((len(tiles) + 7) // 8)), (40, 40, 40))
        for i, t_ in enumerate(tiles): sh.paste(t_, ((i % 8) * 150, (i // 8) * 112))
        sh.save(sheet)


def cover(path):
    m = json.load(open(path))
    for course in ('Downtown', 'The_Heights', 'Sunset', 'The_Rock', 'Market', 'Embarcadero', 'Golden_Gate'):
        T, U, X, texs, names = n64_course(course)
        b = building_textures(T, X, len(texs))
        have = [k for k in b if tex_key(texs[k]) in m]
        good = [k for k in have if m[tex_key(texs[k])]['score'] >= 0.5]
        good = [k for k in have if 'color' not in m[tex_key(texs[k])] or m[tex_key(texs[k])]['score'] >= 0.5]
        print(f'{course}: {len(b)} building textures, {len(have)} mapped' + ('' if path == MAP_OUT else f', {len(good)} scoring 0.5+'))


if __name__ == '__main__':
    a = sys.argv[1:]
    if a and a[0] == 'scan': scan(a[1], int(a[2]), a[3], every='--all' in a)
    elif a and a[0] == 'snowclass': scan(a[1], int(a[2]), a[3], snow=True)
    elif a and a[0] == 'snowpublish': snow_publish(a[1], a[2:])
    elif a and a[0] == 'pick': pick(a[1], a[2], a[3:])
    elif a and a[0] == 'search':
        shard = (0, 1)
        if '--shard' in a:
            i = a.index('--shard'); shard = tuple(int(x) for x in a[i + 1].split('/')); del a[i:i + 2]
        search(a[1], float(a[2]), a[3:] or ['Downtown', 'The_Heights', 'Sunset', 'The_Rock'], shard)
    elif a and a[0] == 'publish': publish(a[1], a[2] if len(a) > 2 else None)
    elif a and a[0] == 'mark': mark(a[1], a[2], [x for x in a[3:] if x != '--snow'], snow='--snow' in a)
    elif a and a[0] == 'ground':
        only = None
        if '--only' in a:
            i = a.index('--only'); only = a[i + 1].split(','); del a[i:i + 2]
        ground(a[1], a[2], a[3:], only)
    elif a and a[0] == 'snowmaps': publish_snow(a[1], a[2] if len(a) > 2 else None)
    elif a and a[0] == 'windows': windows(a[1], a[2])
    elif a and a[0] == 'clip': clip(a[1], [int(v) for v in a[2].split(',')])
    elif a and a[0] == 'light': light(a[1], [[int(v) for v in r.split(',')] for r in a[2:]])
    elif a and a[0] == 'cover': cover(a[1])
    elif a and a[0] == 'material':
        force = {}
        if '--force' in a:
            i = a.index('--force'); force = dict(x.split('=') for x in a[i + 1].split(',')); del a[i:i + 2]
        material(a[1], a[2] if len(a) > 2 else None, force)
    elif a and a[0] == 'snowcover': snow_cover(a[1] if len(a) > 1 else None)
    else: print(__doc__)
