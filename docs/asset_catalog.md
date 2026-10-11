# Asset catalog: named textures and model dumps

Every model and texture the game can draw, read straight from the ROMs and named after the files and models they come
from. Dump Textures (Settings > Graphics > Your Own Upscales) writes the textures under those names, with the RT64
hashes they replace; Dump Models writes every model file as glTF. Both are the base for replacement packs: a texture
pack keyed by name, and later model imports.

Code:
- `src/asset_catalog_walk.cpp`: reading a container (lists, TMEM, hashes, names, triangles), placement files, glTF.
  No game dependencies.
- `src/asset_catalog.cpp`: the sources (`containers`) and Dump Models.
- `src/texture_upscale.cpp`: Dump Textures and Install Upscaled Textures use the catalog (docs/texture_upscaling.md).
- `include/asset_catalog.h`
- Tools:
  - `tools/asset_catalog_test.cpp` (built by `tools/asset_catalog_test.bat`): runs the reader on Rush 2 assets and
    converted Rush 2049 cars outside the game; prints every texture's name, size, format, hash and key, and writes its
    PNGs and the .glb.
  - `tools/glb_view.py`: renders a dumped .glb to PNG (software, textured), to check model dumps without a 3D app.

## Sources

`rush2::catalog::containers` lists every Rush 2 model container the game draws from:

| Source | Dump path | How |
|---|---|---|
| Rush 2's assets (with the recomp's replacements) | `rush2/...`, see below | `rush2::assets::read_current` |
| SF Rush tracks 1-7 | `sfrush/tracks/<name>` | `rush2::track1::convert_track` |
| Rush 2049 tracks, battle and stunt arenas, obstacle course | `rush2049/tracks/<name>`, `battle/DMn`, `stunt/...` | `rush2::track2049::convert_track` |
| Rush 2049 cars and part models (assets 0x71-0x7E) | `rush2049/cars/<name>`, `cars/parts` | the boot-time replacements |

With `source_files` (the model dump), it also lists every model file of the other two ROMs converted on its own, so
each model keeps its own game's name, and leaves out the 2049 car slots (the 2049 files cover them):

| Source | Dump path | How |
|---|---|---|
| SF Rush assets 7-35 (docs/rush1_research.md section 1.1) | `sfrush/<path>`, below | `rush2::track1::convert_container` |
| Rush 2049 model files (the ones with OBHD objects) | `rush2049/<path>`, below | `rush2::track2049::convert_file` |

Converted tracks are converted afresh with the converters the races use (a race only converts its own track); they are
pure functions of the ROMs, so this runs on a background thread. A Rush 2 track slot holding a converted track is
left out of the Rush 2 list. With the Dreamcast disc as the 2049 source, its folder is `rush2049dc`.

Every container is the one the game would draw, whatever has been opened so far: asset 3 (the track select) gets the
SF Rush and 2049 tracks' art only when that menu first opens (`serve_menu_container`), so `containers` builds it with
`rush2::track2049::catalog_menu_container` (the same build, into a copy of its own). A dump made at the title screen
has the 2049 previews (`R49PTRACKn.m`, 199 textures) as one made after a race does.

Rush 2 asset paths (named after what each holds):
- 0x00 `menus/attract`, 0x01 `menus/background`, 0x02 `menus/circuit`, 0x03 `menus/track_select`
- 0x04-0x0F `maps/<prefix>` (each track slot's map), 0x10 `maps/CIRCUIT`, 0x11 `menus/car_select`
- 0x12 `race/effects`, 0x13 `hud/race`, 0x14 `race/objects`, 0x15 `menus/high_scores`, 0x16-0x18 `hud/pause_1..3`
  (three copies of the same pause and end-of-race art)
- 0x19 `menus/logos`, 0x1A `menus/title`, 0x1B `menus/medals`, 0x1C `cars/stripes` (the stripe tiles stamped on the
  cars' panels: `1F_32X64_L1`...)
- 0x1D + type `cars/<PICKUP...>`, 0x33 + slot `tracks/<prefix>`, anything else `assets/<hex>`

SF Rush paths: 7 `menus/title`, 8 `menus/background`, 9 `menus/main`, 10 `menus/track_select`, 11 `menus/car_select`,
12 `race/objects`, 13 `hud/race`, 14 `menus/high_scores`, 15-17 `hud/pause_1..3`, 18-20 `menus/labels_1..3`, 21-23
`menus/car_labels_1..3`, 24 `menus/trophies`, 25-35 `cars/<BMW...FORM1>`. `convert_container` leaves out a model that
doesn't convert by itself (the menu files' `AAANULLOBJ` placeholders hold a malformed G_VTX, `04300040`), and the
texture table: nothing in a Rush 1 texture record tells a load list from texels (+0x1C's high half is a palette index,
0xFFFF for none), and texels walked as a list ran the dump for minutes. The models load their textures themselves, so
nothing a model draws is lost; the texture-only menu files come out with no models and are skipped. The palette
records stay: a car's are its ten paint jobs (BMWBLKD0 ... BMWYELD0, each entries 0-255). The cars have no texture
table (pointer 0), which `Container` now accepts when the count is 0.

**SF Rush car paint [V]:** the body textures are CI8 with no palette load in the lists (the game loads the chosen
paint). With `read_container(..., paint_records)` (the model dump, SF Rush files only) a painted texture is shown with
the file's RED paint record, made opaque as Rush 1 draws its cars (index 0, the glass blue, has alpha 0). The palette
goes into a copy of TMEM, and `loaded_from` is restored after, or every later texture would count as having a palette
and decode black. The 2049 cars' palette records cover only part of a palette, and the texture dump's recoloring needs
the gray ramp, so both keep gray.

Rush 2049 file paths [V, contents listed from the N64 ROM]:

| File | Holds | Path |
|---|---|---|
| 54 | menu buttons, arrows, bars | `menus/interface` |
| 56 | engines, frames, shocks, tires (setup screen) | `cars/parts` |
| 57 | trophies | `menus/trophies` |
| 60 | track select dioramas TRK_* | `menus/track_select` |
| 61 | explosions | `race/explosions` |
| 62 | coin glow, mines, Rocket flames, smoke, sparks, splashes | `race/effects` |
| 63 | battle coins BCOIN_* | `battle/coins` |
| 67 | stunt icons STNT_* | `stunt/icons` |
| 68 | gold and silver coins | `race/coins` |
| 76 | weapon pickups, weapons, projectiles | `battle/weapons` |
| 77 | wings and their flames | `cars/wings` |
| 78 / 79 | F1FLAG frames / F2FLAG, TRIGGERON/OFF | `race/flags`, `race/flag_triggers` |
| 80 / 81 | WHEEL | `menus/wheel`, `menus/wheel_small` |
| 81 + k | race track k's objects (cones, gondolas, trains, sharks...) | `tracks/<name>/objects` |
| 100 + k | track k (1-6 races, 7-14 battle, 15-18 stunt, 19 obstacle course) | as convert_track's |

Files 0-53 and the rest hold no OBHD objects (files 22-37 have an empty object table). The cars (files 87 + n:
CARnFRAME1, HOOD, SHEEN) come from `convert_car` instead, as the game converts them: 2049 copies a paint job's texels
into the car at run time, so the file alone draws garbage where the paint goes. Their models take Rush 2's part names
(`CARnFRAME1` holds the body; the `CARnD0_*` part slots are empty) and the Rocket ZX gets its flames.

**Texture-only containers [V]:** assets 0x02, 0x13, 0x15-0x18 and 0x1C are model containers with no models: header
model count 0 and a texture table (HUD art, end-of-race banners, the car stripe decals of 0x1C). They are read like
any other.

A texture's game folder is the game its texels come from (`Entry::game_at`): the SF Rush and 2049 logos added to Rush 2's
menu containers go to `sfrush` / `rush2049`.

## Reading a container

`read_container` parses the header, model, name, texture and palette tables (docs/rush2049_research/geometry.md
section 2) and runs every LOD's list as the RSP and RDP would:

- **Calls:** `G_DL` into the texture-load lists (push and branch), `G_ENDDL`, the conditional op `E0 01` taken as the
  game takes it for one player, unmirrored (load flags bits 1 and 3).
- **Texture state:** `G_TEXTURE` (tile, levels, scale), `G_SETOTHERMODE_H` / `G_RDPSETOTHERMODE` (TLUT, texture LOD,
  cycle type), `G_SETCOMBINE`.
  - Defaults per LOD: texturing on, tile 0, scale 1, TLUT RGBA16. The model draw leaves these set (geometry.md 5.3);
    Rush 2's car lists never send `G_TEXTURE`.
- **TMEM:** `G_SETTIMG`, `G_SETTILE`, `G_SETTILESIZE`, `G_LOADBLOCK`, `G_LOADTILE` and `G_LOADTLUT` go to a 4 KB TMEM
  copy through RT64's own load code: `RDP::loadTileOperation` / `loadBlockOperation` / `loadTLUTOperation` and
  `loadToTMEMCommon` (lib/rt64/src/hle/rt64_rdp.cpp). The difference is that file bytes are big-endian, while RT64's
  RDRAM is byte-swapped in words (its `^3`).
- **At each triangle:**
  - The tiles RT64 samples are recorded: the render tile, then tile + 1 .. + levels under texture LOD, or tile + 1 in
    2-cycle mode when the combiner reads TEXEL1. This is RT64's `drawCall.tileCount` rule (rt64_state.cpp).
  - Each tile's sample size is RT64's: the tile's size where it clamps, its mask where it wraps, the smaller.
  - The hash is `RT64::TMEMHasher::hash(tmem, tile, w, h, tlut, 5)`, the replacement hash RT64 gives the texture
    observer and texture packs.
  - The image is `decode_tmem` of the same state, and its key is `content_key`. Paletted textures also get
    `index_key`.
  - Tiles RT64 would draw from raw TMEM are skipped; packs and the observer never see them.
- **Paint:** a paletted draw whose palette no list loads (Rush 2's car bodies) is painted: the game loads the palette
  (the player's paint). It gets no hash, and its image uses a gray ramp: entry & 31 for CI8, since Rush 2's paints are
  32-color ramps.
- **Geometry:** `G_VTX` (32-entry buffer), `G_TRI1`, `G_TRI2` and `G_QUAD` give the triangles, grouped by texture.
  - UVs are `(s * scale * shift - uls) / width`, in units of the texture's size.
- **Every load list [V, 2026-10-10]:** after the models, each texture-load list of the file's load range (header +0x1C
  to +0x20) that loads texels not cataloged yet is run on its own (`list_texture`), named `LOADLIST.n` unless a record
  names its texels. These are textures only code points a model at, such as the Rocket ZX's exhaust flame levels.
- **Copies [V, 2026-10-10]:** `Entry::copies` (pristine texels -> copy) are texels the code swaps in for others: a 2049
  car's scuffed panels (`rush2::car2049::scuffed_texels`, built by `add_damage_textures`, which the car's damage points
  the body's loads at). The models are walked again with each `G_SETTIMG` of a pristine panel read from its copy, and
  the copy is named after the panel with `_D1`, as Rush 2's own damaged panels are (`C11_HOOD01_D1`).

**Checked in game (2026-10-10):** every texture a race draws is a catalog texture or made from one at run time
(Textures made at run time).

## Texture records and the builder [V]

Textures the code looks up by name (texture table records, 0x20 bytes) are loaded through the texture-load builder
`func_8007825C`. Records no list draws (car parts' swapped damage textures, rims, HUD art) are emulated the same way.
Decompiled with `tools/rush2049/decomp.py r2 8007825C`.

- **Record flag 0x08000000 clear: a list record.**
  - +0x18 is a texture-load list, which the builder calls with `G_DL` (`DE000000 data`). Examples: `CHKPNT`, the
    Dreamcast converter's records, `RIM01`-`RIM21` and `RIMB1`-`RIMB3` in asset 0x12.
  - The catalog runs the list and records tile 0 and every other tile it sets up as mipmap levels. RIM01's list sets
    tiles 0-4 to 32x32 .. 2x2 with two CI4 palettes, and the game draws the 16x16 level as the blurred rim.
  - The TMEM is the game's, so these get exact hashes.
- **Flag 0x08000000 set: a texel record.**
  - **+0x14 is siz and +0x15 fmt**, the other way round from a list record's bytes. For example, Rush 2's car records
    are `01 02` = CI8, flags `0x28030000`.
  - Flags 0x10000 / 0x20000 clamp S / T (mask 0), and 0x40000 / 0x80000 mirror them. Masks are otherwise log2 of the
    size (`func_80055868`).
  - The catalog loads the texels as one block at TMEM 0 and hashes that. **[V, 2026-10-10]** Every texel record a
    session had drawn (17 in Rush 2) was drawn under this hash, so the builder's layout is the same. Hashing them
    gave 241 more Rush 2 images a hash without being drawn first (the menu frames and labels), so their upscales
    install.
  - A texel record too big for one load (77 in Rush 2, all 2D menu art such as `MIDWAY` and `TITLELEFT`) is drawn in
    strips. The catalog decodes it whole, a TMEM-sized band of rows at a time, with no hash (RT64 hashes the strips);
    the strips drawn are traced to it and replaced as regions of its upscale (docs/texture_upscaling.md, Derived
    replacements).
- **Palette, both kinds:** when +0x16 >= 0 the builder calls `func_80078190` with palette record +0x16, after the
  texture.
  - Palette records are 0x18 bytes: name[16], **u8 first, u8 last** (+0x10, +0x11; `00 0F` for 16 colors), u16, u32
    data.
  - It loads entries first..last: `G_SETTIMG` RGBA16 of the data, `G_SETTILE` tile 7 at TMEM 0x100 + first, then
    `G_LOADTLUT` with lrs = (last - first) << 2.
  - It skips the load when the palette pointer equals the last one loaded (0x80111954).

## Names

Each texture gets one name, unique in its container:

1. **Texture table name:** a record's name for its texels (the data of a texel record, the last `G_SETTIMG` of a list
   record).
2. **Otherwise the first model drawing it:** `MODEL` when that model brings one new texture, else `MODEL.1`,
   `MODEL.2`... in order of first use.
3. **Another draw of the same texels** (another palette or tile) gets `~2`, `~3`...
4. **A mipmap level** gets `<level 0's name>.mipN`, N being the tile offset.

File names keep letters, digits, `.`, `_`, `-` and `~` (`file_name`); anything else becomes `_`.

**Unique file names [V, 2026-10-10]:** in the texture dump, a file name is unique within its game folder, not just its
container. Upscalers such as Gigapixel write a whole folder tree into one folder, and Install then matches by file name
alone (docs/texture_upscaling.md). `dump_named` keeps the names it has given per game:
- A name another model file already has for a different image takes this container's last folder:
  `hud/pause_2/1STPLACE~pause_2`, `battle/DM2/SHEEN0~DM2`, `tracks/Market/HAWAIIFINISH.12~Market`.
- One used twice in the same file gets `~2`, `~3`, as before.
- A mipmap level follows its texture's file name: `AL_COLUN5~Civic.mip2`.


## Texture dump

Dump Textures (`start_dump`, `dump_named` in src/texture_upscale.cpp) writes only what the ROMs hold, the same whatever
has been played. It deletes `asset_dump/textures` and writes afresh, per game folder (`rush2`, `sfrush`, `rush2049`):

- **Catalog textures:** every one, at `<container path>/<name>.png`, the file names unique in the folder (Names). The
  first container with an image names it, by content key. Car body textures are gray: the game supplies their
  palettes (the paint).
- **Interface art** is left out unless Upscale Interface is on (Settings > Graphics): containers whose path
  `rush2::catalog::ui_path` calls interface (`menus/`, `hud/`, `maps/`, `stunt/icons`) and the disc's HUD, HUDBATTLE,
  SELCAR and SELTRK files. A texture also on a car or track is dumped there.
- **`names.txt`:** `<path> <key>` per image. **`hashes.txt`:** key -> the catalog's RT64 hash (none for a paint base, a
  strip-drawn image or a raw-TMEM texture).
- **`rush2049dc`:** every image of the Dreamcast disc (`rush2::rom2049::dc::dump_images`), by disc file and texture
  name, the damaged car textures left out (they are made from the undamaged ones).

Nothing the game draws is dumped: textures it makes at run time from these (other palettes, stamped stripes, copied
signs, strips) get their replacements from these ones' upscales (Textures made at run time). A full dump with all three
ROMs and a disc writes 7,173 catalog textures and 3,047 disc images with Upscale Interface off [V, 2026-10-11].

**Migration:** the pushed builds dumped and saved upscales by content key (`<key><anything>.<ext>` at the top of a game
folder of `user_texture_pack`). `migrate_key_names` moves those upscales to their names, in whichever game folder names
the key, at startup (`migrate_names_on_disk`, from the last dump's `names.txt`) and after each dump. An upscale whose key
the catalog doesn't name stays where it is; an identical one already at the name is deleted.

**Install** (`install`):
- **Reading upscales:** walks `user_texture_pack/<folder>` recursively. A file is the longest dumped path its own path
  starts with, else the longest dumped file name its file name starts with (flat folders), else the key its file name
  starts with. Only images of the dump install.
- **Pack layout:** the pack's files mirror the names: `textures/rush2/race/effects/RIM01.dds`. Each is listed under its
  RT64 hashes and its content key: the key is no hash RT64 draws, but it is how the derived replacements find a
  texture's upscale in any pack, a paint base's included.

## Model dump

Dump Models (`start_model_dump`) lists the containers with `source_files` and writes, under
`<app folder>/asset_dump/models/<game>/<path>/`:
- `<model>.glb` for every model with triangles: its LODs and just the textures it draws (`single_model`);
- `_all.glb` with every model of a container holding several (a car's parts, a file of effects);
- `_placed.glb` for a track: the track as placed (scene 0) and its models (scene 1). The converted 2049 tracks only
  give this one (`Entry::placed_only`), as their models come from the 2049 files under their own names.

Each dump first deletes the game folders of the last one (older dumps had one `<path>.glb` per container). A full dump
with all three ROMs writes about 4,600 model files in about 15 s (`[catalog] Model dump: N s` in the log; an entry
taking over 2 s is logged by name). The walker stops a list after 100,000 commands, so data read as a list can't stall
it.

The glTF 2.0 binary (`write_glb`):
- **Nodes:** one per model, its first LOD. The other LODs are its children named `<model> LOD n`, scaled to 0 so
  viewers show the model once. The LOD distance and flags, and the model's kind (breakable class) and radius, are in
  `extras`.
- **Meshes:** one primitive per texture: positions, UV, vertex color. For 2049 LODs with flag 0x10, the vertex colors
  are normals, so they're written white.
- **Materials:** unlit (`KHR_materials_unlit`). Alpha is BLEND for graded alpha, MASK for cutouts, else OPAQUE. Samplers
  follow the tile's wrap, mirror or clamp.
- **Textures:** embedded PNGs under their catalog names.
- **Tracks** have two scenes:
  - Scene 0 is the track as placed: every placement record (docs/rush2049_research/placement.md section 1) draws its
    model, or its class's (the shortest model name starting with the record's: CONE1 -> CONE1O1). Sounds, keys and
    collision volumes have no model and are left out.
  - Scene 1 is every model at the origin.
- **Units [V]:** placement positions are world units, and models are drawn at 1/16 scale. For example, ALCATRAZL4284's
  vertices span x 1601..2306, which is 100..144 world units at its record's x of 688, meeting its neighbors. Placed
  nodes carry the record's matrix times 1/16, so scene 0 is in the game's world units, and the models scene in vertex
  units.
- **Handedness [V, 2026-10-10]:** the game's space is the mirror image of glTF's right-handed one. Written as is, every
  model came out mirrored, with its triangles (which the game winds clockwise seen from outside) facing inward, so
  viewers that cull back faces showed models inside out. `write_glb` negates x: vertices, placed positions, and placed
  matrices as S M S with S = diag(-1, 1, 1). Checked: the signed volume of closed models (CONE1O1, KEYL1, GASPUMPG1,
  WEP_ROCKG1) went from negative to positive, and "TAXI" on the SF Rush cab's roof reads correctly from the front and
  back. `tools/glb_view.py` had the same mirror (its camera looked down +z) and hid this; it is right-handed now.
- Checked with `tools/glb_view.py`:
  - Rush 2's Alcatraz comes out whole, the island with the track loop.
  - 2049 Marina and SF Rush Golden Gate come out whole.
  - The PICKUP and Rocket ZX bodies are textured, with gray paint.
  - SF Rush BMW and TAXI (D0 parts and FRAME1) come out whole and red, as tools/rush1/carview.py draws them.
  - 2049 props, weapons and coins (GASPUMPG1, WEP_ROCKG1, WEPICON_MISSL1_, GOLDCOING_COIN) are textured as in game.

## Textures made at run time

Every texture the game draws comes from the ROMs: a catalog texture as is, or one the game changes as it runs. The
changed ones aren't dumped; `rush2::derive` (src/texture_derive.cpp, docs/texture_upscaling.md "Derived
replacements") traces each to its catalog texture and makes its replacement from that one's upscale. The kinds, found
with the texture census (`tools/texture_census.py`, src/texture_census.cpp) on 17 tracks of all three games
[V, 2026-10-10]:

- **Other palettes:** car paint jobs (Rush 2, 2049 N64, SF Rush), the tracks' cycling sign palettes. Same indices.
- **Stripes:** Rush 2 stamps a stripe tile (asset 0x1C, `cars/stripes`) on each car panel
  (`car_stamp_stripes_on_panel_80083F50`): the texels under the tile's alpha are remapped to the STRIPE COLOR. Heavily
  striped panels share no bytes at their start with the file.
- **SF Rush stripes on Rush 2 cars:** `src/rush1/car1_stripes.cpp` writes Rush 1's decals into the panels.
- **Copies:** a texture's texels copied over another's: `MPH75HITO1`'s address draws the 45 MINIMUM sign
  (`MPH45HITO1`'s texels) on some tracks, its bottom rows reading the font stored after it.
- **Taller tiles than loads:** `NYTWOCC02.6` is drawn 64 rows tall with its last rows left over in TMEM from earlier
  loads.
- **Strips** of the big menu images.
- **2049 car damage:** the scuffed panels and the Rocket ZX's flame levels, now cataloged (Reading a container).

The census proves the coverage: with a pack of tinted stand-ins for every catalog texture (`tools/fake_upscales.py`),
every texture a race draws is replaced by the pack or derived from it, none untraced (docs/texture_upscaling.md).

## Limits

- **Paint bases:** a painted texture's gray image has no RT64 hash, because the game supplies the palette; it is
  drawn only through its paint jobs, derived from its upscale.
- **SF Rush stripes on Rush 2 cars** are derived from the panel's upscale by recoloring, so their edges follow the
  panel's texels, not an upscale of the Rush 1 decal.
- **Raw TMEM:** textures RT64 draws from raw TMEM aren't cataloged.
- **Dreamcast names:** the N64 2049 textures take the disc's texture names only when a disc is installed and its
  image is matched (`dc::match`); otherwise they are named by model.
- **Model dump textures:** the 2049 files converted on their own keep 2049's lists as drawn, so lit models' vertex
  colors are their normals (written white); Rush 2 and 2049 car paint comes out gray, as in the texture dump.
- **`_all.glb`** puts every model at the origin, so a car's damage levels and wheels overlap there; the per-model
  files are the ones to edit.
