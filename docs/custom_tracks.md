# Custom tracks

A mod can add whole new race tracks. You build the track in Blender, export it as one `.glb` file, write a short
`track.toml` settings file, and put both in a mod. The game converts the track the first time it needs it and lists it
on the track select after Rush 2049's tracks. It races like any other track: forward, backward or mirrored, with
computer opponents, checkpoints, records and ghosts.

The format is still young: names and settings may change. Every conversion writes a report that says what the game
found and what is wrong (see [Checking your track](#checking-your-track)), so read it after every export.

## Contents

- [Packaging](#packaging)
- [Building it in Blender](#building-it-in-blender)
- [Collision surfaces](#collision-surfaces)
- [Markers](#markers)
- [The AI path](#the-ai-path)
- [track.toml](#tracktoml)
- [Track select art](#track-select-art)
- [Textures](#textures)
- [Checking your track](#checking-your-track)
- [Size limits](#size-limits)
- [How custom tracks race](#how-custom-tracks-race)
- [Not supported yet](#not-supported-yet)
- [For developers](#for-developers)

## Packaging

A track lives in a mod, in its own folder under `rush2/tracks/`. The folder's name is the track's id: records and
ghosts are saved under it, so don't rename it once people race it.

```
mods/
  my_track/                   the mod (a folder, or the same contents zipped and renamed to .rtz)
    mod.json
    rush2/tracks/canyon_run/
      track.glb               required: the track
      track.toml              required: its settings
      logo.png                optional: the name banner on the track select
      preview.png             optional: the picture behind the route preview
      diorama.glb             optional: the track select model
```

`mod.json`:

```json
{
    "game_id": "rush2",
    "id": "my_track",
    "display_name": "Canyon Run",
    "description": "Custom track: Canyon Run.",
    "short_description": "Canyon Run custom track",
    "version": "1.0.0",
    "authors": ["your name"],
    "minimum_recomp_version": "0.0.0"
}
```

One mod can hold several tracks, each in its own folder. Turn the mod on in the Mods menu; the track appears on the
track select the next time the game is in the menus. When two mods hold a track folder of the same name, the first in
the mod order wins.

## Building it in Blender

Everything is placed in Blender and recognized by its object name. In the names below, `*` stands for any text you
like: `COL_ROAD_*` means names such as `COL_ROAD_main` or `COL_ROAD_bridge`, not a literal `*`. Case matters;
Blender's `.001` suffixes are fine. Collections are only for your own organizing: the game goes by names.

1. **Model and texture the track.** Every mesh that isn't named as something below is drawn as scenery. Start with a
   single road loop.
2. **Add collision** (required). Duplicate the drivable surfaces and walls, simplify them, and name each copy after its
   surface ([Collision surfaces](#collision-surfaces)). Collision meshes aren't drawn.
3. **Place the markers** (required): the finish line and the checkpoints ([Markers](#markers)).
4. **Draw the AI path** (required): the racing line the computer cars follow ([The AI path](#the-ai-path)).
5. **Export** with File > Export > glTF 2.0. Choose **glTF Binary (.glb)**, tick **Include > Custom Properties**,
   keep the textures embedded, leave Draco compression off, and save as `track.glb`.

**Scale:** model in meters at real-world size (Blender's default); the game converts to its own units. A car is about
1.8 m (6 ft) wide and 3.8 m (12.5 ft) long, and the road at the Las Vegas start/finish line is about 53 m (174 ft)
across. Rush roads are much wider than real ones. Watch the lap length too: at 100 mph a 3 km lap takes about
70 seconds.

## Collision surfaces

Name each collision mesh after its surface. The surface comes from the name only, never from the texture: a road
textured as dirt but named `COL_ROAD_*` drives like road.

| Name | Surface |
|---|---|
| `COL_ROAD_*` | Paved road |
| `COL_DIRT_*` | Dirt: slows cars down, less with off-road tires |
| `COL_GRASS_*` | Grass: slows cars more than dirt |
| `COL_WALL_*` | Walls and barriers |
| `COL_WATER_*` | Water |
| `COL_KILL_*` | Falls and pits that reset the car |
| `COL_<NAME>_*` | A custom surface from track.toml, such as `COL_MUD_*` |

Faces must point up (or into the track, for walls). The report warns about drivable faces that point down; fix them
with Mesh > Normals > Recalculate Outside.

### Custom surfaces

`[surfaces.<name>]` in track.toml adds a surface of your own, used in Blender as `COL_<NAME>_*` (upper case). Each one
borrows a stock surface's handling, dust, skid marks and sound (`like`) and sets its own slowdown (`drag`). Up to 12.

```toml
[surfaces.mud]
like = "dirt"     # "road", "dirt" or "grass"
drag = 14         # slowdown: stock road 0, dirt 10, grass 15
```

| `like` | Dust and skid marks | Sound | Stock drag |
|---|---|---|---|
| `"road"` | Road skid marks | Road | 0 |
| `"dirt"` | Dirt dust | Dirt | 10 |
| `"grass"` | Grass dust | Grass | 15 |

`drag` only changes the slowdown. Everything else comes with `like`, so `like = "dirt", drag = 0` is dirt that doesn't
slow you down, not road. Off-road tires reduce the slowdown, as they do on stock dirt. Keep `drag` at 0 or above: a
negative value pushes the car forward harder the faster it goes, and it runs away to several hundred mph.

## Markers

Markers are Empties (Add > Empty > Arrows). Rotate each so its green (Y) arrow points down the track in the race
direction, put it in the middle of the road, and scale its X so the red arrow just reaches the road's edge: the X
scale is half the gate's width. Leave the Empty's Display Size at 1, because the exported file keeps the scale but not
the display size. Move every marker into place: markers left at the origin are reported as errors.

| Name | What |
|---|---|
| `FINISH` | The start/finish line. The grid lines up behind it. |
| `CHECKPOINT_1`, `CHECKPOINT_2`, ... | Checkpoints in lap order, numbered without gaps. At most 9. |
| `AISPEED_<mph>` | Optional speed cap for the computer cars ([The AI path](#the-ai-path)). |

You don't place the starting grid: Rush 2 lines the cars up itself, two per row, rows 20 ft (6 m) apart, back from the
finish line. Empties named `START_*` are ignored.

## The AI path

Draw the racing line with Add > Curve > Path, name it `AIPATH_main`, and shape it along a good line all the way round
the lap (Curve > Toggle Cyclic closes it). In Object Data Properties > Geometry, set Extrude to about half the road's
width. This matters: a curve with no thickness is left out of the .glb export, while one with Extrude is exported as a
flat ribbon.

- **Height:** shape the curve to follow the road up and down. The game drops it onto the road below it, so over
  bridges and through tunnels a flat curve can pick the wrong level.
- **Width:** the ribbon's width sets how far apart the computer cars' four racing lines spread. Keep the whole ribbon on
  the road and clear of walls, or the cars on the outer lines drive into them. The report counts the points that have
  no road under them.
- **Direction:** the path should run the way `FINISH` points; if it runs the other way, the game reverses it.
- **Backward races** use the same path the other way round, made automatically. Signs and arrows you model point one
  way only, so they face the wrong way in backward races.
- **Speeds:** the computer cars brake only where the path tells them to, so the game works out a target speed for
  every point from how tightly the path bends. To slow them down where that gets it wrong (jumps, blind crests), place
  an Empty named `AISPEED_<mph>` beside the path, such as `AISPEED_45`: it caps their speed from the nearest path point
  on, in the race direction, until the next `AISPEED_` marker. `AISPEED_0` lifts the cap.
- **Tight corners:** on harder difficulties the game never lets the computer cars go below a minimum speed (up to
  123 mph), so corners they should take slower than that make them run wide. The report warns when the path has any.

## track.toml

Only `name` is required. Anything left out uses the default.

```toml
[track]
name        = "Canyon Run"            # shown in the game
author      = "your name"
version     = "0.1"
description = "One short line"

[race]
# Timer (with checkpoints on). Leave these out and the game works it out from the AI path, as stock Rush 2 does.
start_time      = 45                  # seconds on the clock at the start; crossing the finish line adds it again
checkpoint_time = [[20, 18], [22, 20]] # per checkpoint, CHECKPOINT_1 first: seconds added [first lap, later laps]
best_times      = [95, 100, 105]      # the records table's starting entries are made from the fastest

[look]
fog_color    = "#a0b8d0"   # also the color seen past the draw distance
cloud_scroll = 0.0         # sky texture scroll speed
sky_parallax = 0.0         # how much the sky shifts as the camera moves

[surfaces.mud]             # custom surfaces: see Collision surfaces
like = "dirt"
drag = 14

[audio]
music = "track5"           # default song: "track1" to "track8"; players can still change it

[menu]
logo    = "logo.png"
preview = "preview.png"
diorama = "diorama.glb"
```

- `checkpoint_time` has one entry per checkpoint. Rush 2 keeps one time for the first lap and one for every lap
  after it; a third number (an older form of this file) is ignored.
- The timer settings apply to forward races. Backward races work out their own times from the AI path.
- `[surfaces.mud]` above is an example: delete it, or use `COL_MUD_*` meshes in Blender.
- The lap count and mirror mode are the players' choice and aren't set here.

## Track select art

The track select shows a track's logo and a model of it, as it does for the stock tracks.

| File | Size | Without it |
|---|---|---|
| `logo.png` | 128 x 32 px, or any 4:1 picture (it is fitted) | No name is shown, so include one |
| `diorama.glb` | Any size: it is fitted to the stock dioramas' size | Rush 2049's route preview |
| `preview.png` | 128 x 128 px | The route preview has no picture behind it |

- **Logo:** your track's name as you want it to look, on a clear background (PNG with alpha). It is drawn in full color.
- **Diorama:** a small model of the track, made by you, exported like the track (glb, textures embedded). It is fitted
  to the stock size (centered, standing on the turntable). Keep it under about 2,400 triangles, like the stock ones;
  the report warns above 4,000.
- **Route preview:** without a diorama, the track select shows Rush 2049's kind of preview: the AI path as a small tube
  with a light running round it in the race direction, in front of `preview.png` (a screenshot of your track, for
  example).

## Textures

Use PNG or JPEG textures embedded in the .glb, power-of-two sizes (256, 512, 1024...) up to 2048 px. The game keeps
small copies in its own memory and draws your full-size originals on screen, so texture size and count don't count
against the memory budget. A material's base color tints its texture.

Materials with Alpha Clip (glTF MASK) draw as cutouts. Blended (BLEND) materials are drawn as cutouts too for now.

## Checking your track

Every time the game converts a track, it writes a report to `track_reports/<folder>.txt` in the game's settings folder
(`%LOCALAPPDATA%\Rush2Recompiled`, or the game's folder in portable mode). Errors keep the track off the track select;
warnings and notes point at things worth fixing. Each one names the object and, where it helps, the place.

The game converts a track once and reuses the result until its files change. It looks at the files when it starts and
when mods are turned on or off, so after a new export restart the game, or turn the mod off and on again in the Mods
menu. The report covers, among other things:

- markers that are missing, misnumbered, out of order along the path, or not crossed by it
- an AI path that isn't a ribbon, isn't closed, floats above the road or leaves it
- collision faces that point down, and collision that is too dense
- unknown `COL_` names, a `*` typed literally in a name, track.toml mistakes
- the track's size, its textures, and each custom surface's settings

## Size limits

| What | Limit | Why |
|---|---|---|
| Collision triangles | 8,191, and at most 255 in one small area | The game's collision format. Stock Rush 2 tracks use about 2,500. |
| Track triangles | about 40,000 (rule of thumb) | The whole track is loaded into the game's memory, which it shares with the cars. |
| Track size | about 10 km (32,000 ft) from the origin in any direction | Positions are stored as 16-bit numbers. |
| Checkpoints | 9 | The game has 10 gates including the finish line. |
| Custom surfaces | 12 | The collision format's surface slots. |
| Custom tracks | 26 across all enabled mods | Track select slots. |

Vertex counts per object, the number of objects or materials, and name lengths don't matter: the converter splits,
merges and renames as needed.

## How custom tracks race

- **Options:** backward, mirror, laps, fog, drones and the rest all work as on stock tracks.
- **Records:** each track keeps its own records, forward and backward, under its folder name.
- **Ghosts:** saved like the stock tracks'. A ghost holds at most 150 seconds a lap, so a longer lap cuts its ghost
  short.
- **Circuits:** custom tracks are circuit courses too. Settings > General > Circuit Courses has an All Modded Courses
  toggle and one per custom track, at the bottom of the list. New tracks are in by default.

## Not supported yet

- A sky model (`SKY`) or a choice of stock sky; a mesh named `SKY` is drawn as part of the track
- Props that react when hit (`PROP_*` Empties are ignored)
- Translucent (blended) materials
- Moving parts such as lifts, trains and doors
- Stunt and battle arenas
- Setting the fog distance
- Surface grip, such as ice or sticky surfaces (Rush 2 only changes slowdown per surface)
- New sounds, dust or skid marks for custom surfaces

## For developers

- **Code:** `src/track_mod/`: `track_mod_convert.cpp` (the converter), `track_mod_menu.cpp` (logo and route preview),
  `track_mod.cpp` (finding the tracks, the disk cache, full-size textures). `include/track_mod.h` and
  `include/track_mod_runtime.h` hold the interfaces.
- **Hosting:** a custom track is raced in the HAWAII slot like a Rush 2049 track (`src/rush2049/track2049.cpp`,
  track cache key 1000 + a hash of the folder), with track select ids 38-63 (`src/rush2049/track2049_menu.cpp`, the
  tables moved to 0x80310000; the circuit review screen's diorama instances at 0x80310B00). Circuits choose custom
  tracks by folder (`src/circuit.cpp`: the config keeps the folders turned off, each circuit the folders it started
  with). Records: `src/rush2049/track2049_records.cpp` (`{"mod": folder}` courses). Ghosts:
  `src/ghost.cpp` (`mod-<folder>` keys).
- **Custom surface drag:** hooks at 0x8006B63C (dirt) and 0x8006B6A4 (grass) in `car_wheel_forces_drag_8006AFD8`;
  collision flags are `type | material << 4`, custom surfaces use material slots 3-15 except 8.
- **Backward:** the converter writes `path_backward` (asset 0x63 + slot): the spine reversed from the finish, the gates
  in reverse order and turned round, target speeds recomputed. See [race.md section 5](rush2049_research/race.md).
- **Full-size textures:** the converter's RGBA16 stand-ins are keyed by `rush2::upscale::rgba16_key`, cached as
  `trackmod_textures_<key>`, and published through `rush2::derive::set_mod_textures("tracks", ...)`.
- **Testing:** `tools/track_mod_test.bat <track folder> <out folder>` converts a track outside the game, and
  `tools/track_mod_check.py <out folder>` checks the result. In game, `RUSH2_TEST_TRACK=38` opens the track select on
  the first custom track, `RUSH2_TEST_MOD_TRACK=<n>` races custom track n in place of HAWAII, and
  `RUSH2_TEST_BACKWARD=1` races backward.
