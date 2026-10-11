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

### Car paint bases (paint_preview)

A car body texture has no palette in its file (the game writes it: the paint). `rush2::catalog::paint_ramps` picks
these textures (painted CI8, siz 1, in a `cars/` container) and gives their palette's paint ramps:
- **Rush 2's cars:** 8 ramps of 32, MAIN COLOR 1-31 and ACCENT COLOR 33-63 painted, the rest fixed colors or stripe
  targets (`car_palette_classify_800854AC`, the same table for every car, the DEW's included).
- **Rush 2049's cars** (assets 0x71+, Rush2049 containers): the car's own palette (CnPALETTE) with COLOR 1-3 (MAIN,
  ACCENT, STRIPE COLOR) at 32-63, 64-95 and 96-126, as `rush2_car49_paint` (src/rush2049/car2049.cpp) builds them:
  the paint, then each entry 8/256 nearer black. A Dreamcast disc's car has paint jobs, not ramps: no preview.

**The catalog's image** is gray: entry i as the 5-bit gray i & 31 (docs/asset_catalog.md). Keys, names and packs go by
it. CARPALETTE's ramps run from light at entry 1 (230) to black at 30-31, so that image is a negative of the paint
(light paint dark, black trim (191) white, the glass (0) gray 0), and MAIN, ACCENT and the fixed colors share the grays.
An upscale of it can't say where MAIN ends and ACCENT begins: the game's paint borders fell back to the N64 texels, a
staircase past the player's smooth outlines (a user's Hotrod flames, a thin accent line).

**Dump Textures [V, 2026-10-11]** writes `rush2::catalog::paint_preview` for these instead: the first ramp violet
(160, 80, 255), the second yellow (255, 215, 40), the third cyan (40, 220, 230), shaded as the game shades a paint, the
fixed colors as they are, opaque. No fixed entry of CARPALETTE has those hues (they are grays, reds, oranges, greens and
the glass's dark navy; a blue MAIN was tried first and its darker shades matched the glass).
`paint_preview_palette(containers, car)` builds it per car container: Rush 2's from CARPALETTE in the cars/stripes
container (asset 0x1C), each ramp entry the paint times CARPALETTE's gray over 230; a 2049 car's from its own palette
(`rush2::car2049::palette`). The dump's README tells players what the colors mean. Install is unchanged: an upscale is
still listed under the gray image's key.

**Replacements:** `Base::preview` holds the same preview, and `upscaled_from` gives it as the image a paint base's
upscale was made from (the gray image for other bases). `Deriver::recolor` runs `recolor` from the preview, whose
colors tell the paints apart, so the borders follow the upscale. With `brightness_detail`: what the blend of preview
colors doesn't explain is added as brightness, the same on every channel. An upscaler drifts hues a little where
colors meet (a user's Euro LX panels: 6-7 of 255 off the preview); added per channel, as for other palette variants,
that drift came out as pink, green and cyan tints on a white car, scaled up by the paint being brighter than the
preview's violet. Pixels over fixed colors (lights, glass, trim) pass through `recolor` as drawn, so an upscaler's color
spill there (green and pink headlights beside violet paint on a user's Panther C13_64_8) came through too: **Keep Car
Colors** (Settings > Graphics > Textures, on by default, takes effect at once through `set_keep_car_colors`, which
derives everything again) runs `rush2::upscale::clamp_chroma` on every car paint replacement: each pixel keeps its
brightness, and its chroma (each channel minus the brightness) is held within 3 levels of the range the drawn
texture's 3x3 texels around it have. Pixels over paint are blends of their texels with gray detail, so only colors no
nearby texel has are changed. Off, deliberate recolors of lights or trim come through.

Upscales of the gray image (dumps before the preview) aren't read as such: the feature shipped with the preview, so no
pack has them. `Deriver::recolor` serves `derive` (same-size matches with the drawn indices) and both recolors in
`derive_stripes`.

**Checked [V, 2026-10-11]:** in a copy of the build, a race with the Concept and the user's gray pack drew it painted;
after Dump, nearest-neighbor 2x upscales of the 506 Rush 2 car previews and Install, the Concept drew in its own yellow,
not the preview's colors (census: 98 derived, none untraced). Offline, `recolor` from a Lanczos upscale of the Hotrod
HOT_D0_1 preview came within a mean error of 2.1 of the painted panel upscaled directly. The 2049 cars likewise:
nearest-neighbor 2x upscales of their 559 previews installed, the 8-BALL raced in its own yellow and gray (census: no
2049 car texture left unreplaced).

Tests: `tools/texture_upscale_test.cpp` (a synthetic preview panel: `recolor` with `brightness_detail` within 0.11
mean error and no fringe pixels; a spill of green into a fixed color is held to 2 levels by `clamp_chroma`), and real
panels with `tools/rush1/paint_case.py CAR TEXTURE DIR` (preview and painted) then `upscale_test --paint DIR
UPSCALE.png`, which writes the recolor with and without the clamp.

### Half-body sheets

Every car's body is drawn from several tiles, each used on both sides of the car:
- **Rush 2049:** the model named *FRAME1 (first LOD), 15-25 small tiles (C13_64_1..., mostly 32x16 CI8: the N64's
  texture memory holds one at a time);
- **Rush 2:** five parts, `<car>D0_FL1`, `FR1`, `RL1`, `RR1` and `TOP1` (`body_part`; `D0_D...` are the crumpled ones,
  `D2_` the farther set, FRAME1 the whole car far away), in the car's space so their edges meet, with 5-7 64x32 panels
  (BMD0_1..., HOT_D0_1...). GT90's parts draw 16x8 stand-ins carried in the models, but in a race the game draws the
  full GT_D0_n panels (texture census, 2026-10-11): `build` swaps each stand-in for the panel it is a point sample of
  (texel offset 0-3, under 30% of texels unlike). DEW's parts carry full-size textures of their own. An
upscale of each tile alone is cut off at the tile's edge, so the body looked disjointed wherever two tiles met.
`rush2::sheet` (include/body_sheet.h, src/body_sheet.cpp) lays one side's tiles out as one image the way they join on
the body, and Install cuts an upscale of it back into the tiles.

**build**, from the car container read with geometry:
- pieces: runs of connected triangles drawing one tile; a piece's `shown` texels are the ones its triangles cover plus
  a 1-texel rim (`core` without it);
- every other model at every distance drawn with the body's tiles adds pieces too (Rush 2's hood and light parts, the
  far car), after the body's: a texel the car draws anywhere should be on the sheet. A triangle running past a clamped
  tile's edges counts (it draws the edge texels), one past a repeating tile's doesn't;
- stand-ins (GT90) map to a panel only when no model draws that panel itself: BMW's BMD0_1_4 is a real far texture;
- the mirror axis is found from the mesh; one side's pieces are kept, and a piece of the other side only when it shows
  a quarter of its texels or 6 that no kept piece shows (another model's piece: any);
- UVs first move half a texel: the RDP samples a texel at its whole coordinate, and RT64 shifts a replacement's
  coordinates by half a texel to match (lib/rt64 TextureSampler.hlsli), so texel k spans UV k - 0.5 to k + 0.5 while
  the sheet's texel k spans k to k + 1. Without the shift every turned or flipped join was a texel off;
- where two pieces share a mesh edge they vote for how one sits relative to the other: one of the 8 quarter turns and
  flips (`orient`, matrices in body_sheet.cpp) and an offset. A straight edge fits a turn and its mirror alike; the
  right one puts the two triangles on opposite sides of the edge, as on the body (before this check, ties went to
  whichever came first and neighbors could land mirrored). Each pair keeps the turn and offset most edges agree on
  (pieces can touch in two places that disagree) and the mean offset of the edges within a texel of it;
- an edge of another length in the two tiles' texels can't be joined: the art changes scale there (20 to 110 such
  edges a car), and the sheet keeps the original texels, so those seams stay seams;
- islands are grown greedily from the best-voted joins, carrying exact offsets and rounding each piece's position once
  (rounding each against its rounded neighbor added up along a chain), refusing a join whose pieces would overlap by
  more than max(2, size / 12) texels or that disagrees by more than a texel with a neighbor already placed (a loop the
  flat sheet can't close), then shelf-packed with a 2-texel gutter. A tile used by several pieces gives each texel back
  from the first piece showing it (`owned`; the kept side first, larger pieces first). A texel's center maps to
  `floor(M * (x + 0.5, y + 0.5) + t)`;
- the dump log's `[sheet]` line per sheet gives the joined edges and how far their two sides land apart (mean, worst):
  what an upscale shows as a step. The game's own UVs put many neighbors a fraction of a texel apart, which a sheet of
  whole texels can only round, so up to half a texel is expected; a worst of several texels is an edge where two
  pieces touch a second time with another offset.

**render** draws rims, then cores (so a core wins over a neighbor's rim), then fills the gaps ring by ring, each empty
texel the average of its filled neighbors, so an upscaler sees no hard borders. **cut** takes, for each texel of a tile, the scaled block of the sheet
upscale under the piece that owns it (rotated back), texels no piece owns from the tile's own upscale if the user made
one, else the tile scaled up.

**Dump** (`write_sheets` in src/texture_upscale.cpp) writes `cars/<CAR>/<CAR>_BODY.png` for each car of both games,
and `<CAR>_BODY_D1.png` with the same layout over the damaged tiles (Rush 2049's `<tile>_D1`, Rush 2's `...D1_n` for
`...D0_n`; pieces of tiles without one, like glass, left out), plus
`body_sheets.txt` in the game folder (`sheet <path> <w> <h>`, then a `piece <tile> <tw> <th> <orient> <x> <y> <shown>
<core> <owned>` line per piece, masks run-length coded starting with a run of 0s). `build` also gives every tile's
texels the car draws (`drawn_masks`: any model, any distance; Rush 2049's `_D1` tiles are swapped in where their tile
is drawn, so they take its mask), and a tile its sheet gives all of those back (`covers`) moves from the dump to
`asset_dump/sheet_tiles`: upscaling it alone would change nothing. 669 of 700 car tiles move; the rest have texels
only another use shows (a track texture a car borrows, repeating). `read_original` looks there too. **Install**
(`install_sheets`) reads
it first: an upscale named after a sheet (longest path or file name wins, so `_BODY_D1` isn't taken for `_BODY`) is
scaled to a whole multiple of the sheet if it isn't one, and cut into its tiles' upscales, which then go in like any
other (the paint ones recolored as above). Sheet files are skipped in the per-file pass.

**Checked [V, 2026-10-11], Rush 2:** all 22 cars got sheets (BMW 8 pieces of 6 tiles 116x81, GT90 127x133 after the
stand-in swap, Hotrod 18 pieces of 5 tiles), 67 sheets with the damaged ones. With the same striped 4x upscales,
Install cut 666 tiles; on the car select every Rush 2 car showed the lines running across its panels, the paint and
stamped stripes still recolored. DEW's showed only in a race: the car select draws its own copy of DEW's textures,
which the census takes for car select art.

**Checked [V, 2026-10-11], Rush 2049:** all 13 N64 2049 cars got sheets (Panther 13 pieces of 13 tiles 192x83, Magnum
160x245, Crusher 18 pieces of 12 tiles). In a copy of the build, 4x upscales of the sheets with white diagonal lines
drawn across them installed and cut correctly. The first layout's lines broke at many seams with small steps (a user
spotted it): mirrored joins, the missing half-texel shift and rounding that added up, all fixed above. After the fixes
the lines run on across joined seams (Rocket's long side, Prototype's flank); they still step or turn where the
neighbors' art is at another scale or a fraction of a texel off in the game's UVs, and at island edges (`[sheet]`
lines: 0.07 to 1.6 texels mean step on joined edges).

### SF Rush decals

The SF RUSH stripe (docs/rush1_research.md section 11) writes Rush 1's decal into a Rush 2 car's panel texels at run
time, so it is in no file. The decal is handled the way Rush 2's stripe tiles are: an image of the decal alone,
upscaled on its own and laid over the plain body's upscale. `rush2::car1stripes::decal_textures`
(src/rush1/car1_stripes.cpp) makes the images from the same disk cache the game uses (stripe_cache, keyed on the
Rush 1 ROM and the build). Each image is transparent off the decal:

- **Fixed-color decals** (Camaro, Taxi) write their palette indices over the panel's. The image shows the decal in its
  own colors (paint ramp entries in the preview colors).
- **STRIPE COLOR decals** (Bus, Bug, Bugatti): the game remaps the panel texel to the stripe color's shade. The image
  is flat cyan, since only its shape is used.
- **Key:** the image's content key XOR `0x5346525553484443`, so it never equals a drawn image's key. The clean and
  damaged panels draw the same decal, so they share the image, and Dump Textures writes it once.

Dump Textures writes each one beside its panel as `<panel>_SFRUSH`, with the sheet `<CAR>_BODY_SFRUSH` laid out as the
body (the `variant` lambda in `write_sheets`). `rush2::sheet::render` keeps a mostly clear layer's alpha and leaves
the space between its pieces clear; the body's sheets stay opaque.

In the deriver each panel's decal is a base (`Base::plain`, `decal_mask`, `decal_stripe`) whose `indices` are the panel
with the decal written in, so a fixed-color decal panel traces to it directly. `find_decal` finds the rest: the plain
panel with only the decal's texels changed. Rush 2's own stripes, which change other texels, keep the plain base.
`derive_decal` then:

- recolors the plain panel's upscale to the paint;
- recolors the decal's color layer: a STRIPE COLOR decal takes the plain panel's upscale in the stripe shades; a
  fixed-color decal takes its own upscale in the colors drawn, with the alpha its recolor mixed in at the edges divided
  back out;
- blends the decal layer over the paint by the decal upscale's alpha.

In built-in HQ the decals are padded by clamping, like the stripe tiles.

**Checked [V, 2026-10-11]:**

- The decal sheets for CAMARO, BUGAT, VWBUS, VWBUG and TAXI are clear off the decal.
- In HQ4x the car select's Taxi in SF RUSH showed smooth checkers over the body.
- Before the decal-only layer, a pack's test upscale of the full decal panels showed on the Taxi and Van.

### Built-in HQ for cars

With HQ2x or HQ4x and no pack image for a car texture, the upscaling mode leaves the car's textures to the deriver
(`rush2::derive::builtin_car`, checked in `take_jobs`). `builtin_upscale` makes the upscale a pack would give, so
built-in mode gets the same paint recolor, Keep Car Colors, stripes and SF Rush decals:

- **Sheets:** `sheets_of(car)` lays out the car's sheets as Dump Textures does (body, damaged, decal, damaged decal).
  Each is kept on disk as the track_cache blob `body_sheets_<file>`, keyed on the car file's hash.
- **Upscale:** the sheet holding the tile is rendered from the tiles' previews, padded (clamp), run through hqx and
  cut into every tile at once. A tile the cut refuses, or a texture on no sheet, is upscaled alone.
- **Cache:** each result goes on disk as the blob `hq<scale>_<key>` (key | 1), so later boots skip hqx. Blob names
  can't hold a `/`.
- **Mode changes:** `set_mode` calls `packs_changed` so a mode change derives again.
- **Stripe tiles** (cars/stripes) are padded by clamping, not by their own wrap: a stripe tile lies on its panel
  texel for texel. Wrapped, hqx blended a stripe's opaque edge with the tile's clear far side (alpha 191 at the edge),
  and `derive_stripes`, which takes the stripe only at full alpha, drew a line of paint down the middle of every fat
  stripe where two mirrored panels meet. Their blobs are keyed apart so the old ones aren't reused.

**Checked [V, 2026-10-11]:** HQ4x with no pack smoothed both games' cars on the car select. A second launch loaded the
83 cached blobs without rewriting them. hqx rounds single-texel corners but leaves runs of two or three texels as
steps (the Hotrod's flames, the Rocket's stripes). That is hqx on 64x32 art, not the sheets: the cached upscale shows
the same steps as the source.

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
3. Swaps it in (`replace_pack`). Windows can't replace an open file, and three things hold the old pack: RT64
   memory-maps it, the mod context keeps it open, and the derived replacements read car upscales from it during a
   batch. So it has the renderer drop the pack (`secondary_disable_texture_pack`), closes the mods (`close_mods`),
   takes `rush2::derive::hold_packs()` (closes the deriver's handles and keeps them shut until the swap is done),
   renames the new file over the old one (retrying for up to 10 s while the renderer lets go), and turns the pack back
   on (`secondary_enable_texture_pack`). With no upscales left, the old pack is deleted instead. A failure shows its
   reason on the button ("Install Failed: the old pack is in use"). Before the hold, an Install while the game was
   deriving a race's cars (seconds with a full car pack) failed after the 5 s it retried then.
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

- **Speed [V, 2026-10-11]:** a car's paint change brings 20-odd new panel textures at once, drawn native until their
  replacements arrive (visible pop-in on the car select). `recolor` is split into `plan_recolor` (per upscaled pixel,
  the two old texels and the blend that explain it: 45 pairs searched, about 7 ms for a 4x 64x32 panel) and
  `apply_recolor` (the mix in new colors, about 1.5 ms); the plan depends only on the upscale and the old colors, so
  `Deriver::plans` keeps one per base (shared_ptr, 256 at most, cleared on reload) and another paint of a panel only
  applies it. Each batch matches its textures one by one (the index and packs aren't thread-safe), then recolors,
  clamps and encodes them on every core (`cache_mutex` guards `upscales` and `plans`), then hands them to RT64 in order.
  Measured on the car select: a car's first view 115-145 ms of work became 22-30 ms, a repaint of its panels about 3 ms.
- **Latency [V, 2026-10-11]:** what showed as pop-in was mostly plumbing, not work:
  - **Handed over at first sight.** A 3D texture now goes to the deriver the first time it is drawn (`Seen::derived`
    in `textureDrawn`). Before, it went only after `stable_frames` (three frames, 50-100 ms), the upscaling mode's
    rule for its own textures, which still applies.
  - **Loads in parallel.** A batch traces its textures, then loads the distinct bases' upscales on every core (disk
    blobs, pack images to decode, a car's sheets), then recolors. `base_upscale` locks `cache_mutex` only around the
    map and `pack_mutex` only while reading a zip; `car_locks` keep a car's sheets to one thread.
    `track_cache::load_blob` reads a file in one call, where a character iterator took milliseconds.
  - **Plan search split.** The cores the jobs leave free split each plan search by rows (`plan_threads`), so a lone
    texture's 7 ms search takes about 4 ms.
  - **Measured on the car select, caches warm:** a repaint takes about 4.5 ms from first draw to RT64, a car change
    11-15 ms, the first car after boot about 31 ms (115 ms before). RT64 shows a live replacement from the next
    frame, so a repaint draws the native texture for one frame at most.
  - **Not done:** stalling the frame until the replacement is ready (the display list thread or `waitForGPUUploads`),
    which would turn pop-in into stutter.
  - **Timing:** `RUSH2_DERIVE_TIMING=1` prints each batch's time: waited, matched, derived, handed.
- **Packs:** every enabled mod with an `rt64.json` (`.rtz` or folder), read at the first 3D draw and again after Install
  (`packs_changed`), and whenever a pack is enabled, disabled or reordered in the Mods menu (the texture pack content
  type's callbacks in src/main.cpp; before that, toggling a pack changed only the textures it replaces by hash, so a
  Rush 2 car's repainted panels kept whatever they had at the first draw). Images are found by RT64 hash or by content
  key (Install lists both). A zip is opened only while a
  batch reads it, as Windows can't replace an open .rtz; `hold_packs` (Install) closes it mid-batch and blocks the
  next read until the swap is done.
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
- **Car model mods** (docs/car_mods.md): a mod car's stand-in textures are registered with `set_mod_textures` by
  their `index_key`. `mod_replace` runs before tracing, and replaces each stand-in drawn with the mod's full-size
  image (`paint_mod`). The mask's red takes MAIN COLOR and green ACCENT COLOR, read from the stand-in's texels 0 and
  1 (entries 1 and 33). The scuffed stand-in gets the same image mottled darker. The upscaling modes skip stand-ins.
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
