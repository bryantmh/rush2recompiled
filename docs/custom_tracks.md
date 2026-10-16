# Custom tracks

A mod can add whole new race tracks, stunt arenas and battle arenas. You build the track in Blender, export it as one `.glb` file, write a short
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
- [Sky](#sky)
- [Moving objects](#moving-objects)
- [Stunt and battle arenas](#stunt-and-battle-arenas)
- [Track select art](#track-select-art)
- [Textures](#textures)
- [Checking your track](#checking-your-track)
- [Previewing your track](#previewing-your-track)
- [Size limits](#size-limits)
- [How custom tracks race](#how-custom-tracks-race)
- [Not supported yet](#not-supported-yet)
- [For developers](#for-developers)

## Packaging

A track lives in a mod, in its own folder under `tracks/`. The folder's name is the track's id: records and
ghosts are saved under it, so don't rename it once people race it. For a mod `my_track` holding the track
`canyon_run`, the game reads exactly `mods/my_track/tracks/canyon_run/track.glb`; folder names must match that
spelling (no spaces, lower case `tracks`). Older mods that use `rush2/tracks/canyon_run/` still load.

```
mods/
  my_track/                   the mod (a folder, or the same contents zipped and renamed to .rtz)
    mod.json
    tracks/
      canyon_run/             the track's id
        track.glb             required: the track
        track.toml            required: its settings
        logo.png              optional: the name banner on the track select
        preview.png           optional: the picture behind the route preview
        diorama.glb           optional: the track select model
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

Faces count on the side their normals point to: up for road, into the track for walls. A drivable face that points
down is a ceiling (a tunnel roof, a bridge's underside), as in the stock tracks, so check normals with Mesh > Normals >
Recalculate Outside if the car falls through a road. Give a wall one face, toward the track: a wall with faces
on both sides (or two back to back) pushes a car that gets into it out through the far side. Flat neighboring triangles of the same surface are joined into
the game's larger polygons, as the stock collision is built, so triangulating doesn't cost you anything.

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

`drag` sets the slowdown: the drag, and how far the engine loses power as it does off-road (none at 0, all of it
at the stock dirt or grass drag or above). Everything else comes with `like`, so `like = "dirt", drag = 0` is dirt that
doesn't slow you down, not road. Off-road tires reduce the slowdown, as they do on stock dirt. Keep `drag` at 0 or above: a
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
finish line. Empties named `START_*` are ignored on a race track; arenas use them instead of `FINISH` (see
[Stunt and battle arenas](#stunt-and-battle-arenas)).

## The AI path

Draw the racing line with Add > Curve > Path, name it `AIPATH_main`, and shape it along a good line all the way round
the lap (Curve > Toggle Cyclic closes it). In Object Data Properties > Geometry, set Extrude to about half the road's
width. This matters: a curve with no thickness is left out of the .glb export, while one with Extrude is exported as a
flat ribbon. The ribbon has to lie flat on the road: a curve drawn flat extrudes straight up until its points' Tilt is
90 degrees (select them, Ctrl+T). The report gives the ribbon's measured width (`AIPATH_main: a closed ribbon, ...
wide`) and says which it is when it is standing up or too narrow.

- **Height:** shape the curve to follow the road up and down. The game drops it onto the road below it, so over
  bridges and through tunnels a flat curve can pick the wrong level.
- **Jumps:** over a jump, draw the curve along the arc the cars fly. Where the road under the curve drops away (a gap,
  or another part of the track passing underneath), the game keeps the path at the curve's height instead of dropping
  it, so the computer cars aim for the landing and a car put back on the road isn't put on the road below.
- **Width:** the ribbon's width sets how far apart the computer cars' four racing lines spread. Keep the whole ribbon on
  the road and clear of walls, or the cars on the outer lines drive into them. The report warns when more than 2% of
  the racing-line points have no road near them (the stock tracks have up to 0.7%, over their jumps).
- **Direction:** the race runs the way the markers' green arrows point. The curve can be drawn either way round; if most
  arrows disagree with it, the game reverses it, and a single marker pointing against the rest is reported and used
  turned round.
- **Where it crosses the markers:** a marker counts where the path crosses the line across the road through it (as wide
  as its X scale) within 9 m (30 ft) of its height. A stretch of road running alongside, a bridge over it or a tunnel
  under it isn't mistaken for it. Put each marker where the road passes only once: one the path crosses twice (where
  the road meets itself) is reported.
- **Backward races** use the same path the other way round, made automatically. Signs and arrows you model point one
  way only, so they face the wrong way in backward races.
- **Speeds:** the computer cars brake only where the path tells them to, so the game sets a target speed for every
  point from the tightest bend in the next 500 ft, the way the stock tracks' speeds run (83 to 158 mph, fitted to all
  of Rush 2's circuit tracks and Rush 2049's race tracks). To slow them down where that gets it wrong (jumps, blind crests), place
  an Empty named `AISPEED_<mph>` beside the path, such as `AISPEED_45`: it caps their speed from the nearest path point
  on, in the race direction, until the next `AISPEED_` marker. `AISPEED_0` lifts the cap.
- **Tight corners:** on level or rising road the computer cars never aim below 61 mph (92 and 123 mph with the Heavy
  and Jovian gravity cheats). Stock Rush 2 lanes never ask for less than 72 mph, so this only matters on unusually tight custom corners.

## track.toml

Only `name` is required. Anything left out uses the default.

```toml
[track]
name        = "Canyon Run"            # shown in the game
author      = "your name"
version     = "0.1"
description = "One short line"
type        = "race"                  # "race" (the default), "stunt" or "battle": see Stunt and battle arenas

[race]
# Timer (with checkpoints on). Leave these out and the game works it out from the AI path, as stock Rush 2 does.
start_time      = 45                  # seconds on the clock at the start; crossing the finish line adds it again
checkpoint_time = [[20, 18], [22, 20]] # per checkpoint, CHECKPOINT_1 first: seconds added [first lap, later laps]
best_times      = [95, 100, 105]      # the records table's starting entries are made from the fastest

[look]
fog_color    = "#a0b8d0"   # also the color seen past the draw distance
sky          = "none"      # a stock sky: see Sky
cloud_scroll = 0.0         # sky texture scroll speed (the clouds sky)
sky_parallax = 0.0         # how much the sky shifts as the camera moves (the clouds sky)

[movers.lift]              # a moving object's settings: see Moving objects
mode  = "ride"
races = "both"

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
- `[surfaces.mud]` and `[movers.lift]` above are examples: delete them, or use `COL_MUD_*` meshes and a `MOVE_lift`
  object in Blender.
- The lap count and mirror mode are the players' choice and aren't set here.

## Sky

Without a sky the sky is plain `fog_color`. Use one of the games' own skies, or model your own.

### Stock skies

Set `sky` in track.toml's `[look]` to one of these:

| `sky` | What |
|---|---|
| `"none"` | Plain `fog_color` (the default) |
| `"clouds"` | Rush 2's cloud dome, as on most Rush 2 tracks. `cloud_scroll` and `sky_parallax` move it. |
| `"las_vegas"` | Las Vegas's sunset over the mountains |
| `"stunt"` | The stunt arena's night sky |
| `"marina"`, `"haight"`, `"civic"`, `"metro"`, `"mission"`, `"presidio"` | Rush 2049's race track skies. These need the Rush 2049 ROM (set in the Games tab); without it the sky is plain `fog_color` and the report says so. |

The stock skies other than `clouds` are drawn in split screen too. They sit round your road as they do on their own
track: each is raised or lowered by how far your `FINISH` is above or below the start of its own track. A `SKY` mesh
takes the place of a stock sky (the report warns when you have both).

### Your own sky

To model the sky yourself:

1. **Make a dome around the origin.** Add > Mesh > UV Sphere (a cube works too) with its center at the world origin.
   Size doesn't matter: the game scales it to fit and draws it around the camera, so it never gets closer or
   further away as the cars move. Cutting off the lower half is fine; the track hides the bottom of the sky.
2. **Turn its faces inward:** in Edit Mode, select all and use Mesh > Normals > Flip, since the camera is inside it.
3. **Color it** with a texture (a panorama mapped onto the sphere) or with vertex colors (Vertex Paint), for example
   a gradient from the zenith down to the horizon. Without vertex colors it gets the track's fixed light from above,
   which darkens its sides; paint it white to keep a texture's own colors.
4. **Add stars, a sun or clouds** as separate faces just inside the dome, in a material with Alpha Clip (cutouts).
   The sky is drawn without depth, in the order its materials first appear in the file, so keep the dome's material
   first and the details' after it, or the dome paints over them.
5. **Name the object `SKY`.** Several objects named `SKY`, `SKY.001`, ... are joined into one sky.

The sky is drawn behind everything and isn't fogged, as Rush 2049's skies are. The report gives its triangle count
(`SKY: ... triangles`).

## Moving objects

Lifts, spinning signs, swinging gates and the like are animated in Blender and play in the game.

1. **Make an Empty (or a mesh) named `MOVE_<name>`**, such as `MOVE_lift`, and put the moving parts under it as its
   children: meshes to draw, and `COL_*` meshes for the parts cars hit or drive on. Everything under it moves with it.
2. **Animate `MOVE_<name>` itself:** keyframe its Location, Rotation and Scale on the timeline. Parts under it with
   animations of their own just follow `MOVE_<name>` (the report says so).
3. **Export with Animation ticked** in the glTF export settings.
4. **Pick how it plays** in track.toml (optional):

```toml
[movers.lift]          # for MOVE_lift
mode  = "ride"         # "loop" (default), "pingpong" or "ride"
races = "both"         # "both" (default), "forward" or "backward": the races it is there in
```

| `mode` | Plays |
|---|---|
| `"loop"` | The animation over and over. For a smooth loop make the last key the same pose as the first; otherwise it jumps back to the first pose at the end. |
| `"pingpong"` | Forward to the end, back to the start, and again. |
| `"ride"` | Waits at its first pose until a car is on one of its `COL_*` floors, then plays forward to the end and back. For a lift, give it a `COL_ROAD_*` floor. |

Moving floors carry the cars on them (up, down, sideways and turning) and moving walls push them. The animation keeps
its timing in seconds whatever the frame rate.

Limits:

- 31 moving objects with collision (any number without).
- 2,000 keys per animation. A longer one is sampled at 2,000 evenly spaced times (the report says so).
- Every part within about 610 m (2,000 ft) of `MOVE_<name>`'s origin.
- One animation per object: the first one that moves it. Keep one action per moving object.
- Location, rotation and scale only. A negative (mirrored) scale is drawn turned instead.

The report has a line per moving object (`MOVE_lift: 2 keys over 4.00 s, ride, 2 triangles, collision moves with
it.`) and warns about a `MOVE_` object without an animation (it stays put, as ordinary track) and a `[movers.<name>]`
with no object of that name.

## Stunt and battle arenas

Set `type = "stunt"` or `type = "battle"` in track.toml's `[track]` to make an arena instead of a race track. A stunt
arena is listed on the Start Game menu's STUNT track select and played as Rush 2's stunt mode; a battle arena on the
BATTLE track select (it needs the Rush 2049 ROM, as the battle does) and played with Rush 2049's weapons and rules.

An arena has no lap, so it needs no `FINISH`, `CHECKPOINT_n` or `AIPATH_main` (the report says it ignores them).
Instead:

| Name | What |
|---|---|
| `START_1` to `START_4` | Where the cars start, green arrow forward. A stunt arena uses `START_1`; a battle puts player 1 on `START_1`, player 2 on `START_2` and so on. Cars past the starts begin on the route, as far from the others as it goes. Without any, `FINISH` stands in for `START_1`, or else a spot in the middle of the floor. |
| `PICKUP_<kind>` | Battle only: a weapon pickup, `<kind>` one of `CANNON`, `GATLING`, `GRENADE`, `MINE`, `MISSILE`, `RAM`, `ROCKET`, `SONIC`; or `PICKUP_POWERUP`, a power-up spot. Put it where the pickup floats; one too close to the floor is lifted to 2.5 ft (0.8 m) over it. Blender's `.001` suffixes are fine. |

The computer cars and crash respawns need a route over the floor. The game makes one: points about 30 ft (9 m) apart on
every floor (`COL_ROAD`, `COL_DIRT`, `COL_GRASS` or a custom surface) a car can drive to from `START_1`. To choose the
route yourself, draw `AIPATH_main` as for a race track. The report gives the route
(`The arena's route: 442 points on the floor ...`) and counts the pickups (`Pickups: 2 weapon(s), 1 power-up
spot(s).`).

Arenas take the same `COL_KILL_*`, `SKY`, `MOVE_*` objects and track.toml settings as race tracks; the timer
settings in `[race]` don't apply.

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

Open the Mods menu and select the mod: under its description, each of its tracks is listed with what the game found
wrong, problems in red and warnings in yellow (a controller can scroll the list). The same report is written to
`track_reports/<folder>.txt` in the game's settings folder (`%LOCALAPPDATA%\Rush2Recompiled`, or the game's folder in
portable mode). Each entry names the object and, where it helps, the place.

Once you're happy with a track and its remaining warnings, you can keep them out of players' Mods menu: add
`"hide_track_warnings": true` to the mod's `mod.json` (next to `"id"`). It applies to every track in the mod. Errors
are still shown, and the report file still lists everything.

Problems don't keep a track off the track select: it loads as it stands, so you can drive it and see what they do (a
checkpoint left at the origin, say, can't be reached, so the clock runs out). Missing or broken pieces get stand-ins:
with no usable `AIPATH_main` the computer cars and respawns follow a straight path through the markers in order (or a
circle from `FINISH` when it is the only marker), with no `FINISH` a finish line is put across the path near its start
(or just before `CHECKPOINT_1`), with no visible meshes the `COL_*` meshes are drawn, and what is past the size limits
below is left out (holes in the collision, triangles not drawn). A track is left out only when its track.glb can't be
read, it has no collision and nothing to draw, or its collision is too dense for the game's collision index. The Mods
menu then says it can't be raced and why.

The game converts a track once and reuses the result until its files change. It looks at the files when it starts and
when mods are turned on or off, so after a new export restart the game, or turn the mod off and on again in the Mods
menu. The report covers, among other things:

- markers that are missing, misnumbered, out of order along the path, or not crossed by it
- an AI path that isn't a ribbon, isn't closed, floats above the road or leaves it
- collision that is too dense
- unknown `COL_` names, a `*` typed literally in a name, track.toml mistakes
- the track's size, its textures, and each custom surface's settings

## Previewing your track

Trackview shows your track in a web browser the way the game will load it. You don't have to start the game or drive
there to see it.

![Trackview showing a converted track](images/trackview.png)

It shows:

- the collision, colored by surface (road, dirt, grass, walls, kill and water, and your own surfaces), each of which
  you can hide
- the AI path: its center line with arrows for the race direction, and the four racing lines colored by the speed
  the computer cars aim for
- the finish line and checkpoints, and where each racing line crosses them
- the converter's report, with each place it mentions marked on the track
- the backward race (pick Backward)

Click any point for its details (a racing line point's target speed, its Blender coordinates). Drag to orbit, right
drag to pan, scroll to zoom, F to frame the track; while you move the camera a small cross marks the point it turns
around.

**Run AI** races the computer cars around your track in the real game. The race runs hidden and as fast as your PC
manages: a three-lap race takes about half a minute. The game uses its own settings and save for it, so nothing
you've set or saved is touched. Then watch the race: play, pause, rewind and fast forward on the timeline, follow a
car by clicking it in the car list, and jump to wrecks, respawns, lap times and cars that got stuck from the event
list.

### Installing it

Trackview comes with the game's source code, not the release download. You need:

- a copy of this repository with the game built (see [BUILDING.md](BUILDING.md)); the Rush 2 ROM in `build/` is all
  Run AI needs
- [Python](https://www.python.org/downloads/) 3.12 or newer, with no extra packages
- the Visual Studio Build Tools from the build guide (trackview builds the track converter with them the first time)
- Edge, Chrome or Firefox, online the first time the page loads (it fetches three.js)

### Starting it

From the repository's folder:

```
python tools/trackview/server.py --track "C:\path\to\your mod\rush2\tracks\my_track"
```

Your browser opens on the page (`http://127.0.0.1:8766`). Pick a track at the top left. Besides `--track` folders
(give it more than once for several), it lists the tracks of the mods in `build/mods/`. After a new export press
Reload. Close the command window to stop it.

How it works inside is in [Track previewer](#track-previewer) and [Race replays](#race-replays).

## Size limits

| What | Limit | Why |
|---|---|---|
| Collision polygons | 8,191 after flat neighbors are joined, and at most 255 in one small area | The game's collision format. Stock tracks use 2,100 to 7,100. |
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

- Props that react when hit (`PROP_*` Empties are ignored)
- Translucent (blended) materials
- Moving objects that react to the race, such as traffic or a train that waits for the cars: they play on their own
  clock (a ride only waits for a car on its floor)
- A run-up before the lap: the AI path must be a closed loop through `FINISH` (NY One and Rush 2049's race 6 start
  outside their loop)
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
- **Arenas:** track.toml `type` (`rush2::track_mod::RaceType`, `race_type()`) puts a custom stunt or battle arena in
  the STUNT1 slot (`stunt_host_slot`) and on the STUNT or BATTLE track select (`mod_type`, `is_arena` in
  `track2049_menu.cpp`); arenas are never in a circuit and have no collectibles. The converter writes `START_n` as the
  path's gates and a route over the floor as its spine and lanes, and `PICKUP_` markers as `pickup_records` (record
  -1: no placement record; `battle.cpp` draws them in renderer slots). `tools/track_mod_cases.py` has the arena cases.
- **Custom surfaces:** collision flags are `type | material << 4`, custom surfaces use material slots 3-15 except 8.
  The wheel probe `collision_poly_extract_8006F704` stores the type (physics) at car +0x5FC + 4 x wheel and the
  material at +0x60C + 2 x wheel. The effects, sounds, particles and stunt scoring only know materials 0-2 and 8
  (`car_material_effects_update_800663CC` raises dust when a wheel's material is 1), so a hook at 0x8006FA90
  stores a custom material as its `like` material and keeps the custom slot per wheel on the side
  (`rush2_trackmod_wheel_material`; 0x8006FBE4, the no-polygon fallback, drops it). Drag: hooks at 0x8006B63C (dirt)
  and 0x8006B6A4 (grass) in `car_wheel_forces_drag_8006AFD8` replace the -10 / -15 coefficient. Engine torque:
  `car_engine_torque_80070DB8` with both rear wheels on dirt or grass above 40 rad/s uses the common torque map
  (+0x400) x descriptor +0xC4 instead of the gear's map (+0x3FC) x the gear's scale (+0xB8/+0xBC/+0xC0); hooks at
  0x80070E6C (dirt) and 0x80070F14 (grass) blend it toward the gear torque by drag / stock drag (clamped to 0-1).
- **Backward:** the converter writes `path_backward` (asset 0x63 + slot): the spine reversed from the finish, the gates
  in reverse order and turned round, target speeds recomputed. See [race.md section 5](rush2049_research/race.md).
- **Checkpoint crossings:** Rush 2 works out where each path crosses each gate at load
  (`path_compute_checkpoint_crossings_80092D6C` through `checkpoint_crossing_spine_80092BC4` and
  `checkpoint_crossing_lanes_80092884`): for each gate on its own, the first point from the path's start that changes
  side of the gate's plane with it and the point before inside the radius, seen from above, else the nearest point. On a
  compact track another stretch of road (alongside, over or under the gate) often comes first, which puts a checkpoint,
  the grid (lined up at gate 0's spine crossing) and the respawn search in the wrong place. So the converter traces the
  AI path through each gate in 3D (`gate_height` 30 ft), takes the race direction from the gates' arrows (a majority,
  FINISH breaking a tie), starts the spine at FINISH's crossing, and writes every crossing (spine and the 4 lanes, each
  lane's own first point past the gate) into the path files' checkpoint +0x22 slots for both directions. Hooks at the
  entry of 0x80092BC4 and 0x80092884 (`rush2_mod_checkpoint_crossing`, `src/rush2049/track2049.cpp`) return those for a
  custom track instead of searching; a slot of -1 (a gate the path doesn't cross) keeps the game's search. Stock check:
  on every stock gate the path crosses once, the converter's crossings equal the game's own search, except Crash's lane
  0 at gate 3 forward, where the game's search lands 129 points early on another stretch.
- **Path height:** each spine point is dropped onto the road nearest the curve's height (`ground_under`, 400 ft
  reach), except a run of points off the road (over 10 ft) whose road lies below the curve or isn't there, when the
  road steps down more than 15 ft at the run's ends or is where the path itself passes over 300 ft of lap away
  (within 30 ft across): those are in the air (`PathPoint::air`) and keep the curve's height, lanes included. The
  stock paths do the same over their jumps (2049 race 1's converted jump matches its stock heights).
- **Stock skies:** `clouds` keeps Rush 2's dome: `mod_cloud_dome` (`track2049.cpp`) leaves the hooks on
  `sky_setup_800A45A8` (0x800A4644 track id, 0x800A4600 players) to Rush 2's own branch. The others are copied into the
  converted geometry as `SKYO1`: `stock_sky_container` (`track_mod.cpp`) reads the source (Rush 2 asset 0x33 or
  0x33 + 11 through `assets::read_original`, Rush 2049 track k through `track2049::convert_file(100 + k)`) and hashes
  it into the cache key; `plan_copy` collects the model's vertices, sub lists, load lists ([7]..[8]) and the texel
  blocks they load (LOADBLOCK / LOADTLUT / LOADTILE sizes), and `copy_list` moves every pointer. Rush 2 moves a sky
  model with the camera in x and z only (`render_sky_cloud_scroll_8008918C` at 0x80089454 writes the view camera's
  +0x24 / +0x2C into the sky matrix, never y), so its height is world space: the copy's vertices are raised by
  (FINISH height - `StockSky::ground`) x 16, where `ground` is the start of the source track's AI path (spine point 0
  of Rush 2 path asset 0x57 + 2 x track, 2049 file 157 + k). Marina's sky starts 6,400 units up, over Marina's 531 ft
  start. The stunt sky's band is black by its own vertex colors (a night sky), not a bad copy.
- **Moving objects:** each animated `MOVE_` node becomes a Rush 2049 path, drawn and run by the 2049 movers port
  (`src/rush2049/track2049_movers.cpp`, `track2049_movers_logic.cpp`, docs/rush2049_research/movers.md), handed over
  with `set_mover_data(0, mover_paths, mover_collision, path_records, ...)` (`light_track` 0 = a custom track). The
  converter samples every channel (LINEAR with slerp, STEP, CUBICSPLINE) at the union of the key times (at most 2000)
  and writes a chunk directory at +4 with one PTHD per object. Path flags: 0x8000 no model | loop 0x2 or pingpong 0x1
  or ride (wait_trigger 0x40 | one_shot 0x4000) | race direction 0x4 / 0x8, plus moves_group 0x20 for an object with
  collision and switch_group 0x800 when that object is in one direction only. Nodes (0x44 bytes) hold pos, dir, scale
  and `quat_2049`, the standard quaternion of D R D with D = diag(1, -1, 1), since `movers::quat_to_matrix` builds
  D R(q)^T D; node flags 0x8 (constant speed) and 0x1 (spawn) on node 0. A closed loop drops its last key, an open
  loop's last node takes 0.001 s back to key 0, and pingpong and ride end on a node of time 1 and distance 0. Each
  object has its own model (`<prefix>M%03u`, in its own frame) and a placement record after FINISH (its key-0 matrix
  transposed, a zero box) named by `path_records`. Collision: `COL_*` parts under a mover are built into the track's
  polygons at their key-0 pose with polygon info group << 11 (| 0x20 on floors, which carry cars and start rides),
  never merged across groups, and listed in the quadtree over the box they sweep through every key; `mover_collision`
  holds the 2049 MOVER records (0x20 bytes: group, polygon, rest matrix s16[9], vertex +0x16, rest vertex +0x18).
  Changes to the port, for custom tracks only: `setup()` makes a type per path (flags 0x21, or slot 5 << 16 for
  rides). `World::hold_triggers` keeps a ride's one-shot trigger set while a car's bits are set: Rush 2 steps the
  drones every other physics tick, so the trigger test saw them leave and come back and turned the ride round.
  `Options::custom` measures the rotate op's rest copy from the spawn pose with the full inverse of its scaled matrix
  (`Follower::rest_pos`, `rest_inv`): 2049's rotate (`func_800BF838`) measures it from the object's current position,
  which undoes the translate op of an object that also moves (2049's rotating groups turn in place), and every custom
  mover runs `rotation()`; before this a lift's floor stayed on the ground while its model rose. `pad_or_platform`
  carries a car on a rising or falling floor by the floor's vertical speed (2049 carries only sideways). The cache
  format is version 2 (`track_cache.cpp`: `mover_paths`, `mover_collision`). `tools/track_mod_cases.py`'s `movers`
  case checks the flags, node counts, every key's vertex pose against the glTF (a parented, scaled and turned object
  included), the records and the quadtree coverage.
- **Sky:** `SKY` meshes become one model named `SKYO1` (not in the placement), whose list switches to 1-cycle,
  TEXEL0 x SHADE, render modes 0x0F0A4000 / 0x0F0A3000 (opaque / alpha-tested, no fog) and clears G_ZBUFFER and G_FOG,
  then puts the track's state back, as Rush 2049's SKYSKY lists do; its vertices are scaled to fit the s16 range
  (32000 units). `track2049.cpp` sets `track_has_sky` from the converted model names, so the hooks on
  `sky_setup_800A45A8` (0x800A4600, 0x800A4644) build it through Las Vegas's sky branch, as for the 2049 tracks.
- **Hiding warnings:** `"hide_track_warnings": true` in a mod's mod.json (read in `scan`, `Entry::hide_warnings`) leaves
  warnings out of the Mods tab notes; the report file and console keep them.
- **Problems in the Mods menu:** `rush2::track_mod::register_mod_notes` gives recompui's mod details panel
  (`recompui::set_mod_notes_provider`, a `lib/patches/RecompFrontend.patch` addition) each track's errors and warnings.
  A track not checked yet is converted on a background thread (conversions are serialized by `convert_mutex`); the
  report is cached beside the track as `trackmod_report_<key>`. `convert` returns false only when track.glb can't be
  read, there is no collision and nothing to draw, or `build_collision`'s index overflows; everything else gets a
  stand-in (the "stand-ins" block before the race direction: a finish line, a path through the markers along their
  arrows, or a 150 ft circle from FINISH; the path's own height and off-road warnings are then left out) or is left
  out past a limit (far or excess collision polygons, oversized triangles, sections past 900). Thin or upright ribbons
  are used as they are (the AI drives the center line).
- **Full-size textures:** the converter's RGBA16 stand-ins are keyed by `rush2::upscale::rgba16_key`, cached as
  `trackmod_textures_<key>`, and published through `rush2::derive::set_mod_textures("tracks", ...)`.
- **Collision:** `build_collision` keeps each face's normal (no flipping), merges flat same-surface neighbors into
  convex polygons of up to 8 vertices, and builds the quadtree Rush 2's way; past 64 KB of leaves it builds the tagged
  form the 2049 port reads (splits toward zero, 17-bit leaf offsets, root byte +2 = 0x80 | size bits 16-22:
  `src/rush2049/track2049_collision.cpp`). A leaf holds at most 36 polygons (`max_leaf`): the game's queries copy a
  leaf into an unchecked u16[36] stack buffer, and a longer one overwrites the callers' locals
  (docs/rush2049_research/collision.md). With leaves of up to 255 the stock Alcatraz export had cells of 70, and a
  computer car's wall query (`car_wall_collision_query_8006FC94`) about 35 s into a race got its saved point pointer
  (sp+0x15C) overwritten by polygon numbers and crashed in `collision_poly_point_test_8006C670` (until 2026-10-15).
  A cell lists the polygons whose x/z outline (not box: a long diagonal strip's box covers far more than the strip,
  and stacked boxes on pipes and terraced slopes crowded single spots past 36) comes within 1 ft of it, walls within
  6 ft (the wall query reaches 5 ft past the car) unless that crowds the cell past 36. A cell past 36 splits below
  `min_cell` until it fits; one that never does fails with "too dense". `tools/rush2049/collision.py` validation
  flags leaves past 36.
- **Reading the ribbon:** `read_ribbon` builds the rungs from the mesh's shape, never the order its vertices are
  stored in. Blender's glTF export of a cyclic curve stores the first rung's two vertices twice each (the UV seam: u
  wraps 1 to 0), so pairing stored vertices two at a time read a 0 m first rung and failed every cyclic curve with
  "Extrude is too small" (until 2026-10-15); flat shading splits a vertex per face, and exporters may order vertices as
  they like. It merges vertices within 0.01 ft unless an edge joins them (Atari's stock ribbon has one rung pinched to a
  point), takes the boundary loops (one: an open curve, both edges and the ends; two: a cyclic curve, one per edge),
  pairs the two edges with the shift (and, for two loops, the direction) giving the shortest rungs in all, and keeps
  the stored order's start, direction and side (lowest index first), so a plain export reads as before. Errors: no
  faces, an edge shared by 3+ faces, edges meeting (a bow tie), no edges (Bevel Depth tube), unequal edges, 3+ loops;
  more than 2% of rungs under 1 ft wide seen from above is "stands on edge" (2 ft or more tall) or "only ... wide".
- **Checked against the stock tracks:** `tools/stock_track_glb.py OUT --check` exports Rush 2's ten circuit tracks and
  Rush 2049's six race tracks as custom tracks (collision, gates from the path files, a ribbon over the stock spine),
  converts them and counts each message across them. A message every stock track raises means the check is wrong. Stock
  path files point each gate's direction back against the race; the converter writes them the same way.
- **Case suite:** `python tools/track_mod_cases.py` builds a synthetic track for every layout and authoring mistake the
  direction, start and crossing logic must handle (curve reversed or starting mid-lap, ends overlapping at a marker,
  open curve, arrows reversed, markers beside, above or exactly on a rung, kinks and repeated rungs, the ribbon as
  exporters write it (Blender's cyclic seam, a welded loop, flat shaded, shuffled, under a scaled node) and broken
  (on edge, too thin, a tube, one rung pinched), stand-ins (no AIPATH_main, no FINISH, neither, FINISH alone, no
  markers, no visible mesh), a compact layout's
  wide gates reaching the leg before or after, same-way and other-way neighbors, bridges, a flat crossing, a spiral, a
  helix, a vertical loop), converts each and checks the report and the crossings in both path files against the ones
  it was built with; `--list` names them, `--only` runs some. Run it after any change to that logic.
- **Watching the AI race:** `python tools/race_watch.py --launch "<input script>" --path <out>/path.bin` (or `--pid`
  for a game already running) reads the race from the game's memory 10 times a second and logs every car's laps,
  finish, wrecks, respawns and stuck spells with their spine point, offset from the center line and lane speeds.
- **Testing:** `tools/track_mod_test.bat <track folder> <out folder>` converts a track outside the game, and
  `tools/track_mod_check.py <out folder>` checks the result. In game, `RUSH2_TEST_TRACK=38` opens the track select on
  the first custom track, `RUSH2_TEST_MOD_TRACK=<n>` races custom track n in place of HAWAII, and
  `RUSH2_TEST_BACKWARD=1` races backward. `RUSH2_TEST_MOD_FOLDER=<folder>` opens the track select on the custom track
  of that folder instead (`src/rush2049/track2049_menu.cpp`).

### Track previewer

`python tools/trackview/server.py` opens a browser preview of every track folder under `build/mods/*/tracks/` and
`tmp/stock_tracks/` (`--track <folder>` adds others; `.rtz` mods must be unzipped). `tools/trackview/bake.py` runs the
converter outside the game (`tmp/track_mod_test/track_mod_test.exe`, rebuilt through `tools/track_mod_test.bat` when
its sources are newer) and reads its output with `tools/rush2049/collision.py` and `paths.py`, so the page shows what
the game loads, not the glb: collision colored by surface (custom surfaces named from the report's `Surface COL_<NAME>
... (slot n)` lines), the spine with race-direction cones, the four lanes colored by target speed, the gates with
their stored crossings (spine and each lane; -1 falls back to `paths.recompute`), the backward path (backward gate i
is forward gate n - i, labeled by its Blender name), and the report with each quoted position pinned. Clicking a
point shows its index, target speed and Blender coordinates. While the camera moves, a small three-axis marker shows
the point it orbits. Results are cached in `tmp/trackview/`, keyed on track.glb, track.toml, the converter exe and
bake.py; Reload converts again. The page works in glTF meters (game = (-x, y, z) x 3.2808399 ft), so track.glb draws
over it as is. `?show=a,b&hide=c`, `?dir=backward`, `?click=x,y` and `?pivot` set layers, the path, an inspector click
and the orbit marker from the URL, for headless screenshots
(`msedge --headless=new --use-angle=swiftshader --virtual-time-budget=20000 --screenshot=...`).

### Race replays

The previewer's **Run AI** races the track headless in the real game, about 50 times faster than real time (a 7-minute
Wario Stadium race takes about 30 s, 20 of them the menus), and replays it. `tools/trackview/runs.py` starts
`build/Rush2Recompiled.exe` as a simulation run (`src/sim_run.cpp`, `RUSH2_SIM=1`):

- **Its own data folder** (`RUSH2_DATA_DIR` = `tmp/trackview/sim_data/`, registered as the config path in main): the
  run starts from default settings and a fresh save, so nothing the player has set (cheats, options, opponents,
  unlocks) changes it, and it never writes to the player's data. Before each run its settings, saves and mod configs
  are deleted (its `track_cache` and other caches are kept), the ROMs are hard-linked in from `build/`, and its only
  mod, `trackview`, is generated with `tracks/<folder>` a directory junction to the track folder, so any track folder
  the viewer lists can be raced.
- **No window, no sound, no real input:** the window is created hidden, audio isn't queued, and only the menu input
  script (`8:START,11:A,13:A,22:A,25:A,27:A,32:A`, `RUSH2_TEST_MOD_FOLDER=<folder>` picking the track,
  `RUSH2_TEST_CAR=0`, `RUSH2_TEST_BACKWARD=1` for the backward path) reaches the game.
- **Turbo** from the race's first physics tick (`lib/patches/N64ModernRuntime.patch`, `ultramodern::set_turbo`): the
  game's clocks (osGetCount, osGetTime, timers, VIs) run on a virtual clock that the VI thread steps 1/60 s at a time,
  as soon as the game has gone idle after the last VI (the idle thread's `pause_self` finds no game thread to run and
  no message waiting: `ultramodern::turbo_idle`), and the gfx thread skips rendering and presenting. Game threads run
  one at a time by N64 priority, so pacing on the physics tick alone starved the game thread (the countdown never
  ended). Rush 2's physics steps a fixed 1/60 s per VI, so the race is the one real time would give. The script's
  presses stop once turbo is on. `RUSH2_SIM_TURBO=0` runs the same race in real time.
- **Race rules:** the time limit is off (the No Race Timer cheat flag, 0x800D9E88 = 1). Player 1 is parked at the
  back of the grid and kept out of it: no stuck reset (`rush2_track49_stuck_timer`, which would otherwise put it back
  further along the course every few seconds) and no car-against-car tests (`rush2_ghost_collide_self` / `_other`).
  The computer cars keep every rule of the base game, their stuck resets and catch-up included.

`src/race_record.cpp` records from the end of every physics tick (`physics_tick_80076578` at 0x8007660C, the hook
`src/ghost.cpp` steps its ghosts from): per car its drawn pose (car state 0x801124A0 + car x 0x354: position +0,
velocity +0xC, orientation rows up +0x30 and forward +0x3C), speed (+0x3D4), steering, throttle and brake (+0x728,
+0x734, +0x730), laps (+0x7FD), next checkpoint (+0x7FE), place (state +0xE9), finished (+0xEA), wreck state
(+0x342), respawn glide (+0x6C8) and the physics clock of its last step (+0x714). The file's layout is in the source's
header comment. The recording ends once every computer car (+0x7E8 = 1) has finished or gone a minute without reaching
a new checkpoint, or the race ends, writing `<file>.done` (ticks, why, finish times); a sim run then exits. Only the
latest run of a track is kept: `tmp/trackview/runs/<folder>.r2rc`. `python tools/trackview/rec_dump.py <file>` prints
one as text.

The page (`tools/trackview/static/replay.js`) draws the cars as boxes with trails and plays the recording on its
physics clock: play/pause, rewind and fast forward from 1x to 32x, a scrubber marked with wrecks (red), stuck spells
(yellow: a computer car under 15 ft/s for 3 s while racing) and finishes (green), and single ticks. Drones are stepped
every other physics tick (round robin) and the car state publishes a step's pose a tick after the step's clock moves,
so each car is interpolated between the ticks where its pose changes, at the step times recorded with them, and snaps
across a jump of more than 20 m (a respawn). The car table shows each car's place, lap, speed, throttle and brake;
clicking a row follows that car, and clicking an event jumps to 2 s before it. `?seek=<s>&follow=<car>` opens the
recording there.
