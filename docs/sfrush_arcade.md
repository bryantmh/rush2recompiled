# San Francisco Rush arcade (Rush The Rock) tracks

The arcade game San Francisco Rush: The Rock has its own versions of all seven SF Rush tracks. Their layouts are the
N64 game's (the same drone paths, point for point), but their scenery is far more detailed: 139-338 textures a track,
mostly 128 x 128 and 128 x 64, up to 256 x 256, 2.7-5.2 MB of texels per track, where the N64's are held to its 4 KB
texture memory (about 64 x 32) and a whole track file is 0.2-1 MB. The Games tab's **SF Rush Arcade** section builds
them into a custom-track mod with the Rush 2049 arcade converter (`src/arcade2049/`, docs/arcade2049.md), which reads
this data with a few per-game rules (`ArcadeGame` in include/arcade2049.h, `rock` in arcade_track.cpp).

```
cmd //c "tools\arcade2049\cpp_test\build.bat tmp\rock\cpp - sfrush"     (the mod built outside the game)
cmd //c "tools\track_mod_test.bat tmp/rock/cpp/sfrush_arcade/tracks/market_arcade tmp/rock/conv/market"
python tools/track_mod_check.py tmp/rock/conv/market
python tools/arcade2049/arcade.py %LOCALAPPDATA%\ProjectR\Data\sfrushrk\TRACKS\TRACK3   (study)
```

## The mod

- `sfrush_arcade.rtz` in the mods folder, built and installed like the Rush 2049 arcade mods (docs/arcade2049.md#building-it-in-the-game):
  Build and Install / Select Folder, a key of the inputs and the build in its mod.json, rebuilt once at launch
  when the build or the data changes (`refresh_now` runs every arcade game). No cars: the arcade's cars aren't
  converted.
- Seven tracks, each the ARCADE version of its SF Rush course (`[menu] variant_of = "sfrush:<course>"`), with
  `after = "sfrush"` for when the SF Rush ROM isn't there (then they are tracks of their own after the SF Rush
  tracks). The course's own version now shows as N64 (it showed STOCK; a saved STOCK pick of an `sfrush:` course is
  read as N64).
- The game filter files them under SF Rush, by the mod's id (`rush2::track2049::mod_from_sfrush`).
- They carry the N64 course's keys where the arcade track has the same spot (36 of 53; none on The Rock), taken with
  SF Rush's hit test and shared with the N64 version: docs/arcade2049.md#coins-and-keys-on-the-arcade-versions.
- In the track select an arcade version shows its course's diorama and banner (`write_tables`, as the Dreamcast
  versions do): the arcade data has no picture, and SF Rush's dioramas are models of the courses. A course with its
  arcade version installed shows a version band round the foot of its diorama, as the Rush 2049 previews' rings: blue
  on the N64 version, red on the ARCADE one (`add_band_model` in src/rush1/track1_convert.cpp: models `R1TRACKnN` and
  `R1TRACKnA` that call the diorama's list and draw the band; the band follows the diorama's outline from above, the
  union of its triangles pushed out 1.5% of its size, 7% of its size tall). The mod still
  carries a logo (`assets/sfrush_arcade/<id>_logo.png`, the N64 banners, `tools/build_banners.py --arcade`), which
  the race's logo shows.

## Where the data is

ProjectR unpacks the arcade disk to `%LOCALAPPDATA%\ProjectR\Data\sfrushrk` (`SFRUSH_ARCADE_DATA` overrides it):
`TRACKS\TRACK1`..`TRACK7`, `MIRROR1`..`MIRROR7` (the mirrored tracks, not used: Rush 2 mirrors any track), `CARS`
(`CAR1`..`CAR12`, not converted), `ATTRACT`, `SELECT`, `MOVIES` and the rest. A track folder holds the 2049 arcade's
files but `dobs.rom` (no scripted objects): `objects`, `textures`, `worlds`, `roaddat`, `dpath` (`.rom`).

| Arcade folder | N64 track | Mod track |
|---|---|---|
| TRACK1 | 1 Golden Gate | `golden_gate_arcade` (a run-in circuit, as the N64's) |
| TRACK2 | 2 Embarcadero | `embarcadero_arcade` |
| TRACK3 | 3 Market | `market_arcade` |
| TRACK4 | 7 The Rock | `the_rock_arcade` |
| TRACK5 | 4 Downtown | `downtown_arcade` |
| TRACK6 | 5 The Heights | `the_heights_arcade` |
| TRACK7 | 6 Sunset | `sunset_arcade` (the figure 8) |

Matched by their drone paths: table 0 of each `dpath.rom` has the N64 lane 0's point count (1431, 1155, 1333 on
tracks 1-3) and its extents to the foot (4963 x 8967 ft against 4962 x 8966 on Golden Gate).

## How it differs from the Rush 2049 arcade data

- **objects.rom:** the object count is read at 0x60 (0x50 holds another number: 64 on TRACK1, of 607). Texture
  descriptors, formats (YIQ422, AYIQ8422 with NCC tables), object headers and triangles are 2049's. Vertices are 24
  bytes, not 28: f32 x, y, z, s, t, u16 flags (2049's 0 / 0xFFFF / 0xFFFE), u8 a shade index (0 on most, 1-255; not
  decoded), u8 0, with no color (drawn white). The readers tell the sizes apart by the gap from an object's vertices to
  its triangles. Read as 28 bytes, every vertex took the next one's bytes: the first build's scenery was huge dark
  planes.
- **LODs:** an object lists its LODs the other way round: slot 0 is the coarse model with the distance it starts at
  (a cone: 4 triangles from 20 ft), the full model follows with distance 0 (199 of Golden Gate's 202 multi-LOD
  objects). The readers take the slot with the smallest distance (Rush 2049's slot 0). Taken from slot 0, the scenery
  was coarse up close and its pieces didn't meet, leaving holes (Golden Gate 14,600 scenery triangles, 21,300 with the
  full models; Market 18,000, 30,500).
- **Alpha textures:** AYIQ8422 texels are stored alpha first, then the color index (Rush 2049: index, then alpha).
  Read the 2049 way, the trees and backdrops drew as tinted see-through boxes (164 alpha levels on a tree); swapped,
  alpha is 0 or 255 and the colors right, on every alpha texture looked at, the crowds, flags and banners too. The
  readers tell the games apart by the vertex size (`Texture::alpha_first`). Placements in `worlds.rom` are all
  named `TEMPL<n>` with identity matrices; TRACK5-7 also have 111, 100 and 65 placements with no name (left out).
- **roaddat.rom:** 2049's layout, surfaces road, dirt and wall only (no water). The flags' high word holds other bits
  (0x7000, 0xB000, ...), not 2049's covered levels: the N64 SF Rush polygons have none, so only the low word is kept.
- **dpath.rom:** 2049's eight tables, but each header holds the N64 lane header's lap: u16 at +0xA the loop start,
  +0xC the lap's end (Golden Gate 349 and 1045, as the N64's lane 0). A table runs from its grid to the lap's end, then
  a run-out. The converter's lap (`build_one`, the `rock` table pass):
  - closed (loop start to lap end) when the intro runs within 150 ft of the loop all along (The Rock: the grid, up to
    103 ft beside the loop, then the stretch of loop before the finish; its lap starts at its loop-start gate and the
    finish is 70 points before the end) or record 0 is within 300 ft of the lap's end (Embarcadero, Market, Downtown,
    The Heights, Sunset);
  - else a run-in circuit (Golden Gate, whose intro starts 4,300 ft from the loop): records 0 to the lap's end, cut
    where the line first drives back over its own intro (each line joins the loop some points before its loop start:
    table 1's record 305 is record 994).
  - A racing line that comes back over its own points is left out (Golden Gate's table 6 doubles back in its intro).
  - Speeds are the N64 lanes' (ft/s, point for point), mapped as the SF Rush port maps its lanes: ft/s x 15/22 mph,
    without the 2049 converter's lane map (/ 1.2).
- **Sky:** the dome is the `SKY00`-`SKY03` (and `A`) and `SKYTOP` textures (`TRZSKY01`-`04` on The Rock), a ring of
  eight panels about 28,000 ft out, plus `CLOUDS`. `SKYSIDE`, `SKYBOTT` and `SKYSCR` are skyscraper walls. 2049's
  far-object rules apply with a 12,000 ft radius for all but water (2049: 9,000): Market's road and trees reach 9,138
  ft and Embarcadero's signs 9,406, while the backdrops (Market's hills `TP_` 12,190-22,743 ft, Embarcadero's Marin hills
  and bay 16,440-18,698, The Rock's distant land `APANS_01`-`16`) go to the sky. Sky draw order: dome, water, then the
  distant land and cloud strips back to front, triangle by triangle (2026-11-13). The sky has no depth test, and drawn a
  material at a time Embarcadero's far skyline strip (`CLOUDS2`) painted over the Marin hills' tips and a hill's
  cliff face went behind its own slope (the arcade draws them with depth). Each triangle's key is its nearest corner's
  distance across the ground (the sky is always centered on the camera; the triangle's middle got the cliff wrong),
  and land below the water's height is cut off so the bay stays in front of the hills' feet. The order is kept by
  the mod converter, which draws a sky that comes back to a material in its triangle order
  (`track_mod_convert.cpp`; skies whose materials each come as one run are drawn as before). Checked against a
  depth-tested render all the way round with `tools/glb_view.py --no-depth` (draws a glb in file order, as the game
  draws a SKY mesh). An object is sky by its textures only when every triangle has a sky
  texture: Market's TEMPL1966 (a block of road and shop fronts at about (-5400, 0) with four `SKYTOP1` triangles under
  the road) and TEMPL880 (concrete and side road at (-2300, 2500) with a small box of `SKY00A`-`03A` and `SKYTOP1`)
  were taken whole into the sky and drew floating overhead, leaving a hole in the street (2026-11-07); they stay
  scenery, their sky panels drawn in place. Downtown, The Heights and Sunset have no dome in their data (nor in
  `STATIC` or `REFLECT`): they get `sky = "clouds"`, the N64 version's dome. All seven take SF Rush's fog color
  0x9696BE (`fog_color`, as the N64 port's).
- **Far scenery and the far plane (2026-11-13):** Embarcadero's Marin hills seen from the waterfront are not sky but
  three flat shore panels in the scenery (`SHORE2F`, `SHORE3_2`, `SHORE3_3`, 1,500-2,100 m out, up to 9 degrees up).
  At FOG 1 these tracks have no fog (`rush2::sky_fog::fog_removed`), and the far plane (2,000 ft x the Draw Distance)
  sliced the panels along a straight line that moved as the camera turned, leaving a wedge of sky under the cut; at
  the Original draw distance they were gone. On a converted course without fog the reach is now the whole course
  (`reach_scale` in src/draw_distance.cpp: far plane, object cull distance, float matrices and every section at 40,000
  ft), as the arcade draws it; LOD keeps the Draw Distance setting. Checked in game at two headings 10 degrees apart
  and at the Original draw distance (`RUSH2_CS_PLACE=-500,3,-312,-0.766,-0.643,0,1`, backward grid facing the bay).
- **Water:** `WATER2` sheets become `COL_WATER` as on 2049, but only at sea level (every corner at 5 ft or below: the
  sheets lie at -10 to 0 ft): a raised one on The Rock (61-64 ft) has lane points passing under it. `WATER2A` stays
  scenery as on 2049. Golden Gate 45, Embarcadero 356, The Rock 447 water triangles.
- **Spline road footprints:** the converter's report lists footprints whose plane is over 5 ft off their segment's
  surface (Market 16, one 158 ft off, where two passes of the road overlap). `tools/collision_probe.py` (each lane
  point through the game's ground pick: the highest face holding it whose plane is at most 5 ft above, then its
  segment's height) shows they still give the wheels the right height, and cutting their segments again (tried
  2026-11-06) changed nothing where the lanes go, so they are left as built. What the probe does find are gaps between
  faces, 1-2 ft wide, on the lanes (Golden Gate, Market and Sunset 30-40 lane points each, the others 1-10; Rush 2049's
  Civic and Presidio too), there with or without that: open.
- **Drawn road against the spline road (2026-11-06, reported as the road looking bumpy, wavy lane lines and the
  car's shadow clipping into the road on Golden Gate past the bridge):** the drawn road is the arcade's own flat
  facets, one quad across the road and 30-190 ft long, kinked 1-4 degrees at their seams, with the road texture's t
  jumping about 103 texels at two of every three seams (T1_4_GRN repeats every 128), while the cars ride the spline
  road's curved surface: along all four lanes the drawn road is at most 0.76 ft over the game's ground (median
  0.07; the arcade's own ground query, ProjectR project-r.exe 0x1402C6550, is the same math). The arcade draws the
  same facets and seams (checked against a ProjectR screenshot of the spot), so the road stays as the data has it; a
  drape of the drawn road onto the surface was tried and dropped (it reshaped arrows and wall bottoms).
- **Car shadow against the drawn road (open):** Rush 2 puts the shadow over the wheel points
  (car_model_update_parts_80059D24). Logged on Golden Gate (two cars, a minute each): its corners sit 0.3-1.1 ft
  over the drawn road (median 0.75, against 1 ft on a stock track), and in about 1% of frames a crest between them
  rises through it, by up to 0.36 ft. Tried in src/interpolation.cpp and dropped: the decal depth mode (the shadow
  vanished: RT64 passes a decal pixel only within the depth slope of what is there), and moving its vertices
  toward the camera (0.5 ft drew it over the car's lower body, 0.35 ft drew dark lines across other cars seen from
  the side, where the far corners move much more than the near ones). The arcade's shadow (ProjectR 0x1402FEBC0,
  texture SHADBLUR, set up in 0x1402FEB50) is the wheels' ground points raised along the car's up axis by 0.7 ft
  for the watched car and 0.5 ft for the others (table 0x140623970), left out when its wheels' heights differ by
  over 8 ft, its alpha from their average height; the quads go to its Vulkan renderer through 0x14017D0F0 /
  0x14017D5E0, and it never calls vkCmdSetDepthBias. How its renderer keeps the road from cutting it is not found.
- **Key markers:** TRACK5-7 (Downtown, The Heights, Sunset) place small cubes drawn only in the ATARI5 logo (10
  triangles, 7, 8 and 8 of them) through worlds.rom, each at an N64 key's spot (all 23 match one, within a foot; the
  5 at keys the arcade_collectibles.py floor test drops too). They are the arcade's own pickups and are left out, as
  the 2049 arcade's coins are; the SF Rush keys go there instead (arcade2049.md#coins-and-keys-on-the-arcade-versions).
  ATARI5 also shows on signs on TRACK1 and TRACK3, which have other textures and stay.
- **Finish banner:** every track has a banner drawn in CHKPOINT 19-203 ft from its lap line ('F' in rock_gates), and no
  placed object uses its FINISH texture (256 x 64; Market's 128 x 32): the arcade puts FINISH on the banner for the last
  lap. The converter makes that banner's CHKPOINT faces the custom track's FINISH_BANNER (a CHKPNT material) with
  FINISH's image (scaled to CHKPOINT's size) as the FINISH material (custom_tracks.md#finish-banner), as the PS1 tracks
  do. Checked in game with `RUSH2_TEST_RACE_FINISH_BANNER=1` (Downtown, Market).
- **Breakables:** see [Breakables](#breakables).
- The exe's table of 84 TEMPL names at 0x1406F4798 (per track: 64, 19, 1 names with value 0x20, placed through
  0x1402C4930 with that flag at the object record's +4) is not the road: only 7 of Golden Gate's 51 road objects
  are in it.

## Cracks and curbs

The Rock's models leave holes the sky shows through (reported 2026-11-13 on Embarcadero, under the overpass on the
climb at about (-1180, 143, 2130-2520) game ft). They are in the arcade data itself: the converted glb's vertices match
`objects.rom` to 1e-5 and keep every triangle, and the game's 1/16 ft vertex rounding added 0-3 pixels in offline
renders. Embarcadero (TRACK2), counted on its 28,407 placed triangles:
- **Curbs with no face:** the sidewalks (SIDWOKBRIK) rise up to 0.5 ft over the road (6LNROAD3) with nothing under
  their edge, so a wedge of sky runs along the curb.
- **T-junctions:** 3,165 open edges have another piece's corner on them (the road and sidewalks worst); drawn, they
  sparkle.
- **Seams:** 1,151 open edges miss the edge beside them by 0.002-0.2 ft (the ramp top at z 2220, 0.16-0.2 ft).

`close_scenery_seams` (src/arcade2049/arcade_seams.cpp), on the opaque TRACK faces of the Rock tracks only (Rush
2049's arcade tracks are left as they are):
1. A vertical face, both sides, with the upper face's texture, between two open edges of ground faces on one line
   seen from above (0.05 ft), on opposite sides of it, 0.04-1 ft apart in height, over the 0.5 ft or more they share.
2. Open corners within 0.2 ft snap together, unless they are corners of one triangle (then 0.04 ft), so thin strips
   and open pole ends keep their width. "Same object" can't be the test: shared corners join the whole track.
3. An open edge with an open corner on it (0.04 ft, or 0.2 ft when the corner is outside the edge's triangle and has
   an open edge of its own alongside) is split there at the corner's position; the triangle is redrawn as a fan round
   its center.

Embarcadero: 416 curb faces, 602 corners snapped, 1,598 edges split (26,771 -> 32,931 triangles); opaque T-junction
edges 3,623 -> 18, seams under 0.16 ft 550 -> 53. What is left is mostly 0.4-0.8 ft apart: separate features, not
cracks. Offline renders along the lap (sky in magenta, tools/glb_view.py --eye ... --cull 1) showed no new hole.

## Breakables

The arcade draws N64 SF Rush's knock-over objects as plain scenery that stands still: its cone, parking meter and
traffic light models and its single tree cards stand where the N64 courses place their cones, meters, traffic lights,
trees and trash munchers, mostly within 0.1-0.5 ft (`tools/arcade_props.py` lists them). The converter
(`breakable_kind` in src/arcade2049/arcade_track.cpp) makes every placement of those objects a prop, Rush 2's own
breakable as on the N64 courses (src/rush1/track1_convert.cpp; custom_tracks.md#props), its origin at its foot:

| Arcade object | Rule | Prop | Rush 2 class (as the N64's) |
|---|---|---|---|
| cone | texture RED, 19 triangles, under 6 ft | `PROP_KNOCK_` | CONE1 |
| parking meter | PARKMETER, under 8 ft | `PROP_METER_` | METER |
| traffic light | SIGNAL3, under 25 ft tall and 6 ft across | `PROP_TIP_` | CURVEHIT (the N64's PMUNCH) |
| tree | one of T1_TREE1, 4, 5, 9A, DECID2, DECID2A, JUNIMED4 (the textures at N64 tree and muncher spots), at most 8 triangles, 8-70 ft tall, at most 60 ft across | `PROP_TREE_` | TREEHIT |

Matched by object, not by spot: Market and Downtown place twice as many meters as the N64 (48 and 24, against 26 and
12 at N64 spots), Sunset 11 meters away from the N64's, and The Rock 74 small trees where the N64 has cones. Tree
clumps and rows (more triangles, wider) stay scenery. Counts: Golden Gate 38 trees; Embarcadero 78 cones, 15
traffic lights, 9 trees; Market 48 meters, 15 trees; Downtown 24 meters, 21 trees; The Heights 14 cones, 5 meters, 1
tree; Sunset 11 meters, 5 trees; The Rock 74 trees (at most 102 a track, under Rush 2's 130 with the keys). The N64's
flags, fences, windows and gas sign have no arcade object that comes apart in pieces, so they stay scenery. Like the
N64 courses, the arcade versions take SF Rush's hit test (src/rush1/track1.cpp `rush2_track1_breakable_hit`). Checked in
game 2026-11-13 (Embarcadero cones and traffic light, Market meter and tree, cones also without the Rush 2049 ROM).

## Gates

The Rock's data has no gates (its `CHKPOINT` banners mark only some), so the converter keeps them as a table
(`rock_gates` in arcade_track.cpp), read from ProjectR's `project-r.exe` (the arcade's code): records of 0x1C bytes
from file offset 0x6ED650, f32 x, y, z in world feet, u32 radius squared (250,000: 500 ft), then fields not decoded,
each track's forward table followed by its reverse one (radius 100 ft). Tracks 1-3 and TRACK5 are the N64 SF Rush
tables of the same tracks to the foot (docs/rush1_research.md#checkpoints); The Rock, The Heights and Sunset are the
arcade's own: The Rock has a gate where its intro meets the loop, The Heights' first two gates sit elsewhere (the N64's
(2667, -20) and (5238, 3098) are (2960, -20) and (4591, 2581)), and Sunset's figure 8 is shorter (the N64's gate at
(2135, -7777) is 1,528 ft from the arcade's lap). Roles come from the N64 flags: the lap line (flag 2) is `FINISH`,
Golden Gate's checkpoint 0 (the arm gate on its intro) is `START`, the rest `CHECKPOINT_n` in race order.

Each gate goes to the lap point nearest it in 3D at or after the one before (Sunset passes the spots of its 4th and
5th gates twice, 60 ft apart). Every gate lands within 75 ft of the lap. Sunset's 3rd and 4th gates reach the
opposite stretch (the converter warns): a car meets that stretch only while another gate is next, so no gate is
passed early.

Each marker carries the arcade's radius as `pass_radius` (500 ft, 152.4 m), which the mod converter writes into the
path file as the gate radius the race's gate test reads (`func_800A1468`: a change of side counts only within the
radius seen from above), while the marker's X scale (the road's half width + 10 ft, 40-210 ft) stays where the
converter finds the AI path's crossing. With the road's width alone (issue 57, 2026-11-07: "the tunnel shortcut
doesn't count and the left path on top of the rooftop near the end of the lap doesn't count either" on The Rock) a
car taking a shortcut crossed the gate's line beside it and the gate was missed, which stops the race's checkpoints,
laps and respawn window (docs/rush2049_research/checkpoints.md#8-car-tracker-checkpoint-window-and-respawn-point-rush-2-vs-2049-vs-sf-rush).
On The Rock the road under the bridge crosses checkpoint 2's line 43-66 ft out (its marker: 40 ft), the cell-block
roof (ACELLTOP) 52 ft above the finish 66-157 ft out (its marker: 52 ft), and the lower road beside the climb to the
finish up to 180 ft out. `tools/gate_pass.py` (Rush 2's gate test along every AI line, 3 laps) finds no gate counted
on another stretch at 500 ft on any of the seven tracks; in game The Rock's drone counted checkpoints 1-6 in order.

## Music

Rush The Rock's music isn't recorded: its sound board (Atari CAGE style, a TMS32C031 on a 32-bit ROM bus) plays
sequenced songs, a script per track driving a voice that plays samples from the four sound ROMs `audio.u62`, `u61`,
`u53`, `u49` (beside the unpacked disk in ProjectR's `sfrushrk` folder; the disk itself holds no sound). The game
synthesizes them live from those ROMs (`src/arcade2049/rock_music.cpp`, a port of ProjectR's native version of the sound
program; `tmp/music/rtr_render.py` was the Python proof it matches sample for sample), as a song player stream
(`src/music_streams.cpp`). Each TRACKn's song is the Original of its arcade version and is listed under SF Rush >
Arcade in the Sound tab. Addresses below are project-r.exe (base 0x140000000) unless they are DSP addresses (ROM).

**ROM** (loader 0x1401B7E80): dword k of the 8 MB image = bytes u62[2k], u62[2k+1], u61[2k], u61[2k+1] (little
endian) for the first 4 MB, u53/u49 likewise for the second; DSP address = 0xD00000 + dword index. Floats are TMS320C3x
(0x140243320): the top byte the signed exponent (-128: 0.0), bit 23 the sign, a 23-bit mantissa.

**Samples** (parse 0x1401B8B80): 256 pointers at byte 0x7E5800, each to a record of dwords R[0..16]: R[1] format (3
8-bit: four a dword, most significant byte first, x 256, decoder 0x1402DD650; 1 16-bit: two a dword, high half first,
0x1402DD3E0; 5 unsupported, records 0-3), R[2] rate multiplier (float), R[3] start, R[4] length (samples), R[7]/R[8]
loop start/end (a loop only when start < end), R[10] root key, R[13] attack and R[16] release step per block (floats,
negative: none). Tables (DSP addresses): 0xEFC963 semitone ratios 2^((i-36)/12) (73), 0xEFC9AB cent ratios
2^((i-100)/1200) (457), 0xEFCB74 volume i/128 (256), 0xEFC8E3 velocity i/128 (128), 0xEFCDB4 the pan lobe sin(pi i/256)
(130).

**Songs**: the sound program's command table (0x1406EB6C0, 16 bytes an entry: track list pointer, type byte at +8: 8
start a song, 5 stop one; even commands stop the odd one before them). A track list is {next, script PC, priority,
channel}. The game's per-track starter (0x1402303A0, table 0x14065B626) sends sound calls 0x8052, 0x8050, 0x8056, 0x80D8,
0x8054, 0x8058, 0x805A for TRACK1-7, which the call table 0x1406E4200 ({engine, arg count, command}) turns into
commands 27, 25, 31, 41, 29, 33, 35 (TRACK4, The Rock, has the one call out of the run). No direct caller of 0x1402303A0
was found, so the TRACKn order is the table's, taken as the folders' [I]. Command 29's scores are command 3's. Commands 37
(23 s, the attract fanfare, 0x805C) and 61 (154 s) play once; 43-53 are 1-3 s stingers.

**Sequencer** (tick 0x14024EDB0, spawn 0x14024E810): 250 ticks a second. ProjectR runs it once per 64-sample block at
22050 Hz (344.5 a second; the stream thread 0x14024BF10 through 0x14024C340, after that block's mix, vtable
0x140707338 slot 2 = 0x14024FB10), which plays every song 1.378x too fast and cuts its phrase samples short. The songs'
long phrase and loop samples (22050 Hz; the bass, sample 48, is an in-tune E1 at that rate) last exactly a whole note
(0x3C0000) at 250 ticks, 88.2 samples a tick: Sunset's drum loop (sample 24, 70560 samples) is two whole notes, its
guitar phrases one, and every song's phrases fit their notes the same way. The port keeps the 64-sample voice blocks
and runs the sequencer on its own 250 Hz clock. Per tick a track's counter c20 -= tempo, and below 0 its script
runs to the next note; c24 -= tempo, and below 0 the note releases. Instructions are an opcode and an operand dword
(branches a third). Opcodes below 0x80 are notes (0 a rest): operand 0 returns from a call or ends the track, else its
low 4 bits pick the duration (16.16 ticks: 0, 0x3C0000, 0x1E0000, 0xF0000, 0x78000, 0x140000, 0xA0000, 0x50000, 0xC0000,
0x3C000, 0x1E000, 0xF000, 0x7800, 0x28000, 0x14000, 0x60000), 0x40 adds half, 0x80 ties, (A & 0x30) == 0x10 is staccato,
(A >> 4) & 3 adds 0/0/20/40 velocity. Key = transpose + note, through the patch map (0xBC) or + 11. Other opcodes (jump
table 0x14070C368): 0x80/0x81 tempo =/+=, 0x82/0x88/0x83 volume, 0x84/0x85 transpose, 0x8D call through a pointer cell,
0x8E/0x8F loop start (count, 0 forever) and end, 0x99 jump, 0x9C/0xBF pan +=/=, 0x9D sample, 0xA2/0xA3 velocity,
0xA6/0xA7 cents, 0xA9-0xAD register arithmetic, 0xAE/0xAF table jumps, 0xB0-0xBA register moves, shifts, compares and
conditional jumps, 0xBC patch. Not ported (effects): 0x8C, 0x97, 0xA4, 0xA5/0xA8, 0x9F/0xA0, 0xC0/0xC2, reverb sends
(0x1402DE960) and portamento.

**Voices** (0x14024B2D0, render 0x1402DDBA0, envelope 0x1402DD980): 16, one a channel. Rate = semitone[key + cents/100 -
root] x cent[cents % 100] x the sample's rate, linear interpolation; level = velocity x volume x envelope (attack ramps
up by R[13] a block; release fades by R[16] a block, or without one the sample just stops looping). ProjectR's stereo mix
(0x14024AA20) pans by the lobe: left = g(a + 0xC0) + 0.707 g(a + 0x140), right = g(a + 0x40) + 0.707 g(a + 0x1C0), a the pan
angle (512 a turn). Its output level also goes through ProjectR's own volume table (0x1406EA51C, by its game_volume
setting), so the port levels the songs by loudness instead: -4.95 LUFS as synthesized, turned down 11.15 dB to Rush 2's
-16.1 and doubled for the output's halving (`file_song_makeup`, include/audio2049.h): a gain of 0.554
(`rock_music_compensation`).

## Checks

All seven pass `tools/track_mod_test.bat` and `tools/track_mod_check.py` (every lane point has ground in its index
cell, every placement its model). Left as warnings: lanes that take an alternate route for a stretch (Golden Gate's
fourth, Embarcadero's fourth, The Rock's first) are offset from the main path there, and Sunset's lap sits up to 11 ft
off its road at 6 points (dropped onto it).

**Race audit (2026-11-13, `tools/drone_pace.py --race ... --wrecks --lost-laps`, 3 laps, 7 drones):** those alternate
routes are the arcade's own data, not a misread: dpath table 7 on Golden Gate (TRACK1) and Embarcadero (TRACK2) and
tables 2 and 5 on The Rock (TRACK4) leave table 0 by up to 664, 396 and 194 ft. No N64 lane goes there (the N64's four
run 130-230 ft away on average), so they are shortcut lines Rush The Rock added. In the converted track they run along
strips outside the road's walls and cut back through a wall (Embarcadero, (1101, 975) to (1103, 2097): lane 4 crosses
the wall back onto the road; (1200, 2900-3120): it zigzags across the wall line 96-124 ft up with no ground under it). Every wreck of
the drones on that lane (cars 1 and 5) is on those stretches: 37 of 37 on Embarcadero and 29 of 30 on Golden Gate.
Separately, on The Heights (TRACK6) all seven racing lines leave table 0 by up to 149 ft at its records 97-136 (the
spot with 41 wrecks a race), and on Sunset (TRACK7) table 0 has a 63 ft kink at record 840 that every line skips (21
wrecks forward, 48 backward): there table 0, which becomes `AIPATH_main` and sets the target speeds, is the odd one out.

**Fixed (2026-11-13):** the converter leaves out shortcut lines most racing lines don't take and moves table 0 onto the lines where every line is more than 45 ft off it (docs/arcade2049.md#dpathrom-the-drones-paths), and the custom track converter keeps painted lanes in order along the lane (docs/custom_tracks.md#the-ai-path): The Heights' and Sunset's real wreck spots were lanes stepping back and forward again (turns of 150-180 degrees) where AIPATH_main's cross lines met them out of order. Forward wrecks a race after the fixes (before): Embarcadero 1 (37), Golden Gate 4 (30), The Rock 4 (17), Sunset 5 (38), The Heights 22 (60, all now finish), Downtown 6 (6), Market 9 (12); Rush 2049 Haight 0 (12). Left: Rush 2049 Metro (114: drones reach a right-angle corner at (4650, 600) at the arcade lane's 142 mph and roll; a U-turn at (5500, 0) over a small wall block) and every backward race (the arcade had mirror tracks, not backward ones).

**Backward speeds and hand fixes (2026-11-13):** backward races now keep the forward race's authored speeds as a ceiling (docs/custom_tracks.md#the-ai-path); arcade backward crashes a race: Sunset 34 -> 7, Mission 15 -> 0, The Heights 22 -> 9, Market 43 -> 27, Downtown 10 -> 5, The Rock 12 -> 8, Marina 18 -> 13. A hand fix in `build_one` (`line_fixes`): Embarcadero's tables 1-2 move 8 ft off the tip of a wedge of wall at (-3967, -320) before the finish (backward 30 -> 2 crashes, all finish). Rush 2049 Metro's crashes (36-40 forward, 30-32 backward) are start pile-ups and lone hits at speed along (5720, 229) to (6226, -256), where the course's trolley `MOVE_T4MTROLLEY` runs (not confirmed); a cap of 80 mph on its right angle at (4583..4633, 417-477), taken at 141-147, changed nothing and was reverted. The Rush 2049 arcade mods build byte for byte as before these rules.
