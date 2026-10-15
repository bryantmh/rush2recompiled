# Track collision files: Rush 2 and Rush 2049

Tags: **[V]** verified in code and data, **[I]** inferred. Addresses are N64 virtual addresses. Rush 2 functions are
`func_XXXXXXXX` in `analysis/out_disasm/r2.asm`. Rush 2049 functions are in `d49.asm`; its *second* copy of main
(from the line where `80086A50` appears for the second time) is the real one. The first region at the same addresses
is boot-segment overrun and is garbage.

Tool: `tools/rush2049/collision.py`, a parser for both formats, a validator, a 2049 → Rush 2 converter and a
self-check (`python collision.py` from `tools/rush2049`).

## 1. Summary

- **Same format family.** Five of the six sections have identical record layouts in both games. The engines read
  them with near-identical code.
- **Rush 2 parser reproduces the files exactly.** Re-serialising all 12 Rush 2 files gives byte-identical output. The
  2049 vertex-list and leaf sections also re-encode byte-exactly. Every byte of both formats is accounted for.
- **2049 changes:**
  - one new section (moving collision, "movers")
  - a different run-marker encoding in the vertex lists
  - 17-bit leaf offsets
  - a re-purposed polygon `info` word
  - the sections in a different order
- **Converter result.** `convert()` turns all 19 2049 files into valid Rush 2 files.
  - All 19 are semantically identical to the source for everything Rush 2 reads, quadtree included.
  - The recomp walks a converted file's quadtree the 2049 way (split rounding, query cell, 17-bit leaf offsets): see
    Quadtree lookups below. Rush 2's own walk misses walls on 2049's data.
- **Lost without code support:**
  - moving collision (movers)
  - boost pads
  - "ride-on" platforms
  - the car-lighting colour index (visual only)

## 2. Header and section order **[V]**

| | Rush 2 (`func_800814B0`) | Rush 2049 (`func_800AB638`) |
|---|---|---|
| Header | 0xC bytes: u16 `nSeg, nNode, nPoly, nVert, leafBytes, vlistBytes` | 0x10 bytes: u16 `nSeg, nNode, nPoly, nVert, nMover, vlistBytes`, u32 `leafBytes` |
| Sections | SEG, NODE, POLY, VERT, **LEAF**, **VLIST** | SEG, NODE, POLY, VERT, **MOVER**, **VLIST**, **LEAF** |
| SEG ptr | 0x80110BD8 (n×0x84) | 0x80152034 |
| NODE ptr | 0x8010D390 (n×0x14) | 0x80124EEC |
| POLY ptr | 0x8011001C (n×0x18) | 0x801497F8 |
| VERT ptr | 0x80110050 (n×8) | 0x8015201C |
| MOVER ptr | none | 0x801525EC (n×0x20), count also at u16 0x8015267C |
| LEAF ptr | 0x8011187C | 0x80152460 |
| VLIST ptr | 0x80111930 | 0x80152568 |
| file base | 0x80112448 | 0x801526F0 |

- **Section naming.** The earlier notes' "E" (Rush 2) and "G" (2049) are the leaf lists. Rush 2's trailing section
  and 2049's "F" are the vertex lists. Pairing them by their readers proves it: the leaf lists are read together with
  POLY by the query functions, and the vertex lists together with VERT by the polygon test.
- **Header word +0xA.** In Rush 2 it is the vertex-list byte count; all 12 files total exactly.
- **Size limits.** Rush 2's header limits both byte sections to 0xFFFF. All 2049 vertex-list sections fit (max 0xB178).

## 3. Record layouts

### SEG, 0x84 bytes: road-surface spline segment **[V layout, I meaning]**
- **Readers:**
  - `func_8006BDA8`, used for polygons with flag 0x1000: smooth/banked road surface.
  - `func_8006C2B8`, used for flag 0x2000: circular-arc surface (half-pipes and pipes).
  - 2049 equivalents `func_800ACC18` and `func_800AD128` read **identical offsets** (instruction-level diff).
- **Ordering:** segments form a loop; next = i+1, wrapping to 0 at `nSeg` (header +0).

| Offset | Contents |
|---|---|
| +0x00 | vec3 origin |
| +0x0C | 3×3 float matrix (world → segment frame, applied to point − origin by `func_80054454`) |
| +0x30, +0x3C, +0x48 | vec3s, lerped between this segment and the next |
| +0x54 | float scale on the +0x30 offsets |
| +0x58, +0x5C, +0x60 | floats; +0x60 is the segment length used to compute the parameter t |
| +0x64–+0x70 | four floats: across-track mapping coefficients |
| +0x74, +0x78 | floats: cubic height terms |
| +0x7C–+0x83 | not read by either reader (often `80000000` = −0.0) |

### NODE, 0x14 bytes: quadtree over world X/Z **[V]**
- **Readers:** Rush 2 `func_8006CE28` (walk up from a cached node, then descend) and `func_8006CD60` (descend);
  2049 `func_800ACA9C` / `func_800AC9BC`.

| Offset | Contents |
|---|---|
| +0 | s16 parent (−1 = root) |
| +2 | u8 0 |
| +3 | u8 child mask |
| +4 | s16 xmin, xmax, zmin, zmax (integer world units) |
| +0xC | u16 child[4] |

- **Quadrant:** q = (x ≥ midX) + 2·(z < midZ) (`func_8006CD60`, `func_800AC9BC`).
- **Child mask:**
  - Bit q set → child[q] is a node index. Index 0 = null quadrant (no collision).
  - Bit q clear → child[q] is a byte offset into LEAF (0 = empty).
  - **2049 only:** mask bit 4+q set → leaf offset += 0x10000. Only file 141 uses it (leaf section 0x184A4).
- **Query cell:** floor of the query x/z.
  - Rush 2 computes it as x − 0.99 then truncation for negatives, so it is wrong in a 0.01-wide band.
  - 2049 uses an exact floor.
- **Midpoint rounding differs:**
  - Rush 2: `(min+max)>>1` (floor).
  - 2049: rounds toward zero (`func_800AC9BC`: +1 before the shift when the sum is negative).
  - Both games' build tools split children toward zero, so Rush 2 is already "off" on its own data, which is how the
    original Rush 2 behaves on its own tracks.
  - On 2049's data it loses walls: see Quadtree lookups.

### Quadtree lookups **[V]**

Rush 2's walk differs from 2049's in the split rounding and the query cell above, and it has no 17-bit leaf offsets.
The split strips (cells on a split line whose min + max is negative and odd) go to the wrong child, and walls often lie
on those lines. Such a wall is then listed only for the cells on its far side, so a car coming from the near side
never tests it and drives into it (reported on 2049 track 6's covered shortcut: x ≈ −1176 and −1195, z −1830 to
−150). `collision.py` `wall_gaps(c, rules)` counts them: walking 2049's files with Rush 2's rules, 45–195 walls per race
track (file 144: 62 of 2510) and 0–117 per arena have cells whose leaf lacks them; with 2049's rules, none (one edge
cell on file 144). Floors never lose a polygon, since floor leaves overlap.

2049's leaf queries take the cell as x − 1.0 then truncate, for negative x that isn't a whole number, then call
`func_800ACA9C` (checked in `func_800C4200` at 0x800C42B8 and `collision_poly_extract_800C6AA0` at 0x800C6B50).
Rush 2's four (`func_8006CF00`, `func_8006F704`, `func_8006FC94`, `func_8008BDC0`) subtract 0.99 (floats at
0x800CF7B0/B4, 0x800CF80C-18 and the like) before `func_8006CE28`.

**Port** (src/rush2049/track2049_collision.cpp, hooks in us.toml):
- The converter keeps 2049's tree unchanged and tags the file: root NODE +2 (unread by both games) = 0x80 | the leaf
  section size's bits 16–22, and the mask keeps bit 4 + q where a leaf offset passes 16 bits. The header's u16 leaf size
  holds the low 16 bits.
- On a tagged file only (Rush 2's, SF Rush's and custom tracks keep Rush 2's walk):
  - `func_800814B0` at 0x80081550: the leaf section size gets its upper bits back, so the vertex lists are found.
  - `func_8006CD60` at 0x8006CDA8: the splits are rounded toward zero.
  - The four queries, after their cell (0x8006CFBC, 0x8006F7D0, 0x8006FD60, 0x8008BF78): 2049's exact floor.
  - Their leaf fetches (0x8006CFEC, 0x8006F820, 0x8006FD88, 0x8008BFAC): + 0x10000 for mask bit 4 + q.
- `collision.py` `lookup_check` walks each converted file as the hooks do and the 2049 file as 2049 does, over every
  wall cell and 200 000 random cells: the leaves are the same for all 19 files.
- In game, `RUSH2_TEST_COLLISION=1` probes every floor polygon with `func_8006CF00` under both walks on the race's first
  tick and logs the difference. Track 6: 956 894 probes, 727 floors found only with 2049's walk, none lost. Track 3
  (17-bit offsets): 444 found, 6 that 2049's leaf answers with a surface nearer in height (its leaf lists the floor:
  checked offline).
- Leaves stay at most 29 polygons (file 139), inside the queries' u16[36] stack buffers (`func_8006FC94` has 40).

### POLY, 0x18 bytes **[V layout]**

| Offset | Field | Meaning |
|---|---|---|
| +0 | u16 flags | bits 0–3 **type** (§5)<br>bits 4–7 **wheel material** → per-wheel car+0x60C (2049 +0x61C)<br>bits 8–11 magnitude, sign = bit 15 → car+0x614 (2049 +0x624), used as (1 + 0.02·n) speed factor in the drone/speed code `func_80075880` **[I]**<br>0x1000 = surface from SEG via `func_8006BDA8`<br>0x2000 = arc surface from SEG via `func_8006C2B8`<br>bits 12–13 also select the VLIST trailer |
| +2 | u16 info | bits 0–3 = vertex count. Upper bits **differ per game** (§4). |
| +4 | s16[9] | rotation matrix ×16384 (`3880 0000` = 2⁻¹⁴); world → polygon-local frame |
| +0x16 | u16 | byte offset into VLIST |

- **Extraction is identical in both games:** Rush 2 `func_8006F704`, 2049 `func_800C6AA0`. The 2049 struct offsets
  are 0x10 higher.

### VERT, 8 bytes **[V]**
- **Layout:** s16 x, y, z, then u16 fraction bits (x: bits 14–10, y: 9–5, z: 4–0). Value = int + frac/32.
  - Rush 2 decodes with ×1/32 in double precision, 2049 in single; both are exact.
- **Coordinates:** vertex 0 of a polygon is its **world origin**. Vertices 1..n−1 are **in the polygon's local frame**
  (local y = 0).
  - The point test transforms (P − v0) by the matrix, then does 2D edge tests on local x/z (`func_8006C670`,
    `func_800AD734`).
  - Data: local |y| = 0 except 6 non-planar quads in ALCATRAZ.
- This is why rotating or translating a polygon only needs its matrix and origin vertex (see MOVER).

### VLIST, per polygon (contiguous, in file order) **[V]**
- **Encoding:** count = info & 0xF. Read u16 v. If indices remain (≥ 2) and the next byte ≥ MARK, consume it: run =
  byte & MASK, and the indices are v … v+run.
  - **Rush 2:** MARK 0xC0, MASK 0x3F (`func_8006C670`).
  - **2049:** MARK 0xE0, MASK 0x1F (`func_800AD5D0`). Same length, different marker.
  - A raw 2049 list read by Rush 2 would turn run byte 0xE0+r into 32+r: **must be re-encoded**.
- **Trailer:** if flags & 0x3000, a u16 **SEG index** follows. The reader always reads these two bytes, but they are
  meaningful, and present, only for spline polygons.

### LEAF lists **[V]**
- **Encoding:** u8 count, then u16 entries (bits 15–13 = extra run, 12–0 = first poly index), so at most 8191
  polygons.
- **Readers:** `func_8006CCEC` (Rush 2) and `func_800ADCE0` (2049) are instruction-identical.
- **Layout:** byte 0 of the section is a pad, so offset 0 can mean "empty". Lists are unique and stored in node order.

### MOVER, 0x20 bytes, 2049 only: moving collision **[V]**

| Offset | Contents |
|---|---|
| +0 | u16 object id |
| +2 | u16 poly index |
| +4 | s16[9] rest matrix |
| +0x16 | u16 vertex index (always the polygon's origin vertex) |
| +0x18 | 8-byte rest vertex |

- **Data:**
  - In all files the rest copies equal the file's POLY/VERT contents.
  - Every mover polygon, and only those, has info bit 0x20 with info>>11 = its object id.
  - Track files have 9–16 ids and 12–62 movers. The obstacle course (157) has 24 ids and 111 movers.
- **Object id:** matches the animated-object path header field +0x16. That is the geometry file's `PATH`/`PTHD`
  objects; see `func_800C1A00`, which scans the object list at 0x8013C300.
- **Code:**
  - `func_800B2CB4(id)`: set type 0xF (disable) on all of the object's polygons.
  - `func_800B2D20(id)`: restore the rest matrix and origin vertex.
  - `func_800C0294(id, delta)`: translate the origin vertices (re-quantised to 1/32).
  - `func_800BF838(id, …)`: rotate (matrix = rest ∘ object rotation ×16384, origin rotated).
  - Callers are the animated-object update at 0x800B2E..0x800B30 and 0x800C0F94..0x800C17BC. Objects are disabled or
    reset depending on path flags and game mode.

## 4. POLY+2 (`info`) upper bits: the meaning changed

**Rush 2 [V]:**
- The full word goes to per-wheel car+0x61C.
- `func_8009A264` majority-votes it over grounded wheels into car-state +0x33C.
- Car-state **+0x344 = 2·bit15 + bit14** ("covered" level).
- +0x344 ≠ 0 skips the in-air control code in `func_8006AFD8`; that code scales car+0x4C/+0x50 by setting byte
  0x8010D388.
- It is also read by `func_8005CFB0` (level 2 selects a different sound or effect) and `func_80064F70` **[I: tunnels
  and covered areas]**.
- Data uses only 0x7000, 0xB000 and a few 0x40/0x80/0xC0/0x3240 values.

**2049 [V]:**

| Bits | Meaning |
|---|---|
| 0x10 | **boost pad**: `func_800E1F80` drives the car toward (info>>11)×8 mph along the polygon's local X axis. Strong mode when ≥ 56 mph, otherwise an additive force. Values seen: 88–248 mph. |
| 0x20 | **platform / mover**: the car is carried by the animated object (info>>11) via `func_800C1A00` (object path direction × speed). |
| both | When either is set, the wheel copy is masked to 0x7FF and car+0x648 records the polygon (wheel within 1.0 unit). |
| 0x100 | **covered flag** → car-state +0x35A (`func_800E847C`). Gates the same in-air code as Rush 2's +0x344 (`func_800E23A4`) and fades car lighting (+0x360). |
| 11–15 (when not 0x30) | **car lighting colour index** into a per-track table at 0x8011AF90 + 4·track (`func_80093B20`). Visual only. |
| 6, 7, 9, 10 | no reader found **[I unused]** |

**Converter mapping (`convert_info`):**
1. If 0x30 is set, mask the word to 0x7FF, as 2049 does.
2. Clear bits 4, 5, 8, 14 and 15.
3. Set 0x4000 when 2049 bit 8 was set.

This reproduces the covered flag exactly. It keeps the other bits so the wheel vote groups wheels the way 2049 does.

## 5. Surface types (POLY flags & 0xF)

The same in both games: compare `func_8006F704`/`func_8006FC94` with `func_800C6AA0`/`func_800C5644`/`func_800C3AD0`.

| Type | Rush 2 data | 2049 data | Behaviour |
|---|---|---|---|
| 0 | most ground | most ground | normal surface [V] |
| 1 | yes | yes | drivable; pairs with material 1 in data. Rush 2 also uses type 1 / material 1 as its "no ground found" default (2049 uses 0/0). [I off-road] |
| 2 | rare | rare | drivable variant [I] |
| 3 | yes | file 143 (track 5) only | wheel within 0.25 → car wrecked (`func_8006E1A8` / `func_800C54F0`) plus sound 0x6F (2049: 0x15). Also hits from body points. [V code, I "deadly water"] |
| 4 | HAWAII ×2 | none | sets car+0x648 (2049 +0x640); 2049 battle/stunt modes also call `func_803914B4`. [I water/sink] |
| 5 | walls | walls | ignored by wheel probes; wall response in body probe (`func_8006FC64`). [V] |
| 6 | yes | yes | as 5 (HALFPIPE's arc surfaces are type 6 + 0x2000) [V] |
| 7 | yes | yes | wheel within 0.25 → wrecked, no sound [V] |
| 8 | none | none | runtime "airborne" marker in car+0x5FC [V] |
| 0xF | none | runtime | 2049 only: disabled mover polygon; skipped by every 2049 query [V] |

- **Type codes:** no remapping is needed. The 2049 data uses only 0, 1, 2, 3, 5, 6 and 7, all with Rush 2 meanings.
- **Material (bits 4–7):** same extraction and same consumers' pattern in both games: effects and skid code
  `func_80065588`, `func_800663CC`, `func_80067358`; 8 = airborne. Copied unchanged.
- **Type 7 is the out-of-bounds floor.** Rush 2 counts: SEATTLE 135, ALCATRAZ 14, STUNT1 8 (type 3 water: HAWAII
  48, ALCATRAZ 59, SEATTLE 14, NYONE 3). The wheel probe `func_8006F704` loads the type into `$v1` at 0x8006FAD8;
  types 3, 4 and 7 go on to their effects (skipped while the car slot's +0x342/+0x343 is set or car+0x6C8 >= 0),
  any other returns. Type 7 sets car+0x6D1 and calls `car_wreck_trigger` (`func_8006E1A8`: car index, at most one
  wreck per 5 s, placement case 2). The body probe `func_8006FC94` wrecks on types 3 and 4 only (0x800700D8).
  The **No Out-of-Bounds Wreck** cheat (src/cheats.cpp) hooks 0x8006FADC and turns a human car's type 7 into 0.
- **Subway trains** are pseudo-cars, not polygons: `subway_init` (`func_800A4210`) sets the count `subway_count`
  (0x800D4E74) and the 0x81C-byte bodies `subway_bodies` (0x800D50E4), both 0 on a track without trains. The
  **No Subways or Buses** cheat leaves them at 0 (src/rush1/track1_buses.cpp, which also hosts SF Rush Sunset's buses).

## 6. What converts losslessly

- **Copied as-is:** SEG, NODE (2049's tree, tagged: see Quadtree lookups), POLY flags, matrices, VERT, polygon vertex
  lists (re-encoded, same offsets) and leaf lists.
- **Checks:** the self-check re-parses each output, validates every index and compares it with the source.
- **File 141 (2049 track 3):** the leaf section is 99 492 bytes (0x184A4), past 16-bit offsets. It is kept whole,
  with 2049's 17-bit offsets (see Quadtree lookups).
  - Earlier versions merged 1022 bottom nodes into supersets to fit 16 bits. That changed which walls a point was
    tested against near the merged cells, and merged leaves had to stay at 36 polygons or fewer: the four queries copy
    a leaf into an unchecked u16[36] stack buffer (the next local sits 72 bytes in for `func_8006CF00`, sp+0x94 → 0xDC,
    `func_8006F704`, 0xA4 → 0xEC, and `func_8008BDC0`, 0x108 → 0x150; `func_8006FC94` has 40, 0xF4 → 0x144). A
    59-polygon leaf overwrote `func_8006FC94`'s saved point pointer (sp+0x15C), crashing in `func_8006C670`. Rush 2's
    own tracks reach 34.

## 7. What is lost, and the Rush 2 code needed for full fidelity

1. **Movers (all race tracks).**
   - Without code, the collision stays at the rest pose, which is what POLY/VERT hold. Drawbridges, trap doors,
     elevators and rotating platforms are therefore solid where they start, and never disabled.
   - Matching visuals need the geometry drawn at the same rest pose.
   - Full support:
     - keep the MOVER table
     - each frame, for each animated object id, apply `func_800B2D20` / `func_800C0294` / `func_800BF838` /
       `func_800B2CB4` logic to the in-RAM POLY and VERT records (the Rush 2 file buffer is writable)
   - This depends on the animated-object (PATH/PTHD) system, which Rush 2 does not have.
2. **Ride-on platforms (0x20, same polygons as movers).**
   - When a wheel is within 1.0 of such a polygon, add the object's path velocity (`func_800C1A00`) to the car's X/Z
     velocity. This is `func_800E1F80`'s 0x20 branch, 2049 car+0x22C/+0x234 scaled by +0x634.
   - Needs the mover system.
3. **Boost pads (0x10).**
   - Counts: 78 polygons on track 2 (file 140), 3 on track 3 (141), 4 on track 5 (143), and some arenas/obstacle
     course.
   - Needs a port of `func_800E1F80`'s 0x10 branch, called after the wheel probe when the poly pointer is set:
     - With ≥ 3 wheels on pads, the whole car velocity is pushed to the target speed along the pad direction.
     - Otherwise, a per-wheel force is applied.
   - Needs the 2049 → Rush 2 car-struct offset mapping: 2049 +0x124 → ?, +0x5C0, +0x5EC, +0x64+12i, +0x2EC.
4. **Lighting colour index and covered fade.** Visual only, no driving effect. Ported, see Car lighting below.

`collision.extras(data_2049)` returns the movers, boost/platform polygons, covered polygons and colour indices for
a future runtime patch.

### Car lighting

**2049 [V]** (`func_80093B20`, per car and frame):
- Colour table: pointer at `0x8011AF90 + 4·track` (track = the collision file − 139). Three 26-entry RGBA tables
  are shared (0x8011AE58 most tracks, 0x8011AEC0 track 4, 0x8011AF28 track 5 and the later arenas). Index 0 is white
  (0xDCDCDC on track 4), 1–8 greys down to near black, 9–25 pale lamp colours. Track 6 is mostly index 6 (0x282828)
  with lamp pools in 12, 17, 18, 21, 23 and 24.
- Each wheel's polygon (car state +0x344/+0x346/+0x348/+0x34A, the info words) gives `table[(info & 0xF800) >> 11]`;
  the four are averaged per channel (sum >> 2). This also runs for mover polygons, whose bits 11–15 are an object id.
- In the air: with wheel 2's height (player struct +0x5F4; the wheel heights are +0x5EC + 4w) at 20 or more,
  t = (h − 20) × 0.1 (0x80123A60) blends toward `table[s32 0x8011B468 + 4·track]`, fully past 30.
- `func_80092FE0(car, &colour)` writes the colour (alpha 0xFF) into node +0x3C (PRIM) of the car's nodes at car node
  table 0x80139320 + car·0x40, +2, +0x12, +0x1E, +0x22, +0x26, +0x2A (body and wheels). The body combiner
  `FC127E03 FF0FF3FF` is TEXEL0 × SHADE, then × PRIM. Game mode 2 gives the other cars PRIM alpha 0x80, and battle
  cloaking (+0x38C bit 0) takes alpha from +0x3A1.
- Covered fade: car state +0x360 steps by 0x19 down to 0 while +0x35A (covered, 2049 info bit 8) is set, up to 0xFF
  otherwise. It is the alpha of the two colours from `0x8011ADC0 + 8·track` that go to the SHEEN node (car node
  table +0x14), the reflection pass.

**Port** (`src/rush2049/track2049_movers.cpp`, `src/rush2049/car2049.cpp` `light_model`):
- The converted collision file keeps 2049's polygon numbering, and Rush 2 keeps each wheel's polygon at physics car
  +0x594, so the 2049 info word is looked up from the 2049 file; the converted info word is not used. The colour is
  worked out per car and physics tick with wheel 2's Rush 2 height (+0x5E4). The tables are read from the 2049 ROM
  (`rush2::track2049::car_light_table`).
- Car Lighting = Light Probes bakes the painted polygons into the probes (`car_light_paint`,
  docs/graphics_effects.md#car-light-probes).
- 2049 car bodies keep their PRIM combiner; every draw of one (menus too) is wrapped to set PRIM: the car's colour on
  a 2049 track, white elsewhere. The Rocket ZX's flames (ROKTFLAMEG1-3) have the same combiner and are drawn in their
  own pass after the world (`rush2_car49_flames_draw`), which sets PRIM white: they glow, unlit.
- Rush 2 car parts (Rush 2 cars, and the Rush 2 rims under 2049 bodies) use Rush 2's car combiner `FC1216AC
  F0FFFE38`: TEXEL0 × SHADE, then a blend toward ENV by ENV alpha. Rush 2 uses it to darken cars in tunnels
  (`func_8005CFB0`, by car state +0x344: body and +6 nodes get ENV black at alpha 0x80 for level 1, 0x3C for level
  2 and up, PRIM 0xB4B4B4; out of tunnels ENV 0 and PRIM white, 0x800C5620-0x800C5630). The car models (assets
  0x1D-0x32) use these combiners: `FC1216AC F0FFFE38` (TEXEL0 × SHADE, blend toward ENV), `FC127EAC F0FFF238` (the
  same with alpha TEXEL0: a translucent overlay over the body), `FC567FFF 4FFE7C38` / `4FFE7638` (SHADE blended
  toward ENV), `FC41FFFF FFFFF238` (SHADE × PRIM, the plain parts), `FC327FFF FFFFFC38` and `FC127E03 FFFFF3F8`
  (× PRIM) and `FC127FFF FFFFF238` (TEXEL0 × SHADE). On a 2049 track the parts are drawn from copies of their display
  lists (`include/dl_copy.h`) in which every ENV blend multiplies by ENV instead (`FC121605 FFFFFFF8`, `FC127E05
  FFFFF3F8`, `FC42FFFF FFFFFC38` / `FFFFF638`; alpha stages unchanged). The two combiners with neither ENV nor
  PRIM, `FC127FFF FFFFF238` (most cars' roof and side panels, e.g. the SUV's SUV_TOP / SUV_SIDE) and `FCFFFFFF
  FFFE7C38` (SHADE), are made to multiply by ENV as well (`FC127E05 FFFFF3F8`, `FCFFFE05 FFFE7DF8`): left alone they
  stayed at full brightness on track 6 and the panels' window highlights showed as bright spots. ENV = the colour × (1 − the body node's
  ENV alpha) at alpha 0 and PRIM = the body's PRIM × the colour, alpha kept, so Rush 2's darkening stays. A blend lays
  a flat film of the colour over the car; leaving the overlay's combiner as a blend at full ENV alpha drew it as
  bright streaks rolling over the car. A list that can't be copied (a ghost car draws its own copy) falls back to the
  blend: ENV = black at 1 − (smallest channel) plus the rest of the colour at half strength.
- A race car's nodes are its body node's subtree (car record 0x80219DD0 + car·0x134, +0) and its four wheel nodes'
  subtrees (+0x18 + 4w, set up by `func_800837E0`; the wheels are not under the body).
- Not ported: the SHEEN pass and its covered fade (the port doesn't draw SHEEN), and the mode 2 / cloak alphas.

## 8. Cross-references a converter must keep consistent

- **Mover ids and platform parameter** (info>>11 with 0x20) = the geometry file's animated-object (`PTHD`) ids. Keep
  them in sync if the geometry conversion renumbers objects.
- **No other cross-file references:**
  - SEG indices are internal (VLIST trailers).
  - There are no names, model handles or AI-path indices.
  - Coordinates are world units in the same space as geometry (geometry vertices are ×16).
