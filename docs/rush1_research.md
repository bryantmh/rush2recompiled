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
- `banners/`: the track select banners, built into `include/track1_banners.h` by `tools/build_banners.py` (section 12).

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
- **Breakables** become Rush 2 breakable class records, drawn with Rush 1's own models:

  | Rush 1 model | Record | Rush 2 class | Behaviour id given to the model |
  |---|---|---|---|
  | CONE1L1 | same name | CONE1 | 2 |
  | METERL1 | same name | METER | 5 |
  | TREEHIT1L1-4L1 | same name | TREEHIT | 5 |
  | FLAG2L0 | same name | FLAG2 | 23 |
  | FENCEL1 | same name | FENCE | 4 |
  | GASIGNL1 | same name | GASIGN | 7 |
  | WINDOWBL1 | SHATPANEBL1 | SHATPANE | 9 |
  | PMUNCH_01L1 (traffic light) | CURVEHITPMUNCH1 | CURVEHIT (metal sign) | 5 |
  | TMUNCHL1 (trash muncher) | TREEHITTMUNCH1 | TREEHIT | 5 |

  - The class gives the sound (0x80125C54) and debris handling. The behaviour runs from the model's name-record id
    (+0x14), set to that of the Rush 2 model the class normally draws (CONE1O1, METERO1, TREEHIT1O1, ...).
  - Rush 2 looks a class's model up by its debris name, and its shared object files (0x12, 0x14) load before the
    track, so their models would win (`func_8005BE3C` searches slots upward). `src/track1.cpp` redirects the
    placement walker's lookup (func_80081790 at 0x8008194C) to the record's own model.
  - Rush 2's breakable pieces, resolved by name at race start, are redirected to Rush 1's: CONE1O1 → CONE1L1,
    METERO1 → METERL1, TREEHITnO1 → TREEHITnL1, SHATPANEO1-7 → WINDOWBL1-7, FENCEO1-12 → FENCEL1-12,
    FLAG2O1-10 → FLAG2L0-9, GASIGNO1-3 → GASIGNL1-3 (`func_8005BE3C` entry).
  - Rush 1's behaviour table (0x800CC81C, set at 0x80086348) has Rush 2's layout (0x800C526C): the same ids, the
    callbacks in the same order and of similar sizes. Rush 1 ids: 2 cone, 3 meter, 4 fence, 5 traffic light (PMUNCH),
    6 tree / trash muncher, 7 gas sign, 8 key, 9 window, 23 flag.
  - Rush 2's per-frame breakable update (`func_8008A01C`) sets each breakable's model from its +0x62 id (the class's
    CONE1O1, METERO1, ...), and the METER id (0x717) also gets node flag 0x8000 (drawn as a camera-facing card, like
    Rush 2's flat meter). On Rush 1 tracks a breakable keeps the model it was created with until its id changes (hit):
    `rush2_track1_breakable_model` at 0x8008A0CC. Without it, traffic lights drew as floating meter cards and trash
    munchers as trees.
  - Rush 1 models are centred and drawn at 1/16 scale (vertex units / 16 = world units), and their records sit at
    the object's centre; Rush 2's breakable models have their origin at the base and their records on the ground. Rush
    2's car hit test (`func_8008B0CC`) takes the placement point in the car's frame within |y − 1.25| < 2.25, so a
    centred traffic light (centre 9.6 up), trash muncher (7.5) or tree (12.5) was never hit. The converter moves each
    placed breakable model's vertices up to put its base at the origin and lowers the record by the same amount / 16,
    which also makes Rush 2's knock-over pivot about the base. The other models of the same family (the name without
    its number) move by the same amount, since they share the placed model's origin: the flag's animation frames
    FLAG2L0-9 (moving only the placed FLAG2L0 made the flag jump on every loop), WINDOWBL1-7 and FENCEL1-12 pieces.
  - Rush 1's own hit test (`func_8008602C`, from the loop at 0x800860FC) has no height test: the point in the car's
    frame within |z| < 7, |x| < 3. Behaviour 9 (window, type 0x19D) also hits on a sphere of a third of its radius.
    Rush 2's height test still missed objects on Rush 1's hills (a traffic light based at y 31.6 under a car at 36.2),
    so on Rush 1 tracks `rush2_track1_breakable_hit` (func_8008B0CC entry) applies Rush 1's rule with Rush 2's width
    (3.5); windows (0x722) keep Rush 2's test.
  - Checked in game: track 2 creates 124 breakable instances (89 cones, 15 trash munchers, 15 traffic lights, a window, 4 flags), track 3
    112 (44 cones, 52 meters, 16 trees); cones knock over. Rush 2 allows 0x82.
- **Emitters** (no node; ids from the bytes at 0x800CF774) map to the Rush 2 emitter with the same sound and range:

  | Rush 1 | id | Rush 1 sound, range | Rush 2 emitter | Rush 2 sound, range |
  |---|---|---|---|---|
  | CCAR | 0x0A | 0x1E, 200 | CABLECAR | 0x4F, 200 (same sample) |
  | SMALLHOOT | 0x10 | 0x21, 400 | BIGCHR1 | 0x51, 400 (same sample) |
  | BIGCHEER | 0x11 | 0x21, 300 | BIGCHR2 | 0x51, 300 (same sample) |
  | FIRECRK | 0x0D | 0x3E, 100 | FIRECRCK, replaced at run time by Rush 1's sound (§9) | |
  | BIGCHEER2 | 0x00 | none (behaviour 0 returns) | left out | |

- KEYL1 records (key number 1-8 at +0x4A) become Rush 2 key records KEY1-8 drawn with Rush 1's KEYL1 (behaviour 8),
  kept per profile by src/collectibles.cpp. Rush 1 saves them as a u8 mask per track at player record + 0x1C0 +
  track * 0x16 + 7 (`func_8006C2E8`); half a track's keys give its car 8 (TAXI), all of them car 9 (HOTROD).
- MARKER and TIME are dropped.

## 4. Visibility [V]

**`func_80064544`** is Rush 2's `func_8007C27C`:
- region = ordinal of the top-level record whose box holds the camera
- 16-byte masks of two u64, with bit i = section node i
- the hide flag is 0xC00 (Rush 2: 0x400)

**Tables (one per track):** 0x800CD0F8, 0x800CD488, 0x800CDB78, 0x800CE108, 0x800CE7F8, 0x800CEE38, 0x800CF298.

**Region counts:** u8 at 0x800CF77C, values 57, 111, 89, 111, 100, 70, 75.
- Track 2 has 126 sections for 111 regions; regions past the table see everything.

**Region choice differs from Rush 2's:**
- Rush 1 (`func_80064544`): a top-level record whose box holds the camera in x and z, with dy ≤ box top (no bottom
  test); the least |dx| + |dz| wins; none → everything visible.
- Rush 2 (`func_8007C06C`): box bottom ≤ dy (no top test); score √(dx² + dz²), plus dy − bottom when dy is above
  0.75 × top; the least score wins.
- With Rush 2's rule, a camera high above track 2's hill (x 1167, z 2300) picks region 102, whose mask hides section 91
  under it (the ground vanishes mid-jump): 14 of 1155 lane-0 points at +400. Rush 1's rule hides nothing there.

**Conversion:** the masks are used verbatim, and Rush 1 tracks use Rush 1's region choice (`src/track1.cpp`, with the
camera position from the start of `func_8007C27C`).

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

- **File layout:** 4 × { `s16 count, loop, end, x` + count × { `s16 x, y, z` (collision space), `u8 speed` (ft/s: the driver,
  `func_8007CDD4`, compares lane × 1.05 × rubber band with the car's speed, where Rush 2 converts mph × 1.4667; the
  converter stores round(speed × 15 / 22) mph),
  `u8 behaviour`, `s16 index` } }.
- **Lap structure:** a lap runs from point 0 to `end` and continues at `loop`. Tracks 1, 4 and 6 have an intro before
  the loop.
- **Lane 0:** carries a run-out after the lap.
- **Behaviour codes:** 1-5, as in Rush 2 (the steering filter in `func_8007D174` matches `func_80071FBC`).

### Checkpoints

- **Tables:** 0x800C7B6C (forward) and 0x800C7B88 (backward), one per track.
- **Record layout:** 0x18 bytes: `f32 x, y, z` (render space), `s32 gate radius²` (250000 = 500 ft on most; up to
  1000000; 100000 on track 7), `s16 flags` (−1 ends the list),
  `s16 time[3]` (the checkpoint's time extension on lap 1, lap 2, lap 3+).
  - In the end record, +0x12 is the start time's base and +0x14 the loop-start checkpoint.
- **Race timer:**
  - Start (`func_800B9F8C`): base − 4 × difficulty + 4 × laps + 16 s, not scaled.
  - Reaching checkpoint i (`func_8009FFCC`, race leader only) adds time[lap] × (1 + (5 − difficulty) × 0.05).
  - The race starts with checkpoint 0 passed, as in Rush 2.
- **Flags (`func_8009D818`):**
  - 2 = the lap line
  - 1 = the last checkpoint before it, which triggers the FINISH banner
  - 4 = no wrong-way warning: `func_8009BE68` skips its check while the car's last or next checkpoint (car
    +0xA4C / +0xA4E, the 0x44-byte checkpoint copies at *0x800F3A80, flags at +0x3A) has it. Only track 6 (the
    figure 8) uses it.
- **Gates:** `func_800A8F18` copies each record to a 0x44-byte runtime checkpoint (*0x800F3A80: +0x30 radius²,
  +0x3A flags, +0x3C times), snaps its position (+0x24) to the nearest node of the collision segment chain, which
  acts as its spine (linear, `func_8009DF90` / `func_8009E090` step ±1), and takes the chain's direction there
  (+0x18).
- **Gate test** (`func_800A0BB8`, per car, next checkpoint car +0xA4E only): the car within the radius in x/z and
  within 3 ft × frames of the gate's plane, then `func_8009D818` (the twin of Rush 2's `func_8008F220`). So the
  same test as Rush 2's `func_800A1468`, with these radii. `func_8009FFCC` only grants the race leader's time.
- **Wrong way:** `func_8009BE68` maps the car's segment to a lane 0 point (tables 0x800E4E28 / 0x800E66F8) and
  compares the car's heading with the point 5 ahead, like Rush 2's `func_8008CDA4`.

### Conversion to a Rush 2 path file

| Rush 2 field | Built from |
|---|---|
| Spine | lane 0's lap |
| Lanes | the four laps |
| Branches | the lanes' alternate routes (below) |
| Checkpoints | gates on spine points, in race order (the nearest point after the previous checkpoint's) |

- **Branches:** Rush 2 tracks a car on the spine, and only on branches when it is over 40 ft from the spine
  (`func_80090570`). Its wrong-way angle, respawn point (`func_80090A40`) and checkpoint window come from that, so
  with lane 0 alone, the alternate routes of tracks 4 and 5 (lanes up to 850 ft from lane 0) read as wrong way. Each
  lane stretch over 40 ft from the spine and earlier branches that gets over 100 ft away becomes a branch,
  extended until it is back within 20 ft (at most 30 points each way), if it rejoins the spine ahead of where it
  leaves (Rush 2 walks branches forward when it moves a respawn point on). Rush 2 links the ends at load. Result:
  3 branches on track 4 each way, 2 / 1 on track 5, none elsewhere. A car driven along every lane through Rush 2's
  tracker and wrong-way test reads wrong way at 13-42 points per lane on tracks 4 and 5 without them, and at only
  one point (the fork on track 5 forward, still inside 40 ft) with them.
- **Gate width:** in the path file, wide enough for every lane, then widened to the largest of 300, 250, 200, 160,
  130, 100 or 80 ft that keeps every crossing in place and that no path crosses within the radius + 30 ft on its
  way from the previous gate: that radius is what Rush 2's crossing search (`func_80092D6C`) uses at load. Once the
  search has run, `rush2::track1::race_time` puts Rush 1's own radius (316-1000 ft) into the header copy, which is
  what the gate test reads. With the file's radii alone (60-507 ft) a car passing wider than that missed the gate,
  and a missed gate keeps Rush 2's respawn search in the stretch before it (`func_80090570`): crash respawns jumped
  back up to half a lap. Every lane driven through the gate test with Rush 1's radii counts each gate once per lap
  within a point of its crossing.
- **Gate placement:** a gate moves along the spine until Rush 2's crossing search (first side change inside the
  radius) finds the intended pass on every path. Track 6 is a figure 8.
- **Flags:** 2 (arm) on checkpoint 0, 4 on Rush 1's loop start, 1 on Rush 1's lap line. This is NYONE's intro
  pattern.

## 7. Per-track values used

| Item | Value |
|---|---|
| Fog | 0x9696BE. Rush 1's fog colour is a game option (0x800C7FFC by option), not per track. |
| Sky | Rush 2's procedural dome. Rush 1 uses the same with the same texture names, SKY01 and SKYFOUR. |
| Songs | Rush 1's own (§9). Rush 2 songs 1, 0, 4, 2, 3, 5, 6 if its music isn't available. |
| Demo starts | spine quarters |
| Record seeds | lane 1's lap time at its target speeds |
| Checkpoint times | Rush 1's own (`rush2::track1::race_time`, hooked into `rush2_track49_race_time`): header +0 and checkpoint +0x1E / +0x20 get Rush 1's start and lap 1 / lap 2 times, divided by Rush 2's factor 1 + (5 − difficulty) × 0.075. Rush 2 otherwise works them out from the AI lane speeds (`func_800924E4`), never 45 s. |

## 8. Open items

- What the keys unlock is undecided (docs/unlocks_plan.md).
- MARKER and TIME (behaviours 0x0F, 0x15) are not understood.
- The cars (assets 25-35) aren't ported. All 11 are already Rush 2 cars.

## 9. Audio [V]

Both games use libaudio: a song file (ALSeqFile, LZ-compressed type 0 MIDI files) played by an ALSeqPlayer with one
music bank, and a sound effect bank for an ALSndPlayer. The banks (`B1`, one bank each) have the same layout.

| | Rush 1 ROM | Rush 2 ROM |
|---|---|---|
| Music bank .ctl / .tbl | 0x5D9350 (52 instruments + percussion) / 0x5DF380 | 0x7A1170 (116) / 0x7A8DE0 |
| Songs | 0x6F80A0, 16 | 0x96A360, 13 |
| Sound effect bank .ctl / .tbl | 0x70A1D0 (68 sounds) / 0x70DCC0-0x7A7930 | 0x97AE40 (116 sounds) / 0x981180 |

- Rush 1 loads them in `func_800BC2B0`; Rush 2 in `func_800B3B78`.
- Rush 1's race music: `func_80099E44(2, setting)`, setting 0 off, 1 random of 9 race songs (table 0x800D2910:
  0, 1, 2, 7, 3, 10, 6, 12, 15), 2+ a fixed song. There are no per-track songs.
- Rush 1's music bank has empty instrument slots (offset 0). `alBnkfNew` rebases them anyway and patches the bank
  header as an instrument: harmless on the console, out of bounds in the recomp, so they are pointed at an empty
  instrument first.
- The audio microcode reads ADPCM codebooks from the bank with 24-bit addresses: a bank copy above 16 MB of RDRAM
  plays as noise.
- Port (`src/track1_audio.cpp`): Rush 1's ROM 0x5D9350-0x7A7930 is appended to the runtime's ROM image; Rush 2's song
  header gets Rush 1's 16 songs as 13-28; the player switches banks per song; Rush 2's sound effect instrument gets
  Rush 1's fireworks (sound 62) as sound 116, played by FIRECRCK on Rush 1 tracks.
- Sound effects: 42 of Rush 1's 68 samples are byte-identical in Rush 2, including all the emitters' but fireworks.

## 10. Animated / dynamic objects [V, 2026-10-07]

Rush 1 has no windmill model, name or code. Searching every asset and the main code for WIND/MILL/BLADE/ROTOR/PROP/FAN
only finds WINDOWBL*. Rush 1's whole table of named dynamic objects is the name list at 0x800D6900-0x800D7400 (main):
car parts, CARBLASTO1-15, SPARKO3, SMOKE*, DUST*, SPLASH*, CONE1L1, METERL1-5, TMUNCHL1-6, TREEHIT1-4 L1-3,
WINDOWBL1-7, PMUNCH_01L1-2, FENCEL1-12, FLAG2L0-9, T5GATEL1-12, GASIGNL1-10, KEYL1, MINE1, and the emitters MARKER,
TIME, CCAR, FIRECRK, SMALLHOOT, BIGCHEER, BIGCHEER2. Everything else is a static section: every track piece has kind
0 (name record +0x14 = 0xFFFF in the low half) and no per-track animation code was found. Anything that looks like a
windmill on track 6 is therefore baked into a static piece (TRACK6LP1_*, LP2_*, BCH_*), and does not turn in Rush 1.

Geometry models that no placement record names directly (each turned out to be code-driven):
- Track 5: T5GATEL1-12, the gate (see "Track 5 gate" below).
- Track 6: BUSO1 (a 267-triangle bus, 192 x 200 x 640 units; also named in main at 0x800D05AC), the traffic (see
  "Track 6 traffic buses" below).

### Track 6 traffic buses (func_800A9DB8) [V, 2026-10-08]

Track 6 has eight roaming buses driven by code, not placement records. Rush 2's subway trains (subway_init
func_800A4210, subway_update func_80075FB8, the pseudo-car route in func_8006EC78) are this system grown up.

**Setup, `func_800A9DB8(int enable)`.** Called from race setup (0x800AA420) with `enable = (track byte 0x80100050 ==
5)`, i.e. track 6 only, and again with 0 at 0x800AB194. With enable set it:
- loads model BUSO1 (`func_80069D1C("BUSO1", 1)`) and sets the bus count byte 0x800DA084 to 8,
- for each bus i reads a start record at 0x800C6848 + 0x14*i {+0 path, +4 f32[3] start position, +0x10 s8 start
  node} and fills a 0x48-byte state at 0x800DA0B8 + 0x48*i: +0 path, +4 f32[9] yaw matrix, +0x28 f32[3] position
  (render space), +0x34 speed (the start node's), +0x38 distance to the node (1e7 = far), +0x3C heading (the start
  node's), +0x40 last turn step, +0x44 s8 node, +0x46 draw object (func_80066C00, mode 3, on the matrix at +4),
- allocates 8 x 0xA68 bytes (`func_80097F88(0x5340)`, a car state each) at *0x800DA328 as collision bodies with a
  car-style box (+0x114..+0x140: +-20 long, +-6 wide, -12 tall in Rush 1's car axes) and fills them
  (func_8007FB30(0)).
With enable clear the count is 0 and the body pointer null.

**Paths.** Two node lists, 0x10 bytes per node {f32 x, f32 z, s16 speed (units/s, < 0 ends the list), pad, f32
heading}: 0x800C6558 (23 nodes, buses 0-3) and 0x800C66C8 (24 nodes, buses 4-7). Each is a long loop between z
-3770 and +660 on track 6's roads; buses 0/1 and 4/5 start at node 0, 2/3 at 10 and 6/7 at 11, all at y 40.

**Move, `func_8007FC74(dt)`** (Rush 1's physics tick func_800802D8). Per bus, with step = speed * dt and d the xz
distance to the current node:
- if d < step or d grew past the stored distance: speed = the node's speed, node + 1 (back to 0 at a negative
  speed), distance = 1e7;
- else: distance = d; speed += (node speed - speed) * step / d; the heading error (node heading - heading, wrapped
  to +-pi) is clamped to +-pi/4 and times dt is the turn, whose change from the last tick is clamped to +-pi/16;
  heading += turn, wrapped; matrix = func_8007FC14(-heading): {cos, 0, sin; 0, 1, 0; -sin, 0, cos} (func_80005B40
  = cos, func_80005980 = sin; the forward row +0x1C is (sin h, 0, cos h)). The setup builds its first matrix from
  +heading, which the first turn replaces.
- position += step * forward; func_8007FB30 copies velocity, position (2 * step ahead) and orientation into the body,
  converting to Rush 1's collision axes.
Buses do not steer towards node positions: they turn towards each node's heading and pass the node when they stop
closing on it.

**Cars.** func_80079528 (called from car against car, func_80079758) tests each car against each body within the
car's radius + 25 (func_8007947C, box height -12.5) and responds with func_80078F24, as Rush 2's func_8006EC78 does for
the trains (radius + 72, height 18). func_8007CF74, at the end of the computer driver's look at the cars ahead
(func_8007D174, 0x8007D7C8), lowers the throttle limit to 0.8 for a bus up to 120 ahead and within 14 to the side,
rising to 1 at 200.

**Port** (src/track1_buses.cpp). Rush 2's subway array holds 6 entries, so on SF Rush track 6 subway_init and
subway_update are replaced (hooks at their entries) by a port of func_800A9DB8 / func_8007FC74. Bus states stay in
C++; each bus's pose (matrix + position, what its scene node points at) sits in the game heap after the 8 pseudo-car
bodies (0x81C bytes each, pointer 0x800D50E4, count 0x800D4E74), so Rush 2's car collision hits the buses as it hits
the trains; the hook at 0x8006ED1C gives func_8006EC78 the buses' reach and height (25, 12.5), and the hook at
0x80072564 in func_80071FBC adds the drivers' bus check (on Rush 2's sp+0x134 throttle limit, car orientation
+0x7BC, position +0x7B0; Rush 2 car axes x side, y up, z forward). Render space is Rush 2's world space, so paths
and poses carry over unchanged.

### Track 5 gate [V, 2026-10-08]

T5GATEL1-12 are a fence variant. Rush 1's fence spawner (0x800C32EC, called through the breakable class table) gives
a new breakable piece id 0x1A6 (FENCEL1, index 422 of the dynamic model list at 0x800CC978) and 12 pieces, but 0x1BC
(T5GATEL1, index 444) when +0x76 is set. +0x76 is placement record +0x4A, which the placement walker
(func_800829E4, 0x80082AA8 / 0x80082BC8) stores in 0x800EA140 for the spawner. The breakable draws its id's model
and breaks into id..id+11, so a flagged fence is drawn as T5GATEL1 and breaks into T5GATEL1-12. Track 5 has one:
the FENCEL1 record at (-265.7, -53.8, -140.0) with +0x4A = 1, next to the start MARKERs; no other track sets +0x4A on
a fence.

Rush 2's fence spawner (breakable_behavior_alloc_b, func_800BB864) dropped the branch and always uses 0x730
(FENCEO1), but Rush 2's piece name list still has T5GATEO1-12, 22 entries after FENCEO1 as in Rush 1, so id 0x746.
Port: the converter turns a fence record with +0x4A set into a T5GATEL1 object of class FENCE (record
FENCET5GATEL1, drawn with T5GATEL1, origin moved to its base like the other breakables) and redirects T5GATEO1-12 to
T5GATEL1-12; src/track1.cpp (hook at 0x800BB8AC) gives the FENCET5GATEL1 breakable piece id 0x746.

## 11. Car decals as a stripe (SF RUSH STRIPE value)

Code: `src/car1_decals.cpp` (the decal colour maps; port of `tools/rush1/cardecal.py`, checked byte for byte by
`tools/rush1/cpp_test/car_decals.bat`), `src/car1_stripes.cpp` (game side), option "SF Rush Car Stripes" in the Games tab.
Viewers: `tools/rush1/carview.py CAR OUT.png` renders the car in Rush 1, in Rush 2 with the decal and in Rush 2 without,
from several angles (offline, no game run; how to judge the result; it draws back faces, so Rush 2's transparent windows
show the far side); `carview.py --score` measures how much of Rush 1's decal the Rush 2 car shows (2026-10-09: Camaro
100%, Bus 100%, Bug 99%; the Taxi's misses are its roof sign and grille, not decal); `tools/rush1/cardecal.py CAR` writes per-panel
previews; `tools/rush1/cartex.py` (car files, meshes, palettes).

- Cars: only the four with a decal of their own in Rush 1: Camaro (Rush 2 "Bandit", flames across the hood and the
  roof), VW Bus ("Van", white swirls over the sides and roof), VW Bug ("Subcompact", wide white stripes over the roof and
  sides, a target on the rear deck) and Taxi (checker band along the sides and across the nose and tail). BMW, Supra, Bugatti and Viper only have white racing stripes
  (Rush 2's own STRIPE values cover those); the Concept has none; the Hot Rod's flames are where Rush 2 already draws its
  own (in the accent colour), so it is left out; Formula 1 has no textures.
- **Rush 1 car textures come from a BRANCH_Z target.** Each panel group of a Rush 1 car display list is `B4 <target>`,
  `B0` (BRANCH_Z), a call to a 16x8 load, then a 64x32 load; the target is a separate list loading the close-up 64x32
  texture, taken when the car is near. Walking the fall-through instead gives every panel a distant texture: the art
  lands in the wrong places (flames on the Camaro's sides instead of its hood, the Bus's swirls missing from its roof)
  and the tall textures' coordinates seem to need s/t swapped. `tools/rush1/cartex.py meshes()` and the C++ walker take
  the branch; check any Rush 1 car render against the real game.
- Rush 2's cars are the Rush 1 cars re-textured: both games' car meshes use the same car-local coordinates, but the panel
  textures were laid out again (pairing by pixel matches only 65-90%), so the decal is moved through 3D. Each Rush 2 D0
  panel texel is sampled 3 x 3 times; each sample's point on the Rush 2 body is matched to the Rush 1 body (FL1, FR1, RL1,
  RR1, TOP1, WIN1 of D0): the Rush 1 triangle facing the same way (normal dot >= 0.5) that the line along the Rush 2 normal
  crosses farthest out within 3 units, i.e. the visible Rush 1 skin there; else the closest point on the same side.
  Nearest-point matching alone picked hidden inner faces where the two bodies differ, and one sample per texel aliased
  Rush 1's denser panels into streaks.
- Rush 1 palettes: indices 1-25, 30, 65-86 differ across the ten paint sets (paint ramps); the rest are fixed colours.
  Decal indices per car (`CARS` / `cars[]`): Camaro 33-49 and 100-106 (flame ramps), with greys 147-156 and reds
  161-175 kept next to flame texels; Bus and Bug 33-63 (a white
  ramp in those files); Taxi 145-148 (white checks) with blacks 31 and 157-159 next to them (black checks). A texel is
  decal where any of its samples is (and Rush 2 has paint there): Rush 1 blends its decal edges into the paint, and a
  majority rule left every shape a texel thinner per side, visibly sparse. Its colour is the nearest Rush 2 fixed car
  palette entry (96-111 and 144-207, the same for every paint choice) to the mean of those samples. Not 64-255: the
  game's palette class table (ranges `first, last, flags, shift` at 0x800C5670, ending when last < first; built into
  *0x8010C06C by func_800854AC) gives 1-31 flags 0x22 (MAIN tint), 33-63 0x42 (ACCENT tint), 64-95, 112-143 and 208-255
  0x10 (free: func_80084EDC overwrites them with paint / stripe colour blends), and 32, 96-111, 144-207 0 (fixed). A
  decal in the free entries (pure white 128, reds 81-85) picked up the main colour in game. Hand-made cuts (texel rectangles
  per panel, `CUTS` / `Car::cuts`) can drop fragments; specks under 6 texels are dropped.
- Rush 2 car palette: 1-31 main ramp, 33-63 accent ramp with the same greys (accent index = main index + 32), rest
  fixed. The decal goes over Rush 2's own paint, so MAIN and ACCENT (secondary) still colour the rest of the car (user
  decision 2026-10-09; an earlier version turned the accent ramp into the main ramp, as Rush 1 paints these cars in one
  colour, which made ACCENT do nothing).
- Rush 2 stripes: asset 0x1C (deflate, not LZ) tiles stamped by func_80083F50 from func_8008582C's 24-iteration loop
  (D0/D1 x panels 1-6 x full and _4 mip). Per texel the stamp checks the tile byte's alpha nibble against a LOD threshold
  and remaps the panel texel through a class table (*0x8010C06C, 2 bytes per palette index: class nibble, shade) into the
  STRIPE COLOR's remap tables; before stamping, the loop copies the panel's clean texels back (func_80007610) when the game
  keeps a copy (0x8010C0D0 == 2). The STRIPE value 8 is stored as a flag in the record block byte 0x585 bit 0 (STRIPE
  field 0); getter func_800B2608, setter func_80097934, car select row (func_803B9478 at 0x803B9B18 / 0x803B9B8C) and
  text (func_803BC048 at 0x803BC3F4 / 0x803BC644) are hooked. In func_8008582C the value looks up SINGLE's tile
  (0x80085D84) so the loop reaches the stamp, and at 0x80085D8C the decal is written into the panel texture (handle at
  sp+0xBE, car type at sp+0xCE) and the stamp is skipped. The panel is found by its record name, `<car>_D<stage>_<n>` or
  the mip `<car>_D<stage>_<n>_4`; panel 4's full texture also ends in `_4`, so the mip is told apart by the name's length
  (a suffix test missed panel 4, the roof, on every car). `RUSH2_CAR1_DUMP=<dir>` writes the built panels
  (`<car>_<n>.bin`) and logs every stamp call (panel, size, whether a decal matched).
- Built decals are cached in `<app folder>/stripe_cache/<CAR>.bin`, keyed on a hash of the three input files and the
  executable's size and modification time, so each car is built once per ROM and build, not every boot.
- Gotcha: RDRAM is word-swapped on the host; write bytes with MEM_B, never through the alloc pointer.

## 12. Track names and track select banners

The N64 game only numbers its tracks (Track 1-7). The recomp uses the names the Rush the Rock arcade update gave
them (`rush2::track1::track_names`, include/track1.h): 1 Golden Gate, 2 Embarcadero, 3 Market, 4 Downtown,
5 The Heights, 6 Sunset, 7 The Rock (the hidden track). The Progress tab and the UNLOCKS shop show these names.

The track select logo of each track (`R1LOGOn`, `build_logo` in src/track1_convert.cpp) is a 128x32 CI8 texture in
the format of Rush 2's own logos. Tracks 1-6 use the user-submitted banners in `tools/rush1/banners/`
(`trackN_<name>.png`), which `tools/build_banners.py` quantizes to 255 RGBA5551 colors plus a transparent
entry 0 and writes into the generated header `include/track1_banners.h`. After adding or changing a PNG, rerun the
tool. A track without a PNG (currently 7, The Rock) gets its name drawn by the tool with the banner font
(`tools/banner_font.py`) in its game's style: SF Rush's heavy gold letters with a bevel, a navy outline and a drop
shadow, or Rush 2049's wide square letters with some corners cut at 45 degrees, a dark-to-blue fill and a lime and black
outline.
`python tools/banner_font.py OUT.png` previews the alphabet in both styles, and `build_banners.py --preview DIR`
saves every banner.

The banners' art has about a thousand colors even at 5 bits a channel, so the CI8 copy can't be exact. The header
also holds each banner's exact pixels, which `add_banner_images` registers with the renderer at startup
(`rush2::upscale::add_exact_image`, docs/texture_upscaling.md "Exact images"). The game then draws every banner with
its exact colors, in the track select and anywhere else the logo shows.

The Rush 2049 race tracks work the same way (`build_logo` in src/track2049_art.cpp). Their names are the Dreamcast
version's (`rush2::track2049::track_names`, include/track2049.h): 1 Marina, 2 Haight, 3 Civic, 4 Metro, 5 Mission,
6 Presidio. Their banners are in `tools/rush2049/banners/`, and the same tool writes them into
`include/track2049_banners.h`. Only Metro and Presidio have pictures so far. Marina, Haight, Civic and Mission get
their names in the 2049 banner font. The stunt arenas, battle arenas and obstacle course keep their numbered logos.
