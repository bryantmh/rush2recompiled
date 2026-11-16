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
- **Materials:** `pbrMetallicRoughness.baseColorTexture` is the image. A material's `extras.rush2_paint` (a texture
  index, as `baseColorTexture`'s) is its paint mask: red is the MAIN COLOR weight, green the ACCENT COLOR weight. A grayscale mask (every
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
  them. A scuffed copy (the ramp shifted in blotches) is what a damaged panel draws. An image with no paint mask
  (drawn as authored, like the arcade cars' schemes) has no ramp, so its colors are scaled lighter and darker in the
  same blotches and each texel takes the nearest fixed entry; before this, such cars dented but never scuffed. Flat
  single-color images have no separate scuffed key.
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

### Image alignment

How a mod's texture repeats comes from its glTF sampler: each image takes the `wrapS` / `wrapT` of the first texture
using it (`Model::image_cm`), and the stand-in's render tile gets clamp (33071, clamp-to-edge) or mirror (33648)
in its cms / cmt (`tile_cm`; both the new cars' `build_container` and the model mods' and liveries'
`true_color_load`). Repeat (10497, glTF's default) wraps as before.

A vertex's coordinate is `uv * size - 0.5` stand-in texels (the N64 samples texel i at i, glTF at i + 0.5), and
`rush2::derive` hands RT64 the full-size image as a live replacement with the shift "center"
(`addLiveReplacement(..., centered)`, lib/patches/rt64.patch; docs/rush2049_research/dreamcast.md#replacement-texel-centers):
each stand-in texel lands on the middle of its block of the image, where glTF samples it. With RT64's usual "half"
shift the image was sampled half an image texel off.

Both mattered on the Bruiser's arcade liveries (CAR7): the body's mirrored halves meet on textures whose UVs run to
the edge (`Y_ROOF` u 63.88 of 64, `Y_WINDSHID` 0, `Y_TAIL2` 127.25-127.75 of 128), and filtering there pulled in the
texture's far edge: a hairline down the middle of the car. The arcade clamps most of them (textureMode bits 6 / 7,
which `arcade_cars.cpp` now writes as each texture's sampler); `Y_TAIL2` wraps on the arcade too, where its UVs sit
on its last texel's middle, which only the centered image reproduces.

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
- `<ID>_BRAKE.glb` (any name, listed in `brakes`): optional, the [brake lights](#brake-lights).
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
| `no_wheels` | `true` hides the car's four wheels, in races and on the car select (it still drives on them). Default `false`. |
| `no_wings` | `true` hides the car's wings: they still open in the air and steer it there, unseen. Default `false`. |
| `no_skid_marks` | `true`: the car's wheels lay no tire marks. Default `false`. |
| `no_wheel_dust` | `true`: the car's wheels throw no dust, smoke or spray (its own effects, such as sparks, stay). Default `false`. |
| `stripes_above_widest` | `true`: [STRIPE](#stripes-on-one-body-cars) paints nothing below the body's widest point, nor anything facing down from the middle of the body (for a car whose origin isn't inside its body, as the PS1 UFO's is at the ground under its saucer). Default `false`. |
| `round_shadow` | A radius in feet: the car's shadow is a soft disc that wide around its middle (a hovering saucer's), on the ground under its wheels, in place of the box between them. Default: the game's shadow. |
| `lights` | The car's lamps for [Car Lights](#car-lights): a list of `{"kind": "head" or "tail", "at": [x, y, z], "size": radius}` in the glb's space (1/16 ft units, as `wheels`), one entry per lamp (list both sides). The brightest texels within the radius glow as the lens, or with `"color": [r, g, b]` (the lens's color, 0-255) the texels near that color; `"lens": false` for a hidden lamp (no glow on the body). The headlamps or tail lamps listed replace the ones found on the body, kind by kind. Default: found on the body (tail lamps from the brake lights, headlamps from bright texels at the front, or else placed from the body's shape). |
| `logo` | A 128 x 32 PNG in `rush2/new_cars/` drawn as the car's car select name logo instead of `name` in Rush 2049's lettering (the SF Rush PlayStation cars use it for SF Rush's lettering). Default: none. |
| `box` | The box the car hits other cars with: front, rear, half width, height in feet. Default: from the mesh, about a quarter foot inside each end, at least 3 ft to a side, 3 ft high. Stock cars run 5.25 to 8 ft front and rear, half width 3 (the Caddy 3.25). |
| `main_color`, `accent_color` | The colors a new player starts the car in, 0-31 (Rush 2's color list). Default: the base car's. Unused with `schemes`. |
| `schemes` | Color schemes: a list of glb file names in `rush2/new_cars/` (up to 16), in STYLE order. Without it the car is `<ID>.glb` with paint colors. |
| `hoods` | The hood camera hood of each scheme: a list as long as `schemes` (one entry without schemes), each a glb file name in `rush2/new_cars/`, or `""` for none. Without it the hood camera shows no hood. |
| `brakes` | The [brake lights](#brake-lights) of each scheme: a list as long as `schemes` (one entry without schemes), each a glb file name in `rush2/new_cars/`, or `""` for none. |
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

- **Slots:** there are 64 new-car slots (car types 57-120). The enabled mods' cars fill them in mod order, then by
  ID, and the car select lists them after California Speed's cars. A car past the 64th is left out, and the log
  says so (`[newcar] ... every mod car slot is taken`). Types stay below 128 because the game reads a car type as a
  byte, often signed; the per-type RDRAM each extra type costs (about 2 KB) is laid out in `include/car_types.h`
  (docs/rush2049_research/cars.md#8-implementation-v). Reordering or turning off mods can move a car to another slot: a player's saved choice on that slot then
  becomes the car now in it.
- **Game Filter:** new cars count as CUSTOM.
- **Car select rows:** a new car shows MAIN COLOR and ACCENT COLOR only if its paint masks use them, so a car
  without a mask keeps its textures' own colors and shows no color rows, like a Dreamcast car's single livery. A car
  whose mask uses MAIN (and has one look, no color schemes) also gets STRIPE and STRIPE COLOR: see
  [Stripes on one-body cars](#stripes-on-one-body-cars).
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
(the scheme's HOOD object, `<ID>_COLn_HOOD.glb`, in `hoods`) and its brake lights (the BRAKE object, the lit lights the
arcade draws over the body while the car brakes, `<ID>_COLn_BRAKE.glb`, in `brakes`). The wheels are at the
arcade's own points, the same for every arcade car (docs/arcade2049.md#wheel-positions).

Each drives as the Rush 2 car of its kind, its `base` (Truck: PICKUP, Deuce: HOTROD, Caddy: CADDY), with no
`physics` of its own: the arcade gives every car one physics descriptor (docs/arcade2049.md#car-physics), so its
values made the Deuce and the Caddy drive alike, and with its stiffer clutch (1000) and Rush 2049's tire curves they
pulled away harder than any stock car. Every car's `box` is the arcade's own.

The other arcade cars are the N64 cars' models; their arcade-only color schemes are in the mod as
[liveries](#liveries) of the N64 cars (`LIVERIES` in the tool): GX-2 gets CAR2 COL6, Magnum CAR4 COL4, 6 and 7,
Super GT CAR5 COL5 and 7, 8-Ball CAR6 COL1 and 3, and Bruiser the Cuda's (CAR7) COL1, 2, 3, 5, 6, 7 and 8. A livery
body is moved forward by the N64 car's wheel center offset (Bruiser 0.5 ft, the others 0), which a side silhouette
fit against the N64 body confirmed. Each livery has its scheme's HOOD as its hood and its BRAKE as its brake lights,
and a scheme whose `dobs.rom` has a flipbook (the police light bars and the Bruiser's beacon) its frames as
[sirens](#sirens) (`<LIVERY>_COLn_SIRENk.glb`).
The installed mod is keyed on the build, so a new build rebuilds it in the background at launch (an older install
gets its brake lights that way).

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

## Brake lights

A modded car's brake lights are a glb of their own in the same space as the body: the lit lights, usually a few quads
just over the body's own tail lights, drawn as authored only while the car brakes (Rush 2's rule: braking, the car's
flags 0x1000 set and 0x10 clear, func_800599A0). New cars list one per scheme in the `.json`'s `brakes`, liveries one
per style. The arcade cars' BRAKE objects are theirs. Issue #44.

How it works: each scheme's (style's) brake lights are a list of their own, a model named `<prefix>BRK<scheme>` (a
livery's `~LnnBRK`) so the loader rebases it, which the game never draws by name. The body's lists end the scheme's
block (a livery's body lists end) with `rush2::car2049::brake_switch`: a G_NOOP marker (`brake_marker(1)`), then a
G_NOOP holding the brake list's file offset in its low 24 bits (`brake_marker(2)`). `rush2_car49_dent`, every frame per
race car, turns each switch of the car's copy (and of the livery it shows) into a G_DL call to the brake list while
the car brakes, and back into the marker otherwise (`set_brake_switch`, src/rush2049/car2049.cpp; the switches are
found by `find_scheme_blocks` and, for a livery, `find_brake_switches`). Everything starts off, so a copy just loaded
and the car select's previews are unlit. The mirror walk (func_800776DC) turns the brake list over through its own
model and follows calls, so `rush2::car2049::brake_mirror` (from `rush2_livery_mirror_begin` / `_end`) turns the lit
switches off for it and on again after. A livery's brake list loads its textures inline, as its hood does: the
relocator doesn't enter a called load list. `rush2_calspeed_brake_lights` (the func_800599A0 hook) keeps each car's
braking for a car whose `Paint` or shown livery has lights (`Paint::lights`) and still lets Rush 2 write its palette.

The brake lists dent with the body (issue #52): they are models of their own, which the dent's FRAME1 walk never
reaches, so damaged cars showed their lit lights where the undamaged body was (behind or beside a crushed rear, a
third light floating over a crushed cab). `collect_brake_vertices` (after the brake switches are found, in `set_body`
and `add_livery_body`) gives every vertex of the car's brake lists the body vertex nearest it, and `dent_body` moves
each by its body vertex's dent (`Paint::followers`). Checked with the arcade Truck (type 59) and the Super GT's
arcade livery, `RUSH2_TEST_DAMAGE=1`, braking.

A brake list draws blended, unlit and full bright (`brake_render_mode`, src/car_mods.cpp): the arcade's BRAKE textures
are soft glows, alpha 0 and near black around a red core, which the body's opaque render mode drew as dark boxes. The
list clears G_LIGHTING, sets the body's fog cycle with G_RM_ZB_XLU_SURF2 after it and G_CC_MODULATEIDECALA (the
texture's alpha, from its full-size replacement; a fogged list's shade alpha is the fog), draws white vertices, and
ends by setting the body's mode and combiner back; a lit livery LOD sets G_LIGHTING again after its switch. Every
brake light vertex is moved 1.5 model units (0.09 ft) off the body along its normal, toward the back (`brake_lifted`):
vertices are whole 1/16 ft units, so lamps authored a fraction of a unit over the body (the Deuce's 0.3) were rounded
onto or under it, and the half of a lamp quad not flat against a curved panel was buried (issue 80: each Deuce lamp
lost a corner). A decal z mode doesn't fix it: RT64 passes a decal only at the depth already there.

Since brake lights don't write depth, anything drawn over the body after them covers them. A car with stripes
(`add_damage_textures` in src/rush2049/car2049.cpp) gets its stripe blocks at the end of its lists, so its brake light
switch moves after them when the lists are rewritten (issue 80: a striped PS1 Jeep's right lamp stayed dark, the
stripe layer repainting the body over it). A list with color scheme blocks keeps its switches where they are (each
scheme shows only its own; such a car has no stripes).

The PlayStation SF Rush cars' brake lights (src/sfrush_ps1/ps1_cars.cpp `brake_lights`, issue 80) are made this way
too: the PS1 shows braking with a glow sprite (docs/sfrush_ps1.md#snow-and-night), so the Games tab's build gives
each car a `<ID>_BRAKE.glb` of its body faces in the back half facing backward that show a lamp texel, those texels
at Rush 2's lit red and the rest clear. The lamps are a hand table of texture areas (`lamp_areas`, read off each car's
art), because the lens red (173, 41, 16) and amber (181, 90, 16) are also the Police's roof flasher, the Mini's
stickers and lettering and the Bus's paint, and a color rule lit those, specks at the Mini's lamp corners and only
the rims of the Jet Car's lamps. An area lights its red alone (Police, Mini, Bus, Starsky: the amber beside it is a
turn signal and stays) or the whole lamp (the Jeep's striped and the Jet Car's rimmed lenses). The UFO has none.
`tools/glb_view.py CAR.glb OUT.png --yaw 180 --overlay CAR_BRAKE.glb` shows what lights, from the converter's
offline build (`tools/sfrush_ps1/cpp_test/build.bat`).

Test aid: `RUSH2_TEST_LIVERY=<style>` shows that STYLE on every car with liveries (JUST PLAY has no profile to keep one
in): `tools/test_race.py 0 --cars 28 --env RUSH2_TEST_LIVERY=1 --script "14:A:5,19.2:B:3" --shots 18.8,19.6` brakes
the Bruiser in the arcade's first Cuda livery.

## Stripes on one-body cars

Rush 2 stamps its stripes onto its own cars' six panel textures (`car_stamp_stripes_on_panel_80083F50`, from
`car_build_textures_8008582C`'s 24-pass loop). Cars drawn as one body have none of those panels, so their textures'
name prefix is `R49X_` and nothing was stamped: the Rush 2049 N64 cars hid the STRIPE row, and new cars hid STRIPE
and STRIPE COLOR. Both now take Rush 2's seven stripes (`src/car_stripes.cpp`, `include/car_stripes.h`, the blocks in
`add_damage_textures` and `stripe_paint`, src/rush2049/car2049.cpp):

- **The shapes are Rush 2's.** A stripe is a set of bands along the car's length, measured out from its
  centerline. Every stripe tile in asset 0x1C (`cars/stripes`, one per STRIPE value and panel shape: `1ST_`, `1F_`,
  `1FD_`, `2ST_`, `2F_`, `3ST_`, `3A_`, the prefix table at 0x800C6518 indexed by the STRIPE value, entry 0 `DEW_`)
  reaches across half a panel from its centerline edge, and every tile size has the same bands at the same
  fractions (SINGLE is 10/64 of the 64-wide `_L2` tile and 5/32 of the 32-wide `_L1`). So each tile is read as bands
  over the car's half width (the widest |x| of the body): SINGLE 0-10/64, SINGLE FAT 0-20/64, SNGL FADE 0-4/64 solid
  then steps of 2/64 at alpha C, B, 9, 7, 5, 2 out to 16/64, DOUBLE 6-16/64, DOUBLE FAT 8-28/64, TRIPLE A 0-10/64 and
  14-20/64, TRIPLE B 0-2/64 and 6-18/64 (`profiles`, from the `_L2` tiles). Rush 2 also places each tile per car and
  panel (a byte table at 0x800C6230 and floats at 0x800C62B4, 6 per car type, which
  `car_texture_prefix_lookup_800843EC` hands the stamp; not decoded): the one-body cars use the half width as it is.
- **The edges are the geometry's.** Painting the bands into the textures (the first try) can't work on these cars:
  their textures are small (64x32, the PS1 cars' whole body in 256x64), so the edges stair-stepped along the texels,
  and one texture is used at several places on a car (the same texels on the hood and a door), so a band painted
  for one place showed at the others and texels used at places far apart could take none (most PS1 cars, the Mini
  and the UFO among them, showed almost nothing). Instead the band edges are cut by the alpha test, in geometry that
  is the body's own.
- **The body is never cut.** Cutting the body's triangles at the band edges (the earlier way) left hairline cracks:
  a cut point has to be rounded to whole units, so the pieces leave the body's plane and gaps open along the cuts,
  and eight cut versions of every list grew a car to 2 MB. Now the body lists are drawn as they are, and each ends
  with one block per STRIPE value (`add_damage_textures`, `emit_stripe_blocks`) that draws the body's painted
  triangles in that value's bands again, over the body, as decals (`G_RM_ZB_XLU_DECAL`, the body's fog cycle kept),
  from the body's own vertices (or exact copies), with the body's texture coordinates, through the stripe palette
  of the band's level (`stripe_tluts`, 7 x 256 entries kept in the car; the car's own palette loaded back after).
  The same vertices give the same depth, and the same texture coordinates keep all the texture's detail.
  - **Edges by alpha test:** a triangle a band edge crosses is drawn whole once per edge it crosses, left to right,
    each layer through the palette of the region right of its edge (a stripe level's, or the car's own where the
    band ends inside the triangle) and cut off left of the edge by the alpha test (`G_AC_THRESHOLD` at blend alpha
    128): its vertices are copies of the body's whose shade alpha is 128 + k (x - edge), linear in x, so the cut is a
    straight line at the edge's x right across the triangle (RT64 interpolates shade perspective-correct). The last
    layer drawn at a point is its region's. Each edge has one slope k for every triangle it crosses (the steepest
    that keeps all their vertices in 0-255), so a vertex has one alpha and the cut runs on unbroken across
    triangles. The edge layers draw in two cycles: alpha (TEXEL0 - 0) x SHADE in the first, which the alpha test
    reads, the texel's alone in the second, which the blender reads (opaque paint; glass, lights and trim stay
    see-through), the first blender cycle passing the color on and G_FOG off for them (their shade alpha is the
    edge's; restored after). A forced blend keeps RT64 on its hard threshold test rather than its filtered cutout
    coverage. Triangles no edge crosses are drawn whole in their region's palette.
  - **Painted triangles:** the current texture load is a painted one (it has a scuffed copy), G_TEXTURE is on, the
    combiner's first color cycle reads TEXEL0, it doesn't face down (outward normal y >= -0.6), and its corners are
    body vertices. A car mod with `stripes_above_widest` (the PS1 UFO, whose origin is at the ground under its
    saucer, so its underside looked to face up) takes nothing below its widest point and measures facing from the
    middle of its body. The render mode can't tell glass apart (2049 bodies draw with FORCE_BL), but the Super GT's
    windows are untextured triangles drawn while its body texture is still loaded, which the combiner and
    G_TEXTURE tests catch.
  - **Sheen layers** (`track2049::sheen_parts`) are left as they are: they lie on the uncut body.
  - **Dents:** the edge layers' vertex copies move as the body vertex each copies (`Paint::StripeVertex`), and each
    layer's texture load is a damage load like the body's (scuffed by its panel).
- **The palettes** (`stripe_paint`, at the end of car_build_textures, `rush2_car49_paint`): an N64 2049 car's MAIN
  and ACCENT shade k (entries 32-63, 64-95) become shade k of a ramp painted from the STRIPE COLOR as the car's own
  ramps are (the color, then steps towards black); the darkest shades (k >= 28) and every entry that isn't paint
  are see-through (alpha 0), so glass drawn in the paint ramp's near-black end (the Super GT's windows, entry 62),
  lights and trim keep the body's look under a stripe. Its third ramp (96-126) is its TERTIARY COLOR, a row of its own
  (see [Tertiary color](#tertiary-color)). A new car's is the STRIPE COLOR on Rush 2's MAIN and ACCENT ramps (1-31, 33-63), each entry as much darker
  than its ramp's first as the paint's is; rush2::derive reads a stand-in's paint off entries 1 and 33, so the
  full-size image is painted in the stripe's color (entries darker than 12% luma see-through, as above). SNGL FADE's steps blend the paint towards the stripe by level /
  15, as the tile's alpha does. `rush2_car49_paint` reads the STRIPE value from `sp + 0xDB` (the stripe loop's `lb`
  at 0x80085D58; MAIN, ACCENT and STRIPE COLOR are `sp + 0xD3`, `0xD7`, `0xDF`) and opens that value's blocks: each
  block's first command is a branch past it (`0xDE010000`), turned into a G_NOOP for the value picked.
- **Which cars:** the N64 ROM's 2049 cars (`Paint::stripes`), and new cars whose paint mask uses MAIN and that have
  one look (`set_mod_car`: `paint_main`, no color schemes; the PS1 SF Rush cars among them). A Dreamcast disc's
  2049 cars keep their paint jobs (STYLE) and have no stripes. The PS1 police car (PIGGY) is drawn white without a
  paint mask, so it has no stripe, as it has no paint colors.
- **Rows:** the car select shows STRIPE for an N64 2049 car (`src/rush2049/wings_menu.cpp`) and STRIPE and STRIPE
  COLOR for a new car whose mask uses MAIN; the General tab's AI Opponents shows Stripe for both. Saved choices are
  unchanged: these cars' STRIPE starts at NONE (the per-type tables' player rows are 0, and the 2049 and mod types
  copy the Pickup's defaults), so a car looks as before until a stripe is picked. A computer car can draw a stripe of
  its own, as Rush 2's random paint gives one to about half of its drones.
- **Test:** `RUSH2_TEST_RACE_STRIPE=<0-7>[,<color>]` gives every player's car that STRIPE (and STRIPE COLOR) in a
  direct race (src/test_race.cpp), e.g. four players at once:
  `tools/test_race.py 1 --cars 24,27,63,66 --env RUSH2_TEST_RACE_STRIPE=2,4 --script "1:B:70" --shots 45` (two
  2049 cars, the PS1 Mini and UFO), and `RUSH2_TEST_DAMAGE=1` for a damaged car 0. On the car select:
  `tools/test_race.py menus --config cheats.unlock_all_cars=true --env RUSH2_TEST_TRACK=0 --env RUSH2_TEST_CAR=27
  --script "T1:START,T4:A,T6.5:A,T10:A,T12:DD,T13:DD,T14:DD,T15:DD,T16:DD,T17:DD,T18:DR,T19:DR" --seconds 30
  --shots T20,T21,T22,T23` (the Super GT: STRIPE is its seventh row, after TERTIARY COLOR; SINGLE FAT from T19,
  shots as it turns). `RUSH2_TEST_WINDOW=3840x2160` sizes the window for captures at a player's resolution
  (slivers a pixel wide don't show at the default 1600x900). `RUSH2_STRIPE_DUMP=<absolute dir>` writes each
  rewritten car as `<dir>/<body>.bin`. Checked 2026-11-15 at 3840x2160: TRIPLE A on the Super GT, Panther, PS1 bus
  and UFO in a race, the UFO turning on the car select, and fast scrolling through the car select (cars drawn,
  memory flat).
- **Size:** the blocks and edge copies grow a 2049 car from about 55 KB to about 200 KB; the car slots are sized to
  the largest car (`car_slot_size`), which an arcade livery car already sets at about 750 KB. `RUSH2_STRIPE_LOG=1`
  logs each body's size before and after, its edge vertices and blocks.

## Tertiary color

An N64 ROM's Rush 2049 car is painted with three colors (2049's COLOR 1-3, `rush2_car49_paint`: palette ramps 32-63,
64-95, 96-126). Its third used to be the car select's STRIPE COLOR row, relabelled TERTIARY COLOR; with stripes
drawn on these cars (above) that tied the stripe to the car's own third-color parts, so TERTIARY COLOR is a row of its
own under ACCENT COLOR and STRIPE COLOR colors only the stripe.

- **The row** (`src/rush2049/wings_menu.cpp`): option id 18 (`option_tertiary`), inserted after ACCENT COLOR for an
  N64 2049 car's panel (`use_slot`) when the car's textures use the third ramp (`has_tertiary`: at least 16 texels
  in entries 96-126 over every CI8 image its texture-load lists load, `third_ramp_texels`). Seven of the 13 do:
  FORMULA 1, ROCKET ZX, MAGNUM, GX-2, MINI XS, VENOM and EURO LX (88 to 3316 texels); the 8-BALL, SUPER GT, BRUISER,
  LOCUST LX, CRUSHER and PANTHER have none (the SUPER GT 6, its darkest shade), so the row would change nothing and
  is left off. (The palette itself is loaded as a block in the same lists and is skipped: counted, it gave every car
  20-odd texels.) The car select's own code ignores ids above 14, so hooks do what it does for a
  color row: the label (`row_label`, "TERTIARY COLOR"), left / right (`rush2_wings_car_step`: steps through
  `rush2::paint_colors::count()` colors and repaints both panels' previews, `rush2::car2049::repaint_tertiary`), and
  the swatch: carselect_widget_cb_cartype_tables_803BAA44 shows a swatch only for ids 2, 3 and 5, so at 0x803BAAF8
  (`$a3` = the row's option list entry, `$a1` = hide) `rush2_tertiary_swatch_show` keeps it shown for id 18, and at
  0x803BABD8 (`$v1` = the panel; ids 2, 3, 5 have read their table's color into `sp + 0x20`)
  `rush2_tertiary_swatch_value` puts the TERTIARY COLOR there. The option list holds 16 rows (0x803CB3B8 up to the
  panel id at 0x803CB3F8): with the TIRE SIZE cheat's two rows and WINGS a 2049 car's list is full, and the last row
  (TIRE SIZE R) is left off.
- **Kept** per profile and car type in the saves' "tertiary" section (`{"<record>": {"<type>": color}}`,
  `rush2::car2049::tertiary_color`), as the liveries' STYLE is: the player record has no room for it. A player without
  a profile (JUST PLAY) keeps it for the session (owner = player 1-4). A car with none kept takes its STRIPE COLOR,
  which was its third color before; the car select keeps that the first time it shows the car (`slot_tertiary`), so
  a car looks as it did until the row is changed, whatever STRIPE COLOR is set to afterwards.
- **Painted** by `rush2_car49_paint` for the copy's owner (`rush2::car_mods::car_identity`: a car select preview's
  panel player, a race car's human player); a computer car has no owner and keeps its STRIPE COLOR as its third
  color, as before (the AI Opponents' "Tertiary Color" sets both on a 2049 computer car).
- **Online:** `side_block` sends each 2049 car's TERTIARY COLOR after the record's side slot (13 bytes, 0xFF for
  none), and `set_guest_side_block` keeps a guest's for its profile copy; machines in one lobby run the same build
  (the compatibility key holds the build id), so both ends read the same layout.
- **Test:** `tools/test_race.py menus --config cheats.unlock_all_cars=true --env RUSH2_TEST_TRACK=0 --env
  RUSH2_TEST_CAR=25 --script "T1:START,T4:A,T6.5:A,T10:A,T12:DD,T13:DD,T14:DD,T15:DD,T16:DD,T17:DR,T18:DR"` steps
  the Rocket ZX's TERTIARY COLOR (its sixth row); on the Panther (35) the sixth row is STRIPE.

## Track map dots

The race HUD's track map colors each car's dot by its MAIN COLOR: hud_race_setup_widgets (0x800A06F8) fills each dot
model's palette (model +0x14, RGBA5551 halfwords, car i at +2 + i x 2) from the paint table by physics car +0x7EB, in
two loops (ending at 0x800A0848 and 0x800A090C, `$v0` the model). A car drawn in its own textures isn't in its MAIN
COLOR (a new car's color scheme, a livery, a Dreamcast disc's 2049 paint job), so a hook after each loop
(`rush2_map_dot_colors`, src/paint_colors.cpp) gives its dot the color its look reads as
(`rush2::car_mods::look_color`): the color covering most of the body (`scheme_color` in src/car_mods.cpp: the textures
sampled over the triangles by area, binned at 3 bits a channel as the disc's paint job colors are, dark texels left
out), cached on disk per glb (`lookcolor_<hash>`); a Dreamcast paint job's own color (`car2049::paint_job_color`). A
new car with one look and a paint mask keeps the MAIN COLOR dot. Checked: the Deuce at MAIN COLOR 1 (its purple
scheme) shows a purple dot (`test_race.py 1 --env RUSH2_TEST_RACE_CARS=58 --env RUSH2_TEST_RACE_COLOR=1`).

## Car lights

On night tracks, with Graphics > Car Lights on (the Enhanced preset's default), every car's headlamps light the road
ahead and its tail lamps glow red behind it, brighter while it brakes (docs/graphics_effects.md#car-lights). A lamp
goes out once its corner of the car is damaged. A [new car](#new-cars) gets its lamps one of two ways:

- **Found on the body** (no `lights` in its `.json`): tail lamps from its [brake lights](#brake-lights) glb, headlamps
  from bright texels at the front of the body, or else placed from the body's shape. This works for a car with bright,
  clearly textured lamps; dark or gray headlamps, or lamps only painted into one big texture, are often missed.
- **Listed** in the `.json`'s `lights`, one entry per lamp (both sides):

```json
"lights": [
    { "kind": "head", "at": [ 30, 18, 70], "size": 4 },
    { "kind": "head", "at": [-30, 18, 70], "size": 4 },
    { "kind": "tail", "at": [ 28, 22, -68], "size": 3, "color": [220, 40, 30] },
    { "kind": "tail", "at": [-28, 22, -68], "size": 3, "color": [220, 40, 30] }
]
```

| Field | Meaning |
| --- | --- |
| `kind` | `"head"` or `"tail"`. |
| `at` | The lamp's center on the body's surface, in the glb's space (1/16 ft, as `wheels`). |
| `size` | The lens's radius, same units (default 4, a quarter of a foot). |
| `color` | Optional. The lens's color, 0-255: only the texels near it glow (so paint of the same brightness round the lamp doesn't). Without it the brightest texels within `size` glow. |
| `lens` | Optional, default true. `false` for a lamp the model doesn't show: it lights the road but nothing glows on the body. |

Listed headlamps replace the ones found on the body, and listed tail lamps the found tail lamps, kind by kind, so a
car can list just its headlamps and keep its tail lamps from the brake lights.

Finding the numbers: `python tools/car_lamps.py glb <body>.glb tmp/x.json --key mod:<NAME>` turns the body into a
dump, `sheet` draws it from the front and back with the lamps found, and `pick` measures a lamp from the texels in a
box you give (docs/graphics_effects.md#reviewing-the-lamps). [Liveries](#liveries) have no `lights`: theirs are found
on the livery's body and brake lights.

## Flashers

A [new car](#new-cars)'s `.json` can give it roof lights that pulse while it races, day or night (no option: the
PlayStation SF Rush police car's lights, docs/sfrush_ps1.md#police-lights, which the PS1 mod's `PS1POLICE.json`
lists):

```json
"flashers": [
    { "at": [ 20.0, 60.8, -22.3], "color": [255, 0, 0], "phase": 0.0, "size": 19.8 },
    { "at": [-20.0, 60.8, -22.3], "color": [0, 0, 255], "phase": 0.5, "size": 19.8 }
],
"flash_period": 0.5246
```

| Field | Meaning |
| --- | --- |
| `at` | The flare's middle, in the glb's space (1/16 ft, as `wheels`). |
| `color` | Its color, 0-255 (default white). |
| `phase` | Where in the cycle it starts, 0-1 (default 0): 0.5 flashes half a cycle after 0. |
| `size` | Half its width, same units (default 16, a foot). |
| `flash_period` | Seconds a cycle takes (default 0.5): each flasher brightens from dark to full over it and goes dark at once. |

At most 4 per car. Each is a flare (a bright core with a ray across and a ray up, `FLASH_GLOW`) flat in the car's
x-y plane, seen from behind and ahead as the PlayStation's sprite is, drawn by `src/car_flashers.cpp` as a
`rush2::battle_render` model placed with the car's body pose before each view (slots 420-451, its model at RDRAM
0x80E1F480-0x80E1F800), pulled 0.6 ft toward the camera so the roof under it doesn't cut it, and hidden for a
wrecked car, a ghost and a car out of the race. A bad entry fails the car's `.json` as other fields do.

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
- `brakes`: optional. The [brake lights](#brake-lights) of each style, by position in `styles`, as `hoods`.
- `sirens`: optional. Each style's [flipbook](#sirens), by position in `styles`: `{"frames": ["A_SIREN1.glb", ...],
  "fps": 24}` or `null`.

A file that can't be read is skipped and the log says why (`[livery] <file> in <mod>: ...`).

### Sirens

A livery's `sirens` entry is a flipbook drawn on the body, its frames shown one after another at `fps` frames a
second, looping, all the time (the car select's preview too): the arcade's light bars (docs/arcade2049.md#light-bars),
which the `rush2049_arcade_cars` mod gives the Magnum's NYC police livery (CAR4 COL7, 16 frames at 24), the
Bruiser's police livery (CAR7 COL8, 8 at 16) and its roof beacon (COL6, 3 at 20). Each frame is a glb in the car's
space like the style; its vertex colors (`COLOR_0`) are kept, unlit (they color the lenses), and each texture is
drawn in the body's render mode when it is opaque, blended as a brake light when it has see-through texels (the glow
cards round the lit lenses). 2 to 32 frames; a frame that can't be read drops the flipbook.

How it works (`src/car_mods.cpp`): `append_liveries` writes each frame as a model of its own, `~Lnn` + `SRN<n>`
(one LOD, loading its textures inline, rebased by the loader as the brake lights' `~LnnBRK` is), and ends each of the
livery's FRAME1 lists, after the brake light switch, with one switch per frame (`rush2::car2049::siren_switch`): a
`G_NOOP` holding the frame's number with `siren_marker(1)` (0x5C500001), then a `G_NOOP` holding the frame list's file
offset with `siren_marker(2)`. After `add_livery_body` rewrote the lists, `find_siren_switches` records where the
switches ended up (by livery body id). `rush2::car_mods::animate`, from the main loop's top (src/cheats.cpp), turns
on each copy showing the livery (the `shown` map `paint_livery` keeps, its branch checked still in place) the switch
of the frame for the time now into a `G_DL` call to the frame's list in that copy, and the others back to their
`G_NOOP`; a switch holding neither was loaded over and is left alone. `rush2_livery_mirror_begin` / `_end` turn the
shown frame off and on again around the mirror walk, as a frame list is turned over through its own model. The frame
lists' vertices follow the body's dents as brake lights do (`Paint::siren_lists` in `collect_brake_vertices`).

The car select gets a STYLE row under CAR: STYLE 1 is the car as it is, STYLE 2 and up the liveries. While a livery is
picked, the MAIN COLOR, ACCENT COLOR, STRIPE and STRIPE COLOR rows are hidden. The choice is per player profile and
car, kept in the saves' `"liveries"` section (`{"<profile record address>": {"<car type>": style}}`, STYLE 1 not
stored), apart from the car's paint and stripe bytes, so a car that has stripes keeps them for STYLE 1. Computer cars
always use STYLE 1. A car that already has a STYLE row (a Dreamcast disc's Rush 2049 car, with its paint jobs) gets no
second row: its liveries follow its paint jobs on that row, so with 7 jobs STYLE 8 is the first livery (and the stored
style stays the livery's own number, 1 for the first). Turning the mod on or off applies the next time the game is in
the menus. A livery body is a
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
`rush2_livery_mirror_begin` / `_end` put the loaded words back around it. `_end` restores only the lists `_begin`
found still holding the livery's words and drops the rest: a list whose words are gone was loaded over (the copy freed,
another model there), and its first command can match the saved original by chance (a pipe sync), so writing the
branch back put a jump into the new model aimed at the old livery list, now other data; RT64 then never reached the
list's end and the game froze on a black loading screen. The copy is found as `rush2_car49_paint` finds it: the id's texture bank (s16 at `car_banks` + id x 2)
gives the copy's texture records (0x80119220 + bank x 8), and the copy starts that far before them that its header
word +8 (the texture table offset) says. (The car slot word 0x800D97E8 + id x 0x18 +0x14 is the car's palette, not
its copy.) In the car select (game state 2) the id is the preview id, panel x 73 +
type: two players choose in panels 0 and 1, a single player's car shows in panel 1 (race player count 0x8010C3E2 is
1), and the profile is the choosing player's record +0x24. In a race the id is the car 0-7, a human's type at physics
car 0x800F5470 + id x 0x81C +0x7EA (+0x7E8 == 2) and its profile from the player record whose +0 is the car. The
STYLE row is option 17 in `src/rush2049/wings_menu.cpp` (`option_livery`); a step repaints both panels' previews of
the car. On a paint-job car `use_slot` leaves option 17 out and `rush2_wings_car_step` walks jobs then liveries on the
MAIN COLOR (STYLE) row, holding the job byte while a livery is picked.

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
