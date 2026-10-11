# Texture upscaling

Settings > Graphics > Texture Upscaling redraws the game's 3D textures at a higher resolution while the game runs. The
same section has buttons to dump the textures for any other upscaler and to install the results as a texture pack.
The default is Off, which keeps the original game's look.

Code:
- `src/texture_upscale.cpp`: the observer, the worker, the options and the buttons.
- `src/texture_upscale_images.cpp`: image work with no renderer or UI dependencies (decoding, recoloring, padding,
  hqx, DDS, zips, running programs).
- `include/texture_upscale.h`
- `lib/hqx`: hq2x and hq4x, vendored (LGPL 2.1+, see its README.md for the changes).
- The RT64 side is in `lib/patches/rt64.patch`: `src/common/rt64_live_textures.h`, a hook in
  `State::fullSyncFramebufferPairTiles` (`rt64_state.cpp`), and the live replacement layer in `rt64_texture_cache.cpp`.
- `tools/texture_upscale_test.cpp` tests the image code. Build instructions are in its header comment.

## Modes

| Mode | What runs | Cache folder |
|---|---|---|
| HQ2x, HQ4x | hqx on the worker thread (CPU, about a millisecond per texture) | `hq2x`, `hq4x` |
| Custom | the Custom Upscaler Command option, on a folder of padded PNGs | `custom` |

Custom details:

- **Command:** `{input}` and `{output}` are replaced with the quoted work folders. The output can be any integer scale,
  and file names only need to start with the input's 16-character key.
- **Batches:** runs take 16 textures at a time and pause 250 ms between runs. Changing the mode kills a running
  command (`run_command`'s cancel flag).
- **Garbage check:** `matches_original` averages each output back down to the original's size. If it doesn't match,
  that texture uses HQ2x instead, and the settings page says so.

### History: ESRGAN

Real-ESRGAN modes (ncnn-vulkan, `realesr-animevideov3`, downloaded on first use) were tried and removed:

- Its x2 model gave noise on a user's GPU.
- Running it on the same GPU as the game made the game stutter.

What's left of it:

- At startup, the recomp deletes the files the ESRGAN modes left: `texture_upscale/realesrgan` (at migration) and the `esrgan_*`
  cache folders.
- Saved configs map `Esrgan2x` to HQ2x and `Esrgan4x` to HQ4x (`on_json_parse_option`).

## Picking textures

RT64 calls `RT64::TextureObserver::textureDrawn` for every tile a draw call samples from TMEM. That excludes
framebuffer copies, raw TMEM fallbacks and extended-GBI replacement IDs. The call passes:

- the texture's replacement hash;
- the TMEM contents and the tile;
- the sample size and the TLUT mode;
- whether the draw's projection is `Projection::Type::Perspective`.

The rules:

- **Drawn in 2D:** any texture drawn under a rectangle, orthographic or LLE-triangle projection counts as UI and is
  not upscaled (unless Upscale Interface is on, below). That covers the HUD, menus, fonts and 2D images.
  - If it was upscaled already, its live replacement is removed.
  - Its content key is added to `asset_dump/ui_textures.txt` so later sessions skip it as well.
  - Adding a key to that file by hand (the first 16 characters of a dumped file name) excludes a texture.
- **Interface art drawn in 3D:** a texture only in the catalog's interface containers (`rush2::catalog::ui_path`:
  `menus/`, `hud/`, `maps/`, `stunt/icons`; one also on a car or track isn't) counts as UI too, such as HUD pieces drawn
  with a perspective projection. `take_jobs` asks `rush2::derive::interface_texture` before upscaling anything, so it
  waits for the derive index (built from the catalog at the first draw).
- **Upscale Interface** (Settings > Graphics > Textures, off by default, takes effect at once): on, UI textures are
  upscaled like the rest, 2D draws included (`set_ui` requeues every texture), get derived replacements, and Dump
  Textures writes them.
- **Drawn in 3D:** a texture is kept once it has been drawn under a perspective projection on 3 separate submission
  frames. This skips textures that only exist for one frame. On that frame it is decoded from TMEM on the CPU with a
  port of `sampleTMEM` from RT64's `TextureDecoder.hlsli` (`decode_tmem`), so the pixels match RT64's own decode.
- **Too small or flat:** textures under 64 texels, under 4 texels on a side, or of a single color aren't kept.

Textures are identified by their decoded pixels (`content_key`, XXH3 over width, height and RGBA). RT64 can hash the
same image differently, for example when it is loaded with another tile or palette load, or when the TMEM around it
differs. Every such hash is mapped to the one image, which is upscaled and stored once.

## Palette variants (car paint)

RT64's hash covers the palette entries a texture uses, so every paint job gives every car texture a new hash and new
pixels. The counts:

- 32 variants for a texture that uses one paint ramp;
- 1,024 for two ramps;
- about 32,768 for three.

Upscaling each variant is out of the question, so paletted textures (TLUT on) also get an index key: XXH3 of the
decoded palette indices, size and texel size. The first variant seen of an index key is upscaled. Every later
variant is made from that upscale by `recolor`, on the worker in a few milliseconds, and cached like any upscale. A
variant whose base is still waiting is queued behind it. If the base fails, or is UI, the variant is upscaled itself.

How `recolor` works:

1. Each upscaled pixel looks at the 3x3 original texels around it. It finds the pair (or single texel) whose old
   colors, blended, come closest to the pixel, with a small cost for distance.
2. It becomes the same blend of those texels' new colors.
3. Detail the blend doesn't explain is added back, scaled by the new-to-old brightness ratio.

So an anti-aliased edge between red paint and a black line becomes the matching edge between blue paint and the
line, rather than keeping a red fringe. The paint ramps are a color stepping towards black (`rush2_car49_paint`,
`func_8008582C`), which this preserves.

Measured against upscaling the variant directly, recoloring a red car texture to blue gives a mean error out of 255
of 0.2 to 0.4 for HQx. The unrecolored red upscale is 64 away. With Real-ESRGAN x4 (tried before it was removed), the
error was 5, mostly from ESRGAN itself treating red and blue slightly differently.

This covers any texture the game recolors through its palette, not only cars. Rush 2's stamped stripes change the
indices, so each stripe pattern is its own base.

## Live replacements (RT64 patch)

`RT64::addLiveReplacement(hash, contentKey, fileBytes, region = {})` gives the running texture cache a replacement
image without a texture pack. Its behavior:

- **Upload:** images are uploaded once per content key on the texture cache's upload thread, the way RT64 loads a
  pack texture with the `stall` operation. They are transitioned to shader read like stream results. A batch takes
  at most 8, because the game's thread waits on every batch that also carries its own TMEM uploads.
- **Replacing:** each hash then gets `TextureMap::replace` with a half-texel shift, the default for packs, and its
  region (below).
  - Hashes that load later are replaced as their upload finishes.
  - A pack reload (`clearReplacementDirectories`) queues every live hash to be applied again.
  - Each live addition is checked again under the texture map lock before it's applied. A replacement removed in
    the meantime (by a mode change) has its texture on the way to deletion, and drawing it showed garbage.
- **Packs win:** a loaded pack that replaces the same hash takes priority (`packReplaces`), and its streamed texture
  replaces the live one when it arrives.
- **Removal:** `removeLiveReplacement` calls `TextureMap::unreplace` and frees the texture through `evictedTextures`
  once nothing uses it. `clearLiveReplacements` does the same for everything in one pass.

Lock order: `textureMapMutex` before `uploadQueueMutex` before `liveMutex`. `liveMutex` is never held while taking
another lock.

The recomp gives each mode its own content keys (`live_key`), so a result that lands after the mode changes can't stand
in for the new mode's image.

## Replacement regions and same-size replacements (RT64 patch)

Two rules in RT64's replacement sampling, for every replacement (packs, live ones, upscales):

- **Same size is not high resolution.** A replacement exactly the size of the game's texture (`tcScale` 1) is sampled
  the way the game's texture is: in 2D at the console's pixels (`lowResUV` in RasterPS.hlsl) and without a pack's
  half-texel shift (`createGPUTiles` in rt64_framebuffer_renderer.cpp sets `highRes` only for another size). Such a
  replacement changes a texture's colors texel for texel and draws it exactly where and as sharply as the game does.
  Before, it was sampled at every screen pixel with the half shift, which blurred it, sampled past the texture's
  edges and moved a vertically flipped texture by most of a texel.
- **Regions.** `RT64::ReplacementRegion` says which part of a larger image a hash's texture is (`x`, `y`, the image's
  `sourceWidth` x `sourceHeight` in the game's texels). The replacement is the whole image, scaled against the image,
  and `texelShift` places the region in it. A filter's neighbor just past the region, next to a texel inside it, is
  read from the image (`sampleTexel` in TextureSampler.hlsli) instead of wrapping or clamping back into the region; all
  other addressing is the tile's. Region tiles skip the native samplers and mipmaps, which don't know the region. A
  game that loads a texture in strips (a 128x32 CI8 logo is two 128x16 loads, since a TLUT leaves 2 KB of TMEM for
  texels) then draws it from one image with no seam where the strips meet. The default region is the whole texture,
  which is how packs and every other replacement work.

Building RT64: the shader build doesn't track `.hlsli` includes. After editing TextureSampler.hlsli (or another
include), touch `lib/rt64/src/shaders/RasterPS.hlsl` so the raster shaders are compiled again.

## Exact images (track banners)

`rush2::upscale::add_exact_image(indices, image)` draws a CI8 texture with a full-color image in place of its 256
colors, whatever the upscaling mode. The track banners (`add_banner_images` in src/rush1/track1_convert.cpp and
src/rush2049/track2049_art.cpp, docs/rush1_research.md section 12) are its only users: their art has about a thousand colors
even at 5 bits a channel, so no CI8 copy is exact.

Each 2D CI8 hash is decoded once when first seen (`find_exact`) and its indices are compared with every row offset of
the registered images. A match gets the whole image with the region of it the strip is (`apply_exact`). The image is
the texture's size, so RT64 samples it exactly like the game's texture. A strip of one index (blank rows) is never
matched, since other textures share it. Mode changes clear every live replacement, so `set_mode` queues the strips
again.

## Distant Textures: Smooth with texture packs

Smooth (`RT64::GeneratedMipmapsEnabled`) filters replacements too, not only the game's own textures. Before this,
every tile marked high resolution skipped it:

- A PNG replacement had no mipmaps, so it shimmered whatever the setting.
- A DDS replacement with mipmaps went through RT64's replacement path, which ignores the setting, the anisotropy level
  and cutout coverage.

The RT64 patch now handles them this way:

- **Mipmaps for PNGs:** `loadTextureFromBytes` gives PNG replacements a full mip chain when they load
  (`makeMipmappedDDS`, the same alpha-weighted averaging as `TextureDecodeCS.hlsl`), uploaded through `setDDS`.
  - They are marked `Texture::generatedMipmaps`, and `TextureMap::use` reports them only while Smooth is on.
  - With Original, a PNG pack draws as it always did.
- **New tile flag:** `createGPUTiles` sets `generatedMipmaps` (bit 9 of `GPUTileFlags`) for a tile with mipmaps that
  is either the game's texture or a replacement while Smooth is on.
  - `sampleTexture` and RasterPS's override of the game's LOD key on that flag instead of `hasMipmaps && !highRes`.
  - So replacements get `sampleGeneratedMipmaps`: anisotropy from the Anisotropic Filtering setting, and alpha
    coverage for cutouts.
  - Replacements with their own mipmaps keep RT64's replacement path when Smooth is off.
- **Coordinates:** a replacement's coordinates are in its own texels and already shifted to texel centers.
  `sampleGeneratedMipmaps` adds the half-texel offset only for the game's textures, and the derivatives are scaled by
  `tcScale`.

## Your own upscales (Dump / Install)

**Folders:** everything lives in `<app folder>/asset_dump` (`asset_dump_dir`):
- `textures/` holds Dump Textures' images, a folder per game.
- `models/` holds Dump Models' .glb files (docs/asset_catalog.md).
- `user_texture_pack/` holds the player's upscales, in the same folders as the dump.
- `generated_texture_cache/` holds what the game makes on its own:
  - the upscaling modes' results (`hq2x`, `hq4x`, `custom`);
  - the Custom upscaler's `work` files;
  - `dreamcast`, the installed upscales of the disc's images, made only when there are some.
- `ui_textures.txt` sits at the top.

The installed pack is `mods/rush2_custom_textures.rtz`, the one file to share.

**Migration [V, 2026-10-11]:** the migrations run on the upscaler's worker thread before it takes any work, so they
never hold up the window (a player doesn't know they are there); Dump and Install wait for them (`wait_migrated`).
`tools/startup_time.py` times a launch line by line. `migrate_folders` moves the older layout:
- `texture_upscale/dump` and `texture_upscale/originals` go to `textures` (a dump always holds every image, so Dump
  New Textures' originals go back in it);
- `texture_upscale/upscaled` goes to `user_texture_pack`;
- `texture_upscale/cache` goes to `generated_texture_cache`, and `texture_upscale/dreamcast` to its `dreamcast`;
- `work` and `realesrgan` are deleted;
- `texture_upscale` itself is removed once empty;
- the old share copy `texture_upscale/rush2_custom_textures.rtz` is deleted.

A pack folder `mods/rush2_custom_textures` (one holding `mod.json`, from builds that installed a folder) is left as it
is: it loads as a folder mod. The next Install removes it as it puts the .rtz in (`replace_pack`). It used to be
zipped at startup, before the window opened.

Then `migrate_names_on_disk` moves key-named upscales (the pushed builds named them by key) to the names in the last
dump's `names.txt` (docs/asset_catalog.md, Texture dump).

**Buttons:** under the "Your Own Upscales" heading:
- one row: Dump Textures, Dump Models;
- a second row: Open Dump Folder (opens `asset_dump`), Install Upscaled Textures.

Each row is a `rush2::ui::FocusRow` (`OptionsPage::add_row`), which scrolls into view when a button in it takes focus,
so the controller can reach the buttons.

**Status:** each button shows its own progress. What the upscaling mode is doing ("Upscaling textures...", or the
Custom upscaler's last problem, kept to one line) shows in the top right corner of the Texture Upscaling option
(`OptionsPage::add_option_status`). The label is laid over the row, so it moves nothing.

**Dump Textures** runs in the background. It writes every texture of every model the game can draw, read from the ROMs
and named after the model file and model (docs/asset_catalog.md, Texture dump):
`textures/<game>/<file path>/<name>.png`, in a folder per game the texels come from (`rush2`, `sfrush`, `rush2049`).
- Only the ROMs' textures: nothing a session drew. `textures` is deleted and written afresh, so it is the same whatever
  has been played. What the game makes from these as it runs is replaced from their upscales (Derived replacements).
- Interface containers (`menus/`, `hud/`, `maps/`, `stunt/icons`) and the disc's HUD, HUDBATTLE, SELCAR and SELTRK
  files are left out unless Upscale Interface is on. A texture they share with a car or track is dumped under that.
- Every image of the Rush 2049 Dreamcast disc goes into `textures/rush2049dc` at the disc's size, as
  `<disc file>/<texture name>.png`, damaged car textures left out.
- The images are unpadded so they are clean to edit.
- It writes each folder's `names.txt` (path -> key) and `hashes.txt` (key -> the catalog's RT64 hash), and
  `textures/README.txt`.
- `RUSH2_TEXTURE_DUMP=<seconds>` presses it that long after boot (for tests), then Install with
  `RUSH2_TEXTURE_INSTALL=1`.

**Install Upscaled Textures** reads `user_texture_pack/<the same folders>` with their subfolders, plus the top of it,
where dumps from before the per-game folders went.
- Any size and any format stb_image reads are accepted.
- A file name only needs to start with the dumped image's name (or, for an image without one, its key), so suffixes
  such as Gigapixel's are fine.
- **Flat folders:** the folders inside a game folder are optional. A file whose path matches no dumped path is matched
  by its file name alone, the longest dumped file name it starts with (`by_file` in `install`). Dumped file names are
  unique in a game folder for this (docs/asset_catalog.md, Names), so an upscaler that writes a whole game's images
  into one folder (Gigapixel) still installs: `user_texture_pack/rush2/1STPLACE~pause_2-gigapixel.png` is
  `hud/pause_2/1STPLACE~pause_2`. Checked on a copy of a real folder with 1,948 flat `rush2` upscales: every one
  matched a name, and Install packed 1,407 `rush2` textures. The rest are car paint bases, installed through their
  paint jobs, and copies the upscaler made of names that used to repeat (`1STPLACE-2`).
- Upscales of the disc's images in `user_texture_pack/rush2049dc` go to the Dreamcast Textures instead
  (`generated_texture_cache/dreamcast`), drawn wherever the disc's images are.

For the rest it then:
1. Restores alpha from the dumped original (`textures/`), and lists the image under its RT64 hashes and its content
   key (for the derived replacements, which find a texture's upscale by either).
2. Builds the pack in memory (`ZipWriter`): each image as a DDS with mips, plus `rt64.json` and `mod.json`, written to
   `mods/rush2_custom_textures.rtz.new`.
3. Swaps it in (`replace_pack`). Windows can't replace an open file, and two things hold the old pack: RT64
   memory-maps it and the mod context keeps it open. So it has the renderer drop the pack
   (`secondary_disable_texture_pack`), closes the mods (`close_mods`), renames the new file over the old one (retrying
   for up to 5 s while the renderer lets go), and turns the pack back on (`secondary_enable_texture_pack`). With no
   upscales left, the old pack is deleted instead.
4. Rescans the mods, enables the pack and refreshes the textures.

RT64 used to keep a zip pack mapped after its replacements were cleared: `ReplacementMap::clear` now also drops the
pack's file systems (lib/patches/rt64.patch). During the swap RT64 may log "Failed to identify" for the pack once.
`user_texture_pack` holds the player's source images; `mods/rush2_custom_textures.rtz` is the built pack RT64 loads.

## Derived replacements

A texture pack replaces textures by their RT64 hash, and the dump has the hash of every texture the ROMs hold. What the
game draws besides those is always one of them changed at run time (docs/asset_catalog.md, Textures made at run time):
another palette, stripes stamped over, a copy elsewhere in RDRAM, a strip of a big image, a tile taller than its load.
`rush2::derive` (src/texture_derive.cpp) gives each of those a replacement made from its source's upscale, live, so a
pack of the dumped images replaces everything the game draws.

- **Packs:** every enabled mod with an `rt64.json` (`.rtz` or folder), read at the first 3D draw and again after Install
  (`packs_changed`). Images are found by RT64 hash or by content key (Install lists both). A zip is opened only while a
  batch reads it, as Windows can't replace an open .rtz.
- **Index:** at the first 3D draw, the catalog's textures (rush2::catalog, the same as the dump: 11,000 with all three
  ROMs and a disc) and their row pieces (8 texels of palette indices, or colors, keyed with the width and x), about
  800,000, built in under a second. It is built again when the game's assets change (`rush2::assets::generation`) and a
  texture didn't trace.
- **Tracing** a texture drawn that no pack replaces by its hash (once per hash, 3D and 2D, on a worker thread):
  1. its content key, then its palette indices (`index_key`);
  2. else the base and row offset most of its row pieces agree on (at least a quarter of them): stamped panels, copies,
     strips, taller tiles.
- **Making the replacement** from the base's upscale (scaled to a whole multiple of the base's size):
  - Exact rows of the base (the same image, or a strip of a big one): the base's upscale as the region of it the
    texture is (`RT64::ReplacementRegion`), uploaded once for every strip.
  - A Rush 2 car panel with stripes (`derive_stripes`): the stripe tile and threshold (15, 7, 4 or 3 by detail level)
    whose stamp covers the changed texels, each palette index's paint and stripe colors from the drawn texels, the
    panel's upscale recolored to each, blended by the stripe tile's upscale (its own when a pack has one), so the
    stripes' edges come from the stripe tile's upscale.
  - Anything else: the base's rows recolored to the drawn colors (`recolor`), rows past the base's ends from the drawn
    texels scaled up.
- **Precedence:** a pack's own image for the hash wins (RT64 prefers packs over live replacements). The upscaling modes
  leave derived hashes alone (`owns`).
- **Interface:** with Upscale Interface off, a texture drawn in 2D, or traced to an interface-only base, gets nothing
  (outcome `Interface`). Changing the option derives everything again. A mode change clears every live replacement, so
  it has the derived ones made again too (`packs_changed`).

**Texture census** (a test aid): `RUSH2_TEXTURE_CENSUS=<file>` records every texture drawn (3D and 2D) with the bytes
it was loaded from, and at `RUSH2_TEXTURE_DUMP`'s time writes, instead of dumping, the ones the catalog can't name by
key or indices with where their bytes sit in the containers (`tools/texture_census.py OUT STEP...` races one track per
track select step). With a pack enabled each line also says what became of the texture: pack, derived, no upscale of
its base, not traced, interface (left alone), or too small or flat (never dumped, `worth_upscaling`), with totals on
the last line.

**Checked [V, 2026-10-11]:** with `tools/fake_upscales.py --tint --flat` (a magenta-tinted stand-in for every dumped
texture, saved flat) installed and Upscale Interface off, a race drew 1,110 textures: 455 replaced by the pack, 283
derived, 164 interface left alone, 208 too small or flat, none untraced or missing an upscale. The screenshots show
every 3D surface tinted and the HUD as it was. Before Upscale Interface, six tracks of all three games came out the
same (pack or derived for every texture worth upscaling).

The dump waits for the Dreamcast disc's images when it names 2049 textures after them (`images_ready`): a dump made
before they were built named 455 textures differently, and upscales of one didn't install with the other.

## Caveats

- UI detection goes by projection type and the catalog's interface containers. Interface art outside them drawn with a
  perspective projection would be upscaled; add its key to `ui_textures.txt`.
- In this container, only the parts below have been built and tested; the full Windows build hasn't been run.
  - Built: the image code (`tools/texture_upscale_test.cpp`: decoding, hqx, recoloring, the garbage check, DDS,
    cancelling a command, and pack files).
  - Compile-checked: the RT64 changes.
