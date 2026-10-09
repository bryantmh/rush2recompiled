# Animated track textures (blinking barriers, chasing lights, scrolling water)

Tags: **[V]** verified in disassembly or data, **[I]** inferred. Addresses are 2049 main (`tools/rush2049/out/d49m.asm`)
unless marked R2 (`analysis/out_disasm/r2.asm`).

Port: `src/rush2049/track2049_texanim.cpp` (runtime), `convert_tex_anims` in `src/rush2049/track2049_convert.cpp` (patch sites,
`ConvertedTrack::tex_anims`), `tools/rush2049/texanim.py` (Python twin, writes `out/trackK/texanim.txt`, compared by
`cpp_test/build.bat`). Test: `tools/rush2049/cpp_test/texanim_build.bat [png dir] [seconds]`.

## 1. How Rush 2049 does it [V]

No flip-book data is in the track files. Two per-track tables in main data name textures, and code rewrites the
**texture-load lists (TXLD)** of the loaded geometry once per rendered frame. Every model list calls those load lists
with `G_DL`, so one rewrite changes every use of the texture (all LODs, mirror-conditional copies, split screen views).

- Set-up `func_800BDAA8` (race start, called at 0x800FB834); update `func_800BD2C8` (once per frame from the game
  step, 0x800FACFC / 0x800FC060). Both index by track id byte `0x8014978C` (race track K = id K − 1).
- Gate: skipped when `0x801174B4 & 8` (multiplayer) and no Expansion Pak (`0x80156994 == 0`).
- Texture lookup by name: `func_800B24EC` over all loaded model slots (TXHD tables `0x80151AE8`), first hit.
- Frame time: `f32 0x8002EB94` = vblanks this frame (`s32 0x8002EB98`, clamped to 5) × `0x8002AFB8` (1/60).

### 1.1 Flip-books: table `0x8011A31C` (ptr per track id)

Records 0x14 bytes, list ends at count 0:

| Off | Field |
|---|---|
| +0 | s16 frame count |
| +2 | s16 start = **target**: the texture whose load list is rewritten is `frames[start]`; also the initial frame |
| +4 | s16 direction (≠0 forward, 0 backward) |
| +6 | s16 current frame (runtime) |
| +8 | f32 timer (runtime) |
| +C | f32 period (s) |
| +10 | frame table: 12-byte entries {char *name, TXHD record (set-up), texel address (set-up)} |

Set-up per frame: record = lookup(name); texel address = record+0x18 if record flags (+0x1C) & 0x08000000 (data is
texels), else the w1 of the **first `G_SETTIMG`** of the record's load list (`func_800BDA24`). Then current = start,
timer = period. FIREGEN frame tables (`0x8011905C`) are skipped on race tracks without the Expansion Pak.

Per frame: `timer -= dt; if (timer <= 0) { timer = period; current = current ± 1 (wrapping); then patch }`. Patch:
target record flag 0x08000000 → record+0x18 = frame texels; else `func_800BD080` writes the frame texels into w1 of
the first `G_SETTIMG` of the target's load list. The palette load (second `G_SETTIMG` + `G_LOADTLUT`) is untouched, so
frames share the target's palette.

### 1.2 Scrolls and palette cycles: table `0x8011A840` (ptr per track id)

Records 0x14 bytes, list ends at name 0: `char *name, s16 position, s16 wrap, s8 speed, u8 kind, s16 rate, f32 timer,
data`. Set-up: timer = `0x80123E58` (1/30); data = TXHD+0x18 (load list) for kind ≥ 9, else PLHD+0x14 (palette,
lookup `func_800B0F68`). Per frame: `timer -= dt; if (timer > 0) skip; timer += rate × 0x80123E28 (1/30)`; then by
kind (jump table `0x80123E2C`): 0–8 palette rotations/swaps/fixed patterns (not used by race tracks), **9 / 10**:
`position += speed × vblanks`, wrapped once into [0, wrap), then `func_800BD104(list, s = position >> 2, -1, ...)` (9)
or `(list, -1, t = position >> 2, ...)` (10). `func_800BD104` walks the list to `G_ENDDL` and, on each
`G_SETTILESIZE`, replaces uls (bits 12–23 of w0) or ult (bits 0–11); for each tile smaller than the first
(`lrs+4` / `lrt+4` smaller) the value is halved until it is not, which scales it to the mipmap level. lrs/lrt are never
changed.

At ≤ 30 fps (2049's race frame rate) the timer never waits, so the position moves `speed` per 1/60 s.

## 2. What animates on race tracks 1–6 [V]

From the tables and the textures present in the converted geometry (track file + object file 82+K + file 78):

| Track | Flip-books | Scrolls |
|---|---|---|
| 1 | AL_BARRIER1 (3 frames, 0.25 s); AL_BARRIER1N/BN/CN/DN (4 frames each, phases 0–3, 0.25 s) | BRI_WATER2 t −2, GREENFLAME2 s −16 |
| 2 | AL_BARRIER1; LIGHT1–10_GRN and LIGHT1–10_RED (10 frames, phase = light index, **backward**, 0.075 s: a chase) | AROWGRN s +64, BRI_WATER2 t −2, GREENFLAME2 |
| 3 | AL_BARRIER1; LIGHT1_RED_MED only (cycles all 10 frames) | AROWGRN, GREENFLAME2, MAGMA t +2, VAPORS t +8 |
| 4 | AL_BARRIER1N (4 frames); WARN90ON/OFF (2 frames, 0.5 s) | GREENFLAME2 |
| 5 | AL_BARRIER1 | BRI_WATER2, AROWGRN, GREENFLAME2 |
| 6 | AL_BARRIER1; LIGHT1–10_PURP (chase) | GREENFLAME2 |

Wrap 1024 (512 for GREENFLAME2): a full texture period of 64 (32) texels. At 30 fps the effective step intervals are
0.267 s (0.25), 0.1 s (0.075) and 0.533 s (0.5). Not animated by 2049 although present: BRI_WATER2 on track 4 and
BRI_WATER3 on track 6 (their tables don't list them). ROKTFLAME and FIREGEN* aren't track textures.

## 3. Rush 2's own mechanism (for comparison) [V]

R2 `0x800C5D2C[track]` → `func_8008AF48` (set-up) / `func_8008A9C8` (update), jump table R2 `0x800CFB6C`: the same 11
kinds as 2049's scroll table, but with integer timers (`s16` timer at +0xC, `s8` period at +0xA, decremented by the
vblank count `0x8002301C`) and kinds 9/10 pass the position unshifted (R2 `func_8008A804`). It has no flip-book table.
So 2049's records can't be fed to it unchanged, and the flip-books need new code anyway; the port runs 2049's update
itself. `track2049.cpp` keeps `0x800C5D2C[host]` = 0.

## 4. The port

- **Converter** (`convert_tex_anims`): reads both tables for track K, finds each named texture in the converted
  geometry's texture table and records geometry offsets: the target's first `G_SETTIMG`, every frame's texel address
  (file-relative), and every `G_SETTILESIZE` of a scrolled load list with its w1. Records with a missing texture,
  palette kinds and targets without a load list are left out. The converted bytes are unchanged.
- **Runtime** (`texanim_tick`, per physics tick): finds the host geometry's load address (slot record index
  `0x8010C400`, record `0x8010C258 + 12·i` = {u8 asset 0x33+slot, …, ptr data}; falls back to scanning the records),
  checks every site (opcode, and a frame address or the original w1), then runs 2049's update as 30 fps frames
  (2 vblanks each) from accumulated tick time. Frame addresses are rebased like Rush 2's loader rebases `G_SETTIMG`
  (`((w1 + base) & 0xFFFFFF) | (w1 & 0x0F000000)`, R2 `func_80077B38`).
- **Differences:** runs in split screen without the 2049 Expansion-Pak condition; scroll positions restart from the
  table each race (2049 keeps them in RAM across races).

## 5. Test results

`texanim_build.bat`: for all six tracks every site is the first `G_SETTIMG` / every `G_SETTILESIZE` of its target's
load list, every target list is called by model lists (1–53 `G_DL`s), no list outside the animated set loads a target's
texels (except track 3's unused LIGHT3_RED_MED), the RDRAM words follow 2049's 30 fps sequence for 3 s, and a corrupted
site disables the animation. PNG grids (rows = textures, columns = 0.1 s or 1/30 s) are written per track.
