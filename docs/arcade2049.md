# San Francisco Rush 2049 arcade (SE) tracks and cars

The arcade game (Rush 2049 Special Edition) has a track the N64 never got: Alcatraz. `src/arcade2049/arcade_track.cpp`
converts an arcade track into a custom-track mod (docs/custom_tracks.md) at the arcade's full model detail and texture
size, and `src/arcade2049/arcade_cars.cpp` the cars into a car mod; the Games tab runs both (below).
`tools/arcade2049/arcade.py` reads the data for study. Nothing here is shared with the N64 or Dreamcast formats. The
same converter builds the SF Rush arcade (Rush The Rock) tracks, whose data is laid out the same way, with a few rules
of their own: docs/sfrush_arcade.md.

```
python tools/arcade2049/arcade.py TRACK4          (a summary of the track's files)
cmd //c "tools\arcade2049\cpp_test\build.bat [OUT_DIR] [DATA_DIR]"   (both mods built outside the game)
```

Rush 2049's ROM in the Games tab makes the props react (docs/custom_tracks.md, "Props"). The Python converters these
were ported from (`tools/arcade2049/track_mod.py`, `car_mod.py`) were removed on 2026-10-20; their explanations are in
the C++ and this page.

## Building it in the game

The Games tab's San Francisco Rush 2049 Arcade section (Build and Install, Select Folder) builds both mods from the
arcade data with `src/arcade2049/` (its reader a port of `arcade.py`) and installs them as
`<mods>/rush2049_arcade.rtz` and `rush2049_arcade_cars.rtz` (a loose folder of the same id, an install by the old
Python converters, is replaced). The tracks are built at once, a thread each, and put together in the track select's
order (byte for byte the sequential build): about 0.8 s for every track since 2026-10-21 (3.3 s one after another, 29 s
before `crowded_cells` counted in blocks; see Alcatraz Only below). Each mod.json carries
`"rush2_arcade": {"key", "data"}`: the hash of the input files, the shipped assets and the build. At launch, before the
game starts (`a49::refresh_now`, the first job of the launch's build pass, docs/prewarm.md#what-is-built), an installed
mod whose key changed is built again from that folder, keeping whether it was on, so a new build or new arcade data
never needs the button and no race meets an out of date mod. If that folder is gone, it builds from wherever the button
would look (`data_dir`: ProjectR's folder); a loose (old Python) install is left alone. Loading the Alcatraz Only setting
starts no build of its own (the refresh compares the key, which holds it); changing it in the Games tab rebuilds in the
background. The inputs' hash (`source_key`) is kept on disk under the input files' names, sizes and times
(`arcade_source_key_*` in the track cache), so a launch reads the arcade files only when one changed: hashing them took
about a second of every launch. Until 2026-10-21 the refresh ran beside the game and a race on a stale track built that
track again during its race load (`stale_track_files`, a black screen of 2-4 s). Test: `tools/test_race.py menus
--mod-dir <a copy of the .rtz with its mod.json key changed>` (the log says "installed" before "launch builds done"). Afterwards `rush2::car_mods::reload` and `rush2::track2049::mods_changed` make the car select and
track select read the new files without a restart; `mods_changed` also has the custom track list read again
(`rush2::track_mod::rescan`): it was kept by the enabled mods and their file names, which a rebuild keeps, so until
2026-10-19 tracks added by a rebuild (Alcatraz Only turned off) showed only after a restart.

- **The arcade's versions of the six race tracks.** The track mod holds Alcatraz (TRACK4, `[menu] after =
  "rush2049"`) and the arcade's own versions of the six Rush 2049 race tracks (`a49::arcade_tracks`: TRACK1
  `marina_arcade`, TRACK2 `haight_arcade`, TRACK3 `civic_arcade`, TRACK5 `metro_arcade`, TRACK6 `mission_arcade`,
  TRACK7 `presidio_arcade`, each "<Name> Arcade"). Each of the six is a version of its course (`[menu] variant_of =
  "rush2049:<course>"`, `variant = "ARCADE"`): it shares the course's track select entry with the N64 and Dreamcast
  versions, and the version button switches to it (docs/custom_tracks.md#versions). Until 2026-11-05 the Games tab's
  **Alcatraz Only** option (`arcade_alcatraz_only`, default on) left them out, and with it off they were entries of
  their own after the Rush 2049 tracks; the option is gone (a games.json still holding it is ignored) and the mod
  always holds all seven, rebuilt once at the next launch as its key changed. Their logos are the N64 tracks' own
  banners (`tools/build_banners.py --arcade`; until 2026-11-05 with an " A" after the name, which the version button's
  ARCADE replaces), and their round previews are the user's in-game screenshots
  (`tools/arcade2049/previews/<id>.png`, cut by `tools/build_banners.py --arcade` into
  `assets/arcade2049/<id>_preview.png`). Only Alcatraz sets a fog color (its night). Offline
  (`tools/track_mod_test.bat` + `tools/track_mod_check.py` on the C++ output) all seven pass. Haight and Presidio
  needed the collision steps under "roaddat.rom" below (a cone tip on Haight; copies and a tunnel seam on Presidio)
  and Presidio, at 13,047 polygons, the converter's wide leaves (docs/custom_tracks.md: until 2026-10-19 the game took
  8,191). Presidio's checker run reports 543 lane points with no ground under them: the arcade's own collision has
  none there either (its long jumps). In game (`tools/test_race.py presidio_arcade --mod-dir <cpp_test
  output>/rush2049_arcade --drones 4`) the drones race Marina, Haight and Presidio Arcade past their checkpoints.
- **Arcade Checkpoints** (Games tab, default off; `arcade_checkpoints`): see "Arcade checkpoints" below.
- **Checking a change.** `cmd //c "tools\arcade2049\cpp_test\build.bat tmp\arcade\cpp"` builds both mods outside
  the game (about 2.5 s); run the tracks through `tools/track_mod_test.bat` and `tools/track_mod_check.py`.
- **Against the Python port's output** (until it was removed): the car files came out identical, the track's scenery,
  gates, AI lanes, movers and materials identical. The spline road's polygon work is Clipper2 (`lib/Clipper2`,
  collinear corners kept) and earcut (`lib/earcut`) where the Python had shapely and constrained Delaunay: the
  footprint area per segment matched within 0.03%, the converted collision had 6,845 polygons against 6,602 and the
  same ground height along the lap, with fewer enclosed cracks (452 vs 536 under 50 sq ft).
- **Shipped pictures:** each track's logo and round preview are fixed pictures shipped as
  `assets/arcade2049/<id>_logo.png` and `<id>_preview.png` (both part of the mod's key), all made by
  `tools/build_banners.py --arcade` (Alcatraz's logo with `banner_font.render_fit`). Regenerate them if a source
  changes.
- **In the menus they count as Rush 2049's**, decided by the mod's id (`rush2049_arcade`, `rush2049_arcade_cars`),
  which no track.toml or car file can claim. The game filter files the tracks and cars under Rush 2049, not Custom
  (`rush2::track2049::mod_from_2049`, `rush2::car2049::mod_car_from_2049`). The cars list right after the 2049 cars
  (`rush2::car2049::list_order`, used by the car select's list and the unlock system's). Alcatraz's `after =
  "rush2049"` puts it after Presidio: the anchor skips a course's other versions (the Dreamcast ids 249-254).
  The six ARCADE versions show the 2049 course's route tube and color in front of their own picture, with a red
  version ring (docs/menus.md#track-versions).
- **TRACK8** is SF Rush's Track 2, Embarcadero, left on the 2049 disk. It is The Rock's TRACK2 (same 200 textures,
  220 road segments) with checkpoints matching SF Rush 64's exactly; the SF Rush arcade mod has it as Embarcadero's
  ARCADE version (docs/sfrush_arcade.md).
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

## Music

`AUDIO` holds the arcade's music as `.str` streams: `track1`..`track7` (one per TRACKn, 185-278 s), `therock`
(TRACK8), `select`, `finish`/`finish1-3`/`gameover` (91 s), `freegame`, `atari`, `movie1` and twelve 14 s `peelout`
stings. Format: `STRM`, u32 data bytes, u32 0x36, u32 0x52, zeros to 512 bytes; the file is 5.1 PCM16 frames
(FL FR C LFE BL BR, LFE always 0) counted from byte 0, 22050 Hz [I].

None of them is a Dreamcast disc song (and so none is an N64 song, which arrange the disc's): `tools/rush2049/song_match.py`
slides a 40 s chroma clip of each over all 18 disc `.STR` songs, and no arcade stream picks out one disc song
(best 0.22-0.64, each with a runner-up within 0.01-0.08), and the same-named `therock` against the disc's `THEROCK.STR` stays
at 0.25 or less at every rate from 11 to 50 kHz with any key shift; real arrangements score 0.54-0.87, see
docs/rush2049_research/audio.md). So the arcade's soundtrack is its own [I]; confirm by ear with `song_match.py ... --wav OUT`.

The game plays them (src/music_streams.cpp) straight from the arcade data folder, nothing converted: the first whole
frame after the header is frame 43 (byte 516); the six channels carry different parts (the centre and rears hold
their own instruments, uncorrelated with the fronts), so they are folded to stereo (FL + 0.707 C + 0.707 BL, likewise
right; src/stream_decode.cpp), and leveled to Rush 2's songs: folded, they measure -6.4 LUFS as files against Rush 2's -16.1 in the game, which
halves every source at the output (`file_song_makeup`, include/audio2049.h), so they play at 0.656 (-3.65 dB). Each
TRACKn's song is the Original of its arcade version (`track4` Alcatraz's); `therock` (TRACK8's) is no raced track's
own song, so it plays only in shuffles; `select` is a main menu song (Sound tab: Rush 2049 >
Arcade > Main Menu). They are listed under Rush 2049's Arcade heading in the Sound tab.

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

Name prefixes set object flags **[V]** (ProjectR 0x1402D69EE, the per-instance worlds loader; strings at 0x1406EA205):
`FF_` 0x2000000, the same flag dobs' `frontface` sets (0x1402D83B5), so `FF_` placements are cards turned to the camera;
`AF_` 0x20000, `AL_` 0x80000 (not decoded); `LP_` 0x4000090, the same as dobs' `lightpoint`. An identity matrix adds 0x10.
`FF_` placements are mostly trees: Marina 34, Haight 74, Civic 286, Alcatraz 0, Metro 1 (the moon), Mission 133,
Presidio 320 (plus bridge glows and glare sprites). The converter makes them `BILLBOARD_` as it does `frontface` objects
(`src/arcade2049/arcade_track.cpp`, `Instance::card`; until 2026-11-05 they were fixed scenery, edge-on or missing
from most angles). Each is a flat quad centered on its own origin with an identity matrix, so it turns in place.

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
| `coin` | a score pickup, not Rush 2049's hidden coins | left out (the N64 course's coins are placed instead: [Coins and keys on the arcade versions](#coins-and-keys-on-the-arcade-versions)) |
| `spinx`/`spinz <deg/s>` | turns about its own x / upright axis, as its matrix turns it **[I]** (degrees per second fits the lighthouse and police lights); a positive rate is right-handed in dobs.rom's (x, z, y) axes, left-handed in world axes (every plane's pointed nose leads that way; the converter turned them right-handed in world until 2026-10-21 and they flew tail first) | looping `MOVE_` (with its collision and `start` if it has them) |
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
ft.

### Wall heights (N64 port raised them)

The N64 port's collision keeps the arcade's walls but raised many of their tops. Marina's shortcut ramp between the
lap road (y 557) and the train deck (y 643-667, the `#followy` trains at y 681), x 4370-4700, z -470 to -230: the
same 46 wall polygons (roaddat 5933-6009, flags 0x55, info 0) stand 8-11 ft tall in the arcade and 18-22 ft on N64
(file 139, polygons 1056-1114: same bottoms and footprints, tops about 10 ft higher), and in the recomp the rail
cuts through cars driving on the ramp (below). The arcade polygons carry nothing else (info word 0, no flag bits) that would make
them taller. `tools/arcade2049/wall_compare.py <TRACK> <file>` matches each arcade wall to its N64 one (x/z within 2 ft, same
facing): Marina (139) 526 raised (median 10 ft), 1,332 the same, 33 lower, 1,002 unmatched; the other tracks match
much less (Haight 140: 115 raised / 687 same / 1,044 unmatched; Civic 141, Metro 142, Mission 143, Presidio 144
similar or worse), so the N64 reworked them more. Alcatraz has no N64 version. The arcade's wheel/ground probe
(project-r.exe 0x1402C5E40, polygon test 0x1402C6210) is Rush 2's kind: a point counts for a polygon only when it
projects inside it along the polygon's normal and is under 120 units (3 ft) in front; a type 5 hit sets a wall flag
(0x1402C6020). Its contact (0x1402C6B80, same four point slots car + 0x720..0x738) takes the polygon of any
type the point projects into, from 10 units (0.25 ft) behind to 120 (3 ft) in front, and pushes along that
polygon's normal; the type (flags & 0xF) is read only after. So in the arcade a steep road-typed face holds cars as
a wall does, while Rush 2's body probe takes only types 5 and 6. Steep (normal y within ±0.7) type 0 polygons: Marina
634 (N64 234), Haight 128, Civic 184, Alcatraz 912, Metro 348, Mission 81, Presidio 4,572. Marina's elevated train
deck (road y 667 at x 3365-3985 with 0x55 rails 3 ft over it, then y 643 at x 4370-4690, spline segments 3-8; the
`#followy` trains start over it) is lined at x 4370-4690 by such faces (roaddat 5700-5757, y 631-665); the N64 file
has 15 type 5 walls there. Checked 2026-10-21: the converted collision.bin keeps the ramp walls exactly (corners,
heights, facing) and the drawn barrier's top (y 634.1, `tools/arcade2049/glb_near.py`) is the arcade wall's, so the
roaddat decode is right; what differs is how the two games use the types. The lookup tree is right too:
`tools/arcade2049/wall_reach.py <collision.bin>` looks up points 0.25-3.75 ft in front of every wall face (the body
probe's reach) with Rush 2's cell rule, and on Marina all 12,927 per distance find their wall. The user's case
(2026-10-21, screenshot): on the lower ramp, wheels on the road, the rail cuts through the car body. The walls are
present and in the tree, but only as tall as the drawn rail (7-9 ft over the ramp road; the N64 made them 17-19), and
Rush 2's body points pass over a wall only as tall as the drawing, as on California Speed (docs/calspeed_research.md,
Monterey's 3 ft stone wall has an 88 ft collision wall). Measured in game (2026-10-21, Marina Arcade, car 0 read
from RDRAM): the body points (car +0x260) ride 3.5 ft over the wheel contact points (+0x230), their offsets (+0xE8)
y 3.0 from the car box's height (0x8021A000 + type x 16, +0xC; 3 ft for every stock and mod car). The train deck's
rails top out 3.4 ft over its road (y 670.6 over 667.2, walls 5869/5870 of the converted file) and the ramp's
about 3.5 ft over the road beside them, so the body points pass just over them, and the wheels don't hold the car either
(the user's screenshot; the wheel probe `collision_poly_extract_8006F704` skips types 5 and 6 at 0x8006F89C),
where the arcade's wheel contact takes any polygon: 0x1402C6B80 is called once per wheel (0x1402C8919-0x1402C8A27,
the four cases each writing that wheel's compression, car +0x5C8-0x5D4), so in the arcade a wheel that reaches a
wall polygon is pushed out along its normal, and a rail as low as the wheels holds the car. The polygon taken is the
one the wheel's contact point is deepest in (0x1402C6210's distance is along column 2, minus the normal, in 1/40 ft:
over -10 and under 120), of any type; a type 5 gets `0x1402C7090`'s force, -16000 x (depth + 0.25) - 2000 x the
speed into the face (never pulling; past 0.25 ft deep car +0x588 x +0x75C x (speed + 1) x -0.5), a floor
(p < 0: car +0x590 x -0.25, else (-4 depth - speed) x +0x588 x 0.25 x +0x75C) its suspension, clamped to +-1e6.
Not ported (two tries, 2026-10-21, reverted: walls taken as floors in Rush 2's wheel probe let the soft suspension
and the tire's grip carry a car over the rail; the wheel points through Rush 2's body probe got too involved).
Instead the converter raises low walls (`arcade_track.cpp`, after the collision polygons are gathered): a wall whose
corner stands 3 to 4 ft over the road 1.5 ft in front of it has all its corners 2 ft or more over the road raised
to 5 ft (3.5 ft, the body points' height, still let a car over the deck rail in 3 of 4 drops), each corner moved up
within the wall's plane. Each corner is measured on its own: the rails of the ramp up to the deck slope, their top
corners feet apart, and stand 3.5 to 4 ft over the ramp; the car went through them until they were raised too
(Marina 141 walls, Haight 4, Civic 23, Metro 4, Mission 60, Presidio 13, Alcatraz none). Rush 2's own tracks keep their walls clear of that height: of the
steep type 5/6 walls with road 2 ft in front of their middle, those topping out under 3.6 ft over that road are
Vegas 0 of 1,128, LA 4 of 1,673, Seattle 7 of 1,204, Atari 8 of 2,627, Crash 1 of 336; the converted arcade Marina
has 114 of 1,730. Not the cause:
these rails are single faces toward the road (no back-to-back pair, `tools/arcade2049/wall_twins.py`; the MK64
import's two-face problem, docs/custom_tracks.md), and `tools/arcade2049/wall_behind.py <collision.bin>` lists road
behind a wall within the probe's reach (only road edges reaching a foot or so under the rails here). Where more polygons crowd a spot than a collision cell's 255 (36 before the port moved the
queries' lists off the stack, docs/rush2049_research/collision.md#leaf-list-buffers-port-only-v), the converter welds corners and joins flat
neighbors (`relieve_crowding`; on Alcatraz about 130 pairs, corners moved 1.4 ft at most). Before that, both readers'
converter (`arcade_track.cpp`):

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

Loops can't be cut from above (their walls and roof don't face up, and their segments stack, so the bands bowtie);
until 2026-10-21 they fell to their flat facets, a crease every few feet up the loop. The arcade authors them as Rush
2's Alcatraz and Atari do: spline polygons on their own segment (TRACK1, 4, 5, 6, 7: every steep or overhanging one's
corners within 0.04 ft of it, t 0..1, none reaching past it). So a spline polygon facing less than 0.2 up, or whose
segment has no band, and that fits its own segment is kept as it is, in its own facing (`kept` in `spline_road`):
Alcatraz 232, TRACK1 248, TRACK7 4,348, matching the census.

Footprints off their surface (checked 2026-11-06): the report gives how many footprint planes sit over 5 ft from
their segment's surface and where (Marina 4, one 143 ft off near (717, 1576); Civic 1; Presidio 1). The game picks the
face under a wheel by its plane (the highest within 5 ft above), but `tools/collision_probe.py` shows those faces still
give the lanes their segment's height, and cutting their segments again changed nothing there, so they stay as built.
The probe does find 1-2 ft gaps between faces on the lanes near them (Civic (672-690, 1856), Presidio (-740, 915)),
there either way: open (docs/sfrush_arcade.md#how-it-differs-from-the-rush-2049-arcade-data).

## dpath.rom (the drones' paths)

8 tables of 2000 slots of 0x18 bytes (384,000 bytes). Slot 0 is a header (u32 lap point count, u32 1 or 0, ...).
Records: f32 x, y, z (path frame), f32 speed (ft/s), u32 behaviour (the N64 lanes' steering code: 2 follow, 5
damped, 1 blended, 3, 4, 0), u16 index, u16 link. Table 0 is the center lap (659 points on Alcatraz) followed by
routes off the course; tables 1 and 2 are racing lines about 10 ft from it (744 points). On Alcatraz tables 3-7 are
stale data about 2,900 ft off the course; on the other tracks all seven are lanes. The lap is at road height (median
0.4 ft) except over the jumps, where it is the cars' arc, hence `keep_height`. Speeds become Rush 2 mph as
ft/s x 60 / 88 / 1.2 (Rush 2049's lane speeds over Rush 2's, docs/rush2049_research/cars.md).

- **Presidio's lap is a run-in (2026-11-05).** Every Presidio (TRACK7) table starts from a standstill on the arcade's
  grid (record 0 at 1 mph, then 35, 50, ...), runs along a stretch of its own onto the loop at record 285 and ends
  where it first came onto it, 6,253 ft from its start; the other tracks' laps close within 17 ft. Closed into a loop,
  that 6,000 ft straight back to the grid (lane points with no road under them, speeds blended down to 1-2 mph) turned
  the drones about at the junction under checkpoint 1, where they wrecked (every drone, with DEATHS on), and timed the
  first stretch at 183 s (247 s on the clock at difficulty 2). The converter now writes a lap whose ends are more than
  300 ft apart as a run-in circuit (`run_in = true`, docs/custom_tracks.md#run-in-circuits): open AIPATH ribbons, the
  start line 100 ft along it, the checkpoint banner at the junction as CHECKPOINT_1 (the loop start), gates numbered in
  race order. Clock: 28 s to the loop start + 10 + lap / 30, about 42 s (51 s at difficulty 2); a lap of the loop 123 s.
- **Drones still wreck after Presidio's big jump (both Presidios, open).** Sims with DEATHS on, catch-up off, 5
  drones: N64 Presidio (track 17) lost 3, Presidio Arcade 2 and 0 in two runs, all between the jump and the 90 degree
  bend after it ((-1150, 175, -2600) to (-1520, 179, -2960)). The samples show cars leaving the downhill crest
  airborne at 100-113 mph (falling 50-60 ft/s) already rolling, landing on the roof or rolling over within 0.5 s of
  landing; others run wide into the bend's outside wall at 94 mph or roll on its bank at 80. Capping the drones' lane
  speeds to a Rush 2 car's cornering (rush2::calspeed::capped_lane_speeds at 2.5 g, as California Speed's are) did
  not help: with the 4 g crest cap the drones left other crests slow and flipped, corners only lost 3 and 4 of 5, and
  either way they were 25% slower from CP1 to CP2. Not changed; what keeps Rush 2049's own drones upright off these
  crests is not worked out (no drone air control found in the registry; wings are the player's).
- **Shortcut lines and table 0 off the lines (2026-11-13, both arcade games).** A racing line that leaves table 0
  by more than 60 ft for 5 points in a row where fewer than half of the lines go with it is left out (`build_one`,
  "Shortcut lines"): Rush The Rock's Golden Gate and Embarcadero table 7, The Rock's tables 2 and 5 and The Heights'
  table 3, and Rush 2049 Metro's tables 1-3 (a route of their own for 28 records). Rush 2 drones on them drove into
  the walls every lap. Where every racing line is more than 45 ft from table 0 across the ground, the lap point moves
  across the ground to their mean, fully from 60 ft (The Heights, up to 121 ft; Sunset, 65 ft): table 0 is
  AIPATH_main, the race's progress line and the drones' speeds. Wide roads put every line 30-46 ft to one side of it
  (Civic Center, Haight), hence 45. Race audit: docs/sfrush_arcade.md#checks.
- **Civic Center's tables 3-7 open with a standing start** (1, 25, 39, 48, 56 mph) on a closed lap: drones in those
  lanes braked to a crawl at the line every lap. On a closed lap the converter gives that rising ramp the speed the
  lap arrives at the line with.

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
- `preview.png`: `tools/arcade2049/previews/<id>.png` when there is one, cut like Rush 2049's
  track select thumbnails (TPIC): the middle square at 128 x 128 inside a hard-edged disc of radius 62 centered on
  (63.5, 64.5), which matches every TPIC's alpha exactly.
- `logo.png`: the name in Rush 2049's banner lettering (`banner_font.render_fit`; "Alcatraz" fits the smallest
  canvas, 128 x 32, so its letters are as large as the stock tracks').

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

`src/arcade2049/arcade_cars.cpp` builds the `rush2049_arcade_cars` mod (docs/car_mods.md#rush-2049-arcade-cars). Most
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

An earlier `physics()` in the car converter mapped this onto Rush 2 the way `build_physics` (src/rush2049/car2049.cpp) maps
the N64 cars. The arcade's mass, chassis constants, springs, suspension constants, rolling resistance, final drive,
clutch and preload go in as they are, and so does each car's box. The N64 port gives every car a 3.5 ft box height,
so the tool uses that in place of the table's 2. The drivetrain (torque, gears, drag, shift points) is the same for
every arcade car, so it lands on Rush 2's roster mean, as the N64 roster's average car does. The pitch/roll and yaw
inertia follow the mass by the N64's frame formula (2425 and 412500). Tire curves, rear grip, drive flags and
drivetrain inertia are the N64 2049 car's. Steering, yaw damping and the SUSPENSION and TIRES settings are Rush 2's
mean, as for the N64 cars. It was dropped: the cars now drive as their base car (docs/car_mods.md#rush-2049-arcade-cars).

## Coins and keys on the arcade versions

The arcade versions of the six Rush 2049 race tracks and of the SF Rush tracks (Rush The Rock, docs/sfrush_arcade.md)
carry the N64 course's own coins or keys where the arcade track has the same spot. They are the course's: same record
names, so same bits in the profile's masks (src/collectibles.cpp). One taken on either version counts on both, and
the Progress tab and unlocks don't change. The arcade's own `coin` score pickups stay left out (dobs.rom above).

- **Which spots.** `tools/arcade_collectibles.py` compares each N64 pickup against the arcade track's roaddat.rom. The
  N64 position is used as it is: the arcade data is in the N64 games' world frame (Axes). It keeps a pickup when:
  - the highest floor under it (within 2 ft above it) is within 3 ft of the N64's;
  - at 6 or more of 8 points 30 ft around it, the floors match in the same way, or both have none;
  - no arcade wall comes within 4 ft of it (floor to pickup) where the N64 has none.
  `--write` makes `src/arcade2049/arcade_collectibles.inc`. Its source: 2049 placement file 120 + k (GOLDCOIN /
  SILVERCOIN in record order, as the converter numbers them) and Rush 1 placement asset t (KEYL, key number at +0x4A,
  section position added).
- **Kept (2026-11-08).** Coins: Marina 11 of 16, Haight 3, Civic 8, Metro 12, Mission 13, Presidio 8 (55 of 96).
  Keys: Golden Gate 6 of 6, Embarcadero 4 of 7, Market 7 of 8, Downtown 8 of 8, The Heights 6 of 8, Sunset 5 of 8,
  The Rock 0 of 8 (36 of 53). Haight's hidden areas and The Rock are laid out differently in the arcade. Most others
  dropped have no arcade floor at that height (rooftops and ledges the N64 port added). The Dreamcast's coins are the
  N64's, except two Metro gold coins 6-10 ft higher; arcade versions use the N64's.
- **The track.** Only the game-built arcade mods get them (`rush2049_arcade`, `sfrush_arcade`, by track folder).
  track.glb and track.toml can't place collectibles, so other custom tracks still have none.
  `rush2::arcade2049::collectible_extras` (src/arcade2049/arcade_collectibles.cpp) hands the custom track converter
  their models and records (`track_mod::Extras::models` / `records`).
  - Models: 2049 file 68's GOLDCOING_COIN and SILVERCOINS_COI (`track2049::convert_file`), or SF Rush asset 12's
    KEYL1 (`track1::convert_container`), each with behaviour 8.
  - Records: KEYS0-7 / KEYG0-7, or KEY1-8, at their world position, as children of the nearest opaque section. That
    section's box and radius grow to hold them.
  - The PS1 SF Rush's keys are in the same table (KEYP1-4, the same tool with `--ps1`): Embarcadero Arcade and Market
    Arcade get 3 each, and the sfrush_ps1 mod's tracks get theirs the same way ([sfrush_ps1.md](sfrush_ps1.md#keys)).
  - The models and records are in the conversion's cache key. Without the 2049 source (coins) or the SF Rush ROM
    (keys), the version has none.
- **In the race.** `collectibles.cpp` takes the raced version's course at the race's object setup
  (`rush2_collect_race_reset`) and counts its pickups as that course's. The coin rules are the stock tracks': 9 ft
  pickup radius, glow, the 2049 sound. On an SF Rush course's version, `track2049.cpp` points KEY1-8 and KEYP1-4 at
  KEYL1 and turns on SF Rush's key hit test (`track1::set_redirects` with `rush1_hits`), as on the N64 course.
- **Pause box.** It counts only the pickups the race placed (`collectibles::race_pickups`). When the version has
  fewer than the course, the course's total follows in brackets: Haight Arcade shows `0/3 (8)` for its silver coins.
  A kind with none placed has no row; a version with none has no box.
- **Test.** `RUSH2_TEST_ARCADE_SPOT=<record>,x,y,z` moves one pickup, e.g. onto the start straight, to take it on a
  short drive:
  - `tools/test_race.py marina_arcade --drones 0 --env RUSH2_TEST_ARCADE_SPOT=KEYG2,1720,535,1873 --script "8:A:7,17:START" --shots 15,18.5`
    gives `1/4 (8)`;
  - `golden_gate_arcade` with `KEY3,-1639,276,5880` gives `1/6`.
  The first run after a build converts the track again, and the race starts ~8 s later.

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
