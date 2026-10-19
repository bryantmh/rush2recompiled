# San Francisco Rush 2049 arcade (SE) tracks and cars

The arcade game (Rush 2049 Special Edition) has a track the N64 never got: Alcatraz. `tools/arcade2049/track_mod.py`
converts an arcade track into a custom-track mod (docs/custom_tracks.md) at the arcade's full model detail and texture
size; `tools/arcade2049/arcade.py` reads the data. Nothing here is shared with the N64 or Dreamcast formats.

```
python tools/arcade2049/track_mod.py TRACK4 OUT_MOD_DIR [--id alcatraz] [--name Alcatraz] [--logo-text TEXT] [--preview PNG]
python tools/arcade2049/arcade.py TRACK4          (a summary of the track's files)
python tools/arcade2049/track_mod.py TRACK4 OUT --id alcatraz_test --finish-at 1970   (test copy: starts elsewhere)
```

Install the output as `build/mods/<mod id>` and enable it in `build/mods.json`. Rush 2049's ROM in the Games tab makes
the props react (docs/custom_tracks.md, "Props").

## Where the data is

ProjectR (a native port of the arcade games) unpacks the arcade disk (`sf2049se.chd`) to
`%LOCALAPPDATA%\ProjectR\Data\sf2049se` (`ARCADE2049_DATA` overrides it): `TRACKS\TRACK1`..`TRACK8` and `DEV`, `CARS`,
`SELECT` (the select screens' textures, same format as a track's), `STATIC`, `AUDIO` and the rest. Per track: `dobs`,
`dpath`, `objects`, `roaddat`, `textures`, `worlds`, `vlut`, `vl_zpath` (`.rom`), all little-endian and uncompressed.

| Arcade | Track |
|---|---|
| TRACK1, 2, 3, 5, 6, 7 | the N64's Marina, Haight, Civic, Metro, Mission, Presidio (matched by their paths) |
| TRACK4 | **Alcatraz** (arcade only) |
| TRACK8 | STUNT_TRACK2 (`roaddat.rom` +0x1C), its `dobs` empty |

## Axes

World = the N64 2049 frame (feet, y up), which `tools/stock_track_glb.py`'s `to_gltf` takes. `objects.rom` vertices and
`worlds.rom` positions are already in it. `dpath.rom` and `roaddat.rom` points are in the path frame: world =
(p.y, -p.z, p.x). `dobs.rom` positions are (x, z, y) of world; its objects are modeled in world axes, and its matrix
becomes `P M P` with P swapping y and z, its rows where the object's axes go (points are row vectors). The order is
fixed by the three GS_PLANEARC flyby arms: read this way their planes circle 140-2,000 ft up; transposed, 550 ft
underground.

## objects.rom (models)

- 0x50: u32 object count, u32 0, u32 object name table, u32 texture name table. 0x60: u32 object count, u32 texture
  count, u32 0.
- Object headers from 0x68, 0x8C bytes each: u32 0, f32 radius, u32 LOD count, then 4 LOD slots of 0x20 (f32 distance
  squared, u32 1, u32 vertex count, u32 vertex offset, u32 triangle count, u32 triangle offset, u32 0, s32 -1).
- Vertex, 28 bytes: f32 x, y, z; f32 s, t (Voodoo texture space: 0-256 along the texture's long side); u16 flags; u8
  r, g, b, a (multiply the texture; Alcatraz is a night track, so its colors are dark); u16 0xDEAD.
- Triangle, 8 bytes: u16 a, b, c, u16 texture index. The converter swaps b and c, since `to_gltf` mirrors x.
- Texture descriptors (0x50 bytes) follow the object headers: u8 smallest LOD, largest LOD (6), format; u16 width,
  height; u32 offset into `textures.rom`; s32 LOD bias; u32 Voodoo textureMode (bit 6 / 7: clamp s / t); u32 tLOD; 8
  zero bytes; the NCC table: Y[16], I[4], Q[4] (I and Q packed s9 RGB: R bits 18-26, G 9-17, B 0-8).
- Formats: 1 YIQ422 (8-bit NCC), 9 AYIQ8422 (8-bit alpha + NCC), 10 RGB565; the select screens also use 4 and 12
  (not decoded). Mipmaps go down to LOD 6.
- Name tables: objects 0x18 bytes (name[16], f32, u32), textures 0x14 bytes (name[16], u16 width, height).

## worlds.rom (static placements)

u32 1, u32 0x18, a 16-byte TEMP record, then 0x4C-byte instances: name[8] (cut to 8 characters, so `object_named`
matches by prefix), 8 flag bytes, f32 m[9], f32 pos[3], u32, s16 next, s16 -1, u32. Every Alcatraz matrix is identity.

## dobs.rom (dynamic objects)

u8 1, then records: u8 name length (with its NUL), the name (can be longer than the object table's 15 characters; a
flipbook's frames are `<name>_1..n`), u16 script length, the script (CRLF lines), f32 m[9], f32 pos[3]. Script
commands and what the converter makes of them:

| Command | Meaning | Converted to |
|---|---|---|
| `frontface` | a card turned to the camera | `BILLBOARD_` |
| `bounce`, `box x y z`, `point x y z` | knocked when hit (box or point hit shape) | `PROP_KNOCK_` |
| `coin` | a score pickup, not Rush 2049's hidden coins | left out (mods have no collectibles) |
| `spinx`/`spinz <deg/s>` | turns about its own x / upright axis, as its matrix turns it **[I]** (degrees per second fits the lighthouse and police lights) | looping `MOVE_` |
| `flipbook <fps>` | steps through `<name>_1..n` at that rate; negative: on an event | one `MOVE_` per frame, shown by scale steps |
| `alpha 0` / `alpha 1` | blending mode (not decoded) | the texture's own alpha |
| `sound`, `soundsphere <name> <range> <volume>` | object and ambient sounds | left out |
| `followz`, `hide`, `start when finishswap` | scripted moves and events | left out / first frame |

Alcatraz has 100 coin pickups in trails along its shortcuts and jumps (every arcade race track has 100; left out),
16 cones, 30 construction horses, 68 billboards, 8 spinning objects (lighthouse beams, police lights, three circling planes) and
6 flipbooks (runway chaser lights, sirens, blinkers). A frame is hidden by scaling it to 0.001; the movers code hides such a node instead of drawing it
(blended between scales, the runway lights drew shrunken copies between the loop center and the loop; snapped, the
frames shook). A flipbook's pivot is its frames' geometry center on a whole foot, so their vertices round to the
scenery's 1/16 ft grid (BLINK3's placement is the world origin, 1,000 ft away: hairline cracks along its walls). All
frames of a flipbook loop in exactly the same time: the first frame's keys end on its shown pose (an open loop adds
0.001 s, and it drifted until no frame or two showed; the blinker frames carry their walls). The 2 ms steps between
frames are not blended in the game (docs/custom_tracks.md, flip-book frames): blended, frames caught mid-step drew
shrunk, and the lights' walls shook and showed cracks. A flipbook's frames share one pivot (the center of all its
frames' geometry), so the game draws them under one interpolation ID and the frame shown blends with the camera.

## roaddat.rom (collision)

The source of the N64 port's collision files (tools/rush2049/collision.py), in its own layout. Header: u32 size, node
count, polygon count, vertex count, segment count, leaf u16 count, vertex-list u16 count; 0x1C f32[3] and 0x18 bytes
(not decoded). Then, in order: nodes from 0x40 (0x24 bytes: s32 parent, s32 x0, x1, z0, z1, s32 child[4], negative =
leaf), polygons (0x1C: u32 flags, u16 info, s16 m[9] x 16384, u32 vertex-list offset in u16 units), vertices (s32
x, y, z in 1/40 ft), segments (0xA4, the road splines), leaves (u16), vertex lists (u16 count, u16 indices). As on
N64, a polygon's vertex 0 is its origin and the rest are in its frame: point = v0 + M local, M's columns the local
axes, minus column 2 the face normal. Flags: the low byte is the N64 surface (0 road, 1 dirt, 5 wall), 0x1000 a
spline surface, whose vertex list ends with its u16 segment index. Arcade Marina matches the N64 collision to 0.04
ft. Where more polygons crowd a spot than Rush 2's 36 per collision cell, the converter welds corners and joins flat
neighbors (`relieve_crowding`; on Alcatraz about 130 pairs, corners moved 1.4 ft at most).

### Segments (the spline road)

0xA4 bytes after the vertices (`Track.segments()` converts them). f32 origin[3] and matrix[9] in the path frame, the
matrix stored transposed (game matrix = Q Aᵀ Qᵀ, Q the path → world axis swap); f32 across[3], left and right along
directions[3 + 3] (path frame); then f32 0x54..0x78 as Rush 2's SEG (docs/rush2049_research/collision.md); 8 more
words not read. The arcade's across parameter spans twice the road: the N64's 0..1 is its 0.25..0.75, so the
widths (0x54, 0x64..0x70) halve and the edge directions and slopes (0x3C / 0x48, 0x74 / 0x78) are taken a quarter
and three quarters across (directions renormalized). Against arcade Marina's N64 port (file 139) every field of the
500 segments both keep is equal to float precision; the N64 resampled the rest and reversed some chains. On
Alcatraz every spline polygon corner is within 0.05 ft of its segment's surface. The next segment is the next
record, as on N64.

The arcade's spline polygons are long facets (Alcatraz: a crease every 11 ft of lap, 2.5 degrees typical, 6.8 at
the 90th percentile; 99% of the lap is spline road), and many reach well past their own segment (t from -2 to 2.8),
which the arcade handles and Rush 2 can't (it extrapolates the one segment named). Driven flat, they are a stream
of bumps. The converter (`spline_road`) cuts the spline road by segment stretch (start line to the next segment's)
and joins each stretch's pieces into one footprint, snapped to the file's 1/32 ft grid, on a plane near the surface,
naming its segment (`_SEGMENT`, `ROAD_SPLINES`: docs/custom_tracks.md, "Spline road"). Alcatraz: 1,076 segments,
about 2,200 spline polygons, 6,600 in all. Along the lap, the slope's change per foot falls from 0.0070 (p99 0.10) on
the facets to 0.0014 (p99 0.015). The pieces a chain's end leaves uncovered stay flat.

## dpath.rom (the drones' paths)

8 tables of 2000 slots of 0x18 bytes (384,000 bytes). Slot 0 is a header (u32 lap point count, u32 1 or 0, ...).
Records: f32 x, y, z (path frame), f32 speed (ft/s), u32 behaviour (the N64 lanes' steering code: 2 follow, 5
damped, 1 blended, 3, 4, 0), u16 index, u16 link. Table 0 is the center lap (659 points on Alcatraz) followed by
routes off the course; tables 1 and 2 are racing lines about 10 ft from it (744 points). On Alcatraz tables 3-7 are
stale data about 2,900 ft off the course; on the other tracks all seven are lanes. The lap is at road height (median
0.4 ft) except over the jumps, where it is the cars' arc, hence `keep_height`. Speeds become Rush 2 mph as
ft/s x 60 / 88 / 1.2 (Rush 2049's lane speeds over Rush 2's, docs/rush2049_research/cars.md).

## What the converter makes

- Scenery: every `worlds` instance and `dobs` object at LOD 0, one material per arcade texture at full size (PNG),
  MASK or BLEND by how soft its alpha is, clamped where the textureMode clamps. Triangles with an edge over 2,500 ft
  are halved (UVs and colors interpolated).
- `SKY`: objects using the sky textures (GS_SKY, GS_APANS, AL_MOON), the cloud layer, or reaching past 9,000 ft,
  except the bay's WATER2 sheets (G6688-G6693, out to 10,700 ft): the sky is drawn without depth around the camera,
  so in it they painted a dark plane across the dome while the sky kept its world height. They are scenery (the near
  part, depth tested) and sky too: the game draws 2,000 ft, and the sky's water meets the distant land (GS_APANS) as in
  the arcade. Sky materials go in draw order: dome, water, land, clouds. The dome (27,600 ft) is scaled to fit the sky
  model's s16 vertices (about 2,048 ft); Rush 2 moves a model sky with the camera in x and z only, so its top sat near
  520 ft, under the island's top. The sky is now placed at (1 - scale) x the camera's height
  (`rush2_track49_sky_height`, render_sky_cloud_scroll_8008918C at 0x800891F8), as it would look at full size.
- `COL_*`: `roaddat` polygons by surface, the spline road as footprints on its segments (above). `roaddat` has no water: under the bay is sea floor (road) and walls, and
  the arcade drowns cars some other way (not found). The WATER2 sheets (the bay, at sea level) become `COL_WATER`,
  which wrecks and resets the car as Rush 2's own Alcatraz water does; no road polygon or lap point is under them.
- `FINISH` at the finish sign; `CHECKPOINT_n` at the MG_CHECK5 banners (the arcade's own gate data wasn't found).
- `AIPATH_main`: table 0's lap as a ribbon as wide as the road, with `_SPEED` and `_AI_MODE` from its records and
  `keep_height`; `AIPATH_lane1-4`: every table within 60 ft of the lap, left to right, spread over the four slots.
- `[track] gravity = "rush2049"`: Rush 2049's gravity table (the N64's, 37.03 ft/s² at Normal; the arcade's own
  value isn't checked).
- `preview.png`: `--preview`, or `tools/arcade2049/previews/<id>.png` when there is one, cut like Rush 2049's
  track select thumbnails (TPIC): the middle square at 128 x 128 inside a hard-edged disc of radius 62 centered on
  (63.5, 64.5), which matches every TPIC's alpha exactly.
- `logo.png`: the name in Rush 2049's banner lettering (`banner_font.render_fit`; "Alcatraz" fits the smallest
  canvas, 128 x 32, so its letters are as large as the stock tracks').
- `--finish-at FEET` (test aid) moves the finish along the lap to start a race next to something far into it.

## Cars

`CARS/CAR1`..`CAR11`, each with `COL1`..`COLn` (2 to 8) holding an `objects.rom` and `textures.rom` in the track
format. CAR1-CAR6 are Rush 2049's futuristic cars (CAR3 is the Rocket ZX, with its rocket flame objects); the N64 game
has 13, so 7 of its cars aren't in the arcade. CAR7-CAR11 are the arcade's own: a Plymouth 'Cuda (select icons
`CUDA_*`), a pickup truck (`TRK_*`), a McLaren F1 (`MC*`), a '30s hot rod (`HROD_*`) and a '50s Cadillac custom
(`CAD_*`).

Objects per car: `CARBODY` (3-4 LODs, about 160-300 triangles at LOD 0), `HOOD` (the hood camera's model, wider than
the body), `BRAKE` (the lit brake lights), `WHEEL` and `WBLUR` (one wheel, 1.01 ft round, and its spinning version;
CAR1's 0.96), and on some `G2` (a 20 ft cube). The body is in the car's frame: feet, +z the front, y up, and the same
origin as Rush 2's cars, with the wheel centers about 0.2 ft above it (the wheel wells drawn on the side textures sit on
the body's bottom edge at y 0.0-0.3, and the hot rod's rear axle object is at y 0.12).

### Wheel positions

The data holds none. The game (ProjectR's `project-r.exe`, a port of the arcade code) has one car descriptor, at file
offset 0x6210F0. A static initializer (VA 0x140264340) copies it to a global, with the wheel block from 0x621150 going
to 0x140A59F80. Each race car's setup (VA 0x1401FE9B2) copies that block unchanged, and the race start (VA 0x1402585F0,
called once per car from 0x14029AAD2) clones `WBLUR` four times and places the copies there. So every arcade car,
CAR1-CAR11, has the same wheels. Per wheel (z, x, y), y down: front (3.925, ±2.75, 0.8), rear (-3.925, ±2.75, 0.75).
These are the N64 2049 default descriptor's points (Rush 2's descriptor: y -0.8 / -0.75).

A `COL` folder is a whole livery, not a paint color. Its texture names differ (the 'Cuda's `COL2` is a `B_` set,
`COL8` a police car with `COP_` textures and a light bar), and some add parts (a blower, a hood scoop).

`tools/arcade2049/car_mod.py` builds the `rush2049_arcade_cars` mod (docs/car_mods.md#rush-2049-arcade-cars). Most
arcade cars are the arcade forms of N64 cars: matched by side and top silhouette against the N64 cars' bodies (the
asset dump's FRAME1) and by shared texture names (the Truck's `C11_` set is the Crusher's, the Cuda's blower the
Bruiser's). Only their `COL` folders that differ from the N64 car's look go in the mod, as liveries of the N64 car
(docs/car_mods.md#liveries); the Cuda's go to the Bruiser. The Truck, Deuce and Caddy are new cars: their wheels sit
at the arcade's points with a scale of 1.08, which is 1.01 ft at Rush 2's wheel model size, and each `COL` folder is a
color scheme (STYLE), with the arcade's textures as authored and no paint mask.

| Car | N64 counterpart | In the mod |
|---|---|---|
| CAR1 FORMULA 1 SE | FORMULA 1 | nothing |
| CAR2 GX-2 SE | GX-2 | GX-2 livery: COL6 |
| CAR3 ROCKET ZX SE | ROCKET ZX | nothing |
| CAR4 MAGNUM SE | MAGNUM | Magnum liveries: COL4, 6, 7 |
| CAR5 SUPER GT SE | SUPER GT | Super GT liveries: COL5, 7 |
| CAR6 8-BALL SE | 8-BALL | 8-Ball liveries: COL1, 3 |
| CAR7 CUDA | BRUISER | Bruiser liveries: COL1, 2, 3, 5, 6, 7, 8 (moved 0.5 ft forward to the Bruiser's wheels) |
| CAR8 TRUCK | CRUSHER (the lifted monster-truck form of this body) | new car TRUCK, drives as Rush 2's PICKUP |
| CAR9 SUPERCAR | PANTHER | nothing |
| CAR10 DEUCE | none | new car DEUCE, drives as Rush 2's HOTROD |
| CAR11 CADDY | none | new car CADDY, drives as Rush 2's CADDY |

### Car physics

The arcade gives every player car the same physics. A static initializer (VA 0x140264340) builds one descriptor at
0x140A59F10; the setup at VA 0x140226150 copies it into each car (called with that address from VA 0x1402E1646 and
VA 0x14029561A). Its layout is Rush 2's 0xEC-byte one with the mass in front:

| Offset | Value | Meaning (N64 2049's counterpart) |
|---|---|---|
| +0x00 | 109.472 | mass in slugs, 3525 lb (N64: a per-car table, 102.484 plus the frame) |
| +0x04-0x0C | 1000, 1527, 1636 | chassis constants (the N64 descriptor's +0x00-0x08, the same three) |
| +0x10-0x44 | -3600 ... -550 | 14 spring and damper values (the N64's, value for value) |
| +0x48, +0x4C | 16, 4 | suspension constants (same) |
| +0x50-0x54 | 0x0010, 0x0020, 0x0100 | s16 values, as Rush 2's +0x58 words |
| +0x58, +0x5C | 0.0087, 75 | drag (N64 0.0135), rolling resistance (same) |
| +0x60, +0x68 | | tire curve pointers |
| +0x70-0x9C | | wheel points (the N64 default descriptor's) |
| +0xA0 | 0.3 | preload (the N64's per-car 0.3) |
| +0xA4, +0xA8, +0xAC | 0.4, 3.3, 1000 | 0.4 (the N64 frame step K_ENG0), final drive, clutch |
| +0xB8-0xC4 | 0 | torque scales: zero for the player car; the arcade's engine isn't Rush 2's per-gear scale |
| +0xC8-0xDC | -3.1, 0, 3.1, 1.75, 1.25, 1 | gear ratios (N64 gear set 0: 1.78, 1.29 in 2nd, 3rd) |
| +0xE4, +0xE8 | 1047.2, 628.3 | shift points, 10000 and 6000 rpm (N64 662.35, 429.35) |

The only per-car tables, indexed by the car's type (car + 0x99E, CARn - 1), are the collision box (VA 0x140664130,
12 x {front, rear, half width, height}; the drones' at VA 0x1406EF824), the engine sound (bytes at VA 0x140663856 for
the player, 0x140663861 for drones) and three floats at VA 0x1406EF600 a draw routine reads. The four 0x44-byte records
at VA 0x14066390C (car + 0xB00, picked per car at VA 0x1402954E0) scale mass by game mode, not by car. VA 0x14026C390
builds four more descriptors (drone levels, torque scales 1.80-1.90 and 1.3) that the player car doesn't use.

`tools/arcade2049/car_mod.py` `physics()` maps this onto Rush 2 the way `build_physics` (src/rush2049/car2049.cpp) maps
the N64 cars. The arcade's mass, chassis constants, springs, suspension constants, rolling resistance, final drive,
clutch and preload go in as they are, and so does each car's box. The N64 port gives every car a 3.5 ft box height,
so the tool uses that in place of the table's 2. The drivetrain (torque, gears, drag, shift points) is the same for
every arcade car, so it lands on Rush 2's roster mean, as the N64 roster's average car does. The pitch/roll and yaw
inertia follow the mass by the N64's frame formula (2425 and 412500). Tire curves, rear grip, drive flags and
drivetrain inertia are the N64 2049 car's. Steering, yaw damping and the SUSPENSION and TIRES settings are Rush 2's
mean, as for the N64 cars.
