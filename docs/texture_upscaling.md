# Texture upscaling

Settings > Graphics > Texture Upscaling redraws the game's 3D textures at a higher resolution with an AI upscaler
while the game runs. The same section has buttons to dump the textures for any other upscaler and to install the
results as a texture pack. The default is Off, which keeps the original game's look.

Code:
- `src/texture_upscale.cpp`: the observer, the worker, the options and the buttons.
- `src/texture_upscale_images.cpp`: image work with no renderer or UI dependencies (decoding, padding, DDS, zips,
  running programs).
- `include/texture_upscale.h`
- The RT64 side is in `lib/patches/rt64.patch`: `src/common/rt64_live_textures.h`, a hook in
  `State::fullSyncFramebufferPairTiles` (`rt64_state.cpp`), and the live replacement layer in `rt64_texture_cache.cpp`.

## Picking textures

RT64 calls `RT64::TextureObserver::textureDrawn` for every tile a draw call samples from TMEM. That excludes
framebuffer copies, raw TMEM fallbacks and extended-GBI replacement IDs. The call passes the texture's replacement
hash, the TMEM contents, the tile, the sample size, the TLUT mode, and whether the draw's projection is
`Projection::Type::Perspective`.

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

## Automatic upscaling

A worker thread takes kept textures in batches of up to 48 and processes them in this order:

1. Writes each texture as a PNG padded by 8 texels to `texture_upscale/work/in`. The padding follows the tile's edge
   mode (wrap, mirror or clamp from cms/cmt and the masks), so the upscaler sees the neighbors the texture really has
   and repeating textures don't get seams.
2. Runs the upscaler on the folder, below normal priority and without a console window.
3. Crops the padding off (`unpad` works the scale out from the output size).
4. Restores alpha with `restore_alpha`:
   - It puts back the original's alpha, scaled up, when the upscaler dropped it.
   - It keeps cutout textures (alpha only 0 or 255) as cutouts.
5. Stores the result as `texture_upscale/cache/<esrgan_x2|esrgan_x4|custom>/<key>.dds`, an RGBA8 DDS with a full mip
   chain. The mips use the alpha-weighted box filter RT64 uses for the mipmaps it generates (`TextureDecodeCS.hlsl`),
   so upscaled textures blend down with distance like the originals under Distant Textures: Smooth.

Anything already in the cache is loaded without running the upscaler, so nothing is upscaled twice, across sessions
too. Changing the mode clears the live replacements and requeues every kept texture for the new upscaler's cache
folder.

Upscalers:

- **ESRGAN 2x and ESRGAN 4x:** Real-ESRGAN ncnn-vulkan, release v0.2.5.0 (BSD-3-Clause), model
  `realesr-animevideov3`. It is downloaded on first use with the system's `curl`, from the release zip for the
  platform, and extracted with miniz into `texture_upscale/realesrgan`.
  - Windows: `realesrgan-ncnn-vulkan-20220424-windows.zip`, 45,474,481 bytes.
  - Linux: `realesrgan-ncnn-vulkan-20220424-ubuntu.zip`, 46,931,474 bytes.
  - It keeps RGBA images' alpha.
  - Tested in a Linux container on Mesa's lavapipe (software Vulkan).
- **Custom:** runs the Custom Upscaler Command option. `{input}` and `{output}` are replaced with the quoted work
  folders. The output can be any integer scale, and file names only need to start with the input's 16-character key.

## Live replacements (RT64 patch)

`RT64::addLiveReplacement(hash, contentKey, fileBytes)` gives the running texture cache a replacement image without a
texture pack. Its behavior:

- **Upload:** images are uploaded once per content key on the texture cache's upload thread, the way RT64 loads a
  pack texture with the `stall` operation. They are transitioned to shader read like stream results.
- **Replacing:** each hash then gets `TextureMap::replace` with a half-texel shift, the default for packs.
  - Hashes that load later are replaced as their upload finishes.
  - A pack reload (`clearReplacementDirectories`) queues every live hash to be applied again.
- **Packs win:** a loaded pack that replaces the same hash takes priority (`packReplaces`), and its streamed texture
  replaces the live one when it arrives.
- **Removal:** `removeLiveReplacement` calls `TextureMap::unreplace` and frees the texture through `evictedTextures`
  once nothing uses it.

Nothing is written to a pack folder, so upscaled textures appear without the stall of reloading every pack. The
recomp gives each mode its own content keys (`live_key`), so a result that lands after the mode changes can't stand in
for the new mode's image.

## Your own upscales (Dump / Install)

**Dump Textures** writes every kept 3D texture of the session that isn't already in the folder as
`texture_upscale/dump/<key>.png`. The images are unpadded so they are clean to edit. It also:

- merges `dump/hashes.txt`, which maps each key to the RT64 hashes it was drawn under;
- writes `dump/README.txt`;
- opens the folder.

Dumps from several sessions add up.

**Install Upscaled** reads `texture_upscale/upscaled`. Any size and any format stb_image reads are accepted, and
names only need to start with the key, so suffixes such as Gigapixel's are fine. It then:

1. Restores alpha from the dumped original.
2. Writes each image as a DDS with mips into the folder mod `mods/rush2_custom_textures`, along with `rt64.json` and
   `mod.json`.
3. Rescans the mods and enables the pack.
4. Writes `texture_upscale/rush2_custom_textures.rtz` as a copy to share.

The pack is a folder rather than an .rtz so that installing again can't fail on a pack file RT64 has open on Windows.

## Caveats

- UI detection goes by projection type. A 2D element drawn with a perspective projection would be upscaled; add its
  key to `ui_textures.txt`.
- Car paint textures are CI with the paint written into the palette, so each paint combination on screen is its own
  texture and is upscaled when it first appears.
- In this container, only the parts below have been built and tested; the full Windows build hasn't been run.
  - Built: the image code (`tmp/` harness: TMEM decode of RGBA16 with odd-row swaps, CI4 with TLUT, pad and unpad,
    DDS mip layout, the real ESRGAN run, zip and pack files).
  - Compile-checked: the RT64 changes.
