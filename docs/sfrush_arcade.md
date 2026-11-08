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
  and bay 16,440-18,698, The Rock's distant land `APANS_01`-`16`) go to the sky. Sky draw order: dome, water, every
  other sky object (distant land), clouds. An object is sky by its textures only when every triangle has a sky
  texture: Market's TEMPL1966 (a block of road and shop fronts at about (-5400, 0) with four `SKYTOP1` triangles under
  the road) and TEMPL880 (concrete and side road at (-2300, 2500) with a small box of `SKY00A`-`03A` and `SKYTOP1`)
  were taken whole into the sky and drew floating overhead, leaving a hole in the street (2026-11-07); they stay
  scenery, their sky panels drawn in place. Downtown, The Heights and Sunset have no dome in their data (nor in
  `STATIC` or `REFLECT`): they get `sky = "clouds"`, the N64 version's dome. All seven take SF Rush's fog color
  0x9696BE (`fog_color`, as the N64 port's).
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
- The exe's table of 84 TEMPL names at 0x1406F4798 (per track: 64, 19, 1 names with value 0x20, placed through
  0x1402C4930 with that flag at the object record's +4) is not the road: only 7 of Golden Gate's 51 road objects
  are in it.

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

## Checks

All seven pass `tools/track_mod_test.bat` and `tools/track_mod_check.py` (every lane point has ground in its index
cell, every placement its model). Left as warnings: lanes that take an alternate route for a stretch (Golden Gate's
fourth, Embarcadero's fourth, The Rock's first) are offset from the main path there, and Sunset's lap sits up to 11 ft
off its road at 6 points (dropped onto it). The Rush 2049 arcade mods build byte for byte as before these rules.
