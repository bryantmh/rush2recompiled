"""Mario Kart 64 (US) ROM reader: the pieces a course is loaded from, as the mk64 decomp describes them.

Everything here follows the decomp (github.com/n64decomp/mk64), mainly src/racing/memory.c load_course():

  gCourseTable[course] (courses/courseTable.h, 0x30 bytes each)
      +0x00/+0x04 dl ROM start/end       MIO0, decompressed into segment 6 (course_data.c: DLs, paths, ...)
      +0x08/+0x0C vertex ROM start/end   raw, DMAed as segment 0xF: the MIO0 vertices, then the packed DLs
      +0x10/+0x14 offsets ROM start/end  raw, segment 9 (course_offsets.c: the texture table)
      +0x18 vertexStart (0x0F.......)    MIO0 CourseVtx (s16 ob[3], s16 tc[2], u8 ca[4]; 14 bytes)
      +0x1C vertexCount
      +0x20 packedStart (0x0F.......)    packed display lists, unpacked into segment 7 (displaylist_unpack)
      +0x24 finalDisplaylistOffset, +0x28 textures (0x09......), +0x2C unknown1
  textures: course_texture {seg addr in _other_textures, compressed size, size, 0}, each MIO0, laid end to end in
      segment 5 in table order (decompress_textures / MIO0_0F).

Tables are found by their shape rather than fixed addresses, so the reader doesn't depend on a build map; what only
the game code knows about a course is in courses.toml.
"""
import struct

COURSE_NAMES = ['mario_raceway', 'choco_mountain', 'bowsers_castle', 'banshee_boardwalk', 'yoshi_valley',
                'frappe_snowland', 'koopa_troopa_beach', 'royal_raceway', 'luigi_raceway', 'moo_moo_farm',
                'toads_turnpike', 'kalimari_desert', 'sherbet_land', 'rainbow_road', 'wario_stadium', 'block_fort',
                'skyscraper', 'double_deck', 'dks_jungle_parkway', 'big_donut']

PATH_END = b'\x80\x00\x80\x00\x80\x00'      # { -32768, -32768, -32768 }: the end of a track path
X_END = b'\x80\x00'                         # any path's terminator starts with x = -32768


def load_rom(path):
    """The ROM in big-endian (.z64) order, whatever order the file is in."""
    data = bytearray(open(path, 'rb').read())
    magic = data[:4]
    if magic == b'\x37\x80\x40\x12':            # .v64/.n64 byte-swapped
        data[0::2], data[1::2] = data[1::2], data[0::2]
    elif magic == b'\x40\x12\x37\x80':          # little-endian words
        for i in range(0, len(data), 4):
            data[i:i + 4] = data[i:i + 4][::-1]
    elif magic != b'\x80\x37\x12\x40':
        raise ValueError('%s is not an N64 ROM' % path)
    if data[0x3B:0x3F] != b'NKTE':
        raise ValueError('%s is not Mario Kart 64 (US): game code %r' % (path, bytes(data[0x3B:0x3F])))
    return bytes(data)


def mio0(data, off=0):
    """Decompresses the MIO0 block at data[off] (header: 'MIO0', size, compressed offset, raw offset)."""
    if data[off:off + 4] != b'MIO0':
        raise ValueError('no MIO0 header at 0x%X' % off)
    size, comp, raw = struct.unpack_from('>III', data, off + 4)
    out = bytearray()
    bits, bit_count = 0, 0
    lay = off + 16
    comp += off
    raw += off
    while len(out) < size:
        if bit_count == 0:
            bits = struct.unpack_from('>I', data, lay)[0]
            lay += 4
            bit_count = 32
        if bits & 0x80000000:
            out.append(data[raw])
            raw += 1
        else:
            v = struct.unpack_from('>H', data, comp)[0]
            comp += 2
            dist = (v & 0xFFF) + 1
            for _ in range((v >> 12) + 3):
                out.append(out[-dist])
        bits = (bits << 1) & 0xFFFFFFFF
        bit_count -= 1
    return bytes(out[:size])


class CourseEntry:
    FIELDS = ('dl_start', 'dl_end', 'vtx_start', 'vtx_end', 'ofs_start', 'ofs_end', 'vertex_start', 'vertex_count',
              'packed_start', 'final_dl_offset', 'textures', 'unknown1')

    def __init__(self, rom, off):
        vals = struct.unpack_from('>11IH', rom, off)
        for k, v in zip(self.FIELDS, vals):
            setattr(self, k, v)


def find_course_table(rom):
    """Offset of gCourseTable: 20 entries whose dl and vertex ROM ranges are ordered, in the ROM, and start with
    MIO0 (dl) and whose vertexStart / packedStart are segment 0xF addresses."""
    def ok(off):
        e = CourseEntry(rom, off)
        return (e.dl_start < e.dl_end <= len(rom) and e.vtx_start < e.vtx_end <= len(rom)
                and e.ofs_start < e.ofs_end <= len(rom) and rom[e.dl_start:e.dl_start + 4] == b'MIO0'
                and e.vertex_start >> 24 == 0x0F and e.packed_start >> 24 == 0x0F and 0 < e.vertex_count < 0x10000)
    for off in range(0x1000, 0x200000, 4):
        if all(ok(off + i * 0x30) for i in range(len(COURSE_NAMES))):
            return off
    raise ValueError('gCourseTable not found')


def find_texture_base(rom, entries):
    """ROM start of _other_textures: the MIO0 header for which every texture-table entry lands on a MIO0 too."""
    first = entries[0][0] & 0xFFFFFF
    pos = rom.find(b'MIO0')
    while pos >= 0:
        base = pos - first
        if base >= 0 and all(rom[base + (a & 0xFFFFFF):base + (a & 0xFFFFFF) + 4] == b'MIO0' for a, _, _ in entries):
            return base
        pos = rom.find(b'MIO0', pos + 4)
    raise ValueError('texture bank not found')


def unpack_displaylists(packed):
    """displaylist_unpack(): the packed course DL bytes -> segment 7 as a list of (w0, w1) Gfx words. Segment 7
    offsets (0x07xxxxxx, the decomp's d_course_*_packed_dl_<offset>) index this list at 8 bytes a command. Opcodes are
    memory.c's enum PackedOp; the mirror-mode index orders aren't needed (the course is built unmirrored)."""
    out = []
    p = 0

    def arg():
        nonlocal p
        v = packed[p]
        p += 1
        return v
    G_VTX, G_TRI1, G_TRI2, G_QUAD, G_DL, G_ENDDL = 0x04, 0xBF, 0xB1, 0xB5, 0x06, 0xB8
    tilecfg = {0x1A: (32, 32, 0, 0), 0x2C: (32, 32, 0, 256), 0x1B: (64, 32, 0, 0), 0x1C: (32, 64, 0, 0),
               0x1D: (32, 32, 3, 0), 0x1E: (64, 32, 3, 0), 0x1F: (32, 64, 3, 0)}
    loadblock = {0x20: (32, 32, 0), 0x21: (64, 32, 0), 0x22: (32, 64, 0), 0x23: (32, 32, 3), 0x24: (64, 32, 3),
                 0x25: (32, 64, 3)}
    combine = {0x15: (0xFC127E24, 0xFFFFF9FC),   # G_CC_MODULATERGBA x2
               0x16: (0xFC121824, 0xFF33FFFF),   # G_CC_MODULATERGBDECALA x2
               0x17: (0xFCFFFFFF, 0xFFFE793C),   # G_CC_SHADE x2
               0x2E: (0xFC121824, 0xFF33FFFF),
               0x53: (0xFC127FFF, 0xFFFFF3F8)}   # G_CC_DECALRGBA x2
    rmode = {0x18: 0x00552078, 0x19: 0x00553078, 0x2F: 0x005049D8, 0x54: 0x00552D58, 0x55: 0x00504DD8}
    while p < len(packed):
        op = arg()
        if op == 0xFF:
            break
        if op <= 0x14:              # lights: 3 commands
            out += [(0xBC000002, 0x80000040), (0x03860010, 0x09000008 + op * 0x18), (0x03880010, 0x09000000 + op * 0x18)]
        elif op in combine:
            out.append(combine[op])
        elif op in rmode:
            out.append((0xB900031D, rmode[op]))
        elif op in tilecfg:
            w, h, fmt, tmem = tilecfg[op]
            a, b = arg(), arg()
            cms, masks, cmt, maskt = a & 0xF, a >> 4, b & 0xF, b >> 4
            line = (w * 2 + 7) >> 3
            out.append((0xE7000000, 0))
            out.append((0xF5000000 | fmt << 21 | 2 << 19 | line << 9 | tmem, cmt << 18 | maskt << 14 | cms << 8 | masks << 4))
            out.append((0xF2000000, ((w - 1) << 2) << 12 | ((h - 1) << 2)))
        elif op in loadblock:
            w, h, fmt = loadblock[op]
            addr = 0x05000000 + (arg() << 11)
            arg()
            a = arg()
            tmem, tile = a & 0xF, a >> 4
            out.append((0xFD000000 | fmt << 21 | 2 << 19, addr))
            out.append((0xE7000000, 0))
            out.append((0xF5000000 | fmt << 21 | 2 << 19 | tmem, tile << 24))
            out.append((0xE6000000, 0))
            dxt = (2048 + (w * 2 // 8) - 1) // (w * 2 // 8)
            out.append((0xF3000000, tile << 24 | min(w * h - 1, 0x7FF) << 12 | dxt))
        elif op == 0x26:
            out.append((0xBB000001, 0xFFFFFFFF))
        elif op == 0x27:
            out.append((0xBB000000, 0x00010001))
        elif op == 0x28:            # vtx1
            lo, hi = arg(), arg()
            addr = ((hi << 8) | lo) * 0x10
            n = arg() & 0x3F
            v0 = arg() & 0x3F
            out.append((G_VTX << 24 | (v0 * 2) << 16 | ((n << 10) + (n * 0x10 - 1)), 0x04000000 + addr))
        elif 0x33 <= op <= 0x52:    # vtx2: count in the opcode, buffer index 0
            lo, hi = arg(), arg()
            n = op - 0x32
            out.append((G_VTX << 24 | ((n << 10) + (n * 0x10 - 1)), 0x04000000 + ((hi << 8) | lo) * 0x10))
        elif op == 0x29:            # tri1
            b0, b1 = arg(), arg()
            a, b, c = b0 & 0x1F, ((b0 >> 5) & 7) | (b1 & 3) << 3, (b1 >> 2) & 0x1F
            out.append((G_TRI1 << 24, a * 2 << 16 | b * 2 << 8 | c * 2))
        elif op == 0x58:            # tri2
            b0, b1 = arg(), arg()
            a, b, c = b0 & 0x1F, ((b0 >> 5) & 7) | (b1 & 3) << 3, (b1 >> 2) & 0x1F
            b0, b1 = arg(), arg()
            d, e, f = b0 & 0x1F, ((b0 >> 5) & 7) | (b1 & 3) << 3, (b1 >> 2) & 0x1F
            out.append((G_TRI2 << 24 | a * 2 << 16 | b * 2 << 8 | c * 2, d * 2 << 16 | e * 2 << 8 | f * 2))
        elif op == 0x30:            # quad (unpack_spline_3D)
            b0, b1, b2 = arg(), arg(), arg()
            t0 = b0 & 0x1F
            a3 = ((b0 >> 5) & 7) | (b1 & 3) << 3
            a2 = (b1 >> 2) & 0x1F
            a0 = ((b1 >> 7) & 1) | (b2 & 0xF) << 1
            out.append((G_QUAD << 24, a0 * 2 << 24 | t0 * 2 << 16 | a3 * 2 << 8 | a2 * 2))
        elif op == 0x2D:
            out.append((0xBE000000, 0x0000000E))
        elif op == 0x2A:
            out.append((G_ENDDL << 24, 0))
        elif op == 0x56:
            out.append((0xB7000000, 0x00002000))
        elif op == 0x57:
            out.append((0xB6000000, 0x00002000))
        elif op == 0x2B:
            lo, hi = arg(), arg()
            out.append((G_DL << 24, 0x07000000 + ((hi << 8) | lo) * 8))
        # anything else: ignored, as the game does
    return out


class Course:
    """One course loaded the way load_course() loads it: segments 4 (Vtx), 5 (textures), 6 (course data), 7 (the
    unpacked display lists) and 9 (offsets)."""

    def __init__(self, rom, index):
        self.rom = rom
        self.index = index
        e = self.entry = CourseEntry(rom, find_course_table(rom) + index * 0x30)
        self.seg6 = mio0(rom, e.dl_start)
        self.seg9 = rom[e.ofs_start:e.ofs_end]
        segf = rom[e.vtx_start:e.vtx_end]
        raw = mio0(segf, e.vertex_start & 0xFFFFFF)
        # func_802A86A8: CourseVtx -> Vtx; the low 2 bits of ca[0] and ca[1] are the vertex's flag.
        self.vtx = []
        for i in range(e.vertex_count):
            x, y, z, s, t, r, g, b, _ = struct.unpack_from('>5h4B', raw, i * 14)
            self.vtx.append({'pos': (x, y, z), 'uv': (s, t), 'rgba': [r & 0xFC, g & 0xFC, b, 0xFF],
                             'flag': (r & 3) | ((g << 2) & 0xC)})
        self.seg7 = unpack_displaylists(segf[e.packed_start & 0xFFFFFF:])
        entries = []
        off = e.textures & 0xFFFFFF
        while True:
            a, comp, size, _ = struct.unpack_from('>4I', self.seg9, off)
            if a == 0:
                break
            entries.append((a, comp, size))
            off += 16
        seg5 = bytearray()
        self.texture_files = []
        if entries:
            base = find_texture_base(rom, entries)
            for a, comp, size in entries:
                self.texture_files.append((len(seg5), base + (a & 0xFFFFFF), size))
                seg5 += mio0(rom, base + (a & 0xFFFFFF))[:size].ljust(size, b'\0')
        self.seg5 = bytes(seg5)
        self.segd = None        # common_data (load_segd), when an actor's render code needs it
        self.seg3 = bytearray() # actor textures (load_seg3)
        self.vtx6 = {}          # segment 6 Vtx loaded by actor DLs, by address (one dict per vertex)

    def load_segd(self, rom_offset):
        self.segd = mio0(self.rom, rom_offset)

    def load_seg3(self, textures, start=0x9000):
        """Segment 3 as init_actors() fills it: dma_textures(texture, compressed size, alloc) for each (ROM offset of
        the MIO0, alloc), each decompressed into the next alloc bytes. The common textures (shells, finish banners)
        fill the first 0x9000 bytes; only the course's own list matters to its actors."""
        seg = bytearray(start)
        for rom_offset, alloc in textures:
            seg += mio0(self.rom, rom_offset)[:alloc].ljust(alloc, b'\0')
        self.seg3 = seg

    def vtx_at(self, addr):
        """A segment 6 Vtx (s16 ob[3], u16 flag, s16 tc[2], u8 cn[4]) as a vertex dict, the same dict each time."""
        v = self.vtx6.get(addr)
        if v is None:
            x, y, z, _, s, t, r, g, b, a = struct.unpack_from('>3hH2h4B', self.seg6, addr & 0xFFFFFF)
            v = self.vtx6[addr] = {'pos': (x, y, z), 'uv': (s, t), 'rgba': [r, g, b, a], 'flag': 0}
        return v

    # --- segmented reads ---
    def gfx_at(self, addr):
        """Iterates the Gfx commands from a segmented address (segments 6 and 7)."""
        seg, off = addr >> 24, addr & 0xFFFFFF
        if seg == 7:
            i = off // 8
            while i < len(self.seg7):
                yield self.seg7[i]
                i += 1
        elif seg == 6:
            while off + 8 <= len(self.seg6):
                yield struct.unpack_from('>II', self.seg6, off)
                off += 8
        else:
            raise ValueError('display list in segment %d' % seg)

    def texture_bytes(self, addr, size):
        seg, off = addr >> 24, addr & 0xFFFFFF
        src = {3: self.seg3, 5: self.seg5, 6: self.seg6, 0xD: self.segd}[seg]
        return bytes(src[off:off + size]).ljust(size, b'\0')

    def set_vertex_colors(self, dl, alpha, r, g, b):
        """find_vtx_and_set_colours(): every vertex a DL (and the DLs it calls) loads gets the alpha, and the color
        when red isn't 0."""
        for w0, w1 in self.gfx_at(dl):
            op = w0 >> 24
            if op == 0xB8:
                break
            if op == 0x06:
                self.set_vertex_colors(w1, alpha, r, g, b)
            elif op == 0x04:
                first = (w1 & 0xFFFFFF) // 16
                for v in self.vtx[first:first + ((w0 >> 10) & 0x3F)]:
                    if r:
                        v['rgba'][:3] = [r, g, b]
                    v['rgba'][3] = alpha

    # --- display list walking ---
    def walk(self, dl, emit, state=None, depth=0):
        """Runs a display list like the RSP/RDP would for geometry: emit(state, (v0, v1, v2)) for every triangle
        with vertex dicts and the texture/render state current at the draw. Returns the state at the end (MK64's
        section DLs set up textures in one DL and draw in the next, so the state carries across calls)."""
        if state is None:
            state = new_state()
        if depth > 16:
            return state
        for w0, w1 in self.gfx_at(dl):
            op = w0 >> 24
            if op == 0xB8:                      # G_ENDDL
                break
            elif op == 0x06:                    # G_DL (a branch when the push flag says so)
                self.walk(w1, emit, state, depth + 1)
                if (w0 >> 16) & 0xFF == 1:
                    break
            elif op == 0x04:                    # G_VTX (F3DEX)
                n = (w0 >> 10) & 0x3F
                v0 = ((w0 >> 16) & 0xFF) // 2
                first = (w1 & 0xFFFFFF) // 16
                if w1 >> 24 == 4:
                    for k in range(n):
                        if v0 + k < 32 and first + k < len(self.vtx):
                            state['buf'][v0 + k] = self.vtx[first + k]
                elif w1 >> 24 == 6:         # an actor's Vtx
                    for k in range(n):
                        if v0 + k < 32 and (w1 & 0xFFFFFF) + 16 * k + 16 <= len(self.seg6):
                            state['buf'][v0 + k] = self.vtx_at(w1 + 16 * k)
            elif op == 0xBF:                    # G_TRI1
                self._tri(state, emit, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF)
            elif op == 0xB1:                    # G_TRI2
                self._tri(state, emit, (w0 >> 16) & 0xFF, (w0 >> 8) & 0xFF, w0 & 0xFF)
                self._tri(state, emit, (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF)
            elif op == 0xB5:                    # G_QUAD: (b2 b1 b0), (b2 b0 b3) as set_vtx_from_quadrangle
                a, b, c, d = (w1 >> 16) & 0xFF, (w1 >> 8) & 0xFF, w1 & 0xFF, (w1 >> 24) & 0xFF
                self._tri(state, emit, a, b, c)
                self._tri(state, emit, a, c, d)
            elif op == 0xFD:                    # G_SETTIMG
                state['timg'] = ((w0 >> 21) & 7, (w0 >> 19) & 3, w1)
            elif op == 0xF5:                    # G_SETTILE
                tile = (w1 >> 24) & 7
                state['tiles'][tile] = {'fmt': (w0 >> 21) & 7, 'siz': (w0 >> 19) & 3, 'line': (w0 >> 9) & 0x1FF,
                                        'tmem': w0 & 0x1FF, 'cmt': (w1 >> 18) & 3, 'maskt': (w1 >> 14) & 0xF,
                                        'cms': (w1 >> 8) & 3, 'masks': (w1 >> 4) & 0xF}
            elif op == 0xF3:                    # G_LOADBLOCK: the image now sits at the load tile's TMEM address
                t = state['tiles'].get((w1 >> 24) & 7)
                if t:
                    state.setdefault('tmem', {})[t['tmem']] = state['timg']
            elif op == 0xF0:                    # G_LOADTLUT: the palette (gDPLoadTLUT_pal256/16)
                state['tlut'] = state['timg'][2]
            elif op == 0xBA and ((w0 >> 8) & 0xFF) == 14:     # G_SETOTHERMODE_H G_MDSFT_TEXTLUT
                state['tlut_on'] = bool(w1)
            elif op == 0xF2:                    # G_SETTILESIZE
                tile = (w1 >> 24) & 7
                state['tilesize'][tile] = (((w1 >> 12) & 0xFFF) - ((w0 >> 12) & 0xFFF), (w1 & 0xFFF) - (w0 & 0xFFF))
            elif op == 0xFC:
                state['combine'] = (w0, w1)
            elif op == 0xB9:                    # G_SETOTHERMODE_L
                if ((w0 >> 8) & 0xFF) == 3:
                    state['rmode'] = w1
            elif op == 0xB7:
                state['geom'] |= w1
            elif op == 0xB6:
                state['geom'] &= ~w1
            elif op == 0xBB:
                state['texture_on'] = bool(w0 & 1)
        return state

    def _tri(self, state, emit, a, b, c):
        vs = [state['buf'][i // 2] if i // 2 < 32 else None for i in (a, b, c)]
        if None in vs:
            return
        emit(state, vs)

    def render_texture(self, state):
        """The render tile's texture (key, size, format, bytes, clamp/mirror flags) or None when untextured."""
        t = state['tiles'].get(0)
        if not t or not state['texture_on']:
            return None
        img = state.get('tmem', {}).get(t['tmem'])
        if img is None:
            return None
        size = state['tilesize'].get(0, (0, 0))
        w, h = (size[0] >> 2) + 1, (size[1] >> 2) + 1
        if t['masks'] and w > 1 << t['masks']:
            w = 1 << t['masks']
        if t['maskt'] and h > 1 << t['maskt']:
            h = 1 << t['maskt']
        bpp = {0: 4, 1: 8, 2: 16, 3: 32}[t['siz']]
        tex = {'key': (img[2], t['fmt'], t['siz'], w, h, t['cms'], t['cmt']), 'w': w, 'h': h, 'fmt': t['fmt'],
               'siz': t['siz'], 'data': self.texture_bytes(img[2], w * h * bpp // 8), 'cms': t['cms'], 'cmt': t['cmt']}
        if t['fmt'] == 2 and state.get('tlut'):     # CI: its RGBA16 palette
            tex['tlut'] = self.texture_bytes(state['tlut'], 512)
            tex['key'] += (state['tlut'],)
        return tex


def decode_texture(tex):
    """RGBA bytes of an RGBA16, IA16, IA8/4, I8/4 or CI8/4 texture (MK64's course textures are RGBA16 and IA16, its star
    sprite I4, its actors mostly CI8 with tex['tlut'] the RGBA16 palette)."""
    out = bytearray()
    d = tex['data']
    n = tex['w'] * tex['h']
    if tex['siz'] < 2:                          # 4 and 8 bit: a texel per nibble (high first) or byte
        def texel(i):
            if tex['siz'] == 1:
                return d[i] if i < len(d) else 0
            b = d[i // 2] if i // 2 < len(d) else 0
            return (b >> 4) if i % 2 == 0 else b & 0xF
        for i in range(n):
            v = texel(i)
            if tex['fmt'] == 2:                 # CI: an RGBA16 palette entry
                pal = tex.get('tlut', b'')
                c = struct.unpack_from('>H', pal, 2 * v)[0] if 2 * v + 2 <= len(pal) else 0
                r, g, b = (c >> 11) & 0x1F, (c >> 6) & 0x1F, (c >> 1) & 0x1F
                out += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 if c & 1 else 0))
            elif tex['fmt'] == 4:               # I
                i8 = v * 17 if tex['siz'] == 0 else v
                out += bytes((i8, i8, i8, i8))
            elif tex['fmt'] == 3:               # IA: 3+1 or 4+4 bits
                if tex['siz'] == 0:
                    i8, a = ((v >> 1) * 255) // 7, 255 if v & 1 else 0
                else:
                    i8, a = (v >> 4) * 17, (v & 0xF) * 17
                out += bytes((i8, i8, i8, a))
            else:
                raise ValueError('texture format %d/%d not handled' % (tex['fmt'], tex['siz']))
        return bytes(out)
    if tex['fmt'] == 4 and tex['siz'] == 0:     # I 4-bit: two texels a byte, high nibble first
        for i in range(tex['w'] * tex['h']):
            b = d[i // 2] if i // 2 < len(d) else 0
            v = ((b >> 4) if i % 2 == 0 else b & 0xF) * 17
            out += bytes((v, v, v, v))
        return bytes(out)
    for i in range(tex['w'] * tex['h']):
        v = struct.unpack_from('>H', d, i * 2)[0] if i * 2 + 2 <= len(d) else 0
        if tex['fmt'] == 0:                 # RGBA 5551
            r, g, b = (v >> 11) & 0x1F, (v >> 6) & 0x1F, (v >> 1) & 0x1F
            out += bytes(((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255 if v & 1 else 0))
        elif tex['fmt'] == 3:               # IA 8.8
            out += bytes((v >> 8, v >> 8, v >> 8, v & 0xFF))
        else:
            raise ValueError('texture format %d/%d not handled' % (tex['fmt'], tex['siz']))
    return bytes(out)


# --- segment 6 tables found by their shape (the decomp names them per course; the game reaches them from code) ---

def find_section_dls(blobs, seg6_len):
    """The per-camera-section display lists render_course_segments() draws: d_course_<name>_dl_list in segment 6, or
    <name>_dls in segment 9 (course_offsets.c) for the courses that keep it there. The longest run of 8-aligned
    segment 6 pointers in any of `blobs` (segment 9 first)."""
    best = []
    for blob in blobs:
        for start in range(0, len(blob) - 4, 4):
            run = []
            j = start
            while j + 4 <= len(blob):
                v = struct.unpack_from('>I', blob, j)[0]
                off = v & 0xFFFFFF
                if v >> 24 != 6 or off % 8 or off + 8 > seg6_len:
                    break
                run.append(v)
                j += 4
            if len(run) > len(best):
                best = run
    return best


def new_state(rmode=None, combine=None):
    """A walk() state as the course's render code leaves it before the section lists (courses.toml render_mode)."""
    return {'timg': None, 'tiles': {}, 'tilesize': {}, 'combine': combine, 'rmode': rmode, 'geom': 0,
            'texture_on': True, 'buf': [None] * 32, 'tlut': None}


def find_track_sections(seg6, seg7_len):
    """d_course_<name>_addr: TrackSections { Gfx* (segment 7), u8 surfaceType, u8 sectionId, u16 flags } ending in a
    zero entry (parse_course_displaylists). The longest such run; returns (addr, surface, section, flags) tuples."""
    best = []
    for i in range(0, len(seg6) - 8, 4):
        run = []
        j = i
        while j + 8 <= len(seg6):
            addr, surface, section, flags = struct.unpack_from('>IBBH', seg6, j)
            if addr == 0:
                break
            if (addr >> 24 != 7 or (addr & 0xFFFFFF) % 8 or (addr & 0xFFFFFF) // 8 >= seg7_len
                    or 0x11 < surface < 0xFC):      # include/mk64.h SURFACE_TYPE
                run = []
                break
            run.append((addr, surface, section, flags))
            j += 8
        if len(run) > len(best):
            best = run
    return best


def find_track_path(seg6, start=None):
    """The course's first track path as (x, y, z): TrackPathPoints { s16 x, y, z; u16 section } up to
    { -32768, -32768, -32768 }. Without a start, it begins after the entry before it whose x is -32768 (another
    path's terminator)."""
    end = seg6.find(PATH_END)
    while end >= 0 and end % 2:
        end = seg6.find(PATH_END, end + 1)
    if start is None:
        start = end
        while start >= 8 and seg6[start - 8:start - 6] != X_END:
            start -= 8
    return [struct.unpack_from('>3h', seg6, k) for k in range(start, end, 8)]


def read_stars(rom, vram, vram_to_rom):
    """A StarData table (u16 rotY, s16 posY, u16 scalePercent, u16 subType) up to rotY 0xFFFF."""
    out = []
    o = vram - vram_to_rom
    while True:
        rot, y, scale, _ = struct.unpack_from('>HhHH', rom, o)
        if rot == 0xFFFF:
            return out
        out.append((rot, y, scale))
        o += 8
