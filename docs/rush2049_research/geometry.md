# Track geometry: model containers and how each game draws a track

Tags: **[V]** verified in code and data, **[I]** inferred. Addresses are N64 virtual addresses. Rush 2 functions are
`func_XXXXXXXX` in `analysis/out_disasm/r2.asm`. Rush 2049 functions are in `d49.asm`: use the second copy of main
(the first region at the same addresses is boot-segment overrun).

Tool: `tools/rush2049/model.py`. It holds parsers for both containers, a display-list checker, an emulation of
Rush 2's load-time processing followed by an RSP-style walk, `convert_model` / `convert_track`, and PVS table
helpers. Run `python model.py` from `tools/rush2049` for the self-check (§9).

## 1. Summary

- **Same model format underneath.** Both games ship prebuilt F3DEX2 lists, vertices and texels in one file, with a
  table of named LOD'd objects. They relocate the lists at load with the **same relocator** (`func_8007786C` and
  `func_80096734` are instruction-for-instruction the same). Both use the same `E0 01` conditional op, the same
  mirror-mode processing and the same LOD selection.
  - The containers differ only in how the tables are packaged:
    - 2049 uses a chunk directory and 0x58-byte object records with LODs inline.
    - Rush 2 uses a 10-word header and parallel model and name tables.
- **2049 → Rush 2 conversion is mechanical and lossless for what is drawn.** `convert_track()` keeps every display
  list byte except pointer words. It turns the 2049 tables into Rush 2 tables and fixes one render-state leak
  (texture LUT mode, §6.1). All 6 tracks and all 6 track-object files convert. They pass a full emulated Rush 2
  load (1P/2P × normal/mirror) with every address in range.
- **What makes a track look right is not in the geometry file.** Rush 2 and 2049 both decide which sections to
  draw with a **hard-coded PVS table in main data**, indexed by the placement-file region the camera is in. The sky
  is a per-track special case in code, and fog colour and range are per-track settings. For identical rendering,
  Rush 2 needs these per-slot code-side changes (§8). The PVS converts exactly.

## 2. Rush 2 container [V]

Reader: `func_80077CC0` → `func_80077C38`. Slot record (12 bytes) at `0x8010C258 + i*12`:
- +0 asset index
- +1 model slot (allocated from the counter `0x800D5788`)
- +2 current mirror state
- +4 data pointer

Per slot, `func_80077C38` stores {pointer, count} pairs at:
- `0x80118D78` models
- `0x80119018` names
- `0x80119220` textures
- `0x80119428` palettes

### 2.1 Header (10 × u32)

| Word | Meaning | Evidence |
|---|---|---|
| [0] | model table offset (count [4], 0x34 each) | `func_80077C38` |
| [1] | name table offset (count [4], 0x18 each); always the last thing in the file | `func_80077C38` |
| [2] | texture table offset (count [5], 0x20 each) | `func_80077C38` |
| [3] | palette table offset (count [6], 0x18 each) | `func_80077C38` |
| [4] | model count = name count | |
| [5] | texture count (named textures only, not all textures used) | |
| [6] | palette count (0 in cars) | |
| [7], [8] | start and end of the texture-load display lists (linear relocation range) | `func_80077CC0` → `func_80077B38(base+[7], base+[8], base)` |
| [9] | 0 in every file. Not read by the loader. | |

**Observed file order (all 12 tracks):**
1. header
2. texels and palettes (0x28..[7])
3. texture-load lists ([7]..[8], where [8] = [0])
4. model table
5. texture table
6. palette table
7. vertices + model lists
8. name table

Only the offsets matter to the loader.

### 2.2 Model record (0x34)

`u32 lod_count` (1..4), then 4 × `{u16 texture handle, u16 flags, f32 distance, u32 display list}`.
- Unused LOD slots repeat the flags with a zero list.
- Handle `h = (slot << 10) | index`. The record is at `*(0x80118D78 + (h>>10)*8) + (h & 0x3FF)*0x34`.

LOD flags read by the draw function `func_8007AA48`:
- `0x1`: texture swap
  - uses the node's per-LOD texture (node +0x14+lod*4) or, with `0x8000`, the texture record of the LOD's handle
  - via the texture-load builder `func_8007825C`
- `0x2`: post list (`func_80078190` with a caller argument)
- `0x4`: emit PRIM colour from the node
- `0x8000`: the handle field is valid (set at runtime)

Data:
- Every Rush 2 track LOD has flags 0.
- Cars use `0x1` on 22 parts.
- Only one track model has 2 LODs. Track sections use one LOD with distance 2000.

### 2.3 Name record (0x18)

`char name[16]`, `f32 radius`, `u16 kind`, `u16 0`.
- `kind` is the breakable class (e.g. `FENCEO1` 4, `NYTREEHTO1` 5, `GLAMPHITO1` 7); 0 for track sections.
- **Name i belongs to model i.** Lookup `func_8005BE3C` → `func_8005BCB4` is a binary search per slot with comparator
  `func_80058C6C` (15-character `strncmp`), so names **must be sorted** (byte order, first 15 characters).
- It returns `(slot<<10)|index`, or 0xFFFF / −1 if no slot has the name.

### 2.4 Texture record (0x20) and palette record (0x18)

**Texture record:**
- `char name[16]`, `u16 w`, `u16 h`, `u8 fmt`, `u8 siz`, `s16 palette index` (into the same slot's palette table,
  −1 = none), `u32 data`, `u32 flags`.
- `data` points either at texels or at a **texture-load list** (e.g. NYONE `CHKPNT` → a list in [7]..[8] that starts
  with `FD…`). The loader adds the base to +0x18.
- Fields read by `func_8007825C`: +0x10, +0x12, +0x14, +0x15, +0x16, +0x18, +0x1C.

**Palette record:** `char name[16]`, `u32 0x000F8000` (or `0x00FFC000` in 2049), `u32 palette data`. The loader
adds the base to +0x14.

These tables hold only textures that code looks up by name, through `func_800601F8`. Examples:
- `CHKPNT` and `FINISH`, used by `func_8008F080`
- sign colours `NYSIGGRN`/`NYSIGRED`
- cone textures

Ordinary textures are loaded only by the lists.

### 2.5 Load processing (`func_80077CC0`)

1. **`func_80077B38` (linear scan of [7]..[8]).** Every 8 bytes it rebases `G_SETTIMG` (FD), and `G_RDPHALF_1` (E1)
   when the next command is `G_BRANCH_Z` (04). It rebases nothing else, so **texture-load lists must not contain VTX,
   DL, MTX or MOVEMEM pointers.**
2. **For every model and LOD:** add the base to the list pointer, then run **`func_8007786C`** on the list. The
   relocator:
   - **Rebases** `w1 = (w1 + base) & 0xFFFFFF | (w1 & 0x0F000000)` (segment argument −1) for:
     - 01 `VTX`
     - DD `LOAD_UCODE`
     - DE `DL`
     - DA `MTX`
     - DC `MOVEMEM`
     - FD/FE/FF `SET*IMG`
     - E1 when the next op is 04 or DD
   - **Ignores** ops 0x09–0xBF and 0xC0–0xD5 entirely.
   - **Does not enter `G_DL` targets**; it just steps past them, including `DE 01` branches.
   - **Follows `G_BRANCH_Z`** to the saved E1 address and drops the fall-through.
   - **Ends at `G_ENDDL`.**
   - **On the first `G_VTX` of the list**, calls `func_80077810(vertices, mirror)` (§4.2).
   - **Rewrites the conditional op** (§4.1).
   - **Consequence:** every pointer in a model list must be in a top-level LOD list. A list reached only through
     `G_DL` is never rebased, so it may hold only texture-load SETTIMGs (inside [7]..[8]) or no pointers at all.
     Every Rush 2 file and every 2049 file obeys this (self-check).
3. **Add the base to** texture +0x18 and palette +0x14.
4. **`func_800776DC`** (mirror state, §4.2).

## 3. Rush 2049 container [V]

Loader: `func_80096CA8` (entry point; the plan's `func_80096CBC` is inside it). Word 0 is the offset of a directory of
`{tag, offset, size or count}` entries. The loader makes them absolute (`func_80096C28`/`func_80096BBC`) and stores
per-slot pointer and count pairs:
- OBHD → `0x801161F4`
- TXHD → `0x80151AE8`
- PLHD → `0x80138670`
- PTHD → `0x801392D0`/`0x801392D4`, only for file ids 101–119 (tracks and arenas)

| Chunk | Unit | Contents |
|---|---|---|
| IMAG | bytes | texels and palettes |
| TXLD | bytes | texture-load lists. Their SETTIMG (and E1+04) are **IMAG-relative**, rebased by `func_80096A00`, the same scan as `func_80077B38` |
| OBHD | count × 0x58 | objects |
| PLHD | count × 0x18 | palette records, identical to Rush 2's. +0x14 is IMAG-relative |
| TXHD | count × 0x24 | the Rush 2 texture record + one u32. +0x18 is IMAG-relative (texels or a TXLD list) |
| OBJS | bytes | vertices and object lists (file-relative pointers) |
| PATH | bytes | animation paths (tracks) |
| PTHD | count × 0x24 | path headers: `name[16]`, u32, u32, u32 ptr, u32 ptr into PATH, s32. +0x18 and +0x1C are file-relative, rebased by the loader |

### 3.1 OBHD record (0x58)

`char name[16]`, `f32 radius`, `u16 kind`, `s16 lod_count`, then 4 × `{u16 texture handle, u16 flags, f32 distance,
u32 display list, u32 vertices}`.
- **The first 0x18 bytes are exactly Rush 2's name record.**
- **The LOD entry is Rush 2's LOD entry plus a vertex pointer.**
- **The name is 16 bytes, not 12** as the plan says.
  - 2049's lookup `func_80092E2C` → comparator at `0x80095120` compares 15 characters, and file 102 has
    `TRACK2WATER01`/`TRACK2WATER02`.
  - Bytes after the NUL are stale (the "flags" word 0x00077A00 in the plan is name residue).
- Names are sorted (binary search).

**LOD flags** read by 2049's draw function `func_8009C8F0`:
- `0x1`, `0x2`, `0x4`, `0x8000`: same as Rush 2
- **`0x10`:** lit
  - `D9FFFFFF 00020000` before the list, `D9FDFFFF 0` after
  - default is unlit
- **`0x8`:** set on 13–32 objects per track (translucent ones such as `GLASSWALLG1`, water), but **no 2049 code reads
  it**. The only `lhu 0x1A` readers are `func_8008B000`, `func_8009C8F0`, `func_800BEAA0` and `func_800E7D0C`, and
  none tests 8. It looks like a tool flag. Unused LOD slots repeat it.

**Vertex pointer (+0x24 per LOD):** rebased by the loader. The consumer was not identified. Rush 2 has no such field.
[I]

**Track data:** no LOD has 0x10, and `kind` is 0 everywhere. Some sections have 2 LODs (e.g. `TRACK1L54064` at 700
and 2000). Most have distance 0, meaning no distance cut-off.

### 3.2 Load processing

1. Directory fix-up.
2. **TXLD** scan relative to IMAG.
3. **TXHD** +0x18 and **PLHD** +0x14 += IMAG.
4. **Every OBHD LOD:** +0x20 (list) and +0x24 (vertices) += base, then the relocator walk with the same flags as
   Rush 2.
5. **PTHD** +0x18/+0x1C += base.
6. **`func_800965BC`:** mirror processing, same as `func_800776DC`.

## 4. Shared display-list conventions [V]

### 4.1 The conditional op `E0 01 cccc` / `tttttttt`

**Encoding:** G_SPNOOP (0xE0) with byte 1 = 1. The low halfword is a condition bit number. w1 is a file-relative
target, normally further on in the same list.

**Semantics at load (relocator case 0xE0, `0x800779F4` / `0x800968BC`):**
- If load flag bit `cccc` is **clear**, the command is rewritten to `DE 01 0000` / `target+base`: a branch with no
  push, so the RSP skips to the target.
- If the bit is set, the command stays a no-op and the next commands run.

So the bytes between the op and its target are drawn **only when condition c holds**. The decision is baked in when
the track is loaded, and Rush 2 loads the track per race.

**Load flags** (`func_80077CC0` / `func_80096CA8`):

| Bit | Value | Condition | Rush 2 | 2049 |
|---|---|---|---|---|
| 1 | 0x02 | one player | `h 0x8010C3E2 == 1` | `h 0x8014A108 == 1` |
| 2 | 0x04 | two players | `== 2` | `== 2` |
| 3 | 0x08 | not mirrored | `b 0x80119848 == 0` | `b 0x80152570 == 0` |
| 4 | 0x10 | mirrored | `b 0x80119848 != 0` | `b 0x80152570 != 0` |

- **2049 mirror flag:** 0x80152570 selects AI-path file `0x9E+track` (normal) or `0xB1+track` (mirrored) in
  `func_800BB9B0` at 0x800BBCC0. So **2049 does ship mirrored AI paths, files 177–182** (note for the AI-path work).
- **What the data uses:** only conditions 3 and 4, for mirror-only and normal-only geometry (signs, arrows, finish
  banners).
  - Rush 2 tracks 1, 3, 5, 8 and 10 use them.
  - All six 2049 tracks use them (6–56 ops each).
  - Neither game's track data uses the player-count conditions.
  - With 3–4 players, neither bit 1 nor bit 2 is set.

### 4.2 Mirror mode

1. **Vertex chain.** `func_80077810` / `func_800966D8` walk the vertex flag halfword (Vtx +6) from the first VTX of
   each list:
   - bit 15 = another vertex follows
   - bit 14 = already processed
   - bits 0–8 = texture width
   - When mirrored and the width is non-zero: `s = width*32 − s`, flipping text horizontally.
   - Rush 2 tracks use widths (296–502 vertices per track). **2049 track vertices all have width 0.** Both chain
     every vertex of an object with bit 15.
2. **Winding.** `func_800776DC` → `func_80077518(list, 2, func_8006121C)` (2049: `func_800965BC` → `func_800963E8`
   → `func_8008AD6C`, same code). When the mirror state differs from the state the slot was processed in, every LOD
   list is walked again, following `G_DL`. TRI1/TRI2/QUAD vertex order is swapped to keep front faces.
3. **Geometry.** The world itself is mirrored by the camera matrix, not the file. [I]

### 4.3 Addressing

- Object pointers are file-relative with segment nibble 0. Rush 2 keeps every pointer file-relative.
- In 2049, TXLD SETTIMG and TXHD/PLHD data pointers are IMAG-relative; everything else is file-relative.
- Neither game's model lists use BRANCH_Z or E1. Lists are contiguous and end at their first `G_ENDDL`.

## 5. How each game draws a track [V]

### 5.1 Scene nodes come from the placement file

**Rush 2:**
- `func_80081B58` finds the placement tree.
- `func_80081790` walks it. Records are 0x64 bytes; 2049's are 0x68.
- Fields: +0x40 node flags, +0x44 next, +0x46 child, +0x4C..+0x60 box.
- For every record it creates one scene node (`func_8007F6DC`, 0x38-byte nodes at `0x800D9E90`) with the model
  handle looked up by the record's name, and node flags = record +0x40.
  - Track sections are records named like their model (`NYONEBPARK1`, `TRACK1L53574`) with flags 0x40.
- **Node order:** the top-level `next` chain first, then children. The first node index is stored at `0x8010C164`.

**2049:** does the same (`func_8009EBC0` uses `0x80149B80`, records 0x68, nodes of 0x44 bytes at `0x8012E700`, first
index `0x80149D90`).

Placement details are left to the placement work.

### 5.2 PVS: which sections are drawn

**Rush 2: `func_8007C27C`, every frame per view.**
1. **Region.** `func_8007C06C` finds the camera's region: the **ordinal of the top-level chain record** whose box
   contains the camera (closest if several).
2. **Mask.** A per-track table of 16-byte entries gives a 128-bit mask:
   - table bases: jump table `0x800CF9A4` → `0x800C6538, 0x800C6A78, 0x800C7048, 0x800C7788, 0x800C7F38, 0x800C8588,
     0x800C8B28, 0x800C9268, 0x800C9508, 0x800C9818, 0x800C9BA8, 0x800C9F18`
   - one entry per region
   - two big-endian u64, bit i = section i for i < 64, then sections 64–127
3. **Apply.** For the first `N = u8[0x800CA1A8 + track]` nodes from `0x8010C164`, it clears or sets node flag
   **0x400**. The scene walker `func_8007B518` skips nodes with 0x400.

Details:
- With no region found, a default table at 0x800CA1B8 is used.
- A non-zero byte at 0x8010C178 hides every section (2049: 0x8014061A).
- LA (5) has a second 16-byte mask at `0x800C8578` that triggers `func_8007BFBC` on its sections. [I: purpose]

**2049: `func_8009EBC0` does the same.**
- region search over `*0x80149B80`
- tables via jump table `0x80123B14` (19 track ids). Race tracks 1–6: `0x8011B898, 0x8011BFE8, 0x8011C738,
  0x8011CE88, 0x8011D618, 0x8011DC88`.
- entries of **four big-endian u32**, bit (i & 31) of word i >> 5
- count `u8[0x8011E748 + id]`
- hide bit is node flag bit 31

**Counts equal the chain length in both games:**
- Rush 2: 84, 93, 116, 123, 100, 90, 116, 42, 49, 57, 55, 41
- 2049: 117, 117, 117, 121, 103, 122

Exceptions where the chain runs longer than the count (the extra records are never hidden):
- Rush 2 ALCATRAZ: chain 101, count 100
- 2049 track 6: chain 126, count 122

The diagonal (own region visible) is set for 85–100% of regions in both games, which confirms the bit order.

**Conversion:**
- **Rush 2 entry** = (2049 w1:w0, 2049 w3:w2) as two u64 (`pvs_rush2_bytes`).
- **Capacity:** Rush 2's table for slot t has room for exactly count_t regions. Only NYTWO (slot 3, 123) can hold
  every 2049 track (max 122) in place. Any other slot needs the table moved (§8).

### 5.3 Model draw: `func_8007AA48` (Rush 2) vs `func_8009C8F0` (2049)

**Same in both:**
- **LOD choice:**
  - `dist = |node position − camera|`
  - culled if the last LOD's distance ≠ 0 and dist > that × node scale (Rush 2 node +8, 2049 node +0xC)
  - otherwise step down while dist < previous LOD distance × scale
- **Node flag 0x2000:** PRIM colour from the node.
- **Node flag 0x4000:** ENV colour from the node.
- **LOD flag 0x1:** texture swap.
- **One `G_DL`** to the LOD list.
- **`G_SETOTHERMODE_H(TEXTLUT = RGBA16)` once per frame** after the first model (flags `0x80112490` / `0x8017A638`).

**Differences:**

| | Rush 2 | 2049 |
|---|---|---|
| Lighting | Per **node**: on unless node flag 0x800, and only while lights exist (`0x800FAEB0 ≥ 0`). The last state is cached in `0x800E7DE1`. **In races there are no lights:** the count is reset to −1 by `func_8007FAB4`, and `func_80053B60` (add light) is called only from the menu overlay `func_803ADB2C`. So tracks and cars are drawn unlit. | Per **LOD** flag 0x10; otherwise unlit. |
| Extra node features | none | 0x80000: environment map (LookAt + TEXTURE_GEN, car sheen). 0x200000: alpha fade by distance. |
| TLUT in lists | Rush 2 lists never change TLUT, and its cars rely on the per-frame RGBA16. | 2049 lists set `TEXTLUT` before every texture. Many leave it at NONE (§6.1). |

**Render state in the lists:** both games' lists set their own render modes and combiners.
- `E200001C C8112230` / `C8113278` / `C8104A50`: 2-cycle fog modes, opaque, decal and translucent
- `FC127FFF FFFFF238`
- mip-mapping via `E3000F00 00010000` + `FC26A004 1FFC93F8`
- texture scale `D7…`

The command sets are the same in both games, except that 2049 also sets RGB dither (`E3001801` 0x80/0xC0).

### 5.4 Culling and visibility summary

**Rush 2 and 2049:**
- PVS mask per camera region (§5.2), in code tables.
- Per-LOD maximum distance. Rush 2 sections use 2000; 2049 sections mostly use 0 (none).
- Fog in the render mode, with per-view fog colour and range:
  - Rush 2 `func_8007C624`: view struct +0x50 colour, +0x4C/+0x4E range
  - 2049 `func_8009F058`: +0x44, +0x40/+0x42
  - Both come from per-track settings, not the geometry file. [I: Rush 2 table 0x800C1D7C]
- Placement-flag driven RSP box culls (`G_CULLDL` on 8 box vertices, node flags 0x10/0x20/0x380000) in the scene
  walker. Placement concern.

**Split screen:** per-view hide bits 0x100<<view in both. The PVS runs per view. No geometry difference.

### 5.5 Sky

**Rush 2 `func_800A45A8`:**
- **VEGAS (0):** looks up model **`SKYO1`**. For each view it creates a matrix node (`func_8007F91C`) and a node
  hidden in the other views.
- **STUNT (11):** the same with `STUNTSKYO1`.
- **ATARI (10):** no sky.
- **All other tracks:** a procedural 24-piece dome (`func_80054010`) textured with `SKY01` or `SKYFOUR` (chosen at
  random), with vertex colours from 0x800C1C98 / 0x800C1CB4.
- **Only built while** `b 0x800D1FA8 < 3` and `h 0x8010C3E2 < 2`.

**2049 `func_800BB69C`:** for race tracks, looks up **`SKYSKY`** and creates the same per-view matrix and node pair
(`func_800A7D6C`, `func_8008E26C`). This is **the same scheme as Rush 2's VEGAS path**, for any player count.

## 6. Differences that change what the converted file must contain

### 6.1 TLUT leak (fixed in the file) [V]

**Problem:**
- 13–33 objects per 2049 track (and 1–8 per object file) **end with `TEXTLUT = NONE`**:
  26–66 list × mirror-state end states.
- No Rush 2 track list does this.
- In Rush 2, the next model drawn would sample colour-indexed textures without a TLUT. That is the "green car" bug
  from the wings work.

**Fix (`convert_model(restore_tlut=True)`):**
1. Append a copy of each such list with `E3001001 00008000` inserted before its `G_ENDDL`.
2. Pointers into the list itself (conditional targets) are moved to the copy.
3. Point the LOD at the copy. The original list stays in the file, unreferenced.

**Why a copy:**
- A wrapper calling the original would not work: the relocator does not enter `G_DL`, and the mirror winding pass
  would flip the shared triangles twice.
- Replacing the original `G_ENDDL` by a branch would make the relocator walk on into the next object's data.

**Remaining leak:** RGB dither is also left at 0xC0 (disabled) by 2049 lists. Rush 2 sets it once per frame from a
setting (`func_800B45B4`, 0x8002332C). This is cosmetic only and is not fixed.

### 6.2 Fully compatible as is

- display-list bytes, vertices (including the chain flags), texels, palettes, render modes, fog flags, mip-mapping,
  combiners
- the mirror conditionals and mirror processing
- LOD distances and LOD selection
- texture and palette records (TXHD's first 0x20 bytes, all of PLHD)
- names, radii, kinds

### 6.3 Not representable in the Rush 2 container (dropped)

- **LOD flag 0x8:** never read by 2049.
- **LOD flag 0x10:** not used by any track LOD. Rush 2 races are unlit anyway.
- **OBHD per-LOD vertex pointer:** consumer unknown [I].
- **TXHD +0x20:** not read by the texture-load builder.
- **PATH/PTHD:** kept in the file (with the 2049 directory, §7) but nothing in Rush 2 reads them.

## 7. Conversion: `convert_track(data_2049)` / `convert_model(...)`

### Output layout

1. **Header (0x28).** [7]/[8] cover the TXLD lists. [9] = 0.
2. **The 2049 file from offset 8, shifted by +0x20.** This covers IMAG, TXLD, OBHD, PLHD, TXHD, OBJS, PATH, PTHD and
   the 2049 directory. The directory's offsets are updated, and PTHD +0x18/+0x1C are shifted, so
   `report['2049 directory']` finds the animation data.
3. **TLUT-restoring list copies, and one `G_ENDDL` list** for dummies.
4. **Tables:** model table, texture table, palette table, name table.

### Pointer rewriting

- **Object lists:** exactly the words the shared relocator rebases, found by running its walk, plus conditional-op
  targets. Each is shifted by +0x20 (segment nibble kept).
- **TXLD SETTIMG (and E1+04):** += IMAG offset + 0x20, which makes them file-relative.
- **Texture/palette record pointers:** += IMAG + 0x20.

### Tables

- **Models:** OBHD LODs → model records (`flags & 0x8007`).
- **Names:** OBHD's first 0x18 bytes.
- **Order:** sorted by 15-character name.
- **`rename`:** optional, e.g. `SKYSKY` → `SKYO1`, which `convert_track` does by default.
- **`dummies`:** names to add as empty models, for names Rush 2 code looks up without checking.

### Sizes

| | Bytes |
|---|---|
| 101 | 0x12A6A4 |
| 102 | 0x10CCC8 |
| 103 | 0x104698 |
| 104 | 0x12AC6C |
| 105 | 0x116D3C |
| 106 | 0x18B8E0 |
| Rush 2's largest | 0x117898 |

The heap issue is plan §6.1.

The same function converts the per-track object files 82–87 (gondolas, trolleys, windmills…). They pass the same
checks.

## 8. Rush 2 code support needed for full fidelity

1. **Heap above 4 MB.** Plan §6.1.
2. **PVS for the replaced slot.**
   - Write `pvs_rush2_bytes(pvs_2049(q, k))` over the slot's table and set `u8[0x800CA1A8 + slot]` = 2049 count.
   - Only slot 3 (NYTWO) has room in place. Otherwise, put the table in spare RDRAM and repoint it: patch the slot's
     case in `func_8007C27C` (jump table `0x800CF9A4`), or hook it.
   - If the slot is LA (5), also clear its second mask at `0x800C8578`.
   - Without this, Rush 2 would hide converted sections using the old track's PVS.
   - All-visible is a usable fallback: every mask = (1 << N) − 1. It costs fill rate; 2049 shows 12–24 sections per
     region.
3. **Sky.** Ship the sky as `SKYO1` (done by `convert_track`) and make the slot take the VEGAS branch of
   `func_800A45A8` (track id test at 0x800A4644), unless the slot is VEGAS itself. Rush 2 builds that sky only for
   fewer than 2 players; 2049 draws it in split screen too.
4. **Fog colour, fog range and clear colour** for the slot, from 2049's per-track settings (not located yet). [open]
5. **Required names.**
   - `func_8008F080` needs `<prefix>FINISH` (normal) or `<prefix>FINISHB` (mirror) **models** and `CHKPNT`/`FINISH`
     **textures**, and uses the results unchecked.
   - Other per-track name lookups (plan §6.3) likewise.
   - Supply models through `rename` (e.g. map the 2049 section holding the banner) or `dummies`, or skip the code for
     converted slots.
   - Texture-name dependencies cannot be satisfied by dummies.
6. **Animated objects and animated textures.**
   - PTHD/PATH (trap doors, trolleys, gondolas…) and the 2049 named textures (`AL_BARRIER1*`, `BRI_WATER2`,
     `GOLDEN_GATEA/B`, `SHEEN0`) are driven by 2049 code. They render static in Rush 2.
   - Their geometry (track file and files 82–87) converts fine.
7. **Optional: restore RGB dither** after converted models (cosmetic).

## 9. Self-check (`python model.py`, all OK)

**Rush 2 tracks 0–11 and cars PICKUP, INTEG, VWBUS, FORM1, GT90, DEW:**
- parse, with sorted names
- every pointer in range
- texture-load lists hold only SETTIMG pointers
- no list reached only through `G_DL` holds VTX, DL, MTX or MOVEMEM pointers
- **emulated load** (`func_80077B38` + `func_8007786C` + table fix-ups) for 1P/2P × normal/mirror, then an RSP walk
  (DL push/branch): every address inside the file
- 0 TLUT leaks

**2049 files 101–106 and 82–87:**
- the same checks in 2049 terms
- names sorted (15 characters)
- TLUT leaks: 30, 26, 30, 56, 38, 66 / 16, 16, 2, 6, 0, 4

**Converted files:**
- the Rush 2 checks and emulated loads pass
- every list is byte-identical to the 2049 original except pointer words and the inserted TLUT restore
- LOD counts, distances, flags and radii match
- the embedded 2049 directory still describes the chunks
- 0 TLUT leaks

**PVS:** Rush 2 and 2049 tables decode with mostly-set diagonals. The 2049 → Rush 2 encoding round-trips.

**Negative test:** corrupting one VTX pointer in a converted file is reported by both the checker and the emulated
load.
