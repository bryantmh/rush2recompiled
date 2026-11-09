# Dreamcast Rush 2049 disc as a 2049 source

Tags: **[V]** verified in data or code, **[I]** inferred. Addresses `0x8C......` are the disc executable's
(`1ST_READ.BIN`, loaded at 0x8C010000, little endian SH-4).

## Status

The Games tab takes a Dreamcast disc image (.cdi, .gdi, .cue, .iso, .chd; USA disc only) in place of the N64 ROM. Its files are
copied into `rush2049_dc.pak` in the app folder; the N64 ROM's copy (`rush2049.z64`) is kept too. With both there, the
Games tab's Enabled Source option (`rush2049_enabled_source`: N64, Dreamcast or Both, default Both; added
2026-11-05) says which the game may use, and with Both the Primary Source option (`rush2049_source`, named Rush 2049
Source until 2026-11-05; default Dreamcast Disc, which is what a disc chosen before the option existed got) picks the
one in use for everything but the race tracks' versions (`wings.cpp resolved_choice`, `rush2::wings::source_enabled`);
selecting a ROM or disc points Primary Source at it (and Enabled Source at Both if it left that one out)
(src/rush2049/wings.cpp `load_chosen_source`). Switching it takes effect without a restart: the source changes at once, the
disc's full-size textures follow (`set_texture_sources`), the next race converts its track again (track2049.cpp
`loaded_source`), battle tuning is read again, and the cars and their physics are rebuilt the next time the game is
in the menus or track select (car2049.cpp `check_source`, game state 0-1, when no car is loaded from the old assets).
The track select's dioramas, sounds, songs, movers and props already follow the source in use.

The race tracks are the exception (2026-11-05, issue 55): with both copies stored, each of the six offers both
versions on the track select at once, whatever the option says: its N64 version (ids 12-17, from the N64 ROM) and its
Dreamcast version (ids 249-254, from the disc), behind one entry whose version button switches them
(docs/custom_tracks.md#versions). `rush2::wings::track_source(disc)` gives either copy (the other one opened once and
kept); track2049.cpp `source_for` picks a race track's, the arenas and obstacle course keep the option's, and the
disc's conversions are cached apart (`cache_k`: entry k + 100) so neither replaces the other. The raced track's
props and moving objects read that copy's tables (`rush2::track2049::race_source`). With the disc alone, 12-17 are
the disc's. The Dreamcast versions share the course's records, ghosts, unlocks and track select art.
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
| Full-size textures | a texture pack built from the disc replaces the scaled-down ones whenever a disc is installed (see Full-size textures) |
| Track select | the disc's thumbnails at full size (its converted file has them at 32 x 32) |
| Draw state | the disc's clamp / flip per texture; the sky drawn as the N64's (see Draw state) |
| Menus, wings, battle arenas in game | untested |

## Disc image and pack [V]

`src/rush2049dc/rush2049_dc.cpp`. A .cdi or .iso is one file of 2048-, 2336- or 2352-byte sectors; a .gdi or .cue
lists track files (Redump's .cue marks the GD-ROM part with `REM HIGH-DENSITY AREA`: its first file starts at LBA
45000, later files follow on, pregaps inside them). The game's ISO 9660 volume is the first holding `1ST_READ.BIN`,
`TRACK1.LZS` and `SELTRK.LZS` with all files inside the image (a self-booting .cdi also has a low-density copy of the
directory with no data behind it). Our USA .cdi's volume (label CDROM, sector 150) counts sectors from its own track;
other rips count from the disc's start (45000 for a GD track 3 saved as .iso, often 11702 for a self-boot CD-R's second
session), found from the root directory's "." record, which holds its own sector number. CD-R self-boot copies may keep
`1ST_READ.BIN` scrambled (the boot ROM unscrambles it from a CD); the import unscrambles it (Comstedt's scramble.c) and
stores it plain. `tools/rush2049/dc_variants.py` writes the USA .cdi in each of these layouts (plus .gdi and Redump
.cue) for `dc_test.exe` to import offline. A .chd (chdman's conversion of a .gdi or .cue; `src/chd_image.cpp` over
libchdr) is read by `open_chd`: a GD-ROM's (CHGD metadata) data tracks go at their sector numbers, track 3 at 45000 and
each later track after the one before (chdman's GD `FRAMES` already runs up to the next track: `PAD` is the blank frames
it added after the .gdi file, not the CHD's padding to 4 frames), then the volume is read like a .gdi's; a CD .chd's
data tracks are searched like a .cdi. `dc_test.exe chd FILE` lists a .chd's tracks as read. The CHDs of the Redump .cue
and the self-boot .cue import to packs identical to the .cue's. The pack is
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
Files with fixed RDRAM windows (HUD 61 and 62, battle HUD 63, weapons 76: src/rush2049/battle_render.cpp's
`model_files`) are fitted by dropping texture levels until they're no bigger than the N64's (`fixed_window_files`);
every other file keeps each texture as big as TMEM takes. One file over its window fails `battle_render::ready()` and
loses every battle model (weapons, kill coin, health frame): 62 was missing from the list until 2026-11-02. Fitting
63 shrinks HEALTHBG to 4 x 8, so the battle health bar on a disc is drawn from its full-size image
(`battle_render::image`) at the N64's 64 x 8, not as a Rush 2 HUD widget (which can't draw RGBA). File 64 (the N64's HUD images) is made from the disc's `HUDCOM`; the odometer reads its full-size ODOMETER the same way (odometer.md). Object names differ in places: `tools/rush2049/dc_names.py` pairs them
(`src/rush2049dc/rush2049_dc_names.inc`).

Collision (`TRACKnROAD`): little-endian header, SEG, NODE, POLY, VERT and MOVER records; the VLIST and LEAF byte
streams are already big endian and copied as they are. Paths are the N64's format. Placement is swapped and renamed.

A path header (PTHD, 36 bytes) holds two file offsets, and both move with the converted file's layout: +0x1C its
nodes, +0x18 the header of the object a TRIGGER pad opens (movers.md §2). The link was copied as the disc's offset
until 2026-10-13, so `parse_paths` dropped it and no switch opened anything with the disc as the source (41 pads on
the six race tracks; the other files have no paths). Track 4's group 7 pad opens an object the disc names T7 and the
N64 T2; the other 40 links name the same objects as the N64's.

## Code tables [V]

`tools/rush2049/dc_tables.py` writes `src/rush2049dc/rush2049_dc_tables.inc`, a program `src/rush2049dc/rush2049_dc_tables.cpp` runs: each
table is copied from the disc to its N64 address in the N64's layout and row order (type rows by name, model handles
by renamed name), strings and pointed-at data going to a heap past the segment. Value maps renumber indices: map 0
model handles, map 1 sound effects (DC -> N64, from `dc_sounds.py`). `--report` compares the result with the N64
segments field by field. Disc addresses: type table 0x8C0BFD80 (0x34 rows), handle names 0x8C0AD8E4, kind
parameters 0x8C0C1744, flip-books 0x8C0C3170, scrolls 0x8C0C38C4, fog 0x8C0BDD56, PVS 0x8C0C49B0, car descriptors
0x8C0BB894, engine sounds 0x8C0B92A0 (0x1C per layer), battle mounts 0x8C0D1DF0, weapon sounds 0x8C0D1D7C.

Pointers a consumer compares go through the address map (`FUNC` rows): the type table's callbacks, and the car
descriptors' four tire curve pointers at +0x60 (disc 0x8C0A8B1C front, 0x8C0A8B34 rear -> N64 0x8011F814,
0x8011F82C). car2049.cpp `build_physics` picks a wheel's curve by the N64 rear address; copied as plain words the
disc's pointers matched neither, so all four wheels got the front curve (16000 where the rear has 32000) and every
disc car's rear slid. The disc's car tables are otherwise the N64's, value for value.

### Tables with disc units [V]

Two copied tables keep the disc's meaning, so their consumers convert:

- **Sign tip angles** (kind 0 parameters, disc 0x8C0C1744 -> N64 0x80118D70): the disc runs objects at 60 Hz. Its sign
  init 0x8C05A58C (N64 0x8010E72C) sets 8 tip frames at +90 (`mov #8`), where the N64 sets 4, and its angles are half
  the N64's (STOPHIT -0.192 vs -0.384 rad, GETOFF -0.1876 vs -0.3752). The update 0x8C05A61A counts +90 down as
  0x8010E828 does. `src/rush2049/track2049_props.cpp` steps a disc source's signs 8 times at 60 Hz (same 1.54 rad
  over 2/15 s). Stepping the half angles 4 times left the signs half tipped.
- **Texture scroll speeds** (disc 0x8C0C38C4 -> N64 0x8011A840): the disc's scroll 0x8C05B3DE runs the N64's
  position/wrap/rate logic (vblanks per frame from 0x8C122550), but writes a UV offset position x 0.0625 / texture
  width (height for kind 10) to the texture at +0x24 (+0x28), which the renderer adds to the UVs. The N64's tile
  origin is subtracted. So every disc race-track speed is the N64's negated (AROWGRN 64 -> -64, BRI_WATER2 -2 -> 4,
  MAGMA 2 -> -4, VAPORS 8 -> -8, GREENFLAME2 -16 -> 16; the doubled ones have twice the wrap for the larger
  textures). `convert_tex_anims` (src/rush2049/track2049_convert.cpp) negates a disc speed. Unnegated, the Haight
  arrows ran backwards.

### Car lighting tables [V]

The road colors that tint a car (docs/rush2049_research/collision.md "Car lighting") are on the disc in the N64's
layout: per track id a pointer to its color table at 0x8C0C3F94 (N64 0x8011AF90), the three 26-color tables at
0x8C0C3E5C / 0x8C0C3EC4 / 0x8C0C3F2C, and the in-the-air index at 0x8C0C43B8 (N64 0x8011B468, same values). The
disc's car lighting reads them at 0x8C060A24 and 0x8C060B1C. Colors are ARGB words (bytes B, G, R, A); the `c` schema
token turns them into the N64's RGBA. The only value difference: track 4's table (N64 0x8011AEC0) has greys 0-8 one
step darker (0xC8C8C8 first, where the N64 has 0xDCDCDC). Until 2026-11-06 the program didn't copy them, so a disc
source read zeros there: no road tint on the cars and no painted colors in the light probes.

### Audit of the copied tables (2026-11-06)

Every 2049 code-segment address the port reads (src/rush2049, include) was checked against the program's copies.
Besides the lighting tables, the uncopied ones are fine: the drone setup rows B / C / D (0x8011103C, 0x80111080,
0x8011119C, row 0) are all zero on the N64 too; per-car colors (0x80111604) and the paint shades (0x801226C0) don't
apply to disc cars, which paint with their palettes and paint jobs (`paint_colors::load_2049` falls back to the N64's
shades); the file table (0x8011B5BC) is the N64 ROM's own (the disc has its file system). The rest are Rush 2 RAM.

## Audio [V]

The disc has no MusyX. `src/rush2049dc/audio2049_dc.cpp` plays it behind the same `audio2049` calls (N64 effect ids and songs).

- **Banks**: table 0x8C0A7904, 9 x 0x24 {name[0x14], u32 first id, u32 last id, u32 loaded, ...}: common 0x00-0x13,
  battle 0x14-0x20, engines 0x21-0x2F, front 0x30-0x34, gamecommon 0x35-0x5C, race 0x5D-0x97, track3 0x98-0x99
  (unused1/2 aren't on the disc). 0x8C016174 finds a global id's bank. A `.KAT` is u32 count, then 0x2C-byte entries
  {u32 1, offset, size (bytes), rate, loops, format (4 AICA ADPCM low nibble first, 8 signed PCM8, 16 PCM16), u32,
  loop start, loop end (samples), u32, u32}.
- **Playing** (0x8C016210, from the sound queue 0x8C016A02; 3D emitters 0x8C01164C): volume x -> 1 - (1 - x)^2 ->
  index x 255 into the attenuation table 0x8C0BADA8 -> sent as 127 - a/2 (0x8C016508); every id but 0x99 is scaled
  by 0.8 first. [V] Sega's sound library (amSoundSetVolume 0x8C0A2DB2 -> acDigiSetVolume 0x8C09D9AE, its volume
  table mode at 0x8C1A7334 never switched on) sends the level to the AICA driver (`AUDIO64.DRV` 1.90f on the disc,
  command 0x96) as total level a & ~1, the AICA's 0.375 dB steps. Pan: the game sends (1 - p) / 2 x 127 (0x8C0162AC),
  amSoundSetPan (0x8C0A2E1C) sends it >> 2, and the driver (command 0x97, handler 0x3F24) makes that 0-31 value the
  channel's DIPAN: 16 and up attenuate the right side by value - 16 steps, 1-15 the left by 16 - value, 0 the left
  fully (3 dB a step, 15 silent). The game's centre (63 -> 15) leaves the left 3 dB down and the right full. The
  driver's channel template (0x1134: +0x24 = 0x0F10, +0x28 = 0x0020) and its init keep the direct send level and the
  master volume full; the game never changes them (acDspSetMixerChannel, acSystemSetMasterVolume uncalled). Driver
  commands from its jump table at 0x2FF4: 0x81 master volume, 0x90 open, 0x95 mixer channel, 0x96 volume, 0x97 pan.
- **Effect ids**: `tools/rush2049/dc_sounds.py` pairs N64 and disc ids from the type table's sounds (by type name),
  the engine layers, the battle weapon table (N64 0x803942C0 / disc 0x8C0D1D7C) and the explosion call (N64
  func_800AF06C 0x2D/0x45/0x2F = disc 0x8C062D40 0x09/0x3D/0x3E). It emits `src/rush2049dc/rush2049_dc_sounds.inc`; every id the
  port plays has a pair.
- **Songs**: 0x8C0BA818 lists 20 files: 18 `.STR` streams, `HighScore.rom`, `Select.rom` (raw mono PCM16, played as
  22050 Hz loops [I]). Per-track songs 0x8C0BA76C, s32 per 2049 track id (0-5 race, 6-13 battle 1-8, 14-17 stunt 1-4,
  18 obstacle; -1 random of 18), the same index as the N64's 0x8010FFD4. The disc has a song for every track where
  the N64 shares 8; `audio2049::set_track` tells the backend the track, and an N64 song becomes the disc's song for
  that track (else for the first track the N64 plays it on; menu song 6 is Select.rom). `audio2049::disc_songs + n`
  plays disc song n directly; the Sound tab (src/music.cpp) lists all 20 by name while the disc's soundtrack is in use (see Both soundtracks below), and
  `audio2049::track_song` gives a course's original song (disc table 0x8C0BA76C, or the N64's 0x8010FFD4). Song 9
  (`Speed.str`) is on no track. `.STR` header: u32 1, rate,
  bits, block bytes (0x4000), blocks, data bytes, channels (2), end block, loop block; PCM16 blocks per channel.
- Lead-in: every `.STR` opens with about 16 blocks (6 s) of digital silence (all ADPCM bytes one value) and its loop
  block is 0, so `read_song` skips those blocks (start and loop); songs used to sound only after 6 s, and after the
  whole song was decoded. The mixer now decodes the ADPCM a block ahead of the play position (`decode_song`), so a
  song sounds within about 10 ms of the request (`dc_test audio` prints the time).
- Songs' volume and pan: a song starts through the same volume law (0x8C06A086, the music volume 0x8C17843C, 1.0 by
  default: full level); its stream (amStreamSetVolume 0x8C09F7FC, track 0) and every stream track open with volume 127
  and pan 63 (0x8C09FD14), which the game never changes (amStreamSetPan uncalled, the song's pan field +0x43F unread),
  and each track's pan goes to the driver as an effect's does (0x8C0A09B4). So by the code the disc plays both
  channels of a song at the same near-centre pan, folded to mono; the port keeps the songs in stereo [decision pending].
- Levels: the streams play at their mastered level (about -10 dBFS RMS), close to the N64 songs after the port's
  boosts; `audio2049_dc.cpp` divides those boosts back out.
- Both soundtracks: with the N64 ROM and the disc both installed, the Sound tab's Rush 2049 Soundtrack (Same as
  Source, N64, Dreamcast, Both) picks whose songs play. The other source's songs load beside the first's on a worker
  (`audio2049::load_music`: the MusyX engine from the ROM beside a disc, or `dc::load(source, false)`, the disc's song
  tables without its effect banks, beside the ROM); effects stay the Games tab source's. `play_song` sends
  `disc_songs + n` to the disc player and 0-11 to MusyX when it's loaded, stopping the other player's song.
  `n64_track_song` / `disc_track_song` give each soundtrack's course song; with Both, Original plays one of the two.

## Texture sizes vs the N64 [V]

Same-name textures in track 1 (N64 file 101 vs `TRACK1.LZS`): `AL_BARRIER1` 16x128 vs 128x128, `GOLDEN_GATEA` 64x64
vs 128x128, `BRI_WATER2` 32x64 vs 128x128, `SHEEN0` 128x64 vs 512x512. The disc's textures are 16-bit color, mostly
64-256 texels a side; the N64 is limited to 4 KB of TMEM and mostly 8-bit paletted. The converter scales them down to
fit TMEM; the full-size images are drawn in their place (next section).

## Full-size textures [V]

`convert_model` reports every texture it makes (`SourceTexture`: the N64 texels, its name, the disc file, record and
tint, and `shrunk` for the ones it scaled down); `DcSource` keeps the list (and a `dc_file_N_textures` blob beside each
cached file).

The disc's images are drawn in place of its scaled-down textures whenever a disc is installed: with the disc as the
source, and with the N64 ROM as the source, for the race tracks' Dreamcast versions (wings.cpp `set_rom` passes the
stored disc to `set_texture_sources`). `src/rush2049dc/rush2049_dc_pack.cpp` keeps every one of them in
`<app folder>/track_cache/dc_textures`: one DDS with mipmaps per distinct image (named by content key), `images` (each
image's size, kind (scaled down, damaged) and the RT64 hashes its textures are drawn with) and a `stamp` (format,
disc key, build) written last. It's built once per disc and build: in the background, it
converts every file (which also fills the file cache), then hashes each texture. USA disc: 4999 images for 6529
textures.

- `rt64.json` (hash version 5) replaces each scaled-down texture with its image. recompui loads
  the folder through `set_generated_texture_pack` (lib/patches/RecompFrontend.patch) ahead of the mod packs, so a pack
  the player enables wins, and RT64 streams it like any other. Textures drawn at the disc's size already aren't in it
  (they are upscaled like any texture) unless the player installed an upscale of them.
- The N64 ROM's own textures are left as they are (see Matching N64 textures, removed).
- The player's upscales of the disc images (Install Upscaled from `upscaled/rush2049dc`) are kept in
  `texture_upscale/dreamcast/<key>.dds`, linked into the folder's `upscaled/` and used in place of the disc's image.
- Dump Textures writes the disc's images (but the damaged car textures') to `dump/rush2049dc` by disc file and texture
  name (`dump_images`).

`replacement_hash` (src/rush2049dc/rush2049_dc_model.cpp) is the hash RT64 gives the texture when it is drawn from the converter's
own load list (`add_load_list`): LOADBLOCK into TMEM as RT64's RDP runs it (`loadToTMEMCommon`: 64-bit words with the
load tile's line of 0, words swapped (address ^ 4) on every other row as the dxt counter passes 0x800, RGBA32 split
into TMEM's lower and upper halves), then `TMEMHasher::hash` on the render tile (RGBA, line `(w * 2 + 7) >> 3`,
w x h, no TLUT). Checked in game on track 1: every scaled-down texture drawn had its hash in the pack.

The upscaler leaves the pack's textures alone (`in_texture_pack`).

### Replacement texel centers [V]

The disc samples texel i of a `w`-wide image at `u * w = i + 0.5`; the converter writes `s = u * w * 32 - 16` (half a
texel in, `make()` in rush2049_dc_model.cpp), which is the same place on the N64's texel grid (texel i at `s = i`).
For a texture scaled down by `k`, RT64 samples the pack image at `(s + 0.5) * k` with the shift "half" it gives
upscales, and filters there the RDP's way (texel i at a whole coordinate): half a disc texel off where the disc
samples it. A car's side is several wrapping textures side by side (the 8-Ball's `C12_SDE01`-`08`, 64 x 32, drawn
from 32 x 16) whose UVs stop 0.2-0.6 texels short of each edge, so that half texel pulled the opposite edge into
every seam: a thin dark line along each panel join (issue #47's screenshot). Sheen, SSAO and MSAA made no difference;
an offline render of the disc's mesh and textures with the RDP filter at `u * w - 0.5` has no lines, at `u * w` the
same lines as the game.

The pack's rt64.json gives its textures the shift "center" (an RT64 shift added in lib/patches/rt64.patch,
`ReplacementShift::Center`, tile flag `centered`): `(s + 0.5) * k - 0.5`, the middle of each N64 texel's block of the
image, which is exactly where the disc samples for any `k` (and for a player's upscale of a disc image). RT64's
hardware-sampler paths (mipmaps) already put "half" there and take "center" plus half a texel. Other packs and the
upscaler's live replacements keep "half".

### Matching N64 textures (removed 2026-11-07)

With the N64 ROM as the source, a Dreamcast Textures option (Graphics tab) used to offer every Rush 2049 texture
drawn in 3D to a matcher (`dc::match`) that looked for the disc image with the same picture (brightness correlation
of the image scaled down to the texture's size, 0.84 or more, plus transparency and average color checks) and drew
that image in the texture's place. It was removed: the race tracks' Dreamcast versions can be raced side by side with
the N64 versions, so the N64 versions keep their own textures, and a likeness test can pass a wrong picture, which
then stayed for good (results were saved in `matches_v5`). The Rock arcade, a custom track loaded as a Rush 2049
track, had its four sunset sky panels matched to one lavender disc wall, drawn over the sky all race. Loading the
disc's images deletes the old `matches_v4` and `matches_v5` files; the `dreamcast_textures` key in a saved
graphics.json is ignored. Dump Textures names Rush 2049's N64 textures by their model again, and moves a player's
upscales saved under the disc names the matcher gave to the new names (`follow_renames`, src/texture_upscale.cpp).

### Texture origin (dump folders) [V]

`rush2::origin` (src/texture_origin.cpp) tags RDRAM ranges with the game their data came from. Every asset goes
through `rush2_asset_decompress` (src/assets.cpp): a replacement carries its game (`assets::replace` spans: SF Rush
track geometry, Rush 2049 or Rush 2049 DC track geometry, 2049 cars and parts), Rush 2's own assets clear the range.
The RT64 texture observer gets the RDRAM address of the last load into the tile's TMEM
(`rice.lastLoadOpByTMEM[tmem].texture.address`, lib/patches/rt64.patch), and the texture census reports the tag
found there. The track select's container (asset 3) mixes Rush 2's, 2049's and SF Rush's dioramas and is left as Rush 2.

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

### Brake lights [V]

On the N64 ROM a 2049 car's tail lights are lit as Rush 2 lights its own: func_800599A0 writes its palette's tail
light ramp (161-175) bright red while the car brakes. A disc's car draws RGBA textures, which no palette touches, so
its lights stayed dark (issue #44). Its lights are found in its textures instead: `rush2::rom2049::dc::brake_light_mask`
(src/rush2049dc/rush2049_dc_model.cpp) marks a texel of a job texture that is lamp red (red 50 or more, green under
0.55 of it, which leaves the amber markers out, blue under 0.7) in every job (the paint changes between jobs, the
lights don't), then grows two texels into reddish texels that are the same in every job (a lens's rim and highlight),
fills texels closed in by lights within 8 texels on all four sides that are the same in every job (a lens's middle),
and drops texels with no lit neighbor. Textures of the car's front (`front_texture_name`: FRONT, HEAD, NOSE, HOOD,
LOWF, `_F<n>`) are left out. A texture without jobs named for a tail light (`tail_texture_name`: TAIL, TLGHT, as
C3_TAILIGHT) takes its reddish texels. `light_texels` makes a lit copy: red 255, green and blue a third.

src/rush2049/car2049.cpp (`light_jobs`, `load_jobs`) keeps each job's lit copies (of its damaged copy too), and
`add_damage_textures` gives their loads lit copies in the car asset (`DamageLoad::lit`, `lit_damaged`), which
`rush2_car49_paint` fills with the job's and `rush2_car49_dent` switches to while the car brakes, as California
Speed's lit tail lights (`Paint::lights`; `rush2_calspeed_brake_lights` keeps the braking and lets Rush 2 write its
palette too). The texture pack gets a full-size lit image for every scaled-down lit copy (pack format v4,
`rush2049_dc_pack.cpp` build: the mask found again over the full-size images), so a braking car keeps the disc's
resolution. Seen in game: the Bruiser's four round lights and the Magnum's two dark while driving, bright red while
braking.

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
vertex colors (0xFF0000D7 and the like), so it doesn't look like the N64's sky. The N64's track 4 sky switches to
`00504240` (1-cycle G_RM_XLU_SURF, no z) for its moon's two triangles after the opaque panels; the converted sky does
the same for any sky texture that isn't opaque (only `AL_MOON` in TRACK4), whose clear corners showed as a box before.

**Sky size.** The disc draws SKYSKY **2.5x** the model's size. 0x8C04F43C (sky setup) looks the model up and builds one
matrix per view (0x8C06872E) with 2.5 (the float at 0x8C04F508) on the diagonal and no translation; 0x8C04F4C8 copies
the camera's x and z (camera +36, +44) into it each frame, never y. So the dome is centered on the world origin at
2.5x: track 1's horizon ring (radius 1,800, the disc's units) is 4,500 out, its apex 2,872 up. The vertex colors are
plain ARGB8888: the renderer (stream walk 0x8C06FFD0) caches the word as is and 0x8C06EF10 writes it unchanged as the
TA vertex's base color. A real disc capture of Marina matches ARGB (the dome's top is DC_DOMEBLUE x the lavender ring
color). Drawn at 1x at world height, the dome was 2.5x too small, so seen from the track its sunset band (the
DC_SUNSET panels' lower half) sat below the treeline and only its blue top showed. The N64's own sky is a different
model (horizon at y 6,400, top 24,000), drawn at 1x. 2.5x doesn't fit the s16 vertices (the top would be 45,950), so the
converted sky is drawn at its own size and placed as custom tracks' big skies are: `track.sky_scale` = 1/2.5 for a disc
track (src/rush2049/track2049.cpp `convert`), and `rush2_track49_sky_height` sets its height to (1 - sky_scale) x the
camera's, which looks the same as the disc's 2.5x dome about the origin.

## Car unlocks [V]

The disc locks its cars as the N64 does. 0x8C02D434 is the N64's func_800F2A28 (docs/unlocks_plan.md §1.3): the car
table 0x8C12C7E4 + player x 13 gets cars 0-5 always and cars 6-12 from the profile's coin totals with the same
thresholds (all 48 race silver, 24 and 36 race gold, all 32 stunt silver, 16 and 24 stunt gold, every coin). It counts
the coin masks with 0x8C02C958 over the same profile offsets (+0xE8 + t x 0x60, +0x548 + s x 0x40). The word at
0x8C0BEEBC equal to 1 opens all 13 (the N64's 0x801164C0).

## Not done

- In-game checks of menus and thumbnails (`list_image` decodes the RGBA load lists), wings, battle arenas.

## Tools

`dc_cdi.py` (image listing and extraction; `pack` extracts from rush2049_dc.pak), `dc_model.py` (containers, textures to PNG), `dc_names.py` (name pairs),
`dc_tables.py` (table program), `dc_sounds.py` (sound pairs, `--banks`), `dc_check.py` (converted files vs the N64),
`dis_dc.py` (SH-4 disassembly: `dis`, `func`, `refs`, `calls`, `str`), `view49.py` (renders N64 model objects to
PNG), `cpp_test/dc_test.cpp` (`dc_build.bat`; modes: convert files, `track`, `audio`, `n64` (the N64 ROM's own files)).
