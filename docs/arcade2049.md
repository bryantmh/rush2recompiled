# San Francisco Rush 2049 arcade (SE) tracks

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
6 flipbooks (runway chaser lights, sirens, blinkers). A frame is hidden by scaling it to 0.001 about the mover's pivot; src/interpolation.cpp snaps (doesn't
blend) a node whose scale changes more than 4x in a frame, or the runway lights drew shrunken copies between the loop
center and the loop.

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
  so in it they painted a dark plane across the dome. They stay scenery. The dome (27,600 ft) is scaled to fit the
  sky model's s16 vertices (about 2,048 ft); Rush 2 moves a model sky with the camera in x and z only, so its top sat
  near 520 ft, under the island's top. A custom track's SKY mesh now follows the camera's height too
  (`rush2_track49_sky_height`, render_sky_cloud_scroll_8008918C at 0x800891F8).
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
