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
  - 18 of 19 are semantically identical to the source for everything Rush 2 reads.
  - File 141 (race track 3) needs its quadtree partly merged to fit Rush 2's 16-bit leaf offsets. The merged leaves
    are supersets of the originals, which keeps ground queries the same.
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

- **Quadrant:** q = (x ≥ midX) + 2·(z ≥ midZ).
- **Child mask:**
  - Bit q set → child[q] is a node index. Index 0 = null quadrant (no collision).
  - Bit q clear → child[q] is a byte offset into LEAF (0 = empty).
  - **2049 only:** mask bit 4+q set → leaf offset += 0x10000. Only file 141 uses it (leaf section 0x184A4).
- **Query cell:** floor of the query x/z.
  - Rush 2 computes it as x − 0.99 then truncation for negatives, so it is wrong in a 0.01-wide band.
  - 2049 uses an exact floor.
- **Midpoint rounding differs:**
  - Rush 2: `(min+max)>>1` (floor).
  - 2049: rounds toward zero.
  - Both games' build tools split children toward zero, so Rush 2 is already "off" on its own data.
  - Sampling 200k cells per track: 0.01–0.35 % of cells route to a different leaf in both games. **None of them lose a
    ground polygon**, because leaves overlap.
  - Converted files behave in Rush 2 exactly as Rush 2's own tracks do. Negligible.

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
| 11–15 (when not 0x30) | **car lighting colour index** into a per-track table at 0x8011AF90 + 4·track (`func_800930A4`). Visual only. |
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

## 6. What converts losslessly

- **Copied as-is:** SEG, NODE (mask high bits cleared), POLY flags, matrices, VERT, polygon vertex lists (re-encoded,
  same offsets) and leaf lists.
- **Checks:** the self-check re-parses each output, validates every index and compares it with the source.
- **File 141 (2049 track 3):** the leaf section is 99 492 bytes, which exceeds 16-bit offsets.
  - `merge_leaves()` greedily collapses 1022 bottom nodes into their parents to reach 65 533 bytes.
  - Every source leaf maps to an output leaf containing a superset of its polygons (checked).
  - Merged leaves are capped at 36 polygons (65 523 bytes; max leaf 26 → 36). The four queries copy a leaf into an
    unchecked u16[36] stack buffer: the next local sits 72 bytes in for `func_8006CF00` (sp+0x94 → 0xDC),
    `func_8006F704` (0xA4 → 0xEC) and `func_8008BDC0` (0x108 → 0x150); `func_8006FC94` has 40 (0xF4 → 0x144).
    The old 255 cap gave a 59-polygon leaf that overwrote `func_8006FC94`'s saved point pointer (sp+0x15C), crashing
    in `func_8006C670`. Rush 2's own tracks reach 34.
  - Point queries test every listed polygon exactly, so results are unchanged; only CPU work changes.
  - **Alternative:** a 4-line patch for the 0x10<<q bit at the leaf fetch in `func_8006CF00`, `func_8006F704`,
    `func_8006FC94` and `func_8008BDC0`. It would allow an unmodified tree.

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
4. **Lighting colour index and covered fade.** Visual only, no driving effect.

`collision.extras(data_2049)` returns the movers, boost/platform polygons, covered polygons and colour indices for
a future runtime patch.

## 8. Cross-references a converter must keep consistent

- **Mover ids and platform parameter** (info>>11 with 0x20) = the geometry file's animated-object (`PTHD`) ids. Keep
  them in sync if the geometry conversion renumbers objects.
- **No other cross-file references:**
  - SEG indices are internal (VLIST trailers).
  - There are no names, model handles or AI-path indices.
  - Coordinates are world units in the same space as geometry (geometry vertices are ×16).
