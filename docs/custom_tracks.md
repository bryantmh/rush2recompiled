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
- [Billboards and props](#billboards-and-props)
- [Point to point](#point-to-point)
- [Run-in circuits](#run-in-circuits)
- [Stunt and battle arenas](#stunt-and-battle-arenas)
- [Track select art](#track-select-art)
- [Textures](#textures)
- [Checking your track](#checking-your-track)
- [Previewing your track](#previewing-your-track)
- [Everything you can use](#everything-you-can-use)
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

Give a `COL_*` mesh a custom property `covered` (Object Properties > Custom Properties) to shade the cars on it, as
Rush 2 does in its tunnels: `1` for a tunnel (cars drawn much darker) or `2` for a lightly covered spot such as under
a bridge or trees. Leave it out, or `0`, for open sky.

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

### Spline road

Flat polygons meet at creases, and a car feels every crease as a bump. The stock Rush 2 and Rush 2049 tracks smooth
their main roads with the game's spline road: a chain of segments, each a smooth curved stretch of road, and a polygon
on it takes its height from its segment instead of from its own flat face. This is meant for tools that convert
other games' tracks (tools/arcade2049 uses it), not for modelling by hand.

- An empty named `ROAD_SPLINES` with a custom property `segments`: a list of segments, each a list of 33 numbers, the
  game's segment record as it reads it, in game units (feet; game x is minus glTF x): origin (3), the world to segment
  matrix (9, rows across, up, along), the across direction (3), the along directions at the left and right edges
  (3 + 3), then half-width scale, length, the length's taper across (2), the left and right half-widths and their
  taper along (4), the up slopes at the left and right edges, and two unused. Each segment runs from its own start
  line to the next segment's (the next in the list; the last wraps to the first), so a chain's segments are listed in
  order. docs/rush2049_research/collision.md has the surface's formula.
- On `COL_*` meshes, a float attribute `_SEGMENT`: the segment a face takes its height from (its first vertex's value
  counts). Faces without it, or with -1, are flat as usual.
- A face only marks where its segment applies: keep it on that segment's stretch (outside it the game stretches the
  segment's curve on, and the further the worse) and within a foot or so of the curved surface (the game picks the
  polygon under a wheel by its flat face, then asks the segment for the height). Faces of different segments are never
  joined into one polygon, so a face per segment and road surface is the most it costs.

### Smooth collision

Every custom track's road gets a smooth tilt without any work by its author (src/smooth_collision.cpp; California
Speed's converted tracks get the same). Each vertex of the drivable `COL_*` faces (surface types road, dirt, grass
and the like; not walls) takes the mean normal of the faces that meet at its position, each weighted by how near it
lies to the face's own plane (falling to nothing at the fold angle: the weight is the 4th power of how far it is from
the fold toward flat, so a face 17 degrees off under a 20 degree fold counts 0.6%), leaving out neighbors that
fold away by more than the fold angle, and under a wheel the game blends the face's corner normals, so the tilt the
suspension and tires see changes smoothly across a face edge instead of jumping. Two faces that share an edge blend
the same two corners along it, so there is no jump at the edge. Heights are not changed: a car rests on exactly the
faces you built, and a face's flat shape still decides which face a wheel is on. So more detail in the road mesh
makes a smoother ride, not a bumpier one, and anything sharper than the fold angle (a curb, a ramp lip, a jump's
edge) keeps its edge. Faces on a spline road keep their segment's surface.

The same code also covers two flaws of a converted mesh that the game's wheel probe turns into kicks. The probe
takes only the faces whose footprint holds the wheel's point exactly, and of those the highest within 5 ft above the
point. A hairline crack between two faces (vertices that do not quite meet) makes a wheel find no floor for a frame,
and a raised ledge beside the road (a sidewalk, a platform) whose top is a plain floor face with no wall on its edge
lifts a wheel that strays under it by its full height in one step. With smooth collision on, a floor face whose
footprint is within 1 ft of the point counts too, the surface nearest the wheel wins (so a ledge more than 0.75 ft up
gives way to the road the wheel is on while any road is under it), and a ledge with no road left under the wheel is
climbed at 0.1 ft per probe instead of jumped onto (a car coming down fast, a landing, keeps the full depth so the floor holds it). A ramp is not a ledge:
a face whose plane is no more than 0.5 ft above the spot where the wheel last rested on a surface is the road going
on, and keeps its full depth however deep a fast wheel goes into it in one step (before this, cars sank through
California Speed San Francisco's start ramps and stood on end). A face steeper than the fold angle is never extended past its
edge. None of this touches Rush 2's own tracks.

- `[track] smooth_fold = 20`: the fold angle in degrees (0 to 90). An edge whose faces meet at more than this angle
  stays sharp. Lower it to keep more edges sharp, raise it to round more.
- `[track] smooth_collision = false`: the raw flat faces, as the game would drive them, cracks and ledges included.

The Rush 2049 arcade tracks the Games tab builds set `smooth_collision = false`: their road is built for flat faces.

(`rush2::track2049::mod_smooth_collision` hands the raced track's `smooth_collision` and `smooth_fold` to
src/smooth_collision.cpp, set as the race applies the track, so the wheel probes never read the settings.)

Checking it: RUSH2_SMOOTH_LOG=1 prints how many corners were smoothed and how many kept sharp when a race on the
track starts; RUSH2_SMOOTH_LOG=2 adds every probe of the player's car that found a surface more than a foot off, every
crack filled and ledge held, and (from `car_ground_contact_800703EC`) every wheel height over half a foot;
RUSH2_SMOOTH_FOLD=<degrees> tries another fold angle without editing the track.

### Road hold

`[track] road_hold = true` races the track with California Speed's road hold, as its own tracks are raced
(`rush2_calspeed_road_hold` and the hooks beside it in src/calspeed/calspeed_rom.cpp, explained in
docs/calspeed_research.md#road-hold): the world is 2.28 times as heavy with every car's springs, dampers and tire loads
scaled to match, so on flat road, slopes and corners a car drives as on any other track, but it takes 2.28 g to lift
off a crest, a car whose wheels leave the road falls at up to 4.6 g, and over a kink between two faces the car's
velocity away from the road is taken out so it follows the road instead of hopping. Use it for a track whose hills
are built like California Speed's (long crests taken flat out, made of large flat faces); jumps still fly, a little
shorter. The default is off: Rush 2's own gravity. It runs only while the player's CS Road Hold option (Games tab) is
on, which needs no California Speed ROM. It stacks with `gravity = "rush2049"`, which raises the gravity it scales.

## Markers

Markers are Empties (Add > Empty > Arrows). Rotate each so its green (Y) arrow points down the track in the race
direction, put it in the middle of the road, and scale its X so the red arrow just reaches the road's edge: the X
scale is half the gate's width. Leave the Empty's Display Size at 1, because the exported file keeps the scale but not
the display size. Move every marker into place: markers left at the origin are reported as errors.

| Name | What |
|---|---|
| `FINISH` | The start/finish line. The grid lines up behind it. On a point-to-point track, the finish line only. |
| `START` | A point-to-point or run-in track's start line ([Point to point](#point-to-point), [Run-in circuits](#run-in-circuits)). The grid lines up behind it. |
| `CHECKPOINT_1`, `CHECKPOINT_2`, ... | Checkpoints in lap order, numbered without gaps. At most 9. |
| `AISPEED_<mph>` | Optional speed cap for the computer cars ([The AI path](#the-ai-path)). |
| `TRIGGER_<name>` | A box that sets off moving objects when a car drives into it ([Trigger boxes](#trigger-boxes)). A mesh or a cube Empty; not drawn. |

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
- **Painting the lanes yourself (optional):** to place a racing line exactly, draw it as its own curve named
  `AIPATH_lane1` to `AIPATH_lane4`, numbered left to right as you drive the race (forward), each with a little Extrude
  (any width: its center line is the lane) and following the road's height. Paint any of them: a lane you leave out
  is spread across `AIPATH_main`'s ribbon as above. `AIPATH_main` is still needed: it is the race's progress line and
  sets the speeds, and the lanes are matched to it point for point, so keep each lane beside it, within about
  2.5 times its ribbon's half width (at least 18 m) and 9 m (30 ft) of its height. Where a lane wanders further (or
  leaves a gap), that stretch uses the spread lane instead and the report says where. Backward races drive the same
  lanes the other way. Arenas ignore these curves (their lanes are their route).
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
  on, in the forward race's direction, until the next `AISPEED_` marker. `AISPEED_0` lifts the cap. Backward races
  cap the same stretch of road.
- **Tight corners:** on level or rising road the computer cars never aim below 61 mph (92 and 123 mph with the Heavy
  and Jovian gravity cheats). Stock Rush 2 lanes never ask for less than 72 mph, so this only matters on unusually tight custom corners.
- **Your own speeds and steering (optional):** to set the computer cars' target speed point by point, give
  `AIPATH_main` a float attribute named `_SPEED`, in mph. Convert the curve to a mesh first (Object > Convert > Mesh,
  after setting its Extrude), add the attribute in Object Data Properties > Attributes (Domain: Vertex, Type: Float),
  set its values, and tick **Data > Mesh > Attributes** when you export. Where the path has `_SPEED`, those speeds are
  used as they are (1 to 255 mph, no extra braking added), and `AISPEED_` caps still apply; the report says
  `(all from _SPEED)` or `(some from _SPEED)`. A second attribute, `_AI_MODE`, sets how the cars steer along that
  stretch, as the stock lanes do: 2 (the default) holds the line closely, 5 steers more loosely. Painted
  `AIPATH_lane` curves can carry their own `_SPEED` and `_AI_MODE`; lanes without them use `AIPATH_main`'s. Backward
  races work out their own speeds.
- **Paths drawn at road height everywhere:** the game keeps the path at the curve's height over a gap only where the
  road below drops away sharply at the gap's ends. If your curve follows the road everywhere except over long jumps,
  where it follows the cars' arc, give `AIPATH_main` a custom property `keep_height` set to True (Object Properties >
  Custom Properties): the path then keeps the curve's height over every gap.

## track.toml

Only `name` is required. Anything left out uses the default.

```toml
[track]
name        = "Canyon Run"            # shown in the game
author      = "your name"
version     = "0.1"
description = "One short line"
type        = "race"                  # "race" (the default), "stunt" or "battle": see Stunt and battle arenas
gravity     = "rush2"                 # "rush2" (the default) or "rush2049": Rush 2049's, 15% stronger at Normal
road_hold   = false                   # true: California Speed's road hold keeps cars on crests: see Road hold
point_to_point = false                # true: raced once from START to FINISH: see Point to point
run_in      = false                   # true: starts on a run-in into the loop: see Run-in circuits

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

[movers.lift]              # a moving object's settings (and start, open, close...): see Moving objects
mode  = "ride"
races = "both"

[surfaces.mud]             # custom surfaces: see Collision surfaces
like = "dirt"
drag = 14

[audio]
music = "track5"           # default song: "track1" to "track8" (left out: each race a random one of every song, any game's, switched on in the Sound tab); players can still change it

[menu]
logo    = "logo.png"
preview = "preview.png"
diorama = "diorama.glb"
after   = "rush2049"      # where the track select lists it: see Track select art
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
   Model it at its real size, big (miles across): the game scales it down to fit and draws it around the camera,
   placed so it looks as it would at full size (it never gets closer as the cars move, and distant land or water in
   it stays at its true angle below the horizon). A small sky (under about 2,000 ft) stays at the height you put it. Cutting off the lower half is fine; the track hides the bottom of the sky.
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
mode  = "ride"         # "loop" (default), "pingpong", "ride", "once" or "door"
races = "both"         # "both" (default), "forward" or "backward": the races it is there in
```

| `mode` | Plays |
|---|---|
| `"loop"` | The animation over and over. For a smooth loop make the last key the same pose as the first; otherwise it jumps back to the first pose at the end. |
| `"pingpong"` | Forward to the end, back to the start, and again. |
| `"ride"` | Waits at its first pose until a car is on one of its `COL_*` floors, then plays forward to the end and back. For a lift, give it a `COL_ROAD_*` floor. |
| `"once"` | Plays forward to the end one time and stays there, such as a bridge that falls. Starts at the race start unless it has a `start`. |
| `"door"` | Sits shut (its first pose) until an `open` event, plays forward to its last pose (open) and stays; a `close` event plays it back. |

Moving floors carry the cars on them (up, down, sideways and turning) and moving walls push them. The animation keeps
its timing in seconds whatever the frame rate.

### Making it react to the race

Give a moving object events and it waits for them instead of playing from the moment the track loads. An event is
a short piece of text:

| Event | Happens |
|---|---|
| `"time 10"` | 10 seconds after the green light |
| `"trigger gate"` | A car drives into the box `TRIGGER_gate` (see below) |
| `"trigger gate lap 2"` | A car on its 2nd lap drives into `TRIGGER_gate` |
| `"odd_lap"` | The leading car starts lap 1, 3, 5, ... |
| `"even_lap"` | The leading car starts lap 2, 4, 6, ... |
| `"final_lap"` | The leading car starts the last lap |
| `"progress 14000"` | The leading car is 14,000 ft into the race, counting every lap: on a 12,000 ft lap that is 2,000 ft into lap 2. Measured along `AIPATH_main` from the finish line (on a point-to-point track, from its start, with no laps). Needs `AIPATH_main` |
| `"last_checkpoint"` | Player 1 passes the last checkpoint before the finish on the last lap (the finish banner turning to FINISH, say) |

Each setting takes one event or a list of them; any one of a list sets it off.

| Setting | For | What it does |
|---|---|---|
| `start = "time 5"` | `loop`, `pingpong`, `once` | Waits at its first pose until the event, then plays as its mode says. Without it, a `once` plays at the race start and a `loop` or `pingpong` plays from the moment the track loads. |
| `open = ["trigger button", "time 30"]` | `door` | Opens (plays forward) on the event |
| `close = "final_lap"` | `door` | Closes (plays back) on the event |
| `open_delay = 8` | `door` | After it has shut, opens again by itself this many seconds later |
| `close_delay = 8` | `door` | After it has opened, shuts again by itself this many seconds later |
| `open_at_start = true` | `door` | Starts the race open (at its last pose) instead of shut |

A drawbridge that lifts when the leader starts the final lap, and a ramp a car opens by driving through a box, which
shuts again 8 seconds later:

```toml
[movers.bridge]
mode  = "once"
start = "final_lap"

[movers.ramp]
mode = "door"
open = "trigger button"
close_delay = 8
```

### Trigger boxes

A trigger is a box cars set off by driving into it. Make a mesh or an Empty (Display As: Cube) named
`TRIGGER_<name>`, such as `TRIGGER_button`, and size it over the spot: a mesh's bounds, or the Empty's cube (its scale
is the half size). It isn't drawn and cars pass through it. Its name is the one events use (`"trigger button"`, upper
or lower case). Wrecked cars and ghosts don't set it off. A trigger no event names gets a warning, and two with the
same name are an error. Triggers don't move, even under a `MOVE_` object.

### Moving billboards

Give `MOVE_<name>` a custom property `billboard` set to True (Object Properties > Custom Properties) and its meshes
turn to face the camera as it moves, like a [billboard](#billboards): smoke, a flag, a flat sign that slides. Model it
facing Blender's front view (-Y).

### Moving object limits


- 31 moving objects with collision (any number without).
- 2,000 keys per animation. A longer one is sampled at 2,000 evenly spaced times (the report says so).
- With collision, every part within about 610 m (2,000 ft) of `MOVE_<name>`'s origin. Without collision, any size: a
  mesh reaching further is stored a little more coarsely (to 4 cm at 1,200 m), fine for far scenery such as a plane
  circling the track.
- One animation per object: the first one that moves it. Keep one action per moving object.
- Location, rotation and scale only. A negative (mirrored) scale is drawn turned instead.

The report has a line per moving object (`MOVE_lift: 2 keys over 4.00 s, ride, 2 triangles, collision moves with
it.`) and warns about a `MOVE_` object without an animation (it stays put, as ordinary track) and a `[movers.<name>]`
with no object of that name.

## Billboards and props

### Billboards

A mesh named `BILLBOARD_<name>` turns about its upright axis to face the camera wherever it is seen from, like a tree
drawn on one flat card (Rush 2's flat parking meters work the same way). Model the card standing up, facing
Blender's front view (-Y), with the object's origin at its foot. Its location and scale are used; its rotation is
ignored, because it always stands upright. Billboards of the same mesh at the same scale share one model, so a whole
row of trees costs little.

### Props

Meshes named `PROP_<kind>_<name>` react when a car hits them, the way Rush 2049's do:

| Name | When a car hits it | As Rush 2049's |
|---|---|---|
| `PROP_KNOCK_*` | Flies off spinning and is gone after 5 seconds | cones |
| `PROP_TIP_*` | Turns to face the car, tips over and stays down | road signs |

Put each prop's origin at its foot: a sign tips over about it. Its location, rotation and scale are kept. Give a prop
a custom property `billboard` set to True (Object Properties > Custom Properties) to make it a billboard too: it faces
the camera and keeps facing it as it flies off, which suits flat cones. Every prop is back in place at the start of
each race. There are no collectibles (coins, keys) on custom tracks.

Props react only when the Rush 2049 ROM is set in the Games tab, because their movement and sounds come from
Rush 2049. Without it they stand still and cars drive through them.

### See-through materials

Set a material's Blend Mode to **Alpha Blend** (glTF BLEND) for glass, glows, smoke or soft clouds. It is drawn
see-through by its texture's alpha after the solid scenery, the way Rush 2 draws its own windows. This works on
scenery, billboards, props, moving objects and the `SKY`. See-through surfaces aren't sorted against each other, so
two that overlap on screen can show in either order: keep them apart, or use Alpha Clip where hard edges will do.

## Point to point

`[track] point_to_point = true` makes a race that runs once from a start line to a finish line somewhere else, as
California Speed's courses do, instead of laps round a circuit:

- `AIPATH_main` is an open curve (not Cyclic) from behind the start to the finish and a little past it. If the curve
  is drawn from the finish end, it is turned round.
- `START` (an Empty, like the other gates) is the start line. Leave it out and the start line goes 30 m (100 ft) along
  `AIPATH_main` from its start. The grid lines up behind `START`, four rows 6 m (20 ft) apart, so begin the curve
  about 30 m (100 ft) before it; the report warns when there isn't room.
- `CHECKPOINT_1` to `CHECKPOINT_8` lie in order between them, and `FINISH` is the finish line. With no `FINISH`, the
  finish line is the end of `AIPATH_main`.
- The race is one lap: the track select hides LAPS and BACKWARD for it.
- Cars that have finished keep driving and bump off whatever ends the road without wrecking, as on California Speed's
  tracks.
- `[race] start_time` is the clock at the start, and `checkpoint_time` has one entry per `CHECKPOINT_n`. The time
  added at the finish doesn't matter.

The path file is written as California Speed's point-to-point ones are: checkpoint 0 is `START` (flags 2), the last
is `FINISH` (flags 1), and the header's loop, finish and arm checkpoints are 0, the last and 0. The converter's
`p2p` / `open_path` branches (track_mod_convert.cpp) keep the path open, and `rush2::track_mod::point_to_point` /
`rush2::track2049::mod_point_to_point` give the race one lap, hide the rows, and extend `rush2_calspeed_finished_car`
to these tracks.

## Run-in circuits

`[track] run_in = true` makes a circuit whose race starts on a stretch of its own that leads into the loop, as Rush 2's
New York One, Rush 2049's race 6 (Presidio) and San Francisco Rush's Golden Gate do. The first lap runs from `START`
along the run-in and round to `FINISH`; the laps after that go round the loop.

- `AIPATH_main` is an open curve (not Cyclic): it starts about 30 m (100 ft) behind `START`, runs along the run-in,
  goes once round the loop and ends where it first came onto the loop, a few meters before the checkpoint there. That
  checkpoint is where the loop starts. If the curve is drawn from the loop end, it is turned round.
- Put a `CHECKPOINT_n` where the run-in meets the loop. The report names the checkpoint it took as the loop start;
  the curve's end has to finish within 90 m (300 ft) of it. A curve drawn on past it is cut there.
- `START` is the start line and the grid lines up behind it. Leave it out and the start line goes 30 m (100 ft) along
  `AIPATH_main`. `FINISH` is the lap line, anywhere on the loop. The checkpoints are numbered in the order the race
  meets them from `START`, and `FINISH` may come before the last of them (Golden Gate has a checkpoint after its
  finish line).
- LAPS works as on any circuit; the track select has no BACKWARD for it.

The path file is written as Rush 2's run-in ones are (New York One: header loop, finish and arm checkpoints 1, 5 and 0):
the gates in the order the path meets them, `START` flagged 2, the loop start 4 and `FINISH` 1. The spine and the lanes
end at the loop start's crossing, and past the spine's last point the game carries a car's progress on from that
crossing (`func_8008CCF8`, docs/rush2049_research/race.md). The converter's `run_in` branches (track_mod_convert.cpp)
pick the loop start, put the gates in race order and wrap the target speeds' look-ahead and braking from the path's
end to the loop start; `rush2::track_mod::run_in` hides BACKWARD and keeps the race forwards.

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

### Where it is listed

Custom tracks come after the stock and added tracks, in the mod order. `after` in `[menu]` lists a track elsewhere:

| `after =` | Listed |
|---|---|
| `"rush2"`, `"sfrush"`, `"rush2049"`, `"calspeed"` | right after that game's tracks on the same select (a stunt arena after that game's stunt arenas, and so on) |
| a custom track's folder name | right after that track |

Several tracks after the same one keep the mod order. A name that matches nothing leaves the track at the end (the log
says so). Only the order changes: records, ghosts and saved choices are kept under the track's folder name as before.

For developers: the ids stay 38-63; `build_order` in src/rush2049/track2049_menu.cpp makes the display order, the
carousel is put in that order once built (0x803AB878) and TRACK left / right takes the track at the new carousel
position (0x803AC12C).

## Textures

Use PNG or JPEG textures embedded in the .glb, power-of-two sizes (256, 512, 1024...) up to 2048 px. The game keeps
small copies in its own memory and draws your full-size originals on screen, so texture size and count don't count
against the memory budget. A material's base color tints its texture.

Materials with Alpha Clip (glTF MASK) draw as cutouts. Alpha Blend (BLEND) materials are see-through
([See-through materials](#see-through-materials)).

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

**Export track** saves the track as a custom track folder, zipped, that the game reads back as the same track:

- A custom track: its folder's files exactly as they are.
- A stock track (Rush 2's, Rush 2049's, San Francisco Rush's and California Speed's): a track.glb with everything a
  modder would build. That means the collision as `COL_<surface>_stock` meshes, the gates as `FINISH` / `CHECKPOINT_n`
  (or `START` ... `FINISH` on a point-to-point or run-in course) and `AIPATH_main`. The four racing lines come as `AIPATH_lane1`
  to `AIPATH_lane4` with the game's own target speeds and steering in `_SPEED` and `_AI_MODE`, and the track's
  textured model is included. A track.toml comes with it, holding the game's settings for that track: Rush 2049
  gravity, California Speed's road hold, point to point, run-in.

What a stock export can't keep: the collision is the game's own (polygons as the game cut them, not the faces they
were modeled from), and lanes are re-spaced at 6 m (20 ft) like any custom track's. California Speed's start lines
are moved 30 m (100 ft) along the road for the grid, as the game moves them; the run-in courses (New York One, Rush
2049's race 6, Golden Gate) keep theirs, and their AI path is drawn 30 m (100 ft) back behind the start line instead. California Speed's shared
roadside objects (cones, lamps, trees) aren't in its track files, so they're missing.

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

## Everything you can use

Every name, custom property and setting a track can use, on one page. Each links to where it is explained.

### Object names in Blender

| Name | What |
|---|---|
| `COL_ROAD_*`, `COL_DIRT_*`, `COL_GRASS_*`, `COL_WALL_*`, `COL_WATER_*`, `COL_KILL_*`, `COL_<NAME>_*` | Collision: what cars drive on and hit ([Collision surfaces](#collision-surfaces)) |
| any other mesh | Scenery: drawn, not hit |
| `FINISH`, `START`, `CHECKPOINT_n` | Start, finish and checkpoints ([Markers](#markers)) |
| `AISPEED_<mph>` | Speed cap for the computer cars ([The AI path](#the-ai-path)) |
| `AIPATH_main`, `AIPATH_lane1` to `AIPATH_lane4` | The computer cars' path and lanes ([The AI path](#the-ai-path)) |
| `TRIGGER_<name>` | A box that sets off moving objects ([Trigger boxes](#trigger-boxes)) |
| `MOVE_<name>` | A moving object; its children move with it ([Moving objects](#moving-objects)) |
| `BILLBOARD_<name>` | Turns to face the camera ([Billboards](#billboards)) |
| `PROP_KNOCK_*`, `PROP_TIP_*` | Knocked flying or tipped over by cars ([Props](#props)) |
| `SKY` | Your own sky dome ([Your own sky](#your-own-sky)) |
| `START_1` to `START_4`, `PICKUP_<kind>` | Arenas only ([Stunt and battle arenas](#stunt-and-battle-arenas)) |
| `ROAD_SPLINES` | Spline road, for converter tools ([Spline road](#spline-road)) |

### Custom properties and attributes

Custom properties are set in Object Properties > Custom Properties; attributes in Object Data Properties >
Attributes (tick Data > Mesh > Attributes when you export).

| On | Property | Values | What |
|---|---|---|---|
| `COL_*` | `covered` | 0, 1, 2 | Shades the cars on it: 1 a tunnel, 2 lightly covered ([Collision surfaces](#collision-surfaces)) |
| `MOVE_<name>` | `billboard` | True | Its meshes face the camera as it moves ([Moving billboards](#moving-billboards)) |
| `PROP_*` | `billboard` | True | The prop faces the camera ([Props](#props)) |
| `AIPATH_main` | `keep_height` | True | The path keeps its height over every gap ([The AI path](#the-ai-path)) |
| `AIPATH_main`, `AIPATH_lane*` | attributes `_SPEED`, `_AI_MODE` | mph; 2 or 5 | The computer cars' speed and steering point by point ([The AI path](#the-ai-path)) |
| `COL_*` | attribute `_SEGMENT` | segment number | Spline road, for converter tools ([Spline road](#spline-road)) |
| `ROAD_SPLINES` | `segments` | lists of numbers | Spline road, for converter tools ([Spline road](#spline-road)) |

### track.toml settings

| Section | Settings | See |
|---|---|---|
| `[track]` | `name` (required), `author`, `version`, `description`, `type`, `gravity`, `road_hold`, `point_to_point`, `run_in`, `smooth_collision`, `smooth_fold` | [track.toml](#tracktoml), [Smooth collision](#smooth-collision) |
| `[race]` | `start_time`, `checkpoint_time`, `best_times` | [track.toml](#tracktoml) |
| `[look]` | `fog_color`, `sky`, `cloud_scroll`, `sky_parallax` | [Sky](#sky) |
| `[movers.<name>]` | `mode`, `races`, `start`, `open`, `close`, `open_delay`, `close_delay`, `open_at_start` | [Moving objects](#moving-objects) |
| `[surfaces.<name>]` | `like`, `drag` | [Custom surfaces](#custom-surfaces) |
| `[audio]` | `music` | [track.toml](#tracktoml) |
| `[menu]` | `logo`, `preview`, `diorama`, `after` | [Track select art](#track-select-art) |

## Size limits

| What | Limit | Why |
|---|---|---|
| Collision polygons | about 16,000 after flat neighbors are joined (65,535 corners: polygons don't share them, so 16,000 quads or 21,000 triangles) | The game's collision format. Stock tracks use 2,100 to 7,100. |
| Collision in one spot | 255 polygons within about a foot of any point (walls count within 6 ft where there is room) | The game's collision lookups hold 255. The report names the spot when a track passes it. |
| Track triangles | about 40,000 (rule of thumb) | The whole track is loaded into the game's memory, which it shares with the cars. |
| Track size | about 10 km (32,000 ft) from the origin in any direction | Positions are stored as 16-bit numbers. |
| Checkpoints | 9 | The game has 10 gates including the finish line. |
| Custom surfaces | 12 | The collision format's surface slots. |
| Custom tracks | 26 across all enabled mods | Track select slots. |

Vertex counts per object, the number of objects or materials, and name lengths don't matter: the converter splits,
merges and renames as needed.

## How custom tracks race

- **Options:** backward, mirror, laps, fog, drones and the rest all work as on stock tracks (a point-to-point track
  has no laps or backward).
- **Records:** each track keeps its own records, forward and backward, under its folder name.
- **Ghosts:** saved like the stock tracks'. A ghost holds at most 150 seconds a lap, so a longer lap cuts its ghost
  short.
- **Circuits:** custom tracks are circuit courses too. Settings > General > Circuit Courses has an All Modded Courses
  toggle and one per custom track, at the bottom of the list. New tracks are in by default.

## Not supported yet

- Traffic that drives the road and reacts to the cars (moving objects follow their animation; they can wait for
  [events](#making-it-react-to-the-race) but don't steer round cars)
- Setting the fog distance
- Surface grip, such as ice or sticky surfaces (Rush 2 only changes slowdown per surface)
- New sounds, dust or skid marks for custom surfaces

## For developers

- **Code:** `src/track_mod/`: `track_mod_convert.cpp` (the converter), `track_mod_menu.cpp` (logo and route preview),
  `track_mod.cpp` (finding the tracks, the disk cache, full-size textures). `include/track_mod.h` and
  `include/track_mod_runtime.h` hold the interfaces.
- **Hosting:** a custom track is raced in the HAWAII slot like a Rush 2049 track (`src/rush2049/track2049.cpp`,
  track cache key 1000 + a hash of the folder), with track select ids 38-63 (`src/rush2049/track2049_menu.cpp`, the
  tables moved to 0x80310000; the circuit review screen's diorama instances at 0x80311600). Circuits choose custom
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
- **Lanes:** the path files' four lanes run point for point beside the spine (same count, the spine's speeds, steering
  flag 2). `offset_lanes` puts lane slot k at `lane_offsets[k]` (-0.6, -0.2, 0.2, 0.6) of the ribbon's half width along
  the rung's lateral; `paint_lane` replaces a slot with an `AIPATH_lane<k>` curve: for each spine point, the crossing
  of the curve's center line with the flat line across the spine there, nearest the offset lane's place, within
  max(2.5 half widths, 60 ft) across and 30 ft in height, on a segment within 60 degrees of the spine's direction (so
  either drawing direction works); points with no such crossing keep the offset place. Both are dropped onto the road
  (`ground_under`, 40 ft) unless the spine point is in the air. The lateral's side depends on the ribbon's vertex order,
  so lane 1 (leftmost in the forward race) is slot 3 when the lateral leans left over the lap, else slot 0. The
  backward file's slot k at point i is the forward slot 3 - k at point n - i, exactly what offsetting the turned
  lateral gave before painted lanes existed. Each lane's gate crossings are searched on its own points.
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
  holds the 2049 MOVER records (0x20 bytes: group, polygon, rest matrix s16[9], vertex +0x16, rest vertex +0x18). It is written whenever there are moving objects, with no MOVER records when none has collision: the movers code takes an empty file for a track without moving objects and never starts (until 2026-10-17 a track whose moving objects were all scenery, such as the arcade Alcatraz's lights and planes, didn't animate).
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
- **Scripted moving objects and triggers:** a mover with `mode = "once"` or `"door"`, or a `start` on a loop or
  pingpong, is scripted. The converter appends three chunks to the mover file's directory (12-byte entries at +4:
  PTHD per path, then SCRP, TRIG, LAPP, terminator). SCRP has one record per path in PTHD order: u8 mode (0 not scripted,
  1 loop, 2 pingpong, 3 once, 4 door), u8 flags (bit 0 open_at_start), u8 counts of start / open / close events,
  3 pad, f32 open_delay, f32 close_delay (-1 = none), then the events, 12 bytes each: u8 kind (0 time, 1 trigger,
  2 odd_lap, 3 even_lap, 4 final_lap, 5 progress, 6 last_checkpoint), pad, s16 box (TRIG index), s16 lap (0 = any),
  pad, f32 seconds (progress: feet). LAPP (count = points, 0 when no event is a progress one): f32 lap length (0 for
  an open AI path: point to point or run-in, measured along it with no laps), then the spine's points (f32 x, y, z,
  game space, from the finish line every 20 ft), read by `movers::parse_lap`. TRIG holds
  24-byte boxes (f32 lo[3], hi[3], game space) from `TRIGGER_` nodes (mesh bounds, or an Empty's ±1 cube through its
  world matrix). A scripted path gets path flag pingpong 0x1 (loop 0x2 for a loop) and no ride flags.
  `movers::parse_scripts` reads them into `Path::script` and the boxes (`track2049_movers_logic.cpp`), for
  `light_track` 0 only. Each tick `track2049_movers.cpp` hands `World::set_race` whether the race is on (any active,
  non-ghost car released: car +0x71C), the leader's lap (max car +0x7FD laps done + 1), the race's laps (0x8010C0E2)
  and the boxes each car entered this tick with that car's lap (tested in the per-car hook against car +0x224, live
  cars only: active +0x7E4, not wrecked, not a ghost); then `World::set_cars` every active non-ghost car's position
  and whether player 1 is on the last lap with the finish's checkpoint (0x8010BCEC) as its next (car +0x800).
  Progress is the Rush 2049 arcade's (docs/arcade2049.md#dobsrom-dynamic-objects): distance along the lap to the
  nearest point plus a per-car lap counter x the lap length, the counter stepped when that distance jumps by more than
  half a lap; a progress event fires once when the leader's passes it, `last_checkpoint` as player 1's next
  checkpoint turns into the finish on the last lap. `World::step_scripted` runs such a path on a clock in seconds
  (every node's time is travel except the end node's; a loop counts every node, wrapping with a restore group op as
  2049's loop does), sets the follower's node / t / dir from it and poses it with the shared `pose()`; time events
  fire once each, from the green light; progress events once each. The script state is in the online sync. `movers_test.exe --script
  <mover_paths.bin> [laps] [lap seconds]` simulates a race and prints each scripted object's starts and stops;
  `track_mod_cases.py`'s `scripts` case checks the chunks, a moving billboard's record flag (0x8000, `camera_card`:
  `MOVE_` with `billboard`, vertices scaled by key 0 and turned half a turn for the camera basis) and `covered`
  (polygon info 0x4000 for 1, 0x8000 for 2: Rush 2's covered bits, which set car +0x344 and the tunnel shading).
  In the game, `RUSH2_MOVERS_LOG=1` prints the setup (paths, scripted ones, each trigger box) and, each tick, changes
  of the race flag and leader's lap and every box a car enters; `tools/test_race.py TRACK --records
  <converted>/placement.bin --env RUSH2_MOVERS_LOG=1` adds which records moved (`track_mod_test.exe <track dir> <out>`
  writes the placement.bin). A car that isn't driven runs out of checkpoint time about 30 s in: add `--drones 3
  --config cheats.cheat_no_game_timer=true` so the drones run the laps (2026-10-20: Marina A's STVRAMP door, set in a
  test copy to `trigger PROGRESS_14000`, opened 17 ft in game). A flip-book on `final_lap` swaps at the green light in a
  one-lap race, before race_watch's first sample, so it shows as unchanged there.
- **Billboards and props:** each `BILLBOARD_` and `PROP_` node becomes a model in its own frame (`<prefix>B%03u`, or
  `P%03u` for a prop that isn't a billboard), shared by nodes whose triangles are the same in the units the model
  stores (1/16 ft vertices, uv and color to 1/1024), and a top-level placement record after the moving objects, with no
  culling box. A billboard's record carries node flag 0x8000: `scene_walk` (0x8007B518, branch at 0x8007B860) then
  draws it with `view_billboard_basis_8007BF2C`'s yaw-only matrix at its pose's position only (no rotation or scale),
  showing the model's -z side, so the converter turns a billboard's mesh half a turn about y (Blender's front then
  faces the camera) and keeps its scale in the vertices. Props are listed as `PropRecord`s of type
  `rush2::track2049::prop_knock` (-2) or `prop_tip` (-3), handed to `set_prop_data` like a 2049 track's;
  `src/rush2049/track2049_props.cpp` resolves them at race setup to the rows of 2049's CONE1 and STOPHIT types
  (`movers::classify`), so the mod's own model reacts as those do. Custom tracks have no collectibles.
  `track_mod_cases.py`'s `objects` case checks the records, flags, shared models and prop types.
- **Sections:** visible triangles go into 1,024 ft grid cells by their center (s16 vertices at 1/16 ft reach 2,000 ft
  from the record). A triangle wider than a cell, or reaching past its cell's 2,000 ft, joins the first such section
  whose origin reaches all its corners, else starts one at its center. Before, each got a section of its own: the
  arcade Alcatraz's bay (water pieces out to 10,700 ft) made 258 sections, and the race crashed at its start
  (scene_node_create_8007F6DC appends nodes to the table at 0x800D9E90 with no bound; real globals follow it near
  0x800E7950, so it holds about 1,000 nodes for the track, cars and everything else). It now makes 110. The table
  itself has since moved (see Scene node table below).
- **Scene node table:** every section, billboard, prop and moving object's record gets a scene node, and so do the
  cars. The game's table at 0x800D9E90 holds 1,000 (0x38 bytes each, count `scene_node_table_count_800FAE58`, appended
  with no bound). The arcade Haight (Rush 2049 Arcade, Alcatraz Only off) made 1,045 with its 167 billboards, 80 props
  and 117 moving objects: the extra nodes ran over `text_state_stack_a_800E7C50`, whose handler the loading screen
  calls, so its race start jumped to 0x300 and quit every time (and the texture deriver's thread then crashed on the
  exit, which is what the dumps show). The arcade Metro makes 944 and Marina 831 (`race_watch.py --peek 800FAE58
  --peek-always`). The table now lives at 0x80400000-0x80420000, 2,340 nodes (`include/scene_nodes.h`): it is a
  `PLACED` entry of `tools/players4/relocate.py`, which points all 106 game accesses at it in us.toml (two stores in
  func_800BC888 through another pointer are excluded), and the game heap starts at 0x80420000 (`src/assets.cpp`;
  3 MB of it was still free in the arcade Metro's race). Interpolation keeps state for every node and its matrix IDs
  hold 12 bits of node index (`src/interpolation.cpp` node_id).
- **A big track and the heap:** the arcade Presidio (Rush 2049 Arcade, Alcatraz Only off) fills the heap to about
  0x80A5EAA0 (`test_race.py --peek 8010C438 --peek 8010C454`: heap low and high). Two things broke there (issue 27).
  `race_preload_car_assets_800A37F4` loads a car's model only if the free heap holds its asset + 0x200, else a smaller
  Rush 2 type's. Its checks read the size table at asset 0x1D + type, which for a Rush 2049 type is a track asset
  (type 24: 0x35, the HAWAII slot's geometry, the custom track itself), so every 2049 car became the Pickup; they are
  hooked to the type's own asset now (`rush2_car49_asset_size`, src/rush2049/car2049.cpp). And code that checked game
  pointers against the old 8 MB end (0x80800000) rejected data loaded past it: the pause menu's row images, so SKIP
  TRACK was blank. Those checks use `rush2::in_game_ram` (include/rush2.h, the full 16 MB) now.
- **See-through materials:** BLEND triangles get their own sections (the grid cell is keyed by translucency too),
  whose records have the late-pass flag 0x40000 (drawn after the opaque nodes, as Rush 2's own transparent ones). Every
  list draws its translucent materials last with render mode 0xC8104A50 (fog, then AA z-buffered translucent: Rush 2's
  own); the SKY list uses 0x00504240 (G_RM_XLU_SURF, both cycles). A stand-in (RGBA16) holds 1-bit alpha only, so
  `Texture::blend` makes the full-size image keep its alpha (`track_mod.cpp`; the stand-ins' cache records hold the
  flag from converter version 15).
- **Far moving objects:** a `MOVE_` object without collision whose mesh reaches past `vertex_reach` (2,000 ft) is
  stored at 16 x 2000 / reach vertex units per foot, and its path nodes' scale (and the key-0 placement matrix) are
  multiplied by 16 / unit: the movers port scales the drawn rows by the node's scale. Its model radius stays in feet.
- **Authored AI values:** `src/gltf_scene.cpp` keeps a primitive's SCALAR attributes whose names start with `_`
  (`Primitive::attrs`). `read_ribbon` takes a rung's `_SPEED` as the mean of its two vertices and `_AI_MODE` from the
  first; spine resampling interpolates the speed and takes the nearest mode. `target_speeds` uses authored speeds in
  place of the computed ones (no braking pass; `AISPEED_` caps kept); painted lanes store their crossing's values
  (`LaneAI`), and `write_path` writes each lane point's speed byte and steering byte (func_80071FBC's mode, 2 if
  none). `keep_height` (AIPATH_main's custom property) marks every gap run as air, without the step test.
- **Sky:** `SKY` meshes become one model named `SKYO1` (not in the placement), whose list switches to 1-cycle,
  TEXEL0 x SHADE, render modes 0x0F0A4000 / 0x0F0A3000 / 0x00504240 (opaque / alpha-tested / translucent, no fog) and
  clears G_ZBUFFER and G_FOG,
  then puts the track's state back, as Rush 2049's SKYSKY lists do; its vertices are scaled to fit the s16 range
  (32000 units). `track2049.cpp` sets `track_has_sky` from the converted model names, so the hooks on
  `sky_setup_800A45A8` (0x800A4600, 0x800A4644) build it through Las Vegas's sky branch, as for the 2049 tracks.
  Rush 2 moves a model sky with the camera in x and z only (render_sky_cloud_scroll_8008918C, 0x800891EC-0x80089200);
  with its vertices scaled down a big dome then ended below a tall track. The converter records the scale
  (`ConvertedTrack::sky_scale`, drawn / modeled size, cached) and `rush2_track49_sky_height` (hooked at 0x800891F8)
  sets the sky's height to (1 - sky_scale) x the camera's: shrunk about the camera rather than the origin, the sky
  looks as it would at full size. A stock sky model keeps its world height.
- **Flip-book frames:** a custom track's moving object whose scale is under 0.01 has its node hidden (flag 0x400,
  `write_pose` in `track2049_movers.cpp`) instead of drawn shrunk. Shown again at the same matrix, it interpolates with
  the camera like the scenery; a scale step needs a snap (src/interpolation.cpp snaps a node whose scale changes more
  than 4x in a frame), and snapped frames shook against the walls next to them. A scale segment shorter than 1/60 s
  with a hidden end is not blended (`track2049_movers_logic.cpp`, the scale lerp): it jumps at its middle. Blended, a
  game frame landing in the converter's 2 ms step drew the outgoing and incoming frames both shrunk (edges pulled in
  from the walls, see-through cracks, shaking); jumping at the segment's end, the frames' float timers drifted enough
  for a game frame to show two frames or none. A frame shown after being hidden has no matrix group in RT64's previous
  frame, so it was drawn unblended for a game frame while the camera (whose position is in every node's matrix) was
  blended, and it shook at every step: a book's frames share one pivot (`src/arcade2049/arcade_track.cpp`), and
  `track2049_movers.cpp` setup gives the nodes of frames on one pivot one interpolation ID
  (`rush2::interpolation_node_alias`, `src/interpolation.cpp`), so the frame shown pairs with the one it replaces,
  whose matrix is the same. `tools/rush2049/cpp_test/movers_test.cpp --custom <mover_paths.bin>` checks a converted
  track's flip-books (no part-shrunk frames, exactly one frame per pivot at every step).
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
- **Spline road:** `ROAD_SPLINES`' `segments` go into the collision file's SEG section as written (big-endian
  floats, header word 0 = their count); a face whose first vertex's `_SEGMENT` names one gets `CollisionTri::seg`,
  merges only with faces of the same segment, and its polygon gets flag 0x1000 and the segment index after its
  vertex run (the polygon's vertex-list offset is now kept per polygon). The game's spline surface is decoded in
  docs/rush2049_research/collision.md; `seg_surface` in tools/rush2049/collision.py ports it. Faces whose corners meet
  at the file's 1/32 ft are dropped as degenerate: merged, they gave polygons with a repeated corner and no local
  frame, which collapse to a point (holes).
- **Collision:** `build_collision` keeps each face's normal (no flipping), merges flat same-surface neighbors into
  convex polygons of up to 8 vertices, and builds the quadtree Rush 2's way; past 64 KB of leaves it builds the tagged
  form the 2049 port reads (splits toward zero, 17-bit leaf offsets, root byte +2 = 0x80 | size bits 16-20:
  `src/rush2049/track2049_collision.cpp`). Past 8,191 polygons (Rush 2's leaf entries hold a 13-bit polygon number
  and a 3-bit run) the leaves are written wide, one plain u16 polygon number per entry, and the root's byte +2 gets
  bit 5 (`rush2::collision::wide_leaf_tag`, either form); `rush2_collision_leaf_read_wide` reads them in place of
  `collision_leaf_read_8006CCEC` (docs/rush2049_research/collision.md#leaf-lists-v). The limit is then the header's
  u16 corner count (65,535; each polygon has its own corners), which past it leaves the last polygons out with an
  error (until 2026-10-19 the converter cut every track at 8,191 polygons). A leaf holds at most 255 polygons (`max_leaf` = `rush2::collision::max_leaf_polys`, the u8 leaf count's limit).
  The game's four leaf queries copy a leaf into an unchecked u16[36] (u16[40] in the wall query) stack buffer, and a
  longer one overwrote the callers' locals: with leaves of up to 255 the stock Alcatraz export had cells of 70, and a
  computer car's wall query (`car_wall_collision_query_8006FC94`) about 35 s into a race got its saved point pointer
  (sp+0x15C) overwritten by polygon numbers and crashed in `collision_poly_point_test_8006C670` (until 2026-10-15 the
  converter held leaves to 36 for it). Since 2026-10-20 hooks in us.toml (`rush2_collision_leaf_list`, before each
  query's copy and its read loop) point the buffer register at a 0x200-byte list per query at RDRAM 0x80E1EA00
  (`rush2::collision::leaf_lists`; 0 = `collision_query_8006CF00`, 1 = 0x8006F704, 2 = 0x8006FC94, 3 = 0x8008BDC0),
  swapping it only while it still holds sp + the buffer's offset, and the ghost leaf filters read the same lists
  (`rush2_collision_leaf_list_at`) (docs/rush2049_research/collision.md#leaf-list-buffers). Splitting cells toward 36
  as well took the arcade Metro and Presidio past the 2 MB leaf index, so cells split only past 255.
  A cell lists the polygons whose x/z outline (not box: a long diagonal strip's box covers far more than the strip,
  and stacked boxes on pipes and terraced slopes crowded single spots) comes within 1 ft of it, walls within
  6 ft (the wall query reaches 5 ft past the car) unless that crowds the cell past 255. A cell past `leaf_limit` (12) splits down to
  `min_cell` (32 ft); one past 255 splits on below it, down to 4 ft, and past 255 there with walls listed only 1 ft out
  down to 1 ft
  (until 2026-10-19 the 4 ft cell failed: a polygon passing near a crowded spot no longer counts). One that never fits
  fails with "N collision polygons meet near (x, y, z)", the spot in the author's Blender coordinates.
  Past 128 KB of leaves the tagged form's 17-bit child offsets run out (the arcade Presidio): the converter then
  writes a far-leaf table, u32 leaf offsets stored backward from the leaf section's end (entry k at end - 4(k + 1)),
  sets `rush2::collision::far_leaf_tag` (0x40) beside 0x80 in the root's byte +2, and a leaf child's 16-bit field is
  then k. `leaf_offset` in `src/rush2049/track2049_collision.cpp` reads it (California Speed's files use 0x40 without
  0x80, which keeps its own meaning). `tools/rush2049/collision.py` decodes it (`TAG_FAR`) and flags leaves past 255.
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
  markers, no visible mesh), painted lanes (`AIPATH_lane1..4`: some or all, drawn either way, one too far away to
  use, each lane point checked across both path files), a compact layout's
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

The list also holds San Francisco Rush's 7 tracks and California Speed's 16 (listed when `build/rush1.z64` /
`build/calspeed.z64` exist), as the virtual folders `stock:sf:<0-6>` and `stock:cs:<0-15>`: `tools/trackview/stock.py`
builds their collision and paths without the converter (SF Rush through `tools/rush1/track1.py`, with a Backward view;
California Speed's raw assets 94 + t / 110 + t with the game's surface remap, before its lane fixes). Their visible
layer is the track as placed, made by `tools/trackview/track_glb.cpp` (built by `track_glb.bat`: the model dump's
`write_glb` on the game's geometry and placement files, in feet where custom glbs are meters, so the page scales it)
and cached per ROM in `tmp/trackview/`. California Speed's shared objects (cones, lamps, trees) aren't in its track
file, so they're missing. Run AI races them by track select id.

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

**Export track** is `tools/trackview/export.py` (`/export?folder=`; also `python tools/trackview/export.py FOLDER
OUT.zip`). A custom track's folder is zipped as it is. A stock track is rebuilt by `tools/stock_track_glb.py`
`build_glb(..., visual=False, lanes=True)` from the game's collision and path files, and the model dump's placed glb
is merged in by `merge_glb` (its root nodes scaled from feet to meters, its buffers, accessors, textures and
animations re-indexed). The page used to export from the browser through three.js's GLTFLoader and GLTFExporter,
which isn't a round trip: GLTFLoader lower-cases custom attributes (`_SPEED` becomes `_speed`), and GLTFExporter
prefixes every non-standard attribute with `_` again, so the file came back with `__SPEED`, `__AI_MODE` and
`__SEGMENT`, losing authored AI speeds and the spline road. That export also dropped animations, `START_*` and
`AISPEED_*`, and wrote the AI path as lines the converter can't read. Checked by converting the exports back (export, bake, compare
path.bin with the ROM's): Rush 2's and Rush 2049's lanes land within 1.5 m (median) of the ROM's with the same speeds
and the gates exactly on the stock ones; the run-in courses get the stock header (1, 5, 0 on New York One) and flags,
their spine ending 30-57 ft before the loop start as the stock ones do (25-41 ft); a custom track's zip is
byte-identical to its folder. `stock_track_glb.path_layout` tells the three layouts apart from the path file: an
open spine whose start line (flags 2) isn't its finish line (flags 1) is a run-in when a later checkpoint is flagged 4
(the loop start), else point to point. `build_glb` sets each gate's arrow along the path's travel
at the gate's own spine crossing (`cross[0]`). San Francisco Rush's converted paths point their gates along the race,
where Rush 2's point back, and the old blind flip also had New York One's CHECKPOINT_5 backwards.

### Race replays

The previewer's **Run AI** races the track headless in the real game, about 50 times faster than real time (a 7-minute
Wario Stadium race takes about 15 s, 5 of them the boot), and replays it. `tools/trackview/runs.py` starts
`build/Rush2Recompiled.exe` as a simulation run (`src/sim_run.cpp`, `RUSH2_SIM=1`):

- **Its own data folder** (`RUSH2_DATA_DIR` = `tmp/trackview/sim_data/`, registered as the config path in main): the
  run starts from default settings and a fresh save, so nothing the player has set (cheats, options, opponents,
  unlocks) changes it, and it never writes to the player's data. Before each run its settings, saves and mod configs
  are deleted (its `track_cache` and other caches are kept), the ROMs are hard-linked in from `build/`, and its only
  mod, `trackview`, is generated with `tracks/<folder>` a directory junction to the track folder, so any track folder
  the viewer lists can be raced.
- **No window, no sound, no real input:** the window is created hidden, audio isn't queued, and no input reaches the game:
  the race starts straight from the title screen (`RUSH2_TEST_RACE=<folder>`,
  docs/menus.md#starting-a-race-without-the-menus; `RUSH2_TEST_CAR=0`, `RUSH2_TEST_BACKWARD=1` for the backward path).
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
