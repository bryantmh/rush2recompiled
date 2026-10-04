# Rush 2049 cars in Rush 2: findings so far and plan

Status: implemented (October 2026, `src/car2049.cpp`); �4 below is the original plan, superseded by �8. Tags:
**[V]** verified, **[I]** inferred.

## 1. Rush 2049 car data [V]

`tools/rush2049/cars.py` (`python cars.py 49`, `python cars.py convert`, `python cars.py desc-map`) dumps everything
below.

- Files 88–100 = cars 1–13. Each file holds only `CARnFRAME1` (body, 2 LODs), `CARnHOOD` and `CARnSHEEN`: there are
  no damage panels, so 2049 dents the body by moving its vertices at runtime (code not found yet).
- Names: prefix table 0x80110D3C (`CAR1`..`CAR13`), display names 0x80110D70: FORMULA 1, 8-BALL, ROCKET ZX, MAGNUM,
  SUPER GT, BRUISER, LOCUST LX, GX-2, MINI XS, VENOM, CRUSHER, EURO LX, PANTHER.
- Physics descriptor: 0xB4 bytes, pointer per car at 0x80110D08 (6 distinct, at 0x801108D0..). Rush 2's is 0xEC bytes
  (11 at 0x800BFC90, pointer table 0x800C07BC, type -> index 0x800C07E8). `cars.py` `DESC_MAP` maps every field.
  - Shared: inertia constants, suspension, aero drag, rolling resistance, 4 tire-curve pointers, wheel positions,
    drivetrain inertia, final drive, clutch, auto-shift thresholds.
  - Rush 2 only: rear lateral grip (+0x78), torque scale per gear (+0xB8..+0xC4), gear ratios (+0xC8..+0xDC), top
    gear (+0xE0). 2049 derives these from per-player setup tables (transmission 0x801116D0, gear sets 0x80110EBC,
    torque 0x801110C4, rear grip 0x80111130/0x8011121C); `cars.py derived()` computes them for a drone's setup.
  - Per-car tables: mass 0x80110DD8, inertia 0x80110E0C, yaw inertia 0x80110E44, preload 0x80110DA4, drive flags
    0x801114E4, collision box 0x8011F844, wheel scale 0x801112DC / 0x801113E0.
  - The drivetrain, steering and rear-tire code differ slightly between the games, so converted stats drive close to,
    not exactly like, 2049 [I].

## 2. Rush 2 per-car tables [V]

`tools/rush2049/refscan.py START END` lists every instruction addressing a data range (lui + addiu/load pairs,
including indexed `addu`). For 0x800C06B4–0x800C0FE0 it finds **144 sites** over about 40 table bases, many laid out
as 3 rows x 22 (row stride 0x58 or 22 bytes). Extending all of them to 35 cars is a large, error-prone patch, so the
plan below borrows slots instead.

## 3. Car select (overlay `func_803B81F0`) [V partly]

- Builds a per-player list of selectable cars at 0x803CB368 (2 bytes per entry: [i*2 + player]) with counts at
  0x803CB398 (-0x4C68 off 0x803D0000): types 0–15 always, 16–19 when unlocked (keys, `func_803B1AB0`), 20 by a save
  flag, 21 by keys / 4 (0x803B85C0–0x803B8738, loop bound 0x16).
- Carousel: 0x18-byte entries per car per player at 0x803CB980 + player * 0x210 (22 x 0x18), built at
  0x803B8DA8–0x803B8E94; the second player's types are offset by 0x16.
- Loads assets 1, 0x11, 0x12 and 0x1C on entry (0x803B8770–0x803B8798). **0x1C holds the stripe and decal
  textures** (`1F_64X32_L2`, `1ST_…`, `2F_…`: per-stripe, per-side tiles), not preview models [V], so the select
  screen previews the real car assets (0x1D + type, loaded through `func_800A37F4`'s car slots) [I].
- Per-car stat setup calls at 0x803B8B64–0x803B8CC0 (`func_800B2694` .. `func_800B2424`, 22 iterations).

## 4. Plan: borrowed host types (like the tracks)

- The car select offers types 22–34 after Rush 2's 22 (list, carousel and preview tables copied to larger arrays in
  free RDRAM and the instructions repointed, as `src/track2049_menu.cpp` does for the track select).
- In a race each 2049 car runs as a host Rush 2 type whose per-type table rows (the 144 sites' tables), descriptor
  pointer and car asset (0x1D + host) are swapped for the 2049 car's while racing, then restored. Two hosts cover two
  players: **HOTROD (17)** and **GT90 (19)**. The AI picks only 0–15, and neither has special cases
  (`func_8008582C` special-cases 0x12/0x14/0x15; `func_800A37F4` forces TAXI on some tracks).
- Car model: convert `CARnFRAME1` into a Rush 2 car container with all 39 part names (`func_80086700`, suffix list
  0x800C6180; handles are used unchecked) pointing at the intact body so it draws once; port 2049's vertex denting
  onto the body's vertices; wheels from Rush 2 or 2049 files 80/81 placed by the descriptor; paint mapped onto Rush 2's
  main/accent colours.
- Saves: the selected type is saved in the player record; a 2049 choice keeps the old value and is stored in a side
  file, like `track2049.json`.
- Engine sound: keep the host car's Rush 2 engine first; 2049's engine through `src/audio2049.cpp` later.

## 5. Rush 2 car asset layout [V] (asset 0x1D, PICKUP)

37 models: `PICKUPFRAME1` (the chassis), five body panels `FL1 FR1 RL1 RR1 TOP1` in damage stages `D0_` (intact),
`D1_TOP1`, `D2_` (wrecked), each with a dented variant (`D0_DFL1`.., `D0_D2FL2`..), and headlights `H0_L/R`,
`H0D_L/R`, `H2_L/R`, `H2D_L/R`. Textures are per damage stage (`TRK_D0_1`..`TRK_D1_6`, with `_4` variants). So Rush 2
shows damage by swapping whole panels. For a 2049 car: FRAME1 = the 2049 body (`CARnFRAME1` + `CARnHOOD`), every
panel and headlight part = an empty display list, and 2049's vertex denting applied to the body.

## 6. Car options (part select) plan

Rush 2's list (text after "CAR" in main data): Transmission, Main Color, Accent Color, Stripe, Stripe Color, Tire Rims,
Horn, Engine, Torque, Suspension, Tires, Durability (and Tire Size F/R, apparently hidden). Rush 2049's setup (file 56
models): Engine 1-5, Frame 1-3, Shocks 1-3, Tires 1-4, Trans 1-2, plus wings and paint.

2049 cars use Rush 2's list. Every row is first verified in both games' code (the option value traced to what reads
it), then:
- same kind of effect: the row uses 2049's values for 2049 cars;
- different kind of effect: the row stays (closest Rush 2 label, or 2049's own name), and for 2049 cars it applies only
  2049's effect; Rush 2's effect is dropped for those cars;
- cosmetic in one game only: kept cosmetic, or hidden for the cars it doesn't fit (e.g. Rush 2's per-panel stripes).
Candidate pairs to verify: Engine-Engine, Suspension-Shocks, Tires-Tires, Durability-Frame, Torque-gear sets, Tire
Rims + 2049's rims (file 81). Rush 2 cars keep their stock rows and values. Choices are saved in a side file.

## 7. Open questions

1. 2049's denting code (vertex deformation on hits) and where Rush 2 detects hits (its D0/D1/D2 panel swaps).
2. Asset 0x1C's contents (car-select previews?) and how the select screen draws them.
3. How Rush 2 and 2049 paint cars (PRIM/ENV, palettes, texture prefixes 0x800C64C4; 2049's SHEEN pass).
4. Wheel models and placement.

## 8. Implementation [V]

- **Types.** The 2049 cars are Rush 2 types 23-35 (type 22 stays Rush 2's "no car" marker). The 22-entry per-type
  tables (�2) and the per-type box tables move to 36-entry copies at 0x80200000.. (`tools/rush2049/cartypes.py`
  generates the us.toml patches); preview ids become player * 36 + type, so the race/preview per-car arrays move to
  72-entry copies (0x80218000-0x80221EE8).
- **The part-handle table 0x8010C480 is not just 44 cars x 39 parts.** Indices 1716 and up hold track-object,
  debris and smoke model handles (the 0x800C5400 table: CONE1 = 1814, ...; func_80059450 uses 0x6C5), indexed from
  the same base by the object and particle code (func_80059450, func_8005AEA4, func_8005E918/F718/F900/FC08,
  func_8008A01C). Those sites keep the old base (cartypes.py `ID_EXCLUDE`); relocating them drew the sky model at
  every smoke puff.
- **Assets.** Each car is converted at boot (`convert_car`, track2049_convert.cpp) into a Rush 2 car container served
  as asset 0x71 + n: CARnFRAME1 as FRAME1, every other part an empty list. CARnHOOD is not drawn: it is a separate
  hood piece, off-centre (Rocket ZX: x -41..24), that 2049 doesn't draw on an intact car (the body has its own hood);
  drawn as the top panel it stuck out of the body. CARnSHEEN (a reflection pass) is left out too. 2049's
  vertex colours are a flat grey that 2049 relights at runtime, so they are set to white; the `TEX0 * SHADE * PRIM`
  combiner loses its PRIM stage (Rush 2 leaves PRIM unset).
- **Paint.** The game loads each car's own palette (built by func_8008582C from CARPALETTE) as the TLUT of all the
  car's textures. 2049 paints a car (func_800B10D4, called from func_800B1B48 with the physics car's +0x7CD..+0x7CF,
  copied from the setup's COLOR 1-3) by copying its CnPALETTE and generating three ramps: entry 32 / 64 / 96 is
  COLOR 1 / 2 / 3, the next 31 (30 for COLOR 3) step from it to black (func_800B0EA0, t = 0, 8, .. 0xF0); with COLOR 1
  white, entries 1-31 are brightened x1.25. Colours come from 0x8013F300, built from the RGB table 0x801226C0: the
  same 32 colours, index for index, as Rush 2's 0x800CE19C (slightly retuned). At the end of func_8008582C a 2049
  car's palette is painted that way with MAIN, ACCENT and STRIPE COLOR (args at sp+0xD3, 0xD7, 0xDF) as COLOR 1-3,
  using Rush 2's table so the car matches the select screen's swatches. 2049's artists mostly use COLOR 1 for the body,
  COLOR 2 for the second colour (lower body, side panels, the Bruiser's racing stripes) and COLOR 3 for trim and
  pinstripes; the Euro LX differs (COLOR 3 is its roof and hood, COLOR 1 a band along its sides) and is kept as 2049
  authored it. Rush 2's STRIPE patterns are not drawn on the 2049 cars: Rush 2 stamps its stripe tiles onto its own
  cars' panel textures (func_80083F50), which don't exist on a 2049 body. 2049's per-car defaults (COLOR 1-3:
  0x80111604, 0x80111648, 0x8011168C; func_800BB140 gives them to the drones) become the 2049 types' default rows
  (0x800C0ED0 / 0F14 / 0F9C, relocated) and seed new save slots (record block: MAIN bits 5-7 of +1 and 0-1 of +2,
  ACCENT bits 2-6 of +2, STRIPE bits 0-2 and STRIPE COLOR bits 3-7 of +3; func_800B266C / 2640 / 2608 / 25D0).
- **Rims.** Rush 2 draws RIM n as texture RIM0n (asset 0x12), looked up at boot into 0x800E7950 (RIMB1-3, then
  RIM01-RIM21, names 0x800C6088) and set on the wheel node (func_80085FD0 -> func_8005A4C0). 2049 does the same
  (names 0x8011B3A0 -> 0x80143F68, RIM01 + value) with the same 21 designs (file 80); only its palettes differ, about
  18% darker for its lighting. So 2049's rims are already every car's TIRE RIMS choices; a 2049 car starts with its
  own 2049 rim (0x8011157C, record block +4, table 0x800C0E48).
- **Rocket ZX flames.** 2049 hangs three effect models (file 62: ROKTFLAMEG1-3) behind the Rocket ZX
  (func_800AF690 when the car index is 2) at 2 ft up, 5.6 ft back (func_800930A4), and rescales them every 1/30 s:
  flames 1 and 2 in length, flame 3 in width, flickering around a size that grows with the engine. Their ROKTFLAME
  texture scrolls (track scroll tables: kind 9, 16 per 1/60 s, wrap 512). The converter takes just those models from
  file 62 (`subset_model49`: their lists, vertices, texture loads and texels, but no texture or palette records; an
  extra texture record crashed Rush 2's car texture code) and appends calls to them to the body's lists; car2049.cpp
  places, flickers (gas pedal in place of rpm) and scrolls them.
- **Car select.** The per-player list and carousel move to larger copies; the name logos (asset 0x11, looked up by
  name through the overlay table 0x803C7688) get 128x32 "R49CARn" images drawn in Rush 2's logo style.
- **Physics.** Rush 2 0xEC-byte descriptors are built from 2049's 0xB4-byte ones with each car's drone setup
  (cars.py `to_r2`), plus mass, inertia, preload, drive flags and box. 2049's cars differ little among themselves
  (mass, inertia, wheel positions; 2049 tells them apart by setup), and 2049's torque scale (2.2) is above all of
  Rush 2's (1.8-2.05). So each 2049 car takes a Rush 2 analogue (Formula 1 -> FORM1, 8-Ball -> HOTROD, Rocket ZX ->
  GT90, Magnum -> CAMARO, Super GT -> VETTE, Bruiser -> PICKUP, Locust LX -> INTEG, GX-2 -> CONCPT, Mini XS -> VWBUG,
  Venom -> VIPER, Crusher -> SUV, Euro LX -> BMW, Panther -> BUGAT) for what 2049 has no counterpart of: the per-gear
  torque scale and the steering, yaw-damping, suspension and tire rows. The car select's stat bars (func_803B7F7C:
  ACCELERATION from the weight and torque scale, TOP SPEED from torque / top gear, CONTROL from steering, DRIFTING
  from yaw damping and rear grip) then differ per car and sit in Rush 2's range.
- **Damage.** Rush 2 swaps panel models by five 2-bit damage levels (car struct 0x801124A0 + car * 0x354, +0xE4;
  masks 0x800CE188, shifts 0x800CE180; panels FR, FL, RR, RL, top; copied from the physics car's +0x7F4 by
  func_8009A264). Collisions raise a panel to level 1 only (func_8006B730, func_8009A264 and func_800715E8 all stop at
  `slti 2`). A 2049 car instead has its body crushed around each damaged corner (towards the middle, shorter, lower)
  and its roof pushed down (hook at func_8005CFB0, each race car's per-frame callback), as 2049 dents its single body;
  repairs restore it.
- **Computer cars.** func_800A37F4's random drone car (0-15) also draws types 23-35 when the 2049 cars are on.
- **Damage textures.** func_800843EC builds texture names from the per-type prefix table 0x800C64C4 (22 entries);
  its read moves to a 36-entry copy where the 2049 types get a prefix no texture has.
- **Options.** See §9.
- **Saves.** The extra types' per-car options live in side slots (16 x 0xC0 at 0x80222E00, one per player record),
  kept in `car2049.json` and keyed by record address: the active players' records (0x8010D740 + k * 0x6C0) and the
  Controller Pak image's (0x8004B220, 4 x 0x2200); the Controller Pak record keeps its layout. A new slot starts each
  2049 car with the record's Pickup options, ENGINE 1 and the car's own frame weight as its durability.
- **Unlock All Cars** (src/cheats.cpp) rebuilds the list in the moved copy and appends the 2049 cars again.
- **Option.** "Rush 2049 Cars" on the Rush 2049 tab (default on).

## 9. Car options on the 2049 cars [V]

Rush 2049's SETUP rows (menu text, file 0): TRANSMISSION, HANDLING, ENGINE, TIRES, FRAME, WINGS, COLOR 1-3, TEAM,
TIRE RIMS, HORN, TIRE SIZE F, TIRE SIZE R, SHEEN. func_800F6F7C copies a player's setup struct into the per-player
rows (13 bytes per car; row 0 = the drones' defaults): +0x9 gear set (TRANSMISSION), +0xA..+0xE rows B (0x8011103C),
C (0x80111080), D (0x8011119C), E (0x80111230), W (0x8011128C), +0xF..+0x11 colours, +0x12 F (0x8011157C), +0x13 G
(0x801115C0), +0x14/+0x15 tire sizes (v / 100 + 0.75). func_800C760C hands the rows to the setup screen as items
0-11 in the menu's order (B, C, D, E, W, colours, F, G, sizes), so B = HANDLING, C = ENGINE, D = TIRES, E = FRAME,
W = WINGS, F = TIRE RIMS, G = HORN. What they drive (physics car fields +0xB = B, +0xC = C, +0xD = D, +0xE = E):

| 2049 row | Table | Effect |
|---|---|---|
| HANDLING (B, 0-2) | 0x801116D0 [B] (0x2C each) | gear ratios x +3 (1.1, 1.075, 1.05); torque column of 0x801110C4 |
| ENGINE (C, 0-5) | 0x801110C4 [C][B], 0x80111130 [C][B] | torque scale 2.2 .. 2.5; a 1.0 .. 1.1 factor (func_800BCxxx) |
| TIRES (D, 0-4) | 0x8011121C [D] | rear lateral grip 1.0, 0.9, 0.8, 1.0, 1.0 |
| FRAME (E, 0-5) | 0x80111274 [E] | frame weight 0 .. 1.0: mass + (w - k0) * k_mass, inertias + (w - k1) * 1000 / k_yaw |

Rush 2's rows (value strings from 0x800C4960): TRANSMISSION, MAIN/ACCENT COLOR, STRIPE, STRIPE COLOR, TIRE RIMS, HORN,
ENGINE (10 engine sounds), TORQUE (LOW .. ROCKET: torque curve), SUSPENSION (LOOSE, NORMAL, TIGHT, WHEELIE: yaw
damping and a suspension curve, func_8009A264), TIRES (S0 D0 .. S2 D2, HALF / FULL OFFRD: steering, yaw, off-road
grip), DURABILITY (record byte / 100: the weight). The 2049 cars keep Rush 2's list:

- **ENGINE** -> 2049 ENGINE. A different kind of effect (Rush 2's is a sound): for 2049 cars the row is the power
  level, shown as ENGINE 1-6 (the value's text, func_803BC048, and its wrap, func_803B9478), and the car keeps its
  default engine sound (func_8009E6DC).
- **DURABILITY** -> 2049 FRAME: the durability value is the frame weight (a car's default is its own frame).
- **SUSPENSION** and **TIRES** keep their full Rush 2 effect (yaw damping and suspension curve; steering, yaw and
  off-road grip, from the analogue's base values). 2049's HANDLING stays at each car's own setup; its 0x801116D0 +9
  value (1.0 / 0.5 / 0.0) is untraced, so a HANDLING row is left for later.
- MAIN, ACCENT and STRIPE COLOR are 2049's COLOR 1-3 (§8 Paint). TRANSMISSION, TIRE RIMS, HORN, TORQUE and the tire
  sizes keep Rush 2's effect; STRIPE has nothing to draw on a 2049 body. 2049's WINGS are the Rush 2049 tab's wings;
  TEAM and SHEEN have no Rush 2 counterpart.

A player's choices are applied at car init (func_8008DBA0): a per-car copy of the descriptor (0x80225000, 8 x 0xEC)
gets the torque scale x torque[ENGINE][h] / torque[0][h] (h = the car's own HANDLING); the car's mass, inertias and
weight force come from FRAME. Drones keep their 2049 drone setup. The 0x80111130 factor's use (func_800BCxxx) is not
traced, so it isn't applied [I].

The car select's bars (func_803B7F7C) read the type's descriptor and Rush 2's weight (func_8008DB04), so for the
duration of the call (hooks at its entry and its `jr`) the selected car's descriptor pointer is swapped for a copy
(0x80225A00, per player) whose torque scales carry the TORQUE curve and a 2049 car's ENGINE. Rush 2's own bars leave
TORQUE out. The three curves (0x800C0CC0: LOW, STANDARD, HIGH; ROCKET is STANDARD's; func_80070A78 reads columns
every 1150 rpm, rpm = engine rad/s x 9.549, last row = full throttle) are shared by every car, so each counts by a
measured factor. Races with every car forced onto each curve (same track and field; automatic shifts at +0xE4 x
(throttle + 3) / 4, about 6325 rpm at full throttle; top speed on the straights in top gear at 6100-8100 rpm) gave
start to 5000 rpm in gear 3 in 7.58 / 7.41 / 7.47 s and mean top-gear rpm 7165 / 7096 / 7051: ACCELERATION x
STANDARD's time over the curve's, TOP SPEED x (rpm / STANDARD's)^2 (its bar term is linear in force). LOW pulls
hardest below about 3500 and above 7000 rpm, HIGH only between about 5200 and 6600, which the cars pass through
without settling, so HIGH comes out slightly behind STANDARD on both. a 2049 car's mass entry stands in
for its FRAME mass. CONTROL and DRIFTING already follow TIRES and SUSPENSION.
