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

## Building it in the game

Players don't need Python: the Games tab's San Francisco Rush 2049 Arcade section (Build and Install, Select Folder)
builds both mods from the arcade data with `src/arcade2049/`, a C++ port of `track_mod.py`,
`car_mod.py` and `arcade.py`, and installs them as `<mods>/rush2049_arcade.rtz` and `rush2049_arcade_cars.rtz`
(a loose folder of the same id, the Python install, is replaced). It takes about 2.5 s for Alcatraz, 32 s for every
track (see Alcatraz Only below). Each mod.json carries
`"rush2_arcade": {"key", "data"}`: the hash of the input files, the shipped assets and the build. At launch
(`start_refresh`, from the launcher, on a worker thread) an installed mod whose key changed is built again once from
that folder, keeping whether it was on, so a new build or new arcade data never needs the button. If that folder is
gone, it builds from wherever the button would look (`data_dir`: ProjectR's folder); a loose (Python) install is left
alone. A race on an arcade track while the installed track mod is still out of date doesn't wait for the refresh or
race the old version: `rush2::track_mod::load` asks `a49::stale_track_files`, which builds just that track
(`build_track_files`, the files `build_track_mod` writes for it, about 3 s for Marina) during the race load, and
converts it as any track converts on its first race (keyed by its files, so the installed copy, once the refresh
writes it, hits the same cache entry). Until 2026-10-20 a race in the first half minute after a new build raced the
old track until the game was restarted. The inputs' hash (`source_key`) is worked out once per data folder and mod.
Test: `tools/test_race.py marina_arcade --mod-dir <a copy of the .rtz with its mod.json key changed>` (the log says
"built again for this race"); `RUSH2_TEST_RACE_WAIT=<s>` starts the race that long after the title screen instead. Afterwards `rush2::car_mods::reload` and `rush2::track2049::mods_changed` make the car select and
track select read the new files without a restart; `mods_changed` also has the custom track list read again
(`rush2::track_mod::rescan`): it was kept by the enabled mods and their file names, which a rebuild keeps, so until
2026-10-19 tracks added by a rebuild (Alcatraz Only turned off) showed only after a restart.

- **Alcatraz Only** (Games tab, under the section, default on; `arcade_alcatraz_only` in games.json): on, the track
  mod holds just Alcatraz (TRACK4). Off, it also holds the arcade's own versions of the six Rush 2049 tracks as tracks
  of their own after the Rush 2049 ones (`a49::arcade_tracks`: TRACK1 `marina_arcade`, TRACK2 `haight_arcade`, TRACK3
  `civic_arcade`, TRACK5 `metro_arcade`, TRACK6 `mission_arcade`, TRACK7 `presidio_arcade`, each "<Name> Arcade").
  The track select shows them in the N64's order with Alcatraz last: each track.toml's `[menu] after` names the track
  before it in `arcade_tracks` (the first `rush2049`); until 2026-10-19 they all followed `rush2049` and showed in
  folder name order.
  The option is part of the mod's key, so changing it rebuilds an installed mod in the background (and once more if
  it changes during a build); without the arcade data a copied mod keeps the tracks it was built with. Their logos
  are the N64 tracks' own banners with " A" after them (`tools/build_banners.py --arcade`: the font-drawn names drawn
  again as "MARINA A", Metro's and Presidio's picture banners kept, narrowed, with the "A" beside them), and their
  round previews are the user's in-game screenshots (`tools/arcade2049/previews/<id>.png`, cut by
  `tools/arcade2049/track_mod.py --ship-previews` into `assets/arcade2049/<id>_preview.png`). Only Alcatraz sets a
  fog color (its night). `ARCADE_ALL=1` makes cpp_test build them all. Offline
  (`tools/track_mod_test.bat` + `tools/track_mod_check.py` on the C++ output) all seven pass. Haight and Presidio
  needed the collision steps under "roaddat.rom" below (a cone tip on Haight; copies and a tunnel seam on Presidio)
  and Presidio, at 13,047 polygons, the converter's wide leaves (docs/custom_tracks.md: until 2026-10-19 the game took
  8,191). Presidio's checker run reports 543 lane points with no ground under them: the arcade's own collision has
  none there either (its long jumps). In game (`tools/test_race.py presidio_arcade --mod-dir <cpp_test
  output>/rush2049_arcade --drones 4`) the drones race Marina, Haight and Presidio Arcade past their checkpoints.
- **Arcade Checkpoints** (Games tab, default off; `arcade_checkpoints`): see "Arcade checkpoints" below.
- **Keep the two in step.** A change to `track_mod.py` or `car_mod.py` goes into `src/arcade2049/` too. Check with
  `cmd //c "tools\arcade2049\cpp_test\build.bat tmp\arcade\cpp"` (the C++ outside the game, about 2.5 s)
  against the Python output: the car files come out identical, the track's scenery, gates, AI lanes, movers and
  materials identical. Then run both tracks through `tools/track_mod_test.bat` and `tools/track_mod_check.py`.
- **Where they differ:** shapely's polygon work in `spline_road` is Clipper2 (`lib/Clipper2`, collinear corners kept) and
  earcut (`lib/earcut`) instead of constrained Delaunay. The footprint area per segment matches within 0.03%. The
  converted collision has 6,845 polygons against 6,602 (index cells within 36 on both) and the same ground height
  along the lap, with fewer enclosed cracks than the Python output (452 vs 536 under 50 sq ft). `crowded_cells`
  measures distance to each cell instead of buffering, and gives the same joins.
- **Shipped pictures:** each track's logo and round preview are fixed pictures shipped as
  `assets/arcade2049/<id>_logo.png` and `<id>_preview.png` (both part of the mod's key). Alcatraz's logo is
  `banner_font.render_fit`; the others come from `tools/build_banners.py --arcade`, and every preview from
  `tools/arcade2049/track_mod.py --ship-previews`. Regenerate them if a source changes.
- **TRACK8** is SF Rush's Track 2, Embarcadero, left on the 2049 disk. It is The Rock's TRACK2 (same 200 textures,
  220 road segments) with checkpoints matching SF Rush 64's exactly, and it is already in the game from the N64 ROM.
  Its drone path is stale data from another track. `arcade.py` now reads its collision: its vertex lists follow the
  leaves, and the file ends in 2 bytes of padding.

## Where the data is

ProjectR (a native port of the arcade games) unpacks the arcade disk (`sf2049se.chd`) to
`%LOCALAPPDATA%\ProjectR\Data\sf2049se` (`ARCADE2049_DATA` overrides it): `TRACKS\TRACK1`..`TRACK8` and `DEV`, `CARS`,
`SELECT` (the select screens' textures, same format as a track's), `STATIC`, `AUDIO` and the rest. Per track: `dobs`,
`dpath`, `objects`, `roaddat`, `textures`, `worlds`, `vlut`, `vl_zpath` (`.rom`), all little-endian and uncompressed.

| Arcade | Track |
|---|---|
| TRACK1, 2, 3, 5, 6, 7 | the N64's Marina, Haight, Civic, Metro, Mission, Presidio (matched by their paths) |
| TRACK4 | **Alcatraz** (arcade only) |
| TRACK8 | SF Rush's Embarcadero (The Rock's TRACK2), named STUNT_TRACK2 in `roaddat.rom` +0x1C, its `dobs` empty |

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

u32 1, u32 0x18, a 16-byte TEMP record, then 0x4C-byte instances: char name[15], u8 1, f32 m[9], f32 pos[3], u32,
s16 next, s16 -1, u32. Bytes after a name's NUL are leftovers of longer names, so names were read as 8 characters
(`object_named` matches by prefix); but a name that fills 8 characters runs on: `AF_G3226` then `5` is `AF_G32265`.
Read at 8, arcade Marina, Haight, Civic and Presidio lost 64, 107, 355 and 469 placements (the AF_/FF_/AL_/BL_/LP_
objects, ambiguous at 8); the readers now take the long name when it is an object. Every Alcatraz matrix is identity.

## dobs.rom (dynamic objects)

u8 1, then records: u8 name length (with its NUL), the name (can be longer than the object table's 15 characters; a
flipbook's frames are `<name>_1..n`), u16 script length, the script (CRLF lines, split into words at `:=#`, spaces, tabs and line ends, ProjectR
0x1406EA215: `#` is not a comment, so `#followy 70 450 20` moves the object; Marina's four `#` trains and Metro's seven
trolleys run from the start, which the converters missed until 2026-10-20), f32 m[9], f32 pos[3]. Script
commands and what the converter makes of them:

| Command | Meaning | Converted to |
|---|---|---|
| `frontface` | a card turned to the camera | `BILLBOARD_` |
| `bounce`, `box x y z`, `point x y z` | knocked when hit (box or point hit shape) | `PROP_KNOCK_` |
| `coin` | a score pickup, not Rush 2049's hidden coins | left out (mods have no collectibles) |
| `spinx`/`spinz <deg/s>` | turns about its own x / upright axis, as its matrix turns it **[I]** (degrees per second fits the lighthouse and police lights) | looping `MOVE_` (with its collision and `start` if it has them) |
| `flipbook <fps>` | steps through `<name>_1..n` at that rate; negative: once, on its start event (breaking glass, the finish sign) | one `MOVE_` per frame, shown by scale steps; an event flipbook is `mode = "once"` with its `start` |
| `alpha 0` / `alpha 1` | blending mode (not decoded) | the texture's own alpha |
| `sound`, `soundsphere <name> <range> <volume>` | object and ambient sounds | left out |
| `followx`/`followy`/`followz <speed> <distance> [<acceleration>]` (`follow-z`: the negative axis) | slides along its own axis (the script's x, y, z are world x, z, y) at `speed` ft/s for `distance` ft, easing in and out at `acceleration` ft/s² **[I]**: the Marina trolley's 300 ft, the stunt ramp's 110 ft | `MOVE_` with keys along the axis (6 per ramp), `mode = "pingpong"` (back and forth) or a door |
| `start when <event>` (or `start` then a `when <event>` line) | the motion waits for the event | `start = "<event>"` |
| `open when <event>`, `close when <event>`, `open delay N`, `close delay N` | a door: plays forward on open, back on close; a delay closes / reopens it by itself N s later | `mode = "door"`, `open`, `close`, `open_delay`, `close_delay`; `open_at_start` when the first event line is a close |
| `name <X>` with `hide` | a hidden box (UNITCUBE mostly) whose `when X` fires when a car drives into it | `TRIGGER_X` (only when an event uses it) |
| `collision` | cars hit it (`collision 1` / `2`, the number not decoded) | its full mesh as `COL_ROAD_` (faces with normal y > 0.7) and `COL_WALL_` (the rest), moving with it; a card gets a box |
| `hide` without a name | a sound's position | left out (listed in the report) |
| `UNITCUBE` without `hide` | a sound volume (`soundbox`) | left out: the arcade's blue placeholder cube, on Marina a 495 ft blue block over the road |

Events (`when ...`), as the converter reads them:

| Arcade event | Meaning | track.toml event |
|---|---|---|
| a trigger name, such as `mg1` | a car drives into the hidden box named `mg1` | `"trigger MG1"` |
| `progress <feet>` | the leader that far into the race: its lap counter x the arcade's lap length + feet into the lap **[V]** | `"progress <n>"`, the same lap and point on the converted lap (laps x its length + the feet into the lap) |
| a number, such as `3` | seconds into the race **[V]** (x 100,000: a 100 kHz clock) | `"time 3"` |
| `oddlap`, `evenlap` | a new lap, odd / even **[V]** | `"odd_lap"`, `"even_lap"` |
| `lastlap` | the last lap starts **[V]** (no track uses it) | `"final_lap"` |
| `finishswap` | player 1 (car 0) passes the race's last checkpoint before the finish **[V]** (the finish sign swaps; every track has one) | `"last_checkpoint"` |

How the arcade runs them, from ProjectR's native port (`project-r.exe`, read with `llvm-objdump -d`,
`tools/arcade2049/pe_xref.py` for references to a string and `tools/arcade2049/pe_read.py` for constants; the dobs parser at 0x1402D7A80 reads the
word after `when` and compares it with `progress`, `finishswap`, `lastlap`, `oddlap`, `evenlap`, else a number):
- `progress`: the value (a float) goes into a list kept sorted by it (0x14029E680, head 0x140951408). Each frame
  0x14029E8C0 takes the largest progress of the race's cars (count 0x140A1CB39, 0xB48 apart) and fires every entry
  below it, once. A car's progress (0x140233840) is its lap counter (0x1408B9268, s8 per car) x the track's lap length
  (table 0x1407A9FE4, f32 per track: 12,600 Marina, 18,000 Haight, 17,000 Civic, 15,900 Alcatraz, 19,700 Metro, 0
  Mission and Presidio, 3,200 the stunt track) + its distance along the lap path; the counter goes up when that
  distance falls by more than half a lap and down when it rises by as much. The lap path starts at the finish line
  (FINISHSIGN is within 15 ft of its first point on Marina, Haight and Metro). With a 0 lap length it is the car's
  own +0x108 instead (Mission and Presidio, which have progress.rom; no progress events there).
- `finishswap`: fires once (0x14029E360, from 0x1402934E0) when car 0's progress passes 0x140950E3C, the distance of
  the race's last checkpoint before the finish (written while the checkpoint table 0x14094EE34 is built, 0x140293948).
- `lastlap` (0x14029E4D0) fires when the lap number reaches the race's laps (0x14094EE30); `oddlap` / `evenlap`
  (0x14029E530) by the new lap's low bit.
- A number is seconds: x 100,000 (0x140621760) into a timed event (0x14029E3C0).
So on Marina and Metro the trains do wait for lap 2: Marina's `progress 14000` is 1,400 ft into lap 2, Metro's
`progress 32000` 12,300 ft into lap 2 (the N64 version runs them from the start). Until 2026-10-20 the converter made
these a 140 ft box on the lap point a car had to drive through on that lap (often missed in play) and read
`finishswap` as the last lap's start.

An event the converter doesn't know is listed in the report and the object plays freely. Marina's timeline, checked
with `movers_test.exe --script` (docs/custom_tracks.md#for-developers): the button box opens the ramp 17 ft and it
closes 8 s later, the box itself closes and reopens 8 s later; the train starts 1,400 ft into lap 2 (`progress 14000`) and the trolley
slides 300 ft 12,400 ft into lap 2 (`progress 25000`); a second ramp rises 15 ft 11,466 ft into lap 3 (`progress
36666`); leaving the Fuji building (`stv_fujiexit`) raises the finish stunt ramp 110 ft and closes the fences; at the
last checkpoint the finish sign swaps.

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
spline surface, whose vertex list ends with its u16 segment index; the high word is Rush 2's POLY info upper bits, bits
30 / 31 the covered levels 1 / 2 (tunnels: the cars drawn darker, docs/rush2049_research/collision.md#car-lighting),
kept as `covered` on separate `COL_<SURF>_covered<n>` meshes and never joined across levels. The N64 2049's per-polygon
light indices (its car shading map) are N64 only: the arcade's info word is 0 there, and `vlut` / `vl_zpath` are
visibility lists, not lighting. Arcade Marina matches the N64 collision to 0.04
ft. Where more polygons crowd a spot than a collision cell's 255 (36 before the port moved the
queries' lists off the stack, docs/rush2049_research/collision.md#leaf-list-buffers-port-only-v), the converter welds corners and joins flat
neighbors (`relieve_crowding`; on Alcatraz about 130 pairs, corners moved 1.4 ft at most). Before that, both readers'
converters (`arcade_track.cpp` and `track_mod.py`, the same steps):

- `drop_spline_twins`: some spline road polygons are listed twice, once with their segment and once flat (same corners
  and facing): the flat copy is dropped, since the spline copy becomes the smooth road. Presidio 74 (its tunnel, where
  the pairs crowded the seam between two tunnel sections past 36), Metro 4, Civic 1.
- `drop_copies`: exact copies (surface, corners, facing) are dropped: they change nothing but share each edge four
  ways, so the pair joins never took them (Presidio 119, Marina 165, Haight 131, Metro 103).
- `cut_tips`: a corner shared by 240 or more polygons (24 under the old 36) all the way round is a cone tip (wedges at an angle
  can't be joined flat). Under the old limit it cut Haight's pit (31 steep wedges at 2653, 76, -3305); at 240 the pit is
  left as it is. Each polygon's corner is replaced by the points 12 ft
  out across x/z on its two edges (the neighbors sharing an edge get the same point: no cracks) and the hole is
  capped with convex pieces of at most 8 corners over that ring. A fan with a gap over 60 degrees is left alone.
- Then, where a 4 ft cell holds more than 240 (24 under the old 36), exactly flat neighbors are joined first, nothing moved (Presidio 45): the
  game's converter places its cells its own way and splits faces its own way, so it can count a few more than this
  check does.

The custom track converter also splits a cell still past 255 below 4 ft, down to 1 ft (docs/custom_tracks.md), which
Presidio's tunnel portal needed (13 pieces of one flat ring wall where two tunnel sections meet).

The file is a run of such blocks, each starting with its u32 size; the track's collision is the largest (`roaddat()`
in both readers). It is the first block everywhere but TRACK6 (Mission), whose file starts with a 13-polygon block
(1,368 bytes) before the 712,348-byte one; TRACK7 has three small blocks after its own (not decoded, perhaps the
movers').

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
  and every object using a texture that at least two far objects use and that far objects make up at least half the
  users of. The other tracks' skies are a ring of wall panels under a flat ceiling, centered off the origin, so a few
  near-side panels reach less than 9,000 ft: Marina G27322-G27323, Haight G20137-G20139 (under ceiling G20132),
  Civic AF_G27162-AF_G27163, Metro G23003-G23005, Presidio G14557-G14558 (Mission's ring is all past it). As scenery
  the 2,000 ft draw distance cut them off, so the sky had a gap and the ceiling's edge showed as a box overhead. The
  texture rule adds exactly those panels and nothing else on any track. Excluded: the bay's WATER2 sheets (G6688-G6693, out to 10,700 ft): the sky is drawn without depth around the camera,
  so in it they painted a dark plane across the dome while the sky kept its world height. They are scenery (the near
  part, depth tested) and sky too: the game draws 2,000 ft, and the sky's water meets the distant land (GS_APANS) as in
  the arcade. Sky materials go in draw order: dome, water, land, clouds. The dome (27,600 ft) is scaled to fit the sky
  model's s16 vertices (about 2,048 ft); Rush 2 moves a model sky with the camera in x and z only, so its top sat near
  520 ft, under the island's top. The sky is now placed at (1 - scale) x the camera's height
  (`rush2_track49_sky_height`, render_sky_cloud_scroll_8008918C at 0x800891F8), as it would look at full size.
- `COL_*`: `roaddat` polygons by surface, the spline road as footprints on its segments (above). `roaddat` has no water: under the bay is sea floor (road) and walls, and
  the arcade drowns cars some other way (not found). The WATER2 sheets (the bay, at sea level) become `COL_WATER`,
  which wrecks and resets the car as Rush 2's own Alcatraz water does; no road polygon or lap point is under them.
- `FINISH` at the finish sign (the FINISHSIGN dob, else a TE_FINISH sign in worlds.rom, else the lap's first point:
  Mission has neither placed); `CHECKPOINT_n` at the checkpoint banners (the arcade's own gate data wasn't found):
  objects drawn with MG_CHECK5 / MG_W_CHECK6, or TE_CHKPNT on Mission and Presidio. A banner piece farther than 120
  ft from the lap is a sign off the course (three on Mission), and banners within 250 ft along the lap of the one
  before (or of the finish) are pieces of it (Marina's stacked pair). Checkpoints: Alcatraz 4, Marina 4, Haight 4,
  Civic 3, Metro 3, Mission 5, Presidio 3.
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

## Arcade checkpoints

The N64 Rush 2049 tracks mark their checkpoints with nothing. The Games tab's Arcade Checkpoints option
(`src/arcade2049/arcade_checkpoints.cpp`, default off, takes effect at once) hangs the arcade's CHECKPOINT banner
(texture MG_CHECK5, 256 x 64, the same on arcade tracks 1-5) over every checkpoint of the six Rush 2049 race tracks
but the start / finish line (it has its own sign).

- **Where the banner comes from:** Build and Install writes it into the track mod as `checkpoint.png`, so a copy of
  `rush2049_arcade.rtz` brings it to an install without the arcade data. It is read on a worker when the option is on
  (`load_image`), shrunk to a 64 x 16 RGBA16 stand-in in RDRAM 0x80E1E000, and drawn at full size as a mod texture
  (`rush2::derive::set_mod_textures("arcade_checkpoints")`).
- **Where it hangs:** from the race's own path (Rush 2's header copy at 0x8010BCE8: s16 finish checkpoint +4, count +8,
  0x50-byte checkpoints from +0xC), so backward races get theirs. Each banner is centered between the outermost AI
  lanes where they cross the gate's line (`rush2::battle::checkpoint_lane_middle`, from battle.cpp's lane crossings),
  stands on the ground there along the ground's up vector (`rush2::battle::ground_under`,
  collision_ground_query_8006CF00's matrix row 1), so it leans with a sloped or banked road, and faces along the gate.
  It is a 64 x 16 ft quad 22 ft above the ground (the arcade's hang 8-18 ft tall, 40-130 ft wide, about 25 ft up),
  with a mirrored second face 0.25 ft behind so it reads the right way from both sides. Placements are worked out when
  the path's checkpoints change.
- **Drawing:** rush2::battle_render slots 176-185 with a model of the port's own (`battle_render::add_model`): its
  display list at 0x80E1E900 and vertices at 0x80E1E800. It is drawn without fog: the fog in place when battle_render
  draws (the end of each view) isn't the scenery's, and with G_RM_FOG_SHADE_A the banner came out in the fog color
  (the sky's) from about 100 ft away, so it showed only from under it.
- **Test:** `tools/test_race.py 12 --env RUSH2_TEST_RACE_VIEW_CAR=1 --env RUSH2_TEST_ARCADE_INSTALL=1 --games
  arcade_checkpoints=true --shots ...` (a drone passes Marina's first gate about 8 s into the race);
  `RUSH2_TEST_ARCADE_CHECKPOINTS=finish` also hangs one over the start line, in view at the start.
