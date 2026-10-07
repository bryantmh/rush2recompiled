# Car physics: values, bars and parts (Rush 2 and Rush 2049)

October 2026. **[V]** verified in code, **[I]** inferred. Every table below is generated from the two ROMs by
`tools/rush2049/car_tables.py` (rerun it after changing anything). Field maps: cars.md and `tools/rush2049/cars.py`.

Simulated times use the shared drivetrain (§1): full throttle from rest, flat ground, no wheelspin, no clutch slip,
1/60 s steps, up-shift at 6325 rpm. They reproduce cars.md §10 for top speed (168 / 158 mph) and the port's 0-60
(2.5 s). Native 2049 0-60 comes out 0.2 s quicker than cars.md's 1.9 s because the launch clutch slip is left out.
Off-road tires' extra road drag (+0x5A8, func_8006AFD8) is not simulated, so SUV's top speed is optimistic.

## Settings that affect car physics

| Setting (Games tab) | Physics effect |
|---|---|
| Car Speeds = Rush 2 (default) | 2049 cars: torque x 0.827 / 0.837 / 0.857 (gears 1 / 2 / 3+), gear ratios x 0.909 (R) / 0.908 / 0.896 / 0.884 / 0.869 (`build_physics`). Rush 2 cars unchanged. AI lanes on 2049 tracks / 1.2. |
| Car Speeds = Rush 2049 | 2049 cars: 2049's torque scale and gears. Rush 2 and SF Rush cars: the inverse factors (torque / 0.827.., gears / 0.908..), applied per race car at init (`rush2_car49_setup_desc`). Other AI lanes x 1.2; speedometer x 1.2. |
| Rush 2049 Cars | None: only adds the 13 cars to the car select. |
| Rush 2049 Computer Cars | None: only which cars the AI drives. |

In both modes the 2049 cars keep Rush 2's STANDARD torque map, Rush 2's steering and the other §3 differences, so
Car Speeds = Rush 2049 brings them to 2049's top speed (158 mph) but leaves 0-60 about 0.2 s slower than native. The
bars ignore Car Speeds: they read the type's descriptor, and the mode is applied to a per-car copy at race start.

## 1. Formulas

### Drivetrain [V] (both games, cars.md §10)

    engine torque = map(rpm, throttle) x k_gear          k_gear: desc +0xB8 gear 1, +0xBC gear 2, +0xC0 gear 3+
    wheel force   = engine torque x gear ratio x 3.3      rear wheel radius 1.0 ft
    drag          = 0.0135 v^2 + 105                      v in ft/s, same for every car in both games
    accel         = (force - drag) / mass                 mass in slugs (lb / 32.2)
    rpm           = 9.549 x v x gear ratio x 3.3          map is 0 at 9200 rpm

### Yaw [V; units of steering I]

Steering input is Rush 2 car +0x728, 2049 +0x720 (ghost_logic.h). Tires make the cornering force in both games; these
are extra yaw torques added on top.

| Term | Rush 2 (func_80069E74) | Rush 2049 (func_800E1AA0) |
|---|---|---|
| A | speed x shape(+0x3C, +0x3B4) x steer (+0x5A0), halved when yaw gain is nonzero; +0x3C / +0x3B4 untraced | none |
| B | -steer x speed x yaw gain (+0x5A4) | -steer x min(speed, 100 ft/s) x 120 x f; f = 0.2 (0x801111E0, every cell) + per-car adjust + 0.5 x +0x3D4 (untraced), capped at 1 |

At 150 ft/s: Rush 2 term B with the port's yaw gain 31 = 4650 x steer; 2049's = 12000 x f = 1800 (f 0.15) to 3000
(f 0.25) x steer.

### Bars [V] (func_803B7F7C, constants 0x803CADB4)

    ACCEL   = 0.9 x (1 - (m - m_min) / (m_max - m_min)) + 0.1 x (k1 - 1.8) / 0.07
    TOP     = 0.27 x (1 - p_offroad) + 0.73 x 2.25 x (k3 / g4 - 1.8)
    CONTROL = steer / 1200                     steer = base (0x800C06B4) + TIRES S add, min 0   (func_8008D984)
    DRIFT   = yaw gain / 60 + 1 - (rear grip - 0.8) / 0.2                                        (func_8008D9EC)

m = func_8008DB04 mass; m_min / m_max = lightest car at DURABILITY 0 / heaviest at 100 over Rush 2's 22 types (85.40 /
142.86 slugs). k1, k3 = desc +0xB8, +0xC0; g4 = +0xDC; p_offroad = 0.5 HALF OFFRD, 0.75 FULL OFFRD (func_8008DAA0).
ROCKET: ACCEL x 0.3 + 0.85, TOP = 1. Values above 1 overflow the bar.

## 2. Rush 2 cars

### Rush 2 cars: mass, inertia and layout

| Type | Car | Desc | Mass (slugs) | Weight (lb) | Pitch/roll I | Yaw I | Preload | Default weight | Wheelbase (ft) | Track F / R (ft) | Box F / R / half W |
|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | PICKUP | 0 | 102.5 | 3300 | 2200 | 390000 | 0.16 | 0.5 | 7.85 | 5.50 / 5.50 | 6.75 / 6.75 / 3 |
| 1 | INTEG | 1 | 109.5 | 3525 | 2600 | 390000 | 0.18 | 0.5 | 7.85 | 5.50 / 5.50 | 6 / 6.5 / 3 |
| 2 | VETTE | 2 | 109.5 | 3525 | 2800 | 390000 | 0.18 | 0.4 | 7.85 | 5.50 / 5.50 | 6 / 6 / 3 |
| 3 | SLED | 3 | 118.0 | 3800 | 3500 | 450000 | 0.16 | 1.0 | 7.85 | 5.50 / 5.50 | 6.25 / 7.25 / 3 |
| 4 | BMW | 0 | 109.5 | 3525 | 2700 | 390000 | 0.18 | 0.5 | 7.85 | 5.50 / 5.50 | 6.25 / 6.5 / 3 |
| 5 | CAMARO | 1 | 109.5 | 3525 | 3000 | 415000 | 0.16 | 0.7 | 7.85 | 5.50 / 5.50 | 7 / 7.25 / 3 |
| 6 | SUPRA | 2 | 109.5 | 3525 | 2800 | 390000 | 0.20 | 0.5 | 7.85 | 5.50 / 5.50 | 6.5 / 6.5 / 3 |
| 7 | BUGAT | 3 | 105.6 | 3400 | 2200 | 325000 | 0.25 | 0.2 | 7.85 | 5.50 / 5.50 | 6.25 / 5.75 / 3 |
| 8 | VWBUS | 0 | 99.4 | 3200 | 2700 | 360000 | 0.15 | 0.3 | 7.85 | 5.50 / 5.50 | 6.25 / 6 / 3 |
| 9 | VIPER | 1 | 109.5 | 3525 | 3000 | 390000 | 0.18 | 0.5 | 7.85 | 5.50 / 5.50 | 6.25 / 7 / 3 |
| 10 | VWBUG | 2 | 96.3 | 3100 | 2100 | 350000 | 0.15 | 0.3 | 7.85 | 5.50 / 5.50 | 5.75 / 6.25 / 3 |
| 11 | CONCPT | 3 | 109.5 | 3525 | 2400 | 325000 | 0.25 | 0.2 | 7.85 | 5.50 / 5.50 | 6.25 / 5.25 / 3 |
| 12 | CIVIC | 0 | 99.4 | 3200 | 2200 | 360000 | 0.20 | 0.4 | 7.85 | 5.50 / 5.50 | 6 / 6 / 3 |
| 13 | CADDY | 4 | 124.2 | 4000 | 4000 | 470000 | 0.15 | 1.0 | 7.85 | 6.00 / 6.50 | 6.5 / 8 / 3.25 |
| 14 | MUST | 2 | 109.5 | 3525 | 2600 | 390000 | 0.19 | 0.5 | 7.85 | 5.50 / 5.50 | 6.75 / 6.5 / 3 |
| 15 | SUV | 3 | 124.2 | 4000 | 3800 | 440000 | 0.16 | 0.9 | 7.85 | 5.50 / 5.50 | 6.75 / 7.5 / 3 |
| 16 | TAXI | 5 | 116.5 | 3750 | 3500 | 460000 | 0.16 | 1.0 | 7.85 | 5.50 / 5.50 | 6 / 6.5 / 3 |
| 17 | HOTROD | 6 | 109.5 | 3525 | 2800 | 415000 | 0.16 | 0.7 | 7.85 | 5.50 / 5.50 | 5.5 / 5.75 / 3 |
| 18 | FORM1 | 7 | 102.5 | 3300 | 2000 | 325000 | 0.25 | 0.2 | 7.85 | 5.50 / 5.50 | 6.25 / 5.75 / 3 |
| 19 | GT90 | 8 | 99.4 | 3200 | 2200 | 325000 | 0.25 | 0.2 | 7.85 | 5.50 / 5.50 | 6.25 / 5.25 / 3 |
| 20 | ROCKET | 9 | 124.2 | 4000 | 3000 | 315000 | 0.13 | 0.4 | 7.85 | 4.50 / 4.50 | 9.5 / 7.75 / 3 |
| 21 | DEW | 10 | 102.5 | 3300 | 2200 | 390000 | 0.16 | 0.5 | 8.06 | 3.70 / 5.50 | 6.75 / 6.75 / 3 |

### Rush 2 cars: steering, drift and drivetrain inputs (default parts)

| Car | Steer base | Def. TIRES | Steer (+0x5A0) | Yaw base | Def. SUSP | Yaw gain (+0x5A4) | Rear grip (+0x78) | Torque k1 / k2 / k3+ | Gears 1 / 2 / 3 / 4 | Def. TORQUE |
|---|---|---|---|---|---|---|---|---|---|---|
| PICKUP | 1000 | 9 | 1000 | 35 | 0 | 55 | 1.000 | 1.800 / 1.800 / 1.800 | 3.10 / 1.78 / 1.29 / 0.99 | STANDARD |
| INTEG | 600 | 7 | 600 | 30 | 1 | 50 | 0.975 | 1.803 / 1.806 / 1.825 | 3.10 / 1.78 / 1.28 / 0.98 | STANDARD |
| VETTE | 300 | 3 | 100 | 45 | 2 | 45 | 0.950 | 1.805 / 1.812 / 1.850 | 3.10 / 1.77 / 1.27 / 0.97 | STANDARD |
| SLED | 200 | 6 | 0 | 25 | 1 | 45 | 0.900 | 1.810 / 1.850 / 1.900 | 3.10 / 1.75 / 1.25 / 0.95 | STANDARD |
| BMW | 1000 | 5 | 1200 | 40 | 0 | 40 | 1.000 | 1.800 / 1.800 / 1.800 | 3.10 / 1.78 / 1.29 / 0.99 | STANDARD |
| CAMARO | 600 | 4 | 600 | 35 | 1 | 35 | 0.975 | 1.803 / 1.806 / 1.825 | 3.10 / 1.78 / 1.28 / 0.98 | STANDARD |
| SUPRA | 300 | 0 | 100 | 40 | 1 | 20 | 0.950 | 1.805 / 1.812 / 1.850 | 3.10 / 1.77 / 1.27 / 0.97 | STANDARD |
| BUGAT | 200 | 0 | 0 | 20 | 2 | 0 | 0.900 | 1.810 / 1.850 / 1.900 | 3.10 / 1.75 / 1.25 / 0.95 | STANDARD |
| VWBUS | 1000 | 8 | 1200 | 30 | 0 | 50 | 1.000 | 1.800 / 1.800 / 1.800 | 3.10 / 1.78 / 1.29 / 0.99 | STANDARD |
| VIPER | 600 | 4 | 600 | 25 | 1 | 25 | 0.975 | 1.803 / 1.806 / 1.825 | 3.10 / 1.78 / 1.28 / 0.98 | STANDARD |
| VWBUG | 300 | 3 | 100 | 45 | 0 | 45 | 0.950 | 1.805 / 1.812 / 1.850 | 3.10 / 1.77 / 1.27 / 0.97 | STANDARD |
| CONCPT | 200 | 0 | 0 | 20 | 2 | 0 | 0.900 | 1.810 / 1.850 / 1.900 | 3.10 / 1.75 / 1.25 / 0.95 | STANDARD |
| CIVIC | 800 | 8 | 1000 | 35 | 0 | 55 | 1.000 | 1.800 / 1.800 / 1.800 | 3.10 / 1.78 / 1.29 / 0.99 | STANDARD |
| CADDY | 600 | 7 | 600 | 30 | 0 | 50 | 0.975 | 1.805 / 1.812 / 1.850 | 3.10 / 1.77 / 1.27 / 0.97 | STANDARD |
| MUST | 300 | 3 | 100 | 25 | 1 | 25 | 0.950 | 1.805 / 1.812 / 1.850 | 3.10 / 1.77 / 1.27 / 0.97 | STANDARD |
| SUV | 200 | 10 | 100 | 20 | 0 | 50 | 0.900 | 1.810 / 1.850 / 1.900 | 3.10 / 1.75 / 1.25 / 0.95 | STANDARD |
| TAXI | 800 | 7 | 800 | 30 | 0 | 50 | 0.975 | 1.830 / 1.875 / 1.950 | 3.10 / 1.75 / 1.25 / 0.95 | STANDARD |
| HOTROD | 1000 | 5 | 1200 | 45 | 3 | 45 | 0.950 | 1.850 / 1.900 / 2.000 | 3.10 / 1.72 / 1.20 / 0.92 | STANDARD |
| FORM1 | 200 | 4 | 200 | 10 | 2 | 10 | 0.975 | 1.870 / 1.920 / 2.020 | 3.10 / 1.70 / 1.19 / 0.90 | STANDARD |
| GT90 | 200 | 3 | 0 | 25 | 2 | 25 | 0.975 | 1.900 / 1.950 / 2.050 | 3.05 / 1.68 / 1.17 / 0.88 | STANDARD |
| ROCKET | 0 | 0 | 0 | 0 | 2 | 0 | 0.850 | 1.810 / 1.850 / 1.900 | 3.10 / 1.75 / 1.25 / 0.95 | ROCKET |
| DEW | 1000 | 9 | 1000 | 35 | 0 | 55 | 0.975 | 1.900 / 1.950 / 2.050 | 3.05 / 1.68 / 1.17 / 0.88 | STANDARD |

Steer and yaw gain are with the car's default TIRES and SUSPENSION. Steer 0 (SLED, BUGAT, CONCPT, GT90, ROCKET) means no
term A; BUGAT, CONCPT and ROCKET also have yaw gain 0, so they turn on tire force alone.

### Rush 2 cars: bars vs simulated performance (default parts)

Bars as func_803B7F7C computes them (1.0 = full bar; m_min 85.40, m_max 142.86 slugs). Simulated times: full throttle from rest, flat ground, no wheelspin.

| Car | ACCEL | TOP | CONTROL | DRIFT | Speeds R2: 0-60 (s) | 0-100 (s) | Top (mph) | Speeds 2049: 0-60 (s) | 0-100 (s) | Top (mph) |
|---|---|---|---|---|---|---|---|---|---|---|
| PICKUP | 0.63 | 0.16 | 0.83 | 0.92 | 2.52 | 5.40 | 163 | 1.92 | 4.33 | 153 |
| INTEG | 0.53 | 0.37 | 0.50 | 0.96 | 2.68 | 5.73 | 165 | 2.03 | 4.58 | 154 |
| VETTE | 0.53 | 0.45 | 0.08 | 1.00 | 2.67 | 5.70 | 166 | 2.03 | 4.55 | 156 |
| SLED | 0.40 | 0.60 | 0.00 | 1.25 | 2.87 | 6.05 | 169 | 2.18 | 4.82 | 158 |
| BMW | 0.52 | 0.30 | 1.00 | 0.67 | 2.68 | 5.77 | 163 | 2.05 | 4.62 | 153 |
| CAMARO | 0.53 | 0.37 | 0.50 | 0.71 | 2.68 | 5.73 | 165 | 2.03 | 4.58 | 154 |
| SUPRA | 0.53 | 0.45 | 0.08 | 0.58 | 2.67 | 5.70 | 166 | 2.03 | 4.55 | 156 |
| BUGAT | 0.60 | 0.60 | 0.00 | 0.50 | 2.57 | 5.42 | 169 | 1.95 | 4.30 | 158 |
| VWBUS | 0.68 | 0.30 | 1.00 | 0.83 | 2.43 | 5.23 | 163 | 1.87 | 4.20 | 153 |
| VIPER | 0.53 | 0.37 | 0.50 | 0.54 | 2.68 | 5.73 | 165 | 2.03 | 4.58 | 154 |
| VWBUG | 0.74 | 0.45 | 0.08 | 1.00 | 2.35 | 5.02 | 166 | 1.80 | 4.00 | 156 |
| CONCPT | 0.54 | 0.60 | 0.00 | 0.50 | 2.67 | 5.62 | 169 | 2.02 | 4.47 | 158 |
| CIVIC | 0.68 | 0.30 | 0.83 | 0.92 | 2.43 | 5.23 | 163 | 1.87 | 4.20 | 153 |
| CADDY | 0.30 | 0.45 | 0.50 | 0.96 | 3.03 | 6.47 | 166 | 2.32 | 5.15 | 156 |
| MUST | 0.53 | 0.45 | 0.08 | 0.67 | 2.67 | 5.70 | 166 | 2.03 | 4.55 | 156 |
| SUV | 0.31 | 0.40 | 0.08 | 1.33 | 3.02 | 6.37 | 169 | 2.30 | 5.07 | 158 |
| TAXI | 0.46 | 0.68 | 0.67 | 0.96 | 2.80 | 5.87 | 169 | 2.13 | 4.65 | 159 |
| HOTROD | 0.59 | 0.88 | 1.00 | 1.00 | 2.62 | 5.47 | 173 | 1.98 | 4.30 | 163 |
| FORM1 | 0.73 | 1.00 | 0.17 | 0.29 | 2.43 | 5.08 | 175 | 1.85 | 3.98 | 165 |
| GT90 | 0.82 | 1.14 | 0.00 | 0.54 | 2.35 | 4.88 | 178 | 1.78 | 3.83 | 168 |
| ROCKET | 0.94 | 1.00 | 0.00 | 0.75 | 3.02 | 6.37 | 169 | 2.30 | 5.07 | 158 |
| DEW | 0.78 | 1.00 | 0.83 | 1.04 | 2.42 | 5.03 | 178 | 1.83 | 3.95 | 168 |

## 3. Rush 2049 cars

All 13 cars share base mass 102.48 slugs, inertia 2200, yaw inertia 390000, ENGINE 1, HANDLING 0 and TIRES 1 in their
stock setup. They differ only by default FRAME, yaw adjust (0x8011156C), wheel layout and collision box.

### Rush 2049 cars: per-car values (stock setup)

| Car | FRAME | Mass (slugs) | Weight (lb) | Pitch/roll I | Yaw I | Preload | Yaw adjust | f (yaw factor) | Wheelbase (ft) | Track F / R (ft) | Box F / R / half W |
|---|---|---|---|---|---|---|---|---|---|---|---|
| FORMULA 1 | 3 | 102.5 | 3300 | 2200 | 390000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 6.75 / 5.25 / 3 |
| 8-BALL | 3 | 102.5 | 3300 | 2200 | 390000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 6.75 / 5.75 / 3 |
| ROCKET ZX | 2 | 96.3 | 3100 | 2000 | 370000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 6.25 / 5.75 / 3 |
| MAGNUM | 4 | 108.7 | 3500 | 2400 | 410000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 6.25 / 6 / 3 |
| SUPER GT | 3 | 102.5 | 3300 | 2200 | 390000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 6 / 6.75 / 3 |
| BRUISER | 4 | 108.7 | 3500 | 2400 | 410000 | 0.30 | +0.00 | 0.20 | 7.85 | 5.50 / 5.50 | 6.75 / 6.75 / 3 |
| LOCUST LX | 2 | 96.3 | 3100 | 2000 | 370000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 5.75 / 6 / 3 |
| GX-2 | 3 | 102.5 | 3300 | 2200 | 390000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 5.75 / 5.75 / 3 |
| MINI XS | 2 | 96.3 | 3100 | 2000 | 370000 | 0.30 | -0.05 | 0.15 | 6.92 | 5.00 / 5.00 | 4.5 / 5.75 / 3 |
| VENOM | 3 | 102.5 | 3300 | 2200 | 390000 | 0.30 | +0.00 | 0.20 | 9.18 | 6.50 / 6.50 | 7 / 7 / 3.25 |
| CRUSHER | 5 | 114.9 | 3700 | 2600 | 430000 | 0.30 | +0.05 | 0.25 | 11.10 | 6.00 / 7.50 | 7.25 / 9.75 / 3.5 |
| EURO LX | 3 | 102.5 | 3300 | 2200 | 390000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 5.5 / 6.75 / 3 |
| PANTHER | 2 | 96.3 | 3100 | 2000 | 370000 | 0.30 | -0.05 | 0.15 | 7.85 | 5.50 / 5.50 | 6 / 5.75 / 3 |

### Rush 2049 cars: drivetrain native vs port, bars in the port, simulated performance

Native = 2049 stock (ENGINE 1, HANDLING 0): torque 2.20 in every gear, gears 3.410 / 1.958 / 1.419 / 1.100. Port with Car Speeds = Rush 2: torque 1.820 / 1.842 / 1.885, gears 3.095 / 1.754 / 1.254 / 0.956 (x Rush 2 mean / 2049 mean). Every 2049 car in the port has steer 743 and yaw gain 31 (Rush 2 means, TIRES 5), rear grip 1.0.

| Car | ACCEL | TOP | CONTROL | DRIFT | Port, Speeds R2: 0-60 / 0-100 (s) | top (mph) | Port, Speeds 2049: 0-60 / 0-100 (s) | top (mph) | Native 2049: 0-60 / 0-100 (s) | top (mph) |
|---|---|---|---|---|---|---|---|---|---|---|
| FORMULA 1 | 0.66 | 0.55 | 0.62 | 0.51 | 2.48 / 5.27 | 168 | 1.90 / 4.20 | 158 | 1.68 / 3.98 | 158 |
| 8-BALL | 0.66 | 0.55 | 0.62 | 0.51 | 2.48 / 5.27 | 168 | 1.90 / 4.20 | 158 | 1.68 / 3.98 | 158 |
| ROCKET ZX | 0.76 | 0.55 | 0.62 | 0.51 | 2.33 / 4.95 | 168 | 1.78 / 3.93 | 158 | 1.57 / 3.73 | 158 |
| MAGNUM | 0.56 | 0.55 | 0.62 | 0.51 | 2.63 / 5.58 | 168 | 2.02 / 4.45 | 158 | 1.78 / 4.22 | 158 |
| SUPER GT | 0.66 | 0.55 | 0.62 | 0.51 | 2.48 / 5.27 | 168 | 1.90 / 4.20 | 158 | 1.68 / 3.98 | 158 |
| BRUISER | 0.56 | 0.55 | 0.62 | 0.51 | 2.63 / 5.58 | 168 | 2.02 / 4.45 | 158 | 1.78 / 4.22 | 158 |
| LOCUST LX | 0.76 | 0.55 | 0.62 | 0.51 | 2.33 / 4.95 | 168 | 1.78 / 3.93 | 158 | 1.57 / 3.73 | 158 |
| GX-2 | 0.66 | 0.55 | 0.62 | 0.51 | 2.48 / 5.27 | 168 | 1.90 / 4.20 | 158 | 1.68 / 3.98 | 158 |
| MINI XS | 0.76 | 0.55 | 0.62 | 0.51 | 2.33 / 4.95 | 168 | 1.78 / 3.93 | 158 | 1.57 / 3.73 | 158 |
| VENOM | 0.66 | 0.55 | 0.62 | 0.51 | 2.48 / 5.27 | 168 | 1.90 / 4.20 | 158 | 1.68 / 3.98 | 158 |
| CRUSHER | 0.47 | 0.55 | 0.62 | 0.51 | 2.78 / 5.90 | 168 | 2.12 / 4.70 | 158 | 1.88 / 4.45 | 158 |
| EURO LX | 0.66 | 0.55 | 0.62 | 0.51 | 2.48 / 5.27 | 168 | 1.90 / 4.20 | 158 | 1.68 / 3.98 | 158 |
| PANTHER | 0.76 | 0.55 | 0.62 | 0.51 | 2.33 / 4.95 | 168 | 1.78 / 3.93 | 158 | 1.57 / 3.73 | 158 |

### 2049 car values: native vs port

| Value | Rush 2049 | Port (every 2049 car) | Effect |
|---|---|---|---|
| Torque map | 2049 map (230 at 0 rpm, 270 at 2300) | Rush 2 STANDARD (75 at 0, 250 at 2300) in both Car Speeds modes: TORQUE row copied from the Pickup | 0-60 about 0.2 s slower than native even with Car Speeds = Rush 2049 |
| Torque scale | 2.2 all gears (ENGINE x HANDLING) | 1.820 / 1.842 / 1.885 (Car Speeds Rush 2); 2.2 (Car Speeds Rush 2049) | By design |
| Gears 1-4 | 3.410 / 1.958 / 1.419 / 1.100 | 3.095 / 1.754 / 1.254 / 0.956 (Rush 2); 2049's (Rush 2049) | By design |
| Steering term A | none | steer 743 (mean 543 + S2) | Extra turn-in the 2049 cars never had |
| Steering term B | 120 x f x min(v, 100), f 0.15 / 0.20 / 0.25 | 31 x v, uncapped | Same for every car; grows past 68 mph |
| Per-car yaw adjust | -0.05 / 0 / +0.05 | dropped | Crusher, Bruiser, Venom lose their extra rotation |
| Rear lateral grip | TIRES: 1.0 / 0.9 / 0.8 | 1.0 always | 2049 drift tires unavailable |
| Rear longitudinal grip | ENGINE: x1.00 .. x1.20 | x1.0 always | Upgraded engines lose their traction bonus |
| Off-road factor | TIRES: 0, -0.25, -0.5, 0.5, 0.75 | Rush 2 TIRES: 0, 0.5, 0.75 | Drift tires' -0.25 / -0.5 missing |
| HANDLING | 0-2 | fixed 0 | §5 |
| Mass, inertias, FRAME | as 2049 | as 2049 (DURABILITY = FRAME weight) | matches |
| Wheel layout, tire curves, springs, clutch 1000, drag, final drive, shift points | as 2049 | same | matches |

## 4. Rush 2 parts

SUSPENSION and DURABILITY are deltas from the car's default; TIRES is absolute (its add is already in a stock car's
numbers). TRANSMISSION, ENGINE (sound), colours, rims, horn and the hidden TIRE SIZE rows don't touch physics.

### Rush 2 TIRES: every setting

| Value | Label | Steer add (0x800C0DF0) | Yaw gain add (0x800C0E1C) | Off-road factor (+0x5A8) | TOP bar penalty |
|---|---|---|---|---|---|
| 0 | S0 D0 | -200 | -20 | 0 | 0 |
| 1 | S1 D0 | +0 | -20 | 0 | 0 |
| 2 | S2 D0 | +200 | -20 | 0 | 0 |
| 3 | S0 D1 | -200 | +0 | 0 | 0 |
| 4 | S1 D1 | +0 | +0 | 0 | 0 |
| 5 | S2 D1 | +200 | +0 | 0 | 0 |
| 6 | S0 D2 | -200 | +20 | 0 | 0 |
| 7 | S1 D2 | +0 | +20 | 0 | 0 |
| 8 | S2 D2 | +200 | +20 | 0 | 0 |
| 9 | HALF OFFRD | +0 | +20 | 0.5 | 0.5 |
| 10 | FULL OFFRD | -100 | +30 | 0.75 | 0.75 |

S = steering (term A), D = drift (term B). The off-road factor (+0x5A8) adds off-road grip and road drag in
func_8006AFD8.

### Rush 2 SUSPENSION

| Value | Yaw gain add (vs car default) | Cars with this default |
|---|---|---|
| LOOSE | (value - default) x 10 | PICKUP, BMW, VWBUS, VWBUG, CIVIC, CADDY, SUV, TAXI, DEW |
| NORMAL | (value - default) x 10 | INTEG, SLED, CAMARO, SUPRA, VIPER, MUST |
| TIGHT | (value - default) x 10 | VETTE, BUGAT, CONCPT, FORM1, GT90, ROCKET |
| WHEELIE | (value - default) x 10 | HOTROD |

The suspension curve (func_8009A264) also changes with SUSPENSION; WHEELIE is HOTROD's default.

### Rush 2 TORQUE: full-throttle torque map (map units, x torque scale = engine torque)

| rpm | 0 | 1150 | 2300 | 3450 | 4600 | 5750 | 6900 | 8050 | 9200 | 10350 | 11500 | 12650 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| LOW | 125 | 250 | 350 | 280 | 260 | 240 | 200 | 140 | 50 | -50 | -100 | -150 |
| STANDARD | 75 | 200 | 250 | 290 | 300 | 270 | 200 | 100 | 0 | -40 | -60 | -80 |
| HIGH | 50 | 150 | 200 | 240 | 270 | 300 | 200 | 50 | 0 | -20 | -40 | -60 |
| 2049 (all cars) | 230 | 250 | 270 | 290 | 300 | 270 | 200 | 100 | 0 | -40 | -60 | -80 |

| Map on the Rush 2 average car | 0-60 (s) | 0-100 (s) | Top (mph) |
|---|---|---|---|
| LOW | 2.50 | 5.90 | 172 |
| STANDARD | 2.63 | 5.57 | 168 |
| HIGH | 3.18 | 6.07 | 165 |

### Rush 2 DURABILITY: mass at each setting

| Car | Default | Mass at 0 / default / 100 (slugs) | 0-60 at 0 / 100 (s) |
|---|---|---|---|
| PICKUP | 50 | 87.0 / 102.5 / 118.0 | 2.13 / 2.88 |
| INTEG | 50 | 93.9 / 109.5 / 125.0 | 2.30 / 3.05 |
| VETTE | 40 | 97.0 / 109.5 / 128.1 | 2.37 / 3.13 |
| SLED | 100 | 87.0 / 118.0 / 118.0 | 2.12 / 2.87 |
| BMW | 50 | 93.9 / 109.5 / 125.0 | 2.30 / 3.05 |
| CAMARO | 70 | 87.7 / 109.5 / 118.8 | 2.15 / 2.90 |
| SUPRA | 50 | 93.9 / 109.5 / 125.0 | 2.30 / 3.05 |
| BUGAT | 20 | 99.4 / 105.6 / 130.4 | 2.42 / 3.17 |
| VWBUS | 30 | 90.1 / 99.4 / 121.1 | 2.22 / 2.97 |
| VIPER | 50 | 93.9 / 109.5 / 125.0 | 2.30 / 3.05 |
| VWBUG | 30 | 87.0 / 96.3 / 118.0 | 2.13 / 2.88 |
| CONCPT | 20 | 103.3 / 109.5 / 134.3 | 2.52 / 3.27 |
| CIVIC | 40 | 87.0 / 99.4 / 118.0 | 2.13 / 2.88 |
| CADDY | 100 | 93.2 / 124.2 / 124.2 | 2.28 / 3.03 |
| MUST | 50 | 93.9 / 109.5 / 125.0 | 2.30 / 3.05 |
| SUV | 90 | 96.3 / 124.2 / 127.3 | 2.33 / 3.10 |
| TAXI | 100 | 85.4 / 116.5 / 116.5 | 2.05 / 2.80 |
| HOTROD | 70 | 87.7 / 109.5 / 118.8 | 2.10 / 2.83 |
| FORM1 | 20 | 96.3 / 102.5 / 127.3 | 2.28 / 3.02 |
| GT90 | 20 | 93.2 / 99.4 / 124.2 | 2.20 / 2.93 |
| DEW | 50 | 87.0 / 102.5 / 118.0 | 2.05 / 2.78 |

Mass = base + (value / 100 - default / 100) x 31.06 slugs; inertias follow (0x800C0964 rows).

## 5. Rush 2049 parts

### Rush 2049 ENGINE x HANDLING: torque scale and rear longitudinal grip

| ENGINE | Torque H0 / H1 / H2 | Rear long. grip H0 / H1 / H2 | Native 0-60 / top (mph), H0, avg car |
|---|---|---|---|
| 1 | 2.20 / 2.30 / 2.40 | 1.00 / 1.00 / 1.00 | 1.68 / 158 |
| 2 | 2.20 / 2.30 / 2.40 | 1.00 / 1.00 / 1.00 | 1.68 / 158 |
| 3 | 2.20 / 2.30 / 2.40 | 1.00 / 1.00 / 1.00 | 1.68 / 158 |
| 4 | 2.23 / 2.33 / 2.43 | 1.03 / 1.03 / 1.03 | 1.65 / 158 |
| 5 | 2.26 / 2.36 / 2.46 | 1.06 / 1.06 / 1.06 | 1.63 / 158 |
| 6 | 2.30 / 2.40 / 2.50 | 1.10 / 1.10 / 1.10 | 1.60 / 158 |
| 7 | 2.33 / 2.43 / 2.53 | 1.13 / 1.13 / 1.13 | 1.58 / 159 |
| 8 | 2.36 / 2.46 / 2.56 | 1.16 / 1.16 / 1.16 | 1.57 / 159 |
| 9 | 2.40 / 2.50 / 2.60 | 1.20 / 1.20 / 1.20 | 1.53 / 159 |

ENGINE 1-3 are physically identical (same row). The port applies the torque ratio to level 1 but not the rear grip.

### Rush 2049 HANDLING (transmission entries 0x801116D0)

| HANDLING | Gear x | Gears 1-4 | Torque (ENGINE 1) | Torque x gear factor | trans+0x24 (untraced) | All 11 floats | 0-60 (s) | Top (mph) |
|---|---|---|---|---|---|---|---|---|
| 0 | 1.100 | 3.410 / 1.958 / 1.419 / 1.100 | 2.20 | 2.42 | 1.0 | 0 1 1 1.1 1 1 1 0 1.8 1 1 | 1.68 | 158 |
| 1 | 1.075 | 3.333 / 1.914 / 1.387 / 1.075 | 2.30 | 2.47 | 0.5 | 0 1 1 1.075 1 1 1 0 1.8 0.5 1 | 1.63 | 161 |
| 2 | 1.050 | 3.255 / 1.869 / 1.354 / 1.050 | 2.40 | 2.52 | 0.0 | 0 1 1 1.05 1 1 1 0 1.8 0 1 | 1.58 | 164 |

trans+0x24 (1.0 / 0.5 / 0.0) is read somewhere not yet traced. trans+0x4 multiplies mass, +0x8 torque, +0x18 rear
longitudinal grip, +0x1C rear lateral grip: all 1 or 0 in every entry.

### Rush 2049 TIRES

| TIRES | Rear lateral grip (0x8011121C) | Off-road factor (+0x5B4, = Rush 2 +0x5A8) | Rush 2 equivalent |
|---|---|---|---|
| 1 | 1.00 | 0.0 | street |
| 2 | 0.90 | -0.25 | drift (no Rush 2 equivalent) |
| 3 | 0.80 | -0.5 | more drift (none) |
| 4 | 1.00 | 0.5 | HALF OFFRD |
| 5 | 1.00 | 0.75 | FULL OFFRD |

### Rush 2049 FRAME (0x80111274): identical for every car

| FRAME | Weight w | Mass (slugs) | Weight (lb) | Pitch/roll I | Yaw I | Native 0-60 (s) | Stock on |
|---|---|---|---|---|---|---|---|
| 1 | 0.0 | 90.1 | 2900 | 1800 | 350000 | 1.48 |  |
| 2 | 0.2 | 96.3 | 3100 | 2000 | 370000 | 1.57 | ROCKET ZX, LOCUST LX, MINI XS, PANTHER |
| 3 | 0.4 | 102.5 | 3300 | 2200 | 390000 | 1.68 | FORMULA 1, 8-BALL, SUPER GT, GX-2, VENOM, EURO LX |
| 4 | 0.6 | 108.7 | 3500 | 2400 | 410000 | 1.78 | MAGNUM, BRUISER |
| 5 | 0.8 | 114.9 | 3700 | 2600 | 430000 | 1.88 | CRUSHER |
| 6 | 1.0 | 121.1 | 3900 | 2800 | 450000 | 1.98 |  |

Base values (every car): mass 102.48, inertia 2200, yaw inertia 390000; spread across cars: none.

### Rush 2049's own bars [V] (setup overlay, func at 0x8039D300)

Each bar is a product of one row per part: bar[i] = TRANS[t][i] x HANDLING[h][i] x ENGINE[e][i] x TIRES[d][i] x FRAME[f][i]
(tables at 0x803B28C8 / 2948 / 2978 / 2A08 / 2A58, 4 floats per row: TOP SPEED, ACCELERATION, CONTROL, STRENGTH). The
car itself is not an input: every car shows the same bars for the same parts.

| Part | Value | TOP | ACCEL | CONTROL | STRENGTH |
|---|---|---|---|---|---|
| TRANSMISSION | AUTOMATIC / MANUAL | 0.95 / 1.00 | 0.85 | 1 | 1 |
| | SPORT AUTO / MAN. | 0.95 / 1.00 | 0.90 | 1 | 1 |
| | PRO AUTO / MAN. | 0.95 / 1.00 | 0.95 | 1 | 1 |
| | BATTLE / BATTLE MAN. | 0.85 / 0.90 | 1.00 | 1 | 1 |
| HANDLING | NORMAL / ADVANCED / EXTREME | 0.90 / 0.95 / 1.00 | 0.90 / 0.95 / 1.00 | 1.00 / 0.90 / 0.80 | 1 |
| ENGINE | 3.2L HP V6, TURBO 350, 6.2L V8 | 0.70 | 0.70 | 1 | 1 |
| | 5.0L HP V6 / TURBO 400 / 7.0L V8 | 0.75 / 0.80 / 0.85 | same as TOP | 1 | 1 |
| | 6.5L HP V8 / TURBO 500 / 8.0L V10 | 0.90 / 0.95 / 1.00 | same as TOP | 1 | 1 |
| TIRES | RADIALS / SLICKS / PRO SLICKS | 0.90 / 0.95 / 1.00 | 0.90 / 0.95 / 1.00 | 0.90 / 0.95 / 1.00 | 1 |
| | ALL TERRAIN / OFF ROAD | 0.85 / 0.80 | 0.85 / 0.80 | 0.95 / 0.90 | 1 |
| FRAME | LIGHT / STANDARD / HEAVY | 0.90 / 0.85 / 0.80 | 0.90 / 0.85 / 0.80 | 0.90 / 0.95 / 1.00 | 0.33 / 0.69 / 1.00 |
| | LT. / STD. / HVY. ALLOY | 1.00 / 0.95 / 0.90 | 1.00 / 0.95 / 0.90 | 0.90 / 0.95 / 1.00 | 0.33 / 0.69 / 1.00 |

These are hand-set multipliers, and some disagree with 2049's physics:
- FRAME weight in the physics is 0 / 0.2 / 0.4 / 0.6 / 0.8 / 1.0 in menu order (func_800D0B14: mass = (base +
  (w - 0.4) x 31.06) x trans+4). The alloys are the three heaviest frames, yet their bars show them as the quickest.
- STRENGTH depends on FRAME alone. Frame weight is read only by the mass and inertia code (func_800D0B14,
  func_800D0BA0), so its only physical effect is mass, the same thing Rush 2's DURABILITY changes.
- AUTOMATIC and MANUAL share one gear set (sets 0 and 1 are identical), so MANUAL's +5% TOP is the player shifting.
- 2049's TRANSMISSION has 4 real gear sets (ratios 3.1 / 1.78 / x / y: AUTO/MAN 1.29 / 1.0, SPORT 1.3 / 1.02, PRO
  1.25 / 0.98, BATTLE 1.4 / 1.1, times HANDLING's gear factor). The port has none of them; Rush 2's TRANSMISSION is
  automatic or manual only.

### Tires: what each field does [V]

| Field | Rush 2 | Rush 2049 | Effect |
|---|---|---|---|
| Off-road factor f | car +0x5A8 (TIRES: 0, HALF 0.5, FULL 0.75) | +0x5B4 (TIRES: 0, -0.25, -0.5, 0.5, 0.75) | func_8006AFD8, both rear wheels on the surface: dirt (1) force -10 x (1 - f) x v, grass (2) -15 x (1 - f) x v, pavement -0.7 x f x v (v = forward ft/s) |
| Rear lateral grip | desc +0x78 (per car 0.85-1.0) | 0x8011121C [TIRES]: 1.0 / 0.9 / 0.8 / 1.0 / 1.0 | Rear side force (func_8006ABB0) |
| Steer force (term A) | TIRES S: -200 / 0 / +200; off-road 0 / -100 | none | Rush 2 yaw term A |
| Yaw gain (term B) | TIRES D: -20 / 0 / +20; off-road +20 / +30 | none (term B uses f, below) | Rush 2 yaw term B |

At 150 ft/s on pavement, f = -0.5 (PRO SLICKS) gives +52 lb of push against 409 lb of drag, about 13% less drag.
f = 0.75 (FULL OFFRD / OFF ROAD) adds 79 lb.

### Steering terms, completed [V]

- Rush 2 term A uses x = 1 - (forward speed +0x3C) / (speed +0x3D4), a slide-angle measure that is 0 when the car
  runs straight. Its shape is (x - x^4) x (+0x3B4, from the drivetrain, func_80071A1C) + x^4. Its direction comes
  from the sign of +0x34 x +0x44 (untraced).
- 2049's term B factor: f = 0.2 + per-car adjust + 0.5 x throttle (+0x3D4, copied from the throttle input +0x728;
  0.7 on abort, 0.33 in one mode flag 0x8013FECB; 1.0 when above 0.99 or with no driver), capped at 1. That is 0.15-0.25
  off throttle and 0.65 / 0.70 / 0.75 at full throttle (most cars / Bruiser, Venom / Crusher).

| Speed | 2049 term B at full throttle (x steer) | Rush 2 term B, port's 2049-car yaw gain 31 | Rush 2 PICKUP (55) | Rush 2 BUGAT (0) |
|---|---|---|---|---|
| 50 ft/s (34 mph) | 3900 / 4200 / 4500 | 1550 | 2750 | 0 |
| 100 ft/s (68 mph) | 7800 / 8400 / 9000 | 3100 | 5500 | 0 |
| 150 ft/s (102 mph) | 7800 / 8400 / 9000 (capped) | 4650 | 8250 | 0 |
| 220 ft/s (150 mph) | 7800 / 8400 / 9000 | 6820 | 12100 | 0 |


## 6. Where the bars mislead (from the tables above)

| Bar | Example | Bar | Simulated |
|---|---|---|---|
| TOP SPEED | PICKUP vs GT90 | 0.16 vs 1.14 | 163 vs 178 mph: a 7x bar gap for 9% speed |
| TOP SPEED | TAXI, SLED, CONCPT, SUV | 0.68, 0.60, 0.60, 0.40 | all 169 mph (SUV less by its off-road drag) |
| TOP SPEED | 2049 cars vs Rush 2 cars at 168-169 mph | 0.55 vs 0.40-0.68 | same speed |
| ACCELERATION | ROCKET | 0.94 (fixed formula) | 3.02 s 0-60, among the slowest |
| ACCELERATION | VWBUG vs GT90 | 0.74 vs 0.82 | both 2.35 s |
| CONTROL | SLED, BUGAT, CONCPT, GT90 | 0 (empty) | They steer through tires and yaw gain, which CONTROL ignores |
| CONTROL / DRIFT | all 13 2049 cars | 0.62 / 0.51 | identical, though Crusher's wheelbase is 11.1 ft and Mini XS's 6.9 ft |
| DRIFT | SUV, SLED | 1.33, 1.25 | overflow the bar |

## 7. Options

Recommended order: 2049 physics for 2049 cars, then bars computed from the physics, then part rows.

1. **2049 physics for 2049 cars.** Port func_800E1AA0's yaw code for them (no term A, term B capped at 100 ft/s, per-car
   adjust); give them 2049's torque map; apply 2049 TIRES' rear lateral grip and off-road factor and ENGINE's rear
   longitudinal grip. First trace the +0x3D4 term in f and trans+0x24.
2. **Bars from the physics.** ACCELERATION = simulated 0-100 time, TOP SPEED = simulated top speed (both from this
   script's model, including off-road drag), CONTROL = yaw response to full steering at a reference speed ((term A +
   term B) / yaw inertia), DRIFTING = rear grip vs front with yaw response. Optional fifth bar WEIGHT.
3. **Part rows on 2049 cars.** ENGINE = 2049 ENGINE + its rear grip; SUSPENSION = 2049 HANDLING; TIRES = 2049 TIRES
   (5 values, which line up with Rush 2's: street, two drift sets, HALF OFFRD, FULL OFFRD); DURABILITY = FRAME (as
   now); TORQUE stays. Reusing rows keeps the car select layout and the save format.

Open question: on 2049 cars, should 2049's five TIRES replace Rush 2's eleven sets, or sit alongside them?

## 9. Accurate Car Stats and Torque Rebalance (implemented, October 2026)

Two options in the General tab under "Cars", both on by default (`src/config.cpp`, `src/car2049.cpp`).

**Corrections to the sections above.** Their simulated times, top speeds and "bars from physics" came from hand
formulas that the game does not follow:

- The "Yaw I" column (table 0x800C090C, car +0x644) is the crash threshold: a total force on the car above it wrecks
  it (func_80071724). "Pitch/roll I" (0x800C08B0, car +0x5B4) is the weight used in car-to-car collisions
  (func_8006E8D8). Every car turns with its descriptor's inverse inertias (+0x0C / +0x10 / +0x14 = 1 / 1527, 1636,
  1000; yaw is +0x10; func_80069A14), the same in every Rush 2 and 2049 descriptor.
- Car +0x5B0 is the mass in slugs, +0x5AC the weight in lb (mass x 0x80110018 = 32.2 x the race's gravity setting,
  0x800A5E4C), also each wheel's load cap.
- Straight line: func_8006A2FC passes on torque x m / (m + drivetrain inertia) and the clutch slips (func_80070730),
  so the Pickup takes 7.6 s to 100 mph (formulas: 5.4 s) and tops out at 157 mph (formulas: 163).
- The Rocket (type 20) has no drivetrain: func_800712EC gives it a thrust (+0x118) instead.

**Physics step** (func_80071D78, per car per frame; dt = the frame time clamped to 0.016-0.1 s, car +0x718):
func_80071A1C (inputs: front wheel angle +0x390 = +0x3A4 x steering, +0x3A4 = 540 / descriptor +0x50 degrees = 33.75;
throttle +0x3B4, brake +0x3B8, clutch +0x3B0, gears), func_800715E8 (crash), func_800712EC (drivetrain: func_800711F4
automatic shifts, func_80070DB8 engine torque, func_800709E8 ratio, func_80070730 clutch, then the differential: rear
wheel torques +0x39C / +0x3A0 split by the wheels' loads), func_800706D0 (forces: func_800703EC ground contact and
suspension travel, func_8006AFD8 wheels and drag, func_8006A02C force sum, func_80069E74 steering yaw terms,
func_80069AB4 torque = sum of force x position, func_80069A54 / func_80069A14 accelerations, func_800697D0 velocities,
func_8006942C position and orientation).

- Body-frame velocity is car +0x34 (x, sideways), +0x38, +0x3C (z, forward); angular velocity +0x40, +0x44 (yaw),
  +0x48; speed +0x3D4. A point's velocity is v + position x angular velocity (func_8006AE3C), so a turn toward +x has
  a negative yaw rate.
- Tire (func_8006A2FC, called by func_8006ABB0 per wheel): a brush model. Wheel struct (car +0x424 + 0x5C x wheel) =
  the tire curve's six floats (radius 1, 4080 and 200: deflection spring and damper, +0xC cornering stiffness 16000
  front / 32000 rear, +0x10 grip 2.6 front / 3.2 rear in Rush 2 and 2.8 rear in 2049, +0x14 inverse wheel inertia),
  then +0x18 static load and coefficients from it (func_8008D84C), +0x44 deflection, +0x48 wheel spin. The side force
  is load x a cubic in the slip, capped at sqrt(grip^2 - (drive force / load)^2) x load (friction circle); torque past
  grip x load spins the wheel, which then gives no side force. func_8006ABB0 then scales the rear wheels' side force
  by descriptor +0x78 x 0x800C0FF8.
- Steering yaw terms (func_80069E74), with x = 1 - forward speed / speed: the slide term, speed x ((x - x^4) x
  throttle + x^4) x steer force (+0x5A0), halved when the yaw gain is not 0, acts against the yaw only while sideways
  and yaw velocity have the same sign (the tail out and still swinging out: a spin or a fishtail's overshoot); the
  steering term is -steering x speed x yaw gain (+0x5A4). The steering input is the stick cubed, passed on at once
  (func_80076694), with no reduction at speed.
- The engine gives torque at rest only with +0x3DA / +0x3DC set (set when a race starts, 0x8008E3B4), and
  func_800711F4 holds neutral while the asked gear +0x3E0 is 0.

**Accurate Car Stats.** The bars come from driving a test car through that code (`TestCar` in `src/car2049.cpp`: the
game's own func_8008DBA0, func_800712EC, func_8006A2FC and func_80069E74 on physics car slot 0, with flat ground, a
level car and the motion integration supplied; its header comment lists every step). ACCELERATION = 1 / seconds to
100 mph; TOP SPEED = the speed it settles at; DRIFTING = the widest slide angle in 1 s of full steering from 100 ft/s, drawn on a ratio (log) scale (over all
2289 handling setups the angles run 3.6 to 98 degrees with the median at 23);
CONTROL = 1 / the sliding over 3 s after being put into a 30 degree slide spinning at 1.5 rad/s, stick centered. Each
bar runs from the lowest to the highest value over every selectable car and option (about 0.9 s to work out, once per
Car Speeds / 2049 cars / Torque Rebalance state; results are cached per setup). TIRES' D grade and SUSPENSION move
DRIFTING; TIRES' S grade moves CONTROL; rear grip and weight move both. Not shown by any bar: the off-road tires'
grip on dirt and grass.

**Torque Rebalance.** Patches the LOW and HIGH maps in RDRAM (`torque_changes`): LOW gains torque from 3450 to 5750
rpm and loses it above 6900, HIGH gains it above 6900. On every car LOW then accelerates hardest and tops out lowest,
HIGH the reverse, STANDARD between (Pickup, lightest: 6.8 / 7.6 / 8.4 s to 100 mph, 153 / 157 / 163 mph).

Still to do: HANDLING on SUSPENSION, the 2049 tires (SLICKS, PRO SLICKS on all cars; 2049's five on 2049 cars, with
unlock items), 2049's steering (func_800E1AA0), torque map and ENGINE rear grip for 2049 cars (the test car will pick
those up once the race code has them). Rush 2 car +0x734 is the throttle and +0x730 the brake; the comments in
include/ghost_logic.h and src/ghost.cpp have them swapped.
