# San Francisco Rush (Rush 1) tracks in Rush 2 Recompiled

Research notes for the SF Rush track port (`src/track1*.cpp`, `src/rush1_rom.cpp`, prototype `tools/rush1/track1.py`).
Addresses are Rush 1 N64 virtual addresses unless marked as ROM offsets or Rush 2. Tags: **[V]** verified in code or
data, **[I]** inferred.

Tools (`tools/rush1/`):
- `r1.py`: ROM reader.
- `dis1.py`: annotated disassembly, written to `out/r1.asm`.
- `decomp1.py`: m2c front end. It needs `tools/rush2049/m2c`.
- `fmatch1.py`: matches Rush 2 functions to Rush 1 functions.
- `track1.py`: the converter prototype. `python track1.py check` runs every structural check.
- `colltest.py`: compares Rush 1 and Rush 2 ground queries.
- `cpp_test/build.bat`: checks that the C++ output is byte-identical to the Python output.
- `shots.py`: launches the game with an input script and takes screenshots.

## 1. ROM

- **Supported ROM:** San Francisco Rush - Extreme Racing (USA).
  - game code `NSFE`, 8 MB
  - SHA-1 `cc62539cb30b180c3c7e0aa927786ed061d8d9ab` (big-endian)
  - The SF Rush tab stores it as `%LOCALAPPDATA%\Rush2Recompiled\rush1.z64`.
- **Graphics microcode:** F3DEX 1.21 (Rush 2 and 2049 use F3DEX2). **[V]**
- **Boot segment:** raw at ROM 0x1000, loaded at 0x80000400, BSS from 0x8001E2E0.
- **Main code:** LZ compressed at ROM 0x7A7930. It inflates to 0x7C550 bytes at **0x8005BB10** (load call at
  0x80000728, `func_80003B18`). The game thread starts at 0x800BBE88. **[V]**
- **Compression:** Rush 2's LZSS variant, with absolute ring positions (`roms.lz(..., relative=False)`). Every asset
  uses it. **[V]**

### 1.1 Asset table [V]

The table holds 72 u32 ROM offsets at **0x800C7C1C**. The loader `func_80098210` indexes it from 0x800C7C38, so
asset id = table index − 7.

| Index | Contents |
|---|---|
| 0-6 | track placement (Rush 2 layout) |
| 7-24 | UI containers (11: car select; 10: **track select dioramas** TRK1O2, TRK_2O2, TRK_3O2, TRK4O2, TRK5O1, TRK6O3, TRK7O5, SIGNO2) |
| 12 | shared race objects and effects: CONE1L1, KEYL1, METERL1, TREEHIT1L1-4L3, **FINISH** texture, SKY01, SKYFOUR, ... |
| 25-35 | cars: BMW, CAMARO, SUPRA, BUGATTI, VWBUS, VIPER, VWBUG, CONCEPT, TAXI, HOTROD, FORM1 (27 parts each) |
| 36 | headerless shared track texture bank (0x37B20 bytes). Loaded right after the track geometry, in the same segment. |
| 37-43 | track geometry |
| 44-50 / 51-57 | collision, forward / backward. Tracks 2, 3, 5 and 7 use the same data for both. |
| 58-64 / 65-71 | AI lanes, forward / backward |

## 2. Model containers [V]

- **Header (0x20 bytes):**
  - names pointer, model count, 0, 0
  - textures pointer, texture count
  - palettes pointer, palette count
- **Records:** the model records start at +0x20. Model (0x34), name (0x18), texture (0x20) and palette (0x18)
  records have Rush 2's layouts.
- **Pointers:** every pointer is an address in the file's own segment (5 = track, 6 = asset 12, 4 = asset 10,
  7 = car). The loader stores each file's base in the segment table 0x80021608, and the lists are drawn with
  `G_SEGMENT`, so nothing is relocated.
- **Track segment:** a track's segment also covers asset 36. Track SETTIMGs point up to 0x37B00 past the end of the
  geometry file.
- **Name record:** +0x14 is the behaviour id, called kind in Rush 2. The ids match Rush 2's: 2 cone, 4 fence,
  6 tree / trash can, 7 sign, 8 key, 9 window, 23 flag. Rush 1 also has 3 (meter) and 5 (PMUNCH).
- **Texture record:** +0x14 is 0. The `CHKPOINT` texture's data is a texture-load list, as in Rush 2.

### 2.1 Display lists [V]

- **Commands in track and diorama lists:**
  - `04` VTX, `06` DL, `B1` TRI2, `BF` TRI1, `B8` ENDDL
  - `B9` / `BA` othermode L / H, `BB` TEXTURE
  - `B4` + `B0` (RDPHALF_1 + BRANCH_Z, near/far detail; tracks 1, 3 and 5)
  - RDP commands `E6`-`FD`
  - No geometry-mode, conditional or matrix commands.
- **Translation to F3DEX2:**
  - Every command is 8 bytes in both GBIs, so lists are translated in place.
  - VTX: `n = (w0 >> 10) & 0x3F`, `v0 = byte1 / 2`, then `01 | n<<12 | (v0+n)<<1`.
  - TRI1 → `05 | w1 & 0xFFFFFF`, TRI2 → `06`.
  - OTHERMODE: `(sft, len)` → `(32 − sft − len, len − 1)`.
  - TEXTURE `on` is shifted left by one.
  - ENDDL → `DF`, DL → `DE`.
- **Mirror support:**
  - Rush 1's mirror walker `func_80097FE0` is the ancestor of Rush 2's `func_80077518`, with the same branch, mirror
    and `CHKPOINT` → `FINISH` banner logic.
  - Vertex flag halfwords (+6) are all 0, so Rush 2's mirrored-text pass leaves them alone.
- **State leaks:** Rush 1 lists switch the texture LUT (22 places) and texture LOD and don't always switch them
  back. Converted model lists end with `E3001001 00008000` (LUT RGBA16) and `E3000F00 00000000` (LOD off).

### 2.2 Conversion

Rush 2 rebases pointers in two ways:
- a walk of each model list. It rebases VTX, DL, SETTIMG and RDPHALF_1+BRANCH_Z pointers, and does not enter `G_DL`
  calls.
- a linear scan of the texture-load range [7]..[8], which rebases SETTIMG and RDPHALF_1+BRANCH_Z only.

So every model list is flattened:
- Called lists are inlined.
- Lists that only set state and load textures stay calls, into the texture-load range. Each one is emitted there
  once.
- A BRANCH_Z always takes the near branch, which is inlined.

Each source file is copied verbatim behind the header, so vertices, texels and palettes keep their offsets.

Names:
- Models whose names Rush 2's placement classifier would treat as objects (FENCEL*, GASIGNL*, ...) get the prefix
  `R1`.
- The finish model (table 0x800C7AA0: TRACK1FINISH2, TRACK2A1, ...) becomes `<prefix>FINISH`.
- An empty model `R1EMPTY` is added.

Named textures:
- Only `CHKPOINT` (renamed `CHKPNT`) and `FINISH` (taken from asset 12) are kept.
- `TRKMAP` (Rush 1's map image) and `GLASS_TL` are left out.

## 3. Placement [V]

**Format:** Rush 2's (`{1, 0x18, name}` header, then 0x64-byte records).

**Walker `func_800829E4`:**
- Every record is a plain model. Its behaviour comes from the model's name-record kind (`func_80081104`, which
  matches Rush 2's `func_8007FFA0`).
- Rush 1 has no name classifier. The only special names are the no-node emitters at 0x800CF748: MARKER, TIME, CCAR,
  FIRECRK, SMALLHOOT, BIGCHEER, BIGCHEER2.
- Children positions are relative to their parent section.
- Field +0x48/+0x4A holds two object parameters; Rush 2's +0x48 is 0.
- Sections carry flag 0x1000, which Rush 2 doesn't use.

**Tree shape (all tracks):**
- The top-level chain holds only sections; its length is the visibility region count, or more.
- Children appear only under sections, are one level deep, and their parents are unrotated.

**Conversion:**
- The chain and its order are kept.
- Rush 1 objects that Rush 2 has map to Rush 2 classes with world positions: CONE1L → CONE1, METERL → METER,
  TREEHIT → TREEHIT, FLAG2L → FLAG2, WINDOWBL → SHATPANE. Rush 2 draws its own model for these.
- Emitters map to Rush 2's: CCAR → CABLECAR, FIRECRK → FIRECRCK, SMALLHOOT → SMLHOOT, BIGCHEER(2) → BIGCHR1(2).
- KEYL, MARKER and TIME are dropped.
- Other objects stay static Rush 1 models: fences, trash cans, gas signs, parking munchers. Cars pass through them.

## 4. Visibility [V]

**`func_80064544`** is Rush 2's `func_8007C27C`:
- region = ordinal of the top-level record whose box holds the camera
- 16-byte masks of two u64, with bit i = section node i
- the hide flag is 0xC00 (Rush 2: 0x400)

**Tables (one per track):** 0x800CD0F8, 0x800CD488, 0x800CDB78, 0x800CE108, 0x800CE7F8, 0x800CEE38, 0x800CF298.

**Region counts:** u8 at 0x800CF77C, values 57, 111, 89, 111, 100, 70, 75.
- Track 2 has 126 sections for 111 regions; regions past the table see everything.

**Conversion:** the masks are used verbatim.

## 5. Collision [V]

**Layout:** a 0x20-byte header (`u16 nSeg, nNode, nPoly, nVert, leafBytes, vlistBytes`), then SEG (0x84), NODE
(0x14), POLY (**0x1A**), VERT, LEAF and VLIST, in Rush 2's order (loader `func_800828EC`).

**Header fields:**
- +0x18 and +0x1A go to globals.
- **+0x1C** is the segment that follows the last one. Rush 2 wraps to 0.

**Polygons:** a polygon is Rush 2's with an extra u16 at +4, so its matrix is at +6 and its vertex-list offset at
+0x18. The extra u16 is dropped.

**Spaces:**
- Rush 1 collision space is **(render z, render x, −render y)**. Evidence: vertex extents against placement extents,
  and floor heights inside section boxes for all 7 tracks.
- A Rush 1 polygon frame holds the surface in local x/y, with depth along local z. The point test
  `func_80079CDC` accepts local z in [best, 5) and takes the maximum.
- Rush 2 (`func_8006C670`) uses local z/x, with height along y in (−5, best], and takes the minimum.
- **Rush 1 multiplies by the transpose of its stored matrices.** `func_80079B90` (polygons) and `func_80078414`
  (segments) compute local = Mᵀ · (p − origin), where Rush 2's `func_80054454` computes M · v. On flat ground this
  hardly shows; on slopes the untransposed reading tilts every plane the wrong way. Track 5's start hill then sat
  40 ft off the road, and cars fell through it.

**The mapping (A = B):**
- render = A · collision, and Rush 2 local = B · Rush 1 local.
- Both A and B take (x, y, z) to (y, −z, x).
- Polygon and segment matrices: M2 = B · M1ᵀ · A⁻¹.
- Every vertex, world origin or local, maps by (y, −z, x).
- The results are right-handed (det +1), like Rush 2's own matrices.

**Segments (`func_8007A89C` vs Rush 2 `func_8006BDA8`):**
- The formulas are the same with Rush 1 along = L[0] / across = L[1] and Rush 2 along = L[2] / across = L[0].
- Both have the cubic height term.
- World vectors (+0x00, +0x30, +0x3C, +0x48) map by A.
- For a wrap target other than 0, a copy of that segment is appended after the last one.

**NODE, LEAF and VLIST:** identical to Rush 2's (the VLIST marker is 0xC0).

**Checks (`colltest.py`):**
- Ground queries at random points along the lanes hit the same polygon at the same height in both games, for all
  7 tracks.
- Segment (t, across) parameters match exactly.

## 6. Race definition [V]

### AI lanes (assets 58+t / 65+t, loader `func_800A804C`)

- **File layout:** 4 × { `s16 count, loop, end, x` + count × { `s16 x, y, z` (collision space), `u8 speed` (mph),
  `u8 behaviour`, `s16 index` } }.
- **Lap structure:** a lap runs from point 0 to `end` and continues at `loop`. Tracks 1, 4 and 6 have an intro before
  the loop.
- **Lane 0:** carries a run-out after the lap.
- **Behaviour codes:** 1-5, as in Rush 2 (the steering filter in `func_8007D174` matches `func_80071FBC`).

### Checkpoints

- **Tables:** 0x800C7B6C (forward) and 0x800C7B88 (backward), one per track.
- **Record layout:** 0x18 bytes: `f32 x, y, z` (render space), `f32 0`, `s16 flags` (−1 ends the list),
  `s16 time[3]` (the checkpoint's time extension on lap 1, lap 2, lap 3+).
  - In the end record, +0x12 is the start time's base and +0x14 the loop-start checkpoint.
- **Race timer:**
  - Start (`func_800B9F8C`): base − 4 × difficulty + 4 × laps + 16 s, not scaled.
  - Reaching checkpoint i (`func_8009FFCC`, race leader only) adds time[lap] × (1 + (5 − difficulty) × 0.05).
  - The race starts with checkpoint 0 passed, as in Rush 2.
- **Flags (`func_8009D818`):**
  - 2 = the lap line
  - 1 = the last checkpoint before it, which triggers the FINISH banner
  - 4 = unknown (track 6)
- **Progress:** Rush 1 snaps checkpoints to the collision segment chain, which acts as its spine (`func_800A8F18`).

### Conversion to a Rush 2 path file

| Rush 2 field | Built from |
|---|---|
| Spine | lane 0's lap |
| Lanes | the four laps |
| Branches | none |
| Checkpoints | gates on spine points, in race order (the nearest point after the previous checkpoint's) |

- **Gate width:** wide enough for every lane.
- **Gate placement:** a gate moves along the spine until Rush 2's crossing search (first side change inside the
  radius) finds the intended pass on every path. Track 6 is a figure 8.
- **Flags:** 2 (arm) on checkpoint 0, 4 on Rush 1's loop start, 1 on Rush 1's lap line. This is NYONE's intro
  pattern.

## 7. Per-track values used

| Item | Value |
|---|---|
| Fog | 0x9696BE. Rush 1's fog colour is a game option (0x800C7FFC by option), not per track. |
| Sky | Rush 2's procedural dome. Rush 1 uses the same with the same texture names, SKY01 and SKYFOUR. |
| Songs | Rush 2 songs 1, 0, 4, 2, 3, 5, 6. |
| Demo starts | spine quarters |
| Record seeds | lane 1's lap time at its target speeds |
| Checkpoint times | Rush 1's own (`rush2::track1::race_time`, hooked into `rush2_track49_race_time`): header +0 and checkpoint +0x1E / +0x20 get Rush 1's start and lap 1 / lap 2 times, divided by Rush 2's factor 1 + (5 − difficulty) × 0.075. Rush 2 otherwise works them out from the AI lane speeds (`func_800924E4`), never 45 s. |

## 8. Open items

- The Rush 1 versions of breakables (fences, trash cans, gas signs) are static. Making them breakable needs Rush 2's
  debris names, or code.
- Checkpoint flag 4 (track 6) is not understood.
- The cars (assets 25-35) aren't ported. All 11 are already Rush 2 cars.
