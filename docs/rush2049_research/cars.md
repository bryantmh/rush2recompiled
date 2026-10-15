# Rush 2049 cars in Rush 2: findings so far and plan

Status: implemented (October 2026, `src/rush2049/car2049.cpp`); �4 below is the original plan, superseded by �8. Tags:
**[V]** verified, **[I]** inferred.

## 1. Rush 2049 car data [V]

`tools/rush2049/cars.py` (`python cars.py 49`, `python cars.py convert`, `python cars.py desc-map`) dumps everything
below.

- Files 88–100 = cars 1–13. Each file holds only `CARnFRAME1` (body, 2 LODs), `CARnHOOD` and `CARnSHEEN`: there are
  no damage panels, and 2049 shows no damage on its race cars: no code in the main binary or the race overlay
  stores vertex positions into a car model (scan of every x/y/z halfword store, 2026-10-11; the only car damage is
  the battle health +0x386). The dents on the converted cars are this port's own, standing in for Rush 2's panels.
  Searched [V, 2026-10-11], main code and race overlay: (a) x/y/z halfword stores, and functions that read a
  vertex's x/y/z and store halfwords: only HUD rectangles (func_800A7508), world polygons (polygon_set_8008C074),
  fog, paths, texture scrolls and mover polygons; (b) the OBHD per-LOD vertex pointer: never read (geometry.md §3.1);
  (c) car nodes (func_800B15B4 -> func_800AF5E0 -> func_8008E26C) point at the shared model with no per-car vertex
  copy; (d) the model draw func_8009C8F0 edits no vertices; (e) no G_MODIFYVTX (0x02) commands are built.
  The earlier "2049 dents the body (code not found yet)" was an unverified guess.
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
  free RDRAM and the instructions repointed, as `src/rush2049/track2049_menu.cpp` does for the track select).
- In a race each 2049 car runs as a host Rush 2 type whose per-type table rows (the 144 sites' tables), descriptor
  pointer and car asset (0x1D + host) are swapped for the 2049 car's while racing, then restored. Two hosts cover two
  players: **HOTROD (17)** and **GT90 (19)**. The AI picks only 0–15, and neither has special cases
  (`func_8008582C` special-cases 0x12/0x14/0x15; `func_800A37F4` forces TAXI on some tracks).
- Car model: convert `CARnFRAME1` into a Rush 2 car container with all 39 part names (`func_80086700`, suffix list
  0x800C6180; handles are used unchecked) pointing at the intact body so it draws once; dent the body's vertices in place of
  Rush 2's damaged panels (2049 has no denting to port); wheels from Rush 2 or 2049 files 80/81 placed by the descriptor; paint mapped onto Rush 2's
  main/accent colours.
- Saves: the selected type is saved in the player record; a 2049 choice keeps the old value and is stored in a side
  file, like the save file's `track_select` section.
- Engine sound: keep the host car's Rush 2 engine first; 2049's engine through `src/rush2049/audio2049.cpp` later.

## 5. Rush 2 car asset layout [V] (asset 0x1D, PICKUP)

37 models: `PICKUPFRAME1` (the chassis), five body panels `FL1 FR1 RL1 RR1 TOP1` in damage stages `D0_` (intact),
`D1_TOP1`, `D2_` (wrecked), each with two translucent sheen layers over it (`D0_DFL1`.. and `D0_D2FL2`.., §11; not
dents), and the hood halves `H0_L/R`, `H2_L/R` with their sheen layers `H0D_L/R`, `H2D_L/R` (not headlights: PICKUPH0_L runs from the windshield's base, z 21 y 36, to the nose,
z 98 y 23; H2 is the same hood buckled up across a crease at z 70, up to y 43). Textures are per damage stage
(`TRK_D0_1`..`TRK_D1_6`, with `_4` variants). So Rush 2 shows damage by swapping whole panels. For a 2049 car: FRAME1
= the 2049 body (`CARnFRAME1`), every panel and hood part = an empty display list (but the sheen layers are made from
the body, §11), and the body dented in place of the panels (2049 has no damage of its own).

### Damaged hood [V]

func_8005CFB0 picks the hood halves each frame by panel 0's damage level (mask 0x800CE188[0], shift 0x800CE180[0]):
node +0xC gets part handle 35 + level (`H0D_L`, `H2D_L`), node +0x10 part 37 + level (`H0D_R`, `H2D_R`), from the
handle table 0x8010C480 + car * 0x4E (+0xC / +0x10 hold the halves' sheen layers, §11). A collision that raises panel 0 to 1 buckles the hood. The 2049 cars and car
mods are one body, so `body_hood` (src/rush2049/car2049.cpp) marks the body's hood vertices (the top surface of the
front, z 0.2-0.95 of the half length, no body vertex just above) with a fold weight peaking at a crease 60% of the
way from the windshield's base to the nose, and `rush2_car49_dent` lifts them by 0.2 of the height times panel 0's
level. `CARnHOOD` is not a damaged hood: 2049 shows it only in its hood camera (car view struct 0x80152818 + player *
0x3B8, +0x35D view mode 1 of 0-3, cycled by the view button in func_800EB90C; drawn by func_80093B20 when one player
races; func_800B200C sets the node up hidden), and hides it otherwise.

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

1. ~~2049's denting code~~: none exists (§1). Where Rush 2 detects hits (its D0/D1/D2 panel swaps).
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
  as asset 0x71 + n: CARnFRAME1 as FRAME1, every other part an empty list. CARnHOOD is not drawn: it is the hood
  2049's hood camera shows (Damaged hood, above); drawn as the top panel it stuck out of the body. CARnSHEEN (a reflection pass) is left out too (§11). 2049's
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
  (mass, inertia, wheel positions; 2049 tells them apart by setup). The torque scale and gear ratios are 2049's for
  the drone setup mapped onto Rush 2's roster by a fixed formula (�10). The steering, yaw-damping, suspension-setting
  and handling-setting rows, Rush 2 tuning 2049 has no counterpart for, are the means of Rush 2's roster. The car
  select's stat bars (func_803B7F7C: ACCELERATION from the weight and torque scale, TOP
  SPEED from torque / top gear, CONTROL from steering, DRIFTING from yaw damping and rear grip) then sit in Rush 2's
  range and differ between 2049 cars only by weight and ENGINE, as in 2049.
- **Damage.** Rush 2 swaps panel models by five 2-bit damage levels (car struct 0x801124A0 + car * 0x354, +0xE4;
  masks 0x800CE188, shifts 0x800CE180; panels FR, FL, RR, RL, top; copied from the physics car's +0x7F4 by
  func_8009A264). Collisions raise a panel to level 1 only (func_8006B730, func_8009A264 and func_800715E8 all stop at
  `slti 2`). A 2049 car instead has its body crushed around each damaged corner (towards the middle, shorter, lower)
  and its roof pushed down (hook at func_8005CFB0, each race car's per-frame callback) (2049 itself shows no damage);
  panel 0's damage also folds the hood up (Damaged hood, above); repairs restore it.
- **Computer cars.** func_800A37F4's random drone car (0-15) also draws types 23-35 when the 2049 cars are on.
- **Damage textures.** Rush 2's damaged panels (D1 models) use its D1 textures: copies of the D0 ones whose paint
  indices are mottled lighter and darker in blotches. func_800843EC builds those names from the per-type prefix
  table 0x800C64C4 (22 entries); its read moves to a 36-entry copy where the 2049 types get a prefix no texture has.
  2049 has no damage textures (its car files hold only FRAME1, HOOD and SHEEN), so the converted body gets them: each
  painted CI8 texture (10% or more of its texels in the paint ramps 32-127) gets a scuffed copy (blotches of +-8 ramp
  steps, a few bright scrapes; other texels kept), and the body's lists (both LODs) are rewritten with every load of
  such a texture inlined before the triangles of each panel it textures (panel by triangle centroid: top above half
  the height, else front/rear by z, right/left by x), a G_TRI2 split where its halves differ. The relocator rebases
  the inlined G_SETTIMGs (pristine); the dent hook points each panel's at the scuffed copy while that panel is
  damaged. This makes the cars up to about 0xF000 bytes, so the car slot size word 0x8001CF28 (0xD1F0, the largest
  Rush 2 car; func_800A37F4 allocates each slot at it + 0x400) is raised to the largest car at boot.
- **Options.** See §9.
- **Saves.** The extra types' per-car options live in side slots (16 x 0xC0 at 0x80222E00, one per player record),
  kept in the `cars` section of `saves/rush2.n64.us.json` and keyed by record address: the active players' records (0x8010D740 + k * 0x6C0) and the
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
  level, shown as ENGINE 1-6 (the value's text, func_803BC048, and its wrap, func_803B9478). In 2049 the ENGINE
  level also picks the engine sound (audio.md �7), so a 2049 car plays 2049's engine for its level
  (src/rush2049/engine2049.cpp); Rush 2's sound byte (func_8009E6DC) keeps the car's default.
- **DURABILITY** -> 2049 FRAME: the durability value is the frame weight (a car's default is its own frame).
- **SUSPENSION** and **TIRES** keep their full Rush 2 effect (yaw damping and suspension curve; steering, yaw and
  off-road grip, from the analogue's base values). 2049's HANDLING stays at each car's own setup; its 0x801116D0 +9
  value (1.0 / 0.5 / 0.0) is untraced, so a HANDLING row is left for later.
- MAIN, ACCENT and STRIPE COLOR are 2049's COLOR 1-3 (§8 Paint); STRIPE COLOR reads TERTIARY COLOR on a 2049 car.
  TRANSMISSION, TIRE RIMS, HORN, TORQUE and the tire sizes keep Rush 2's effect; STRIPE has nothing to draw on a 2049
  body and is hidden. A Dreamcast disc's car has paint jobs in place of the three colors: one STYLE row (see Car select
  rows). 2049's WINGS are the Rush 2049 tab's wings; TEAM and SHEEN have no Rush 2 counterpart.
- **TIRE SIZE F / R** (wheel model scale only, func_8005A598; no physics): Rush 2's per-type scale tables 0x800C0A6C /
  0x800C0B74 (relocated to 0x802006E8 / 0x802009B8, 5 rows) get 2049's own row-0 scales 0x801112DC / 0x801113E0: 1.0
  for every car except the Venom (rear 1.1) and the Crusher (1.4). The car select keeps a player's sizes in record
  bytes 0x58F / 0x590 (block +11 / +12) as scale x 100 - 75 (func_803B9478 0x803BA488 / 0x803BA638) and
  func_803B81F0 reads them back into the player rows. Until 2026-10-09 the 2049 types kept the Pickup's 1.25 that
  init_tables copies (and new side slots copied the Pickup's record bytes), so every 2049 car's wheels were drawn 25%
  too large; side-slot saves before version 6 are migrated to 2049's sizes at load. A Dreamcast source builds its
  Main segment from `tools/rush2049/dc_tables.py`'s copy list, which lacked these tables (read as 0.0: no tires); they
  are copied now (disc 0x8C0BBE64 / 0x8C0BBF68, identical to the N64's), a scale outside 0.75-1.5 falls back to 1.0,
  and saved sizes outside bytes 0-75 are reset at load. `dc_test.exe segments PACK|ROM.z64 OUTDIR` dumps either
  source's segments (what car2049.cpp reads) for checking.
- **Wheel placement** is the same in both games: the car-start setup copies the descriptor's four wheel positions
  into the car state (Rush 2 `car_race_start_pose_setup_8009A264` 0x8009A604: desc +0x7C.. -> state +0xA8 + 0xC i;
  2049 func_800E847C 0x800E87F8: desc +0x70.. -> state +0xB0 + 0xC i, the same fields 8 bytes later), and the wheel
  draw (Rush 2 func_8005A598, 2049 func_8008D8D8 at 0x8008DC3C) puts each wheel at that point, y plus the wheel's
  suspension travel and a small random bounce (clamped 0-0.5), then scales its matrix by the type's wheel scale
  (0x8008DF40 / 0x8008DFA0; skipped when 1.0). The converted descriptor's +0x7C block is 2049's +0x70 block, so the
  2049 cars' wheels sit where 2049 puts them.

### Car select rows

The car select keeps one option list (0x803CB3B8, count 0x803CB3B0) for both panels, built once per visit by
func_803B81F0. Each panel's rows follow its car (src/rush2049/wings_menu.cpp): the whole list is kept when it is built
(0x803B8488) and the panel's list is put in its place before each read. Readers: func_803B9478's per-slot loop (head
0x803B951C, slot $s7: navigation, left/right), func_803BC048's per-slot loop (head 0x803BC0C8, slot $s2: labels and
values), and the row widgets' callbacks, which take their panel from the widget's +0x2C (it is the panel whose id
equals 0x803CB3F8 or the other): func_803BAA44 (color swatch, bits 8-11), func_803BAC24 and func_803BACF8 (bits 8-11),
func_803BAFD8 (arrows, bits 12-15) and func_803BB9F8 (stat bars, bits 0-3). Per panel: row 0x803C6990[slot], first
shown row 0x803CB3A0[slot] (4 shown), car type 0x803CB362[slot] (s8).

| Car | Rows |
|---|---|
| Rush 2 | unchanged |
| 2049 (N64 ROM) | STRIPE hidden; STRIPE COLOR labelled TERTIARY COLOR |
| 2049 (Dreamcast disc) | ACCENT COLOR, STRIPE, STRIPE COLOR hidden; MAIN COLOR labelled STYLE, value STYLE n, no swatch |

STYLE is the MAIN COLOR byte (0x80201190 + (slot + 1) * 36 + type) modulo the car's paint jobs. func_803B9478 adds a
step to it and wraps at 32 (case 2 after 0x803B98A4); the hook there sets the byte so the sum wraps at the jobs. The
swatch widget (func_803BAA44 shows one for ids 2, 3 and 5) sees STYLE as id 16, so it draws none.

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

### Paint colors [V]

Rush 2's 32 paint colors are RGBA words at 0x800CE19C; Rush 2049's RGB table 0x801226C0 (32 RGBA words, from which it
builds 0x8013F300) holds the same 32 colors index for index, retuned. While the 2049 cars are offered (Games tab), the
2049 shades that no Rush 2 color is close to (CIE76 dE of 10 or more from every one) are colors 32-52, for every car
and for the AI Opponents' paint (src/paint_colors.cpp). They are read from the 2049 ROM's Main segment; a source
without the table (a Dreamcast disc: its Main segment is rebuilt from `tools/rush2049/dc_tables.py`'s list, which
doesn't have it) offers none. Each has a Rush 2 stand-in, the closest Rush 2 color in hue:

| Color | 2049 index | Shade | Stand-in | Color | 2049 index | Shade | Stand-in |
|---|---|---|---|---|---|---|---|
| 32 | 2 | 5A5A5A gray | 2 | 43 | 19 | 296855 sea green | 20 |
| 33 | 4 | DD4156 rose red | 5 | 44 | 20 | 1B5653 dark teal | 20 |
| 34 | 9 | C4D6DF ice blue | 1 | 45 | 21 | E525D0 orchid | 21 |
| 35 | 10 | 6FA0E3 sky blue | 9 | 46 | 22 | B74CE5 violet | 22 |
| 36 | 11 | 1B38E5 royal blue | 10 | 47 | 24 | 660FA9 purple | 24 |
| 37 | 12 | 081B70 dark blue | 11 | 48 | 26 | F0D41D gold yellow | 26 |
| 38 | 13 | 040E38 midnight blue | 12 | 49 | 28 | E08E1A amber | 28 |
| 39 | 14 | 58BB5B leaf green | 16 | 50 | 29 | 8A6449 khaki | 29 |
| 40 | 15 | 00A92F emerald | 16 | 51 | 30 | C8B4CA lilac gray | 1 |
| 41 | 17 | 005A2A forest green | 17 | 52 | 31 | 504136 taupe | 31 |
| 42 | 18 | 3EC1BF aqua | 18 | | | | |

Readers of the color tables, all pointed at max-entry copies in the recomp heap by a hook after the address load:
the RGBA table in race_load_car_assets 0x80087008 (the 2-player ARROW), menu_car_select_run 0x803AF64C / 0x803AF6A0
(the preview car's paint, func_8008547C) and the swatch widget func_803BAA44 0x803BABEC; race_scene_load_assets
0x8008A244 converts it (loop end 0x8008A24C) into RGBA5551 words at 0x8010C180, read by car_build_textures 0x80085C48
($fp: MAIN sp+0xD3, ACCENT sp+0xD7, STRIPE COLOR sp+0xDF) and the HUD map dots (hud_race_setup_widgets 0x800A0810,
0x800A0880, by the car's +0x7EB). src/arrows.cpp and the 2049 paint (rush2_car49_paint) take
`rush2::paint_colors::rgba`.

The player record keeps each color in 5 bits of the car type's block (record + 0x584 + type * 13): MAIN bits 5-7 of
+1 and 0-1 of +2, ACCENT bits 2-6 of +2, STRIPE COLOR bits 3-7 of +3. Setters (they take the record, type and value,
add the block's +1 = record + 0x585 + type * 13 and call a packer): MAIN 0x80097A5C (packer 0x80097A04), ACCENT
0x800979C0 (0x80097978), STRIPE COLOR 0x800978AC (0x80097868); each marks the bytes dirty through 0x8005F338. Getters,
read only by carselect_build_list (0x803B8B6C..) into the car select rows: MAIN 0x800B2694 (via 0x800B266C, which
takes the block's +1), ACCENT 0x800B2640, STRIPE COLOR 0x800B25D0. An extra color is stored as its stand-in, so the
Controller Pak (and a build without the extras) sees the stand-in; the shade's 2049 index goes in the saves' "paint"
section, keyed by the block's +1 address and the field. A 2049 type's block lives in its side slot (§8 Saves), keyed
by the record address it stands for (`rush2::car2049::record_address`). The getters give the extra back while it is
offered and the record still holds its stand-in. Deleting a player forgets its entries.

The car select's left/right on a color row (func_803B9478 cases MAIN 0x803B9960, ACCENT 0x803B9A20, STRIPE COLOR
0x803B9BA0) adds the step to the row byte (0x80201190 / 0x80201248 / 0x802013B8 + (slot + 1) * 36 + type), wraps it
at 32 (31 below 0, 0 from 32) and calls the setter with it. Hooks keep the byte before the add (0x803B9998, 0x803B9A58,
0x803B9BD8) and, at the setter call (0x803B9A0C, 0x803B9ACC, 0x803B9C4C: row byte $v0 + 0x24, value $v1, type $a3,
step $a1), wrap it at the colors offered instead. A Dreamcast car's MAIN row is its STYLE and keeps its own wrap.

## 10. Car speeds [V]

Rush 2049's cars are faster than Rush 2's, and its AI lanes are set for them. The "Car Speeds" option (Games tab,
Rush 2049 section) runs everything at one game's speed: **Rush 2** (default) or **Rush 2049**.

**Drivetrain formulas (both games).** Engine torque = torque map (rpm, throttle) x torque scale (Rush 2: descriptor
+0xB8 gear 1, +0xBC gear 2, +0xC0 gear 3+, func_80070DB8; 2049: trans+8 x 0x801110C4[ENGINE][HANDLING], all gears,
func_800E2D18). Axle torque = engine torque x gear ratio x final drive (3.3 in every car of both games, func_800709E8 /
func_80070730). Rear wheel radius 1.0 ft (tire curve +0, func_8008D950), so wheel force = axle torque. Drag =
0.0135 v^2 + 30 + 75 (func_8006AFD8; same constants in every car of both games). rpm = 9.549 x wheel rad/s x gear x
final drive; up-shift at 6325 rpm. 2049's torque map (0x8011F724) is Rush 2's STANDARD map (0x800CA96C) above 2300 rpm
(more torque below). So the games differ in the torque scale per gear and the gear ratios.

**The rosters.** Rush 2: 21 cars (all 22 types but the secret ROCKET, type 20, whose steering and yaw rows are 0).
2049: its 13 cars in their stock (drone) setup, all on ENGINE 1, HANDLING 0 (torque scale 2.2 in every gear, gears
3.41 / 1.958 / 1.419 / 1.1). They differ only in mass, inertia, wheel positions and box.

| | torque gear 1 / 2 / 3+ | gears 1 / 2 / 3 / 4 | mass (slugs) |
|---|---|---|---|
| Rush 2 mean | 1.820 / 1.842 / 1.886 | 3.095 / 1.754 / 1.254 / 0.956 | 108.4 |
| 2049 mean | 2.2 / 2.2 / 2.2 | 3.41 / 1.958 / 1.419 / 1.1 | 102.5 |

**Cars.** Each 2049 car's torque scale per gear is x (Rush 2 mean / 2049 mean) for that gear (0.827, 0.837, 0.857),
and each gear ratio likewise (reverse 0.909, 1st 0.908, 2nd 0.896, 3rd 0.884, 4th 0.869) (`build_physics`, means
read from the ROMs at boot). The 2049 roster's average lands on Rush 2's average car, and each 2049 car keeps its
place among the 2049 cars; ENGINE's levels keep their ratios to level 1. Masses stay each car's own. +0xC4 (Rush 2's
common-map scale, 1.3 in every Rush 2 car) has no 2049 counterpart: Rush 2's mean. The steering, yaw damping,
suspension setting and handling setting rows (0x800C06B4, 0x800C070C, 0x800C0D68, 0x800C0DAC) are Rush 2's roster
means too (543, 30.7, 1, 5). With Rush 2049 speeds, every race car (Rush 2's, 2049's, drones') gets a descriptor copy
at car init (0x80225000 + slot x 0xEC, `rush2_car49_setup_desc`) with the inverse factors, so 2049 cars run native and
Rush 2's cars move up the same way.

From these formulas (1/60 s steps, full throttle, flat ground, traction not limiting):

| setup | top speed | 0-60 / 0-100 / 0-150 mph |
|---|---|---|
| Rush 2 average car (108.4 slugs) | 168 mph | 2.7 / 6.0 / 14.3 s |
| 2049 average car, native (102.5) | 157 mph | 1.9 / 4.5 / 12.0 s |
| 2049 average car, mapped (102.5) | 168 mph | 2.5 / 5.6 / 13.5 s |

2049's edge is acceleration: its shorter gears reach the end of the torque map (0 at 9200 rpm) earlier.

**Speedometers.** Both read the rear wheels' speed in mph (Rush 2 car+0x76C, 2049 car+0x758: wheel spin x radius x
2.7273 = mph x 4, copied to car-state +0xE0 / +0xF8). Rush 2's HUD (func_800BA2A8) shows it as is; 2049's
(func_800EF8F4) shows it x 1.2 (0x801245AC; x 1.61 more in km/h). A default Rocket ZX reading 183 in 2049 is doing
152.5 mph, what these formulas give it 15 s into a straight from rest (the 157 above takes 30 s or more).

**AI.** The drones are physics cars like the players'. Their driver (func_80074990) aims for the lane's target speed
(u8 mph at lane point +6, x 1.4667 ft/s) x 1.05 x the rubber band (+0x808, func_800A1A98), at least 75/90/135/180
ft/s by the gravity cheat (0x800C3FFC[0x800E7D22]: Light/Normal/Heavy/Jovian) unless the car is moving down, when the lane speed is scaled by 0x800C3FEC (0.8/1.0/1.25/1.5) instead; no stock lane point is below 72 mph, so at Normal gravity the floor never applies. It works the throttle and brake to hold it. Rush 2049's driver (func_800E4B58),
rubber band (func_800F93A0, without the per-track factor), minimum speeds and drone speed hint (polygon flags bits
8-11, x (1 + 0.02 n)) are the same code and constants; its hint points (0x801108B0, func_800EB690) are aim targets,
not speed limits. Its road drag (func_800E23A4) is Rush 2's for stock cars (wings, off-road tires and surfaces 1-2 add
terms). So the lanes alone carry each game's AI pace.

Where a lane is below what the drone's car can do, the drone holds back, and a player can catch it; where it is above,
the drone drives flat out on its line and cannot be caught. Lane speeds (distance-weighted over all four lanes of the
race paths: Rush 2's 7 circuit tracks and 2049's 6 race tracks, both directions):

| | 10th / 50th / 90th / 95th percentile |
|---|---|
| Rush 2 | 111 / 140 / 160 / 162 mph |
| 2049 | 113 / 150 / 180 / 190 |
| 2049 / 1.2 | 94 / 125 / 150 / 158 mph |

**Lane units.** Rush 2's lanes are in true mph: its straights (160-162) sit at its cars' top speed. 2049's are in the
mph its speedometer shows: its straights' 180-190 are its cars' true top speed (about 155) x 1.2, and no 2049 car can
drive 180 true mph on the flat. The shared driver code reads both as true mph, so 2049's own drones aim 20% over the
speeds the lanes were set to and drive its straights flat out.

**Lane map.** So a 2049 lane speed / 1.2 (read from 0x801245AC at boot) is its Rush 2 lane speed: 180 -> 150,
190 -> 158, 120 -> 100. With Rush 2 speeds, `rush2_race_lane_speeds` (before func_800924E4 works out the race time
from the lanes) divides every 2049 path's lane speeds by it; with Rush 2049 speeds the other paths (Rush 2's,
SF Rush's) are multiplied by it and 2049's stay native.

## 11. Car sheen [V]

Graphics > Car Sheen (`src/rush2049/car2049.cpp` `sheen_model`, `add_sheen_layers` in track2049_convert.cpp):
**Original** draws Rush 2's sheen layers on its own cars only (the 2049 cars had none), **All On** on the 2049 cars
too, **All Off** on no car. Hot-swappable: the layers' nodes are drawn or dropped (an empty list) per draw.

**Rush 2's layers.** The car parts named `D0_D*` / `D2_D*` and `H0D_*` / `H2D_*` (§5) are not dents: they are
translucent copies of the panels' and hood halves' triangles drawn over them, white vertices, untextured in the
container, render mode `C8104E50` (translucent, decal depth), combiner `FC41FFFF FFFFF238` (SHADE × PRIM, alpha
TEXEL0), `E3001001 0` (TLUT off) and LOD flag 0x1 (the node's texture swap). Car record (0x80219DD0 + id × 0x134)
slots, as `func_80085FD0` builds them: +0x3C/+0x40/+0x44/+0x48/+0x4C = parts 11/13/15/17/19 (`D0_DFR1`.. `D0_DTOP1`,
the DECAL layer), +0x50/+0x54/+0x58/+0x5C/+0x60 = parts 21/23/25/27/29 (`D0_D2FR2`.. `D0_D2TOP2`, the SHEEN layer),
+0xC/+0x10 = parts 35/37 (`H0D_L`, `H0D_R`, the hood's SHEEN layer; the hood halves themselves are +0x4/+0x8, parts
31/33); each slot's other damage stage is the next part.
- Textures: `func_8005CE68(car, $t0 = 0x21, $s5)` sets the texture swap (`func_8005A4C0`: node +0x14..+0x20, one per
  LOD, or all four with a2 < 0) of +0x3C and +0x44 to DECAL and of +0x50, +0x58 and +0xC to the sheen texture; the
  other layer nodes draw with what their row's first node left in TMEM. The handles are 0x80113F40 (37, names
  0x800C60EC: SPARK01.., DECAL at index 0x21, SHEEN10 / SHEEN7 / SHEEN2 at 0x22-0x24, loaded by `func_8008A1FC`).
  `func_8005CFB0` picks SHEEN7 at covered level (car state +0x344) 1, otherwise SHEEN2 when 0x800D1FA8 >= 3 and
  SHEEN10 below. All are intensity maps (sparkles, city lights, streaks).
- Scrolling: each frame `func_8005CFB0` points node +0x28 (the tile window `func_8007825C` writes as the last
  `G_SETTILESIZE`) of +0x50, +0x58 and +0xC at the car's SHEEN window 0x8010FEB8 + car × 8 and of +0x3C and +0x44 at
  its DECAL window 0x8010F808 + car × 8, recomputed from the car's heading (`func_8005C8E8` = atan2) and the camera
  (view 0x800E79D0 + view × 0x40, car state +0x347), wrapped to 0x1000 / 0x800.
- With car +0xE4 bit 0x10 it hides every layer node (`func_80058D1C`).
- UVs (1/32 texels), fitted over every Rush 2 car: the SHEEN layer (and the hood's) is a cylinder around the width
  axis 8 units below the origin, s = 1008 − 11.37 × (degrees from straight up towards the nose), t = 1008 + ~11 × x
  (rms 2-3 per car); the DECAL layer wraps once around the vertical axis, continuously: s = 2032 at the nose, falling
  to −16 at the tail down the +x side and rising to 4080 down the −x side (a seam of doubled vertices at the tail),
  t ≈ 30-45 × y (the texture's height over the car's). The spacing is authored per car; s = 2032 − 4096/360 ×
  atan2(1.85 × x / half width, z / half length) in degrees fits it within 1-2 texels (VETTE, CADDY, SUPRA, HOTROD,
  INTEG), i.e. a wrap around a body 1.85 times wider than a square one: about 42 texels across the tail or nose and
  22-26 along a side.
- DECAL (128 × 64, I4, as the three SHEEN textures) is a panoramic sky: a sun with its reflection under a horizon.
  Its window's s is `func_8005CFB0`'s −(car heading) × 2048/π − (direction from the camera) × 0x800CF54C, t 0, so the
  sky stays put while the car turns under it; the SHEEN window is the one that scrolls with the car's motion. A layer
  node's window is only a scroll: `func_8007825C` loads the whole swapped texture and writes the window as the last
  `G_SETTILESIZE` (its end words stay 0), wrapping by the texture's size.
- The DECAL lists also set PRIM white (`FA000000 FFFFFFFF`); the SHEEN lists draw with it.

**2049 cars.** `add_sheen_layers` builds the layers from the converted body's first LOD (same scale as Rush 2's cars):
its triangles with white colors, the fitted UVs and Rush 2's layer state. The SHEEN layer is a copy of the body's
vertex loads under its commands. The DECAL layer takes the fit above (t over the body's height) and, the wrap being
continuous, is rebuilt as triangles over (body vertex, s) pairs: each triangle takes the shortest stretch of the wrap,
so the ones across the tail's seam get vertices of their own with s a turn (4096) up. (Until 2026-10-13 the angle
was folded so both sides matched and taken over a square body: two suns, mirrored about the center line on the tail
and nose, and 1.4 times too big there.) They are parts `D0_DFR1` / `D2_DFR1` and `D0_D2FR2` / `D2_D2FR2` (each its own list copy, shared vertices),
so Rush 2 textures and scrolls them like a Rush 2 car's front right panel layers. Denting moves their vertices with
the body vertex at the same position (Paint::mirrors). `sheen_model` finds the layer nodes from the car records
(validated against the part handles at 0x80218000 + id × 0x4E) and drops the 2049 ones unless All On, the Rush 2 ones
at All Off.

**Rush 2049's own SHEEN model** (not used): each car file's `CARnSHEEN` (render mode `00504E50`, combiner
`FC309661 552EFF7F`) under a node `func_800B15B4` creates with flag 0x80000, which `func_8009C8F0` draws with
`G_TEXTURE_GEN` and a LookAt from `func_8009C5E0` (guLookAtReflect) and a window scrolled by `func_8008C768` (atan2)
and `func_8009C3F8` (asin), tinted from 0x8011ADC0 + 8 × track.

### Car reflections

Graphics > Car Sheen = **Cube Map** (`Sheen::CubeMap`, `sheen_model`): every car has its layers, as under All On,
and in a race `sheen_model` wraps each SHEEN layer node (+0x50..+0x60 and the hood's +0xC / +0x10, `SheenSlot::sheen`) in
`gEXSetCarReflection(id + 1)` / `gEXSetCarReflection(0)` (a side list: the two commands around a `G_DL` to the node's
list, swapped in through $fp = r30 like the hidden layers' empty list) and drops the DECAL layer nodes (+0x3C..+0x4C),
a second fake reflection over the same panels. RT64 draws the marked triangles as the paint reflecting a cube map of
the scene around the nearest car: docs/graphics_effects.md#car-reflections. `id` is the car's preview id, which RT64
uses to tell the cars apart when it works out their bounds and normals.

The car select and the shop (`rush2::engine2049::car_preview_shown`, src/engine_preview.cpp) keep the textures: a
car shown on its own has no scene to reflect, and the reflection only darkened its panels.

`build_sheen_tables` trap: outside those two screens only ids 0-7 (the race's cars) are looked at, and a node with
no model (handle 0) never counts. The other ids' records are left over from the car select; their stale node indices
and part handles matched nodes of the race's scene, so the sky and the cars' shadows were taken for DECAL layers and
dropped whenever reflections were on (and would be under Car Sheen All Off).
