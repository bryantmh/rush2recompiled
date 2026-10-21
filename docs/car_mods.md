# Model mods

A mod can change cars and models four ways:

| What | Files | Section |
|---|---|---|
| Replace a Rush 2 car's body | `rush2/cars/<PREFIX>.glb` (+ `<PREFIX>_HOOD.glb`) | [Mod format](#mod-format) |
| Add a new car to the car select | `rush2/new_cars/<ID>.json` + glbs | [New cars](#new-cars) |
| Give an existing car more looks (STYLE row) | `rush2/liveries/<ID>.json` + glbs | [Liveries](#liveries) |
| Replace any model by name | `rush2/models/**/<MODEL>.glb` | [Replacing any model](#replacing-any-model) |

All cars made from a mod's glbs (replaced, new, liveries) dent and scuff with Rush 2049's one-body car code and can
carry a [hood camera hood](#the-hood-camera-hood), and get Rush 2's sheen layers made from the body (following
Graphics > Car Sheen as a Rush 2 car's do).

A mod can replace any Rush 2 car with one mesh of any detail. The game draws it in place of the car's panels, paints it
with the player's MAIN COLOR, and dents it like a Rush 2049 car's body when it is damaged. The mod's enable switch in
the Mods menu is the only toggle: turning the mod off brings the car back (applied the next time the game is in the
menus).

Code: `src/car_mods.cpp` (loading, conversion, stand-in textures), `rush2::car2049::set_body_car` /
`clear_body_car` (`src/rush2049/car2049.cpp`, denting and scuffing), `rush2::derive::set_mod_textures`
(`src/texture_derive.cpp`, full-size textures in the car's paint). Tool: `tools/car_mod.py`.

## Mod format

A mod (`.rtz` zip or folder in `mods/`, with the usual `mod.json`, `game_id` `rush2`) holds
`rush2/cars/<PREFIX>.glb` for each car it replaces. `<PREFIX>` is the car's part prefix (table 0x800CAF3C):

| Prefix | Car | Prefix | Car | Prefix | Car |
|---|---|---|---|---|---|
| PICKUP | Pickup | VWBUS | Van | TAXI | Taxi |
| INTEG | Compact | VIPER | Sportster | HOTROD | Hot Rod |
| VETTE | Muscle | VWBUG | Subcompact | FORM1 | Formula |
| SLED | Mobster | CONCPT | Concept | GT90 | Prototype |
| BMW | Sedan | CIVIC | Hatchback | ROCKET | Rocket |
| CAMARO | Bandit | CADDY | Cruiser | DEW | Mountain Dew |
| SUPRA | Coupe | MUST | Stallion | | |
| BUGAT | Exotic | SUV | 4x4 | | |

When several enabled mods hold the same car, the first in the mod order wins.

Optionally, `rush2/cars/<PREFIX>_HOOD.glb` in the same mod is the car's [hood camera hood](#the-hood-camera-hood).
Without it the hood camera shows no hood.

The glb:
- **Space:** the model dump's (docs/asset_catalog.md, Model dump): vertex units of 1/16 ft, y up, +z the front, x
  negated (glTF's right-handed space). A car modelled against a dumped Rush 2 car's `.glb` lines up. The wheels stay
  Rush 2's, at the stock car's positions, so the body should fit the stock car's wheel arches.
- **Meshes:** every node's meshes, with its transform (TRS or matrix). Positions, normals, UVs, indices. Any number of
  triangles.
- **Materials:** `pbrMetallicRoughness.baseColorTexture` is the image. A material's `extras.rush2_paint` (an image
  index) is its paint mask: red is the MAIN COLOR weight, green the ACCENT COLOR weight. A grayscale mask (every
  texel gray) is all MAIN (`decode_mask`). Without a mask, the texture is drawn as it is (not painted).
- **Lighting:** the converter bakes a fixed light from the normals into the vertex colors, as Rush 2's cars are unlit.

## Conversion

`rush2::car_mods::init` (at boot, from `rush2_heap_init`) and `check` (every 30 frames, in game states 0-1, when the
set of enabled mods changes) read the glbs and build a Rush 2 car container per car (`build_container`):

- `<PREFIX>FRAME1` is the mesh, with LODs at 80 and 600 like Rush 2's FRAME1s (two copies of the list, since the
  relocator rebases each LOD list on its own). The four sheen parts (D0_DFR1, D2_DFR1, D0_D2FR2, D2_D2FR2) are made
  from it by `rush2::track2049::add_body_sheen`, and the hood parts from `<PREFIX>_HOOD.glb`. Every other one of the
  37 parts func_80086700 looks up is an empty list, so Rush 2's damage panel swaps draw nothing.
- **Lists:** per texture, a `G_DL` to its load list, then batches of up to 32 vertices (`G_VTX`) with their `TRI2`s.
- **Texel centers:** the N64 puts texel i's center at coordinate i (Rush 2's panels run 0 to width - 1), glTF at
  i + 0.5, so s and t are uv x size - 0.5 texels. Without the half texel, every sample was half a stand-in texel
  (16 texels of a 1024-wide image) off, into the black between the image's islands: dark seams along every panel
  edge and the paint visibly shifted.
- **Stand-in textures:** each image the mesh uses gets a CI8 stand-in in Rush 2's car palette (CARPALETTE, asset 0x1C,
  classes 0x800C5670), sized by its aspect: 64x32, 32x64 or 32x32. Texels the mask paints go on MAIN's ramp
  (entries 1-31) or ACCENT's (33-63) by brightness, and the rest go to the nearest fixed entry, so Rush 2's own paint
  (func_8008582C) colors them. Texels 0 and 1 are always entries 1 and 33, so the drawn paint can be read back from
  them. A scuffed copy (the ramp shifted in blotches) is what a damaged panel draws. Flat single-color images have no
  separate scuffed key.
- **Texture names:** `<PREFIX>Mnn`. They must differ per car: every mod container has the same layout, and
  `bank_instance` tells the bodies apart by the first texture record's name. With shared names, a car is matched to
  another car's body, and the dent hook writes that car's vertices into it (the mesh comes out scrambled).
- **Cache:** the container is kept as a `track_cache` blob `carmod_<PREFIX>`, keyed on the glb's hash, the palette and
  `cache_version`, so later boots skip decoding the images.
- **Install:** `set_body_car` copies the container in as asset 0x1D + type and raises `car_slot_size` if needed. It
  points the stripe prefix at `R49X_` (no stripes are stamped) and collects FRAME1's vertices for denting
  (`rush2_car49_dent`, func_8005CFB0 hook, docs/rush2049_research/cars.md section 8). The car's own asset is saved
  first (`assets::replacement`) so `clear_body_car` can put it back.
- **No denting outside a race:** in game states 0-2 (menus, track and car select) the dent hook treats the car as
  undamaged. The car state there still holds the last race's damage bits (car + 0xE4), and before this the car
  select's preview was drawn crushed: the roof pressed down over the windows and the corners pulled in. Every panel
  was then off from its texture and the wheel wells off the wheels.

With texture replacements on, `rush2::derive` swaps each drawn stand-in for the mod's full-size image tinted with
the paint read from the stand-in (one RT64 hash per paint), mottled darker when damaged
(docs/texture_upscaling.md, Derived replacements). Without RT64 replacements, the stand-ins draw.

## Building a mod: tools/car_mod.py

```
python tools/car_mod.py build MAPPING.json OUT.rtz [--preview PREVIEW.png]
python tools/car_mod.py textures MESH.dae TEXTURE_DIR    # which texture each material's UVs fit best
python tools/car_mod.py preview MAPPING.json PREVIEW.png  # side and top views next to the Rush 2 cars
```

It needs RUSH2_ROM (default `rush2.us.recomp.z64`). The mapping's format is in the tool's docstring. Per car, it
takes the mesh files (Collada `.dae` or `.glb`) and a texture per material, node or file (`*` for the rest, `#rrggbb`
for a flat color):

- **Orientation and fit:** it tries the 24 axis rotations against the stock car (built from the ROM), keeps the best
  by occupancy overlap, and scales each axis to the stock car's size (`"fit": "uniform"` keeps proportions).
  `"flip"` overrides it.
- **Extra files:** an undertray or suspension file with its own rotation is realigned only when it sticks out of the
  body's box by more than 15%.
- **Winding:** meshes wound inside out (negative signed volume, as Unity exports are) are flipped.
- **Bleed:** each image's colors are spread 24 texels past the triangles that use it (`bleed`), and its paint mask
  with them, so filtering and mipmaps don't pull the black between an atlas's islands into the panel edges.
- **Paint mask:** `"paint": "auto"` makes it from the texture (light, unsaturated texels take MAIN COLOR), `"none"`
  leaves the car unpainted, or give an image (red MAIN, green ACCENT; a grayscale image is all MAIN).

## Test: the PJSF3 cars

The test mod `rush2_pjsf3_cars` (mapping kept in `tmp/carmod/mapping.json` during development) maps 17 of the 22
cars from a set of PJSF3 meshes and textures. The VWBUS, SUV, TAXI, ROCKET and DEW have no fitting mesh in the set.

Recipe:
1. `python tools/car_mod.py build MAPPING.json <dir>/mods/rush2_pjsf3_cars.rtz`.
2. Enable it in `mods.json`. Put opponents on modded types in `npc_cars.json` (`npcN_car`).
3. `RUSH2_EXE=<dir>/Rush2Recompiled.exe python tools/rush1/shots.py OUT "8:START,11:A,13:A,15:A,17:A,19:A" 16 22 26`.
   This shows the car select at 16 s and the race at 22 and 26 s.
4. The log shows `[carmod] <PREFIX> from <mod>: N triangles, M textures` per car.

To see denting without crashing, force the damage bits in `rush2_car49_dent` (bits at car + 0xE4) for a test build.

To check a converted container outside the game, read the blob from `track_cache/` (the container follows a u32 size
after track_cache's header) and walk it with `tools/rush1/cartex.py` `meshes(d, 2)`.

## Limits

- **Wheels:** the wheels and their placement stay Rush 2's.
- **Stripes:** no SF Rush or Rush 2 stripe decals are stamped. ACCENT COLOR is used only where a mask's green
  channel asks for it.
- **Stand-in resolution:** without texture replacements, a car draws its 64x32 stand-ins, a blurry version of the
  texture.
- **Hot swap:** a mod turned on or off is applied the next time the game is in the menus (game states 0-1), not
  mid-race.

## New cars

A mod can also add cars to the car select. Each one starts as a copy of a Rush 2 car you name (its physics, engine
sound, tires and the player's options), with your mesh, its own wheel positions, size and colors. It dents the same
way as a replaced car (see [Mod format](#mod-format)). Its colors work one of two ways:

- **Paint colors:** one glb whose paint masks mark the paint, as a replaced car's. MAIN COLOR (and ACCENT COLOR, if a
  mask uses it) repaint it with Rush 2's colors.
- **Color schemes:** several glbs, each a complete livery drawn exactly as authored (its own textures, and its own
  mesh if a scheme adds parts). The car select's STYLE row picks one, as it picks a Dreamcast Rush 2049 car's paint
  job.

### Files

Put these files per car in the mod under `rush2/new_cars/` (the car's ID is any letters):

- `<ID>.glb`: the car, in the same format and space as a replaced car's glb (see [Mod format](#mod-format)). You don't
  need to fit it to a Rush 2 car. Model it at its real size: Rush 2's cars are about 14 ft long. The wheels go where
  the `.json` puts them. With color schemes, one glb per scheme instead, named in the `.json`'s `schemes`.
- `<ID>_HOOD.glb` (any name, listed in `hoods`): optional, the [hood camera hood](#the-hood-camera-hood).
- `<ID>.json`. Every key but `name` is optional. A full example:

```json
{
    "name": "DEUCE",
    "base": "HOTROD",
    "wheels": { "front": [39.2, 3.2, 60.0], "rear": [39.2, 4.0, -63.4] },
    "wheel_scale": [1.08, 1.08],
    "box": [6.9, 6.9, 3.0, 3.0],
    "main_color": 26,
    "accent_color": 3,
    "schemes": ["DEUCE_COL1.glb", "DEUCE_COL2.glb"],
    "hoods": ["DEUCE_COL1_HOOD.glb", "DEUCE_COL2_HOOD.glb"],
    "physics": { "mass": 105.0, "steering": 800, "yaw_damping": 35, "torque": [1.85, 1.9, 2.0, 1.3] }
}
```

| Key | Meaning |
|---|---|
| `name` | Required. The car select's logo and the AI Opponents list show it. Up to 12 letters, digits, spaces or `-`. |
| `base` | The Rush 2 car it starts from, by its prefix (see the table in [Mod format](#mod-format)). The new car takes its physics, engine sound, tire curves, TORQUE, options and anything else not set here. Default `PICKUP`. |
| `wheels` | The front and rear right wheel centers, in the glb's space (1/16 ft units). The left wheels mirror them (the sign of x doesn't matter). They move both the drawn wheels and the physics' wheel points. Default: the base car's. Rush 2's own wheels sit at y 3.2 (front) and 4.0 (rear), x 44 and z ±62.7. |
| `wheel_scale` | Front and rear wheel model sizes, as Rush 2's TIRE SIZE (most of its cars use 1.0, the Pickup 1.25; stock 0.8 to 1.5). It changes only how big the wheels are drawn. A player's TIRE SIZE for the car starts here. Default: the base car's. |
| `box` | The box the car hits other cars with: front, rear, half width, height in feet. Default: from the mesh, about a quarter foot inside each end, at least 3 ft to a side, 3 ft high. Stock cars run 5.25 to 8 ft front and rear, half width 3 (the Caddy 3.25). |
| `main_color`, `accent_color` | The colors a new player starts the car in, 0-31 (Rush 2's color list). Default: the base car's. Unused with `schemes`. |
| `schemes` | Color schemes: a list of glb file names in `rush2/new_cars/` (up to 16), in STYLE order. Without it the car is `<ID>.glb` with paint colors. |
| `hoods` | The hood camera hood of each scheme: a list as long as `schemes` (one entry without schemes), each a glb file name in `rush2/new_cars/`, or `""` for none. Without it the hood camera shows no hood. |
| `physics_from` | A Rush 2049 car (`"BRUISER"`, `"8-BALL"`, ...) whose physics the car takes in place of the base car's (see [Physics](#physics)). |
| `physics` | The car's handling, speed and weight (see [Physics](#physics)). Each value you leave out is the base car's. |

The wheels' drawn position is their center, and the car rides on them like the base car.

### Physics

The new car drives as its `base` car unless told otherwise. Pick a `base` that is close to what you want, then change
only the values that should differ: the stock cars' values are balanced against each other, and moving several at once
(or copying one value set onto several cars) makes cars that drive alike or outclass the roster.

`physics_from` names a Rush 2049 car: the new car then drives as that car does in its default setup, its physics
exactly as the game builds them for it (its descriptor and its weight, inertia, steering, drive and settings rows),
with the new car's own wheels and box. Without Rush 2049 loaded, the base car's physics are used instead. `physics`
values apply on top of it.

`physics` is an object of the values below, in Rush 2's own units (the values Rush 2's tables and car descriptors
hold; `python tools/rush2049/cars.py r2` prints every stock car's). Give any of them; the rest stay the base car's.
A key the game doesn't know, or a value of the wrong shape, skips the car and the log says why. The Car Stats bars
(Accurate Car Stats) are measured from these values, so they show the effect of a change in the car select.

Weight and body:

| Key | What it does | Stock range |
|---|---|---|
| `mass` | The car's mass in slugs (lb / 32.2). Heavier is slower to speed up and harder to push around in collisions. | 96.3 (Subcompact) to 124.2 (Cruiser, 4x4); most 109.5 |
| `durability` | Where DURABILITY starts, as a weight value 0-1 (the option shows it x 100). Each step from the default adds or takes off mass: (value - default) x 31.06 slugs. | 0.2 to 1.0 |
| `pitch_roll_inertia` | The weight used when cars hit each other (func_8006E8D8): higher shoves other cars more and is shoved less. | 2000 to 4000 |
| `yaw_inertia` | The crash threshold: a total force on the car above it wrecks it (func_80071724). Higher survives harder hits. | 315000 to 470000 |
| `inertia_constants` | The 3 rotational inertias the car turns with (pitch, yaw, roll; their inverses are written with them). Higher turns and tips more slowly. | 1527, 1636, 1000 on every car |
| `preload` | Suspension preload factor (car +0x3C0..+0x3CC): how much the springs are loaded at rest, so how the car sits and how soon it bottoms out. | 0.13 to 0.25 |
| `suspension` | 14 spring and damper constants (descriptor +0x18), in front and rear pairs. Lower (more negative) is stiffer. | -3600, -3600, -4200, -4200, -1000, -1000, -880, -880, -1100, -1100, -480, -480, -550, -550 on every car |
| `suspension_constants` | 2 values (descriptor +0x50, +0x54). The first sets the steering lock: the front wheels turn 540 / value degrees at full stick (16 = 33.75 degrees). | 16, 4 on every car |

Steering and grip:

| Key | What it does | Stock range |
|---|---|---|
| `steering` | Steering force (term A of func_80069E74): how hard the car turns into a corner and catches a slide. TIRES' S grade adds -200, 0 or +200 to it. 0 means none, so the car turns on tire force alone. | 0 to 1000; mean 543 |
| `yaw_damping` | Yaw gain (term B): how much the steering swings the tail out, so how much the car drifts. TIRES' D grade adds -20, 0 or +20, SUSPENSION 10 per step. | 0 to 45; mean 30.7 |
| `rear_grip` | Rear tires' sideways grip factor (descriptor +0x78). Lower lets the tail slide out sooner. | 0.85 to 1.0 |
| `tire_curves` | `"base"` or `"2049"`: Rush 2049's front and rear tire curves, as its cars use (only with Rush 2049 loaded; otherwise the base car's). 2049's rear tires grip less (2.8 against Rush 2's 3.2). | `"base"` |
| `drive` | Drive flags (bits 0 and 1, car +0x59C / +0x59D). Leave it at the base car's. | 3 on every car |
| `suspension_setting` | Where SUSPENSION starts: 0 LOOSE, 1 NORMAL, 2 TIGHT, 3 WHEELIE. It also picks the suspension curve (func_8009A264). | 0 to 3 |
| `tires_setting` | Where TIRES starts: 0-8 S0 D0 to S2 D2 (steering grade S, drift grade D), 9 HALF OFFRD, 10 FULL OFFRD (more off-road grip, more road drag, lower top speed). | 0 to 10 |

Speed:

| Key | What it does | Stock range |
|---|---|---|
| `torque` | 4 torque scales: gear 1, gear 2, gears 3 and up, and the common engine map. Higher pulls harder in that gear. Small changes matter: the whole roster spans 1.8 to 2.05. | 1.8-1.9, 1.8-1.95, 1.8-2.05, 1.3 |
| `gears` | 6 gear ratios: reverse, neutral, 1st to 4th. Higher ratios accelerate harder; a lower top gear gives a higher top speed. | -3.1, 0, 3.05-3.1, 1.68-1.78, 1.17-1.29, 0.88-0.99 |
| `top_gear` | The highest gear the automatic uses, 1-4. | 4 |
| `final_drive` | Final drive ratio, multiplying every gear. | 3.3 on every car |
| `drivetrain_inertia` | How much of the engine's torque goes to spinning up the drivetrain: torque x mass / (mass + this). Higher is slower off the line. | 1.25 to 2.5 |
| `clutch` | Clutch strength: how much torque it passes before it slips. Higher bites harder off the line (the arcade's 1000 pulls away much harder than any stock car). | 600 on every car |
| `shift_up`, `shift_down` | Automatic shift points, engine speed in radians a second (662.35 = 6325 rpm). | 662.35, 429.35 on every car |
| `drag` | Air drag. Higher lowers the top speed. | 0.0135 on every car |
| `rolling_resistance` | Rolling resistance. Higher lowers the top speed and the pull at low speed. | 75 on every car |

The TORQUE option (the engine map: LOW to ROCKET) and the engine sound are the base car's.

Code: `read_new_car` (`src/car_mods.cpp`) maps the keys to descriptor offsets and per-type tables, and
`build_mod_physics` (`src/rush2049/car2049.cpp`) writes them over the base car's copy (or the `physics_from` car's),
every row of each table. `refresh_mod_physics` applies `physics_from` again whenever the Rush 2049 cars' physics are
rebuilt (a new Rush 2049 source).

### In the game

- **Slots:** there are 16 new-car slots (car types 57-72). The enabled mods' cars fill them in mod order, then by
  ID, and the car select lists them after California Speed's cars. A car past the 16th is left out, and the log
  says so. Reordering or turning off mods can move a car to another slot: a player's saved choice on that slot then
  becomes the car now in it.
- **Game Filter:** new cars count as CUSTOM.
- **Car select rows:** a new car has no STRIPE or STRIPE COLOR row (no stripes are stamped on it). It shows MAIN
  COLOR and ACCENT COLOR only if its paint masks use them, so a car without a mask keeps its textures' own colors
  and shows no color rows, like a Dreamcast car's single livery.
- **Car Sheen:** a new car gets Rush 2's sheen layers, built from its body as California Speed's cars get them, and
  follows the Graphics > Car Sheen setting like a Rush 2 car (Cube Map included). Its hood gets sheen layers too.
- **Damage:** a new car takes damage with Rush 2049's one-body car code (`set_body`): its body dents by panel, the
  hood folds, and each damaged panel's textures turn to scuffed copies (with texture replacements on, the full-size
  images scuffed the same way).
- **Hood camera:** the view shows the car's `hoods` entry for the picked STYLE (see
  [The hood camera hood](#the-hood-camera-hood)).
- **AI Opponents:** each opponent can be given a new car ("<name> (Mod)"). Computer cars never pick one on their own.
- **Online:** the host won't start a race while a player has a new car picked, because the other machines may not
  have that mod.
- **Car Stats bars:** a new car's bars are measured against the stock cars' range. Mods don't change the stock cars'
  bars.
- **Hot swap:** turning a mod on or off applies the next time the game is in the menus, like a replaced car.
- **Log:** `[newcar] <name> (<ID>) from <mod> as type N: ... based on <base>` for each car, or why one was skipped.

Code: `apply_new_cars` (`src/car_mods.cpp`) reads the files and converts the mesh as `build_container` does (parts
named `NC1`..`NC16`, cached as the `track_cache` blob `newcar_<slot>`). `rush2::car2049::set_mod_car`
(`src/rush2049/car2049.cpp`) serves it as asset 0xA0 + slot and copies the base car's per-type table entries, physics
descriptor (with the wheel points) and boxes (`build_mod_physics`). The type layout is in `include/car_types.h`.

Two traps the 73 types brought out, both crashes in the car select:

- Car preview ids (player x 73 + type) pass 127 for player 2's mod cars (130-145). The carousel entry keeps the id in
  byte +0x15, which `carselect_build_list_803B81F0` read signed at 0x803B93A0 (`lb`), so `func_8008813C` indexed the
  preview arrays at -126. us.toml makes it `lbu`.
- The part name table (0x800C0764's copy) is one of the per-type tables `build_mod_physics` copies from the base car,
  so `set_mod_car` writes the `NC<slot>` prefix after the copy. Left on the base car's name, the car select found none
  of the container's parts: `car_preview_make_nodes_80087A8C` gave every part the same scene node and linked it to
  itself, and `node_show_80058ED4` recursed until the guest stack ran out.

### Rush 2049 arcade cars

The Games tab (`src/arcade2049/arcade_cars.cpp`) builds the `rush2049_arcade_cars` mod from the arcade data ProjectR unpacks
(docs/arcade2049.md). It holds the three arcade cars that have no Rush 2049 (N64) counterpart as new cars: the Deuce
(CAR10), the Caddy (CAR11) and the Truck (CAR8). Every `COL` folder of a car is one color scheme (`<ID>_COLn.glb`,
picked with STYLE), each its body's full-detail LOD with the arcade's textures as authored, and its hood camera hood
(the scheme's HOOD object, `<ID>_COLn_HOOD.glb`, in `hoods`). The wheels are at the
arcade's own points, the same for every arcade car (docs/arcade2049.md#wheel-positions).

Each drives as the Rush 2 car of its kind, its `base` (Truck: PICKUP, Deuce: HOTROD, Caddy: CADDY), with no
`physics` of its own: the arcade gives every car one physics descriptor (docs/arcade2049.md#car-physics), so its
values made the Deuce and the Caddy drive alike, and with its stiffer clutch (1000) and Rush 2049's tire curves they
pulled away harder than any stock car. Every car's `box` is the arcade's own.

The other arcade cars are the N64 cars' models; their arcade-only color schemes are in the mod as
[liveries](#liveries) of the N64 cars (`LIVERIES` in the tool): GX-2 gets CAR2 COL6, Magnum CAR4 COL4, 6 and 7,
Super GT CAR5 COL5 and 7, 8-Ball CAR6 COL1 and 3, and Bruiser the Cuda's (CAR7) COL1, 2, 3, 5, 6, 7 and 8. A livery
body is moved forward by the N64 car's wheel center offset (Bruiser 0.5 ft, the others 0), which a side silhouette
fit against the N64 body confirmed. Each livery has its scheme's HOOD as its hood.

## The hood camera hood

In the hood camera view (the L button cycles the views), Rush 2 and Rush 2049 cars stop drawing their body and draw a
separate hood model, the parts `H0_L` (and `H2_L`, which Rush 2 swaps in by the front panel's damage). A Rush 2049
car's is its CARnHOOD, the arcade's the HOOD object of each color scheme. A modded car's hood is a glb of its own in
the same space as the body (the hood and whatever of the car the camera sees, usually the hood panel and the base of
the windshield), drawn as authored:

- replaced car: `rush2/cars/<PREFIX>_HOOD.glb`;
- new car: the `.json`'s `hoods`, one per scheme;
- livery: the `.json`'s `hoods`, one per style.

Without one, the hood camera shows the road with no hood. The hood gets Rush 2's hood sheen layers (`H0D_L`, `H2D_L`,
made from it by `add_body_sheen`) and keeps the paint scheme markers its lists carry.

Code: `convert` / `build_container` (`src/car_mods.cpp`) emit the hood parts as lists `H0_L` and `H2_L` of their own
(the loader rebases a list once per part that names it, so two parts never share one); `append_liveries` adds a
livery's as `~Lnn` + `H0_L` / `H2_L`, which `paint_livery` branches the car's own hood lists to.

## Liveries

A mod's `rush2/liveries/<ID>.json` gives an existing car more looks, each a whole mesh:

```json
{
    "car": "BRUISER",
    "styles": ["BRUISER_COL1.glb", "BRUISER_COL2.glb"],
    "hoods": ["BRUISER_COL1_HOOD.glb", ""]
}
```

- `car`: required. A Rush 2 car by its part prefix (`CAMARO`, `VETTE`, ...) or a Rush 2049 car by its name
  (`BRUISER`, `8-BALL`, ...; spaces and `-` don't matter).
- `styles`: required. Glb files in `rush2/liveries/`, up to 15 per car across all mods, in STYLE order. Each is drawn
  as authored (its own textures at full size, no paint colors), in the car's space like a [model mod](#mod-format), on
  the car's own wheels. Model it over a dump of the car (Dump Models) so it sits on the wheels.
- `hoods`: optional. The [hood camera hood](#the-hood-camera-hood) of each style, by position in `styles`: a glb file
  in `rush2/liveries/`, or `""` (or a shorter list) for none. A style without one shows no hood in the hood camera.

A file that can't be read is skipped and the log says why (`[livery] <file> in <mod>: ...`).

The car select gets a STYLE row under CAR: STYLE 1 is the car as it is, STYLE 2 and up the liveries. While a livery is
picked, the MAIN COLOR, ACCENT COLOR, STRIPE and STRIPE COLOR rows are hidden. The choice is per player profile and
car, kept in the saves' `"liveries"` section (`{"<profile record address>": {"<car type>": style}}`, STYLE 1 not
stored), apart from the car's paint and stripe bytes, so a car that has stripes keeps them for STYLE 1. Computer cars
always use STYLE 1. Turning the mod on or off applies the next time the game is in the menus. A livery body is a
one-body car's, as a [new car](#new-cars)'s: it gets Rush 2's sheen layers made from it (following Graphics > Car
Sheen as a new car's do, Cube Map included) and takes the car's panel damage as a 2049 car's body does: it dents,
folds its hood, and each damaged panel's textures turn to scuffed copies (with RT64's replacements, the livery's
images scuffed the same way, `paint_mod` in src/texture_derive.cpp).

How it works: the asset patcher (`append_liveries`, `src/car_mods.cpp`) appends livery n to the car's asset as a body of
its own: `~Lnn`FRAME1 with the car's FRAME1 LODs (each LOD its own list, unlit or lit by the LOD's flag 0x10: the
loader rebases a list once per LOD that names it, so a shared one breaks), whose lists load their own textures (16x16
stand-ins that `rush2::derive` replaces with the full images), and `~Lnn` + the four sheen parts (D0_DFR1, D2_DFR1,
D0_D2FR2, D2_D2FR2), filled by `rush2::track2049::add_body_sheen` as a new car's, and with a hood `~Lnn` + `H0_L` and
`H2_L` (each its own list, loading its textures inline) and their sheen parts `H0D_L` / `H2D_L`. `rush2::car2049::add_livery_body`
runs set_body's steps on it: `collect_body_vertices` (with the sheen layers' copies and the hood fold), then
`add_damage_textures`, which inlines its texture loads (lists the livery's lists call, appended beside its texels) per
panel with the scuffed copies (`TrueColor::scuffed`, made by `scuff_true_color`). `rush2_car49_dent` damages the
livery a copy shows (`rush2::car_mods::livery_body`) with the same code as a one-body car (`set_damage_textures`,
`dent_body`); `build_sheen_tables` treats its layers as a one-body car's. The car slot size grows to fit.
`paint_livery` runs on each loaded copy of the car, from the end of `rush2_car49_paint` (func_8008582C) and when STYLE
changes: for a livery, every body part the livery has its own of (FRAME1 by LOD, the four sheen parts, and with a
hood `H0_L`, `H2_L`, `H0D_L`, `H2D_L`) gets a `G_DL` branch to the livery's same-named list over its first command,
and the car's other body parts (panels, damage parts, a hood the livery lacks) a `G_ENDDL`; for STYLE 1 it puts back the words it saved. The mirror walk (func_800776DC, which turns an asset's triangles
over and follows branches) would turn a livery over twice and the car's own parts not at all, so
`rush2_livery_mirror_begin` / `_end` put the loaded words back around it. The copy is found as `rush2_car49_paint` finds it: the id's texture bank (s16 at `car_banks` + id x 2)
gives the copy's texture records (0x80119220 + bank x 8), and the copy starts that far before them that its header
word +8 (the texture table offset) says. (The car slot word 0x800D97E8 + id x 0x18 +0x14 is the car's palette, not
its copy.) In the car select (game state 2) the id is the preview id, panel x 73 +
type: two players choose in panels 0 and 1, a single player's car shows in panel 1 (race player count 0x8010C3E2 is
1), and the profile is the choosing player's record +0x24. In a race the id is the car 0-7, a human's type at physics
car 0x800F5470 + id x 0x81C +0x7EA (+0x7E8 == 2) and its profile from the player record whose +0 is the car. The
STYLE row is option 17 in `src/rush2049/wings_menu.cpp` (`option_livery`); a step repaints both panels' previews of
the car.

## Replacing any model

A mod's `rush2/models/**/<MODEL>.glb` replaces every model named `<MODEL>` wherever the game loads it: Rush 2's own
assets, the converted SF Rush and Rush 2049 tracks and cars, and car containers (a car mod's included). `<MODEL>` is
the file name Dump Models gives that model (`rush2::catalog::file_name`; case does not matter). The folders are free;
mirroring the dump's (`rush2/tracks/ALCATRAZ/ALCATRAZL4156.glb`) keeps a mod readable. When several enabled mods hold
the same name, the first in the mod order wins.

The glb is read like a car's (same parser, same space: the dump's), with these differences:
- **Start from the dump.** A dumped `.glb` loads back unchanged. Its other LODs are child nodes scaled to 0, and nodes
  with a zero-determinant transform are skipped, so an edited dump brings only its visible LOD. Every LOD of the
  model draws the new mesh.
- **Vertex colors:** `COLOR_0` is the vertex shade (the dump stores the model's baked lighting there). A lit LOD (flag
  0x10) gets the normals instead, as Rush 2 lights it. Without `COLOR_0`, a fixed light is baked from the normals.
- **Textures:** each image becomes a true-color RGBA16 stand-in (64x32, 32x64 or 32x32; alpha below 128 is clear),
  keyed by its decoded content (`rgba16_key`). `rush2::derive` draws the full-size image over it when texture
  replacements are on.
- **No paint:** a car part replaced this way keeps its texture's colors. Use `rush2/cars` for a painted body.
- **Cache:** the stand-ins are kept as the `track_cache` blob `modelmod_<hash>` of the glb.

Code (`src/car_mods.cpp`, model mods section): `apply_models` gathers the files (`model_files`) and loads them
(`load_model_mod`), then hands `patch_container` to `rush2::assets::set_patcher` (`src/assets.cpp`).

The patcher runs on every container when it is replaced or restored, and on Rush 2's own assets when it is set. So
the game always loads the patched bytes through the asset table's fake ROM offsets. The dumps, the catalog and
`assets::replacement` keep seeing the unpatched bytes.

For each matched model, `patch_container` appends the stand-ins' texels and one list per LOD (`emit_model`; the
relocator rebases each LOD list on its own), then points the model record's LODs at them:
- **Render state:**
  - The list sets no TLUT (`E3001001 0`, true color) and no mipmaps (`E3000F00`).
  - It copies the model's own first `G_SETOTHERMODE_L` (`E2`: blending, z, fog).
  - It copies the model's first combiner that samples TEXEL0 (`list_state`, following `G_DL` up to 4 deep). If there
    is none, it uses TEXEL0 x SHADE with cycle 1 passing it on (`FC1219FF FFFFFE38`). The car combiner
    (`FC1216AC F0FFFE38`) is not a safe default: its cycle 1 multiplies by the environment color, which drew other
    models dark gray.
  - It turns `G_TEXTURE` on (`D7000002 FFFFFFFF`), since a model's list may start on an untextured part.
    ALCATRAZTRACK starts with texture off and a shade-only combiner.
  - It ends by restoring the RGBA16 TLUT that Rush 2's lists expect (`E3001001 00008000`).
- **Radius:** the name record's radius (vertex units / 16, used for culling) grows to cover the new mesh.
- **Car slots:** a car asset that grows past `car_slot_size` (0x8001CF28) raises it.

The log shows `[modelmod] <path> (asset 0xNN) from <mod>: N triangles` for each container patched.

Test: copy dumped glbs into a test mod (`tmp/carrun/mods/test_models`), edit them (tint the images, scale y), and
take shots of:
- the track select: `"8:START,11:A,13:A"`, 16 s;
- a race: `"8:START,11:A,13:A,15:A,17:A,19:A"`, 22 s;
- the car select on the Taxi: `"8:START,11:A,13:A,15:A,17:DL"`, 20 s.

Verified 2026-10-11 on the track select island, all 101 Alcatraz sections and every Taxi part.

Limits:
- **Collision:** only what is drawn changes. Track collision is a separate asset (0x4B + slot), so a reshaped section
  still drives like the original.
- **Hot swap:** a change applies the next time the game loads that container. The menus reload theirs, and a track
  loads at race start. Mods are checked every 30 frames in game states 0-1.
- **Stand-in resolution:** without texture replacements, the stand-ins draw (64x32 at most).
- **Texture swaps:** animated textures and texture swaps act on the original lists' texture loads, so they don't reach a
  replaced model. These are LOD flag 0x1 and the CHKPOINT -> FINISH banner.
