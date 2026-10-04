# Race logic, AI paths, mirror/backward and per-track tables (Rush 2 vs Rush 2049)

Tags: **[V]** verified in code or data; **[I]** inferred. Function addresses are Rush 2 (`func_8xxxxxxx`)
unless marked "2049". Tools: `tools/rush2049/paths.py` (path parser, converter, self-check),
`decomp.py` (m2c decompiler front end: `M2C=<path to m2c.py> python decomp.py r2|49 ADDR`), `annot2.py`
(lui-pair annotated r2 listing → `tools/rush2049/out/r2a.asm`), `fmatch.py` (Rush 2 ↔ 2049 function matching).

---------------------------------------------------------------------------------------------------------------------

## 1. Summary

- **AI path format is identical** in both games, and so is the loader. The loader is Rush 2 `func_80093300` +
  `func_80093048` and 2049 `func_800BADE0` + `func_800BAAA0`; the code matches instruction for instruction
  apart from addresses. **[V]**
- **Everything derived is recomputed at load**: checkpoint crossing indices, branch connectivity, the
  header indices and the distances. `paths.py` reimplements these computations, and the values stored in every
  file of both games match its results. **[V]**
- **So a 2049 path file can be used in Rush 2 unchanged** (`paths.convert()` validates and returns the same bytes).
- **The path file defines the race.** It holds the checkpoints (lap line, lap arming, loop wrap), the start-grid
  anchor, AI racing lines and speeds, branches/shortcuts, the respawn line, wrong-way detection and the minimap.
  There is no per-track lap count. The checkpoint time bonuses in the file are ignored.
- **The "+12" files are BACKWARD paths, not mirror paths.** Same positions, reversed direction and checkpoint order;
  the editor names are `…cpb.txt`. 2049 ships them too: files 177–182. **Mirror** is a pure render-time flip
  in both games and needs no data.
- **2049 file map [V]** (from the loader `func_800BB9B0` (2049) and the editor strings left in each file):
  - 158–163 = race tracks 1–6, forward
  - 164–171 = battle arenas DM1, DM6, DM5, DM8, DM3, DM7, DM4, DM2 (in that file order)
  - 172–175 = stunt 4, 3, 2, 1
  - 176 = obstacle 1
  - 177–182 = race tracks 1–6, backward

## 2. Loading (both games) [V]

- **Rush 2:**
  - path asset = `0x57 + track + (backward ? 12 : 0)` (`func_800A4C98`), pointer at `0x800D575C`
    (`func_80086A60`, range 0x57–0x6E)
  - the header copy goes to `0x8010BCE8` (0x32C bytes), the route header to `0x80111940`, the lane headers to
    `0x80110020` (4×8 bytes)
  - lane counts go to `0x800D5780`, the lane pointers to `0x800D57A0`, the branch table pointer to `0x800D5794`,
    the point total to `0x8010D3B0` and the spine pointer to `0x8010F240`
  - the derived data is then computed by `func_80092D6C` (checkpoint crossings and distances), `func_80092060`
    (branch links), `func_80091C38` (bounding box) and `func_800924E4`
- **2049:** file index = `(0x80152570 ? 0xB1 : 0x9E) + track_index` (track_index = byte `0x8014978C`, 0..18).
  - Copies go to `0x80151CE8`, `0x801407F0`, `0x8012E5E8`, `0x801527A4` and `0x801409E8`.
  - The loader skips loading when byte `0x80114650` is set.
- **Size:** the Rush 2 path is loaded into a heap block sized from the asset size table `0x8001CD64[index]`. A
  replacement file needs that entry, and the ROM offset at `0x800C185C`, updated. 2049 race paths are 28–56 KB,
  Rush 2's 8–68 KB.

## 3. Path file format [V]

All values are big-endian. Rows marked **(runtime)** are overwritten or recomputed at load, so their file contents
don't matter. They hold whatever the editor dumped, sometimes stale.

### 3.1 Header (0x000–0x00B)

| Off | Type | Meaning |
|---|---|---|
| +0 | u16 | Base race time. **Ignored** (runtime): `func_80093300` forces it to 90 s. |
| +2 | s16 | Loop-start checkpoint, the first checkpoint with flag 4 (default 0). (runtime) |
| +4 | s16 | Finish checkpoint, the first with flag 1 (default 0). (runtime) |
| +6 | s16 | Arming checkpoint, the first with flag 2 (default 0). (runtime) |
| +8 | s16 | Checkpoint count, at most 10. |

### 3.2 Checkpoints (0x00C + i·0x50, 10 slots)

| Off | Type | Meaning |
|---|---|---|
| +00 | f32[3] | Centre of the gate. |
| +0C | f32[3] | Unit direction of travel (the plane normal; y is always 0). |
| +18 | u32 | Radius² of the gate, tested in 2D on x/z. |
| +1C | s16 | Flags: 1 = finish/lap line, 2 = lap counting armed once passed, 4 = loop start. Flags may combine; 7 = all three on the start line. |
| +1E / +20 | s16 | Time bonus for lap 1 / later laps. **Overwritten with 45** (runtime). |
| +22 | s16[20] | Crossing index on the spine [0], lanes [1–4] and branches [5–19] (−1 = none). (runtime: `func_80092BC4`, `func_80092884`, `func_80092A38`) |
| +4C | f32 | Spine distance to the next checkpoint. (runtime: `func_80092240`) |

### 3.3 Routes (from 0x32C)

- **Route header (0x32C, 16 bytes):**
  - u16 spine point count
  - u32 pointer (runtime)
  - **u8 branch count** at +8, at most 15. The checkpoint crossing array has 20 slots = spine + 4 lanes + 15
    branches.
  - u32 pointer (runtime)
- **Branch records (0x10 each):**

  | Off | Type | Meaning |
  |---|---|---|
  | +0 | u8 | Alternate-route flag. A flagged branch may only attach to other flagged branches (`func_8008FD74`); route choice is in `func_80090514`. **[I]**: "special/expert shortcut" marker. |
  | +1 / +2 | s8 / u16 | Path (−1 = spine, else branch #) and point index where the branch leaves. (runtime: nearest point to the first point, `func_80091E54`) |
  | +4 / +6 | s8 / u16 | Path and point index where the branch rejoins. (runtime: nearest point to the last point) |
  | +8 | s8 | Checkpoint the branch belongs to. (runtime: `func_80091CFC`) |
  | +A | u16 | Point count. |
  | +C | u32 | Pointer. (runtime) |

- **u16 total point count** = spine + all branch points. It sizes the bounding-box loop.
- **Spine points:** s16 x, y, z (6 bytes each). The centre line used for progress, distances and the minimap. Past
  the last point it wraps to the loop-start checkpoint's crossing (`func_8008CCF8`).
- **Branch points:** 6 bytes each, in branch order.
- **4 lanes** (AI racing lines), each:
  - header: u16 count, u8, u8 (unused) **[I]**, u32 pointer (runtime)
  - points of 8 bytes: s16 x, y, z, then **u8 target speed in mph** (×88/60 = ft/s, `0x800CF890` = 1.4667,
    `func_80071FBC`, `func_80074990`, `func_80090A40`), then **u8 behaviour**
  - behaviour selects the AI steering filter in `func_80071FBC`: 5 damped, 2 follow exactly, 1 blended,
    3 and 4 variants, other values the 50/50 default. 2049 race 4 uses 0 on a few points. **[I]**
- **Units:** world feet, the same in both games. 2049 coordinates stay well inside s16.

### 3.4 Self-check (`python paths.py`)

- All 24 Rush 2 and all 25 2049 files parse to exactly their length.
- Recomputing the derived fields reproduces the header indices for every file. It also reproduces almost all stored
  crossing indices and branch links; the few differences are stale editor values or my simplified nearest-point
  search.
- Every recomputed link and index is in range: **ALL OK**.
- Observations:
  - 2049 race 2 (forward) has no flag-1 checkpoint.
  - 2049 races 4 and 5 (forward) have all flags 0.
  - All backward files lack flag 2.
  - In all these cases the defaults (checkpoint 0) give the intended start/finish line.
  - Max lane speed is 247 mph (2049 race 2).

## 4. How the race is defined

| Item | Rush 2 | 2049 |
|---|---|---|
| Checkpoints / laps | Path checkpoints. `func_8008F220` runs when a car's progress passes its next checkpoint: next = cp+1, wrapping to loop-start. A lap is counted when leaving the finish cp after the arming cp has been passed (car+0x7FC). [V] | Same logic in `func_800D24C8` (2049). [V] |
| Number of laps | Option byte `0x800D5760[0]` → `0x8010C0E2`; circuit mode forces 3 (`func_80094698`). **No per-track lap count.** [V] | Option → `0x80152734`. [I] same |
| Finish | When the leader starts the last lap, `func_8008F080` looks up model `<prefix>FINISH` (or `<prefix>FINISHB` when backward) and swaps G_DL calls to texture `CHKPNT`'s load list for texture `FINISH`'s (a banner swap). [V] | No equivalent (no FINISH/CHKPNT strings). [V] |
| Checkpoint time | When checkpoints are on (`0x8010C17B`, options+0x19): `func_80093300` writes 90 / 45 into the header copy, but `func_800924E4` (from `func_80093048`) then **overwrites** them with driving times from the path's AI lanes (start = first stretch + 10 + a share of the rest; each checkpoint +0x1E / +0x20 = time to the next one). Both are multiplied by 1 + (5 − difficulty) × 0.075 (`func_800AE670`, `func_8008F220`). **Not 45 s per checkpoint.** 2049 tracks scale the result (`rush2_track49_race_time`); SF Rush tracks use SF Rush's own times (`rush2::track1::race_time`). See checkpoints.md §0. [V] | Never reads the bonus fields. [V] |
| Start grid | Grid slot table `0x800CC2BC` (12-byte offsets; 8 race slots in staggered rows of 2 every 20 ft, slot 1 at +17 ft lateral, rows at 0/−20/−40/−60), applied in the car's path frame. The anchor is the spine crossing of the car's current checkpoint (cp 0 at start), oriented along the spine (`func_8006DE20` case 0, `func_8006D348`). Slot from `0x800D9C10[car*9+6]`. [V/I] | Same table at `0x801210E8`, but **6 race slots** (the last row is −40). [V] |
| Attract/demo start | `func_8008E6B4`: cycles through per-track spine indices (`0x800C456C` lists, counts `0x800C45CC`, 24 = 12×fwd/back). [V] | `func_800B9284` (2049): `0x801173D8` / `0x80117408` (12 = 6×fwd/back). [V] |
| Respawn | Each frame the car's last safe pose goes to car+0x7B0 (`func_80091ADC`). Reset (`func_8006DE20` case 2, `func_8008CDA4` stuck timer: speed < 25 for 6 s or y < −190) puts the car at the nearest point on its AI lane, +1.5 ft up and oriented along the lane (`func_80090A40`). [I, strong] | Same code family (`func_800D3B28` (2049)). [I] |
| Wrong way | `func_8008CDA4` computes, for humans, the angle between the car and the path 5 points ahead (closest spine/branch point) → car-state+0x34C. [V] | [I] same |
| Minimap | **Generated at runtime from the spine:** bounding box (`func_80091C38`) → 88×88 px fit; the track is drawn into a 96×96 texture (`func_800B8188`). The finish flag sits at the finish checkpoint, nudged by `0x800C3F84` / `0x800C3FB4[t%12]` (`func_800B9D48`). [V] | Same method (`func_80109554` (2049) reads the spine and bounding box). [V] |
| AI | Lanes (speed and behaviour), branches, rubber band ×`0x800C1A3C[t+12b]` (all 1.0). Tracks 7 and 9 use lane-only targeting (`func_80090A40`). [V] | Adds a per-track AI hint-point list `0x801108B0[t]` (`func_800EB690` (2049)) that Rush 2 has no code for. |

## 5. Mirror and backward

- **Backward** (Rush 2 byte `0x80119848`; 2049 byte `0x80152570`):
  - selects the backward path (+12 / file 177+)
  - selects the save/record slot, the demo lists and the AI table index
  - selects model `<prefix>FINISHB`
  - is disabled on STUNT (`func_80094698`)
- **Mirror** (Rush 2 byte `0x800D0190`):
  - render-time only: the view matrix skips its normal X negation (`func_8007C624`)
  - the projection fix-up is at +0x140
  - every loaded model's DLs have TRI1/TRI2/QUAD winding swapped in place (`func_800776DC` → `func_80077518` with
    callback `func_8006121C`)
  - steering is negated (`func_80076694`)
  - the minimap is flipped
  - physics and paths are untouched
- **Requirements for a converted track:**
  - Ship 2049's backward file as the +12 path.
  - Use display lists that `func_80077518` can walk: G_DL `0xDE` (call or branch), `0xDF` end, `0xE1` + `0x04`
    conditional, and 0x05/0x06/0x07 triangles. Anything else must be 8 bytes long. Segment-addressed DLs are
    handled (`& 0x0F000000`).
- **2049** has the same mirror option (text banks: "BACKWARD", "MIRROR"). [V]

## 6. Rush 2 per-track data (all sites) [V unless marked]

### 6.1 Tables

| Address | Size | Meaning | Readers |
|---|---|---|---|
| `0x800C182C` | char*[12] | Track prefix: placement name prefix and `<prefix>FINISH(B)` | `func_8008F080`, `func_800A5110` |
| `0x800C25AC` | char*[12] | Banner/record name (also the texture name in assets 4–15) | `func_800606A8` |
| `0x800C4C40` | char*[] | Display names (part of the UI string table; [12] = "PRESS L+R") | overlay `func_803BE5B0` (index via `0x800C4618`) |
| `0x803C9668` | char*[12] | Overlay copy of the banner names | `func_803C6208` |
| assets `0x04+t` | 255×128 image | Track banner (`func_80086A60(t+4)` in `func_800A5110`, `func_800ABE7C`; 0x10 = CIRCUIT) | |
| `0x800C1D7C` | 3 B × 13 | Fog / clear RGB; [12] = menu | `func_800A5ADC` (via `0x8010C440 < 12`), `func_800971A8`, `func_800AE670`, `func_803AB0E4`, `func_803B60F4` |
| `0x800C1D14` | f32[12] | Cloud UV scroll speed (× wind × dt) | `func_8008918C` |
| `0x800C1D44` | f32[12] | Sky parallax with camera movement | `func_8008918C` |
| `0x800CC37C` | s16[12] | Default song (→ sequence `0x800CC394`); random if t ≥ 12 | `func_8008C370` |
| `0x800CF9A4` | jump[12] | Visibility zone → group-mask tables `0x800C6538` … `0x800C9F18` (16 B per zone; LA also `0x800C8578`); `0x800CA1B8` = all visible | `func_8007C27C` (`sltiu 0xC`) |
| `0x800CA1A8` | u8[12] | Zone count per track (84, 93, 116, 123, 100, 90, 116, 42, 49, 57, 55, 41) | `func_8007C27C` |
| `0x800CA1F8` | ptr[12] | Fog-override zones {s16 x, z, radius, near, far, …} 0x10 each (NYONE only) | `func_80081074` |
| `0x800C5D2C` | ptr[12] | Animated/scripted prop list (VEGAS, NYONE, NYTWO, LA, STUNT1) | `func_8008A9C8`, `func_8008AF48` |
| `0x800C3F84`, `0x800C3FB4` | s32[12] | Minimap finish-icon x/y nudge | `func_800B9D48` |
| `0x800C1DBC` | u16[12] | Per-track secret bitmask, no-save copy (save +0x18+2t) [I meaning] | `func_8005F00C`, `func_8005F418`, `func_803B1AB0`, `func_803B1B78` |
| `0x800C45CC` / `0x800C456C` | s16 / ptr [24] | Demo start spine indices (t + 12b) | `func_8008E6B4` |
| `0x800D28F0` | s16[24] (bss) | Demo cursor | `func_8008E6B4` |
| `0x800C1A3C` | f32[24] | AI rubber-band scale (all 1.0) | `func_800A1A98` |
| `0x800D3B64` | s16 (bss), car·0x58 + (t + 7b)·2 | Circuit AI skill | `func_800A1A98` |
| `0x8001CF3C` | f32[24] (boot) | Seed time (s) for the default record tables | `func_800606A8` |
| `0x800D2160` | 2 × 22 × 0x12 (bss) | Record tables, slot = t + 12b − (≥ 11 ? 1 : 0) | `func_800606A8`, `func_800A952C` |
| `0x800C182C` / `0x8001CD64` / `0x800C185C` | — | Asset index arithmetic t + 0x33, 0x3F, 0x4B, 0x57 (+12) | `func_800A4C98`, `func_800AB878`, `func_80086A60` |

### 6.2 Not per-track (plan §6.3 corrections)

- `0x800C17AC`: HUD vectors.
- `0x800C1CB4`, `0x800C1AA4`, `0x800C1BD0`, `0x800C1C98`: sky-dome topology, vertices, UVs and alpha.
- `0x80023028`: frame dt.
- `0x80022FE8`: camera FOV.

### 6.3 Literal track tests

| Tracks | Sites and effect |
|---|---|
| 0 (VEGAS) | `func_800A45A8`: sky is model `SKYO1`. `func_80081074`: fog level 2 → 1. `func_8005CFB0`. `func_800A63A0`. |
| 1, 3 (NY) | `func_800A37F4`: forces the Taxi (0x10) / 0x1A. `func_800A4210`: traffic lists `0x800BF710` / `0x800BFBB8`, model `SUBWAYO1`. |
| 3 | `func_8005F118`, `func_800BB27C`: object 0x756. |
| 5 (LA) | Overlay `func_803C59A0`. |
| 7, 9 (HALFPIPE, PIPE) | `func_80081074`: no fog in 1P. `func_80090A40`, `func_8009D9F4`: lane-only AI / stunt handling. |
| 10 (ATARI) | `func_80081074`, `func_80094698`: fog 1, wind 0. `func_800A45A8`: no sky. `func_8005CFB0`. |
| 11 (STUNT1) | `func_800A45A8`: sky `STUNTSKYO1`. `func_80094698`: no backward or mirror. `func_800A34A8`: start from spine points. `func_80060E34`, `func_800B7654`, `func_800B8188`, `func_800B8900`, `func_800B8CC8`, `func_800B920C`, `func_800B9754`, `func_800AE670`, `func_803C5798`. |

### 6.4 Other per-track inputs

- **Sky** for all other tracks: a procedural dome textured with `SKY01` or `SKYFOUR` (random), looked up in the
  track's own textures (`func_800A45A8`). It is not drawn when fog ≥ 3, with 2+ players, or on ATARI.
- **Names the track file must provide:**
  - model `<prefix>FINISH` and `<prefix>FINISHB`. The model lookup result is used unchecked: a missing model sends
    `func_80077518` walking a garbage DL. **Provide at least a dummy.**
  - textures `CHKPNT` / `FINISH` (a NULL texture only causes a harmless read)
  - `SKY01` / `SKYFOUR` (or `SKYO1` in slot 0)
  - `SUBWAYO1` in slots 1 and 3
  - the props in the `0x800C5D2C` list for that slot

### 6.5 Rush 2 values

| t | prefix | banner | display | fog RGB | cloud | parallax | song | zones | flag nudge | props | fog zones | rec seed f/b | cp f/b | spine | br f/b |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 0 | VEGAS | VEGAS | LAS VEGAS | 292939 | 0 | 0 | 1 | 84 | 1,−4 | yes | – | 116/113 | 6/6 | 1215 | 4/5 |
| 1 | NYONE | NEWYORKL | LOWER MANHATTAN | 969696 | .2 | 0 | 0 | 93 | 0,0 | yes | yes | 104/107 | 6/6 | 1237 | 14/13 |
| 2 | HAWAII | HAWAII | HONOLULU | 9696d2 | .3 | .3 | 2 | 116 | 0,−2 | – | – | 142/143 | 4/4 | 1490 | 10/11 |
| 3 | NYTWO | NEWYORKU | UPPER MANHATTAN | 9696be | .2 | 0 | 6 | 123 | 0,0 | yes | – | 166/166 | 7/7 | 1596 | 9/9 |
| 4 | ALCATRAZ | ALCATRAZ | ALCATRAZ | 9696be | 1.0 | 1.0 | 4 | 100 | −4,0 | – | – | 75/76 | 7/7 | 824 | 5/3 |
| 5 | LA | LA | LOS ANGELES | a08058 | .2 | 0 | 3 | 90 | 0,0 | yes | – | 160/161 | 7/7 | 1607 | 5/5 |
| 6 | SEATTLE | SEATTLE | SEATTLE | 9696be | .3 | .2 | 5 | 116 | 0,0 | – | – | 114/112 | 4/4 | 1054 | 8/8 |
| 7 | HALFPIPE | HALFPIPE | HALFPIPE | 000000 | .2 | 0 | 7 | 42 | 0,0 | – | – | 68/69 | 5/5 | 758 | 0/0 |
| 8 | CRASH | CRASH | CRASH | 9696be | .2 | 0 | 7 | 49 | 0,0 | – | – | 84/93 | 6/6 | 918 | 0/0 |
| 9 | PIPE | PIPE | PIPE | 000000 | 0 | 0 | 7 | 57 | 0,0 | – | – | 85/78 | 4/4 | 738 | 4/4 |
| 10 | ATARI | ATARI | ATARI | 000000 | 0 | 0 | 7 | 55 | 0,0 | – | – | 91/98 | 4/4 | 827 | 8/8 |
| 11 | STUNT1 | STUNT1 | STUNT 1 | 000000 | 0 | 0 | 7 | 41 | 0,0 | yes | – | 167/134 | 3/5 | 281 | 5/0 |

- **Songs** map through `0x800CC394 = [3, 4, 0, 1, 7, 10, 2, 8]`.
- **Demo start lists** are in `paths`/`0x800C456C`. For example, VEGAS is [139, 359, 877, 1052] forward and
  [78, 323, 459, 848] backward.

## 7. Rush 2049 per-track data (race tracks 0–5 = files 158–163) [V unless marked]

- **Track id** byte `0x8014978C`:
  - 0–5 race, 6–13 battle, 14–17 stunt, 18 obstacle
  - copies: `0x801427A0`, `0x80142528` (t + 6b)
  - backward byte `0x80152570`
- **Literal tests:**
  - ≥ 6 / < 6 (non-race): many sites
  - `== 3 || == 5`: `func_800B0C48` (2049)
  - 7–13, 17, 18: `func_800BB69C` (2049)
- **Track names** are not in the text banks (files 0–5 hold only UI strings, including "BACKWARD" and "MIRROR").
  They are bitmaps `TRK_T1G1` … `TRK_T6G1` in file 60, next to the battle (`TRK_D1–8`), stunt (`TRK_S1–5`) and
  obstacle (`TRK_O1`) ones.

### 7.1 Tables

| Address (2049 main) | Meaning | Rush 2 counterpart |
|---|---|---|
| `0x80114658` | 3 B × 20 fog / clear RGB (`func_800FB2C8` (2049) → `func_800A7480` (2049), the twin of `func_80054DCC`) | `0x800C1D7C` |
| `0x8010FFD4` | s32[19] song (−1 = random) when the music option = 12 "per track" (`func_800D6160` (2049)) | `0x800CC37C` |
| `0x801173D8` / `0x80117408` | Demo start lists [12] | `0x800C456C` / `0x800C45CC` |
| `0x8002E870` (boot) | f32 seed record time, index t + 19·b | `0x8001CF3C` |
| `0x80111754` | f32[6] maximum ghost-recording time (s) (`func_800CC50C`, `func_800F6AB8` (2049)) | none (no ghosts) |
| `0x8011ADC0` | 2×RGBA per track: car SHEEN prim/env tint (`func_800B15B4` (2049)) | none (no sheen) |
| `0x8011A840`, `0x8011A31C` | Prop / animated object lists (`func_800BDAA8` (2049)) | `0x800C5D2C` |
| `0x8011AF90` | Per-track pointer into 3 shared tables used for car-state init [I] | ? |
| `0x801108B0` | AI hint points {u8 flag, f32 x, y, z} (`func_800EB690` (2049)) | none |
| `0x8011E76C` | Fog-override zones (all NULL) | `0x800CA1F8` |
| `0x801210E8` | Grid slot table (6 race slots) | `0x800CC2BC` |

The 2049 AI rubber band has no per-track factor (`func_800F93A0` (2049)).

### 7.2 Values

| t | file f/b | fog RGB | song | ghost max s | rec seed f/b | sheen tint | cp f/b | spine | br f/b | demo starts f / b |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | 158/177 | 9696be | 0 | 225 | 47/48 | fcdd7eff ff5b35ff | 6/5 | 651 | 10/7 | 0,242,494 / 0,250,480 |
| 1 | 159/178 | 9696be | 1 | 285 | 81/79 | ffffffff 80affdff | 6/6 | 920 | 8/6 | 0,258,484,700 / 0,270,525,740 |
| 2 | 160/179 | 8c8c8c | 4 | 300 | 65/76 | fdb370ff fedf71ff | 5/5 | 907 | 6/7 | 0,250,520,740 / 0,250,520,770 |
| 3 | 161/180 | 000000 | 2 | 390 | 97/104 | ca96ffff 90c7ffff | 8/7 | 976 | 7/8 | 0,230,430,730 / 0,220,450,760 |
| 4 | 162/181 | 8c8c8c | 3 | 330 | 78/86 | ca96ffff e4fd9eff | 6/6 | 878 | 6/5 | 0,200,430,600 / 0,260,515,750,1030 |
| 5 | 163/182 | 000000 | 7 | 450 | 117/119 | fea664ff fef071ff | 6/6 | 1383 | 5/5 | 0,230,420,680 / 0,260,505,760,1027 |

2049 song ids index 2049's own MusyX music. Rush 2 needs a Rush 2 song number (plan §9.8).

## 8. What a converted 2049 race track needs (replacing Rush 2 slot t)

1. **Paths:** asset `0x57+t` = 2049 file `158+k`, and `0x63+t` = file `177+k`, both unchanged
   (`paths.convert`). Update `0x8001CD64` (sizes) and `0x800C185C` (offsets).
2. **Fog / clear colour:** `0x800C1D7C[t]` = 2049 `0x80114658[k]`.
3. **Sky:** set cloud speed and parallax `0x800C1D14` / `0x800C1D44[t]` to taste. 2049 has no matching table; use
   0 for a static sky.
   - The geometry must contain `SKY01` or `SKYFOUR` textures, or `SKYO1` in slot 0.
   - Or pick a slot whose sky test suits the track.
4. **Music:** `0x800CC37C[t]` = a Rush 2 song.
5. **Demo start lists:** `0x800C456C` / `0x800C45CC[t]` and `[t+12]` = 2049 lists `[k]` and `[k+6]`. They are spine
   indices of the same path, so they carry over directly.
6. **Record seeds:** `0x8001CF3C[t]` and `[t+12]` = 2049 `0x8002E870[k]` and `[k+19]`. Cosmetic.
7. **Visibility:**
   - make `func_8007C06C` return −1 so the `0x800CA1B8` all-visible mask is used, keeping the group count ≤ 128
   - or build a zone table from 2049's own data (geometry agent's topic)
   - `0x800CA1A8[t]` = group count
8. **Minimap:** nothing needed (runtime). Set the finish nudge `0x800C3F84` / `0x800C3FB4[t]` to 0.
9. **Fog-override zones:** `0x800CA1F8[t]` = NULL, or keep NYONE's only in slot 1. Props `0x800C5D2C[t]` = NULL,
   or a list matching names that exist.
10. **Required names:** the track must define `<prefix>FINISH` and `<prefix>FINISHB` models (dummies are fine). Keep
    the slot's prefix or replace `0x800C182C[t]`.
11. **Slot side effects:** the literal tests in §6.3 apply to the chosen slot.
    - Avoid 1/3 (taxi and subway).
    - Avoid 7/9 (lane-only AI, no fog).
    - Avoid 10/11 (no sky, fog/wind forced, stunt rules).
    - Avoid 0 (`SKYO1`).
    - **Best slots for a 2049 race track: 2 (HAWAII), 4 (ALCATRAZ), 5 (LA), 6 (SEATTLE), 8 (CRASH).** Slot 5 has an
      overlay text quirk; slot 4 has a minimap nudge.
12. **Grid:** Rush 2 places 8 cars, 2049 6. Rows 7–8 sit 60 ft behind the anchor. Check that 2049 start straights
    have 60 ft of road behind checkpoint 0, or cut the field to 6 cars.
13. **Not portable** without new code (drop them): 2049 AI hint points, sheen tints and ghost limits.

## 9. Adding 2049 tracks as NEW slots (t ≥ 12): every site to change

**Index arithmetic and loaders**
- `func_800A4C98` and `func_800AB878`: t + 0x33, 0x3F, 0x4B, 0x57 (+12). Collides with the next asset group for
  t ≥ 12.
- `func_80086A60`: hardcoded ranges 0x4B–0x56 and 0x57–0x6E.
- Decompressor choice by index: `func_80077FE0`, `func_8008687C`, `func_800A4B04`.
- The asset tables `0x800C185C` and `0x8001CD64` (0x71 entries) must grow.
- Banner asset t + 4: `func_800A5110`, `func_800ABE7C`. Collides with 0x10 CIRCUIT and up.

**Tables sized for 12**
- `0x800C182C`, `0x800C25AC`, `0x800C4C40` (the next strings are UI), `0x803C9668` (overlay)
- `0x800C1D7C` (13; [12] is the menu)
- `0x800C1D14`, `0x800C1D44`, `0x800CC37C`
- jump table `0x800CF9A4` (`sltiu 0xC` in `func_8007C27C`) and `0x800CA1A8`
- `0x800CA1F8`, `0x800C5D2C`, `0x800C3F84`, `0x800C3FB4` (read with `%12`)
- `0x800C1DBC`

**Tables of 24 (t + 12·backward):** `0x800C45CC`, `0x800C456C`, `0x800D28F0` (bss), `0x800C1A3C`, `0x8001CF3C`.
New tracks need a new backward offset scheme.

**Checks against 12, 11 or 0xB**
- `func_8008C370` (`t >= 12` → random song)
- `func_800A5ADC` (`0x8010C440 < 12`)
- `func_800606A8`, `func_80065360`, `func_8006544C`, `func_800A7804`, `func_800A9164`, `func_800A9250`,
  `func_800A952C`: save slot `t + 12b − (≥ 11)`
- `func_8009E400` (`slti 0xB` cycle; locks on 9 and 10)
- `func_8009E45C` (0–8, 9, 10)
- `func_800AB824` (cycle up to 7 / 10)
- overlay `func_803AB294` (track list loop < 0xB) and `func_803AB01C`, `func_803ABE0C` (0xA/0xB special cases)

**Save data**
- player save (`*(0x800C2140 + p·0x28 + 0x24)`):
  - +0x18 u16[12] secrets
  - **+0x30 low nibble = last track** (4 bits, max 15)
  - +0x1F0 22 × 0x20 records (index t + 12b − 1 for ≥ 11)
- circuit table `0x800D3A60` (u8 track per race)
- `0x800D3B64` (t + 7b index)
- `0x800D2160` record tables (22 slots)
- The `.mpk` layout is fixed. Extra tracks need side storage.

**Literal tests** (§6.3): a new slot gets the "generic" behaviour automatically, except `func_8005CFB0`, which
enables a wind-driven texture scroll (`0x800D5710`) for every t ≠ 0, 10, 11. That is harmless.

**Track id copies** to keep consistent: `0x8010C440`, `0x800E7C18`, `0x800D3938`, `0x800C400C`, `0x80125AA8`,
`0x803D05AC`, `0x800D9C10[car*9]`.
