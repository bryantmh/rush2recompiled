# Texture upscaling

Settings > Graphics > Texture Upscaling redraws the game's 3D textures at a higher resolution while the game runs. The
same section has buttons to dump the textures for any other upscaler and to install the results as a texture pack.
The default is Off, which keeps the original game's look.

Code:
- `src/texture_upscale.cpp`: the observer, the worker, the options and the buttons.
- `src/texture_upscale_images.cpp`: image work with no renderer or UI dependencies (decoding, recoloring, padding,
  hqx, DDS, zips, downloads, running programs).
- `include/texture_upscale.h`
- `lib/hqx`: hq2x and hq4x, vendored (LGPL 2.1+, see its README.md for the changes).
- The RT64 side is in `lib/patches/rt64.patch`: `src/common/rt64_live_textures.h`, a hook in
  `State::fullSyncFramebufferPairTiles` (`rt64_state.cpp`), and the live replacement layer in `rt64_texture_cache.cpp`.
- `tools/texture_upscale_test.cpp` tests the image code. Build instructions are in its header comment.

## Modes

| Mode | What runs | Cache folder |
|---|---|---|
| HQ2x, HQ4x | hqx on the worker thread (CPU, about a millisecond per texture) | `hq2x`, `hq4x` |
| ESRGAN 2x | Real-ESRGAN `realesr-animevideov3` x4, averaged down to 2x | `esrgan_x2_from_x4` |
| ESRGAN 4x | Real-ESRGAN `realesr-animevideov3` x4 | `esrgan_x4` |
| Custom | the Custom Upscaler Command option, on a folder of images | `custom` |

Real-ESRGAN details:

- **Release:** the ncnn-vulkan build from Real-ESRGAN v0.2.5.0 (BSD-3-Clause).
- **Download:** only the files that are used are downloaded, about 7.5 MB: the program, `vcomp140.dll` on Windows,
  and the x4 model's `.bin` and `.param`.
  - `download_zip_files` reads the release zip through ranged requests with the system's curl. miniz reads the
    archive through a callback that fetches 256 KB blocks.
  - If the server won't serve ranges, the whole zip (45 MB) is fetched and only those files are extracted.
- **Why no x2 model:** on at least one user's GPU, the x2 model produced noise. Averaging the x4 output down also
  looks better.
- **Garbage check:** `matches_original` averages each output back down to the original's size. If it doesn't match,
  that texture uses HQx at the same scale instead, and the settings page says so.
- **Pacing:** runs take 16 textures at a time (`-j 1:1:1`) and pause 250 ms between runs, so the game gets the GPU
  back.
- **Mode changes:** changing the mode kills a running upscaler (`run_command`'s cancel flag).
- **Old cache:** the old `cache/esrgan_x2` folder, made by the x2 model, is deleted at startup.

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
of 0.2 to 0.4 for HQx and 5 for ESRGAN x4. ESRGAN itself treats red and blue slightly differently; the unrecolored
red upscale is 64 to 66 away.

This covers any texture the game recolors through its palette, not only cars. Rush 2's stamped stripes change the
indices, so each stripe pattern is its own base.

## Live replacements (RT64 patch)

`RT64::addLiveReplacement(hash, contentKey, fileBytes)` gives the running texture cache a replacement image without a
texture pack. Its behavior:

- **Upload:** images are uploaded once per content key on the texture cache's upload thread, the way RT64 loads a
  pack texture with the `stall` operation. They are transitioned to shader read like stream results. A batch takes
  at most 8, because the game's thread waits on every batch that also carries its own TMEM uploads.
- **Replacing:** each hash then gets `TextureMap::replace` with a half-texel shift, the default for packs.
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

## Your own upscales (Dump / Install)

The buttons are Dump Textures, Open Dump Folder and Install Upscaled, in a row under the "Your Own Upscales" heading.

**Dump Textures** writes every kept 3D texture of the session that isn't already in the folder as
`texture_upscale/dump/<key>.png`. The images are unpadded so they are clean to edit. It also merges `dump/hashes.txt`,
which maps each key to the RT64 hashes it was drawn under, and writes `dump/README.txt`. Dumps from several sessions
add up. Car paint variants are dumped as separate images; a pack only replaces the variants it has.

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
- In this container, only the parts below have been built and tested; the full Windows build hasn't been run.
  - Built: the image code (`tools/texture_upscale_test.cpp`: decoding, hqx, recoloring, the garbage check, DDS,
    cancelling a command, a real ESRGAN run on lavapipe, the partial download against GitHub, and pack files).
  - Compile-checked: the RT64 changes.
