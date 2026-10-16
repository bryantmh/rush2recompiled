# mk64_import

Builds Mario Kart 64 courses as Rush 2 custom-track mods straight from the MK64 (US) ROM, as a real-world test of the
custom track converter (docs/custom_tracks.md). The output is an ordinary mod: `track.glb` + `track.toml` +
`logo.png`, nothing the game treats specially.

```
python mk64_import/mk64_track.py all|rainbow_road ... [--rom "Mario Kart 64.n64"] [--out build/mods/mk64]
```

The ROM defaults to `MK64_ROM` (any byte order). `all` builds the 16 race courses and the 4 battle courses into one
mod: the battle courses as custom battle arenas (`type = "battle"`, on the BATTLE track select), with MK64's 4 player
battle starts as `START_1-4` and its item boxes as Rush 2049 `PICKUP_` markers. Rainbow Road is the tuned course;
Mario Raceway, Block Fort (battle, and as a stunt arena) have been driven in the game; the others build but haven't
been raced or tuned. Frappe Snowland doesn't convert: its collision is denser in one spot than the game's collision
index holds.

Actors (spawn_course_actors) come in as meshes: trees, bushes, cacti, cows and piranha plants (billboards in MK64,
so two crossed copies, with a `COL_WALL` trunk the size of their bounding box), palm trees and railroad crossings
(models), and the spinning Mario and Wario signs and the Yoshi Valley egg as `MOVE_` objects. Their textures come
from segment 3, which init_actors() fills from a texture list per course (courses.toml `seg3`).

| File | What |
|---|---|
| `mk64.py` | ROM reading as the mk64 decomp's `load_course()` does it: MIO0, the course table, vertices, the packed display list unpacker, textures, a display list walker, and shape searches for the per-course tables (section DLs, TrackSections, the track path) |
| `courses.toml` | What only MK64's code knows about a course, transcribed from the decomp: init vertex colors, sky colors, star tables, the battle courses' DLs, starts and item boxes, the actors, plus Rush 2 tuning (wall height, kill sheet, AI speed caps) |
| `mk64_track.py` | Course to mod: visible meshes, `COL_*` collision, AI ribbon and gates (or a battle course's starts and pickups), actors, `SKY` (sky dome and MK64's star field), logo |
| `glb.py` | glTF binary writer (textured, vertex colored meshes, Empties, child nodes and animations, as Blender exports them) |
| `decomp_addrs.py` | Finds where the decomp's named course data (spawn tables, actor models, their DLs) sits in a course's segment 6, for courses.toml: `python mk64_import/decomp_addrs.py <decomp> <course> [names]` |

Test a build offline with `tools/track_mod_test.bat build/mods/mk64/tracks/rainbow_road <out>` and
`tools/track_mod_check.py <out>`; race it headless with `python tools/trackview/runs.py
build/mods/mk64/tracks/rainbow_road` (about 35 s).

## Rainbow Road findings

- Converts with no warnings: 1622 visible triangles, 1157 collision polygons, a 7.2 km lap at the default scale
  (0.2 m per MK64 unit).
- Double-sided walls let cars through: with each rail written as two back-to-back faces, cars that hit a rail at
  speed ended up outside it and fell (31 wrecks in a 3-lap AI race). One face toward the track: 7. The importer
  writes one face toward the path; `--double-walls` writes both, to reproduce it.
- The converter's bend-based AI speeds don't see drops: Rush 2 cars reach 200 mph down Rainbow Road's two long drops
  and wreck in the dips at the bottom. `AISPEED_` caps there (courses.toml) bring it to 1-3 wrecks a race.
- Not imported: Rainbow Road's neon signs and chain chomps, other courses' clouds.

## Not imported yet

- Vehicles (Kalimari's train, Toad's Turnpike's traffic, DK's paddle boat), Choco Mountain's falling rocks, Bowser's
  Castle's thwomps, Banshee Boardwalk's cheep cheeps and bats, Moo Moo Farm's moles, Sherbet Land's penguins, DK's
  Jungle Parkway's kiwano fruit and trees (its tree spawn table is a different struct), Rainbow Road's neon signs and
  chain chomps: they move or react by MK64's code (the falling rocks also have a spawn table).
- Item boxes on the race courses (Rush 2's race has no items; the BATTLE row's race battle places its own pickups).
- Animation frames: the piranha plants and the cows show their first frame.
- Big Donut's 0x07000DE8 (drawn only while the camera is off the ground).
