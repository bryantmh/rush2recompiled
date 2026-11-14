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
- [Unlisted chunks](#unlisted-chunks)
- [Far stand-ins](#far-stand-ins)
- [Embarcadero overpass](#embarcadero-overpass)
- [Embarcadero upper deck](#embarcadero-upper-deck)
- [Race times](#race-times)
- [Run-in](#run-in)
- [Cars](#cars)
- [Wheels](#wheels)
- [Car physics](#car-physics)
- [Sky](#sky)
- [Sea](#sea)
- [Props](#props)
- [Keys](#keys)
- [Unlocks](#unlocks)
- [Game modes](#game-modes): [Explosive mode](#explosive-mode), [Time Sprint](#time-sprint)
- [What the converter makes](#what-the-converter-makes)
- [Track select diorama](#track-select-diorama)
- [Snow and night dioramas](#snow-and-night-dioramas)
- [Snow and night](#snow-and-night)
- [Stunt track](#stunt-track)
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
  matches none of N64 tracks 4-7 nor the arcade's. `T1ROAD/T5ROAD2.DAT` + `COLLDAT5` are Golden Gate's stunt road
  (track 1's scenery and textures with ramps on the road), not a separate course: [Stunt track](#stunt-track). **[V]**
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
  copies below it, each a step brighter or darker (distance shading). Row 0 is the original. **[V]** Track 2's
  loader also calls `func_80015090` (`clut_upload_dark`) twice, at (992, 384) and (992, 256): the same upload, but
  every copy a step darker whatever the time of day, and color 0 not kept transparent (the shaded one leaves 0 alone).
  Those are the CLUTs of the tunnel under the climb to checkpoint 1 (road chunks 58-60, the N64's `TRACK2BT1`-`6`) and
  of scenery 42-43. **[V]**
- **Which chunk each call gets:** the loaders read the whole .DAT into one buffer and step a chunk pointer through it
  by the .SIZ sizes (`lw rt, 4k($s1)` adds chunk k's), so a call gets the chunk the pointer is at, not the call's
  number. Every CLUT loader uploads all 52 chunks in order, the page loaders all 34; track 2's puts chunks 10 and 26
  up with `clut_upload_dark` (above). **[V]**
  The converter first took only `clut_upload_shaded`'s calls, call k as chunk k: every Embarcadero CLUT from the 11th
  on got a later chunk's colors (noise on walls, road and trees, grass black, the night CHECKPOINT gantry unreadable,
  the snow one blue), and the tunnel's two CLUTs stayed empty, so the whole tunnel was transparent (a hole in the
  wall at its mouth, user report). It (and `vram.py`) now follow the pointer and count both functions.
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
- **The surface doesn't change the driving.** Its only reader is the wheel ground query `func_80061574` (called per
  wheel from 0x80062B88-0x80062BE8): `(FLAGS >> 3) & 7` of 1 (dirt) or 2 (grass) stores 1, and 3 (water) stores 2,
  in the per-car byte 0x801014E9 + car x 0x408 (cleared at 0x800537E0; 0x80050254 also stores 2). Nothing in the car
  tick reads that byte: its only readers are player 1's, 0x8001659C (copied to 0x801014EA, which nothing reads) and
  0x80021584 (water, 2, only). The grip (`func_8005E34C`) and rolling friction take the car table's values alone. So
  a PS1 car drives dirt and grass exactly like road. **[V]** Every read of the flags byte in RUSH.EXE (the whole
  game's code) is one of: `& 7` (record shape: the ground queries 0x8004D498, 0x8004DA34 and the loops before them),
  `& 0x40` (skip), `(>> 3) & 7` (the three above) and `& 0x80` (0x80061A9C, 0x80061E38, 0x80061EC4 -> 0x801015C4).
  0x801015C4's readers: 0x8005E134 (the gravity a frame while not every wheel is down, `func_8005E0CC`: +200 with it
  set instead of the car table's gp+0x4EC) and 0x80062F04 (clears gp+0xBE8). Neither touches speed along the ground.
  **[V]**
- **Against the N64:** the N64's drag step `func_80077C90` (called from `car_forces_wrapper_80078264`) adds
  -10 x speed (0x800C7D44) while both rear wheels (+0x780, +0x784) are on surface 1 (dirt) and -40 x speed
  (0x800C7D48) on surface 2; Golden Gate's N64 collision has only road and dirt. `tools/lane_surfaces.py` lists the
  ground under each AI lane: on the N64 the lanes are road all lap except the outer two clipping 180-270 ft of dirt at
  30-31% of the lap (the park); on the PS1 the same stretch (31-37%) puts all four lanes on 400-1000 ft of the grass
  class, which the PS1 drives as road. **[V]**
- **The drones' line there:** `tools/lane_compare.py --n64 0 --rush2 <converted Golden Gate>` fits the PS1 lanes onto
  the N64's (18 ft apart on average; 5-15 ft over most of the lap). In the park's left-hand bend (converted x
  1000-2000, z -2850 to -3300) the PS1's two drone lines (PATHDAT/EXTDAT) cut 25-80 ft inside the N64's line, over
  the grass class, while the N64's line lies on the PS1's road. They also zigzag there (85-180 ft radius kinks 20-40 ft
  apart), so the converter's corner speeds through it are 70-115 mph against the N64's 130-145. **[V]**
- **Walls under a road above:** an A wall's face (10 ft under its ground to 40 ft over it) is cut to 3 ft under any
  ground over its line, and only along the stretch that ground covers (the wall is split into pieces, 4 ft samples).
  Ground counts as over the line where one polygon lies 8 ft out on both sides of it (a road whose edge sits on or
  just past the line, Embarcadero's ramp at x -2674 overhanging its wall by 2-5 ft, keeps the wall that stops cars
  driving off it). Testing only ground corners within 30 ft of the wall missed big polygons: Market's street at
  61.7 ft (one 490 x 103 ft polygon, x -440 to 50, z -4988 to -4883) over a 30 ft wide lower level (29-44 ft, x
  -131.5 to -161.5), whose two side walls rose to 84 ft through the street, an invisible wall across all four lanes
  61% of the way round the lap (snow and night; user report 2026-11-10). The same cut now also applies to one wall
  each on Chinatown (x 806-846, z 2608, street at 27.5 ft over the -6 ft level) and Golden Gate (x 2710, z -782, a
  slope at -76 ft over the -85 ft level), off the lanes. **[V]** (converter output, AI lanes against wall faces)
- **Steps:** a dead end can be a ground polygon instead of a wall. Track 4's lane beside the street that climbs to the
  big loop (glb x 744-762 m, ending at z -120 m) ends in one road polygon rising 65 ft in 40 ft (59 degrees) to the
  street above, drawn as a rock wall with a chevron sign. A PS1 car doesn't climb it **[I]** (how the PS1 refuses it
  isn't traced); Rush 2 drives up any ground face, so players went straight through the "wall" (user report
  2026-11-09). The converter (`build_condition`, ps1_track.cpp) walls the foot of every ground polygon steeper than
  53 degrees (normal y 0.2-0.6), rising 4 ft or more, with flat ground (normal y > 0.9) just past both its low and
  high edges: one face along the low edge facing downhill, 3 ft under it to 3 ft over the top, the polygon kept as
  ground so a car coming down from above (behind the one-sided wall) still rolls down. Only that polygon qualifies
  on the PS1 disc; Golden Gate's 54-degree banked-curve bands (next to other bands or nothing) stay drivable.
- **Creases:** the PS1 ground is flat planes (above), so its curves are facets, and on tracks 1-3 most floor quads
  are bent quads (two planes: Golden Gate 583 of 859, folded a median 2 degrees; Embarcadero 73 of 501; Market 613
  of 1,426; Chinatown 37 of 922). Every bent quad's split matches both of its record's planes (offline, every quad on
  tracks 1, 3 and 4), and the converted planes meet within 0.05 ft: the data is read right, the creases and the bank
  wobbling from quad to quad are the PS1's. The PS1 hides them by damping its cars' pitch and roll hard every frame
  on the ground (`car_damp_x`, `car_damp_z`, docs/functions/sfrush_ps1_car.md); Rush 2 rides them, and Golden Gate
  rocked and bumped all round, worst in the turns (user report 2026-11-09).
  - **Spline road (Golden Gate):** the converter fits Rush 2's spline road ([Spline road](custom_tracks.md#spline-road))
    to the PS1 floor (`road_splines`, ps1_track.cpp): stations every 40 ft along the drones' line, at each a profile
    across the road with a knot every 12 ft out to 60 ft each side (least squares through the ground every 2 ft,
    lightly held straight where the ground gives out), the center height smoothed along the lap once and the knots'
    heights off it twice ([1 4 6 4 1]). A segment's cross-section is a straight line and the turns are dished (flat in
    the middle, 6-8 ft up toward the edges), so the road is 10 parallel chains of segments, one per strip, meeting at
    the knots. Each segment's frame lies along its chord (the cut takes a segment whose next station's center isn't
    at u 0.5 for a chain's end, and a 12 ft strip's chord in a turn strays to u 0.57 off the station's tangent); the
    surface keeps the station's cross-section (the start line's slant) and tangent (the normal, the cubic's end
    slopes). The arcade converter's cut (`a49::cut_spline_road`, with the strips' bands and every strip of the 4
    stations either side as candidates) makes the footprints, keeping pieces within 1 ft of their segment; kerbs and
    the rest stay flat, with the averaged height of `smooth_height` ([Smooth collision](custom_tracks.md#smooth-collision)).
    Measured in game, a computer car's lap (`tools/test_race.py --motion 1`: pitch / roll jerk, 95th percentile,
    rad/s^2; vertical, g): faces with the averaged height 1.71 / 1.01, 0.70 g; spline road 0.28 / 0.29, 0.51 g, the
    wheels on it 99% of the lap; the N64 Golden Gate 0.74 / 0.11, 0.45 g. On the faces the big pitch and roll changes
    were where the wheels crossed from spline road to flat (26 of 56 such steps), almost none on the spline (11 of 1,244).
  - **Not the others yet:** Market rides smoothly on its faces (1.90 / 0.86, the N64's 2.59 / 4.16); with the spline
    road its jumps' crests and size went wrong (5.08 / 4.37, and at 40 ft its collision passed the game's 64 KB of
    vertex lists). Embarcadero is rough either way (4.82 / 4.42 faces, 6.08 / 6.17 spline, N64 1.45 / 0.37): the car
    shudders side to side every 5 ft along the waterfront straight (2,900-4,300 ft into the lap), which isn't the
    creases, and the strips left that straight flat.
  - **Tried first:** smooth collision's tilt blending alone (its corner matching also misses most of these corners:
    each polygon's corners are rebuilt through its s16 matrix, and only 27% land in the same 1/32 ft cell as their
    neighbor's); California Speed's crease fillets (on triangle fans with obtuse corners each triangle rounding only its
    own edges stepped the height by up to 0.8 ft: kicks over 30 g 3 -> 58); the height averaged over a disc round the
    wheel (`smooth_height`, 8-64 ft: a wider disc can't take out a wobble that is in the road's heights, and past
    32 ft it got worse). A single chain with a straight cross-section left the dished turns 1-8 ft off it and flat,
    stepping where spline met flat.

## AI paths

`COLLDAT2/PATHDAT<n>.DAT` and `EXTDAT<n>.DAT` (forward), `REVPATH<n>` and `EXTRDAT<n>` (backward), loaded by
`func_80055678` (pointers gp+0xE38 and gp+0xBA0, count gp+0x454). u32 count (low byte), then 20-byte records: s32 x,
s32 y, s32 speed (?), s32 heading (`& 0xFFF`, 4096 a turn), s32 height (y down); 0.1 ft. **[V]**

- The two files are the two edges of the drones' corridor: a drone aims at PATH + (EXT - PATH) x w / 16, w its own
  byte at 0x800B2B38 (`func_800560B0` area). Where they meet the corridor is one line. **[V]**
- Reset points take x, height and y from a record (`0x80055E78`). **[V]**
- The points are about 200 ft apart; the drones drive straight chords between them. **[V]**
- The third field (235-898, high on straights) isn't read near the path code; not ported. **[I]** a speed.

## AI lanes

What the converter writes as `AIPATH_main` and `AIPATH_lane1-4` (`build_condition` in ps1_track.cpp):

- **Golden Gate, Embarcadero and Market, with the N64 ROM in** (the Games tab's SF Rush ROM): the N64's four forward
  lanes (assets 58-60, read by `include/track1_lanes.h` as the N64 port reads them). `n64_lane_offsets` fits them
  onto the PS1 drones' middle line in 2D (trimmed ICP from the eight axis-aligned starts). The fits come out as the
  90-degree turn and mirror between the two games' axes, 12-20 ft apart on average. Golden Gate and Market are offset
  0-13 ft; Embarcadero sits (1260, -304) ft off. Each lane is then placed where the N64 lane crosses each station's
  section; every lane crosses 100% of the stations. Any short gap (up to 25 stations) is bridged, and a longer one
  keeps the PS1 lane, blended in over 15 stations. The N64's lines lie 97-99.8% on the PS1's road; the PS1 drones'
  own lines cut 25-80 ft inside them through Golden Gate's park (over the grass shoulders) and zigzag
  ([Collision](#collision)). With them the drones run the N64's line, within 3-8 ft of it
  (`tools/lane_compare.py`).
- **Chinatown, Embarcadero's snow, or without the N64 ROM:** the PS1's lines (the lerp of PATHDAT/EXTDAT,
  [AI paths](#ai-paths)), smoothed (`smooth_lane`: a 5-station moving average, 3 passes) so the kinks don't read as
  corners, except where the smoothed lane would come within 6 ft of a wall or cross one: there the raw line stays,
  blended back over 6 stations (smoothing cut Chinatown's turn round a wall end, 12 of 21 night wrecks a race).
- **Walls: hand fixes per course** (`lane_fixes`, ps1_track.cpp). The N64's lines don't fit the PS1's road
  everywhere (12-20 ft apart on average), and in places they cut a corner through a PS1 wall or run over its pavement:
  Embarcadero's first left-hander, 1650 ft in, had all four lanes through the corner's wall and every car scraping it.
  Each fix moves the four lanes together sideways at one spot (the station nearest a point on the road, or a given
  station where two passes of the lap lie together), by a given number of feet toward that point, at full strength
  over a few stations and eased out over 12 or so either side; nothing else moves. The spots came
  from `tools/lane_surfaces.py --rush2 OUT --walls 3` (where a lane runs within 3 ft of a wall at the car's height, or
  crosses one) and `tools/drone_pace.py RUN.r2rc --hits` (where the drones hit something: jolts over 8 g, grouped),
  checked on the `--plot` maps: 5 on Golden Gate (3 for its run-in merge), 7 on Embarcadero (6 night, 1 snow), 8 on Market, 1 on Chinatown.
  After them every lane keeps 3 ft from every wall but 10 ft of Market at 2.7 ft. They replaced an automatic nudge
  (any lane point within 5 ft of a wall, 8 in snow, pushed away from it): it moved the lines everywhere near a wall,
  and where a lane was already past one it pushed it further out or not at all.
  `lane_walls` (the PS1's walls and the converter's step walls) and `nearest_wall` remain for the smoothing above and
  the stunt road's lanes ([Stunt lanes](#stunt-lanes)).
- **`AIPATH_main`** is the middle of the four final lanes, since the converter takes the target speeds from it alone
  (`target_speeds` in track_mod_convert.cpp). It is as wide as the PS1's corridor, or wider where the lanes spread
  further.
- **Gates** reach every lane's crossing with 15 ft to spare: a drone on a lane outside a gate never counts it
  (Embarcadero's snow lanes ran 68-73 ft from its overpass gate's center, 60 ft wide: 5 of 7 drones never counted a
  lap).
- **Golden Gate's run-in:** the path ends 27-47 ft beside where it rejoins its loop at the toll plaza. The converter
  continues from the loop start's crossing, so that sideways step read as a hairpin and the plaza's straight targeted
  70 mph. The lanes' last stations ease onto the lanes they rejoin. The lap's end (the lower road) and the run-in (the
  upper) are split by a gap until game (-126, -985), so the ease starts just past its tip; eased from 30 stations
  back, every lane crossed the gap and every car hit its walls. Lane fixes keep both roads' lanes to the gap's side up
  to its tip and put the loop's first lanes (station 320, beside the path's end) on the lap end's line, drifting back
  by station 335 (clear of the joined road's narrowing at game (126, -1000)), so the lap's end runs nearly straight on
  and the run-in takes the step, once a race. Merged only in the 120 ft past the tip, the S-bend's target in snow was
  77-80 mph (snow 112 mph a lap against 115).
- **Target speeds:** with the N64's lines, the N64 lanes' own speeds (as the N64 port converts them, `lane_mph`), as
  each lane's `_SPEED` and their mean as `AIPATH_main`'s. Ranking corners by radius against the N64's speed
  quantiles (`[ai] speed_quantiles`) put Golden Gate's park bend at 69-100 mph where the N64 asks 115-158: the
  drones followed their targets there and averaged 125 mph a lap against the N64's 141. Chinatown, or no N64 ROM:
  the converter's radius ranks with the quantiles (`ai_speed_toml`).
- **Snow** (`snow_tuning`, tuned by hand per course): the recomp's snow costs half what the PS1's does (one traction
  halving of its two), so the drones aim for half the PS1's slowdown, on the faster side. The PS1's own snow clock (8 s
  more a gate) gives snow / night lap shares of 0.64 Golden Gate, 0.66 Embarcadero, 0.63 Market and 0.76 Chinatown, so
  0.82, 0.83, 0.82 and 0.88. The snow's physics allows less on the tighter courses: the race time model's best lap
  with the traction (`tools/race_time_test.bat` on each course's night path) is 0.79, 0.79, 0.74 and 0.76 of the dry
  one, and drones pushed past it wrecked (Market at 82 mph: 34 wrecks a race). Per course: a scale on the target
  speeds (the N64 lanes' `_SPEED`, or `[ai] speed_scale` on the converter's own), `[ai] traction_margin` (the share
  of the snow's grip the corner speeds aim for, which also caps authored speeds), whether the N64's lines are kept
  (Embarcadero's snow drones rolled and slid off them, 21-25 wrecks a race against 1 on the PS1's own). Snow drones hold only 31-36 ft/s^2 sideways at full
  lock (night 67-69), and past it they fishtail or slide wide; wreck counts swing with small changes, so the tuning
  aims at pace with few wrecks rather than none.
- **Measured** (`tools/drone_pace.py --race`, 3 laps, Drone Cheating off; average speed, wrecks of the 7 drones):

  | Course | N64 | PS1 night | PS1 snow | snow / night (aim; race time model) |
  |---|---|---|---|---|
  | Golden Gate | 140 mph, 0 | 139 mph, 2 | 115 mph, 0 | 0.83 (0.82; model 0.79) |
  | Embarcadero | 132 mph, 0 | 130 mph, 2 | 100 mph, 2 | 0.77 (0.83; model 0.79) |
  | Market | 105 mph, 6 | 108 mph, 0 | 80 mph, 12 | 0.74 (0.82; model 0.74) |
  | Chinatown | (none) | 109 mph, 9 | 91 mph, 0 | 0.83 (0.88; model 0.76) |

  With the lane fixes (2026-11-10): Embarcadero night went from 12 wrecks a race to 2, Market night from 9 to 0.
  Market's snow wrecks swing between races (9-13 over the last runs, as before the fixes); Chinatown night's are cars
  sliding 60-70 ft wide of the lanes, or hit 42 ft from any wall.

  Before (the PS1's lines, the stock radius table): Golden Gate night 122 mph against the N64's 141; Chinatown night's
  remaining wrecks are cars stopping dead on a straight 24-55 ft from any wall, **[I]** its trams.
- **The mod's key** includes the N64 lane files, so adding the N64 ROM later rebuilds the PS1 mod once at the next
  launch (`source_key`, ps1_install.cpp).
- **Offline:** `tools/sfrush_ps1/cpp_test/build.bat`, with RUSH1_LANES_DIR set to a folder holding lanes58-60.bin.

## Checkpoints

`COLLDAT<n>/TRACK<n>.CPF` (loaded to 0x80113490 by the track's loader, `func_8004FBFC` for track 4): one plane a gate
of s32 c0, c1, c2, d (16384 = 1; c0 x + c1 height + c2 y against d, 0.1 ft): 5 on Golden Gate, 6 on Embarcadero, 9
on Market, 4 on Chinatown. Plane 0 is the finish line. **[V]**

- **Sectors:** `func_8004FBFC` also loads `COLLDAT<n>/BOUNDS.DAT` to 0x800D8168 (49 records on track 4, the count in
  gp+0xE90): s32 x min, x max, y min, y max (0.1 ft). `func_800554F0` (car_sector) gives a car the last box that holds
  it (car +0x2FC). While 0x800B2740 (a mode index, 0-3, set by the front end `func_8001ED30`) is 1 it then gives box
  0x37 as 0x2B and box 0x41 as 0x20 (0x800555E4-0x80055664); no gate lists any of the four. **[V]**
- **The test:** `func_80054630` (checkpoint_test), each car each frame: a car at gate k (car +0x300) tests plane k
  only while its sector is one of gate k's, from per-track lists (track 4: counts 0x800B2B18, sectors 0x800B2B1C,
  first index 0x800AA3D8; one sector a gate: 1, 9, 19, 21; track 1: 0x800B2AF8, 0x800AA388, 0x800B2B00; track 2:
  0x800B2B08, 0x800AA394, 0x800B2B10; track 3: 0x800AA3A0, 0x800AA3AC or 0x800AA3BC, 0x800AA3CC). With 0x800B271C =
  1 (gp+0x4C; the AI path PATHDAT's direction: it crosses gates 1, 2, 3... going from n . p > d to below) n . p < d
  passes the gate (+1; Golden Gate wraps 5 to 1, Embarcadero 6 to 0, Market 9 to 0, Chinatown 4 to 0; the lap counts,
  car +0x68, on passing gate 0, Golden Gate's gate 3); otherwise they count down crossing the other way. Market's two
  sector lists differ at gate 0 only (box 0x00 against 0x3D). `func_80054130` is the pass event. **[V]**
- **Market's gate 5** (box 0x27, its only one) tests plane 5's n . p against 0x4BC8000 instead (0x800548F4-0x80054A34;
  passed when over it with 0x800B271C = 1, else when not over 0x4BC7FFF): the line y = 485 ft, passed the other way,
  not CPF's y = -6.3 ft. A rule after it (0x80054B8C) passes gate 7 in box 0x45 on Market whatever the plane, but it
  runs only inside gate 7's own boxes, which are 0x34 alone: it never fires. **[V]**
- **No overflow:** the PS1's n . p is 32-bit (`mult`/`mflo`, `addu`); over every gate's boxes the sum stays under
  1.2 x 10^9, so it is the plain product. **[V]**
- **Into Rush 2:** every plane is a gate (until 2026-11-13 only planes 0-3 were: Embarcadero's gates 4-5 and Market's
  4-8, each with its CHECKPOINT banner, counted nothing; GitHub issue 68). Each goes where the AI path crosses its
  plane (from n . p > d to below) inside one of its sectors, the finish nearest the path's first point and each
  checkpoint the first such crossing after the gate before it (path points 0, 279, 613 and 724 of 1121 on track 4).
  Market's gate 5 uses its real line (above). Golden Gate's lap is toll plaza (gate 0, a checkpoint: [Run-in](#run-in)),
  loop start, gates 1 and 2, FINISH at gate 3, gate 4.
- **How wide:** Rush 2 counts a gate when a car crosses its line within the marker's half width of it
  (`func_800A1468`), so each marker spans the whole road there. From the crossing the plane's line is walked 1 ft at
  a time each way over the floor (road, dirt, and grass up to 40 ft past the last road; cracks up to 8 ft stepped
  over; each step within 4 ft of the last one's height; a written COL_WALL face standing 1-4 ft over the floor ends
  it), but never out of the gate's own boxes (at a crossroads the line runs on down the cross street). The road
  reached furthest each way, 15 ft to spare, is the marker's end; the AI lanes are on it too. Before, a marker spanned
  its box or the lanes where the path crosses, and a car near the edge of a wider road missed it and every gate after
  it. `tools/gate_shortcuts.py` on every version finds no floor beside a gate left out of it and no way round one;
  what it lists inside a radius is the same road banked (Golden Gate) or, under Embarcadero's overpass (gate 1), the
  street 92 ft below, which the PS1's flat boxes count too.

## Start and finish banner

The gantry over the start line is two scenery chunks with the same 217 vertices: SCN2 chunk 1's two panels read
CHECKPOINT (red, page (384, 0)) and chunk 89's read FINISH (texture entry 3, u 0-127; faces 15, 19, 99, 103). The
START texture beside FINISH on that page isn't on the gantry. **[V]** (rendered from the converted chunks)

- **The swap:** the scenery draw loop in `func_80042FF0` takes chunk id s2 from the visible list and its pointer from
  the table 0x800B5C88 (ids from 1, so id n is SCN2 chunk n - 1). On track 4 (0x800B34F8 = 3), when gp+0x20 is set
  and the id is 2 (0x80044628), it draws the chunk at 0x800B5DF0 (id 90) instead (0x80044638). Tracks 2 and 3 have
  their own pairs (ids 0x5F -> 0x800B5EA4, 0x95 -> 0x800B5F44), track 1 0x4E -> 0x800B5E7C. Track 2 also draws id
  131 (0x800B5E94) in place of 130 (0x82) once gp+0x88 is set, a one-time trigger a car trips (0x800171EC: a box
  test on the cars' positions, then a sound); the converter keeps 130, the start of the race. **[V]**
- **gp+0x20 (0x800B26F0):** set per view before each player's scenery pass (0x80016468 player 1, 0x80016884 player
  2) when that player's laps still to go (0x80101250 / 0x80101658, car stride 0x408; laps 0x800B2888 - 1 at race
  setup 0x80015EE0) are 0 or fewer: the viewing player's own last lap. **[V]**
- **Into Rush 2:** Rush 2's own finish banner swap (track_finish_line_setup_8008F080: CHKPNT's texture-load list for
  FINISH's in `<prefix>FINISH`, when a player passes the last checkpoint of the last lap). Chunk 1 is the custom track's `FINISH_BANNER`
  ([Finish banner](custom_tracks.md#finish-banner)); its four panel faces (the ones chunk 89 retextures) are the
  CHKPNT material, the CHECKPOINT strip resampled corner for corner onto FINISH's 128 x 32 rectangle, and FINISH is
  that rectangle of chunk 89's page. Chunk 89 isn't drawn. Rush 2's swap is for the whole screen, so in split screen the first player there turns it for everyone.

## Unlisted chunks

`track_draw_chunks` (`func_80042FF0`) only walks the section lists (the per-track 32-byte records, lists at +0x18 and
+0x1C, [Snow and night](#snow-and-night)), and nothing else reads the chunk table 0x800B5C88 but the loader
(`func_80036C40`) and the swaps above. So a scenery chunk no list names is never drawn. **[V]**

| Track | Unlisted ids (SCN2 chunk = id - 1) |
|---|---|
| 1 Golden Gate | 97-100 (one chevron sign each, 100 ft over the cliff road), 115, 116, 125 (the FINISH swap) |
| 2 Embarcadero | 96, 103-105, 110-114, 131 (the gp+0x88 swap), 134, 135 (the FINISH swap) |
| 3 Market | 174, 175 (the FINISH swap) |
| 4 Chinatown | 58 (an elevated rail section on pillars), 90 (the FINISH swap) |

The converter drew every chunk in the file, so Golden Gate showed the four chevrons floating in the sky (user
report). It now leaves out every unlisted chunk but the finish banners, which [the swap](#start-and-finish-banner)
handles (`ps1_track.cpp`, `listed`).

## Far stand-ins

The PS1's short draw distance has a few listed chunks stand in for a far model; with everything drawn at full
distance they sit in front of the real one, so the converter leaves them out:

- Golden Gate's id 91 (SCN2 chunk 90, x 653-824 m, z -94 to 87 m in track.glb): a flat card with the bridge painted
  on it, 74 m tall, standing over the bay where the N64 has hillside (`TRACK1GG4`). Only sections 33-37 and 52 list it:
  the cliff road, from where the bridge itself is out of range. **[V]**

`tools/sfrush_ps1/n64_census.py` (align a conversion with the N64 dump, list what either lacks) and `census_shots.py`
(matching N64 and PS1 renders of a spot) found it. Their renders are mirrored against the game (flip them).

## Embarcadero overpass

With the N64 SF Rush ROM in (Games tab), the front of the PS1 Embarcadero's waterfront freeway overpass is the N64's:
the install converts the N64 Embarcadero (`track1::convert_track`, then `catalog::read_container` and
`for_each_placed_triangle`, `ps1_install.cpp` `n64_embarcadero`) and `ps1_track.cpp` (`n64_overpass`) takes every N64
course triangle over the freeway's footprint (x 1201-1245 m, z -1843 to -1283 m in track.glb) with a corner over 8 m
(mesh `PS1_N64_OVERPASS`, N64 textures and vertex shades, both sides drawn, at night darkened by the PS1's night light
`night_light`). The PS1's pieces of that stretch go: roads 26, 27 and 70, scenery 42, and every other face over 9 m in
the footprint (roads 25 and 28's abutment walls); the curbs and signs at the road stay. **[V]** (offline renders
against the N64 dump and the PS1-only build, `tools/glb_view.py`)

- **Back end:** the PS1's own back end stays: scenery 43 (z -1989 to -1843), the enclosed stretch with paneled
  walls, a roof and the DANGER wall, which the N64 doesn't have (its decks open onto the loop there). The N64 copy
  is clipped at z -1843 (`n64_overpass_end`), triangles crossing it cut in the N64's world. A copy that ran on to
  -1990 replaced that stretch with the N64's open truss and loop ramps and was dropped.
- **Placement:** the N64 world (feet, x to the left) to track.glb by the fit `tools/sfrush_ps1/n64_census.py`
  measured: x = 0.3048 z64 + 384.08, z = 0.3048 x64 - 88.06, y = 0.3048 y64 + 5.28. A translation-only refit on the
  overpass alone moved nothing sideways.
- **Heights:** the collision stays the PS1's, so each vertex moves by a table over z to put the driven decks on it
  (PS1 collision minus N64 surface, measured): the near upper deck (the jump, its road ending at z -1553 in both)
  1.0 m lower, the far lower deck 0.4 m. At the cut that also puts the lower deck's road (20.4 m) and underside
  (15.5 m) on scenery 43's. The upper deck's underside is 39.2 m there against scenery 43's 36.5 m, so from z -1700
  to the cut everything above the lower deck's curbs (21.5 m) is squeezed toward them, by up to 0.847 at the cut.
- **Two-sided:** the faces keep the N64's winding (it holds in track.glb: road tops face up) but are drawn from
  both sides. The N64 culls back faces and built the overpass for its own route; the PS1's route also sees it
  from behind (the far end), where one-sided faces left holes (tried and dropped). A deck's underside is
  either the girder image (32x64, mirrored in s, under the ramp TRACK2BT1-6) or the road image with s spanning
  only about 8 texels of its edge (TRACK2C16, 2D1, 2D2, 2PIPE1-4: s span 203-328 against the top's 4096),
  which shows as plain streaks across the deck. That is the N64's look, not a mistake to repair (a girder
  retexture of them was tried and dropped).
- **Mirrored textures:** the road image repeats mirrored in s, and the undersides' strip starts just under
  s = 0. The custom track converter (`track_mod_convert.cpp`) moves each triangle's coordinates by whole
  tiles toward zero; an odd move flips a mirrored image, which put the road's yellow line across the
  undersides. It now moves a mirrored coordinate by even counts only (`DrawMaterial::mirror_s`/`mirror_t`).
- **PS1 textures:** where the PS1's own overpass has the same surface, the N64 image gives way to the PS1's texels
  of it, read from the condition's VRAM (snowy, or the night's colors; no night darkening on top), the N64's mapping
  kept: 880 of the 1305 triangles. An N64 image is known by its size and mean color. The sources (page, CLUT, texel
  box; the same slots in snow and night, found with the PS1-only build's roads 23-28, 70 and scenery 42, 43):
  - road (N64 64x32, mirrored in s: edge line x 9.5, dashes 38.5, center line 61.5, two dashes down it) = page
    (320, 256) CLUT 30781, x 0-157 the other way round (center line x 2, dashes 63.5, edge line 133.5, then the snowy
    shoulder), 32 of its rows (a dash every 16), its columns resampled through those three anchors;
  - deck fascia (64x16) = page (320, 256) CLUT 30782, (128, 96)-(256, 128);
  - ivy wall (64x64) = page (832, 256) CLUT 26684, (184, 0)-(256, 64);
  - plain concrete (three 64x64 images) = page (320, 256) CLUT 30781, (184, 8)-(232, 52);
  - dangling rebar (64x64, cut out) = page (576, 0) CLUT 10302, (192, 0)-(256, 64);
  - undersides = page (896, 256) CLUT 24638, (12, 88)-(72, 108) (scenery 43's deck underside), for the N64's girder
    image and for the road image's triangles facing down (their s 0-0.12 strip scaled to the whole image).
  The truss members (three images), the red pillars and two ground-level images have no PS1 surface: they stay the
  N64's, darkened at night by `night_light`, and not snowy.
- **Collision:** the PS1's, plus floors under the N64 road it doesn't cover (`ps1_track.cpp` after
  `read_collision`), so no drawn road lets a car through (user report 2026-11-10: a fall through the lower deck past
  PS1 key 2):
  - The PS1's lower deck is broken between scenery 42 and 43: its floor stops at a jagged edge (track.glb meters:
    (1207.37, -1793.02), (1223.01, -1799.14), (1238.65, -1783.51), at 20.33 m) with a raised wall (a B record) along
    the last two corners across the right half of the road and a drop past the first two, and starts again at z
    -1842.91. The N64 deck runs across unbroken, so the gap gets three floor triangles on the PS1's own corners, that
    wall is removed and the deck's two side walls run on to -1842.91 with its height band.
  - Every other N64 road triangle (facing up, the road image) whose middle has no floor within 5 ft becomes floor:
    176 of 236 (the lower deck's front and its ramp, z -1390 to -1640, 13 m over the PS1's street under the
    overpass, and the far upper deck). These have no side walls.
  - Checked offline with `tools/sfrush_ps1/overpass_floor_check.py` (a top-down map of drawn road against floors; drawn heights and floor heights
    agree along the overpass's middle except the far upper deck's first few meters).
- **Camera under the lower deck:** the PS1's collision has a raised wall (a B record, band 25.6 to 67.3 ft) along
  the lower deck's front edge, track.glb x 1217-1233 at z -1723 m, hanging over the street under the overpass
  (18.4 ft). Cars pass under it, but Rush 2's chase camera, 6 ft over the car, stops at any collision polygon
  within 5 ft (`collision_ground_query_basis_8008BDC0`: a polygon whose plane is -5 to 0.25 ft from the camera
  and holds its projection pushes it), so the camera was held back there (user report 2026-11-11; with or
  without the N64 ROM). That one wall now starts 14 ft over the street (`ps1_track.cpp`, the raised wall branch,
  only inside the overpass's footprint: the same rule everywhere also lifted the Rampage truck's trailer and
  was narrowed). Found by replaying recorded car positions against the collision with the camera's rule.
- Without the ROM the PS1's own overpass is patched as below.
- `tools/sfrush_ps1/n64_scene.py` writes the same N64 scene from a model dump for the offline converter
  (`SFRUSH_PS1_N64_SCENE`).

## Embarcadero upper deck

The waterfront freeway is a double deck, broken off over the docks in both versions (the upper deck ends at z -1553
m in track.glb and picks up again with its rebar end at -1653; the N64's `TRACK2C16` / `TRACK2D1` break at about the
same place). Past the break the N64 has both levels all along (`TRACK2D1`, `D2`: the upper deck at 38-48 m). The PS1
has the lower deck (road 70) but of the upper one only the broken end (scenery 42, z -1653 to -1698), its west rail
and truss panels, until scenery 43 (z -1843 to -1989) has it again: no chunk anywhere on the disc, listed or not,
covers the 145 m between, so the rail and panels hang in the air. **[V]**

The converter fills it from scenery 43's own deck (`ps1_track.cpp`, `upper_deck`, mesh `PS1_UPPER_DECK`): scenery 43
is drawn from below only (its upper level is an underside, page 704,256, CLUT 28733, at 36.3 m, plus the west rail
and the truss), and its underside is 146 m long, so it is mapped z-linearly onto the gap from scenery 42's end to
scenery 43's start, drawn from both sides. The rest of the section is gappy there too (measured part by part: deck
tops and undersides, each deck's side faces, the truss): the lower deck's last 37 m (z -1806 to -1843), the west
lower side to -1793, the east one from -1708 to the broken end, the west upper side to -1820 and the truss panels
(west -1771 to -1843, both sides beside the broken end) are missing, so the sky shows through under and beside the
deck (user report). Each is filled with scenery 43's faces of the same part, mapped the same way (or shifted to end
at the broken end's top, -1653, for the parts beside it). Drawn only: the race runs underneath.

The PS1's water class (`FLAGS & 0x18 == 0x18`, the tunnel's channel) becomes the custom surface `COL_CHANNEL`
(`like = "grass"`, `drag = 0`): Rush 2's `COL_WATER` is deep water that ends the race on touch, while the PS1 only
notes a splash and drives it as road ([Collision](#collision)).

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
- **Into Rush 2:** none of this. The track.toml has no `[race]` times, so the race clock is the race time model's
  (src/race_time_model.cpp: the time Rush 2's average car needs along the AI lanes, with the track's traction), as on
  every converted track; the snow versions always were (with their snow traction). Until 2026-11-13 the day and night
  versions took the PS1's times above for planes 0-3.

## Run-in

Track 1 (Golden Gate) starts on a run-in, as the N64's Golden Gate does: the AI path (175 points, 21,500 ft) is open,
from the Golden Gate Bridge's north end (point 0) south over the bridge, through the toll plaza (point 10) and onto
the loop at point 29; its end (point 174) comes back beside point 29 (60 ft) instead of to its start (5,687 ft away).
**[V]**

- **The grid:** the race setup `func_80051590`'s track 1 branch (forward, 0x80051710 on) puts the cars in single file
  in two columns (x -1622 and -1639 ft: lui/ori 0x80051750-0x80051760), the back car at y 0xE9AC (5982 ft, ori at
  0x80051770) and each next one 20 ft further on (a2 += 0xC8, 0x80051840), height 277 ft up (car +4 = 0xFFF52B00 in
  1/2560 ft, y down: lui/ori 0x80051708 / 0x8005170C, + 0x500 a car): on the bridge's north approach, 210 ft before
  the path's first point and 3,300 ft before the toll plaza. **[V]**
- **The gates:** the finish plane (gate 0) is crossed once, at the toll plaza on the run-in, and the loop never comes
  back through its sector (4): every path point of the loop is in other sectors. Gates 1-4 are on the loop (PATHDAT
  points 42, 86, 121, 158). After gate 4 the PS1 goes back to gate 1, and it counts a lap on passing gate 3
  (checkpoint_test, [Checkpoints](#checkpoints)). **[V]**
- **Not the run-in:** `T1ROAD/T5ROAD2.DAT` replaces `T1ROAD2` (func_80036C40) while 0x800B2AA0 is set: the stunt
  road ([Stunt track](#stunt-track)), which starts on the same grid. **[V]**
- **Into Rush 2** (`src/sfrush_ps1/ps1_track.cpp`, `run_in_grid`): a run-in circuit (`run_in = true`,
  docs/custom_tracks.md#run-in-circuits). The path is carried back from point 0 along its first chord to 100 ft
  behind the PS1's front car, at the grid's height (the path's own heights run 50 ft under the bridge deck there), so
  the start line falls on the PS1's grid; it ends at point 174. The gates follow the N64 course (the SF Rush arcade
  version's gates, from the N64's, stand at the PS1's: docs/sfrush_arcade.md): CHECKPOINT_1 the toll plaza (gate 0),
  CHECKPOINT_2 the loop start (the path point nearest the end, point 29, 60 ft from it), CHECKPOINT_3 and 4 gates 1
  and 2, FINISH gate 3. Times: the start's (38 s, snow 60), each gate's own byte (toll 20, gates 1 and 2 21 and 17,
  FINISH 17; snow 8 more each), the loop start 0. The toll gantry keeps reading CHECKPOINT (chunk 124, its FINISH
  version, isn't drawn). Checked in game: the race starts on the bridge's north approach and runs over the bridge;
  drones pass start, toll, loop start, gates 1-2 and FINISH, then lap 2 goes loop start, gates 1-2, FINISH.

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
  [AI lanes](#ai-lanes) (the N64's lines on Golden Gate, Embarcadero and Market with the N64 ROM; otherwise the PS1's
  at 0, 1/3, 2/3 and 1 across its corridor, smoothed), AIPATH_main down their middle; speeds left to the converter. Logo: `assets/sfrush_ps1/chinatown_logo.png`
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
- **Textures:** painted in code (window grids, city-from-above ground, grass, earth, water, tiles), nothing from the
  disc but the route's line. At the N64 dioramas' grain (user, 2026-11-12: finer textures and smaller windows didn't
  match the N64 dioramas on the carousel): painted at 64 x 64 and shrunk to 16 x 16 (`reduce`; the pyramid 32), the
  city ground painted at 16 x 16 as 2 x 2 blocks of greys and a few tints (`city_tile` 170 m: about 11 m a texel,
  as coarse as Market's city), grass at 8 x 8 (the N64's grass size). Walls have 2 x 2 windows a cell (glass towers
  4 x 4), so a window is about half a cell wide, as large on the menu as the N64 buildings' window texels.
- **Checks:** the build log lists buildings closer to the route's middle than half the band, buildings whose
  footprint or eaves reach past the island's edge, and trees off it. Keep it under 4,000 triangles (the custom-track converter's warning; the N64 ones have 740-2,400
  vertices): about 3,650 now.

## Snow and night dioramas

A SNOW or NIGHT version's track select diorama wears its condition (2026-11-12). The colors come from the PS1's own
snow and night sets ([Snow and night](#snow-and-night), `tools/sfrush_ps1/snow_map.py`), in `include/diorama_look.h`:

- **Night** (`snow_map.py check N`): unlike snow, the night set is a consistent recolor. A 4-bit day-color table
  built from three tracks predicts the fourth's night texels to a median 16-19, against 39-41 for flat channel
  gains. The dioramas use its shape as a formula (`night`): about half the saturation, then 0.27 / 0.28 / 0.42 of
  each channel plus a little blue (its greys: 104 -> (20, 20, 38), 168 -> (45, 45, 71)). The route's red is left
  lit. The PS1's lit windows average (216, 206, 126) (`window_light`, warm yellow to pale).
- **Snow:** the snow set is painted by hand, not a recolor ([Snow and night](#snow-and-night)), so the dioramas
  get a rule: snow lies on what faces up, in the PS1's new-snow color (`snow_white`, its 10th-90th percentile), in
  soft patches; the rest takes the snow set's colder channel balance (`snow_cold`, x 1.08 / 1.09 / 1.27 at the same
  brightness).
- **The N64 courses' versions** (Golden Gate, Embarcadero, Market; `src/rush1/track1_convert.cpp` `dress_look`,
  called by `add_band_model` for the S and K band models): each version model's list points at its own recolored
  copies of the diorama's vertices and TLUTs (the texels stay shared). The ground (gravel, grass, water) is CI
  textures times the vertex shade, so the condition goes on whichever of the two draws a triangle. Darkening both
  multiplied night down to near black in game. A textured triangle's shade is kept (snow: lighter by up to half on
  what faces up) and its TLUT is recolored; an untextured one's shade is recolored. Snow: a TLUT that at least half
  of its triangles use facing up (the ground's) goes 80% to snow white, its light and dark kept as lighter and
  darker snow, water-blue entries left as water; other TLUTs are frosted 30%. Untextured up-facing vertices go
  snow white in patches about a tenth of the island across, and 160 flakes (three crossed squares each, 1.1% of the
  island's radius) float up to half the radius above the ground, like a snow globe; they are drawn last, between G_NOOPs tagged "SNOW" / "SNOE" (include/snowfall.h), which the track select
  branches past while Graphics > Falling Snow is off (track2049_menu.cpp `show_diorama_snow`; Chinatown's SNOWFLAKES
  material is tagged the same way by the custom track converter). Roofs: the up-facing triangles
  above the ground whose corners are all corners of the buildings' walls are dropped from the list (their indices
  made one vertex) and redrawn untextured in snow white, a tone per roof, shaded as the roof was. Night: TLUTs and
  untextured shades through `night`, and lights in the textures themselves. Every CI texture is read back with the
  render tile in force when its triangles draw (lists set tile 0 only when it changes, so a texture's own load is
  often followed by no SETTILE or SETTILESIZE). A building texture (16 x 8 CI4: a wall color and a grid of window
  texels, two or three entries, the darkest 5-45% of the texels, drawn on walls) gets 45% of its window texels lit;
  the city ground (the 64 x 32 grey CI8 on what faces up) gets 6% of its texels lit, scattered. The lit texels are
  a copy of the texture pointing at a TLUT entry none of its TLUT's textures uses, set to window light and made
  opaque (the unused entry is usually 0, transparent). So the lights sit on the modeled windows. (Panes added as
  geometry on the walls were rejected: they didn't line up with the textures' windows.)
- **Trap:** the game draws models with texturing on and the dioramas' lists never turn it on themselves, so a
  viewer that starts with texturing off shows them as white shaded plaster (`tools/menu_model_view.py` starts on).
- **Chinatown** (`src/sfrush_ps1/ps1_diorama.cpp` `dress_paints`): its paints are made in code, so each is redone.
  Snow: the up-facing paints (city ground, grass, roofs, tiles, foliage, deck, dome) get `cover` 50-95% snow in
  wrapping noise patches, the rest `snow_cold`, plus the N64 versions' flakes (160, three crossed squares each, both
  sides; 80 tetrahedra first looked sparse and triangular, user 2026-11-12) in the material SNOWFLAKES. Night: walls light 45% of their windows (12% by day), the Transamerica pyramid 30% of its slots,
  the city ground 6% of its texels (as the N64 ones' city), and everything else goes through `night`.
- **Checking offline:** `tools/rush1/cpp_test/build.bat noref` builds the track select art into
  `tools/rush1/cpp_test/out/asset3_rush1.bin`, and `python tools/menu_model_view.py <that> OUT.png R1TRACK3N R1TRACK3S
  R1TRACK3K` draws a course's N64, SNOW and NIGHT models side by side (tracks 1-3 are the PS1's courses).
  Chinatown: `tools/sfrush_ps1/cpp_test/build.bat OUT` and `tools/glb_view.py` on
  `OUT/sfrush_ps1/tracks/chinatown_snow/diorama.glb`.

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

## Sea

The PS1 has no sea: under the horizon is the sky's grey haze. With Fill Scenery Gaps on, the extended ground carried
the city out over the bay and rose to Golden Gate's deck (user report, 2026-11-09). The converter writes a `SEA` mesh
(docs/custom_tracks.md#sea; src/sfrush_ps1/ps1_track.cpp, after the night sky) for tracks 1 and 2: shore lines in
track.glb feet, each with the side the sea lies on, extruded out to 30,000 ft. Golden Gate (level -150 ft): the strait
under the bridge, Marin an island, the Pacific west of the south end and the bay along the Marina; Embarcadero (level
-8 ft): the bay east of the waterfront road and north of the wharf, placed from the N64 track select dioramas (TRK1O2,
TRK_2O2), 100-300 ft off the track's ground. The water is a flat 16 x 16 texture of the PS1's own water texel (track
2's page 576,256, CLUT 28734): snow 16,49,99, night 0,0,16 (only snow and night versions of tracks 1-3 are made).
The extended ground also lays each course's N64 diorama over it (docs/graphics_effects.md#diorama-guides, fits of
their own: the PS1 courses are the N64 ones turned a quarter), which adds Market's and any water the shore lines miss.

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
- **Into Rush 2:** cones are `PROP_KNOCK_cone_*` (knocked flying as Rush 2's cones are: custom_tracks.md#props), trams `MOVE_tram_*`
  pingpong movers from the green light, a leg 1572 ft in 26.2 s (24 a frame at the 25 frames a second the car
  conversion implies: [Car physics](#car-physics)), each with a `COL_WALL_` box of its body, which pushes cars (Rush
  2 has no wreck-on-touch surface; a hard hit still wrecks by Rush 2's crash threshold). The models use the track's
  axes (a prop isn't turned), which mirror the cars' mapping, so their faces are wound the other way round.
- **The pickups:** two more tables of the same 16-byte records, but with the fourth field a track 0-3 (gp+0xE28),
  neither turning (their rotation stays 0; the +0x28 angle turns kind 0 only): 0x800A59C4 (71 records, kind 8, model `extra\time.psv` at gp+0xCF0), placed only when gp+0x3C is set
  (the Time Sprint clocks: [Time Sprint](#time-sprint)), and 0x800A5E44 (16 records, kind 9, model `extra\key.psv` at
  gp+0xB6C), the PS1's keys ([Keys](#keys)), placed in every race (0x80015958 branches over the clocks only). **[V]**

## Keys

The PS1 has 4 keys on each of its tracks: 0x800A5E44, read by `func_800155F8` (props_place) at 0x80015A90 for the
current track ([Props](#props)), model `extra\key.psv` (loaded by `func_80036C40` at 0x800378BC into 0x800B323C,
gp+0xB6C). **[V]** A record's x, y and height are the PS1 track frame's (0.1 ft, height negative up, as the props'),
the key's origin 1.6 ft above the ground. The table is per track, not per condition: day, snow and night have the same
keys. The PS1 track index 0-2 is SF Rush course 0-2 (Golden Gate, Embarcadero, Market), 3 is Chinatown.

| PS1 track | key 1 | key 2 | key 3 | key 4 |
| --- | --- | --- | --- | --- |
| 1 Golden Gate | new | N64 KEY4 | new | N64 KEY3 |
| 2 Embarcadero | new | new | new | new |
| 3 Market | new | new | new | new |
| 4 Chinatown | own course | own course | own course | own course |

- **Against the N64 keys** (`tools/arcade_collectibles.py --ps1`): the PS1 courses are in the PS1's frame and their
  models differ from the N64's by a few ft, so each key is placed on the N64 course through a local fit: the PS1
  floors within 250 ft of it, placed by a fit of the whole course (`tools/sfrush_ps1/n64_census.py`'s `fit2d`), then
  fitted onto the N64's floors there (trimmed ICP: a turn about the vertical and a shift; median miss 2-4 ft). A PS1
  key within 40 ft across and 20 ft up or down of an N64 key is that key: Golden Gate's key 2 is 8 ft across from N64
  KEY4 (17 ft below it: the N64 hill is taller there) and key 4 is 32 ft from KEY3 at the same road narrowing. The
  other 10 are new spots. **[V]**
- **Which versions get them:** the arcade versions' rule ([arcade2049.md](arcade2049.md#coins-and-keys-on-the-arcade-versions):
  the floor under the key within 3 ft, 6 of 8 points 30 ft around it alike, no new wall beside it), run between the
  version the key is from and each other version:
  - The PS1's snow and night versions have all of their track's PS1 keys (the PS1 places them in every condition);
    the stunt version of Golden Gate has the 2 new ones (its ramps leave their spots alone).
  - New PS1 keys that fit the N64 course and its arcade version: Embarcadero 2, 3, 4 and Market 1, 2, 4. Golden Gate
    1 and 3 and Embarcadero 1 are on ground the N64 course doesn't have (or not the same around them), Market 3 has
    no N64 floor under it.
  - N64 keys on the PS1 versions: Golden Gate KEY3 and KEY4 (at the PS1's own spots), KEY5, KEY6; Embarcadero KEY3;
    Market KEY1, KEY4, KEY5, KEY6. The rest have no PS1 floor at their spot (the PS1's Embarcadero doesn't reach
    KEY1 and KEY2) or a wall beside them.
  - Chinatown: its 4 keys on its day, snow and night versions.
- **Records and bits:** a PS1 key at an N64 key's spot is that key (its record KEYn and bit, so it counts on every
  version); a new one is KEYPn (PS1 key n), bit 8 + n - 1 of its course (`src/collectibles.cpp` bit_of), Chinatown's
  too, in course 17 (`collectibles::sfrush_ps1_course`, the save file's `sfrush_ps1` section). They are drawn with SF
  Rush's KEYL1 and taken by SF Rush's hit test, as the N64 courses' keys: PS1 keys are raised 2.4 ft, from the PS1
  model's 1.6 ft origin to the 4 ft the N64's ground keys sit at. Without the N64 ROM there is no key model and the
  PS1 versions have no keys.
- **Where they are placed:** the sfrush_ps1 mod's tracks and the arcade versions get theirs from
  `src/arcade2049/arcade_collectibles.inc` (`collectible_extras`), the N64 courses from `src/rush1/ps1_keys.inc`
  (`track1_convert.cpp` convert_placement: a copy of one of the course's keys, renamed, a child of the section whose
  origin is nearest). Both are written by `tools/arcade_collectibles.py --ps1 OUT/sfrush_ps1 --write` from a PS1
  conversion (`tools/sfrush_ps1/cpp_test/build.bat`) and the disc.
- **Counts:** a course's keys are its N64 keys plus the PS1 keys on the versions the player has
  (`collectibles::key_bits`, installed: the N64 course and the enabled mods' tracks), so the Progress tab and pause box
  only count keys that can be reached. A key found counts for points even after its version is removed. The car
  rewards for an SF Rush course's keys (`unlocks::added_track_keys`) and the records screen's key icons still count
  the N64 keys only.
- **Test:** `RUSH2_TEST_ARCADE_SPOT=<record>,x,y,z` moves one onto the start straight as for the arcade versions
  ([arcade2049.md](arcade2049.md#coins-and-keys-on-the-arcade-versions)); positions are the track's world feet.

## Unlocks

What the PS1 locks, and how. gp is 0x800B26D0 (gp+0xB6C = 0x800B323C, the key model).

- **Keys:** `func_80041244` (props_update) at 0x80041640, when the player takes a kind 9 pickup: counts it in
  0x800B353C (gp+0xE6C, cleared by props_place at each race's setup) and at the 4th sets the track's flag,
  0x800B2838 + 4 x track (0-3). So a track counts only when all 4 of its keys are taken in one race. At race end
  (0x80014BFC) the flags of tracks 1-3 are copied to gp+0x178-0x17A (0x800B2848-0x800B284A), and track 4's sets
  0x800B2C64. All are kept on the memory card (func_80025250 loads them, 0x800253E8). **[V]**
- **Special car menu:** `func_800719EC` cycles a 5-slot menu (gp+0x50C, 1-5, 0 none) that skips a slot whose flag
  (0x800B2848 + slot - 1) is clear, and sets the special car 0x800B26F8 (gp+0x28, [Cars](#cars)): slot 1 (Golden Gate's
  keys) STARSKY, 2 (Embarcadero's) BBUS, 3 (Market's) JETCAR, 4 (0x800B284B, from 0x800B2C14) PIGGY, 5 (0x800B284C,
  gp+0x17C) TAXI; jump table 0x80012120. **[V]**
- **Held buttons:** the race setup (0x80014918, one player, a pad plugged in) reads the pad (0x800ED732, active low):
  R1 (0x800) MINI, L1 (0x400) JEEP, Triangle (0x1000) UFO. Never locked. **[V]**
- **Stunt road:** 0x800B2C64, Chinatown's 4 keys ([Stunt track](#stunt-track)). **[V]**
- **Snow, night, PIGGY, TAXI:** race-end counters (0x80017498): 0x800B2C60 (snow) when gp+0x10 reaches 10, counted
  in one player races with gp+0x38 (0x800B2708, set by the front end's mode 3) or gp+0x150; 0x800B2C58 (night) when
  gp+0xC reaches 3 and 0x800B2C5C with 0x800B2C14 (PIGGY's slot) at 8, counted in gp+8 (0x800B26D8, mode 2) races;
  TAXI's flag gp+0x17C when gp+0x50 is 1 at a race setup, which the snow unlock leads to (gp+4). **[V]** for the
  stores; what a counted race is (car +0x6C, a finish) is **[I]**. Which front-end mode sets which flag:
  [Game modes](#game-modes).
- **Into Rush 2:** the unlock system (`src/unlocks.cpp`) sells STARSKY, CITY BUS (BBUS), JET CAR and UFO and the
  Golden Gate stunt version for 5 points each while the sfrush_ps1 mod is installed; JEEP, MINI and POLICE are open.
  With the unlock system off everything is open: the PS1's rules above aren't kept.

## Game modes

The PS1's one-player modes (memory card save names at 0x800A6158: "1 player game", "extreme race", "circuit mode",
"gp mode", "explosive mode", "time pickup"). The mode menu (`func_8001A0D8`) sets one flag each, `func_80019200`
clears them all, and the race-over screen (`func_800702B8`) picks the mode's win or loss picture (`TITLETGA/*.TGA`,
512 x 240, 15-bit), which names it. gp is 0x800B26D0. **[V]** for the flags and their readers below.

| Flag | Mode | Win / loss picture |
|---|---|---|
| 0x800B26D8 (gp+8) | Circuit: 8 races (gp+0xC), unlock flags after races 3 and 8; failing a race ends it (gp+0x54) | CHAMPN "You are the CIRCUIT CHAMPION" / CLOSEN "Circuit mode, You have failed" |
| 0x800B2708 (gp+0x38) | Grand Prix: 10 races (gp+0x10; 3 laps, 4 drones), judged at the end (gp+0x50 from 0x800F8E6C) | GPRIXN "You are Grand Prix WINNER" / GPLOSEN "Grand Prix, You have failed to win" |
| 0x800B26FC (gp+0x2C) | Extreme: its own HUD icon (`func_8002EBA0`); lost (gp+0x58) when the race ends with 0x8010165C set | DEATHN "You have won the EXTREME race" (a skull) |
| 0x800B2700 (gp+0x30) | Explosive: [Explosive mode](#explosive-mode) | EXPLON "You are the EXPLOSIVE WINNER" / EXLOSEN "Better luck next time!" (the car burning) |
| 0x800B270C (gp+0x3C) | Time Sprint ("time pickup"): [Time Sprint](#time-sprint) | TIMEN "You've won the TIME-SPRINT!!" |

Grand Prix plays as Rush 2's circuit (a series judged on the whole) and Extreme's skull as Rush 2's DEATHS (one life),
so neither is ported. Explosive and Time Sprint are, as third values of two of the race track select's rows.

### Explosive mode

A speed bomb, in the race loop `func_800155F8`. At the race start (0x80015E24) Explosive turns Extreme on (gp+0x2C)
and clears the bomb (gp+0x34). Each frame (0x800161A8) the speedometer's mph (0x801106F8, written by `func_8008C038`,
doubled for the JETCAR and UFO) at 66 (0x42) or more arms the bomb; armed, a reading under 60 (0x3C) sets the
player's wreck bit (0x8010153A, car +0x352 |= 0x8000) unless the car has finished (0x80101254). The bomb never
disarms, and the wreck loses the race. The Extreme icon (`func_8002EBA0`) swaps frames (u 0x40 / 0x60 by 0x800B3384
bit 2, every 4 frames) while the bomb is armed, u 0x20 otherwise. **[V]**

**Into Rush 2** (`src/explosive.cpp`, include/explosive.h): the track select's DEATHS row steps OFF, ON, EXPLOSIVE
(menu +0x11 = 2; `rush2_track49_deaths_step` and `_value` in src/rush2049/track2049_menu.cpp). A circuit keeps it in
its record (`rush2::circuit::explosive`), since its block holds DEATHS as one bit. The race global 0x8010BC98 holds 2,
which every one of the game's tests reads as DEATHS on (one life), so the wreck puts the car out. Every human player's
car carries a bomb (drones none), read from the speedometer's own value (car state +0xE0, mph x 4, shown >> 2 by hud_speedometer_800BA2A8) once a physics
tick: 66 arms it; armed, under 60 sets the car's wreck flag (+0x648) unless it has finished or is out. The DEATHS skull
blinks four times a second while the player's bomb is armed. The armed flags are online race state (protocol 11).
Test: `RUSH2_TEST_DEATHS=2` with `tools/test_race.py 0 --drones 0 --script "T5:A:17,T22.5:B:8"` blows up near 58 mph
("[explosive]" in the log).

### Time Sprint

The clocks: 71 records at 0x800A59C4 (Golden Gate 21, Embarcadero 16, Market 19, Chinatown 15). The model
`EXTRA/TIME.PSV` is an hourglass (two square pyramids meeting at a neck) 5 ft across and 5.6 ft tall at its load scale
0x140 (`func_80036C40`, 0x80037850); a record's point is its middle, about 2 ft above the road. Its 16 vertices make
13 quads (no bottom), all on texture 1, z up. **[V]**

- **Texture:** props_update draws a clock (0x80041484) with the PSV prop renderer `func_80046DD4`, a1 = CLUT id 0x1037
  (VRAM (880, 64), plus the distance-fog row 0x800B2F44), texture page gp+0x2F8 = 13 (VRAM (832, 0)) at v + gp+0x2FC
  (0x40), its CLUT override zeroed. That is `ART/PICKUPS.GFX`, which `func_8003CF20` uploads to (832, 64, 64, 64) with
  its CLUT at (880, 64), in every race: an 8-byte header, 16 PS1 BGR555 colors at +8, then 256 x 64 4-bit texels at
  +0x28 (128 bytes a row, the low nibble the left texel). The hourglass is u 64-127 (the key u 0-63); it uses no
  transparent color. **[V]**
- **Sound:** a clock taken plays sound 3 of the race's bank (`func_80020750(voice, 3, volume 0x800B28B0, pan 0x7D00,
  pitch 0x8CA)`: centered, 0x8CA / 0x1000 x 44.1 kHz = 24224 Hz). `BANKS/GAMESND.BNK` is "BANK", a u32 count (29), then
  per sound name[8], offset (from the table's end, 8 + count x 16), size; sorted by name, sound 3 is CHECK (10192
  bytes of SPU ADPCM). **[V]**

- **Mode:** the menu (0x8001A730) sets gp+0x3C and 3 laps (gp+0x1B8). props_place (0x80015950) places the raced
  track's clocks as kind 8. `checkpoint_passed` (0x80054130) returns at once, so a gate adds no time. The race starts
  with the usual clock ([Race times](#race-times)). **[V]**
- **Taking one** (`func_80041244`, 0x80041538): a clock the car reaches goes (state 0, for the rest of the race) and
  adds 20 s to the clock gp+0x144 (24 at difficulty 0x800B288C 5, 27 at 6). The clocks don't come back on later laps,
  so lap 1's banked time has to last. Many lie off the drones' line, on the courses' other routes. **[V]**

**Into Rush 2** (`src/time_sprint.cpp`, include/time_sprint.h): the track select's CHECKPOINTS row steps OFF, ON,
CLOCKS (menu +0x19 = 2; `rush2_track49_clocks_step` and `_value`) on an entry whose version has clocks. The race global
0x8010C17B holds 2, which the game reads as CHECKPOINTS on (a time limit). In such a race `checkpoint_passed_8008F220`
skips its time bonus and BONUS TIME banner (hook at 0x8008F664) and, as the PS1's gates are silent, its checkpoint
sounds (hook at 0x8008F418: the chime 0x4B and sound 0x60, skipped while $a3 is 0x60; a finish place call 0x33-0x38
still plays). Each clock a human player's car comes within 8 ft of
(10 ft up or down) adds 20 s to the time allowed (race_time_float 0x8010C204), raises the BONUS TIME banner
(bonus_time_banner_end 0x8010C3C4), plays the PS1's CHECK sound at its pitch (`rush2::car_engines::play_sample`, at
the effects volume) and is gone for the race. The 20 s is scaled by difficulty as Rush 2 scales a checkpoint's time
(1 + (5 - 0x8010C211) x [0x800CFBEC], as the PS1 courses' checkpoint times are), in place of the PS1's difficulty
bonus. The hourglass is TIME.PSV with its PICKUPS.GFX texels and CLUT (CI4 with a 16-color TLUT, RDRAM
0x80E14040-0x80E14FA0), standing still as on the PS1, drawn by rush2::battle_render (slots 202-223). ps1_cars.cpp's
build_mod copies TIME.PSV, PICKUPS.GFX and CHECK (out of GAMESND.BNK) into the sfrush_ps1 mod
(`rush2/time_sprint/`), and `rush2::time_sprint::available` reads them once on a worker: without the mod the row has
no CLOCKS. The menu's laps and Rush 2's start time are kept. Online, the clocks taken are race state (protocol 11). A
CLOCKS left from another track races as ON on a track without clocks.

- **Which versions:** `tools/sfrush_ps1/time_sprint_clocks.py --ps1 <sfrush_ps1 mod folder> --write` writes
  `src/sfrush_ps1/time_sprint_clocks.inc` per version, with the local fit and ground tests the PS1 keys use
  ([Keys](#keys)) but looser: the floor within 6 ft, 4 of 8 ring points alike, and a clock may move up to 16 ft to a
  spot that passes. It keeps the PS1's height above the floor, but its middle at least 4 ft up: the PS1 records put
  it 1-3 ft up (props_place 0x80015950 and the draw 0x800413C4-0x80041400 / func_80046DD4 add no offset), which with
  2.8 ft to the hourglass's foot sinks it into the road, and 4 ft is the height the SF Rush keys hang at
  ([Keys](#keys)). The PS1 versions (snow, night,
  stunt, and Chinatown's three) have all their track's clocks. The N64 and arcade versions have Golden Gate 19 of 21,
  Embarcadero 15 of 16 and Market 17 of 19; the rest are on ground those versions don't have. N64 tracks 4-7 and The
  Rock have none (the PS1 has no such courses), so their CHECKPOINTS row keeps two values.
- **Test:** `RUSH2_TEST_CHECKPOINTS=2` sets the row. `RUSH2_TEST_TIME_SPRINT_DRONES=1` lets drones take clocks,
  `RUSH2_TEST_TIME_SPRINT_REACH=<ft>` widens the reach, and `RUSH2_TEST_TIME_SPRINT_AHEAD=<ft>` adds a clock in front
  of player 1, for `tools/test_race.py 18 --drones 1 --env RUSH2_TEST_RACE_VIEW_CAR=1` ("[time_sprint]" lines in the
  log).

## Snow and night

The PS1 races every track (1-4) in three conditions over the same road, scenery and collision: day, snow and night.
gp+0x40 (0x800B2710) == 1 is snow, gp+0x98 (0x800B2768) set is night. **[V]** No falling snow or other weather
effect: the only strings with "snow" are the menu's `TITLETGA/SNOW.GFX`. **[V]** The mod's SNOW versions add falling
snow anyway (user's request, 2026-11-12): their track.toml has `[look] snowfall = 1` (docs/custom_tracks.md#snowfall,
src/snowfall.cpp): none at FOG 1, heavier and faster at each step after.

- **Textures:** the page and CLUT loaders of every track read `T<n>PAGES/T<n>CLUTS` (snow) or `T<n>PAGEN/T<n>CLUTN`
  (night). Snow: snow on grass, roofs, walls and trees, the road kept; the CLUTs' median brightness is the day's
  (x 1.00), channels x 1.08, 1.09, 1.27 (whiter, bluer). Night: lit windows, median brightness x 0.30, channels x 0.49,
  0.49, 0.67 (bluer). The page loaders (`func_80039A88` for track 4) only pick the file by the flags (night first,
  then snow); the uploads after are the day's, the same RECTs. Every chunk is the day's size except track 4's night
  page chunk 28 (64 x 28 texels against its RECT's 64 x 32): the loader reads the whole .DAT into one buffer, steps
  through it by the .SIZ's sizes and uploads 64 x 32 from the chunk whatever its size, so that RECT's last 4 rows
  come from the start of chunk 29. **[V]**
- **Snow is painted, not a recolor** (`tools/sfrush_ps1/snow_map.py check S`, 2026-11-11): the snow pages change
  the texels too (28-47% of texel bytes per track, not just the CLUTs). Pairing every day texel a road or scenery face
  shows with the snow texel at the same place (4.2 M distinct texels, each once per face tilt), 55% keep their
  brightness and 25% turn snow white, and the same day color goes both ways: day grey (88, 88, 88) stays 63%, goes
  white 18%. A day-color table (4-bit bins, built from three tracks, tested on the fourth) is off by a median 24-30
  and over 48 on 23-31% of texels, its bins averaging white and bare into a muddy in-between; whether the face looks
  up improves it by 1-2. So the snow dioramas use a rule instead ([Snow and night dioramas](#snow-and-night-dioramas)).
  The new snow's color: mean (195, 195, 223), 10th-90th percentile (173, 173, 198)-(222, 231,
  255). **[V]**
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
  friction scale together), but above 39% of it a car accelerates, brakes and grips a quarter as hard. **[V]** The
  thrust is one of the scaled forces (0x8005FCB8-0x8005FD4C add it into 0x800ED0FC, +4, +8, per car 0xC8 apart), and
  car speed is sqrt(vx^2 + vz^2) >> 5 (0x8005031C-0x8005032C), so the thresholds are 32 and 64 mph at the car
  conversion's 25 frames a second. On a flat straight the BMW's velocity change a frame is 16 x 12 (thrust) - v / 128
  (rolling friction, 2/1024 a wheel), top |v| 24576; worked through frame by frame (that sum, halved from |v| 4832 and quartered from 9632 in snow) it is
  at 0.62, 0.86, 0.95 of its top speed after 5, 10, 15 s dry and at 0.42, 0.55, 0.65, 0.72 after 5, 10, 15, 20 s in
  snow: the PS1's own snow takes 3.5 times as long to reach 90% of top speed (1022 frames against 293), and covers
  0.77 of the dry distance over 40 s (the 8 s a gate allow for it). The PS1 has no gravity along a slope on the ground:
  `car_gravity` (0x8005E0CC) adds gravity only while a wheel is off the ground and otherwise takes the velocity's part
  along the car's up axis away, so hills neither slow nor speed a car. **[V]**
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
  its 52 chunks, 10 and 26 with `clut_upload_dark`: [Textures](#textures)), texture table (0x800A7478 for track index 1, 0x800A77C0 for the others),
  banner instructions (track 1 0x80044524 / 0x80044534, track 2 0x8004459C / 0x800445AC, track 3 0x800445DC /
  0x800445EC), gate tables (counts / sectors / first / times: track 1 0x800B2AF8 / 0x800AA388 / 0x800B2B00 /
  0x800B2AD8, track 2 0x800B2B08 / 0x800AA394 / 0x800B2B10 / 0x800B2AE0, track 3 0x800AA3A0 / 0x800AA3BC /
  0x800AA3CC / 0x800AA37C) and start time; the bonus instruction 0x800542D8 is track 4's only. Track 2's knock-over
  props (table 0x800A55B4: T2K001 cone at scale 0x380, T2K002 traffic light and T2K003 tree at 0x280, both 6.4 ft up
  and the tree upside down, all turned 90 degrees as `model_prepare` 0x8003E714 does) become `PROP_KNOCK_` props. Snow: the PS1's race times are 8 s more at every gate (0x80054358), for its two halvings; the recomp keeps one, so its snow versions keep no times and the race time model times them with the traction (docs/custom_tracks.md#handling). And
  `[handling] traction = [[32.1, 0.5]]` (speed 151 from 0x8005D204 in mph at the car conversion's 25 frames a second;
  the PS1's second halving from 301 / 64.1 mph, 0x8005D240, is left out on purpose: speeding up on a quarter of the
  thrust played far too slowly, so snow is half the push from 32 mph, twice as long to speed up: `src/track_traction.cpp`, before car_linear_accel_stage_80069A54 turns the
  car-frame force sum +0x4..+0xC into accelerations (car +0x1C..+0x24 = force x 0x80110018 / the weight +0x5AC),
  turns the sum, gravity in it, into the world, scales its horizontal parts and keeps the vertical one, as the PS1
  scales its frame's velocity change). Rush 2's slopes push a car through the ground forces' horizontal parts, so
  those weaken with the thrust and a hill in snow is what it is dry, slowed alike; the first version kept gravity's
  share whole, which left a car on a quarter of its thrust against all of a hill, a thing the PS1 (no slope force)
  never does. Checked against the PS1's numbers above: Rush 2's car is at 0.57, 0.82, 0.94 of its top speed (about
  145 mph) after 5, 10, 15 s dry and 0.41, 0.54, 0.61, 0.66 after 5-20 s in snow (the PS1's 0.42, 0.55, 0.65, 0.72),
  so snow's slow climb to speed (60-80 mph for the first 10 s) is the PS1's own. Over 23 s of the same start the
  drones covered 0.76 of their night (full traction) distance with no wrecks, against the 0.75 the PS1's 8 s per gate
  allows for. The drones' target speeds take the traction into account (docs/custom_tracks.md#handling): without
  it they reached Market's and Golden Gate's tight corners at their dry speed and wrecked there (7 and 3 wrecks in
  60 s, none at night); with it none of the four tracks had a wreck, the drones at 0.58-0.71 of their night pace.
  That first model slowed every corner to sqrt(f) and braked f times as softly, so the drones braked on the
  straights and a player pulled away (user report); slowing only corners past f x the table's 177 ft/s^2 gives, over
  60 s, Market 0.77 and Golden Gate 0.91 of night pace, Chinatown and Embarcadero snow no wrecks either, braking
  above 100 mph 0-2% of the time (tools/drone_pace.py).
  Scaling the tires' grip alone (the first version) left the engine and drag whole: cars reached full
  speed and slid off (0.58, two drones wrecking over and over). Night: fog #080808, a `SKY` dome colored as the PS1's
  gradient (grey 64 at 25.6 degrees and up to (0, 0, 25) at the horizon), `car_light` the night car pages' ratio to
  the day's over every car texture both sets have (entries 1-6: #454046). The diorama, logo and picture are the day
  track's. The light glow sprites above aren't ported: a first version drew them as flare cards at the PS1's points
  (or points measured from other cars' bodies), and they didn't sit on the cars' real lights.
- **Not a texture:** `ART/T<n>P08.GFX` / `P08N.GFX` (VRAM (384, 32), 64 x 128) loads only while gp+0x3C (Time
  Sprint) is set, and mods don't carry it. It isn't the clocks' texture (that is `ART/PICKUPS.GFX`,
  [Time Sprint](#time-sprint)); what draws from it isn't traced. **[V]**

## Stunt track

Golden Gate has a second road, a stunt course: the same course, scenery and textures with ramps built onto the road
(split lanes with a divider, DANGER / CONSTRUCTION ZONE boards, chevrons). **[V]**

- **Picked by:** the race setup in `special_car_setup` (0x800142C4) sets 0x800B2AA0 (gp+0x3D0) to 1, the track index
  gp+0x70 and gp+0xE28 to 0 (track 1), when the menu's variant value gp+0x48 is 3 while 0x800B2C64 is set (loaded from
  the memory card with the other unlock bytes 0x800B2C58-0x800B2C60 by func_80025250 **[I]**: an unlock) and
  0x800B2B4D is clear; mirror mode (0x800B3468) clears it again. Snow needs gp+0x48 == 2 and night gp+0x48 == 0, so
  the stunt road is day only. **[V]**
- **What it changes (only these three readers):** `track_load_geometry` (func_80036C40) loads `T1ROAD/T5ROAD2.DAT`
  (+ `.SIZ`) for `T1ROAD2` (0x80036C80); `track1_load_tables` (func_8004F9C0) loads `COLLDAT5`'s `BOUNDS.DAT`,
  `COLLMAP.SIZ`/`.DAT` and `FLAGS.DAT` for `COLLDAT1`'s (0x8004F9CC, 0x8004FA30, 0x8004FA94), still with `TRACK1.CPF`;
  the race setup (func_800155F8) starts the clock at 180 s (0x80015E1C) in place of 38. Track 1's AI paths, gates, gate
  times, scenery (T1SCN2), pages and CLUTs are kept. **[V]**
- **The road:** 55 chunks as T1ROAD2's, 16 reshaped (1, 3, 5, 10, 14, 16, 20, 24, 30, 31, 33-37, 39; chunk 5 from 124 to
  138 faces). **Collision:** 52 chunks, 29 changed: 914 ground polygons against 859 (116 of them the stunt road's own,
  61 of Golden Gate's gone), 743 walls against 671 and 146 raised walls (B records) against 80, the ramps' sides. **[V]**
- **Into Rush 2** (`src/sfrush_ps1/ps1_track.cpp`, `Condition::stunt`): custom track `golden_gate_stunt`, a STUNT
  version of the N64 Golden Gate (`variant_of = "sfrush:golden_gate"`, green version band `version_ring_stunt`),
  built from the day pages and CLUTs, T5ROAD2 and COLLDAT5, a run-in circuit like the other Golden Gate versions, start
  time 180 and track 1's gate times. The spline road (Golden Gate's smoothed floor, [Collision](#collision)) is fitted
  to the floor without the ramps (ground polygons whose corners COLLDAT1 has no polygon with) and they keep their own
  flat faces, so their lips stay sharp. Fill Scenery Gaps' sea under it takes the day water texel (24, 49, 99).
- **Air gravity:** the PS1 pulls a car in the air with 600 a frame^2 (descriptor +0x18, `func_8005E0CC`), about 4.5 x
  Rush 2's gravity at the conversion's frames per second; a jump's length goes as v^2 / g, so on Rush 2's gravity the
  kickers threw cars about 550 ft (a drone at 115 mph flew 3 s and wrecked nose-first). The track sets
  `[handling] air_gravity = 2`: Rush 2's gravity x 2 while all four wheels are off the road, eased in over the first
  foot (`air_gravity` in `src/calspeed/calspeed_rom.cpp`, from `rush2_calspeed_road_hold` after `car_force_sum`), so
  jumps fly about half as far as on Rush 2's gravity and on the road the cars drive as on any other track. x 4 (near
  the PS1's) felt too heavy in play.
- **AI paths:** the disc has no stunt path (only `COLLDAT2/PATHDAT1-4`, `EXTDAT1-4`, `REVPATH1-4`, `EXTRDAT1-4`;
  COLLDAT5 holds only BOUNDS, COLLMAP and FLAGS), and the stunt flag's three readers don't touch the path loader
  (`func_80055678`), so the PS1's stunt race reads Golden Gate's PATHDAT1/EXTDAT1. It has drones: their count
  gp+0x1BC is the options menu's DRONES row (0x8001C210 steps it up while below 6, 0x8001C444 down while 3 or more), set
  apart from the variant value gp+0x48 that picks the stunt road, and nothing that reads the stunt flag (0x80015E10,
  0x80036C80, 0x8004F9CC/FA30/FA94) touches it. **[V]** Golden Gate's lanes (the N64's, [AI lanes](#ai-lanes)) know
  nothing of the stunt road: on them the drones hit the bridge's divider wedge (-3780, 148, -1635), the park's blocks
  (175, 129, -1062) and drove into the pit at (1530, 22, -3245), 12-16 wrecks in a 64-78 s race; the conversion plans
  its own ([Stunt lanes](#stunt-lanes)).
- **Stunt scoring:** the track scores stunts and shows the stunt panel in its races, as Rush 2's Stunts on All Tracks
  cheat does, without the cheat being set: `[track] stunts = true` (docs/custom_tracks.md#tracktoml). The cheat byte
  0x8010C200 has two readers in a race, the stunt panel's visibility `hud_stunt_box_visible_80060E34` and its text
  `hud_widget_text_cb_player_stats_800B7654`; `rush2_track_mod_stunts` (src/rush2049/track2049.cpp) hooks each right
  after its load into $a1 (0x80060E7C, 0x800B76B0) and reads it as set; the HUD layout's `stunts_all_tracks` takes it
  too (src/hud.cpp). Stunt scoring itself (`car_stunt_scoring_update_80065588`) runs on every track. **[V]**

### Stunt lanes

`build_condition` in `src/sfrush_ps1/ps1_track.cpp` (the `if (stunt)` block after the lanes are built) re-plans each
of the four lanes over the stunt road; Golden Gate's other versions keep theirs.

- **Stations and offsets:** at each AI path station, offsets every 1 ft across, 60 ft either side of the path's middle.
  The ground is sampled out from the middle, each sample taking the layer nearest the one before (the first, the
  layer nearest the path's own height: carried on from station to station, it locked onto the water under the bridge).
- **Open:** ground under the car's body (+-4 ft) with no step over 1.5 ft between 1 ft samples (a step over 4 ft, a pit's
  or a ledge's side, by 4 ft more), no ground steeper than 25 degrees that isn't a stunt ramp (the cliffs on the park's
  grass shoulder), not straddling a stunt ramp's edge, 6 ft from every wall at that ground's height. A move between
  stations mustn't cut a wall.
- **Dead ends:** an open offset only counts when the path's end can be reached from it (worked back from the end;
  a station nothing gets past starts it again). The pit at (1500, -3250) is open stunt ramp down to a 20 ft face.
- **Cost** (dynamic programming over the whole path, at most 3 ft across a station): the old line's distance squared
  (x 0.05, capped at 12 ft); dirt 25 and the park's grass 30 a station, more than straying or crowding can cost (at 1.5
  the lanes kept the N64's line over the stunt road's dirt before the tunnel and the drones lost seconds a lap there);
  0.15 x (16 - distance)^2 within 16 ft of a closed offset at this station or the next 3 (drones aim ahead and cut a
  bend's inside); 0.1 x (8 - gap)^2 within 8 ft of a lane planned before it (alone they met on one line and drones
  bunched there swung out to pass); 2 x step^2; and on a ramp (a stunt polygon over 12 degrees, its fall line within 20
  of the road's) and 4 stations before it 10 x step^2 more, so the lane crosses it straight. Following each ramp's own
  fall line instead (with every stunt polygon over 3 degrees a ramp) walked the lanes 20 ft sideways onto the dirt
  before the tunnel and round the pit's bypass.
- **Smoothing:** the 1 ft steps [1 2 1] twice, then in the world twice (the stations' middle line has kinks of its own),
  each where the result is still open and off a ramp's line.
- **Grid:** the run-in's first 20 stations keep the old lanes, blended into the new over 10 (planned lanes met at one
  point there and the cars started on top of each other).
- **Speeds:** where a lane left the old line by more than 6 ft (and 10 stations either side), its N64 speed is dropped
  and the converter's own, from the new line's corners, takes over.
- **Result** (`tools/drone_pace.py --race golden_gate_stunt --laps 1 --mod-dir OUT --wrecks --sections 1500`, headless):
  a 103-107 s lap against base Golden Gate's 90 (night, the N64's lanes). From the bridge through the tunnel the
  stretches run within a second of base's. The rest is the start (about 4 s: the cars from the back of the grid land
  off the big ramp at the foot of the bridge onto the shoulders) and the park round the pit (about 11 s: the stunt road
  puts the pit where Golden Gate's road ran, and its only way round is the park's dirt). Checks: the converter's report
  line `stunt lanes: ...`; SFRUSH_PS1_LANE_DEBUG=x,z[,n] (converter frame, x mirrored from the game's) prints n
  stations' offsets (X closed, D a dead end, r on a ramp's line, R the same on dirt, d dirt) and where each lane is;
  `tools/lane_surfaces.py --rush2 OUT --plot X Z RADIUS PNG [--trail motion.jsonl] [--steep DEG]` maps the ground,
  walls, lanes and a car's line round a spot, and without --plot lists each lane's runs off the road.

## Music

The disc's four CD audio tracks (tracks 2-5, 16-bit stereo at 44.1 kHz) are the game's music. The port plays them as
SF Rush songs (src/music_streams.cpp: `ps1::audio_tracks` finds them through the .cue, a .ccd or a CHD and
`ps1::read_audio` reads them; nothing is converted or copied into the mod). Like SF Rush's N64 songs they belong to no
track: an SF Rush N64 track's Original is a random one of the N64 and PlayStation songs, Chinatown's too. They are
listed in the Sound tab under SF Rush > PlayStation, with a switch for the source while SF Rush has more than one
installed. Measured -8.4 LUFS as files against Rush 2's -16.1, they are turned down 7.7 dB and doubled for the
output's halving (`file_song_makeup`, include/audio2049.h): a gain of 0.826 (`ps1_music_compensation`). The
songs are taken to be the PlayStation version's own (not checked against the N64 songs, which only play as sequences
in the game). **[I]**

## Not ported

- Grand Prix and Extreme ([Game modes](#game-modes)): Rush 2's circuit and DEATHS play them. Explosive and Time Sprint
  are ported: [Explosive mode](#explosive-mode), [Time Sprint](#time-sprint). The keys too: [Keys](#keys).
