# Dreamcast Rush 2049 disc as a 2049 source

Tags: **[V]** verified in data or code, **[I]** inferred. Addresses `0x8C......` are the disc executable's
(`1ST_READ.BIN`, loaded at 0x8C010000, little endian SH-4).

## Status (experimental)

The Games tab takes a Dreamcast disc image (.cdi, .gdi, .iso; USA disc only) in place of the N64 ROM. Its files are
copied into `rush2049_dc.pak` in the app folder; the N64 ROM's copy is kept, and the pack wins while it exists.
Everything reads 2049 through `rush2::rom2049::Source` (include/rush2049_rom.h) in N64 terms: the disc's files are
converted to N64 files and its tables to N64 code segments on the fly, so no consumer knows which game it got.

| Content | State |
|---|---|
| Race tracks 1-6, stunt arenas, obstacle course | convert (`dc_test track`); track 1 raced in game |
| Placement, collision, AI paths | convert; checked against the N64 by `dc_check.py` |
| Code tables (types, cars, PVS, fog, texture animation, battle tuning, engine sounds) | built from the disc |
| Sound effects and songs | disc's own (see Audio); in game the banks load and the track song plays |
| Wing sound | disc sound 0x4D at the N64's pitch [I] |
| Cars | geometry converts, but **unpainted**: the port paints through the N64 CI8 palette, the disc's bodies are RGBA |
| Full-size textures | not used yet: every texture is scaled to fit TMEM |
| Menus, wings, battle arenas in game | untested |

## Disc image and pack [V]

`src/rush2049_dc.cpp`. A .cdi or .iso is one file of 2048-, 2336- or 2352-byte sectors; a .gdi lists track files.
The game's ISO 9660 volume is the first holding `1ST_READ.BIN`, `TRACK1.LZS` and `SELTRK.LZS` with all files inside
the image (a self-booting .cdi also has a low-density copy of the directory with no data behind it). The pack is
`"R49DCPAK"`, u32 version, entries `{name, u64 offset, u64 size, u32 kind}`; kind 0 is the file as stored (.LZS stays
compressed, same LZ as the N64's), kind 1 a song re-encoded as IMA ADPCM (block_samples 8192 per channel, 4-byte
header per block). `known_executable` probes four table addresses so only the disc this map was made for is accepted.

## Model containers [V]

`src/rush2049_dc_model.cpp` (`convert_model`), reader `tools/rush2049/dc_model.py`. Little-endian container, chunk
directory at the end with reversed tags (`DHBO` = OBHD). OBHD records are 0x48 bytes: name[16], f32 radius, u16 kind,
s16 LOD count, 4 x {u16 texture handle, u16 flags, f32 distance, u32 stream}. TXHD 0x30: name, w, h, u8 2, u8 pixel
(0 ARGB4444, 1 RGB565, 2 ARGB1555), s16 -1, u32 data (IMAG-relative), u32 flags (0x04000000 mipmapped), u32 layout
(0x2000 VQ, 0x4000 twiddled, else linear). Twiddled order interleaves y (even bits) and x (odd bits); a rectangle is
squares of its short side in a row. VQ: 2 KB codebook of 2x2 blocks. Object streams are 32-bit commands (top 3 bits):
end, texture (+1 word), conditional (bit 0 not mirrored, bit 1 mirrored; +1 word byte length), vertex (0x20000000 ends
a strip; position / uv / colour from cache slots unless the 0x10000000 / 0x08000000 / 0x04000000 bit is clear). The
disc's loader is 0x8C026104 (mirror flag byte 0x8C12C657), a draw routine 0x8C077F00.

Conversion: each disc texture becomes an N64 RGBA16 texture (RGBA32 for ARGB4444, which has graded alpha) shrunk to
fit TMEM, with its own TXLD load list; strips become F3DEX2 batches of 32 vertices (positions x16, st from the uv with
a per-batch shift, subdivided where the span overflows); conditionals become `E0010003/4`. The layout keeps 8 bytes
between IMAG, TXLD and OBHD: `merge_models` (src/track2049_convert.cpp) maps an address equal to a chunk's end into
that chunk, so a load list starting where the texels end was relocated into the texels (the first track crash).
Files with fixed RDRAM windows (battle HUD 61, 63, weapons 76) are fitted by dropping texture levels until they're
no bigger than the N64's. Object names differ in places: `tools/rush2049/dc_names.py` pairs them
(`src/rush2049_dc_names.inc`).

Collision (`TRACKnROAD`): little-endian header, SEG, NODE, POLY, VERT and MOVER records; the VLIST and LEAF byte
streams are already big endian and copied as they are. Paths are the N64's format. Placement is swapped and renamed.

## Code tables [V]

`tools/rush2049/dc_tables.py` writes `src/rush2049_dc_tables.inc`, a program `src/rush2049_dc_tables.cpp` runs: each
table is copied from the disc to its N64 address in the N64's layout and row order (type rows by name, model handles
by renamed name), strings and pointed-at data going to a heap past the segment. Value maps renumber indices: map 0
model handles, map 1 sound effects (DC -> N64, from `dc_sounds.py`). `--report` compares the result with the N64
segments field by field. Disc addresses: type table 0x8C0BFD80 (0x34 rows), handle names 0x8C0AD8E4, kind
parameters 0x8C0C1744, flip-books 0x8C0C3170, scrolls 0x8C0C38C4, fog 0x8C0BDD56, PVS 0x8C0C49B0, car descriptors
0x8C0BB894, engine sounds 0x8C0B92A0 (0x1C per layer), battle mounts 0x8C0D1DF0, weapon sounds 0x8C0D1D7C.

## Audio [V]

The disc has no MusyX. `src/audio2049_dc.cpp` plays it behind the same `audio2049` calls (N64 effect ids and songs).

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
  func_800AF06C 0x2D/0x45/0x2F = disc 0x8C062D40 0x09/0x3D/0x3E). It emits `src/rush2049_dc_sounds.inc`; every id the
  port plays has a pair.
- **Songs**: 0x8C0BA818 lists 20 files: 18 `.STR` streams, `HighScore.rom`, `Select.rom` (raw mono PCM16, played as
  22050 Hz loops [I]). Per-track songs 0x8C0BA76C, s32 per 2049 track id (0-5 race, 6-13 battle 1-8, 14-17 stunt 1-4,
  18 obstacle; -1 random of 18), the same index as the N64's 0x8010FFD4. The disc has a song for every track where
  the N64 shares 8; `audio2049::set_track` tells the backend the track, and an N64 song becomes the disc's song for
  that track (else for the first track the N64 plays it on; menu song 6 is Select.rom). `.STR` header: u32 1, rate,
  bits, block bytes (0x4000), blocks, data bytes, channels (2), end block, loop block; PCM16 blocks per channel.
- Levels: the streams play at their mastered level (about -10 dBFS RMS), close to the N64 songs after the port's
  boosts; `audio2049_dc.cpp` divides those boosts back out.

## Texture sizes vs the N64 [V]

Same-name textures in track 1 (N64 file 101 vs `TRACK1.LZS`): `AL_BARRIER1` 16x128 vs 128x128, `GOLDEN_GATEA` 64x64
vs 128x128, `BRI_WATER2` 32x64 vs 128x128, `SHEEN0` 128x64 vs 512x512. The disc's textures are 16-bit colour, mostly
64-256 texels a side; the N64 is limited to 4 KB of TMEM and mostly 8-bit paletted. The converter scales them down,
so a Dreamcast source looks like the N64 for now; registering the full-size images as RT64 replacements is the step
that would make it look better.

## Not done

- Car paint: `CARnPJ1-12.LZS` are each car's 12 paint jobs; how the disc colours a body (texture swap, vertex or
  material colour) isn't traced. src/car2049.cpp needs a paint path for RGBA bodies.
- Full-size textures as RT64 replacements.
- In-game checks of menus and thumbnails (`list_image` decodes the RGBA load lists), wings, battle arenas.

## Tools

`dc_cdi.py` (image listing and extraction), `dc_model.py` (containers, textures to PNG), `dc_names.py` (name pairs),
`dc_tables.py` (table program), `dc_sounds.py` (sound pairs, `--banks`), `dc_check.py` (converted files vs the N64),
`dis_dc.py` (SH-4 disassembly: `dis`, `func`, `refs`, `calls`, `str`), `view49.py` (renders N64 model objects to
PNG), `cpp_test/dc_test.cpp` (`dc_build.bat`; modes: convert files, `track`, `audio`).
