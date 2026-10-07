"""Per-car physics tables, part effects, stat bars and drivetrain simulations for docs/rush2049_research/car_physics.md.

python car_tables.py > out.md

Bars follow func_803B7F7C exactly; simulations follow the drivetrain in cars.md section 10 (full throttle, flat
ground, traction not limiting, 1/60 s steps, up-shift at 6325 rpm, torque map 0 at 9200 rpm).
"""
import struct
import roms
from cars import R2Cars, R49Cars

r2 = R2Cars()
c49 = R49Cars()
R = r2.r
Q = c49.r
N2 = 22
ROCKET = 20
CARS2 = [t for t in range(N2) if t != ROCKET]

MAPS = {'LOW': 0x800CA87C, 'STANDARD': 0x800CA96C, 'HIGH': 0x800CAA5C}
STEER_D = [R.f(0x800C0DF0 + 4 * i) for i in range(11)]
YAW_D = [R.f(0x800C0E1C + 4 * i) for i in range(11)]
K_WEIGHT = R.f(0x800CFBCC)   # 31.0559 slugs per weight unit


def full_row(addr, q=False):
    if q:
        return [struct.unpack('>h', struct.pack('>H', Q.w(addr + 2 * (9 * 12 + c)) >> 16))[0] for c in range(12)]
    return [R.h(addr + 2 * (9 * 12 + c)) for c in range(12)]


ROWS = {k: full_row(a) for k, a in MAPS.items()}
ROW49 = full_row(0x8011F724, True)


def torque_at(row, rpm):
    x = rpm / 1150.0
    i = int(x)
    if i >= 11:
        return row[11]
    return row[i] + (row[i + 1] - row[i]) * (x - i)


def simulate(mass, ks, gears, row, fd=3.3):
    """Returns (0-60 s, 0-100 s, top mph)."""
    v, t, g = 0.0, 0.0, 0
    t60 = t100 = None
    dt = 1 / 60.0
    vmax = 0.0
    while t < 120:
        rpm = 9.549 * v * gears[g] * fd
        while g < 3 and rpm >= 6325:
            g += 1
            rpm = 9.549 * v * gears[g] * fd
        k = ks[min(g, 2)]
        f = max(torque_at(row, rpm), 0) * k * gears[g] * fd
        drag = 0.0135 * v * v + (105 if v > 0 else 0)
        v = max(v + (f - drag) / mass * dt, 0)
        t += dt
        vmax = max(vmax, v)
        if t60 is None and v >= 88:
            t60 = t
        if t100 is None and v >= 146.667:
            t100 = t
    return t60, t100, vmax / 1.4667


def f(v, n=2):
    if v is None:
        return '-'
    return ('%.' + str(n) + 'f') % v


# ---- Rush 2 per-type values ----
def r2_desc(t):
    a = r2.desc_addr(t)
    d = {o: R.f(a + o) for o in range(0, 0xE0, 4)}
    d['idx'] = R.read(r2.DESC_INDEX + t, 1)[0]
    return d


def tab(addr, t, kind='f', row=0):
    return r2.table(addr, kind, t, row)


def wheels(d, off):
    w = [[d[off + 12 * i + 4 * k] for k in range(3)] for i in range(4)]
    return w[0][2] - w[2][2], 2 * abs(w[0][0]), 2 * abs(w[2][0])


R2 = []
for t in range(N2):
    d = r2_desc(t)
    tires = tab(0x800C0DAC, t, 'b')
    susp = tab(0x800C0D68, t, 'b')
    wb, tf, tr = wheels(d, 0x7C)
    R2.append(dict(
        t=t, name=r2.name(t), desc=d['idx'], mass=tab(0x800C0858, t), inertia=tab(0x800C08B0, t),
        yaw_i=tab(0x800C090C, t), preload=tab(0x800C0800, t), w0=tab(0x800C0964, t), steer0=tab(0x800C06B4, t),
        yaw0=tab(0x800C070C, t), tires=tires, susp=susp, tmap=tab(0x800C0CD0, t, 'b'),
        steer=max(tab(0x800C06B4, t) + STEER_D[tires], 0), yaw=max(tab(0x800C070C, t) + YAW_D[tires], 0),
        offroad={10: 0.75, 9: 0.5}.get(tires, 0.0), grip=d[0x78], k=[d[0xB8], d[0xBC], d[0xC0]], kc=d[0xC4],
        gears=[d[0xD0], d[0xD4], d[0xD8], d[0xDC]], clutch=d[0xB4], wb=wb, tf=tf, tr=tr, box=r2.box(t)))

M_MIN = min(c['mass'] + (0 - c['w0']) * K_WEIGHT for c in R2)
M_MAX = max(c['mass'] + (1 - c['w0']) * K_WEIGHT for c in R2)


def bars(mass, k1, k3, g4, steer, yaw, grip, offroad, rocket=False):
    acc = 0.9 * (1 - (mass - M_MIN) / (M_MAX - M_MIN)) + 0.1 * (k1 - 1.8) / 0.07
    if rocket:
        acc = acc * 0.3 + 0.85
        top = 1.0
    else:
        top = 0.27 * (1 - offroad) + 0.73 * 2.25 * (k3 / g4 - 1.8)
    return acc, top, steer / 1200.0, yaw / 60.0 + 1 - (grip - 0.8) / 0.2


MAPNAME = {0: 'LOW', 1: 'STANDARD', 2: 'HIGH', 3: 'STANDARD'}
for c in R2:
    c['bars'] = bars(c['mass'], c['k'][0], c['k'][2], c['gears'][3], c['steer'], c['yaw'], c['grip'], c['offroad'],
                     c['t'] == ROCKET)
    c['sim'] = simulate(c['mass'], c['k'], c['gears'], ROWS[MAPNAME[c['tmap']]])

# ---- 2049 ----
mean = lambda xs: sum(xs) / len(xs)
R2K = [mean([R2[t]['k'][i] for t in CARS2]) for i in range(3)]
R2G = [mean([R2[t]['gears'][i] for t in CARS2]) for i in range(4)]
R2STEER0 = mean([R2[t]['steer0'] for t in CARS2])
R2YAW0 = mean([R2[t]['yaw0'] for t in CARS2])
R2TIRES = round(mean([R2[t]['tires'] for t in CARS2]))
R2SUSP = round(mean([R2[t]['susp'] for t in CARS2]))

TRANS = [c49.trans(i) for i in range(3)]
TORQ49 = [[c49.f(c49.TORQUE + 12 * cc + 4 * b) for b in range(3)] for cc in range(9)]
RGRIP49 = [[c49.f(c49.REAR_GRIP + 12 * cc + 4 * b) for b in range(3)] for cc in range(9)]
REARLAT49 = [c49.f(c49.REAR_LAT + 4 * i) for i in range(5)]
FRAMEW = [c49.f(c49.ENGINE + 4 * i) for i in range(6)]
GS0 = c49.gearset(0)
K49 = 2.2
G49 = [g * TRANS[0][3] for g in GS0[2:6]]
TF = [R2K[i] / K49 for i in range(3)]
GF = [R2G[i] / G49[i] for i in range(4)]

C49 = []
for k in range(13):
    s = c49.stock_setup(k)
    dv = c49.derived(k)
    d = c49.desc(k)
    w = [struct.unpack('>f', d[0x70 + 4 * i:0x74 + 4 * i])[0] for i in range(12)]
    wb = w[2] - w[8]
    adj = {0: 0.05, 2: -0.05}.get(c49.b(c49.YAW_ADJ + k), 0.0)
    port_k = [K49 * TF[i] for i in range(3)]
    port_g = [G49[i] * GF[i] for i in range(4)]
    steer = max(R2STEER0 + STEER_D[R2TIRES], 0)
    yaw = max(R2YAW0 + YAW_D[R2TIRES], 0)
    C49.append(dict(
        k=k, name=c49.display(k), frame=s['E'], mass=dv['mass_slugs'], inertia=dv['inertia'], yaw_i=dv['yaw_inertia'],
        preload=dv['preload'], adj=adj, f=0.2 + adj, wb=wb, tf=2 * abs(w[0]), tr=2 * abs(w[6]), box=c49.box(k),
        port_k=port_k, port_g=port_g,
        bars=bars(dv['mass_slugs'], port_k[0], port_k[2], port_g[3], steer, yaw, 1.0, 0.0),
        sim_port=simulate(dv['mass_slugs'], port_k, port_g, ROWS['STANDARD']),  # TORQUE row copied from the Pickup
        sim_fast=simulate(dv['mass_slugs'], [K49] * 3, G49, ROWS['STANDARD']),
        sim_native=simulate(dv['mass_slugs'], [K49] * 3, G49, ROW49)))

out = []
p = out.append


def table(head, rows):
    p('| ' + ' | '.join(head) + ' |')
    p('|' + '---|' * len(head))
    for r in rows:
        p('| ' + ' | '.join(str(x) for x in r) + ' |')
    p('')


p('### Rush 2 cars: mass, inertia and layout')
p('')
table(['Type', 'Car', 'Desc', 'Mass (slugs)', 'Weight (lb)', 'Pitch/roll I', 'Yaw I', 'Preload', 'Default weight',
       'Wheelbase (ft)', 'Track F / R (ft)', 'Box F / R / half W'],
      [[c['t'], c['name'], c['desc'], f(c['mass'], 1), '%.0f' % (c['mass'] * 32.2), '%.0f' % c['inertia'],
        '%.0f' % c['yaw_i'], f(c['preload']), f(c['w0'], 1), f(c['wb'], 2), '%s / %s' % (f(c['tf']), f(c['tr'])),
        '%g / %g / %g' % tuple(c['box'][:3])] for c in R2])

p('### Rush 2 cars: steering, drift and drivetrain inputs (default parts)')
p('')
table(['Car', 'Steer base', 'Def. TIRES', 'Steer (+0x5A0)', 'Yaw base', 'Def. SUSP', 'Yaw gain (+0x5A4)',
       'Rear grip (+0x78)', 'Torque k1 / k2 / k3+', 'Gears 1 / 2 / 3 / 4', 'Def. TORQUE'],
      [[c['name'], '%.0f' % c['steer0'], c['tires'], '%.0f' % c['steer'], '%.0f' % c['yaw0'], c['susp'],
        '%.0f' % c['yaw'], f(c['grip'], 3), '%.3f / %.3f / %.3f' % tuple(c['k']),
        '%.2f / %.2f / %.2f / %.2f' % tuple(c['gears']), MAPNAME[c['tmap']] if c['tmap'] != 3 else 'ROCKET']
       for c in R2])

p('### Rush 2 cars: bars vs simulated performance (default parts)')
p('')
p('Bars as func_803B7F7C computes them (1.0 = full bar; m_min %.2f, m_max %.2f slugs). Simulated times: full throttle '
  'from rest, flat ground, no wheelspin.' % (M_MIN, M_MAX))
p('')
def fast2(c):
    return simulate(c['mass'], [c['k'][i] / TF[i] for i in range(3)], [c['gears'][i] / GF[i] for i in range(4)],
                    ROWS[MAPNAME[c['tmap']]])
table(['Car', 'ACCEL', 'TOP', 'CONTROL', 'DRIFT', 'Speeds R2: 0-60 (s)', '0-100 (s)', 'Top (mph)',
       'Speeds 2049: 0-60 (s)', '0-100 (s)', 'Top (mph)'],
      [[c['name']] + [f(b) for b in c['bars']] + [f(c['sim'][0]), f(c['sim'][1]), '%.0f' % c['sim'][2]] +
       [f(x) if i < 2 else '%.0f' % x for i, x in enumerate(fast2(c))] for c in R2])

p('### Rush 2049 cars: per-car values (stock setup)')
p('')
table(['Car', 'FRAME', 'Mass (slugs)', 'Weight (lb)', 'Pitch/roll I', 'Yaw I', 'Preload', 'Yaw adjust', 'f (yaw factor)',
       'Wheelbase (ft)', 'Track F / R (ft)', 'Box F / R / half W'],
      [[c['name'], c['frame'] + 1, f(c['mass'], 1), '%.0f' % (c['mass'] * 32.2), '%.0f' % c['inertia'],
        '%.0f' % c['yaw_i'], f(c['preload']), '%+.2f' % c['adj'], f(c['f']), f(c['wb']),
        '%s / %s' % (f(c['tf']), f(c['tr'])), '%g / %g / %g' % tuple(c['box'][:3])] for c in C49])

p('### Rush 2049 cars: drivetrain native vs port, bars in the port, simulated performance')
p('')
p('Native = 2049 stock (ENGINE 1, HANDLING 0): torque %.2f in every gear, gears %s. Port with Car Speeds = Rush 2: torque '
  '%s, gears %s (x Rush 2 mean / 2049 mean). Every 2049 car in the port has steer %.0f and yaw gain %.0f (Rush 2 means, '
  'TIRES %d), rear grip 1.0.' % (K49, ' / '.join('%.3f' % g for g in G49), ' / '.join('%.3f' % x for x in C49[0]['port_k']),
                                  ' / '.join('%.3f' % g for g in C49[0]['port_g']), max(R2STEER0 + STEER_D[R2TIRES], 0),
                                  max(R2YAW0 + YAW_D[R2TIRES], 0), R2TIRES))
p('')
table(['Car', 'ACCEL', 'TOP', 'CONTROL', 'DRIFT', 'Port, Speeds R2: 0-60 / 0-100 (s)', 'top (mph)',
       'Port, Speeds 2049: 0-60 / 0-100 (s)', 'top (mph)', 'Native 2049: 0-60 / 0-100 (s)', 'top (mph)'],
      [[c['name']] + [f(b) for b in c['bars']] +
       ['%s / %s' % (f(c['sim_port'][0]), f(c['sim_port'][1])), '%.0f' % c['sim_port'][2],
        '%s / %s' % (f(c['sim_fast'][0]), f(c['sim_fast'][1])), '%.0f' % c['sim_fast'][2],
        '%s / %s' % (f(c['sim_native'][0]), f(c['sim_native'][1])), '%.0f' % c['sim_native'][2]] for c in C49])

# ---- parts ----
p('### Rush 2 TIRES: every setting')
p('')
TL = ['S0 D0', 'S1 D0', 'S2 D0', 'S0 D1', 'S1 D1', 'S2 D1', 'S0 D2', 'S1 D2', 'S2 D2', 'HALF OFFRD', 'FULL OFFRD']
table(['Value', 'Label', 'Steer add (0x800C0DF0)', 'Yaw gain add (0x800C0E1C)', 'Off-road factor (+0x5A8)', 'TOP bar penalty'],
      [[i, TL[i], '%+.0f' % STEER_D[i], '%+.0f' % YAW_D[i], {10: 0.75, 9: 0.5}.get(i, 0), {10: 0.75, 9: 0.5}.get(i, 0)]
       for i in range(11)])

p('### Rush 2 SUSPENSION')
p('')
table(['Value', 'Yaw gain add (vs car default)', 'Cars with this default'],
      [[['LOOSE', 'NORMAL', 'TIGHT', 'WHEELIE'][v], '(value - default) x 10',
        ', '.join(c['name'] for c in R2 if c['susp'] == v)] for v in range(4)])

p('### Rush 2 TORQUE: full-throttle torque map (map units, x torque scale = engine torque)')
p('')
table(['rpm'] + ['%d' % (1150 * i) for i in range(12)],
      [[k] + row for k, row in ROWS.items()] + [['2049 (all cars)'] + ROW49])
avg = dict(mass=mean([R2[t]['mass'] for t in CARS2]))
table(['Map on the Rush 2 average car', '0-60 (s)', '0-100 (s)', 'Top (mph)'],
      [[k] + [f(x) if i < 2 else '%.0f' % x for i, x in enumerate(simulate(avg['mass'], R2K, R2G, row))]
       for k, row in ROWS.items()])

p('### Rush 2 DURABILITY: mass at each setting')
p('')
table(['Car', 'Default', 'Mass at 0 / default / 100 (slugs)', '0-60 at 0 / 100 (s)'],
      [[c['name'], '%.0f' % (c['w0'] * 100),
        '%.1f / %.1f / %.1f' % (c['mass'] - c['w0'] * K_WEIGHT, c['mass'], c['mass'] + (1 - c['w0']) * K_WEIGHT),
        '%s / %s' % (f(simulate(c['mass'] - c['w0'] * K_WEIGHT, c['k'], c['gears'], ROWS[MAPNAME[c['tmap']]])[0]),
                     f(simulate(c['mass'] + (1 - c['w0']) * K_WEIGHT, c['k'], c['gears'],
                                ROWS[MAPNAME[c['tmap']]])[0]))] for c in R2 if c['t'] != ROCKET])

p('### Rush 2049 ENGINE x HANDLING: torque scale and rear longitudinal grip')
p('')
table(['ENGINE', 'Torque H0 / H1 / H2', 'Rear long. grip H0 / H1 / H2', 'Native 0-60 / top (mph), H0, avg car'],
      [[cc + 1, ' / '.join('%.2f' % x for x in TORQ49[cc]), ' / '.join('%.2f' % x for x in RGRIP49[cc]),
        '%s / %.0f' % (f(simulate(102.48, [TORQ49[cc][0]] * 3, G49, ROW49)[0]),
                       simulate(102.48, [TORQ49[cc][0]] * 3, G49, ROW49)[2])] for cc in range(9)])

p('### Rush 2049 HANDLING (transmission entries 0x801116D0)')
p('')
rows = []
for b in range(3):
    tr = TRANS[b]
    g = [x * tr[3] for x in GS0[2:6]]
    s = simulate(102.48, [TORQ49[0][b]] * 3, g, ROW49)
    rows.append([b, '%.3f' % tr[3], ' / '.join('%.3f' % x for x in g), '%.2f' % TORQ49[0][b],
                 '%.2f' % (TORQ49[0][b] * tr[3]), '%.1f' % tr[9], ' '.join('%g' % x for x in tr), f(s[0]), '%.0f' % s[2]])
table(['HANDLING', 'Gear x', 'Gears 1-4', 'Torque (ENGINE 1)', 'Torque x gear factor', 'trans+0x24 (untraced)',
       'All 11 floats', '0-60 (s)', 'Top (mph)'], rows)

p('### Rush 2049 TIRES')
p('')
table(['TIRES', 'Rear lateral grip (0x8011121C)', 'Off-road factor (+0x5B4, = Rush 2 +0x5A8)', 'Rush 2 equivalent'],
      [[i + 1, '%.2f' % REARLAT49[i], {4: 0.75, 3: 0.5, 1: -0.25, 2: -0.5}.get(i, 0.0),
        ['street', 'drift (no Rush 2 equivalent)', 'more drift (none)', 'HALF OFFRD', 'FULL OFFRD'][i]]
       for i in range(5)])

p('### Rush 2049 FRAME (0x80111274): identical for every car')
p('')
B49 = ([c49.f(c49.MASS + 4 * k) for k in range(13)], [c49.f(c49.INERTIA + 4 * k) for k in range(13)],
       [c49.f(c49.YAW_INERTIA + 4 * k) for k in range(13)])
m0, i0, y0 = (x[0] for x in B49)
rows = []
for n, w in enumerate(FRAMEW):
    m = m0 + (w - 0.4) * K_WEIGHT
    s49 = simulate(m, [K49] * 3, G49, ROW49)
    rows.append([n + 1, '%.1f' % w, '%.1f' % m, '%.0f' % (m * 32.2), '%.0f' % (i0 + (w - 0.4) * 1000),
                 '%.0f' % (y0 + (w - 0.4) * 100000), f(s49[0]), ', '.join(c['name'] for c in C49 if c['frame'] == n)])
table(['FRAME', 'Weight w', 'Mass (slugs)', 'Weight (lb)', 'Pitch/roll I', 'Yaw I', 'Native 0-60 (s)', 'Stock on'], rows)
p('Base values (every car): mass %.2f, inertia %.0f, yaw inertia %.0f; spread across cars: %s.' % (
    m0, i0, y0, 'none' if all(len(set(x)) == 1 for x in B49) else 'SEE TABLES'))
print('\n'.join(out))
