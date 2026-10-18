# Model mods

A mod can replace any model the game draws (track sections, menu models, car parts, props) with a mesh of its own:
see [Replacing any model](#replacing-any-model). Whole cars have their own format, which also takes the player's paint
and dents.

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
  relocator rebases each LOD list on its own). Every other one of the 37 parts func_80086700 looks up is an empty list,
  so Rush 2's damage panel swaps draw nothing.
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

- **Wheels and lights:** the wheels and their placement stay Rush 2's. The headlight parts (H0_L ...) are empty, so a
  modded car has no headlight glow.
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
- `<ID>.json`:

```json
{
    "name": "CUDA",
    "base": "CAMARO",
    "wheels": { "front": [39.2, 3.2, 60.0], "rear": [39.2, 4.0, -63.4] },
    "wheel_scale": [1.08, 1.08],
    "box": [6.9, 6.9, 3.0, 3.0],
    "main_color": 26,
    "accent_color": 3
}
```

| Key | Meaning |
|---|---|
| `name` | Required. The car select's logo and the AI Opponents list show it. Up to 12 letters, digits, spaces or `-`. |
| `base` | The Rush 2 car it starts from, by its prefix (see the table in [Mod format](#mod-format)). Default `PICKUP`. |
| `wheels` | The front and rear right wheel centers, in the glb's space (1/16 ft units). The left wheels mirror them (the sign of x doesn't matter). Default: the base car's. Rush 2's own wheels sit at y 3.2 (front) and 4.0 (rear), x 44 and z ±62.7. |
| `wheel_scale` | Front and rear wheel model sizes, as Rush 2's TIRE SIZE (most of its cars use 1.0, the Pickup 1.25). A player's TIRE SIZE for the car starts here. Default: the base car's. |
| `box` | The box the car hits other cars with: front, rear, half width, height in feet. Default: from the mesh, about a quarter foot inside each end, at least 3 ft to a side, 3 ft high. |
| `main_color`, `accent_color` | The colors a new player starts the car in, 0-31 (Rush 2's color list). Default: the base car's. |
| `schemes` | Color schemes: a list of glb file names in `rush2/new_cars/` (up to 16), in STYLE order. Without it the car is `<ID>.glb` with paint colors. |
| `physics_from` | A Rush 2049 car (`"BRUISER"`, `"8-BALL"`, ...) whose physics the car takes in place of the base car's (see [Physics](#physics)). |
| `physics` | The car's handling, speed and weight (see [Physics](#physics)). Each value you leave out is the base car's. |

The wheels' drawn position is their center, and the car rides on them like the base car.

### Physics

`physics_from` names a Rush 2049 car: the new car then drives as that car does in its default setup, its physics
exactly as the game builds them for it (its descriptor and its weight, inertia, steering, drive and settings rows),
with the new car's own wheels and box. Without Rush 2049 loaded, the base car's physics are used instead.

`physics` is an object of the values below, in Rush 2's own units (the values Rush 2's tables and car descriptors
hold; `python tools/rush2049/cars.py r2` prints every stock car's). Give any of them; the rest stay the base car's.
A key the game doesn't know, or a value of the wrong shape, skips the car and the log says why.

| Key | Meaning (Rush 2's stock range) |
|---|---|
| `mass` | Weight in slugs (lb / 32.2). |
| `pitch_roll_inertia`, `yaw_inertia` | How hard the car is to tip and to turn. |
| `preload` | Suspension preload. |
| `inertia_constants` | The descriptor's 3 chassis constants (their inverses are set with them). |
| `suspension` | The descriptor's 14 spring and damper values, front and rear pairs. |
| `suspension_constants` | The 2 suspension constants that follow them (16, 4). |
| `drag`, `rolling_resistance` | Air drag (0.0135) and rolling resistance (75): top speed. |
| `rear_grip` | Rear tires' sideways grip factor. |
| `tire_curves` | `"base"` or `"2049"`: Rush 2049's front and rear tire curves, as its cars use (only with Rush 2049 loaded; otherwise the base car's). |
| `drivetrain_inertia`, `final_drive`, `clutch` | Drivetrain (2.5, 3.3, 600). |
| `torque` | 4 torque scales: gear 1, gear 2, gears 3 and up, and the common engine map. |
| `gears` | 6 gear ratios: reverse, neutral, 1st to 4th. |
| `top_gear` | 1-4. |
| `shift_up`, `shift_down` | Automatic shift points, engine speed in radians a second (662.35 = 6325 rpm). |
| `steering`, `yaw_damping` | Turning force and its damping. |
| `drive` | Drive flags, as Rush 2's per-car table. |
| `suspension_setting`, `tires_setting` | Where the SUSPENSION and TIRES options start (TIRES 0-10). |
| `durability` | Where DURABILITY starts (the setup weight value). |

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
  follows the Graphics > Car Sheen setting like a Rush 2 car.
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

`tools/arcade2049/car_mod.py OUT_MOD_DIR` builds the `rush2049_arcade_cars` mod from the arcade data ProjectR unpacks
(docs/arcade2049.md). It holds every arcade car (the table in docs/arcade2049.md#cars). Every `COL` folder of a car is one color scheme (`<ID>_COLn.glb`, picked with STYLE), each its body's
full-detail LOD with the arcade's textures as authored. The wheels are at the arcade's own points, the same for every
arcade car (docs/arcade2049.md#wheel-positions). `--sides SIDES.png` draws them over the side views to check.

A car with a Rush 2049 (N64) counterpart takes its physics (`physics_from`). The Deuce and the Caddy have none: their
`physics` are the arcade's, mapped onto Rush 2 as Rush 2049's N64 cars are (`physics()` in the tool,
docs/arcade2049.md#car-physics). Every car's `box` is the arcade's own. The tool reads the Rush 2 and Rush 2049 ROMs for that (`RUSH2_ROM`, `RUSH2049_ROM`).

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
