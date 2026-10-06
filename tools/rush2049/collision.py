"""Rush 2 / Rush 2049 track collision files: parser, validator and 2049 -> Rush 2 converter.

Format notes live in docs/rush2049_research/collision.md. Summary (offsets in bytes, big-endian):

  Rush 2 header (0xC):  u16 nSeg, nNode, nPoly, nVert, leafBytes, vlistBytes
  Rush 2049 header (0x10): u16 nSeg, nNode, nPoly, nVert, nMover, vlistBytes; u32 leafBytes
  Section order, Rush 2:  SEG[nSeg*0x84] NODE[nNode*0x14] POLY[nPoly*0x18] VERT[nVert*8] LEAF[leafBytes] VLIST[vlistBytes]
  Section order, 2049:    SEG NODE POLY VERT MOVER[nMover*0x20] VLIST[vlistBytes] LEAF[leafBytes]

  SEG   (0x84) road-surface spline segment, read by func_8006BDA8 (poly flag 0x1000) and func_8006C2B8 (flag 0x2000).
  NODE  (0x14) quadtree node: s16 parent(-1 root), u8 0, u8 childMask, s16 xmin,xmax,zmin,zmax, u16 child[4].
               childMask bit q set -> child[q] is a NODE index, clear -> child[q] is a LEAF byte offset (0 = empty).
               2049 only: bit 4+q set -> leaf offset += 0x10000.
  POLY  (0x18) u16 flags (bits0-3 type, 4-7 wheel material, 8-11 + sign bit15 drone speed hint, 0x1000/0x2000 spline),
               u16 info (bits0-3 vertex count; upper bits differ per game), s16 m[9] (rotation * 16384),
               u16 vlist offset.
  VERT  (8)    s16 x,y,z, u16 frac (x:14-10, y:9-5, z:4-0); value = int + frac/32. Vertex 0 of a polygon is its world
               origin, vertices 1.. are in the polygon's local frame (local y = 0).
  VLIST        per polygon: run-length list of vertex indices (u16 start; if more remain and next byte >= MARK, that
               byte & MASK more consecutive indices follow), then u16 SEG index only if flags & 0x3000.
               Rush 2: MARK 0xC0 / MASK 0x3F.  2049: MARK 0xE0 / MASK 0x1F.
  LEAF         u8 count, then u16 entries: bits 15-13 extra run length, bits 12-0 first poly index.
  MOVER (0x20, 2049 only) u16 objectId, u16 poly, s16 restMatrix[9], u16 vert, u8 restVert[8].

Run from tools/rush2049:  python collision.py   (parses, validates and converts every collision file in both ROMs)
"""
import struct, sys
from collections import Counter, OrderedDict

R2, R49 = 'rush2', 'rush2049'
SEG_SIZE, NODE_SIZE, POLY_SIZE, VERT_SIZE, MOVER_SIZE = 0x84, 0x14, 0x18, 8, 0x20
VLIST_RUN = {R2: (0xC0, 0x3F), R49: (0xE0, 0x1F)}
SPLINE_FLAGS = 0x3000
TYPE_WALL = (5, 6)
TYPE_DISABLED = 0xF          # 2049 runtime only (func_800B2CB4)


# ----------------------------------------------------------------------------------------------------------------
# low-level codecs

def decode_vlist(buf, off, count, game):
    """Return (vertex indices, end offset) for one polygon's run-length list (func_8006C670 / func_800AD5D0)."""
    mark, mask = VLIST_RUN[game]
    out = []
    left = count
    while left > 0:
        v = (buf[off] << 8) | buf[off + 1]
        off += 2
        run = 0
        if left >= 2 and buf[off] >= mark:
            run = buf[off] & mask
            off += 1
        left -= run + 1
        out.extend(range(v, v + run + 1))
    return out, off


def encode_vlist(indices, game):
    mark, mask = VLIST_RUN[game]
    out = bytearray()
    i = 0
    while i < len(indices):
        v = indices[i]
        if v >= (mark << 8):
            raise ValueError('vertex index 0x%X collides with run marker' % v)
        j = i + 1
        while j < len(indices) and indices[j] == indices[j - 1] + 1 and j - i <= mask:
            j += 1
        out += struct.pack('>H', v)
        if j - i > 1:
            out.append(mark | (j - i - 1))
        i = j
    return bytes(out)


def decode_leaf(buf, off):
    """Return (poly indices in file order, end offset) for a leaf list (func_8006CCEC / func_800ADCE0)."""
    count = buf[off]
    off += 1
    out = []
    left = count
    while left > 0:
        v = (buf[off] << 8) | buf[off + 1]
        off += 2
        run = v >> 13
        out.extend(range(v & 0x1FFF, (v & 0x1FFF) + run + 1))
        left -= run + 1
    return out, off


def encode_leaf(polys):
    """Encode ascending poly indices as a leaf list (runs of up to 8)."""
    out = bytearray([len(polys)])
    i = 0
    while i < len(polys):
        j = i + 1
        while j < len(polys) and polys[j] == polys[j - 1] + 1 and j - i < 8:
            j += 1
        out += struct.pack('>H', ((j - i - 1) << 13) | polys[i])
        i = j
    return bytes(out)


def vert_xyz(raw):
    x, y, z, f = struct.unpack('>hhhH', raw)
    return ((x * 32 + ((f >> 10) & 31)) / 32.0, (y * 32 + ((f >> 5) & 31)) / 32.0, (z * 32 + (f & 31)) / 32.0)


# ----------------------------------------------------------------------------------------------------------------
# parsed representation

class Collision:
    def __init__(self, game):
        self.game = game
        self.segs = []      # raw 0x84-byte records
        self.nodes = []     # dicts
        self.polys = []     # dicts
        self.verts = []     # raw 8-byte records
        self.movers = []    # dicts (2049)
        self.leaves = OrderedDict()   # leaf byte offset -> (poly list, raw bytes)
        self.leaf_blob = b''
        self.vlist_blob = b''

    # -- parsing ---------------------------------------------------------------------------------------------
    @classmethod
    def parse(cls, data, game):
        c = cls(game)
        if game == R2:
            n_seg, n_node, n_poly, n_vert, leaf_bytes, vlist_bytes = struct.unpack('>6H', data[:12])
            n_mover = 0
            o = 12
        else:
            n_seg, n_node, n_poly, n_vert, n_mover, vlist_bytes, leaf_bytes = struct.unpack('>6HI', data[:16])
            o = 16
        expect = o + n_seg * SEG_SIZE + n_node * NODE_SIZE + n_poly * POLY_SIZE + n_vert * VERT_SIZE \
            + n_mover * MOVER_SIZE + vlist_bytes + leaf_bytes
        if expect != len(data):
            raise ValueError('%s collision size 0x%X != header total 0x%X' % (game, len(data), expect))
        c.segs = [data[o + i * SEG_SIZE:o + (i + 1) * SEG_SIZE] for i in range(n_seg)]
        o += n_seg * SEG_SIZE
        for i in range(n_node):
            p, pad, mask, x0, x1, z0, z1, c0, c1, c2, c3 = struct.unpack('>hBBhhhhHHHH', data[o:o + NODE_SIZE])
            c.nodes.append(dict(parent=p, pad=pad, mask=mask, bounds=(x0, x1, z0, z1), child=[c0, c1, c2, c3]))
            o += NODE_SIZE
        for i in range(n_poly):
            r = data[o:o + POLY_SIZE]
            flags, info = struct.unpack('>HH', r[:4])
            c.polys.append(dict(flags=flags, info=info, matrix=struct.unpack('>9h', r[4:22]),
                                voff=struct.unpack('>H', r[22:24])[0]))
            o += POLY_SIZE
        c.verts = [data[o + i * 8:o + i * 8 + 8] for i in range(n_vert)]
        o += n_vert * VERT_SIZE
        if game == R49:
            for i in range(n_mover):
                r = data[o:o + MOVER_SIZE]
                oid, poly = struct.unpack('>HH', r[:4])
                c.movers.append(dict(id=oid, poly=poly, matrix=struct.unpack('>9h', r[4:22]),
                                     vert=struct.unpack('>H', r[22:24])[0], vraw=r[24:32]))
                o += MOVER_SIZE
            c.vlist_blob = data[o:o + vlist_bytes]
            o += vlist_bytes
            c.leaf_blob = data[o:o + leaf_bytes]
        else:
            c.leaf_blob = data[o:o + leaf_bytes]
            o += leaf_bytes
            c.vlist_blob = data[o:o + vlist_bytes]
        # polygon vertex lists
        for p in c.polys:
            idx, end = decode_vlist(c.vlist_blob, p['voff'], p['info'] & 0xF, game) \
                if p['voff'] < len(c.vlist_blob) else ([], p['voff'])
            p['verts'] = idx
            p['seg'] = None
            if p['flags'] & SPLINE_FLAGS and end + 2 <= len(c.vlist_blob):
                p['seg'] = (c.vlist_blob[end] << 8) | c.vlist_blob[end + 1]
                end += 2
            p['vend'] = end
        # leaf lists referenced by the quadtree
        for n in c.nodes:
            n['leaf'] = [None] * 4
            for q in range(4):
                if n['mask'] & (1 << q):
                    continue
                off = n['child'][q] | (0x10000 if (n['mask'] >> 4) & (1 << q) else 0)
                n['leaf'][q] = off
                if off and off not in c.leaves and off < len(c.leaf_blob):
                    polys, end = decode_leaf(c.leaf_blob, off)
                    c.leaves[off] = (polys, c.leaf_blob[off:end])
        return c

    # -- validation ------------------------------------------------------------------------------------------
    def validate(self):
        errs = []
        n_seg, n_node, n_poly, n_vert = len(self.segs), len(self.nodes), len(self.polys), len(self.verts)
        mark = VLIST_RUN[self.game][0]
        # nodes
        for i, n in enumerate(self.nodes):
            if i == 0 and n['parent'] != -1:
                errs.append('node 0 parent %d' % n['parent'])
            if i and not (0 <= n['parent'] < n_node):
                errs.append('node %d parent %d out of range' % (i, n['parent']))
            x0, x1, z0, z1 = n['bounds']
            if x0 > x1 or z0 > z1:
                errs.append('node %d bad bounds' % i)
            if self.game == R2 and n['mask'] & 0xF0:
                errs.append('node %d has 2049-only leaf high bits' % i)
            for q in range(4):
                if n['mask'] & (1 << q):
                    k = n['child'][q]
                    if k == 0:
                        continue            # node index 0 = null child: func_8006CD60 returns no leaf
                    if not (0 < k < n_node):
                        errs.append('node %d child %d -> node %d out of range' % (i, q, k))
                    elif self.nodes[k]['parent'] != i:
                        errs.append('node %d child %d parent mismatch' % (i, q))
                else:
                    off = n['leaf'][q]
                    if off and off >= len(self.leaf_blob):
                        errs.append('node %d leaf %d offset 0x%X past leaf section' % (i, q, off))
        for off, (polys, raw) in self.leaves.items():
            if off + len(raw) > len(self.leaf_blob):
                errs.append('leaf 0x%X runs past section' % off)
            if len(polys) != raw[0]:
                errs.append('leaf 0x%X count mismatch' % off)
            for p in polys:
                if p >= n_poly:
                    errs.append('leaf 0x%X poly %d out of range' % (off, p))
                    break
        # polygons
        ends = []
        for i, p in enumerate(self.polys):
            n = p['info'] & 0xF
            if n < 3:
                errs.append('poly %d has %d vertices' % (i, n))
            if p['voff'] >= len(self.vlist_blob):
                errs.append('poly %d vlist offset out of range' % i)
                continue
            if len(p['verts']) != n:
                errs.append('poly %d vlist decodes to %d verts' % (i, len(p['verts'])))
            for v in p['verts']:
                if v >= n_vert:
                    errs.append('poly %d vertex %d out of range' % (i, v))
                    break
                if v >= (mark << 8):
                    errs.append('poly %d vertex %d collides with run marker' % (i, v))
            if p['flags'] & SPLINE_FLAGS:
                if p['seg'] is None or p['seg'] >= n_seg:
                    errs.append('poly %d spline segment %r out of range (%d)' % (i, p['seg'], n_seg))
            if self.game == R2 and (p['flags'] & 0xF) == TYPE_DISABLED:
                errs.append('poly %d uses 2049-only type 0xF' % i)
            ends.append((p['voff'], p['vend']))
        ends.sort()
        for (a, e), (b, _) in zip(ends, ends[1:]):
            if e > b:
                errs.append('vertex lists overlap at 0x%X' % a)
                break
        if ends and ends[-1][1] > len(self.vlist_blob):
            errs.append('last vertex list runs past section')
        # movers
        for i, m in enumerate(self.movers):
            if m['poly'] >= n_poly:
                errs.append('mover %d poly %d out of range' % (i, m['poly']))
            elif m['vert'] >= n_vert:
                errs.append('mover %d vertex %d out of range' % (i, m['vert']))
        return errs

    def stats(self):
        s = Counter()
        for p in self.polys:
            s['type%X' % (p['flags'] & 0xF)] += 1
        ys = []
        for p in self.polys:
            for v in p['verts'][1:]:
                ys.append(abs(vert_xyz(self.verts[v])[1]))
        ys.sort()
        mv_rest = sum(1 for m in self.movers if m['poly'] < len(self.polys)
                      and tuple(m['matrix']) == tuple(self.polys[m['poly']]['matrix'])
                      and self.verts[m['vert']] == m['vraw'])
        mv_first = sum(1 for m in self.movers if m['poly'] < len(self.polys)
                       and self.polys[m['poly']]['verts'][:1] == [m['vert']])
        return dict(types=dict(sorted(s.items())), local_y_p99=ys[int(len(ys) * 0.99)] if ys else 0,
                    local_y_max=ys[-1] if ys else 0, movers=len(self.movers), mover_ids=len({m['id'] for m in self.movers}),
                    movers_at_rest=mv_rest, movers_on_origin_vertex=mv_first,
                    leaf_bytes=len(self.leaf_blob), unique_leaves=len(self.leaves),
                    unique_leaf_bytes=len({raw for _, raw in self.leaves.values()}) and
                    sum(len(r) for r in {raw for _, raw in self.leaves.values()}) + 1)

    # -- fitting Rush 2's 16-bit leaf offsets ----------------------------------------------------------------
    def referenced_leaves(self):
        return {n['leaf'][q] for n in self.nodes if not n.get('dead') for q in range(4)
                if not n['mask'] & (1 << q) and n['leaf'][q]}

    def leaf_section_size(self):
        return 1 + sum(len(r) for r in {self.leaves[o][1] for o in self.referenced_leaves()})

    def merge_leaves(self, limit=0xFFFF):
        """Collapse bottom-level quadtree nodes into a single leaf in their parent until the leaf section fits.

        A merged leaf holds the union of its four children's polygon lists. The ground/wall queries
        (func_8006CF00, func_8006F704, func_8006FC94, func_8008BDC0) run an exact point-in-polygon test on every
        listed polygon and keep the best hit, so a superset list returns the same polygon as long as every source
        leaf was complete; only the amount of work changes. Returns the number of nodes removed."""
        if self.leaf_section_size() <= limit:
            return 0
        import heapq
        next_key = [max(self.leaves, default=0) + 0x1000000]

        def leaf_polys(off):
            return self.leaves[off][0] if off else []

        def child_is_leaf(n, q):
            return not n['mask'] & (1 << q) or n['child'][q] == 0

        def candidate(i):
            n = self.nodes[i]
            if i == 0 or n.get('dead') or not all(child_is_leaf(n, q) for q in range(4)):
                return None
            polys = sorted({p for q in range(4) if not n['mask'] & (1 << q) for p in leaf_polys(n['leaf'][q])})
            if len(polys) > 36:     # the queries' unchecked stack buffers hold 36 (func_8008BDC0); Rush 2 reaches 34
                return None
            raw = encode_leaf(polys)
            saved = sum(len(self.leaves[n['leaf'][q]][1]) for q in range(4)
                        if not n['mask'] & (1 << q) and n['leaf'][q]) - len(raw)
            return saved, polys, raw

        heap = []
        for i in range(len(self.nodes)):
            c = candidate(i)
            if c:
                heapq.heappush(heap, (-c[0], i))
        removed = 0
        while heap and self.leaf_section_size() > limit:
            _, i = heapq.heappop(heap)
            c = candidate(i)
            if not c:
                continue
            saved, polys, raw = c
            n = self.nodes[i]
            par = self.nodes[n['parent']]
            q = par['child'].index(i) if i in par['child'] else None
            key = next_key[0]
            next_key[0] += 1
            self.leaves[key] = (polys, raw)
            for k in range(4):            # parent's quadrant k that points at this node becomes a leaf
                if par['mask'] & (1 << k) and par['child'][k] == i:
                    par['mask'] &= ~(1 << k)
                    par['leaf'][k] = key
                    par['child'][k] = 0
            n['dead'] = True
            removed += 1
            pc = candidate(n['parent'])
            if pc:
                heapq.heappush(heap, (-pc[0], n['parent']))
        # compact node indices, keeping source order
        alive = [i for i, n in enumerate(self.nodes) if not n.get('dead')]
        newidx = {old: new for new, old in enumerate(alive)}
        nodes = []
        for i in alive:
            n = self.nodes[i]
            n['parent'] = newidx[n['parent']] if n['parent'] >= 0 else -1
            for k in range(4):
                if n['mask'] & (1 << k) and n['child'][k]:
                    n['child'][k] = newidx[n['child'][k]]
            nodes.append(n)
        self.nodes = nodes
        if self.leaf_section_size() > limit:
            raise ValueError('leaf section still 0x%X after merging' % self.leaf_section_size())
        return removed

    # -- writing ---------------------------------------------------------------------------------------------
    def build_rush2(self):
        """Serialise as a Rush 2 file. Leaf lists are re-packed (deduplicated); vertex lists use Rush 2 run markers."""
        # vertex lists, kept in the source file's order
        vblob = bytearray()
        voffs = [0] * len(self.polys)
        for i in sorted(range(len(self.polys)), key=lambda i: self.polys[i]['voff']):
            p = self.polys[i]
            voffs[i] = len(vblob)
            vblob += encode_vlist(p['verts'], R2)
            if p['flags'] & SPLINE_FLAGS:
                vblob += struct.pack('>H', p['seg'])
        if len(vblob) > 0xFFFF:
            raise ValueError('vertex-list section 0x%X exceeds Rush 2 u16 limit' % len(vblob))
        # leaf lists: byte 0 reserved so that offset 0 keeps meaning "empty"
        lblob = bytearray(b'\0')
        placed = {}
        remap = {}
        used = self.referenced_leaves()
        for off, (polys, raw) in sorted(self.leaves.items()):
            if off not in used:
                continue
            if raw not in placed:
                placed[raw] = len(lblob)
                lblob += raw
            remap[off] = placed[raw]
        if len(lblob) > 0xFFFF:
            raise ValueError('leaf section 0x%X exceeds Rush 2 16-bit leaf offsets' % len(lblob))
        out = bytearray(struct.pack('>6H', len(self.segs), len(self.nodes), len(self.polys), len(self.verts),
                                    len(lblob), len(vblob)))
        for s in self.segs:
            out += s
        for n in self.nodes:
            child = list(n['child'])
            for q in range(4):
                if not n['mask'] & (1 << q):
                    off = n['leaf'][q]
                    child[q] = remap[off] if off else 0
            x0, x1, z0, z1 = n['bounds']
            out += struct.pack('>hBBhhhhHHHH', n['parent'], 0, n['mask'] & 0x0F, x0, x1, z0, z1, *child)
        for p, vo in zip(self.polys, voffs):
            out += struct.pack('>HH9hH', p['flags'], p['info'], *p['matrix'], vo)
        for v in self.verts:
            out += v
        out += lblob
        out += vblob
        return bytes(out)


# ----------------------------------------------------------------------------------------------------------------
# 2049 -> Rush 2

def convert_info(info):
    """Map a 2049 POLY+2 word to Rush 2's meaning.

    2049 (func_800C6AA0, func_800E847C, func_800E1F80): bits 0-3 vertex count; bit 4 = conveyor/boost polygon and
    bit 5 = polygon carried by animated object, both with bits 11-15 = parameter (wheel value then masked to 0x7FF);
    bit 8 = covered/tunnel flag (car-state +0x35A); bits 11-15 otherwise = car lighting colour index.
    Rush 2 (func_8009A264): the whole word is majority-voted over the wheels, then bit 15 -> 2, bit 14 -> 1 is stored
    as the covered/tunnel level (car-state +0x344), which gates the same airborne code 2049 gates with bit 8.
    Bits 14/15 must therefore come only from 2049 bit 8; the rest is kept so the vote groups wheels as 2049 does.
    """
    v = info & 0x7FF if info & 0x30 else info
    out = v & 0x3EFF & ~0x30            # drop bit 8 (moved), bits 14-15 (Rush 2 tunnel bits) and 2049 force bits
    if v & 0x100:
        out |= 0x4000
    return out


def convert(data_2049, keep_info=False, merge=True, report=None):
    """Convert one Rush 2049 collision file to Rush 2 format.

    - POLY+2 is remapped with convert_info() unless keep_info.
    - VLIST run markers are re-encoded (0xE0|n -> 0xC0|n); offsets do not move.
    - The 2049 MOVER section is dropped: its polygons stay at the rest pose stored in POLY/VERT.
    - If the leaf section does not fit Rush 2's 16-bit offsets and merge is set, bottom quadtree nodes are merged
      (see Collision.merge_leaves); otherwise a ValueError is raised.
    report, if a dict, receives what was changed or dropped."""
    c = Collision.parse(data_2049, R49)
    errs = c.validate()
    if errs:
        raise ValueError('source file invalid: ' + '; '.join(errs[:5]))
    rep = report if report is not None else {}
    rep['movers_dropped'] = len(c.movers)
    rep['mover_ids'] = sorted({m['id'] for m in c.movers})
    rep['force_polys'] = sum(1 for p in c.polys if p['info'] & 0x10)
    rep['platform_polys'] = sum(1 for p in c.polys if p['info'] & 0x20)
    rep['covered_polys'] = sum(1 for p in c.polys if p['info'] & 0x100)
    for p in c.polys:
        if not keep_info:
            p['info'] = convert_info(p['info'])
        if (p['flags'] & 0xF) == TYPE_DISABLED:
            raise ValueError('source polygon already disabled (type 0xF)')
    c.game = R2
    rep['nodes_merged'] = 0
    if c.leaf_section_size() > 0xFFFF:
        if not merge:
            raise ValueError('leaf section 0x%X exceeds Rush 2 16-bit leaf offsets' % c.leaf_section_size())
        rep['nodes_merged'] = c.merge_leaves()
    return c.build_rush2()


def extras(data_2049):
    """The 2049-only collision behaviour convert() cannot express in Rush 2 data, for a future runtime patch:
    movers (id, poly, rest matrix, origin vertex) and force/platform polygons (poly, kind, parameter)."""
    c = Collision.parse(data_2049, R49)
    special = []
    for i, p in enumerate(c.polys):
        if p['info'] & 0x30:
            special.append(dict(poly=i, kind='platform' if p['info'] & 0x20 else 'boost', param=p['info'] >> 11,
                                speed_mph=(p['info'] >> 11) * 8 if p['info'] & 0x10 else None))
    return dict(movers=c.movers, special=special,
                covered=[i for i, p in enumerate(c.polys) if p['info'] & 0x100],
                light_index={i: p['info'] >> 11 for i, p in enumerate(c.polys) if not p['info'] & 0x30 and p['info'] >> 11})


def leaf_paths(c):
    """Yield (quadrant path from root, poly list or None for null) for every leaf of the quadtree."""
    stack = [(0, ())]
    while stack:
        i, path = stack.pop()
        n = c.nodes[i]
        for q in range(4):
            if n['mask'] & (1 << q):
                if n['child'][q]:
                    stack.append((n['child'][q], path + (q,)))
                else:
                    yield path + (q,), None
            else:
                off = n['leaf'][q]
                yield path + (q,), (c.leaves[off][0] if off else [])


def leaf_at(c, path):
    i = 0
    for depth, q in enumerate(path):
        n = c.nodes[i]
        if n['mask'] & (1 << q):
            if not n['child'][q]:
                return None, depth
            i = n['child'][q]
        else:
            off = n['leaf'][q]
            return (c.leaves[off][0] if off else []), depth
    raise ValueError('path ends at a node')


def cover_check(src, out):
    """Every source leaf must map to an output leaf with a superset of its polygons. Returns (exact, merged, bad)."""
    exact = merged = bad = 0
    for path, polys in leaf_paths(src):
        got, depth = leaf_at(out, path)
        want = set(polys or [])
        if not want.issubset(set(got or [])):
            bad += 1
        elif depth == len(path) - 1 and sorted(want) == sorted(set(got or [])):
            exact += 1
        else:
            merged += 1
    return exact, merged, bad


def semantic_equal(a, b):
    """Compare two parsed files for the data the Rush 2 runtime actually reads."""
    diffs = []
    if a.segs != b.segs:
        diffs.append('segments')
    if len(a.nodes) != len(b.nodes):
        return ['node count']
    for i, (x, y) in enumerate(zip(a.nodes, b.nodes)):
        if (x['parent'], x['mask'] & 0xF, x['bounds']) != (y['parent'], y['mask'] & 0xF, y['bounds']):
            diffs.append('node %d header' % i)
            break
        for q in range(4):
            if x['mask'] & (1 << q):
                if x['child'][q] != y['child'][q]:
                    diffs.append('node %d child %d' % (i, q))
            else:
                la = a.leaves[x['leaf'][q]][1] if x['leaf'][q] else b''
                lb = b.leaves[y['leaf'][q]][1] if y['leaf'][q] else b''
                if la != lb:
                    diffs.append('node %d leaf %d contents' % (i, q))
    if len(a.polys) != len(b.polys):
        return diffs + ['poly count']
    for i, (x, y) in enumerate(zip(a.polys, b.polys)):
        if (x['flags'], x['matrix'], x['verts'], x['seg']) != (y['flags'], y['matrix'], y['verts'], y['seg']):
            diffs.append('poly %d' % i)
            break
    if a.verts != b.verts:
        diffs.append('vertices')
    return diffs


# ----------------------------------------------------------------------------------------------------------------
# self-check over both ROMs

def selfcheck():
    import roms
    ok = True
    r2 = roms.Rush2()
    r49 = roms.Rush2049()
    names = ['VEGAS', 'NYONE', 'HAWAII', 'NYTWO', 'ALCATRAZ', 'LA', 'SEATTLE', 'HALFPIPE', 'CRASH', 'PIPE', 'ATARI',
             'STUNT1']
    print('Rush 2 (assets 0x4B-0x56)')
    for t in range(12):
        data = r2.asset(0x4B + t)
        c = Collision.parse(data, R2)
        errs = c.validate()
        st = c.stats()
        rebuilt = c.build_rush2()
        same = rebuilt == data
        sem = semantic_equal(c, Collision.parse(rebuilt, R2))
        ok &= not errs and not sem
        print('  %2d %-8s seg %4d node %4d poly %4d vert %5d leafB %5d vlistB %5d  %s  rebuild %s  |localY| max %.3f'
              % (t, names[t], len(c.segs), len(c.nodes), len(c.polys), len(c.verts), len(c.leaf_blob),
                 len(c.vlist_blob), 'OK' if not errs else 'ERR ' + '; '.join(errs[:3]),
                 'byte-identical' if same else ('semantic-OK' if not sem else 'DIFF ' + ','.join(sem[:3])),
                 st['local_y_max']), st['types'])
    print('Rush 2049 (files 139-157)')
    for k in range(139, 158):
        data = r49.file(k)
        c = Collision.parse(data, R49)
        errs = c.validate()
        st = c.stats()
        line = '  %3d seg %4d node %4d poly %4d vert %5d movers %3d (ids %2d, at rest %3d, on v0 %3d) leafB %6d ' \
               '(dedup %6d) vlistB %5d  %s' % (k, len(c.segs), len(c.nodes), len(c.polys), len(c.verts),
                                               st['movers'], st['mover_ids'], st['movers_at_rest'],
                                               st['movers_on_origin_vertex'], len(c.leaf_blob),
                                               st['unique_leaf_bytes'], len(c.vlist_blob),
                                               'OK' if not errs else 'ERR ' + '; '.join(errs[:3]))
        ok &= not errs
        try:
            rep = {}
            out = convert(data, report=rep)
            o = Collision.parse(out, R2)
            oerrs = o.validate()
            src = Collision.parse(data, R49)
            for p in src.polys:
                p['info'] = convert_info(p['info'])
            if rep['nodes_merged']:
                ex, mg, bad = cover_check(src, o)
                sem = semantic_equal(src, o)
                sem = [d for d in sem if not d.startswith('node')] + (['%d leaves lost polygons' % bad] if bad else [])
                how = 'merged %d nodes (%d leaves exact, %d merged, superset OK)' % (rep['nodes_merged'], ex, mg)                     if not sem else 'DIFF ' + ','.join(sem[:3])
            else:
                sem = semantic_equal(src, o)
                how = 'semantic-OK' if not sem else 'DIFF ' + ','.join(sem[:3])
            line += '  -> rush2 0x%X bytes %s %s; dropped %d movers, %d force + %d platform polys' % (
                len(out), 'valid' if not oerrs else 'INVALID ' + '; '.join(oerrs[:3]), how,
                rep['movers_dropped'], rep['force_polys'], rep['platform_polys'])
            ok &= not oerrs and not sem
        except ValueError as e:
            line += '  -> CONVERT FAILED: %s' % e
            ok = False
        print(line, st['types'])
    print('ALL OK' if ok else 'PROBLEMS FOUND')
    return ok


if __name__ == '__main__':
    sys.exit(0 if selfcheck() else 1)
