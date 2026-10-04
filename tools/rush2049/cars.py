"""Rush 2 and Rush 2049 per-car data (research aid for docs/rush2049_research/cars.md).

Usage (from tools/rush2049):
    python cars.py r2          Rush 2: 11 physics descriptors, the type->descriptor map and the per-car tables
    python cars.py 49          Rush 2049: 13 cars, descriptors, per-car tables, setup tables, transmissions
    python cars.py convert     each 2049 car (stock setup) expressed as a Rush 2 descriptor + Rush 2 table row
    python cars.py desc-map    field-by-field layout of the two descriptor formats

Addresses are N64 virtual addresses (Rush 2 main/overlay through roms.Rush2, 2049 main through roms.Rush2049).
"""
import struct, sys
import roms

F = lambda w: struct.unpack('>f', struct.pack('>I', w))[0]
W = lambda f: struct.unpack('>I', struct.pack('>f', f))[0]

# ---------------------------------------------------------------------------------------------------------------
# Descriptor layouts. Rush 2: 0xEC bytes (11 at 0x800BFC90, pointer table 0x800C07BC, type -> index u8 0x800C07E8).
# Rush 2049: 0xB4 bytes (6 at 0x801108D0, pointer per car type at 0x80110D08[13]).
# Each entry: (Rush 2 offset, 2049 offset or None, kind, meaning). kind: f = f32, p = pointer, h = s16 + pad, x = word
DESC_MAP = [
    (0x00, 0x00, 'f', 'inertia / chassis constant (1527)'),
    (0x04, 0x04, 'f', 'inertia / chassis constant (1636)'),
    (0x08, 0x08, 'f', 'inertia / chassis constant (1000)'),
    (0x0C, 0x0C, 'f', 'inverse inertia (6.549e-4)'),
    (0x10, 0x10, 'f', 'inverse inertia (6.112e-4)'),
    (0x14, 0x14, 'f', 'inverse inertia (0.001)'),
] + [(o, o, 'f', 'suspension spring/damper pair (front/rear), see func_8008D7E4/func_800D0894') for o in range(0x18, 0x50, 4)] + [
    (0x50, 0x50, 'f', 'suspension constant (16), func_8008D7E4'),
    (0x54, 0x54, 'f', 'suspension constant (4), func_8008D7E4'),
    (0x58, None, 'x', 'R2 only: 0x00100020; no reader found in car code (default: copy Rush 2 value)'),
    (0x5C, None, 'x', 'R2 only: 0x01000000; no reader found in car code (default: copy Rush 2 value)'),
    (0x60, 0x58, 'f', 'aero drag coefficient (0.0135), func_8006AFD8 / func_800E23A4'),
    (0x64, 0x5C, 'f', 'rolling resistance (75), func_8006AFD8 / func_800E23A4'),
    (0x68, 0x60, 'p', 'tire curve, wheel 0 (front)'),
    (0x6C, 0x64, 'p', 'tire curve, wheel 1 (front)'),
    (0x70, 0x68, 'p', 'tire curve, wheel 2 (rear)'),
    (0x74, 0x6C, 'p', 'tire curve, wheel 3 (rear)'),
    (0x78, None, 'f', 'R2 only: rear lateral grip factor (func_8006ABB0). 2049: player+0x5B0 = 0x8011121C[setup D] '
                      'times (1 - trans+0x1C * 0x80123E18) in func_800BCBB8'),
] + [(0x7C + i * 4, 0x70 + i * 4, 'f', 'wheel %d %s' % (i // 3, 'xyz'[i % 3])) for i in range(12)] + [
    (0xAC, 0xA0, 'f', 'drivetrain inertia factor (2.5), clutch func_80070730 / func_800E2AC4'),
    (0xB0, 0xA4, 'f', 'final drive ratio (3.3), func_800709E8 / func_800E2C70'),
    (0xB4, 0xA8, 'f', 'clutch slip torque (R2 600, 2049 1000), func_80070730 / func_800E2AC4'),
    (0xB8, None, 'f', 'R2 only: torque scale in gear 1 (func_80070DB8). 2049: trans+8 * 0x801110C4[C][B], all gears'),
    (0xBC, None, 'f', 'R2 only: torque scale in gear 2'),
    (0xC0, None, 'f', 'R2 only: torque scale in gear 3+'),
    (0xC4, None, 'f', 'R2 only: torque scale on the common map (car+0x604/0x608 == 1|2 above 40 ft/s)'),
    (0xC8, None, 'f', 'R2 only: gear ratio reverse (-3.1). 2049: 0x80110EBC[player+9][0] * trans+0xC'),
    (0xCC, None, 'f', 'R2 only: gear ratio neutral (0)'),
    (0xD0, None, 'f', 'R2 only: gear ratio 1st'),
    (0xD4, None, 'f', 'R2 only: gear ratio 2nd'),
    (0xD8, None, 'f', 'R2 only: gear ratio 3rd'),
    (0xDC, None, 'f', 'R2 only: gear ratio 4th'),
    (0xE0, None, 'h', 'R2 only: s16 top gear (4); 2049 uses the literal 4 (func_800E313C slti 4)'),
    (0xE4, 0xAC, 'f', 'auto-shift up threshold, engine rad/s (662.35 = 6325 rpm), func_800711F4 / func_800E31D4'),
    (0xE8, 0xB0, 'f', 'auto-shift down threshold (429.35 = 4100 rpm)'),
]


class R2Cars:
    N = 22
    NAMES = 0x800C0764
    DESC_PTRS = 0x800C07BC
    DESC_INDEX = 0x800C07E8
    # (address, element kind, rows, meaning) for the per-type physics tables read by func_8008DBA0 and helpers
    TABLES = [
        (0x800C06B4, 'f', 1, 'steer/yaw force base -> car+0x5A0 (func_8008D984)'),
        (0x800C070C, 'f', 1, 'yaw damping base -> car+0x5A4 (func_8008D9EC)'),
        (0x800C0800, 'f', 1, 'suspension preload factor -> car+0x3C0..0x3CC'),
        (0x800C0858, 'f', 1, 'mass in slugs -> car+0x5B0 (func_8008DB04)'),
        (0x800C08B0, 'f', 1, 'pitch/roll inertia -> car+0x5B4'),
        (0x800C090C, 'f', 1, 'yaw inertia -> car+0x644'),
        (0x800C0964, 'f', 3, 'setup weight value; row 1/2 minus row 0 adjusts mass, 0x5B4, 0x644'),
        (0x800C0A6C, 'f', 3, 'front wheel model scale (func_8005A598)'),
        (0x800C0B74, 'f', 3, 'rear wheel model scale (func_8005A598)'),
        (0x800C0CD0, 'b', 3, 'engine torque map index -> 0x800C0CC0[i] -> car+0x3FC'),
        (0x800C0D14, 'b', 3, 'drive flags: car+0x59C = &1, car+0x59D = &1'),
        (0x800C0D68, 'b', 3, 'setup value; row delta * 10 adds to car+0x5A4'),
        (0x800C0DAC, 'b', 3, 'handling setting 0..10 -> 0x800C0DF0/0x800C0E1C, car+0x5A8'),
        (0x800C0E48, 'b', 3, 'car+0x59E (also read by model/colour code)'),
    ]
    BOX = 0x800CAC6C       # 22 x {front extent, rear extent, half width, height}

    def __init__(self):
        self.r = roms.Rush2()

    def name(self, t):
        return self.r.s(self.r.w(self.NAMES + 4 * t))

    def desc_addr(self, t):
        return self.r.w(self.DESC_PTRS + 4 * self.r.read(self.DESC_INDEX + t, 1)[0])

    def desc(self, t):
        return self.r.read(self.desc_addr(t), 0xEC)

    def table(self, addr, kind, t, row=0):
        if kind == 'f':
            return self.r.f(addr + row * self.N * 4 + 4 * t)
        return struct.unpack('>b', self.r.read(addr + row * self.N + t, 1))[0]

    def box(self, t):
        return [self.r.f(self.BOX + 16 * t + 4 * i) for i in range(4)]


class R49Cars:
    N = 13
    DESC_PTRS = 0x80110D08      # ptr[13] -> 0xB4-byte descriptors at 0x801108D0 (6 distinct)
    PREFIX = 0x80110D3C         # "CAR1".."CAR13"
    DISPLAY = 0x80110D70        # "FORMULA 1".."PANTHER"
    PRELOAD = 0x80110DA4        # f32[13] -> player+0x3DC.. (Rush 2 0x800C0800)
    MASS = 0x80110DD8           # f32[13] slugs (Rush 2 0x800C0858)
    INERTIA = 0x80110E0C        # f32[13] (Rush 2 0x800C08B0)
    YAW_INERTIA = 0x80110E44    # f32[13] (Rush 2 0x800C090C)
    GEARSETS = 0x80110EBC       # f32[8][6]: reverse, neutral, 1st..4th; index player+9
    GEAR_GRIP = 0x80110F80      # f32[8][6]: rear long. grip blend per gear (func_800BCBB8)
    TORQUE = 0x801110C4         # f32[9][3]: torque scale [setup C][setup B] (func_800E2D18)
    REAR_GRIP = 0x80111130      # f32[9][3]: rear long. grip scale [C][B] (func_800BCBB8)
    YAW = 0x801111E0            # f32[5][3]: player+0x5AC base [D][B] (func_800D0A34)
    REAR_LAT = 0x8011121C       # f32[5]: player+0x5B0 [D]
    ENGINE = 0x80111274         # f32[6]: engine level value 0..1 [E]
    DRIVE = 0x801114E4          # u8[13]: player+0x5A8 = &1, +0x5A9 = &2 (Rush 2 0x800C0D14)
    YAW_ADJ = 0x8011156C        # u8[13]: 0 -> +0.05, 2 -> -0.05 on player+0x5AC
    TRANS = 0x801116D0          # 3 x 0x2C transmission/tuning entries, player+4 = &TRANS[B]
    ENGINE_MAP = 0x8011F724     # s16[10][12] torque map (player+0x40C, all cars)
    BOX = 0x8011F844            # 13 x {front extent, rear extent, half width, height}
    # per-player setup rows: 5 rows x 13 bytes; row 0 = drones, rows 1..4 = players 1..4 (copied from setup screens)
    SETUP_ROWS = {'B': 0x8011103C, 'C': 0x80111080, 'D': 0x8011119C, 'E': 0x80111230, 'wing': 0x8011128C,
                  'F': 0x8011157C, 'G': 0x801115C0}
    WHEEL_SCALE_F = 0x801112DC  # f32[5][13] (Rush 2 0x800C0A6C)
    WHEEL_SCALE_R = 0x801113E0  # f32[5][13] (Rush 2 0x800C0B74)
    # constants
    K_MASS = 0x80124150         # 31.0559 = 1000 / 32.2
    K_ENG0 = 0x80124154         # 0.4, engine value subtracted in the mass formula
    K_ENG1 = 0x80124158         # 0.4
    K_YAWI = 0x8012415C         # 100000

    def __init__(self):
        self.r = roms.Rush2049()

    def rd(self, a, n):
        o = a - self.r.MAIN_VRAM
        return self.r.main[o:o + n]

    def f(self, a): return F(self.r.w(a))
    def b(self, a): return struct.unpack('>b', self.rd(a, 1))[0]

    def s(self, a):
        o = a - self.r.MAIN_VRAM
        return self.r.main[o:self.r.main.index(b'\0', o)].decode('latin1')

    def prefix(self, t): return self.s(self.r.w(self.PREFIX + 4 * t))
    def display(self, t): return self.s(self.r.w(self.DISPLAY + 4 * t))
    def desc_addr(self, t): return self.r.w(self.DESC_PTRS + 4 * t)
    def desc(self, t): return self.rd(self.desc_addr(t), 0xB4)
    def setup(self, key, t, row=0): return self.b(self.SETUP_ROWS[key] + 13 * row + t)
    def trans(self, i): return [self.f(self.TRANS + 0x2C * i + 4 * k) for k in range(11)]
    def gearset(self, i): return [self.f(self.GEARSETS + 0x18 * i + 4 * k) for k in range(6)]
    def box(self, t): return [self.f(self.BOX + 16 * t + 4 * i) for i in range(4)]

    def stock_setup(self, t):
        """The setup a drone gets (func_800EC2F8 drone branch): row 0 of each table, gear set 0, torque row 0."""
        return {'9': 0, 'B': self.setup('B', t), 'C': 0, 'D': self.setup('D', t), 'E': self.setup('E', t),
                'F': self.setup('F', t)}

    def derived(self, t, s=None):
        """Values func_800D0BA0 / func_800D0B14 compute for car t with setup s (dict of setup bytes)."""
        s = s or self.stock_setup(t)
        tr = self.trans(s['B'])
        eng = self.f(self.ENGINE + 4 * s['E'])
        d = eng - self.f(self.K_ENG1)
        mass = (self.f(self.MASS + 4 * t) + (eng - self.f(self.K_ENG0)) * self.f(self.K_MASS)) * tr[1]
        yaw_adj = {0: 0.05, 2: -0.05}.get(self.b(self.YAW_ADJ + t), 0.0)
        steer = {4: 0.75, 3: 0.5, 1: -0.25, 2: -0.5}.get(s['D'], 0.0)
        return {
            'mass_slugs': mass,                                             # player+0x5C0 (R2 car+0x5B0)
            'inertia': self.f(self.INERTIA + 4 * t) + d * 1000.0,           # player+0x5C4 (R2 +0x5B4)
            'yaw_inertia': self.f(self.YAW_INERTIA + 4 * t) + d * self.f(self.K_YAWI),   # +0x63C (R2 +0x644)
            'preload': self.f(self.PRELOAD + 4 * t),                        # R2 0x800C0800
            'yaw_base': self.f(self.YAW + 12 * s['D'] + 4 * s['B']) + yaw_adj,   # +0x5AC
            'rear_lat': self.f(self.REAR_LAT + 4 * s['D']),                 # +0x5B0
            'steer_bias': steer,                                            # +0x5B4 (R2 +0x5A8)
            'torque_scale': tr[2] * self.f(self.TORQUE + 12 * s['C'] + 4 * s['B']),
            'rear_long': self.f(self.REAR_GRIP + 12 * s['C'] + 4 * s['B']) * tr[6],
            'gears': [g * tr[3] for g in self.gearset(s['9'])],
            'drive': self.rd(self.DRIVE + t, 1)[0],
            'trans': tr,
        }


def to_r2(c49, r2, t, s=None):
    """Express 2049 car t as a Rush 2 descriptor (bytes) and Rush 2 per-type table values.

    Data-only approximation (cars.md §3.4 option A): Rush 2's drivetrain, steering and rear-tire code differ from
    2049's, so the result drives close to, not exactly like, 2049. Tire curve pointers are left as 2049 addresses;
    the caller relocates them to Rush 2 copies (2049's rear curve differs from Rush 2's).
    """
    d49 = c49.desc(t)
    base = bytearray(r2.desc(0))
    dv = c49.derived(t, s)
    out = bytearray(0xEC)
    for o2, o49, kind, _ in DESC_MAP:
        if o49 is not None:
            out[o2:o2 + 4] = d49[o49:o49 + 4]
        else:
            out[o2:o2 + 4] = base[o2:o2 + 4]
    put = lambda o, v: out.__setitem__(slice(o, o + 4), struct.pack('>f', v))
    put(0x78, dv['rear_lat'] * (1.0 - dv['trans'][7] * 1.0))   # trans+0x1C is 0 in all three entries
    for o in (0xB8, 0xBC, 0xC0, 0xC4):
        put(o, dv['torque_scale'])
    for i, g in enumerate(dv['gears']):
        put(0xC8 + 4 * i, g)
    out[0xE0:0xE4] = struct.pack('>hh', 4, 0)
    tables = {
        0x800C06B4: None, 0x800C070C: None,   # steering model differs: see cars.md §3.3
        0x800C0800: dv['preload'], 0x800C0858: dv['mass_slugs'], 0x800C08B0: dv['inertia'],
        0x800C090C: dv['yaw_inertia'], 0x800C0D14: dv['drive'], 'box': c49.box(t),
    }
    return bytes(out), tables


def fmt(v):
    if isinstance(v, float): return '%g' % v
    return str(v)


def main():
    cmd = sys.argv[1] if len(sys.argv) > 1 else '49'
    if cmd == 'desc-map':
        for o2, o49, kind, m in DESC_MAP:
            print('R2 +%02X  49 %s  %s  %s' % (o2, '+%02X' % o49 if o49 is not None else ' --', kind, m))
        return
    if cmd == 'r2':
        c = R2Cars()
        print('type name    desc  ' + '  '.join('%08X' % a for a, _, _, _ in c.TABLES) + '  box')
        for t in range(c.N):
            idx = c.r.read(c.DESC_INDEX + t, 1)[0]
            print('%2d %-7s %2d  ' % (t, c.name(t), idx) +
                  '  '.join('%8s' % fmt(c.table(a, k, t)) for a, k, _, _ in c.TABLES) +
                  '  ' + ' '.join(fmt(x) for x in c.box(t)))
        print()
        for a, k, rows, m in c.TABLES: print('%08X %s x%d  %s' % (a, k, rows, m))
        return
    c = R49Cars()
    if cmd == '49':
        print('t  file prefix display    desc      B  C  D  E  F  wing G | mass   inertia yaw_I   torque rear_lat gears')
        for t in range(c.N):
            s = c.stock_setup(t); dv = c.derived(t)
            print('%2d %3d  %-6s %-10s %08X  %d  %d  %d  %d %2d  %d   %d | %.3f %.0f %.0f %.3f %.2f %s' % (
                t, 88 + t, c.prefix(t), c.display(t), c.desc_addr(t), s['B'], c.setup('C', t), s['D'], s['E'],
                s['F'], c.setup('wing', t), c.setup('G', t), dv['mass_slugs'], dv['inertia'], dv['yaw_inertia'],
                dv['torque_scale'], dv['rear_lat'], ' '.join('%.3g' % g for g in dv['gears'])))
        print('\nbox (front, rear, half width, height) / wheel scale front, rear (row 0):')
        for t in range(c.N):
            print('%2d %-10s %s   %g %g' % (t, c.display(t), ' '.join(fmt(x) for x in c.box(t)),
                                           c.f(c.WHEEL_SCALE_F + 4 * t), c.f(c.WHEEL_SCALE_R + 4 * t)))
        print('\ntransmission entries (player+4, 11 floats):')
        for i in range(3): print(i, ' '.join(fmt(x) for x in c.trans(i)))
        print('gear sets (player+9):')
        for i in range(8): print(i, ' '.join(fmt(x) for x in c.gearset(i)))
        print('descriptors:')
        seen = sorted(set(c.desc_addr(t) for t in range(c.N)))
        for a in seen:
            d = c.rd(a, 0xB4)
            print('%08X used by %s' % (a, [t for t in range(c.N) if c.desc_addr(t) == a]))
            print('   wheels', ' '.join('%g' % F(struct.unpack('>I', d[o:o + 4])[0]) for o in range(0x70, 0xA0, 4)))
        return
    if cmd == 'convert':
        r2 = R2Cars()
        for t in range(c.N):
            desc, tab = to_r2(c, r2, t)
            print('%2d %-10s' % (t, c.display(t)))
            print('   desc', desc.hex())
            print('   tables', ' '.join('%s=%s' % (('%08X' % k) if isinstance(k, int) else k,
                                                  fmt(v) if not isinstance(v, list) else v) for k, v in tab.items()))
        return
    print(__doc__)


if __name__ == '__main__':
    main()
