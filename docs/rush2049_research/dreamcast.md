# Dreamcast Rush 2049 disc as a 2049 source

Tags: **[V]** verified in data or code, **[I]** inferred. Addresses `0x8C......` are the disc executable's
(`1ST_READ.BIN`, loaded at 0x8C010000, little endian SH-4).

## Status (experimental)

The Games tab takes a Dreamcast disc image (.cdi, .gdi, .iso; USA disc only) in place of the N64 ROM. Its files are
copied into `rush2049_dc.pak` in the app folder; the N64 ROM's copy (`rush2049.z64`) is kept too. With both there, the
Games tab's Rush 2049 Source option (`rush2049_source` in games.json, default Dreamcast Disc, which is what a disc
chosen before the option existed got) picks one; selecting a ROM or disc points the option at it
(src/rush2049/wings.cpp `load_chosen_source`). Switching it takes effect without a restart: the source changes at once, the
Dreamcast Textures follow (`set_texture_sources`), the next race converts its track again (track2049.cpp
`loaded_source`), battle tuning is read again, and the cars and their physics are rebuilt the next time the game is
in the menus or track select (car2049.cpp `check_source`, game state 0-1, when no car is loaded from the old assets).
The track select's dioramas, sounds, songs, movers and props already follow the source in use.
Everything reads 2049 through `rush2::rom2049::Source` (include/rush2049_rom.h) in N64 terms: the disc's files are
converted to N64 files and its tables to N64 code segments on the fly, so no consumer knows which game it got.

| Content | State |
|---|---|
| Race tracks 1-6, stunt arenas, obstacle course | convert (`dc_test track`); track 1 raced in game |
| Placement, collision, AI paths | convert; checked against the N64 by `dc_check.py` |
| Code tables (types, cars, PVS, fog, texture animation, battle tuning, engine sounds) | built from the disc |
| Sound effects and songs | disc's own (see Audio); all 20 disc songs in the Sound tab while the disc is the source |
| Wing sound | disc sound 0x4D at the N64's pitch [I] |
| Cars | convert, with the disc's own paint jobs (see Car paint) |
| Full-size textures | the Dreamcast Textures option (Graphics tab): a texture pack built from the disc replaces the scaled-down ones, and with the N64 ROM as the source the N64 textures that are the disc's pictures (see Full-size textures) |
| Track select | the disc's thumbnails at full size (its converted file has them at 32 x 32) |
| Draw state | the disc's clamp / flip per texture; the sky drawn as the N64's (see Draw state) |
| Menus, wings, battle arenas in game | untested |

## Disc image and pack [V]

`src/rush2049dc/rush2049_dc.cpp`. A .cdi or .iso is one file of 2048-, 2336- or 2352-byte sectors; a .gdi lists track files.
The game's ISO 9660 volume is the first holding `1ST_READ.BIN`, `TRACK1.LZS` and `SELTRK.LZS` with all files inside
the image (a self-booting .cdi also has a low-density copy of the directory with no data behind it). The pack is
`"R49DCPAK"`, u32 version, entries `{name, u64 offset, u64 size, u32 kind}`; kind 0 is the file as stored (.LZS stays
compressed, same LZ as the N64's), kind 1 a song re-encoded as IMA ADPCM (block_samples 8192 per channel, 4-byte
header per block). `known_executable` probes four table addresses so only the disc this map was made for is accepted.

## Model containers [V]

`src/rush2049dc/rush2049_dc_model.cpp` (`convert_model`), reader `tools/rush2049/dc_model.py`. Little-endian container, chunk
directory at the end with reversed tags (`DHBO` = OBHD). OBHD records are 0x48 bytes: name[16], f32 radius, u16 kind,
s16 LOD count, 4 x {u16 texture handle, u16 flags, f32 distance, u32 stream}. TXHD 0x30: name, w, h, u8 2, u8 pixel
(0 ARGB4444, 1 RGB565, 2 ARGB1555), s16 -1, u32 data (IMAG-relative), u32 flags (0x04000000 mipmapped), u32 layout
(0x2000 VQ, 0x4000 twiddled, else linear). Twiddled order interleaves y (even bits) and x (odd bits); a rectangle is
squares of its short side in a row. VQ: 2 KB codebook of 2x2 blocks. Object streams are 32-bit commands (top 3 bits):
end, texture (+1 word), conditional (bit 0 not mirrored, bit 1 mirrored; +1 word byte length), vertex (0x20000000 ends
a strip; position / uv / colour from cache slots unless the 0x10000000 / 0x08000000 / 0x04000000 bit is clear). The
disc's loader is 0x8C026104 (mirror flag byte 0x8C12C657), a draw routine 0x8C077F00.

Conversion: each disc texture becomes an N64 RGBA16 texture (RGBA32 for ARGB4444, which has graded alpha) shrunk to
fit TMEM, with its own TXLD load list (cars: CI8, see Car paint). Its render mode is the one the N64 draws the same
texture with in the same file (`TEXMODE` rows of `rush2049_dc_names.inc`, from `dc_names.py texture_modes`, which
walks the N64 lists), else by its alpha: mostly graded translucent (C8104A50), any clear texels alpha-tested edges
(C8113278), else opaque (C8112230). The N64 names few of a track's textures, so most go by alpha; drawing cutouts
translucent let later objects show through tree cards, and translucent with depth writes blocked what's behind them. strips become F3DEX2 batches of 32 vertices (positions x16, st from the uv with
a per-batch shift, subdivided where the span overflows); conditionals become `E0010003/4`. The layout keeps 8 bytes
between IMAG, TXLD and OBHD: `merge_models` (src/rush2049/track2049_convert.cpp) maps an address equal to a chunk's end into
that chunk, so a load list starting where the texels end was relocated into the texels (the first track crash).
Files with fixed RDRAM windows (battle HUD 61, 63, weapons 76) are fitted by dropping texture levels until they're
no bigger than the N64's (`fixed_window_files`); every other file keeps each texture as big as TMEM takes. Object names differ in places: `tools/rush2049/dc_names.py` pairs them
(`src/rush2049dc/rush2049_dc_names.inc`).

Collision (`TRACKnROAD`): little-endian header, SEG, NODE, POLY, VERT and MOVER records; the VLIST and LEAF byte
streams are already big endian and copied as they are. Paths are the N64's format. Placement is swapped and renamed.

## Code tables [V]

`tools/rush2049/dc_tables.py` writes `src/rush2049dc/rush2049_dc_tables.inc`, a program `src/rush2049dc/rush2049_dc_tables.cpp` runs: each
table is copied from the disc to its N64 address in the N64's layout and row order (type rows by name, model handles
by renamed name), strings and pointed-at data going to a heap past the segment. Value maps renumber indices: map 0
model handles, map 1 sound effects (DC -> N64, from `dc_sounds.py`). `--report` compares the result with the N64
segments field by field. Disc addresses: type table 0x8C0BFD80 (0x34 rows), handle names 0x8C0AD8E4, kind
parameters 0x8C0C1744, flip-books 0x8C0C3170, scrolls 0x8C0C38C4, fog 0x8C0BDD56, PVS 0x8C0C49B0, car descriptors
0x8C0BB894, engine sounds 0x8C0B92A0 (0x1C per layer), battle mounts 0x8C0D1DF0, weapon sounds 0x8C0D1D7C.

## Audio [V]

The disc has no MusyX. `src/rush2049dc/audio2049_dc.cpp` plays it behind the same `audio2049` calls (N64 effect ids and songs).

- **Banks**: table 0x8C0A7904, 9 x 0x24 {name[0x14], u32 first id, u32 last id, u32 loaded, ...}: common 0x00-0x13,
  battle 0x14-0x20, engines 0x21-0x2F, front 0x30-0x34, gamecommon 0x35-0x5C, race 0x5D-0x97, track3 0x98-0x99
  (unused1/2 aren't on the disc). 0x8C016174 finds a global id's bank. A `.KAT` is u32 count, then 0x2C-byte entries
  {u32 1, offset, size (bytes), rate, loops, format (4 AICA ADPCM low nibble first, 8 signed PCM8, 16 PCM16), u32,
  loop start, loop end (samples), u32, u32}.
- **Playing** (0x8C016210, from the sound queue 0x8C016A02; 3D emitters 0x8C01164C): volume x -> 1 - (1 - x)^2 ->
  index x 255 into the attenuation table 0x8C0BADA8 -> sent as 127 - a/2 (0x8C016508); every id but 0x99 is scaled
  by 0.8 first. The port takes the attenuation as 0.375 dB steps [I] and uses an equal-power pan [I].
- **Effect ids**: `tools/rush2049/dc_sounds.py` pairs N64 and disc ids from the type table's sounds (by type name),
  the engine layers, the battle weapon table (N64 0x803942C0 / disc 0x8C0D1D7C) and the explosion call (N64
  func_800AF06C 0x2D/0x45/0x2F = disc 0x8C062D40 0x09/0x3D/0x3E). It emits `src/rush2049dc/rush2049_dc_sounds.inc`; every id the
  port plays has a pair.
- **Songs**: 0x8C0BA818 lists 20 files: 18 `.STR` streams, `HighScore.rom`, `Select.rom` (raw mono PCM16, played as
  22050 Hz loops [I]). Per-track songs 0x8C0BA76C, s32 per 2049 track id (0-5 race, 6-13 battle 1-8, 14-17 stunt 1-4,
  18 obstacle; -1 random of 18), the same index as the N64's 0x8010FFD4. The disc has a song for every track where
  the N64 shares 8; `audio2049::set_track` tells the backend the track, and an N64 song becomes the disc's song for
  that track (else for the first track the N64 plays it on; menu song 6 is Select.rom). `audio2049::disc_songs + n`
  plays disc song n directly; the Sound tab (src/music.cpp) lists all 20 by name while the disc is the source, and
  `audio2049::track_song` gives a course's original song (disc table 0x8C0BA76C, or the N64's 0x8010FFD4). Song 9
  (`Speed.str`) is on no track. `.STR` header: u32 1, rate,
  bits, block bytes (0x4000), blocks, data bytes, channels (2), end block, loop block; PCM16 blocks per channel.
- Lead-in: every `.STR` opens with about 16 blocks (6 s) of digital silence (all ADPCM bytes one value) and its loop
  block is 0, so `read_song` skips those blocks (start and loop); songs used to sound only after 6 s, and after the
  whole song was decoded. The mixer now decodes the ADPCM a block ahead of the play position (`decode_song`), so a
  song sounds within about 10 ms of the request (`dc_test audio` prints the time).
- Levels: the streams play at their mastered level (about -10 dBFS RMS), close to the N64 songs after the port's
  boosts; `audio2049_dc.cpp` divides those boosts back out.

## Texture sizes vs the N64 [V]

Same-name textures in track 1 (N64 file 101 vs `TRACK1.LZS`): `AL_BARRIER1` 16x128 vs 128x128, `GOLDEN_GATEA` 64x64
vs 128x128, `BRI_WATER2` 32x64 vs 128x128, `SHEEN0` 128x64 vs 512x512. The disc's textures are 16-bit color, mostly
64-256 texels a side; the N64 is limited to 4 KB of TMEM and mostly 8-bit paletted. The converter scales them down to
fit TMEM; the full-size images are drawn in their place (next section).

## Full-size textures [V]

`convert_model` reports every texture it makes (`SourceTexture`: the N64 texels, its name, the disc file, record and
tint, and `shrunk` for the ones it scaled down); `DcSource` keeps the list (and a `dc_file_N_textures` blob beside each
cached file).

The disc's images are drawn in place of its scaled-down textures whenever the disc is the source, and with the N64 ROM
as the source when the Dreamcast Textures option is on (Graphics tab, on by default, `dreamcast_textures` in
graphics.json; hot-swappable). The option is grayed out with the disc as the source or no disc stored
(`rush2::upscale::set_dreamcast_option_state`, called by wings.cpp `set_rom`). `src/rush2049dc/rush2049_dc_pack.cpp` keeps every one of them in
`<app folder>/track_cache/dc_textures`: one DDS with mipmaps per distinct image (named by content key), `images` (each
image's size, kind (scaled down, paint job, damaged), an 8 x 8 thumbnail and the RT64 hashes its textures are drawn
with) and a `stamp` (format, disc key, build) written last. It's built once per disc and build: in the background, it
converts every file (which also fills the file cache), then hashes each texture. USA disc: 4999 images for 6529
textures.

- **Disc as the source**: `rt64.json` (hash version 5) replaces each scaled-down texture with its image. recompui loads
  the folder through `set_generated_texture_pack` (lib/patches/RecompFrontend.patch) ahead of the mod packs, so a pack
  the player enables wins, and RT64 streams it like any other. Textures drawn at the disc's size already aren't in it
  (they are upscaled like any texture) unless the player installed an upscale of them.
- **N64 ROM as the source, disc stored, option on**: src/texture_upscale.cpp offers every kept 3D texture loaded from Rush 2049
  data (`rush2::origin`, below) to `rush2::rom2049::dc::match`, and draws a match with its disc image (an RT64 live
  replacement, which installed packs also override). See Matching N64 textures.
- The player's upscales of the disc images (Install Upscaled from `upscaled/rush2049dc`) are kept in
  `texture_upscale/dreamcast/<key>.dds`, linked into the folder's `upscaled/` and used in place of the disc's image in
  both modes.
- Dump Textures writes only the disc images in use to `dump/rush2049dc`: with the disc as the source those drawn from
  the pack or drawn at the disc's size already (each texture's RT64 hash looked up in `images`), with the N64 ROM those
  matched.

`replacement_hash` (src/rush2049dc/rush2049_dc_model.cpp) is the hash RT64 gives the texture when it is drawn from the converter's
own load list (`add_load_list`): LOADBLOCK into TMEM as RT64's RDP runs it (`loadToTMEMCommon`: 64-bit words with the
load tile's line of 0, words swapped (address ^ 4) on every other row as the dxt counter passes 0x800, RGBA32 split
into TMEM's lower and upper halves), then `TMEMHasher::hash` on the render tile (RGBA, line `(w * 2 + 7) >> 3`,
w x h, no TLUT). Checked in game on track 1: every scaled-down texture drawn had its hash in the pack.

The upscaler leaves the pack's textures alone (`in_texture_pack`), and the textures drawn with a matched disc image.

### Matching N64 textures [V]

The N64's Rush 2049 textures are the disc's art redrawn in 16 or 256 colors, at a quarter or less of the texels and
often at another aspect (the UVs make up for it), sometimes mirrored or recropped. Texel differences can't tell a
match from a lookalike: a real pair's mean largest-channel difference is 10-25, a dark or flat lookalike's lower. What
does is structure. `match` compares, for each disc image at least the texture's size (not paint jobs or damaged car
textures) whose thumbnail brightness correlates 0.5 or more, the image box-averaged to the texture's size: the
Pearson correlation of brightness over texels opaque in both must be 0.84 or more, the texture's brightness spread 6 or
more (flat ones can't be told), at most a fifth of texels opaque in one and clear in the other (the N64's cutouts are
hard where the disc's fade: trees differ in 9-16%), the average colors within 40 a channel, and for an N64 I or IA
texture (tinted by the draw) a gray image. The best correlation wins.

Measured on Marina (N64 source, `RUSH2_TEXTURE_DUMP`, tmp tools comparing dump/rush2049 with dump/rush2049dc): true
pairs reach 0.84 (building fronts and walls 0.98+, the road 0.93, trees 0.87-0.93, recropped vents 0.85); the
nearest lookalikes are 0.82 and below. 146 of the 425 Rush 2049 textures drawn match; the rest have no disc image of
the same picture (different art, or a part of a larger image, which a whole-image comparison doesn't find). Results
are kept in `matches_v4` (N64 content key, image key or 0).

A matched image larger than the texture is drawn as it is. One the same size (the road: 64 x 64 on both, full color
on the disc) is upscaled in the texture's place when upscaling is on (its own upscaler entry, `disc_upscale`).
`RUSH2_DC_MATCH_LOG=1` logs every comparison.

### Texture origin (dump folders) [V]

`rush2::origin` (src/texture_origin.cpp) tags RDRAM ranges with the game their data came from. Every asset goes
through `rush2_asset_decompress` (src/assets.cpp): a replacement carries its game (`assets::replace` spans: SF Rush
track geometry, Rush 2049 or Rush 2049 DC track geometry, 2049 cars and parts), Rush 2's own assets clear the range.
The RT64 texture observer gets the RDRAM address of the last load into the tile's TMEM
(`rice.lastLoadOpByTMEM[tmem].texture.address`, lib/patches/rt64.patch), and the upscaler records the tag found
there. The track select's container (asset 3) mixes Rush 2's, 2049's and SF Rush's dioramas and is left as Rush 2.

### Track select art from the disc [V]

`build_menu_container` builds every 2049 track's diorama and preview or none. The obstacle course has no outline
model, only its thumbnail, and the disc's converted file 60 has the thumbnails at 32 x 32 (fit to TMEM) where the
preview wants 128 wide, so the disc build failed and the track select's tables still named the missing logos:
`texture_find_by_name` (0x800601F8) returned 0 and `hud_widget_load_texture` (0x80060340) read through it. Now
`disc_thumbnail` (track2049_art.cpp) takes the thumbnail from the disc's image, and the tables name 2049 and SF Rush
art only when the container has it (`menu_has_2049`, `menu_has_rush1` in track2049_menu.cpp).

## Car paint [V]

The disc's cars have no paint ramps. `CARnPJ1-12.LZS` hold, per paint job, the body textures (same names and sizes as
the base car's plus a color suffix: `C1_TOP01_BLU`, `_RED`, `_YEL`, `_GRN`, `_PUR`, `_TEL`, `_ORG`, `_CHR`). Most jobs
recolor one livery, but some have a pattern of their own (car 2's jobs 7-9, checkers and swooshes; car 1's job 3 is
two-tone), so no palette scheme can stand in for them. Jobs 9-12 mostly repeat an earlier one. Recoloring one livery
through N64-style ramps (the first port) lost those patterns and smeared the shading.

The jobs are used as they are. `car_jobs` (src/rush2049dc/rush2049_dc_model.cpp) reads every job, drops the ones equal to an
earlier job (8-9 distinct per car), marks a texel as paint where any job differs from the first by 24 or more in a
channel, and gives each job a color: the most common color among its paint texels (8 levels a channel), so a two-tone
job is its main color. The converted car file holds the first job's textures, RGBA like every other converted
texture (scaled down only to fit TMEM), and a zeroed palette record where the N64's cars have their palette (the game
loads it as the car's TLUT; nothing uses it). Every job's scaled-down texels are reported as
`SourceTexture`s (`job`, `job_rgb`, `name`), with a damaged copy of each texture that has paint: `scuff_image` mottles
the paint darker and lighter in blotches and adds bright scrapes, laid out over the scaled-down texels so the disc-size
image (`DcSource::source_image` scuffs it the same way) and the scaled-down one match. All of them go in the texture
pack, so each job and its damage draw at the disc's size.

src/rush2049/car2049.cpp (`load_jobs`) finds each job texture's texels in the car asset through its record's load list
(record +0x18 is the load list, not the texels) and takes the damaged copies for its damage textures from the
converter. `rush2_car49_paint` (end of func_8008582C) takes job MAIN COLOR % jobs and copies its texels and damaged
texels into that car's asset copy (each preview id and race car has its own). Most jobs have no Rush 2 color to stand
for, so the car select picks them as Rush 2049 does, by style: a Dreamcast car's MAIN COLOR row reads STYLE ("STYLE n",
no swatch, wrapping at the car's jobs, `rush2::car2049::paint_jobs`) and ACCENT, STRIPE and STRIPE COLOR are hidden
(src/rush2049/wings_menu.cpp; cars.md "Car select rows").

## Draw state [V]

How the disc draws a model, read from its code (Sega's Kamui 2 library):

- **Texture load** 0x8C06E95C: kmCreateTextureSurface / kmLoadTexture, then **0x8C06EBB2** fills a `KMSTRIPCONTEXT`
  (0x90 bytes) from the texture record's flag word (TXHD +28) and has **0x8C0DBD60** (kmGenerateStripHead) build the
  texture's 32-byte strip head (record +44). Context fields as 0x8C0DBD60 packs them: +4 list type (PCW 26:24), +20
  offset, +24 Gouraud (1); image control at +80: +96 fog mode (0, table fog), +112 flip (TSP 18:17), +116 clamp (TSP
  16:15), +120 filter (TSP 14:13; point sampled when flag 0x20 is set, else bilinear), +132 shading (TSP 7:6) = 3,
  modulate alpha, for every texture, +140 the surface.
- **Flags to clamp / flip**: 0x30000 -> clamp UV, 0x10000 -> clamp U, 0x20000 -> clamp V; 0xC0000 -> flip UV, 0x40000
  -> flip U, 0x80000 -> flip V (the values 3 / 2 / 1 land in TSP's 2-bit fields, where 2 is U and 1 is V). Flip is the
  N64's mirror. `set_tile_modes` (src/rush2049dc/rush2049_dc_model.cpp) puts them in the render tile's cms / cmt; a clamped axis
  gets no whole-repeat shift of its coordinates, a mirrored one an even shift. Examples in track 1: `DC_SUNSET1-4`, the
  trees 0x30000 (clamped), `ROAD1_BLUE` 0x140000 (flip U: the half road mirrors at the centre line).
- **Model load** 0x8C026104 rewrites each texture command to `(cmd & 0xFF000000) | bucket << 16 | list | 0x300`, the
  next word the strip head; a texture whose record +32 has 0x20000 gets 0x01000000 and record +36 instead, which the
  renderer (stream walk at 0x8C06FFD0) draws through 0x8C0DAC20 with a float pair from that record (no track 1 texture
  has it in the file).
- **Vertex colors** multiply the texture (modulate alpha, Gouraud), as the N64 combiner does.

**Sky.** The N64's SKYSKY lists (every track file, and STUNTSKYSKY) draw 1-cycle (`E3000A01 00000000`), TEXEL0 x SHADE
in both combiner cycles (`FC121824 FF33FFFF`), render mode `0F0A4000` (opaque, no fog), with G_ZBUFFER and G_FOG
cleared (`D9FEFFFE 00000000`), and put both back after (`D9FFFFFF 00010001`, `E3000A01 00100000`, 2-cycle). The
converted disc sky now does the same (`LodBuilder::sky`, objects ending in SKYSKY). Before, it was depth tested with the
track's fogged modes and hid the distant scenery behind it: the Golden Gate (TRACK1L54015) never showed. The disc's
sky is its own model (`DC_DOMEBLUE` top, `DC_SUNSET1-4` panels with v 0 at the horizon), tinted blue at the top by its
vertex colors (0xFF0000D7 and the like), so it doesn't look like the N64's sky.

## Not done

- In-game checks of menus and thumbnails (`list_image` decodes the RGBA load lists), wings, battle arenas.

## Tools

`dc_cdi.py` (image listing and extraction; `pack` extracts from rush2049_dc.pak), `dc_model.py` (containers, textures to PNG), `dc_names.py` (name pairs),
`dc_tables.py` (table program), `dc_sounds.py` (sound pairs, `--banks`), `dc_check.py` (converted files vs the N64),
`dis_dc.py` (SH-4 disassembly: `dis`, `func`, `refs`, `calls`, `str`), `view49.py` (renders N64 model objects to
PNG), `cpp_test/dc_test.cpp` (`dc_build.bat`; modes: convert files, `track`, `audio`, `n64` (the N64 ROM's own files)).
