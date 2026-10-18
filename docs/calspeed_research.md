# California Speed (N64) research

California Speed's N64 version (Atari Games, 1999) runs on the Rush 2 N64 engine, so its tracks are Rush 2 files
and race in Rush 2's HAWAII slot like the SF Rush and Rush 2049 tracks. Code: `src/calspeed/` (ROM, Games tab,
conversion, track select art), the hosting in `src/rush2049/track2049.cpp` and the track select in
`src/rush2049/track2049_menu.cpp`. Tools: `tools/calspeed/cs.py` (ROM reader, asset list) and
`tools/calspeed/disasm.py` (disassembles its main code). [V] = checked against the ROM or in game, [I] = inferred.

## 1. ROM

- **Dump:** "California Speed (USA)", 16 MB, game code `NCL`. The No-Intro `.n64` is byte-swapped; the app stores a
  big-endian copy as `calspeed.z64` (SHA-1 checked, `src/calspeed/calspeed_rom.cpp`). [V]
- **Code:** boot segment raw at ROM 0x1000 / 0x80000400; main code and data raw (not compressed) at ROM 0x1CD60 /
  0x80053710 up to the first asset at ROM 0xB1C70 (functions end at 0x800D20B0). F3DEX2 display lists. [V]
- **Assets:** 128 u32 ROM offsets at 0x8008DFBC (ROM 0x5760C), every one compressed with Rush 2's LZ variant
  (`track2049::rush2_lz_decompress`, `roms.lz(rom, off, False)`). [V]

| Assets | Contents |
|---|---|
| 0-23 | menus: 3 = track name banners (144 x 23 each), 6 = track select landmarks, 8 = Rush 2's leftover dioramas |
| 26-38 | shared hit objects (cones, lamps, trees, windows, buildings, helicopter, stealth bomber), animated frames `I1..In` |
| 39-61 | cars `<NAME>FRAME1` (one 2-LOD body each) |
| 62-77 | track t geometry (Rush 2 model container) |
| 78-93 | track t placement (Rush 2 format) |
| 94-109 | track t collision (Rush 2 format, its own surface types: section 4) |
| 110-125 | track t AI / checkpoint path (Rush 2 format plus a second route line: section 5) |

Tracks (t, prefix, name from its table at 0x8008A588): 0 MONTEREY Monterey, 1 SILICON Silicon Valley, 2 HWY1
Highway 1, 3 SF San Francisco, 4 CENTRAL Central Valley, 5 SC Santa Cruz, 6 SD San Diego, 7 CLOVER Clover, 8 LA Los
Angeles, 9 SHASTA Mt. Shasta, 10 YOSEMITE Yosemite, 11 MOJAVE Mojave Desert, 12 FUJI Fuji, 13 OVAL Oval, 14 CALI
California ("Cal" in the table), 15 SA San Andreas. Tracks 7, 12, 13, 14 and 15 are loops; the rest run from a start to a finish
(`rush2::calspeed::point_to_point`). [V from the paths: checkpoint flags]

## 2. Per-track tables

All in the main data, Rush 2's layouts (`src/calspeed/calspeed_convert.cpp`):

| Address | Contents |
|---|---|
| 0x80094C2C | f32 per track: cloud scroll |
| 0x80094C6C | f32 per track: sky parallax |
| 0x80094CB4 | RGB per track: fog colour |
| 0x800C4C80 | u8 per track: visibility region count |
| 0x800C0680 | 16-byte region masks, the tracks' tables back to back |

## 3. Conversion for the host slot

- **Geometry:** the track's container merged with the shared object containers its placement uses, plus the empty
  models and textures Rush 2 looks up by name (`<prefix>FINISH`, `<prefix>FINISHB`, `CHKPNT`, `FINISH`), through
  `track2049::merge_rush2_containers`. A Rush 2 container's image data ends where its texture-load lists start, so a
  G_DL to the first load list points at the image chunk's end; the merge maps an address to the chunk that starts
  there (`merge_models` `new_addr`), or every model calling that list draws texels as commands (RT64 then stops on an
  unknown GBI). Behaviour ids are cleared (California Speed's hit-object kinds mean other things in Rush 2). [V]
- **Placement:** the tree takes the host's name. Hit objects (`TREEHIT*`, `PLUMHIT1I1`, `CON17HIT1I1`,
  `LAMPHIT*`, `LIGHTHIT1I1`) become Rush 2 breakable records drawn with their first frame through SF Rush's record
  redirects (`track1::set_redirects`); windows and buildings stay static; the helicopter and stealth bomber draw
  nothing (`CSNONE`). [V]
- **Cache:** `track_cache` entry 2000 + t plus the blob `calspeed_track<t>_<prefix>`, keyed on the ROM and the build. [V]
- **Track select:** ids 64-79 (`rush2::calspeed::first_menu_id`), after the custom tracks; the screen's tables are
  at 0x80310000 (names +0, scales +0x200, cloud heights +0x400, logos +0x600, carousel +0x800 (128 x 0x1C), circuit
  instances +0x1600 (80), custom track strings +0x1800, California Speed strings +0x1C00), us.toml repointing the
  instructions. BACKWARD isn't listed on its tracks and LAPS isn't on the point-to-point ones. [V]

- **Cars under the course:** Rush 2's tracks lie over an out-of-bounds floor (surface 7: a wheel on it wrecks the
  car, which puts it back on the road); California Speed's have none and their ground runs on below the roads
  (Monterey's to y -252). A drone glancing off Monterey's ramp at (0, -10, 5630) flew over the left wall and drove on
  200 ft under the course. `rush2_calspeed_car_frame` (calspeed_rom.cpp, hooked with the car frame callback at
  0x8005CFB0, `car_update_draw_nodes_8005CFB0`) wrecks a car (`car_wreck_trigger_8006E1A8`) that has stayed more than
  60 ft below the nearest racing-line point (the path's spine, its branches (the alternate routes, which on Monterey run up to 127 ft below the main road) and AI lanes, height weighted 0.5) for 2 s of race
  time, as surface 7 would. A road under another one is nearer its own points, so a car on it isn't counted. [V:
  Monterey, every drone finished]

- **Breakables:** Silicon Valley's 71 cones and 41+ trees need more than the 40 breakable groups Rush 2's table
  holds, and loading it crashed (docs/rush2049_research/placement.md, Breakable groups); `src/game_fixes.cpp` gives
  the table 0x82 groups. [V: sim]
- **Fall floor:** Rush 2 puts a car back below y -190 (`race_stuck_wrongway_update_8008CDA4`); Silicon Valley's roads
  go down to -306, and its cars were put back every 7.5 s onto road still under the floor. On California Speed's tracks
  the floor is 300 ft below the racing line's lowest point (`rush2_calspeed_fall_floor`). [V: sim]

### Track select landmarks

California Speed's track select shows each track's landmark (asset 6, `<NAME>NICHIMEN`) as a relief in a niche,
its surface facing -x. `lift_model` copies the model with its texture loads and turns it a quarter about z
((x, y, z) -> (y, -x, z)) so it stands on y like Rush 2's dioramas; it is unlit, so only positions turn. Scale =
17 / radius: Rush 2's flat dioramas sit at radius x scale about 34, and a landmark as deep as it is wide reaches
the camera at that size. The name logo is the 144 x 23 banner (asset 3) box-filtered to 128 x 32 CI8. Cached as
the blob `calspeed_menu`. [V in game]

## 4. Surface types

California Speed numbers its collision surface types (POLY flags & 0xF) its own way. Over its 16 tracks (counts
of polygons):

| Type | Polygons | Shape | Rush 2's meaning |
|---|---|---|---|
| 0 | 34833 | ground | ground |
| 1 | 6612 | ground, material 1 | ground (off-road) |
| 2 | 1611 | ground, material 2 | ground |
| 3 | 5745 | flat ground beside the road, material 3 | wrecks a wheel touching it ("water") |
| 4 | 258 | vertical, every one | sinks the car |
| 5, 6 | 17274, 5390 | walls | walls |
| 7 | 1416 | vertical, every one | out-of-bounds floor: wrecks a wheel touching it |
| 8 | 213 | vertical, every one | runtime "airborne" marker only |
| 9 | 314 | vertical, every one | none |

California Speed's wall query at 0x800AECC4 takes types 5-9 together. In Rush 2 the type drives the physics (0
road, 1 dirt, 2 grass: off-road drag and torque), only 5 and 6 are walls to the body probe, so its cars drove
through the 4, 7, 8 and 9 walls at the road's edge (Monterey's right-hand verge near (4000, -40, -450) is type 8,
385 ft tall) and wrecked on the type 3 verges. Wheel materials (bits 4-7) drive only the dust, surface sounds,
particles and stunt scoring, which know materials 0-2 and 8 (airborne); California Speed's ground also uses 3-9
(8 on road). The converter (`remap_surfaces`) makes the type 3 verges grass (type and material 2), the 4, 7, 8 and
9 walls type 5, and ground materials past 2 road (0). [V data; the drones' wrecks there, in game]

## 5. Paths

- **Format:** Rush 2's path file (`tools/rush2049/paths.py`), one per track (no backward races). After the four AI
  lanes come two more lists of `{u16 count, 3 x u16 0}` + count x {s16 x, y, z}, unused by Rush 2: two more route
  lines (701 points on Monterey; 620 and 635 on San Francisco, one on each carriageway of its bridge, up on the deck
  at y 180, x 2426 and 2518 at z -2400). Perhaps California Speed's reset line. [V layout, I use]
- **Route line (Rush 2's spine):** its points are about 40 ft apart (median 39-44 ft; 73-82 ft on Monterey, Santa Cruz,
  Fuji and California), where Rush 2's are 20 ft (19.7-20.0 on every race track). California Speed's loader
  (`func_800A2A44`) keeps the route header at 0x80142700 (+4 the points 0x80142704, +8 u8 branches, +0xC the branch
  records 0x8014270C: 16 bytes, +6 the main-line point a branch joins, +0xA its count, +0xC its points); Rush 2 keeps
  the same at 0x80111940 (`path_branch_records` 0x8011194C). [V]
- **Resets:** California Speed's tracker `func_8009BF34` keeps each car's nearest route point (per-car state +0x10E,
  branch +0x10C (-1 the main line), the previous at +0x110 / +0x10D; the nearest in 3D of the 10 points from the last
  one on). A reset (`func_800A4020`, a1 = 0) puts the car back on that point, with no move along the route; a1 = 1 (a
  car outside the collision grid, `func_800AE668`) uses the last checkpoint's route crossing instead. Nothing else
  resets a car: California Speed has no wrecks, and its only other caller is the ground contact `func_800A58A4`, when
  no wheel is down and the four wheels' heights sum under -3000 (a car fallen 750 ft under the world).
  (`func_8009F2E4` would reset with a0 != 0, but its one caller `func_8009F194` is only called with a0 = 0, the
  grid, from `func_80093678`.) `func_800A56C0` places it at the point + the slot's offset turned
  along the route (`func_800A4C44`: x = (dz, -dx), z = toward the next point, a branch's last point toward its
  main-line join; 0x800C8E00 turns the rows by (-dx, dz)), at the route point's height. The slot table 0x800D8630
  (12 bytes each) holds the grid (slots 0-7: x 0 / 12, z 60 / 40 / 20 / 0) and the resets (slots 8-15, = 8 + the
  car: x -2.4, 2.4, -16.8, -12, -7.2, 7.2, 12, 16.8), and every slot gets the track's shift 0x800D86F0 (16 floats:
  9, 6, -6, -3.6, 10.8, 16.2, 7.2, -6, -15.6, 6.96, -3.6, 7.2, -10.8, 3.6, 0, 0) on x. Rush 2's respawn
  (`respawn_place_car_80090A40`, docs/rush2049_research/checkpoints.md section 8) instead moves a car on by its speed x
  (seconds wrecked + 1.5) in 20 ft points (590-800 ft; 2 to 4 times that on California Speed's points), players along
  the route line and computer cars along their lanes. Ported as California Speed has it, for every car
  (src/calspeed/calspeed_rom.cpp): `track_route` is `func_8009BF34` run for each race car every frame from
  `rush2_calspeed_car_frame` (its candidates, forks, carried list and reads past a branch's end alike; it starts on
  the main-line point nearest the car, where California Speed starts at checkpoint 0's crossing + 1 ahead of its own
  grid); `rush2_calspeed_respawn_advance` (0x80091368) moves the respawn point on by nothing; and
  `rush2_calspeed_respawn_place` (0x800918E8) puts the car on its tracked point + the slot offset for the car and
  track along (dz, -dx), at the point's height (California Speed's route points sit 0.1-0.4 ft over the road, median,
  and it adds no lift where Rush 2 adds 1.5 ft), faced along the route. Not ported: the a1 = 1 reset (outside the
  collision grid; Rush 2 has no such test) and `func_800A4C44`'s road-polygon matrix (`func_800AF57C`), which tilts the
  side offset with a banked road; the no-road matrix is used. A car stuck at one spot is put back at that spot. [V code;
  the shift's sign from the grid: only z toward the next point keeps all 8 slots on every track's road]
- **Route heights:** the route line isn't always on the road: San Francisco's points 324-433 (the bridge, 3,700 ft)
  lie at sea level (y 2-6), 180 ft under the deck they follow in x and z, in the middle of its two carriageways (the
  deck's collision is one polygon over both and the middle; the lanes climb the cables to 400 ft at the towers).
  California Speed's reset puts a car on the point's height, and its wheel query (`func_800AF1EC` -> `func_800AE884`:
  the wheel's height over each polygon's plane along its normal, the polygon's matrix at +4 in s16 / 16384, taken
  from 5 under to 250 over, the lowest wins) only finds road under a wheel, and the collision has nothing under the
  bridge, so a reset there would drop the car and reset it again when it is 750 ft down. The real game never gets
  there: the bridge's sides are type 6 walls 13-658 ft high (337 polygons) that keep a car on the deck, and it has no
  wrecks, so neither reset cause happens on the bridge. Rush 2's wreck respawns do, and the exact port dropped the car
  there. So where California Speed's wheel test finds no road under the reset spot (`rush2::calspeed::ground_under`,
  its `func_800AE884` over the converted collision's ground polygons, types 0-2 as remapped, -5 to 250 ft), the car
  goes on the AI lane point nearest where it wrecked, 1.5 ft up as Rush 2 puts a computer car, faced along that lane
  (`func_8006D348` with $a3 = 0); everywhere else the reset is California Speed's. Route points with no road within 15 ft under or 5 over them: San Francisco 111, LA 95, Santa Cruz 61, San
  Diego 43, California 34, San Andreas 14, Silicon Valley 4, Monterey 2, Central Valley 1, the rest 0 (jump gaps
  included). [V data and code]
- **Lane speeds** are displayed mph, true mph x 1.2 like Rush 2049's: lane 0 averages 162-172 mph over the race
  tracks, distance-weighted, where Rush 2's paths average 139.3. They go through the Car Speeds lane map as 2049
  paths (`rush2_race_lane_speeds`), and the record seed (`record_seed`, lane 0's time) uses true mph. [V]
- **Start grid:** Rush 2 lines the grid up behind the spine point where checkpoint 0 crosses the spine, 60 ft deep
  (slot table 0x800CC2BC, `car_place_by_case_8006DE20` case 0). A point-to-point track's road ends a few feet behind
  its first spine point, and on six tracks checkpoint 0 crosses within 50 ft of it, so the back rows (the player's)
  started off the road and fell. `move_start` moves checkpoint 0 to the first spine stretch at least 75 ft along;
  every grid slot then has ground under it on every track (checked offline: each slot against the track's
  collision). The stretch must also leave every slot 5 ft from the walls (types 5 and 6 after the remap, within 15 ft
  of the road's height): the slots reach 8.5 ft to one side of the spine and 17 to the other, and on Highway 1, where
  the spine runs near the right wall, two cars stood 2 ft from it, tipped onto it before the start and fell through
  the course. California Speed's own grid isn't traced. [V]
- **Laps:** a point-to-point track is raced over one lap; every drone finishes Monterey's lap (about 200-225 s). [V]
- **Lane gaps:** Rush 2's drone driver puts a drone back on the road once it is more than 100 ft from its lane point
  along the lane or beside it: `drone_steer_along_path_80074284` keeps the car's offset from the point in the lane's
  direction (state +0) and across it (state +4, both x/z); `drone_driver_80074990` sets car +0x810 at 0x80075238 when
  either is past 100 (or the drone has been stuck: state +0x24 >= 11 for over 1 s), and
  `race_stuck_wrongway_update_8008CDA4` (0x8008CF00, 0x8008D0B8) places it by case 2 and sets its wreck state +0x342 to
  2. California Speed's lanes have no points in the air: Monterey's jump is one 120-128 ft step on every lane, so every
  drone that flew it was wrecked over the landing edge (-118..-75, -11, 5729..5755), at any height, with the road hold
  on or off. Central Valley, San Diego and San Andreas also have 100-131 ft steps. The converter
  (`split_lane_steps`, calspeed_convert.cpp) splits steps over 90 ft into even pieces of at most 45 ft; steps over
  1000 ft (Yosemite's lane 1 jumping back 5581 ft at its end) aren't road and stay. The lanes' checkpoint
  crossings are worked out at load (`func_80092BC4`). [V: Monterey's jump wrecks gone in the sim]

## 6. Music

`src/rush1/track1_audio.cpp`, as SF Rush's. California Speed's audio setup (0x80056AD4) loads the music bank
(.ctl) at ROM 0xB82BC0, its samples (.tbl) at 0xB8A390, a song file at 0xD69FC0 (`S1`, nine songs, each
LZ-compressed MIDI as SF Rush's) and the sound effect bank at 0xD73AE0. The block 0xB82BC0-0xD73AE0 is reserved
past SF Rush's in the ROM image and filled from the ROM, the .ctl copy lives at 0x80E16000 (below 16 MB: the audio
microcode reads its ADPCM codebooks with 24-bit addresses), and the songs are Rush 2 songs 29-37. California Speed
has no per-track songs: its options' Audio Track picks one (strings at 0x800D4658), so a California Speed track
plays a random one of its songs by default (`src/music.cpp`). Its menu song isn't identified. [V layout]

### Engine sounds

California Speed's engine code is Rush 2's (src/car_engines.cpp plays it; Rush 2's side is in src/engine_preview.cpp
and docs/functions/rush2_audio.md), with its own tables, constants and samples. [V code; not yet heard in game]

| California Speed | Rush 2 | What it does |
|---|---|---|
| 0x80056594 | 0x8008C61C | Starts engine a0 on slot a1: 0x801427C0[slot] = engine + 9 x slot + 1 (Rush 2: + 10 x slot), command 0x1FFE + slot |
| 0x800563DC | 0x8008C49C | Stops every slot's engine sounds, clears 0x801427C0 and 0x80149D44 |
| 0x800565FC | 0x800626FC | Command 0x1FFE / 0x1FFF: starts the slot's layers, sets 0x80149D44[slot] = 1 |
| 0x80056724 | 0x80062CC4 | Every audio frame: each slot's layers' volume, pitch and reverb mix (0x80134384[slot]) |
| 0x8005629C | 0x80062598 | Layer pitch: min(rpm / base, 2), 1 for sound 0x23 |
| 0x8005632C | 0x80062640 | Table indices: rpm / 1000 (0-10), (load + 80) x 0.025 (0-12) |
| 0x800564B0 | 0x800624E4 | Layer volume (below) |
| 0x800ADE48 | 0x8008C6E8 | Race start: the slot's rpm and load, then 0x80056594 with 0x80141568[slot] |
| 0x800ADEFC | 0x80064F70 | The slot's rpm (0x80168F68) and load (0x80134458) each frame |
| 0x8007EC08 | 0x8009E66C | Player engine start: rpm scale 0.8 (0x800F7798; Rush 2 1.0), load scale 0.3 (0x8016E150; Rush 2 0.3) |
| 0x8007EC70 | 0x800650DC | rpm = \|car +0x846\| x 0.8, load = car +0x458 x 0.3 (both as u16 & 0x7FFF); +0x846 is Rush 2's +0x7F0 (rad/s x 9.549 x 0.9, written at 0x80090EDC), +0x458 Rush 2's +0x3F4 |

- **Engine per car:** s8 table 0x8007D284 (car select order, read at 0x8009EDE0 into 0x80141568[slot]): SPORTSTER 5,
  MUSCLE 0, 486SE 2, PREDATOR 0, MANO 1, FIVE OH 4, MT DEW 6, HOPPER 1, FAIRCHILD 7, INSECT 3, MERCADO 7, GOLF CART 4,
  SQUIRREL 7, FORKLIFT 0, CONV 0, SLED 2, BAJA 6, OL TRUCK 3, CAMPER 7, SEMI 0, DOZER 1. Engine 8 is unused. There is
  no engine choice, so the port's car select hides ENGINE for these cars.
- **Layers:** sound 0x80058220 [id x 2 + layer] (-1 none, | 0x200 on slot 1, the same sample), pitch base
  0x80058198 [(id - 1) x 2 + layer]. Engines 0-8 on slot 0: 26, 2D, 2C, 30, 2B + 23, 2D, 2E + 2F, 29 + 2A, 27 + 28;
  bases (layer 0 / 1) 4000/3000, 5000/3000, 6000/3000, 4000/3000, 4000/4000, 7000/5000, 5000/5000, 5000/5000,
  5000/5000. Slot 1 (player 2) differs: engine 4 is 2B + 2B (no 23), engine 5 is 24 + 25, and engines 5, 6 and 8
  have bases 4000/4000. Sound 0x23 gets volume 0 and pitch 1 every frame (0x80056888), so it is silent and not played.
- **Volume:** (int)((int)(A[layer][rpm index] x B[layer][load index] x 1635 + 3270 x 6) x engine volume / 40), with
  1635 at 0x800AE3DC and 6 at 0x800AE3E0 (no code writes them; Rush 2: x 2000 + 24000), A = 0x80058354 (2 x 11),
  B = 0x800582B8 (2 x 13), the engine volume option at 0x80150B31 (0-40).
- **Samples:** the sound effect bank, ROM .ctl 0xD73AE0-0xD78C40 and .tbl 0xD78C40 (loaded at 0x80056E88; one
  instrument, 95 sounds, 44100 Hz). The engine sounds 0x24-0x30 are looped VADPCM (order 2, 4 predictors), volume 127, sustain
  127, no detune. The output rate is 22050 Hz, as Rush 2's.

The port reads these from the ROM once (`cs::make`) and plays every California Speed car's engine on the host:
local players 1 and 2's at a player's own volume (Rush 2's slot engine start is skipped for them,
`rush2_engine49_start`, as for 2049 cars; player 2 uses the slot 1 rows), other cars by position as
src/car_engines.cpp plays Rush 2's. Without the bank the cars fall back to their stand-in Rush 2 ENGINE values.

## 7. Cars

California Speed's 21 cars are car types 36-56 (`src/calspeed/calspeed_cars.cpp`; the type layout in
`include/car_types.h`, the shared car code in `src/rush2049/car2049.cpp`). Its car code is Rush 2's: the car struct is
Rush 2's with the fields from +0x5B0 on moved 0x64 later (Rush 2's mass +0x5B0 is +0x614), and the car init
`func_8009A300` is Rush 2's `car_init_setup_8008DBA0`. [V]

- **Names** (0x8008A6D4, 20 bytes each): SPORTSTER, MUSCLE, 486SE, PREDATOR, MANO, FIVE OH, MT DEW, HOPPER,
  FAIRCHILD, INSECT, MERCADO, GOLF CART, SQUIRREL, FORKLIFT, CONV, SLED, BAJA, OL TRUCK, CAMPER, SEMI, DOZER. The car
  select logos are drawn in Rush 2's style from these (`set_calspeed_logos`, texture names RCSCARn). [V]
- **Models:** asset 41 + n is a Rush 2 container holding only `<prefix>FRAME1` (two LODs) and its textures; prefixes
  Z3, CAMERO, FERRARI, DIABLO, MANO, COPCAR, M2, PROWL, SUPER, BUG, CIVIC, GOLF, DRONE, FORK, CADDY, SLED, BAJA, OLTRUCK,
  WIN, TRUCK, DOZER. `track2049::convert_body_car` makes it a Rush 2 car as a 2049 car is: the body as CSnFRAME1,
  Rush 2's other 36 parts empty, the sheen layers from the body; its textures renamed `CSn_<name>` (several cars share
  WINDOW and UNDRSDE2). Served as asset 0x80 + n (`car2049::car_asset`), dented by panel damage like a 2049 body, its
  damaged panels drawn with scuffed copies along California Speed's ramps (`paint_ramp`). The driver and underside
  textures load their own TLUTs in their load lists, so only the car palette is the game's. Cached as the blobs
  `calspeed_car_<n>`, keyed on the ROM and the build. [V]
- **Paint** (`func_800BAE2C(palette, c1, c2)`, RGBA5551 colors; its blend `func_800BAD6C` is 2049's `blend5551`, from a
  at t = 0 to b at 255): every car uses CARPALETTE (asset 25: a blue ramp at 0-127 plus fixed entries). Entries 0-62 =
  blend(c1, black, trunc(255 - 3.984375 i)), 63 = c1, 64-127 = blend(c1, white, trunc(3.984375 (i - 64))); 188-221 =
  blend(c2, black, trunc(255 - 7.5 (i - 188))), 222 = c2, 223-255 = blend(c2, white, trunc(7.72 (i - 223))).
  `rush2_car49_paint` (end of `car_build_textures_8008582C`) paints a California Speed car so with MAIN as c1 and
  ACCENT as c2; the car select hides STRIPE and STRIPE COLOR on them (`wings_menu.cpp` `Rows::TwoColors`). [V]
- **Default colors:** a new player record's colors (record + car x 8 + 0x536 / 0x538, rims + 0x53A) come from
  `func_8005E5C0`: c1 the RGB word at 0x800DC0D0 + 4n, c2 at 0x800DC124 + 4n, rims row 0 of 0x8007D3BC. The drones'
  (`func_800A6F88`) are picked from a run-time table at 0x8014A908. Here they are the nearest of Rush 2's 32 colors
  (`calspeed::car_defaults`), in the default rows and new side slot blocks. [V]
- **Per-car tables** (21 entries each, Rush 2's layout and meaning): steer base 0x8007CF20 (10000 for all), yaw
  damping 0x8007CF74 (1.0), name pointers 0x8007CFC8, type -> descriptor 0x8007D01C (u8), preload 0x8007D034 (0.05),
  mass 0x8007D088 (x 0x8007D47C = 1.0; 111.8 / 139.75 / 167.7 / 174.69 by class), pitch/roll inertia 0x8007D0DC,
  crash threshold 0x8007D134 (390000 for all; no code reads it), weight 0x8007D188 (0.5; `func_8009A8E0` takes its
  delta as itself minus itself, so it never changes the mass), torque map 0x8007D2AC (2 = HIGH for all), drive flags
  0x8007D2C4 (3), handling 0x8007D304 (4 for all), steer and yaw adds 0x8007D31C / 0x8007D348 (Rush 2's 0x800C0DF0 /
  0x800C0E1C, the same values), rims 0x8007D3BC. The handling value indexes both adds (`func_8009A838`,
  `func_8009A88C`), as Rush 2's TIRES does: TIRES = 4 (S1 D1). Rush 2's SUSPENSION has no counterpart (NORMAL), and
  ENGINE (the engine sound) is the most similar Rush 2 car's, heard only if California Speed's own engines (6. Engine sounds) can't be read. 0x8007D3FC / 0x8007D43C are per-player carousel ids
  (player x 42 + i, + 21), not colors. [V]
- **Descriptors** (0x800D6418: 18 pointers to 0xE0-byte descriptors) are Rush 2's 0xEC-byte layout less a few fields
  (`build_descriptor`): 0x00-0x53 the same; Rush 2's +0x54/+0x58/+0x5C missing (California Speed reads
  only +0x50, 16 in both games), taken from the Pickup; 0x54/0x58 = Rush 2 +0x60/+0x64 (drag, rolling
  resistance); 0x5C-0x68 = +0x68-+0x74 (tire curve pointers: 0x800D50F0 front, 0x800D5108 rear, much grippier than
  Rush 2's); 0x6C and 0x70 two grip factors (`func_800B8CE8` scales a tire's two force components by them; 1.0 on most
  cars, the Dozer 1.5 / 3.0), with no counterpart in Rush 2, whose +0x78 (rear lateral grip) California Speed lacks:
  +0x70 is folded into the car's own copy of the curves' grip (`calspeed_tire_curves`) and +0x78 is 1.0 [I which
  factor is lateral]; 0x74-0xA0 = +0x7C-+0xA8 (wheel positions); 0xA4-0xAC = +0xAC-+0xB4; 0xB0-0xBC = +0xB8-+0xC4
  (torque scales, the same 1.9 / 1.95 / 2.05 / 1.3 on every car); 0xC0-0xD0 = +0xCC-+0xDC (neutral, gears 1-4:
  3.05 / 1.68 / 1.17 / 0.88 on every car; `func_8008E748` reads desc + 0xC0 + gear x 4); no reverse (the Pickup's);
  0xD4 = +0xE0 (top gear, s16); 0xD8/0xDC = +0xE4/+0xE8 (shift thresholds). The cars differ only in mass, inertia,
  wheel layout, grip factors and box (0x800D60E0, 16 bytes each). [V layout]
- **Options** (Games tab): California Speed Cars (offered in the car select, after the other cars) and California
  Speed Computer Cars (All Tracks, California Speed Tracks, Off: `rush2_car49_drone`). They have no unlock shop items.
  Online, a California Speed car needs every machine to have the ROM (`online_net.cpp`). [V]
- **California Speed Road Hold** (Games tab, `calspeed_road_hold`, default On, takes effect at once): off, its tracks
  have Rush 2's own gravity only (see Road hold). `tools/calspeed/sim_wrecks.py` races the tracks headless with it on
  and off and counts the computer cars' wrecks and rollovers.

### Road hold

California Speed keeps its cars on the road with a force Rush 2 lacks, and its cars' values (grip 7.8, springs
-6800 to -18360, steer force 10000, yaw damping 1.0) are tuned against it: copied into Rush 2's physics they flip at
small steering inputs, and its tracks' hills throw every car several times higher than California Speed does. Gravity
is the same in both (32.2 ft/s/s x the 0x800D7380 table, Rush 2's 0x800CB580). [V]

- **Ground contact** `func_800A58A4` (Rush 2 `car_ground_contact_800703EC`; car fields +0x64 past Rush 2's here:
  compression +0x620 = Rush 2 +0x5BC) clamps each wheel's compression to +-3 and its rate to +-40 as Rush 2 does, then
  sets a per-wheel road-hold factor +0x314: in the air (compression < 0) 0.9 below -0.75, 1.5 below -0.5, else 0.5; on
  the ground 0.32 (double at 0x800D8740), or past 0.5 compression 0.82 - compression, first raising wheel (i + 2) % 4's
  factor by 0.2 when it is below 0.2 (only wheels 0 and 1 keep that: the later wheels replace theirs). A wheel that
  found no road while fewer than 3 did is put 1000 below it (clamped to -3). It also normalizes +0x3D8, respawns
  (`func_800A4020`) with no wheel down and the summed +0x258 below -3000, and ends in `func_80080FCC` (Rush 2 calls
  `car_collide_with_cars_8006F3C0` there). [V code, I field roles]
- **Wheel forces** `func_8008F23C` (Rush 2 `func_8006AFD8` + `car_force_sum_8006A02C`) adds, per wheel, the vector
  (0, -(weight +0x610) x factor, 0) to the car's force sum (+0x4, the car's frame): as it is (along the car's own down
  axis), or rotated from the world by the car matrix +0x324 (`func_800C9008`, as gravity is) when any wheel is more
  than 1.0 out of the road. It is a push on the whole car, not at the wheels (no torque). The factor is x4 once the
  byte at 0x8014FE0B is set (when the race is over, `func_80080850`; it also takes over the throttle,
  `func_80082894`) or the player's byte at 0x80151C02 + index x 0x2A8. So a car on the ground carries 4 x 0.32 = 1.28
  times its weight extra, and one well off the ground 3.6 extra (4.6 g in all), which is why its jumps are low. [V]
- **The port** (`rush2_calspeed_road_hold`, hooked in `car_physics_forces_800706D0` after `car_force_sum_8006A02C`,
  for every car on a California Speed track, not in the attract demo (0x800FAE6C, which starts without the track
  select that clears the California Speed track and could land on HAWAII, the host slot); `hold` picks the version,
  `Hold::Heavy`, `heavy_push`). California Speed's world as a heavier one, with the car built to match so it drives
  as on any other track, and a hold at the road. California Speed is built this way: its road hold is 1.28 g more on
  the road (2.28 g in all) and up to 3.6 more with the wheels well up (4.6 g), and its cars' springs per unit of mass
  are 2.27 times Rush 2's (-10620 against -3600 at 1.3 times the mass), their dampers 1.56 times (sqrt 2.28 = 1.51),
  so they sit at Rush 2's height under it. With `gravity_scale` = 1 + 4 x 0.32 = 2.28:
  - gravity x the road hold's factors (`heavy_factor`: a constant 0.32 per wheel on the road, off it California
    Speed's 0.5, 1.5 from 0.5 to 0.75 ft out, 0.9 further), the car's gravity +0x200 (the world gravity +0x2C0, a
    force of the car's weight, turned into the car's frame by `car_force_sum_8006A02C`) added to the summed force at
    +0x4; California Speed's easing of the ground factor past 0.5 ft of compression (its landing softener) is not
    kept: under a fixed spring scale it dropped the car's gravity below what the springs were set for in a dip, on a
    loaded corner or at a ramp foot, lifting the car and costing the tires up to a tenth of their grip (Gravity's
    slope trouble);
  - springs and anti-roll x 2.28, dampers x sqrt(2.28) (`rush2_calspeed_wheel_damper` rebuilds the load,
    `car_wheel_force_loop_8006ABB0` 0x8006AC8C-0x8006AD0C, for 0 < compression <= 10): the car sits at Rush 2's
    height and the suspension's damping ratio is Rush 2's;
  - the tires' load / 2.28 (`rush2_calspeed_tire_load`, hooked at 0x8006AD4C as $a2 goes to `car_tire_model_8006A2FC`:
    min(0x34($sp) / 2.28, weight +0x5AC), the game's cap): grip is Rush 2's;
  - direction: while every wheel is within 1.0 ft of the road, only the part of the extra square to the road (the
    mean normal of the polygons under the touching wheels, or under all four when none touch, turned into the car's
    frame by the car matrix +0x2CC as `push_car`), so hills, banks and grades pull as on any other track and the
    springs see exactly 2.28 x their Rush 2 load; once any wheel is more than 1.0 ft out (a jump, or over nothing:
    compression clamps at -3) the whole extra pulls straight down, California Speed's rule;
  - stick: no amount of gravity keeps a car on the road over a kink (California Speed's roads are large flat
    polygons, a hill is a run of them): the car's velocity has to turn at once while gravity bends it over time, and
    at 2.28 g a 5 degree kink at 180 ft/s still lifts the car 1.7 ft for 0.4 s. So while a wheel has lifted
    (compression below 0) and none is more than 1.0 ft out, the car's velocity away from the road (+0x34 along the
    normal above) is taken out that step: a force of mass +0x5B0 x that velocity / the step +0x634, at most
    `stick_cap` = 6 x the weight (California Speed's 1.5 band is 6 g over its ground pull). A ramp lip gives far more
    separation than the cap takes back within the foot, so jumps still fly, a little shorter; a wheel lifted in a
    corner has next to no velocity away from the road, so nothing is added there; with four wheels down it never
    runs.
  Mass is unchanged: a lighter car under more gravity keeps the same weight on the springs but the same tire, engine
  and brake forces move it that much harder; steady-state weight transfer, grip, engine and brakes are Rush 2's, the
  suspension runs 1.5 times faster with a 2.28th of the body roll, and a car has to lift against 2.28 g to roll over
  or leave a crest: California Speed's own trade, and what keeps its narrow cars down on its banks. Springs x 4.6
  (the air's gravity: twice as stiff as California Speed's) kicked cars over at ramp feet: Monterey's jump, a 17
  degree crease at 185 ft/s, threw 6 of 7 computer cars into backflips (stock 3 of 5, x 2.28 2 of 5).
  `RUSH2_ROAD_HOLD_LOG` prints the player's car's compressions, factor sum and stick (x weight) every step a wheel
  is off the road.
- **Net** (`Hold::Net`, `net_push`, tried 2026-10-17; kept in the file). California Speed's push less what its
  springs hold up: a wheel on the road adds nothing, one off it California Speed's whole factor x weight / 4 (ramped
  over the first 0.25 ft, `net_ramp`), so with four wheels down the car is exactly Rush 2's. Cars stopped flying far
  (over a crest needing 1.5 g they hovered 0.05 ft up), but they still rolled over on the road: Santa Cruz's
  computer cars wrecked 91 times (137 rollovers) against stock's 103 (225) and Gravity's 11 (31); San Francisco 11
  against 5. The 1.28 g on the road is what keeps California Speed's narrow cars down, not the pull once they have
  left it. Earlier `Hold::AirPull` had taken the 0.32 off the air factors as well (0.18 for the first half foot), so
  it lifted and was then yanked: once a wheel is off the road its spring carries nothing, so the whole 0.5 is the
  net.
- **Gravity** (`Hold::Gravity`, ran until 2026-10-17; kept in the file). Heavy's heavier world as first built, with
  the car built to match so it drives as on any other track. California Speed is built this way: its road hold is 1.28 g more on the road (2.28 g in all) and up to
  3.6 more with the wheels well up (4.6 g), and its cars' springs per unit of mass are 2.27 times Rush 2's (-10620
  against -3600 at 1.3 times the mass) and their dampers 1.56 times (sqrt 2.28 = 1.51), so they sit at Rush 2's height
  under it. With `gravity_scale` = 1 + 4 x 0.32 = 2.28:
  - gravity: the road hold's factors (`road_hold_push` / weight: 1.28 on the road, up to 3.6 in the air) x the car's
    gravity (+0x200, the world gravity +0x2C0 turned into the car's frame by `car_force_sum_8006A02C`; Rush 2 sets
    +0x2C0 once at car init) is added to the summed force at +0x4 (`gravity_push`), leaving +0x2C0 to the wings'
    gravity scaling;
  - springs and anti-roll x 2.28, dampers x sqrt(2.28) (`rush2_calspeed_wheel_damper` rebuilds the load,
    `car_wheel_force_loop_8006ABB0` 0x8006AC8C-0x8006AD0C, for 0 < compression <= 10): the car sits at Rush 2's
    height and the suspension's damping ratio is Rush 2's;
  - the tires' load / 2.28 (`rush2_calspeed_tire_load`, hooked at 0x8006AD4C as $a2 goes to `car_tire_model_8006A2FC`:
    min(0x34($sp) / 2.28, weight +0x5AC), the game's cap): grip is Rush 2's;
  - hills: on the road the extra gravity's part along the road (the touching wheels' mean polygon normal, as
    `push_car`) is taken out, a quarter per touching wheel, so it fades as the car leaves the road and the whole
    extra pulls it down in the air. `extra_downhill` / `extra_uphill` keep a fraction of it where it pulls along /
    against the car's travel (+0x34, the car's velocity in its frame): 0 = Rush 2's hills, 1 = the whole heavier
    world; both 0. Its part across the road (banking) is always taken out.
  Mass is unchanged: a lighter car under more gravity keeps the same weight on the springs but the same tire, engine
  and brake forces move it that much harder. Springs x 4.6 (the air's gravity: twice as stiff as California Speed's)
  kicked cars over at ramp feet: Monterey's jump, a 17 degree crease at 185 ft/s, threw 6 of 7 computer cars into
  backflips (stock 3 of 5, x 2.28 2 of 5). Set aside for Net: the ground factor isn't constant (0.82 - compression
  past 0.5 ft, California Speed's landing softener), while the spring scale is, so in a dip, on a loaded corner or
  at a ramp foot the car's gravity fell below what its stiffened springs were set for: it was lifted and the tires
  (load / 2.28) lost up to a tenth of their grip; the along-road removal was weighted by the touching wheels, so a
  wheel lifting on a bank stepped a quarter of the extra's sideways part back in; and the suspension ran
  sqrt(2.28) = 1.5 times faster with a 2.28th of the roll everywhere, the price of the heavier world.
- **Damper** (`rush2_calspeed_wheel_damper`, hooked at 0x8006AD10, the same toggle; under Net only the jump limit
  below runs, the spring and damper scaling is Gravity's). The load is (anti-roll +
  compression x spring - v x damper) x a time-step scale, v being the wheel's velocity along the normal of the polygon
  under it. On large flat polygons v jumps at once by speed x the change of slope as a wheel crosses an edge (3
  degrees at 190 ft/s: 10 ft/s), and the damper throws the car or drops the wheel for that frame: the slight lifts
  over polygon edges that let cars spin out. Monterey's 1.6 ft curb at (-2870, -55, 8765), which its lanes cross at
  170 mph, has a 29 degree face 2.9 ft deep: a rear wheel that landed on it for a frame read v = -85 ft/s while its
  spring was extending, took 12.5 x the car's weight and flipped the car over backward; most computer cars flipped
  there. v may move only 200 ft/s per second (6.7 ft/s in a 1/30 s frame) past the larger of last frame's v and the
  spring's own rate in one frame; on smooth road it changes far less, so nothing changes, and a wheel that was off the
  road (a landing) keeps v. Tried: the spring's rate as the damper's input (a frame behind: the stiffer dampers, type
  39's, oscillated and the car hopped along at walking pace) and holding v to that slew over several frames (every
  car bouncier: 28 rollovers on Monterey against 13).
- **Tried and set aside** (kept in the file): `Hold::Copy` (California Speed's road hold as it is: weight x the
  per-wheel factors, square to the road or straight down once a wheel is more than 1.0 out, with the springs holding
  its ground part up: `rush2_calspeed_wheel_support`, hooked at 0x8006ADFC, records each wheel's support at
  0x34($sp) and the car is lifted by push x their sum / weight at its center, or each support is multiplied by
  (weight + push) / weight while a wheel is off the road. Per wheel all the time it made the springs 2.3 times as
  stiff in roll and pitch, and hooked at 0x8006AD18 it fed the tire load's cap, so a loaded wheel's tire jumped to the
  full weight and cars let go swerving downhill; lifted only at the center, cars landing on two wheels stayed tipped:
  California 90 wrecks and 140 rollovers against 62 and 47 per wheel). `Hold::Shape` (a push from the road's shape: weight x v^2 k / g from the
  pitch between the road under the front and rear wheels, averaged over 0.12 s; cars still lifted a little at
  polygon edges and spun), a push from the tires' unloading (the loads jump every step with the damper, so it
  snapped between 0 and 2 x weight on ordinary bumps), the whole road hold with the tires' load scaled back
  (`Hold::Full`: cars still handled differently and parked cars rolled 6-26 ft in the race intro), its air part alone
  over its ground part (`Hold::AirPull`) and air gravity (`Hold::AirGravity`, 3.6 x weight as the lowest wheel goes
  from 0.25 to 1.25 ft up): both start only once the car has begun to fly, so it lifted and was then yanked down.
- California Speed also stops cars from turning around on the track (they can only face along it); not located yet,
  and not ported.

### Rebalance

The California Speed cars' values are mapped onto Rush 2's roster as the 2049 cars' are (`Rebalance` in
src/calspeed/calspeed_cars.cpp; `tools/rush2049/cars.py calspeed` prints the three rosters side by side): each value
x Rush 2's mean (its 21 cars, all but the secret ROCKET) / California Speed's mean. Values every California Speed car
shares become Rush 2's mean: steering 10000 -> 543, yaw damping 1 -> 30.7, preload 0.05 -> 0.185, torque scales
1.9 / 1.95 / 2.05 -> 1.82 / 1.84 / 1.89, gears 3.05 / 1.68 / 1.17 / 0.88 -> Rush 2's, the tire curves (cornering
stiffness 64000 / 78000, grip 7.8) -> Rush 2's (16000 / 32000, 2.6 / 3.2). Values that differ by car keep their order
with the mean car on Rush 2's: springs and dampers (descriptor +0x18-+0x4C; mean -10620 / -8511 -> -3600 / -4200), mass
(140.4 -> 108.4 slugs), inertia (2614 -> 2729). The grip factors (+0x70, the Dozer 3.0) stay on top of Rush 2's
curves, and the handling row (California Speed's 4 for every car) is Rush 2's mean, 5. Inertia constants, +0x50 (16),
drag, rolling resistance, final drive, clutch and shift points are the same in both games. [V]

Rollover cap: a car rolls over rather than slides when its grip is high for how far its wheels sit from the middle
(every car's wheels are 0.8 ft below its center). Every Rush 2 car but the ROCKET and the DEW keeps grip / half track
at the Pickup's, 0.95 front (2.6 / 2.75) and 1.16 rear (3.2 / 2.75); 2049's cars are 2.5-3.75 ft wide on similar
curves. California Speed's cars are narrower (0.83 ft for the Golf Cart, 1.8 / 1.96 the Sportster, 2.16-2.45 most) and
its grip factors reach 3.0 (the Dozer), so on Rush 2's curves they sat at 1.06-3.86 and flipped at small steering
inputs. Each car's front and rear grip is held to at most the Pickup's ratio x its own half track (the rear with its
+0x78), so they slide where Rush 2's cars do (the Golf Cart 0.78 / 0.97, the Sportster 1.70 / 2.28, the 2.45 ft cars
2.32 / 2.85, the Dozer 2.34 / 3.10). The car select's bars count grip but not rollover, so before this a car could rate
well and still flip. [V]

### Car name art

California Speed has no name logos for its own cars. Its car select bank, asset 9, is Rush 2's (asset 0x11): the same
26 textures, among them Rush 2's 22 car logos (4X4, BANDIT, COMPACT, CONCEPT, COUPE, CRUISER, DEW "Mtn Dew", EXOTIC,
FORMULA, HATCHBACK, HOTROD, MOBSTER, MUSCLE "Muscle Car", PICKUP, PROTOTYPE, ROCKET, SEDAN, SPORTSTER, STALLION,
SUBCOMPACT, TAXI, VAN; Rush 2's type -> logo table is 0x803C7688), every one redrawn in California Speed's lettering:
a bold pixel sans, mixed case, in a blue gradient (palette 1-11, light at the top) with a dark edge (12-15), all but
HOTROD on one shared palette, rows stored bottom-up. Capitals and l span rows 5-23, the x-height starts at row 10,
descenders reach row 27. Only Sportster, Muscle and Mt Dew among its cars have a word. [V]

The port (`src/calspeed/calspeed_logos.cpp`, cached as the blob `calspeed_name_logos`) uses those three words as they
are and spells the other 18 names (486SE, Predator, Mano, Five Oh, Hopper, Fairchild, Insect, Mercado, Golf Cart,
Squirrel, Forklift, Conv, Sled, Baja, Ol Truck, Camper, Semi, Dozer) from letters cut out of the words: a letter is the
pixels in a column window of a word whose 4-connected piece inside the window holds fill, so a touching neighbor's
edge stays behind (the T and V also drop a neighbor's corner piece). Sources: P PICKUP 25-39, M MOBSTER 18-35,
F FORMULA 17-29, H HATCHBACK 4-18, C MUSCLE 87-103, S SEDAN 27-41, E EXOTIC 29-41, D DEW 66-80, B BANDIT 27-42,
T TAXI 41-54, 4 4X4 44-57, V VAN 42-59, a SEDAN 71-83, c MUSCLE 48-59, d SEDAN 57-69, e MUSCLE 67-79, h HATCHBACK
56-67, i CRUISER 65-68, k HATCHBACK 112-123, l MUSCLE 62-65, m COMPACT 46-64, n SEDAN 86-97, o COMPACT 31-44,
p PICKUP 89-102, r CRUISER 41-48, s MUSCLE 34-46, t BANDIT 93-100, u MUSCLE 20-32. The letters no word uses are put
together from real ones: I = l; O = C united with its mirror image, 8 = S with its mirror; 6 = the S's top (its right
side cut after row 8) on the 8's bottom loop; G = C closed by the mirrored C's lower right and a bar; q = p mirrored;
f = r's arm raised to cap height on an l stem with t's crossbar; j = i continued by p's descender with a hook; v = V
resampled to the x-height at 0.78 width; z is drawn, colored with the lowercase letters' median fill per row. Letters
keep their rows and are set one texel apart (a space is 6). If the bank isn't as expected the names fall back to the
port's 5x7 lettering. [V in game]

## 8. Open items

- The menu song (for a California Speed menu song switch in the Sound tab).
- What the finer route line in the paths is for.
