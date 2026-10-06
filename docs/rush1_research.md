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
  - 4 = no wrong-way warning: `func_8009BE68` skips its check while the car's last or next checkpoint (car
    +0xA4C / +0xA4E, the 0x44-byte checkpoint copies at *0x800F3A80, flags at +0x3A) has it. Only track 6 (the
    figure 8) uses it.
- **Progress:** Rush 1 snaps checkpoints to the collision segment chain, which acts as its spine (`func_800A8F18`).
  The chain is linear (`func_8009DF90` / `func_8009E090` step ±1), and a checkpoint counts once the car's place on
  it passes the checkpoint's (`func_8009FFCC`), however far to the side the car is.
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
- **Gate width:** wide enough for every lane, then widened to the largest of 300, 250, 200, 160, 130, 100 or 80 ft
  that keeps every crossing in place and that no path crosses within the radius + 30 ft on its way from the
  previous gate. Lane-only gates were 60 ft, so a car driving wide of the lanes missed gates, and a missed gate keeps
  Rush 2's respawn search in the stretch before it (`func_80090570`): crash respawns jumped back up to half a lap.
  Rush 2's own gates are 35-890 ft.
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
