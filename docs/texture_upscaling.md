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

- At startup, the recomp deletes the files the ESRGAN modes left: `texture_upscale/realesrgan` and the `esrgan_*`
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
  never upscaled. That covers the HUD, menus, fonts and 2D images.
  - If it was upscaled already, its live replacement is removed.
  - Its content key is added to `texture_upscale/ui_textures.txt` so later sessions skip it as well.
  - Adding a key to that file by hand (the first 16 characters of a dumped file name) excludes a texture.
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

The buttons are Dump Textures, Open Dump Folder and Install Upscaled, in a row under the "Your Own Upscales" heading.
The row is a `rush2::ui::FocusRow` (`OptionsPage::add_row`), which scrolls into view when a button in it takes focus,
so the controller can reach the buttons.

**Dump Textures** runs in the background (the note under the heading shows its progress). It writes every kept 3D
texture of the session that isn't already in the folder as `texture_upscale/dump/<game>/<key>.png`, in a folder per
game the texture was loaded from (`rush2`, `sfrush`, `rush2049`; `rush2::origin`, docs/rush2049_research/dreamcast.md
Texture origin), and the Rush 2049 Dreamcast disc's images drawn in the session into `dump/rush2049dc`, at the disc's
size, damaged car textures left out: with the disc as the source every image it drew, with the N64 ROM (and Dreamcast
Textures on) the images matched to N64 textures. The images are unpadded so they are clean to edit.
It also merges each folder's `hashes.txt`, which maps each key to the RT64 hashes it was drawn under, and writes
`dump/README.txt`. Dumps from several sessions add up. Car paint variants are dumped as separate images; a pack only
replaces the variants it has. `RUSH2_TEXTURE_DUMP=<seconds>` presses it that long after boot (for tests).

**Install Upscaled** reads `texture_upscale/upscaled/<the same folders>` (and the top of `upscaled`, where dumps from
before the per-game folders went). Any size and any format stb_image reads are accepted, and names only need to start
with the key, so suffixes such as Gigapixel's are fine. Upscales in `upscaled/rush2049dc` of the disc's images go to the
Dreamcast Textures instead (`texture_upscale/dreamcast`), drawn wherever the disc's images are. For the rest it then:

1. Restores alpha from the dumped original.
2. Writes each image as a DDS with mips into the folder mod `mods/rush2_custom_textures`, along with `rt64.json` and
   `mod.json`.
3. Rescans the mods and enables the pack.
4. Writes `texture_upscale/rush2_custom_textures.rtz` as a copy to share.

The pack is a folder rather than an .rtz so that installing again can't fail on a pack file RT64 has open on Windows.

## Caveats

- UI detection goes by projection type. A 2D element drawn with a perspective projection would be upscaled; add its
  key to `ui_textures.txt`.
- In this container, only the parts below have been built and tested; the full Windows build hasn't been run.
  - Built: the image code (`tools/texture_upscale_test.cpp`: decoding, hqx, recoloring, the garbage check, DDS,
    cancelling a command, and pack files).
  - Compile-checked: the RT64 changes.
