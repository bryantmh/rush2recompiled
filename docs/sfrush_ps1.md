# San Francisco Rush (PlayStation)

Research notes for porting the PlayStation version's exclusive content: its fourth track and seven cars the N64 game
doesn't have. The shared content (tracks 1-3, the other cars) comes from the N64 ROM as before; the PS1 disc only
adds what is its own, whether or not the N64 ROM is there.

Addresses are PS1 (RUSH.EXE) unless marked otherwise. Tags: **[V]** verified in code or data, **[I]** inferred.

Tools (`tools/sfrush_ps1/`, research aids; the converter is C++):
- `ps1.py`: disc reader (the .cue's MODE2/2352 data track, ISO 9660), `.DAT`/`.SIZ` chunk splitter. Set
  `SFRUSH_PS1_DISC` to the .cue or the data track's .bin.
- `disps1.py`: annotated disassembly of RUSH.EXE (`asm`, `func`, `refs`, `str`), written to `out/rush.asm`.
- `vram.py`: the VRAM placement of a track's texture pages and CLUTs, read from the game's loaders.
- `track.py`: road and scenery chunks as a textured .glb for `tools/glb_view.py`.
- `collision.py`: the collision map.

## Contents

- [Disc](#disc)
- [Exclusive content](#exclusive-content)
- [Units and axes](#units-and-axes)
- [Track geometry](#track-geometry)
- [Textures](#textures)
- [Collision](#collision)
- [AI paths](#ai-paths)
- [Checkpoints](#checkpoints)
- [Start and finish banner](#start-and-finish-banner)
- [Race times](#race-times)
- [Cars](#cars)
- [Wheels](#wheels)
- [Car physics](#car-physics)
- [Sky](#sky)
- [Props](#props)
- [What the converter makes](#what-the-converter-makes)
- [Track select diorama](#track-select-diorama)
- [Snow and night](#snow-and-night)
- [Music](#music)
- [Not ported](#not-ported)

## Disc

- **Supported disc:** San Francisco Rush - Extreme Racing (USA), SLUS-00505. A .cue with one MODE2/2352 data track
  and four CD audio tracks.
- **Image formats** (`src/sfrush_ps1/ps1_disc.cpp`): a .cue (one .bin per track, or all in one), a CloneCD .ccd/.img
  (the .img is the whole disc in raw sectors; the .ccd's `[TRACK n]` sections give `MODE=0` for audio and
  `INDEX 1=<sector>` from the .img's start), a .chd (`include/chd_image.h`, shared with the Dreamcast disc), or the data
  track alone (.bin/.img, or an .iso of 2048-byte sectors, which has no music). A data track file's sector layout is
  found from where the volume descriptor sits: raw 2352 (+24, +16 for a MODE1 rip), 2048 or 2336 (+8). The mod's key
  is the picked file's name, size and time, so a .cue-built install keeps its key. **[V]**
- **Music:** the CD audio tracks, read through `audio_tracks()` / `read_audio()`; a .chd stores CD audio big-endian
  and `read_frame` swaps it back. `tools/sfrush_ps1/ps1_variants.py` writes the .cue as .iso, .ccd/.img, one-file .cue
  and .chd; the converter (`cpp_test`) builds the same mod from each and prints each song's hash (all equal). **[V]**
- `SLUS_005.05` is the boot program; `EXE/RUSH.EXE` is the game (PS-X EXE, loaded at 0x80010000, size 0xA3000,
  entry 0x8008D794). **[V]**
- File loads go through `func_8004AA58(name, size, 0, dest)`. **[V]**
- Many files are `.DAT` + `.SIZ` pairs: the `.SIZ` is a list of s32 chunk sizes, the `.DAT` the chunks back to back.
  **[V]**

## Exclusive content

Compared with the N64 game (docs/rush1_research.md) by the AI paths and the car table:

- **Tracks.** PS1 tracks 1-3 are N64 tracks 1-3 (Golden Gate, Embarcadero, Market): their AI paths match the N64
  lanes once scaled and mirrored ([Units and axes](#units-and-axes)). **PS1 track 4 is its own course** (Presidio,
  MacArthur Tunnel, Chinatown, the Financial District with the Transamerica Pyramid, the Bay Bridge approach); its path
  matches none of N64 tracks 4-7 nor the arcade's. `T1ROAD/T5ROAD2.DAT` + `COLLDAT5` are a second road for track 1
  (track 1's scenery and textures), not a separate course. **[V]**
- **Cars.** The car table in RUSH.EXE (`cars\*.psv` from 0x800113D8) has 16 entries: BMW, BUS, CAM, VIPER, SUPRA,
  BUG, BUGETTI, CONCEPT, STARSKY, JEEP, JETCAR, MINI, BBUS, TAXI, PIGGY and the UFO (`extra\ufo.psv`). The N64 cars
  are BMW, Camaro, Supra, Bugatti, VW Bus, Viper, VW Bug, Concept, Taxi, Hot Rod and Formula 1, so the PS1's own are
  STARSKY (texture `HUTCH01`), JEEP, JETCAR, MINI, BBUS (a school bus), PIGGY and the UFO. **[V]**

## Units and axes

- **Track frame:** x and y across the ground, **z up**. A road or scenery vertex is 1 ft; a chunk origin is 1/256 ft;
  AI paths and collision are 0.1 ft. **[V]** (collision vertices land on road vertices to 0.05 ft; the Transamerica
  Pyramid stands up.)
- **The converter's frame** (`game` in ps1_track.cpp) is (x, y, z) = (PS1 y, PS1 z, PS1 x). It is the PS1 world's
  mirror image ([Handedness](#handedness)), so the track is written to glTF with game x kept (`gl`), not negated as
  `a49::to_gltf` does.

### Handedness

The PS1 shows the mirror image of (PS1 y, PS1 z, PS1 x) written out the usual way (x negated into glTF). **[V]**, by
three checks that agree:

- **Text, read by the renderer's own UV pairing** ([Track geometry](#track-geometry)): every sign and banner face
  checked (the McDouglas Tunnel / Freeway gantry, SCN2_14 faces 66, 67, 80, 81; FINISH, SCN2_89 15, 19, 99, 103)
  comes out upright and mirrored left to right in that frame, and reads right only un-mirrored. The gantry's strips
  then join (u 191, 159, 127 across their shared edges).
- **The N64 courses:** PS1 tracks 1-3 are the N64 SF Rush's. A best fit (rotation, scale, offset) of each PS1 path
  (COLLDAT2/PATHDAT<n>) to the N64's lane 0 is closer mirrored: 0.6% against 0.9% of the course's size (track 1),
  0.4% against 3.7% (2), 0.5% against 1.2% (3). (An earlier note here read the unmirrored frame as a 17 ft fit; it
  doesn't reproduce.)
- **The cars:** PSV models were already written un-mirrored ((-y, z, -x), a turn of (y, z, x)), as their lettering
  needs; the old track was the odd one out.

The PS1's track select picture (TITLETGA/TRK4N.TGA) is itself drawn mirrored against the course it shows, so it can't
be used to check this.

## Track geometry

`T<n>ROAD/T<n>ROAD2.DAT` (the road, 50 chunks on track 4) and `T<n>ROAD/T<n>SCN2.DAT` (scenery, 90 chunks), loaded by
the track loader `func_80036C40`; each chunk is fixed up in place by `func_800369B8` (road flag a1 = 1). **[V]**

| Offset | Contents |
|---|---|
| +0x00 | s32 1 |
| +0x04 | s32 origin x, y, z (1/256 ft) |
| +0x10 | u32 vertex count |
| +0x14 | u32 face count |
| +0x18, +0x1C | RAM pointers to the vertices and faces (rewritten by the loader) |
| +0x20 | vertices: 3 x s32, 16.16 ft, added to the origin |
| then | faces, 0x28 bytes each |

Face (0x28 bytes), as `func_800369B8` turns it into the game's 0x1C-byte draw record:

| Offset | Contents |
|---|---|
| +0x00 | u8[3] flags (0x1000, 0x2000 seen) |
| +0x03 | s8 CLUT index: CLUT id = u32 table at 0x800A7B08 (`(y << 6) | (x >> 4)`) |
| +0x04 | u16[4] vertex indices, in ring order (not the PS1 GPU's zigzag) |
| +0x0C | 4 x (u16 v, u16 u) |
| +0x1C | u32 texture: an entry of the track's texture table ([Textures](#textures)) |
| +0x20 | u16[4]; only +0x22 is kept (draw record +4) |

Faces are one-sided: the road is wound one way throughout (seen from above with `glb_view.py --cull -1`). **[V]**

**UV pairing:** the record's UVs don't go with its vertices corner by corner. The chunk renderers
(`func_80069684`, `8006C588`, `8006F33C`, `8006F95C`) fill a POLY_FT4 with the vertices 0, 1, 3, 2 (the GPU's zigzag)
and the UVs 1, 0, 2, 3, so vertex 0 shows corner 1's coordinate, vertex 1 corner 0's, vertex 2 corner 3's and vertex
3 corner 2's. Paired straight, a face's texture is mirrored along its first edge: sideways where that edge is
level (the gantry's strips, halves swapped), upside down where it is upright (walls, signs, banners). **[V]** The
converter pairs them as the renderer does (`read_chunk`, `uv[k ^ 1]`). PSV models (cars, props) pair them straight
(`func_80044E34` fills a POLY_GT4 with vertex k and coordinate k).

## Textures

- `T<n>PAGE/T<n>PAGE.DAT` (day), `T<n>PAGES.DAT` and `T<n>PAGEN.DAT` (other times of day; the loader picks by
  0x800B2710 / 0x800B2768): chunks of 8 header bytes, a 16-color CLUT and 4-bit texels 256 wide. The page loader
  (`func_80037C2C`, `80038644`, `80039048`, `80039A88` for tracks 1-4) uploads chunk k with LoadImage
  (`func_80092C58`) at a RECT hard-coded in the loader. **[V]**
- `T<n>CLUT/T<n>CLUT.DAT` (+ S, N): 1064-byte chunks whose 16-color CLUT (+8) the CLUT loader (`func_8003A4A0`,
  `8003AF18`, `8003B990`, `8003C408`) puts at a hard-coded (x, y) through `func_80014F38`, which also writes 31
  copies below it, each a step brighter or darker (distance shading). Row 0 is the original. **[V]**
- The texture table (35 entries of 6 s32: VRAM x, VRAM y, ?, v offset, CLUT x, CLUT y) is static: 0x800A7478 for
  track 2 (when 0x800B34F8 == 1), 0x800A77C0 for tracks 1, 3 and 4, which share one VRAM layout. A face's texel (u, v)
  is at VRAM (x & ~63 + u / 4, (y & 256) + v + v offset), 4-bit, through its CLUT. **[V]** The fixup
  (`func_800369B8`) stores u and v + v offset as bytes, so they wrap at 256 (18 corners on track 4); the converter
  wraps them too.
- **Snow and night:** every track has an `S` (snow) and `N` (night) set of pages and CLUTs as well: see
  [Snow and night](#snow-and-night).
- **Against the N64 courses (tracks 1-3):** same routes, but the PS1's own lower-detail models and 16-color textures;
  props only on track 2 (33 cones, 8 + 6 of `T2KNOCK`'s models, against the N64's 89 cones, 15 trash munchers, 15
  traffic lights and more) and track 4; none on tracks 1 and 3 (the N64's track 3 has 112). **[V]**
- `vram.py` replays the loaders' calls (straight-line code) to get the RECTs and CLUT positions, and samples VRAM as
  the GPU does.

## Collision

`COLLDAT<n>/` (loader `func_8004FBFC`): `COLLMAP.DAT` + `.SIZ` (49 chunks on track 4), `FLAGS.DAT` (one byte per
polygon, chunks in order), `BOUNDS.DAT` (per chunk: x min, x max, y min, y max), `TRACK<n>.CPF`. Units 0.1 ft;
vertices are (x, -height, y) of the track frame. **[V]**

| Offset | Contents |
|---|---|
| +0x04 | origin (1/256 ft) |
| +0x10 | u32 vertex count |
| +0x14 | u8 polygon count |
| +0x24 | u8 wall count (group A) |
| +0x25 | u8 group B count |
| +0x26 | (loader) ground count = polygons - A - B |
| +0x28 | vertices, 3 x s32 |
| then | A records (0x18), B records (0x20), ground records (0x20, or 0x28 when FLAGS & 7) |

- **Walls (A, 0x18):** s32 d, s16 nx, s16 nz (16384 = 1, pointing into the road), s32 x min, x max, y min, y max: a
  vertical line in plan with no height, the diagonal of that box lying on the plane n . p = d (on track 4 every wall
  fits one diagonal exactly; taking the other ran diagonal walls across the road and wrecked the computer cars).
  **[V]**
- **Raised walls (B, 0x20):** an A wall (s32 d, s16 nx, nz, its box) and two heights, +0x18 and +0x1C (0.1 ft, y
  down). `func_8005AD28` tests a car against one only while its center is over the first (car height - 8 < +0x18):
  railings on a deck over other road. 8 on track 4 (chunk 12 has one plane in two bands, 70-90 and 129-169 ft up).
  **[V]** The converter builds each from 3 ft under its first height (a car on the deck, its center 2-3 ft up, meets
  it there) to its second.
- **Ground (0x20):** s32 d, s32 x min, x max, z min, z max, u8 vertex indices[4], s16 normal[3] (16384 = 1, y down).
  **[V]**
- **Ground (0x28, FLAGS & 7 != 0):** s32 d0, d1, bounds, u8 indices[4], two normals: a bent quad, one plane per
  triangle. **[V]**
- The ground query (`func_8004D12C`) checks the bounds with 15 units of slack, then the quad. **[V]**
- **FLAGS bits:** `& 0x18` is the surface: 0 road, 0x08 dirt, 0x10 grass, 0x18 water (read from the textures drawn
  over each class's polygons; on track 4 the 0x10 class is the park's grass and sand shoulders, and 0x18 the
  channel beside the shortcut). 0x40 = the next record is skipped by the ground query; 0x80 (ordinary road and grass
  on track 4) sets a per-car flag (0x801015C4 + car x 0x408) read at 0x8005E134 and 0x80062F04. `& 7 != 0` marks the
  0x28-byte records. **[V]** surfaces, **[I]** 0x80.
- **Steps:** a dead end can be a ground polygon instead of a wall. Track 4's lane beside the street that climbs to the
  big loop (glb x 744-762 m, ending at z -120 m) ends in one road polygon rising 65 ft in 40 ft (59 degrees) to the
  street above, drawn as a rock wall with a chevron sign. A PS1 car doesn't climb it **[I]** (how the PS1 refuses it
  isn't traced); Rush 2 drives up any ground face, so players went straight through the "wall" (user report
  2026-11-09). The converter (`build_condition`, ps1_track.cpp) walls the foot of every ground polygon steeper than
  53 degrees (normal y 0.2-0.6), rising 4 ft or more, with flat ground (normal y > 0.9) just past both its low and
  high edges: one face along the low edge facing downhill, 3 ft under it to 3 ft over the top, the polygon kept as
  ground so a car coming down from above (behind the one-sided wall) still rolls down. Only that polygon qualifies
  on the PS1 disc; Golden Gate's 54-degree banked-curve bands (next to other bands or nothing) stay drivable.

## AI paths

`COLLDAT2/PATHDAT<n>.DAT` and `EXTDAT<n>.DAT` (forward), `REVPATH<n>` and `EXTRDAT<n>` (backward), loaded by
`func_80055678` (pointers gp+0xE38 and gp+0xBA0, count gp+0x454). u32 count (low byte), then 20-byte records: s32 x,
s32 y, s32 speed (?), s32 heading (`& 0xFFF`, 4096 a turn), s32 height (y down); 0.1 ft. **[V]**

- The two files are the two edges of the drones' corridor: a drone aims at PATH + (EXT - PATH) x w / 16, w its own
  byte at 0x800B2B38 (`func_800560B0` area). Where they meet the corridor is one line. **[V]**
- Reset points take x, height and y from a record (`0x80055E78`). **[V]**
- The points are about 200 ft apart; the drones drive straight chords between them. **[V]**
- The third field (235-898, high on straights) isn't read near the path code; not ported. **[I]** a speed.

## Checkpoints

`COLLDAT<n>/TRACK<n>.CPF` (loaded to 0x80113490 by the track's loader, `func_8004FBFC` for track 4): four planes of s32
nx, ny, nz, d (16384 = 1; n . (x, -height, y) = d in 0.1 ft). Plane 0 is the finish line. **[V]**

- **Sectors:** `func_8004FBFC` also loads `COLLDAT<n>/BOUNDS.DAT` to 0x800D8168 (49 records on track 4, the count in
  gp+0xE90): s32 x min, x max, y min, y max (0.1 ft). `func_800554F0` (car_sector) gives a car the last box that holds
  it (car +0x2FC; tracks 1 and 3 then patch a few overlapping boxes, 0x80052F00 on). **[V]**
- **The test:** `func_80054630` (checkpoint_test), each car each frame: a car at gate k (car +0x300) tests plane k
  only while its sector is one of gate k's, from per-track lists (track 4: counts 0x800B2B18, sectors 0x800B2B1C,
  first index 0x800AA3D8; one sector a gate: 1, 9, 19, 21). Forward (0x800B271C = 1) n . p < d passes the gate (+1,
  4 wraps to 0; passing gate 0 counts the lap at car +0x68), backward courses count down crossing the other way.
  `func_80054130` is the pass event. **[V]**
- **Into Rush 2:** each gate goes where the AI path crosses its plane (from n . p > d to below) inside one of its
  sectors, the finish nearest the path's first point and each checkpoint the first such crossing after the gate
  before it (path points 0, 279, 613 and 724 of 1121 on track 4).

## Start and finish banner

The gantry over the start line is two scenery chunks with the same 217 vertices: SCN2 chunk 1's two panels read
CHECKPOINT (red, page (384, 0)) and chunk 89's read FINISH (texture entry 3, u 0-127; faces 15, 19, 99, 103). The
START texture beside FINISH on that page isn't on the gantry. **[V]** (rendered from the converted chunks)

- **The swap:** the scenery draw loop in `func_80042FF0` takes chunk id s2 from the visible list and its pointer from
  the table 0x800B5C88 (ids from 1, so id n is SCN2 chunk n - 1). On track 4 (0x800B34F8 = 3), when gp+0x20 is set
  and the id is 2 (0x80044628), it draws the chunk at 0x800B5DF0 (id 90) instead (0x80044638). Tracks 2 and 3 have
  their own pairs (ids 0x5F, 0x82 -> 0x800B5EA4, 0x800B5E94; 0x95 -> 0x800B5F44). **[V]**
- **gp+0x20 (0x800B26F0):** set per view before each player's scenery pass (0x80016468 player 1, 0x80016884 player
  2) when that player's laps still to go (0x80101250 / 0x80101658, car stride 0x408; laps 0x800B2888 - 1 at race
  setup 0x80015EE0) are 0 or fewer: the viewing player's own last lap. **[V]**
- **Into Rush 2:** Rush 2's own finish banner swap (track_finish_line_setup_8008F080: CHKPNT's texture-load list for
  FINISH's in `<prefix>FINISH`, when a player passes the last checkpoint of the last lap). Chunk 1 is the custom track's `FINISH_BANNER`
  ([Finish banner](custom_tracks.md#finish-banner)); its four panel faces (the ones chunk 89 retextures) are the
  CHKPNT material, the CHECKPOINT strip resampled corner for corner onto FINISH's 128 x 32 rectangle, and FINISH is
  that rectangle of chunk 89's page. Chunk 89 isn't drawn. Rush 2's swap is for the whole screen, so in split screen the first player there turns it for everyone.

## Race times

The clock (gp+0x144, seconds left) is set when a race starts (0x80015D60): 30 by default, 38 on track 4 and 45 on
track 4 forward (gp+0x70 the track, gp+0x4C the direction), 60 or 180 in other modes. **[V]**

- **Gates:** `checkpoint_passed` (0x80054130), for the leading human player only, adds the passed gate's byte from a
  per-track table (gp+0xC60; track 4: 0x800B2AE8 = 26, 29, 11, 32, the time for the stretch after the gate; track 1
  0x800B2AD8, track 2 0x800B2AE0, track 3 0x800AA37C), then 5 more on track 4 forward while the player has passed
  fewer than 3 gates (gp+0x424 counts them, so lap 1's first two checkpoints), 4 at difficulty (0x800B288C) 5 and 3
  more at 6, 8 when gp+0x40 is set and 2 less when 0x800B3160 is 2 (menu modes). **[V]**
- **The clock** (`race_clock`, 0x80018198) takes a second off every 61 calls, once a frame. **[V]** Its seconds are
  treated as real seconds: on the courses both versions have the PS1's lap times are the N64's to 10% (track 2: 90
  against 100 s), and a drone with the converted cars reaches track 4's first checkpoint in 42 s of the PS1's 45. A
  frame at the 25 frames a second the car conversion implies would make them 2.4 times longer. **[I]**
- **Into Rush 2:** the track.toml `[race]` times: `start_time` 45, `finish_time` 26 (the finish line's own time;
  Rush 2's custom tracks otherwise add the start time again there), `checkpoint_time` [34, 29], [16, 11], [32, 32].
  Rush 2's difficulty scaling (x 1 + (5 - difficulty) x 0.075) applies to them as to every custom track's, in place
  of the PS1's difficulty bonus.

## Cars

`CARS/<name>.PSV`, `EXTRA/UFO.PSV`; textures `CARSGFX/<tex>.GFX` (8 bytes, a 16-color CLUT, 4-bit texels 256 wide:
256 x 64), loaded by `func_80030C18` (car_load). **[V]**

| Offset | Contents |
|---|---|
| +0x38 | u32 vertex count, face count, vertex offset, face offset |
| vertices | 3 x s32, 16.16 |
| faces | u32 flags, u32 n (3 or 4), u32 index[n], (u32 u, u32 v) << 16 x n, u32 texture, u32[n] |

- **Size:** `func_8003E714(model, a1)` turns each coordinate into coordinate x a1 / 64 (an integer, 0.1 ft), so a
  coordinate is a1 / 640 ft. car_load passes a1 = 12 for the eight SF Rush cars and for STARSKY, JEEP, JETCAR and
  PIGGY, 13 for MINI and BBUS, 11 for TAXI and 19 for the UFO (the order of its calls). **[V]** The wheel offsets
  below land on the models' wheel arches only at these sizes (STARSKY's front axle: 91.2 sixteenths of a foot from
  the table, 91.1 from the arch). Against Rush 2 these sizes are big: Rush 2's SF Rush cars (types 4-11) are the
  same eight cars, and the PS1's models of them come out 1/0.8184 times as long on average
  (`python tools/sfrush_ps1/car.py size <disc>` compares them), so the converter scales every PS1 car by 0.8184
  (`car_size`, ps1_cars.cpp). At that scale some of the PS1's own cars outgrow Rush 2's range across and up (its
  widest car, all but the secret ROCKET, the CADDY at 6.62 ft; its tallest, the DEW at 5.12): the Starsky and Police
  are 7.0 ft wide, the UFO 10.2 ft across. Each car is shrunk further, wheels with it, by as much as its width or
  height needs to fit (`rush2_max_width`): the Starsky and Police to 0.770 of the PS1's size, the UFO to 0.531; the
  others fit at 0.818. Length is free, so the City Bus keeps its 18.7 ft (fitting it to the CADDY's 15.0 made it
  too small).
- **Ride height:** the PS1's physics doesn't know the model, so it carries every car's origin at the same height
  over the ground. The wheeled cars' arches put it at 2.8 ft on average (at car_size); the UFO, with no arches, gets
  that mean (scaled to its size), and its underside clears the road by 0.64 ft, as theirs do by 0.55-0.91. From the
  bare wheel point (0.9 ft down) it sat on the road.
- **Axes:** -x is the front, z up. `func_8003E714` then turns the model 90 degrees about x into the GTE frame (y
  down). Game (x, y, z) = (-y, z, -x) makes the lettering read right on both sides (the Mini's 15, MAYLEE, CURTIS;
  POLICE; OZONE 33) and leads with the grille. **[V]** It has the track's handedness ([Handedness](#handedness)). (An
  inside-out render first suggested +x; it was the far side seen through culled faces.)
- **Height:** a PSV model is centered on its bounding box, its origin half its height over its underside; a Rush 2
  car model stands on y 0 with its axles at y 3.2 (front) and 4.0 (rear), and the physics takes the model's origin
  as the center of mass. Centered, a PS1 car's mass sat about 2.4 ft over a Rush 2 car's, and the police car went
  over backwards under full throttle. The converter lifts each model and its wheels so the axles' mean is at 3.6
  (`rush2_axle_y`, ps1_cars.cpp); full throttle from a standing start then keeps it level. **[V]**
- **Textures:** face texture 2 is `EXTRA/WHEELS.GFX` (256 x 32: the underside); the other index is the car's own
  `<tex>01.GFX`. car_load uploads only a car texture's texels (the file + 0x28), never its CLUT: every car is
  drawn with the player's paint page (`func_80030930`: 0 BLUE, 1 GREEN, 2 RED, 3 YELLOW, 4 WHITE, 5 PURPLE, 6 ORANGE,
  7 BROWN, 8 BLACK; the player starts on 0), whose CLUT entries 1-6 are the paint ramp. PIGGY is always WHITE
  (`func_800142C4`). **[V]** The converter draws the ramp with WHITE's greys scaled so entry 6 is 230 and masks it as
  MAIN COLOR, so each car but PIGGY takes any Rush 2 color (default 10, Rush 2's blue, for the PS1's BLUE); PIGGY gets
  WHITE's ramp unmasked. Rush 2 draws a mod's painted texel as the paint's color (CARPALETTE MAIN entry 1, grey 230)
  times the texel's brightness over 230 (texture_derive `paint_mod`), so the ramp's lightest entry is the full paint
  color, as on the PS1. The mask goes in the glb as `extras.rush2_paint`, a texture index (Glb::paint_mask makes the
  texture); an image index there once picked the wrong texture, WHEELS.GFX, and painted the cars in its streaks.
- **The PS1-only cars:** STARSKY (`HUTCH01`, a striped Torino), JEEP (`JEP01`), JETCAR (`JET01`), MINI (`MINI01`),
  BBUS (`BBUS`, a city bus, OZONE 33), PIGGY (`PIGGY14`, a police car) and the UFO (`EXTRA/UFO.GFX`). **[V]**
- **Special cars:** gp+0x28 (0x800B26F8) is the special car: 1 STARSKY, 2 JEEP, 3 JETCAR, 4 MINI, 5 BBUS, 6 TAXI,
  7 PIGGY, 8 UFO. `func_800142C4` builds each on one of SF Rush's eight cars, the car type (car +0x4C; 0 BMW, 1 BUS,
  2 CAM, 3 VIPER, 4 SUPRA, 5 BUG, 6 BUGETTI, 7 CONCEPT) that picks its physics row and wheels: STARSKY 6, JEEP 5,
  JETCAR 3, MINI 4, BBUS 1, TAXI 0, PIGGY 2 (white paint, 0x800B2FE8 = 4), UFO 7. **[V]**

## Wheels

`func_8004B230` places and draws each car's four wheels (`COLLDAT3/WHEEL.PSV`, loaded by `func_8004B120` at a1 = 12:
radius 1.14 ft) at the points the physics queries the ground with. **[V]** In 0.1 ft, car frame (x across, y down,
z ahead), with the car type's row of 0x800A9CD0 (4 s8: b0..b3):

| | Offset |
|---|---|
| left, right | -30 - b0, 30 + b2 |
| front, rear | 51 + b1, -51 - b3 |
| height | +9 (0.9 ft under the car's origin) |

Rows: BMW (0, 3, 0, -9), BUS (0, -1, 0, 3), CAM (0, -5, 0, -4), VIPER (0, 1, 0, -10), SUPRA (0, 1, 0, -9), BUG
(-5, -5, -5, -7), BUGETTI (-1, -7, -1, 1), CONCEPT (4, -7, 4, 3). The special cars change them: STARSKY right
35 + b2, left -5, front +13, rear -8; JEEP front +6; JETCAR front -7, rear -7; MINI front 41 + b1; BBUS left -6,
right +4, front +10, rear -1; TAXI left -3, right +3, front -2, rear -8; PIGGY left -4, right +4, front +15, rear -7.
The UFO's wheels aren't drawn (the draw loop skips the player's car when the special car is 8). **[V]** The height
isn't fixed: the PS1 sets each wheel on the ground under its point (per-wheel heights at 0x800E9938, 9 at load), so
where a wheel shows on the body is set by the body's arch. The converter keeps the PS1's x and z and puts each wheel's
top at its arch's top (the side silhouette's lowest point over the wheel, `arch_top`); the tires' outer faces then
meet the bodies' sides to 0.2 ft (the Jet Car's stand out 0.4 ft, the Jeep's tuck in). **[V]** The fixed +9 put every
wheel a radius too high, centered on its arch's top.

## Car physics

The PS1 has one physics model for every car (`func_8005F234`, the per-frame car tick, run once a game frame from
`func_8004FD98`), unlike Rush 2's: a heading the steering turns, a thrust along it, and friction tires. **[V]**

The car table (0x800AA4D4, 44 bytes per car type, copied to gp+0x4C0 and on each frame by `func_8005B340`; drones,
car +0x2FF set, use row 0): **[V]**

| Offset | To | Meaning |
|---|---|---|
| +0, +1, +2 | gp+0x4C1, 0x4C0, 0x4C2 | angular velocity damping shifts (`func_8005EAC0`, `func_8005ED90`, `func_8005EF98`) |
| +0x4 | gp+0x4C4 | steering rate, angle units (4096 = 360 degrees) a frame (`func_8005D96C`); `func_8005B340` adds 20 - 2 x class under each of speeds 300, 270, 200 and 150 |
| +0x8..+0x14 | 0x800AA8D8[4] | thrust by gear: the heading (256 long) x it / 16 is added to the velocity with the throttle (0x8005FC98); a gear too high for the speed gets 8, 6, 5 (`func_80067298`) |
| +0x18 | gp+0x4EC | gravity a frame while not every wheel is down (`func_8005E0CC`) |
| +0x1C | gp+0x4CC | tire grip: the sideways friction is the sideways velocity x (grip + up to 64) / 256 a wheel (`func_8005E34C`) |
| +0x20 | gp+0x4D0 | rolling friction: the velocity along the car x it / 1024 a wheel |
| +0x24 | gp+0x4D4 | the height the tire forces act at, for the turn they give (`func_800608E0`) |
| +0x28 | gp+0x4D8 | written, no reader found |

| Type | +0 +1 +2 | +0x4 | Thrust | +0x18 | Grip | +0x20 | +0x24 | +0x28 |
|---|---|---|---|---|---|---|---|---|
| 0 BMW | 10 14 10 | 20 | 12 | 600 | 16 | 2 | 0 | 2560 |
| 1 BUS | 10 14 11 | 20 | 12 | 600 | 16 | 2 | 48 | 2560 |
| 2 CAM | 10 14 11 | 22 | 13 | 600 | 12 | 2 | 0 | 2304 |
| 3 VIPER | 10 14 11 | 20 | 13 | 600 | 12 | 2 | 0 | 2304 |
| 4 SUPRA | 10 15 12 | 22 | 15 | 650 | 4 | 2 | 16 | 1024 |
| 5 BUG | 11 14 12 | 20 | 15 | 700 | 8 | 2 | 25 | 1280 |
| 6 BUGETTI | 11 14 12 | 25 | 17 | 550 | 1 | 2 | 16 | 1280 |
| 7 CONCEPT | 11 14 12 | 25 | 17 | 550 | 1 | 2 | 16 | 1280 |

- **Class** (car type / 2): `func_8005D190` moves each car its velocity x step / 128 a frame, step 0x80, 0x84, 0x88,
  0x94 by class (drones: the player's class, 0x90 for class 3, plus 6 after their first 40 frames), then adds the
  frame's forces to the velocity. `func_8005E34C` adds friction along a sliding car for classes 2 (x2) and 3 (x4).
  `func_8008C038` scales the push a car gives in a collision by 192, 200, 208 and 218 / 128. `func_80064D48` damages
  a car hit at a speed / 10 of 7 or more (types 6 and 7: / 9). **[V]**
- **Speed:** with the same rolling friction on every car, the velocity settles where the thrust meets it, so a car's
  top speed and its acceleration both go as thrust x step: against the BMW, CAM and VIPER x1.117, SUPRA and BUG
  x1.328, BUGETTI and CONCEPT x1.638. The gearbox (0x800AA8B8: m 0-1500, 1200-3400, 2900-4500, 2700-6000 by gear, m =
  |v| / 4) puts the BMW's top speed (m 6144) at its last gear's end. Car speed (car +0x30) is the horizontal |v| / 32
  (0x80050324); positions are 1/2560 ft (the start grid 0x800AA1B8 is 0.1 ft << 8). **[V]**
- **Special cars:** the UFO's forces are doubled under speed 100 and its vertical one halved above it; PIGGY's are
  halved over speed 301 and quartered over 351 (only as the player's car, `func_8005D190`). **[V]**
- **Helpers:** `func_8005BF58` builds the thrust direction 0x800ED780 (256 long) from the car matrix and the front
  wheel angle (car +0x9A); `func_800671F8` keeps the gearbox's speed (|v| / 4) in 0x8010CA10; `func_800675A8` picks
  the gear (0x800B3230) against 0x800AA8A8's thresholds or from the shift buttons. **[I]**
- **The clock** counts 61 game frames to a second (`func_80018198`, gp+0x148), but the game runs as fast as it draws
  (`VSync(0)`, `func_80090DB4`), so the PS1's speeds in feet a second aren't fixed ([Race times](#race-times)). **[I]**

**Into Rush 2** (`ps1_physics`, src/sfrush_ps1/ps1_cars.cpp): the model the 2049 cars use (car2049.cpp): each PS1
value with a Rush 2 counterpart becomes the Rush 2 roster's mean (its 21 cars) times the car's value over the PS1
roster's mean (its 8 SF Rush cars); every Rush 2 value with no PS1 counterpart is the Rush 2 roster's mean (gear
ratios -3.1, 0, 3.095, 1.754, 1.254, 0.956; common torque 1.3; yaw damping 30.7; mass 108.4; preload 0.185;
durability 0.524). The pairs: thrust x class step -> torque in gears 1-3 (Rush 2 means 1.820, 1.842, 1.885), steering
rate -> steering (542.9), grip + 64 -> rear grip (0.960), class push -> mass and pitch/roll inertia (2728.6), damage
divisor -> yaw inertia (387619). SF Rush and Rush 2 are close relatives, so the values land near Rush 2's own; no
roll cap or per-car fix-ups (STARSKY's and the UFO's torque above Rush 2's range is the PS1's). Not carried: PIGGY's
taper, the per-car gravity in the air, the low-speed steering bonus, the slide friction and the damping shifts
(Rush 2 has no per-car values for them).

## What the converter makes

`src/sfrush_ps1/` (C++, run from the Games tab's SF Rush PlayStation section; `tools/sfrush_ps1/cpp_test/build.bat`
runs it outside the game) builds one mod, `sfrush_ps1`:

- `tracks/chinatown/`: track 4 as a custom track, named Chinatown (the PS1 only numbers its tracks; the name is the
  middle of its map). Scenery = every road and scenery face, one material per (texture page, CLUT), sampled from a
  VRAM laid out by replaying the game's page and CLUT loaders (`scan` in ps1_track.cpp), each vertex with the UV the
  renderer gives it ([Track geometry](#track-geometry)). Faces wind clockwise from the front in the game frame,
  written un-mirrored ([Handedness](#handedness)). Collision = the ground polygons by surface (the grass class as the custom surface `COL_VERGE`, `like = "dirt"`,
  `drag = 15`, since Rush 2's `COL_GRASS` is wheel material 2, its water, which throws the water splash), the walls as faces from 10 ft under the ground
  beside them to 40 ft over it (cut short under ground above). FINISH and CHECKPOINT_1-3 from the CPF planes. AI:
  AIPATH_main between PATHDAT and EXTDAT (at least 12 ft to a side), AIPATH_lane1-4 at 0, 1/3, 2/3 and 1 across the
  corridor (the drones' own spread); speeds left to the converter. Logo: `assets/sfrush_ps1/chinatown_logo.png`
  (`tools/build_banners.py --ps1`). Track select picture (`preview.png`, behind the route preview): the PS1's own
  TITLETGA/TRK4N.TGA whole (512 x 168, 15-bit, rows top-down whatever its header says), a map of the course naming
  its districts, at its own 512 x 168 texels and shown 224 x 73 between the carousel arrows. Diorama:
  `diorama.glb`, hand-made ([Track select diorama](#track-select-diorama)). Listed after the SF Rush tracks.
- `rush2/new_cars/PS1*.glb/.json`: the seven cars at the PS1's sizes x 0.8184, fitted to Rush 2's range
  ([Cars](#cars)), lifted to Rush 2's
  axle height, wheels where the PS1 puts them at its wheel's size (`wheel_scale` 0.997 of Rush 2's 15/16 ft), and the
  PS1's handling of the SF Rush car each is built on ([Car physics](#car-physics)). The UFO's json has
  `"no_wheels": true`: it drives on wheels drawn at scale 0 (`rush2_mod_car_wheel_scale`, hooked after the wheel
  scale loads: in a race carselect_preview_update_8005A598 at 0x8005ABA4 and 0x8005AC04, the type at physics car
  +0x7EA; on the car select car_preview_copy_matrices_800878C8 at 0x800879F4 and 0x80087A34, the type = preview id %
  73, x 4 in $s2). It hovers, so it also has `"no_wings"` (they open and steer it in the air, unseen:
  src/rush2049/wings_render.cpp skips it), `"no_skid_marks"` and `"no_wheel_dust"` (src/surface_fx.cpp: no tire marks,
  its wheels throw nothing) and `"round_shadow"` 3.31 (ft, half its width): src/mod_car_shadow.cpp redraws its
  shadow polygon after car_model_update_parts_80059D24 sets it (0x8005A270) as a square of that radius on the ground
  under its wheel points, with two copies turned 30 and 60 degrees, a soft disc (docs/car_mods.md#new-cars).
  Each car's name logo is `assets/sfrush_ps1/<ID>_name.png` in SF Rush's banner lettering (`tools/build_banners.py
  --ps1`), named by the json's `"logo"`. Cars of this mod count as SF RUSH in the Game Filter and list after Rush 2's
  cars (`set_mod_car_game` by mod id); the track lists with the SF Rush tracks.

## Track select diorama

The N64 SF Rush shows each track on its track select as a hand-made diorama (asset 10: TRK1O2 ... TRK7O5, which the
Rush 1 port uses as they are, src/rush1/track1_convert.cpp). The PS1 has none for Chinatown (its track select shows
the TRK4N.TGA map), and shrinking the track itself reads as noise at that size (the Rush 2049 attempt,
src/rush2049/track2049_art.cpp `use_miniature`). So `src/sfrush_ps1/ps1_diorama.cpp` (`ps1::build_diorama`) builds one
in the N64 dioramas' manner from a hand-written layout, `assets/sfrush_ps1/chinatown_diorama.json` (shipped; part of
the mod's source key, so editing it rebuilds the mod once):

- **Coordinates:** track.glb meters (x, z), the frame `tools/diorama_map.py` draws with a grid (`--overlay
  diorama.glb` draws the built diorama over the track to check placement).
- **Island:** the route widened by `outline.margin` each side with its loops filled, notches narrower than 2 x
  `closing` closed, plus the `add` shapes (the bay, the Palace of Fine Arts' corner), simplified and rounded: it
  follows the course, with little ground off it. Ground: the designed height sampled on a `grid` whose cells are
  split along a diagonal into two flat triangles; each is clipped to the island and cut along the convex `areas`
  (grass, water; a later one drawn over an earlier one), and every piece takes its heights from its triangle, so
  pieces meet without cracks however their corners fall (cutting pieces and then reading each corner off the smooth
  height left T-junction seams). The base, `base_depth` deep, hangs from the ground's own rim edges (so wall tops meet
  the ground exactly), water-blue where the bay meets the edge, earth elsewhere: a 16 x 16 field of browns blended up
  to 64 x 64, one tile per `side_tile` (380 x 140 m), as TRK4O2's 16 x 16 earth tile spans about 380 x 140 units.
- **Height:** near the route the ground takes the route's height with its rises `height_scale` times the track's
  (2.2: the arc's freeway reads as a ridge), easing to `ground` between `falloff_near` and `falloff_far`; `hills`
  (x, z, radius, height) are added only a grid cell or more away from the route, so the band never sinks under them.
- **Route:** the AI path's middle thinned (`step`, `turn`), corner-cut `smooth` times, a flat red strip `width` wide
  (as the N64 ones), lifted to clear the ground sampled along each segment; the ground near it follows the strip's own
  line. Measured on the seven N64 dioramas, the red line is 1.6-1.9% of the long side wide (TRK6/7 thinner) and 2.4-3.9
  long sides long; Chinatown's course is 4.3 long sides long, so its band is 36 m (1.8%) and smoothed, which keeps it
  reading like theirs rather than a long thin line.
- **Buildings:** boxes (`size` x `building_scale`, `height`, `angle`, `wall` color, flat `roof_color` or a hipped
  `roof` of red, green or grey tiles), `tiers` for setbacks and pagodas. **Landmarks:** `pyramid` (Transamerica),
  `rotunda` and `colonnade` (Palace of Fine Arts), `bridge` (the Bay Bridge, over the bay). **Trees:** six-sided bushes.
- **Textures:** painted in code (window grids, city-from-above ground, grass, earth, water, tiles), 64 x 64, nothing
  from the disc but the route's line.
- **Checks:** the build log lists buildings closer to the route's middle than half the band, buildings whose
  footprint or eaves reach past the island's edge, and trees off it. Keep it under 4,000 triangles (the custom-track converter's warning; the N64 ones have 740-2,400
  vertices): about 3,650 now.

## Texture coordinates

The PS1 point-samples: a face whose coordinates span texels 0 to 4 shows texels 0-3 whole. Rush 2 filters, so a
corner on a texel edge blends in the texel past it, and a narrow strip stretched over a big face showed streaks of the
next rows (the City Bus's roof is v 0-4 of BBUS: solid paint, with grey rows 5 and 6 under it). `ps1::texel_inset`
(include/sfrush_ps1.h) moves each corner half a texel toward its face's center (at most halfway there), so the corners
sample the PS1's outermost texels at their centers. It applies to the cars, the track's faces and its props.
- A first install moves the mod to the end of the mod order (`set_mod_index`): new mods go first and new cars take
  the mod car slots in mod order, so otherwise the arcade cars' slots (57-59) would move and saves picking them would
  get other cars. The mod.json keeps the disc's path and a key (data track name, size, time, logo, build); the
  launch refresh rebuilds a stale one from the disc. Test aid: `RUSH2_TEST_PS1_INSTALL=<disc>`.

## Sky

The PS1 has no sky model or texture (`ART/CLOUD64.GFX`, its one cloud texture, is the smoke and dust puffs'
(`func_8003F188`), shaded by distance through 32 CLUT rows at 0x3B0). **[V]**

- **Background:** `func_8003DBCC` builds screen quads in each frame's packet (gp+0xBCC, 0x800B329C: flat quads at
  +0x18 and +0x30, gouraud quads at +0x48 and +0x6C) and `func_8002FCB0` places and colors them each frame: the first
  gouraud quad from the top of the screen (grey 128: gp+0x844, 0x810, 0x7FC, 64 at night) down to the horizon line
  (0x80107768, 0x80107778) at 250 (gp+0xCAC, 0xC8C, 0xC88, all 250 from 0x80015FC4; night 0, 0, 25), and the second
  250 under it. A grey sky whitening to a haze at the horizon, the haze below it. **[V]**
- **Field of view:** `func_8002C208(320, 240, 250)` sets a 320 x 240 screen at projection distance 250 (GTE H), so the
  top of the screen is atan(120 / 250) = 25.6 degrees over a level horizon. **[V]**
- **Fog:** `clut_upload_shaded` (0x80014F38) builds 31 shaded CLUT rows per texture, each channel stepped up by one per
  row to 26 of 31 (day; gp+0x98 set: down to 1, night), so distant scenery fades to 214 grey. **[V]**
- **Into Rush 2:** Rush 2's cloud dome (`sky = "clouds"`, `cloud_scroll = 0.2` as on most Rush 2 tracks and the N64
  SF Rush ones), as the other converted tracks have; `fog_color` #D6D6D6, the PS1's fog grey. A dome shaded as the PS1's
  gradient was tried and looked poor in Rush 2.

## Props

`func_80036C40` (track_load_geometry) loads track 4's knock-over models, `T4KNOCK/CONE.PSV` at a1 = 0x380 (its
coordinates x 1.4 ft: a 5.6 ft cone) turned 90 degrees about vertical, `T4KNOCK/TRAM.PSV` at 0x280 (x 1 ft: a 30 ft
cable car) and `T2KNOCK/T2K003.PSV`, into gp+0xD60, 0xD64 and 0xD6C. **[V]**

- **Placement:** `func_800155F8` (props_place) fills the prop array 0x80108810 (0x2C each: state, kind, position,
  rotation, model, the track's texture table) from a table of 16-byte records, x, y, height (0.1 ft, the AI path's
  units: a model's origin) and kind (0 cone, 1 tram, 2 T2K003), ended by -1: 0x800A55B4 for track 2, 0x800A58B4 for
  track 4 (12 cones in two rows, 4 trams). **[V]**
- **Textures:** a prop face's texture is a texture table entry (0x800A77C0) and its flags >> 24 the CLUT, an index of
  0x800A7B08, as a scenery face's clut byte is. **[V]**
- **Behavior:** `func_80041244` (props_update) knocks a cone away when a car hits it. On track 4 each tram runs back
  and forth along x between 6397 and 22115 at 24 a frame (state 1 up, 2 down, turning at either end; two start at
  each end), and wrecks (car +0x352 bit 0x8000) a car within 23.2 ft along and 9.6 ft across it. **[V]**
- **Into Rush 2:** cones are `PROP_KNOCK_cone_*` (knocked flying as Rush 2049's cones are), trams `MOVE_tram_*`
  pingpong movers from the green light, a leg 1572 ft in 26.2 s (24 a frame at the 25 frames a second the car
  conversion implies: [Car physics](#car-physics)), each with a `COL_WALL_` box of its body, which pushes cars (Rush
  2 has no wreck-on-touch surface; a hard hit still wrecks by Rush 2's crash threshold). The models use the track's
  axes (a prop isn't turned), which mirror the cars' mapping, so their faces are wound the other way round.
- **The second table** 0x800A59C4 (71 records, the fourth field a track 0-3, only when gp+0x3C is set; kinds 8 and 9
  spin) is the PS1's hidden pickups. **[I]** Not ported: mods carry no collectibles.

## Snow and night

The PS1 races every track (1-4) in three conditions over the same road, scenery and collision: day, snow and night.
gp+0x40 (0x800B2710) == 1 is snow, gp+0x98 (0x800B2768) set is night. **[V]** No falling snow or other weather
effect: the only strings with "snow" are the menu's `TITLETGA/SNOW.GFX`. **[V]**

- **Textures:** the page and CLUT loaders of every track read `T<n>PAGES/T<n>CLUTS` (snow) or `T<n>PAGEN/T<n>CLUTN`
  (night). Snow: snow on grass, roofs, walls and trees, the road kept; the CLUTs' median brightness is the day's
  (x 1.00), channels x 1.08, 1.09, 1.27 (whiter, bluer). Night: lit windows, median brightness x 0.30, channels x 0.49,
  0.49, 0.67 (bluer). The page loaders (`func_80039A88` for track 4) only pick the file by the flags (night first,
  then snow); the uploads after are the day's, the same RECTs. Every chunk is the day's size except track 4's night
  page chunk 28 (64 x 28 texels against its RECT's 64 x 32): the loader reads the whole .DAT into one buffer, steps
  through it by the .SIZ's sizes and uploads 64 x 32 from the chunk whatever its size, so that RECT's last 4 rows
  come from the start of chunk 29. **[V]**
- **Night-only and day-only chunks:** the visible-chunk lists `track_draw_chunks` walks (static, in RUSH.EXE around
  0x8009C86C: u32 chunk id | flags, ended by -1) carry flags: 0x4000 = not drawn at night, 0x2000 = drawn only at
  night, 0x1000 = semi-transparent (gp+0x310). The lists come from per-track section tables (32-byte records, the
  two list pointers at +0x18 and +0x1C: track 1 0x8009DEF4, 2 0x800A0180, 3 0x800A2798, 4 0x800A4188). Track 1
  (Golden Gate): ids 112 and 114 day-only; track 2 (Embarcadero): 132 and 133 night-only (0x3000: night-only,
  semi-transparent); track 3 (Market): none; track 4 (Chinatown): 89 day-only. `track_draw_chunks` also draws a
  few ids semi-transparent by number whatever their flags: track 1 0x70 and 0x72, track 2 0x82 and 0x83, track 4
  0x59. **[V]**
- **Night fog and sky:** `clut_upload_shaded` steps the fog rows down to 1 instead of up to 26 (distant scenery fades
  to black, 8, 8, 8, against the day's 214 grey); `sky_draw` makes the sky grey 64 at the top and (0, 0, 25) at the
  horizon. Snow keeps the day's fog and sky. **[V]**
- **Night cars:** the paint pages come from `CARSGFXN` (`car_paint_load`), the body textures (`<tex>01.GFX`) too, and
  `EXTRA/WHEELSN.GFX` for the wheels (`func_8003CF20`). Paint ramps go to 0.25-0.35 of the day's (BLUE 168 -> 88 in
  blue, RED 192 -> 64, WHITE 208 -> 56), while CLUT entry 7 (white, 248) stays: the light lenses keep full
  brightness. **[V]**
- **Car lights** (`track_draw_chunks` after each car): `func_80041C58` draws a glow sprite at a point of the car (car
  frame, 0.1 ft: x across, y down, z ahead) with a size. **[I]** (the sprite: `ART/LIGHT*.GFX`, loaded by
  `func_8003CF20`.)
  - Tail lights, day and night, at (±x, y, z) of 0x800A8FA8 (12 bytes per car index: car +0x4C, 0-7 the SF Rush cars;
    8-15 the special cars **[I]**; BMW (26, -8, -82)), size car +0x90 (0x80101278): +32 a frame while car +0x3E0 is set
    (braking **[I]**), to 128, else -8 a frame to 0; +63 at night, so they always glow then. **[V]**
  - Headlights, night only, at (±x, y, z) of 0x800A8EE8 (BMW (24, -6, 82): 2.4 ft out, 8.2 ft ahead), size 0xC0.
    **[V]**
  - Each light goes out when its corner's damage (car +0x352, 3 bits per corner: rear bits 0-2 and 3-5, front right
    6-8, front left 9-11) reaches 3. **[I]** (which bits are which corner)
- **Snow physics** (`car_integrate`): every car's horizontal forces for the frame (thrust, tire friction; 0x800ED0FC
  and +8; the vertical one is kept) are halved at speed (car +0x30) 151 and up and halved again at 301 and up (a
  quarter). With the UFO picked (special car 8) every car's are halved instead. PIGGY's own taper and the other rule
  of the dry path are skipped. Against the BMW's top speed (768) that is 20% and 39%: the top speed is kept (thrust and
  friction scale together), but above 39% of it a car accelerates, brakes and grips a quarter as hard. **[V]**
- **Snow race times:** `checkpoint_passed` adds 8 s more at each gate (gp+0x40, [Race times](#race-times)), and the
  race starts with 60 s on the clock on every track (0x80015E04; the day and night starts: track 1 38 s at
  0x80015DF0, track 2 38 s at 0x80015DCC, track 3 60 s at 0x80015DA8, track 4 45 s at 0x80015D80). **[V]**
- **Into Rush 2** (`src/sfrush_ps1/ps1_track.cpp` `tracks` and `conditions`): the converter makes track 4 three
  times, custom tracks `chinatown` (DAY), `chinatown_snow` (SNOW) and `chinatown_night` (NIGHT), the last two
  versions of the first (`variant_of`, docs/custom_tracks.md#versions), each from its own page and CLUT set (the
  night set's chunk 28 read through into chunk 29 as above). Tracks 1-3 are the PS1's own models of N64 courses:
  only their SNOW and NIGHT are made (`golden_gate_snow` / `_night`, `embarcadero_*`, `market_*`), versions of the
  N64 SF Rush course (`variant_of = "sfrush:golden_gate"` and so on), so the track select shows the course's diorama
  with the version's band. Per track the converter reads its own loaders (page / CLUT: track 1 0x80037C2C /
  0x8003A4A0, track 2 0x80038644 / 0x8003AF18, track 3 0x80039048 / 0x8003B990; track 2's CLUT loader uploads 50 of
  its 52 chunks, the last two unused), texture table (0x800A7478 for track index 1, 0x800A77C0 for the others),
  banner instructions (track 1 0x80044524 / 0x80044534, track 2 0x8004459C / 0x800445AC, track 3 0x800445DC /
  0x800445EC), gate tables (counts / sectors / first / times: track 1 0x800B2AF8 / 0x800AA388 / 0x800B2B00 /
  0x800B2AD8, track 2 0x800B2B08 / 0x800AA394 / 0x800B2B10 / 0x800B2AE0, track 3 0x800AA3A0 / 0x800AA3BC /
  0x800AA3CC / 0x800AA37C) and start time; the bonus instruction 0x800542D8 is track 4's only. Track 2's knock-over
  props (table 0x800A55B4: T2K001 cone at scale 0x380, T2K002 traffic light and T2K003 tree at 0x280, both 6.4 ft up
  and the tree upside down, all turned 90 degrees as `model_prepare` 0x8003E714 does) become `PROP_KNOCK_` props. Snow: the race times 8 s more at every gate (read from 0x80054358), and
  `[handling] traction = [[32.1, 0.5], [64.1, 0.25]]` (speeds 151 and 301 from 0x8005D204 / 0x8005D240 in mph at the
  car conversion's 25 frames a second: `src/track_traction.cpp` scales every car's across and ahead forces, gravity's
  share kept, before car_linear_accel_stage_80069A54 turns them into accelerations: car +0x1C..+0x24 = the car-frame force sum +0x4..+0xC x 0x80110018 / the weight +0x5AC). Checked: over 23 s of the same start, the
  drones covered 0.76 of their night (full traction) distance with no wrecks, against the 0.75 the PS1's 8 s per gate
  allows for. The drones' target speeds take the traction into account (docs/custom_tracks.md#handling): without
  it they reached Market's and Golden Gate's tight corners at their dry speed and wrecked there (7 and 3 wrecks in
  60 s, none at night); with it none of the four tracks had a wreck, the drones at 0.58-0.71 of their night pace.
  Scaling the tires' grip alone (the first version) left the engine and drag whole: cars reached full
  speed and slid off (0.58, two drones wrecking over and over). Night: fog #080808, a `SKY` dome colored as the PS1's
  gradient (grey 64 at 25.6 degrees and up to (0, 0, 25) at the horizon), `car_light` the night car pages' ratio to
  the day's over every car texture both sets have (entries 1-6: #454046) and `car_lamps` (src/car_lamps.cpp: the glows
  above for every car, the PS1 cars at the PS1's points through their .json's `lamps`). The diorama, logo and picture
  are the day track's.
- **Not a texture:** `ART/T<n>P08.GFX` / `P08N.GFX` (VRAM (384, 32), 64 x 128) loads only while gp+0x3C (the hidden
  pickups) is set: the pickups' page, which mods don't carry. **[V]**

## Music

The disc's four CD audio tracks (tracks 2-5, 16-bit stereo at 44.1 kHz) are the game's music. The port plays them as
SF Rush songs (src/music_streams.cpp: `ps1::audio_tracks` finds them through the .cue, a .ccd or a CHD and
`ps1::read_audio` reads them; nothing is converted or copied into the mod). Like SF Rush's N64 songs they belong to no
track: an SF Rush N64 track's Original is a random one of the N64 and PlayStation songs, Chinatown's too. They are
listed in the Sound tab under SF Rush > PlayStation, with a switch for the source while SF Rush has more than one
installed. Measured -8.4 LUFS against Rush 2's -16.1, they are turned down 7.7 dB (`ps1_music_compensation`). The
songs are taken to be the PlayStation version's own (not checked against the N64 songs, which only play as sequences
in the game). **[I]**

## Not ported

- The hidden pickups (0x800A59C4, [Props](#props)): mods carry no collectibles.
- The second road of track 1 (`T5ROAD2`, `COLLDAT5`): a variant of a course the N64 game has.
