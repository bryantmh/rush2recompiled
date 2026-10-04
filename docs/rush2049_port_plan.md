# Porting San Francisco Rush 2049 cars and tracks into Rush 2 Recompiled

Status (October 2026): the Rush 2049 **wings** are done (`ab5cb2d`, `src/wings*.cpp`). The six **race tracks**
are playable as extra track select entries, converted from the user's ROM at runtime:
- Research write-ups for every format and system are in `docs/rush2049_research/` and supersede the details below
  where they differ. Notable corrections: the "+12" AI paths are **backward** paths, and 2049 ships them (files
  177-182); byte 0x80119848 is Rush 2's backward flag (mirror is 0x800D0190); 2049 object names are 16 bytes.
- The tracks are **added**, not replacing anything: the menu offers ids 12-17 and a race runs in a borrowed slot
  (HAWAII) whose files and tables are swapped only while a 2049 track is raced (`src/track2049*.cpp`).
- Done since: moving objects (`src/track2049_movers*.cpp`, movers.md), objects turning in place (windmills,
  TROLLEY2), animated textures (`src/track2049_texanim.cpp`, texanim.md), separate records (records.md), Rush 2049's
  music and object sounds through a host-side MusyX port (`src/audio2049.cpp`, `src/track2049_audio.cpp`,
  audio.md), and track select miniatures built from each track's real geometry (`src/track2049_art.cpp`; the game
  heap is now 0x80400000-0x80B00000 to fit them). Cars remain future work (§8 Phase 2).

This document gathers everything learned while reverse-engineering both games, so the work can resume without
repeating it. Addresses are N64 virtual addresses unless they are marked as ROM offsets. Tags: **[V]** = verified in
disassembly or data, **[I]** = inferred and needs checking.

---------------------------------------------------------------------------------------------------------------------

## 1. Goal and ground rules

- **Goal:** let players race Rush 2049's cars and tracks inside Rush 2 Recompiled.
  - Start by **replacing existing Rush 2 slots**: a 2049 car stands in for one of Rush 2's 22 cars, and a 2049 track
    stands in for one of Rush 2's 12.
  - Only then consider **adding** slots.
- **No 2049 data ships with the project.** Everything is read and converted at runtime from the user's own ROM. That
  ROM is already chosen on the *Rush 2049* settings tab (`src/wings.cpp`) and stored as
  `%LOCALAPPDATA%\Rush2Recompiled\rush2049.z64`.
- **Supported ROM:** only San Francisco Rush 2049 **(USA)**, big-endian SHA-1 `3f99351d7bb61656614bdb2aa1a90cfe55d1922c`,
  12 MB, game code `NRUE`. All 2049 offsets below are for that ROM.
- **Supported Rush 2 ROM:** Rush 2 (USA), `NR2E`, the ROM the project already builds from.
- **Feature flags:** follow the wings pattern. Each feature is an option on the Rush 2049 tab, disabled while no valid
  ROM is present. With the option off, the game must behave exactly like stock Rush 2 (compare how the Controls-screen
  WINGS row disappears when wings are off).

### Feasibility summary

| Piece | Difficulty | Why |
|---|---|---|
| Wings | Done | Small code feature on top of identical physics. |
| One 2049 car in a Rush 2 slot | Moderate | Same model and display-list format family. Rush 2 needs 39 named body parts per car where 2049 has 3. Physics stats need tables. |
| One 2049 track in a Rush 2 slot | Hard | Same geometry, collision and AI-path families, but Rush 2 hardcodes many per-track names and tables. It also needs a mirrored AI path, a bigger heap, and its custom conditional display-list op. 2049's animated objects need code. |
| Adding 13th+ tracks / 23rd+ cars | Large | About 20 per-track tables, about 30 per-car tables and about 10 code sites with index arithmetic. Save format implications. |
| 2049 battle arenas, weapons, coins (stunt arenas: done, hosted by STUNT1, see src/track2049.cpp) | Out of scope | These need 2049 game systems Rush 2 doesn't have. A separate 2049 recomp would be the better route. |

---------------------------------------------------------------------------------------------------------------------

## 2. ROM and code layout

### 2.1 Rush 2 (USA, `NR2E`)
| Segment | ROM | RAM | Notes |
|---|---|---|---|
| boot | 0x1000 raw | 0x80000400 | libultra, libaudio, inflate (`func_800059D4`), LZSS (`func_80003C6C`) |
| main | 0xAFD0C0 raw deflate | 0x800539E0 | text to 0x800BCBB0, bss to 0x80125C90 |
| overlay | 0xB3D20E raw deflate | 0x803AA800 | menus, loaded by `func_800A62C0`, size 0x1C190 |

In the recomp ROM (`rush2.us.recomp.z64`), main sits at fake ROM 0x01000000 and the overlay at 0x01080000, both
uncompressed (`tools/extract.py`).

### 2.2 Rush 2049 (USA, `NRUE`)
| Segment | ROM | RAM | Notes |
|---|---|---|---|
| boot | 0x1000 raw | 0x80000400 | its own synth and sound code, inflate `func_80006814` |
| main | 0xB0CB10 raw deflate (0x4FA24 → 0x9DFA0) | **0x80086A50** | inflated at boot (call at 0x80002334) |
| overlays | 0xB5C534, 0xB6FEC4 raw deflate | 0x8038A400 | |

Graphics microcode is F3DEX2 in both: Rush 2 has `F3DEX.NoN fifo 2.07`, 2049 has `fifo 2.08`. **Display lists are
directly compatible.**

---------------------------------------------------------------------------------------------------------------------

## 3. Compression formats

Both games use raw deflate (zlib `wbits = -15`) and a 4 KB-window LZSS. **The two LZ variants differ in one detail.**

```python
def lz(data, offset, relative):
    """relative=False: Rush 2 (func_80003C6C). relative=True: Rush 2049."""
    out, i, pos = bytearray(), offset, 1          # pos = ring position, Rush 2 only
    while True:
        flags = data[i]; i += 1
        for bit in range(8):                      # LSB first, 1 = literal
            if flags & (1 << bit):
                out.append(data[i]); i += 1; pos = (pos + 1) & 0xFFF
                continue
            b1, b2 = data[i], data[i + 1]; i += 2
            off = (((b1 & 0xF0) << 4) | b2) & 0xFFF
            length = (b1 & 0xF) + 2
            if off == 0 and length == 2:
                return bytes(out)                 # end marker
            if relative:
                dist = off                        # 2049: distance back from the output position
            else:
                dist = pos - off                  # Rush 2: absolute ring-buffer position
                if dist <= 0: dist += 0x1000
            for _ in range(length):
                out.append(out[-dist] if dist <= len(out) else 0)   # before start = zero
            pos = (pos + length) & 0xFFF
```
The C++ version of the 2049 variant is `rush2::wings::lz_decompress` (`src/wings_rom.cpp`).

---------------------------------------------------------------------------------------------------------------------

## 4. Asset directories

### 4.1 Rush 2 asset table **[V]**
- **Offset table:** u32 ROM offsets at **0x800C185C**, 0x71 entries.
- **Size table:** decompressed sizes per index at **0x8001CD64** (boot segment).

| Index | Contents |
|---|---|
| 0x00–0x1C | misc (fonts, UI, sky, effects, etc.) |
| 0x1D–0x32 | the **22 cars** (LZ), in this order: PICKUP, INTEG, VETTE, SLED, BMW, CAMARO, SUPRA, BUGAT, VWBUS, VIPER, VWBUG, CONCPT, CIVIC, CADDY, MUST, SUV, TAXI, HOTROD, FORM1, GT90, ROCKET, DEW (car type = index − 0x1D) |
| 0x33 + t | track t geometry (deflate) |
| 0x3F + t | track t object placement |
| 0x4B + t | track t collision |
| 0x57 + t (+12 if mirrored) | track t AI path; the mirrored-path variant is chosen by byte 0x80119848 |
| 0x6F, 0x70 | extras |

- **Decompressor choice:** hardcoded in `func_80077FE0` (loader thread), `func_8008687C` and `func_800A4B04`.
  - LZ: indices 0x12, 0x13, 0x15–0x18 and 0x1D–0x32 (`func_80077F84` → `func_80003C6C`).
  - Deflate: everything else (`func_80077F20` → `func_800059D4`).

### 4.2 Rush 2049 file table **[V]**
- **Location:** a flat sorted list of u32 ROM offsets at **0x8011B5BC** in main (the list starting with 0x2F4E0),
  183 files.
- **File sizes:** a file runs to the next offset; the main code at 0xB0CB10 follows the last file.
- **Index:** this table is the complete index of files used here.

| Files | Contents |
|---|---|
| 0–5 | text banks per language (EN/ES/IT/DE/NL/FR) |
| 6–8 | sound banks (LZ): sound defs, voice scripts, sample headers |
| 9 | raw sample data (ROM 0x39B40, 0x2D2A30 bytes) |
| 10–21 | 12 LZ files with `0x00000018 …` headers, unidentified (car paint/skin data? see §9) |
| 22–37 | fonts (chunked models) |
| 38–53 | small tables (unidentified) |
| 54–60 | UI models: buttons, car-setup parts (ENGINE/FRAME/SHOCK/TIRE/TRANS), trophies, track-select thumbnails |
| 61–79 | effects, coins, stunt icons, weapons, **77 = wings**, flags |
| 80, 81 | wheels (`WHEELWHEEL1_LOD`, rims RIM01–RIM21, RIMB1–3) |
| 82–87 | per-track animated object models (gondolas, trolleys, trains, windmills, sharks…) for tracks 1–6 |
| **88–100** | **13 cars**: `CARnFRAME1`, `CARnHOOD`, `CARnSHEEN` |
| **101–106** | **6 race track geometries** (chunked, with PATH/PTHD) |
| 107–114 | 8 battle arenas (DM1–DM8) |
| 115–118 | 4 stunt arenas |
| 119 | obstacle course |
| **120–125** | **object placement**, tracks 1–6 (126–133 battle, 134–137 stunt, 138 obstacle) |
| **139–144** | **collision**, tracks 1–6 (145–152 battle, 153–156 stunt, 157 obstacle) |
| 158–182 | AI path files, 25 in all. The mapping to tracks still needs work (§9) |

Per-file ROM ranges come from the table. For example, track 1 geometry is ROM 0x43FE80–0x4D2FA0, deflate, and
decompresses to 0x123920 bytes.

---------------------------------------------------------------------------------------------------------------------

## 5. File formats (both games)

### 5.1 Model containers
Both games ship models as **prebuilt F3DEX2 display lists, vertices and textures in one file**, with a table of named
objects. The packaging differs.

**Rush 2 [V]**
- **Header:** 10 u32 words.
  - [0] texture-section offset / size
  - [1] named-object table offset
  - [2], [3] further table offsets
  - [4] named-object count (0x25 for cars)
  - [5] texture count
  - [6] count of a 0x18-byte table
  - [7]–[8] display-list range
  - [9] 0
- **Reader:** `func_80077CC0` → `func_80077C38`, which records 4 table pointers per model slot at 0x80118D78,
  0x80119018, 0x80119220 and 0x80119428 (64 slots).
  - Group records: 0x34 bytes.
  - Named-object records: 0x18 bytes, `name[16]`, float radius, u32.
  - Texture records: 0x20 bytes, e.g. `name[16]`, `u16 w,h`, `0102FFFF`, texel offset.
- **Model record:** `*(0x80118D78 + (h>>10)*8) + (h&0x3FF)*0x34`.
  - +0 = LOD count.
  - LOD i at +4 + i·0xC: +6 flags, +8 distance, +0xC display list.
- **Name lookup:** `func_8005BE3C` → `func_8005BCB4`, a **binary search** (strncmp, 15 chars), returning
  handle = (slot<<10) | index, or 0xFFFF.
  - **Named objects must be sorted by name.**

**Rush 2049 [V]**
- **Header:** word 0 = offset of a directory of 12-byte `{tag, offset, size|count}` entries:
  - `IMAG` texels/palettes
  - `TXLD` texture-load display lists
  - `OBHD` object headers
  - `PLHD`
  - `TXHD` texture headers
  - `OBJS` vertices + display lists
  - tracks also have `PATH` / `PTHD`
- **OBHD record (0x58 bytes):**
  - `name[12]`, u32 flags, f32 radius, u16, s16 LOD count
  - LOD[4] × `{u32 lodflags, f32 dist, u32 dl, u32 vtx}`
  - 2049 keeps LODs inside one record; Rush 2 uses separate named objects (`_LOD1`, `_LOD2`).
- **TXHD record (0x24 bytes):** almost identical to Rush 2's 0x20-byte texture record, plus 4 bytes.
- **Loader:** `func_80096CBC`. The relocator `func_80096734` and the TXLD rebaser `func_80096A00` are covered
  in §5.2.
- **LOD flag 0x10** = draw with lighting (otherwise unlit vertex colours).

### 5.2 Display-list addressing **[V]**
- **Pointers:** in both games, G_VTX, G_DL, G_SETTIMG (and MTX/MOVEMEM) pointers are **file-relative** with segment
  nibble 0. The loader rebases them in place: `w1 = ((w1 + base) & 0xFFFFFF) | (w1 & 0x0F000000)`.
- **2049 TXLD G_SETTIMG:** relative to the **IMAG chunk**, not the file.
- **2049 relocator:** `func_80096734` does not follow G_DL. The TXLD lists are rebased separately.
- **Rush 2 relocation:**
  - `func_80077B38` rebases G_SETTIMG and RDPHALF_1+BRANCH_Z.
  - The recursive walker `func_8007786C` rebases VTX/MTX/MOVEMEM/DL/0xDD/0xE1.
  - **It also rewrites a custom conditional-DL opcode into G_DL branches based on player count and mirror mode.**
    Converted geometry must either avoid that opcode or use it deliberately (e.g. for mirror-only objects).
- **Wings code as a working reference:** `src/wings_render.cpp` loads a 2049 model into spare RDRAM, rebases it the
  2049 way and draws it with Rush 2's matrices. RDRAM must stay below 16 MB (24-bit addresses). The wings use
  0x80D00000–0x80E10000; interpolation uses 0x80B00000–0x80C00000 (the game heap ends at 0x80B00000).

### 5.3 Units and axes **[V]**
- Both games: vertex unit = 1/16 world unit, x = lateral, y = up, z = forward.
- Node matrices store rotation ×65536 and translation ×2^20 (world × 16 in 16.16).
- Car extents are similar: 2049 cars are about ±2.7–4.0 wide and 2.7–5.0 tall; Rush 2 cars about ±1.9–3.3 and
  2.7–5.6.

### 5.4 Collision **[V family, I exact layout]**
- **Header:** four u16 counts, then variable-length float records, in both games.
- A size model fitted **only on Rush 2's 12 collision files** predicts 2049's six track collision sizes within 1–5%.
  Same format family; the record layout still has to be confirmed field by field.
  - Rush 2 track 1: counts 0x7B, 0x30E, 0x9B4, 0x1A10 → 0x2BF77 bytes.
  - 2049 track 1: 0x252, 0x585, 0x1119, 0x2498 → 0x55CDA bytes.

### 5.5 Object placement **[V]**
- **Records:** identical in both games: `name[16]`, 3×3 float matrix, float position, then `0x00000040 0001FFFF` and
  more fields.
- **Rush 2:** files start `00000001 00000018` and name the track prefix (e.g. `NYONE`).
- **2049:** the same records behind a 16-byte header, plus a trailing chunk directory (`WHDR`, `WOBJ`, `GTLD`, `GDAT`).
- Named objects reference models in the track geometry (`TRACK1L53574` …) or in shared files (cones, coins).

### 5.6 AI paths **[V family]**
- **Format:** both games use 0x50-byte node records (position, direction, link indices, `FFFF` padding) after a small
  header (`001f0001 00050000 00060000…` in Rush 2; `00130000 00000000 00060000…` in 2049).
- **Mirrored path:** Rush 2 needs one per track (index +12); 2049 doesn't ship one. Generate it by mirroring x and
  swapping left/right links, after checking how Rush 2's own mirrored path files differ from their normal ones.

### 5.7 2049-only track content
- **Animated objects:** a `PATH`/`PTHD` chunk inside each track geometry drives trap doors, trains, gondolas,
  windmills and similar. Rush 2 has no equivalent data; its animated props are hardcoded by name. The first version
  can draw these objects static, or leave them out.
- **Per-track shared models:** files 82–87, and coins (`GOLDCOIN`, `SILVERCOIN`). Coins are a 2049 game mechanic;
  leave them out.

---------------------------------------------------------------------------------------------------------------------

## 6. How Rush 2 loads and uses tracks and cars **[V unless marked]**

### 6.1 Memory
- **Heap:** `func_80083A0C` is a two-ended bump allocator (low pointer 0x8010C438, high 0x8010C454), **no bounds
  checks**.
- **Layout:** `func_800AF3B0` sets base = align64(0x80125C90) + 2 × 0x25800 framebuffers = 0x80170CC0, and
  top = 0x803DA800.
  - With the overlay resident (flag 0x800C1A20), `func_8009FB40` lowers the top to 0x803AA800.
  - 0x803DA800–0x80400000 is a 320×240×16 buffer (depth buffer **[I]**).
- **Room:** about 2.2–2.4 MB of heap, shared with car slots (about 0x6A000), a 0x55730 block from `func_800B3B78`, etc.
  The 4 MB layout is hardcoded (no osMemSize read).
- **Problem:** 2049 track geometry is 1.0–1.6 MB (largest 0x181450); Rush 2's largest is 0x117898. Expect the
  track loader's free-space check to fail.
- **Fix:** move the arena above 4 MB by hooking the constants in `func_800AF3B0` / `func_8009FB40`. Keep everything
  RSP-visible below 16 MB.

### 6.2 Loaders
- **Tracks:** `func_800A4C98` sums the sizes of the 4–5 track files, checks free heap (fails cleanly with 0),
  allocates, then queues loads.
- **Generic file loader:** `func_80086A60`, with hardcoded index ranges:
  - 0x4B–0x56 → pointer 0x800D5754
  - 0x57–0x6E → 0x800D575C
  - 0x6F–0x70 → 0x803D01FC
- **Cars:**
  - `func_800A37F4` picks cars and preallocates up to 8 car slots (table 0x800D9CD8), each `[0x8001CF28] + 0x400`
    bytes (0xD1F0 + 0x400). 2049 cars (0x76A8–0x9C70 decompressed) fit.
  - `func_80087E88` queues index type + 0x1D.
- **Car parts:** `func_80086700` builds `<CARNAME>` + **39 suffixes** (FRAME1, D0_FR1, …, H2D_R; list at 0x800C6180)
  and stores handles at 0x8010C480 + car·0x4E.
  - **Handles are used without checking for 0xFFFF.** `func_80053BE4` decodes a missing one to slot 63 / index 1023
    and writes out of bounds, so **all 39 names must exist** (duplicates or dummies are fine).

### 6.3 Per-track hardcoding
- **Track id:** byte 0x8010C3F0, read by about 45 functions.
- **Index arithmetic:** 19 `slti …, 0xC` checks; the +0x33/+0x3F/+0x4B/+0x57/+0xC offsets appear in `func_800A4C98`,
  `func_800AB824` and `func_80086A60`.
- **Literal track tests:**
  - ==5 (LA, in the overlay)
  - ==1 / ==3 force the Taxi in `func_800A37F4`
  - ==0 / ==0xB in `func_8005D890`
- **Tables sized for 12:**
  - prefixes 0x800C182C
  - names 0x800C25AC
  - record-screen names 0x800C4C40 (also the Controller Pak record layout)
  - floats 0x800C1D14, 0x800C1D44
  - u16 0x800C1DBC
  - 0x800C17AC, 0x800C3F84, 0x800C3FB4, 0x800C5D2C
  - 12×5 bytes at 0x800C1D7C (fog/sky colours **[I]**)
  - boot segment: 0x80023028, 0x80022FE8
  - 12-way jump table 0x800CF9A4 (`func_8007C27C`)
- **Tables of 24 (normal + mirror):** 0x800C45CC, 0x800C1A3C, 0x800C1CB4.
- **Names the game expects in a track:**
  - `func_8008F080`: `<prefix>FINISH`, `<prefix>FINISHB`, `CHKPNT`, `FINISH` (prefix table 0x800C182C)
  - `func_800A45A8`: `SKYO1`, `STUNTSKYO1`, `SKY01`, `SKYFOUR`
  - about 150 more: breakables (`CONE1`, `FENCE*`, `TREEHIT*`…), animated props (`CASINO*`, `NYSIG*`, `FRMT*`…)
  - per-track prop lists via the pointer table 0x800C5D2C (entries for tracks 0, 1, 3, 5, 11)
  - **Missing names have the same unchecked-handle risk as car parts. Audit every lookup site before loading a
    foreign track.**

### 6.4 Per-car hardcoding
- **Tables:** about 30 tables of 22 entries at 0x800C06B4–0x800C0FE0. Among them:
  - names 0x800C0764
  - mass 0x800C08B0
  - torque-like 0x800C090C
  - various u8/u16/8-byte tables
- **Other data:**
  - texture prefixes 0x800C64C4 (`TRK_`, `I8_`…)
  - a per-car 22×2×5 table (`func_800A663C`, `func_800AA974`), probably saved data
- **Code:**
  - car count 0x16 in 6 functions
  - AI picks random 0–15 (the 16 base cars only)
  - jump table on type−3 at 0x800CFA58 (`func_800843EC`)
  - special cases for 0x12/0x14/0x15 in `func_8008582C` (DEW has its own palette)
- **Physics:**
  - car descriptors (`car+0x0`) match 2049's: inertia, drag 0.0135, rolling 75
  - **the physics engines are identical** (see `src/wings_state.cpp` header comment); g = 32.2 at 0x80110018
  - so 2049 car stats should transfer if 2049's per-car descriptor tables are found and copied (§9)

### 6.5 Physics and car structs (from the wings work)
- **Car physics struct:** 0x800F5470 + i·0x81C.
  - index +0x7E0
  - type +0x7EA
  - flags +0x7F4 (0x10 = wrecked)
  - torque +0x10/+0x14/+0x18
  - position +0x224/+0x228/+0x22C
  - weight +0x5AC, mass +0x5B0
  - wheel heights +0x5DC…
  - speed +0x3D4
- **Car state:** 0x801124A0 + i·0x354 (+0x350 → human player struct 0x800C2140 + p·0x28).
- **Body scene node index:** `MEM_W(0x80113F90 + car·0x134)`. Nodes are at 0x800D9E90, 0x38 bytes each.
- **2049 equivalents:**
  - player struct 0x8014A250 + i·0x808
  - car-state 0x80152818 + i·0x3B8
  - model handle table 0x801427C0
  - instance table 0x8012E700
- **Model drawing:** `func_8007AA48`.
  - It emits PRIM/ENV colours from node +0x30/+0x34 (flags 0x2000/0x4000) and **caches G_LIGHTING in byte
    0x800E7DE1**.
  - **Any injected geometry must restore lighting and the paint colours afterwards.** `src/wings_render.cpp` shows
    how; this bug turned the car body green during the wings work.

---------------------------------------------------------------------------------------------------------------------

## 7. Approach

### 7.1 Where conversion happens
- **Convert at runtime in C++ from the user's 2049 ROM**, the way the wings do. Nothing is distributed, and players
  only need the existing ROM picker.
- **Prototype converters in Python first** (`tools/rush2049/`), checked against real files. Port each converter to
  C++ once its output is verified. The Python scripts double as documentation.

### 7.2 Getting converted files into the game
The loaders take an asset index, look up a ROM offset and size, and decompress. Intercept at that level so the
rest of the game sees ordinary files:
- Hook the decompress dispatch (`func_80077F20` deflate / `func_80077F84` LZ, both reached from `func_80077FE0`,
  `func_8008687C`, `func_800A4B04`). When the index belongs to a replaced slot, copy the converted file from a
  host-side buffer into the destination instead.
- Override the size table entry (0x8001CD64 + index·4) while the replacement is active, so heap reservations match.
- Prefer this over writing converted data into the recomp ROM: it doesn't need a rebuild and can be switched at
  runtime.

### 7.3 Converting a 2049 model to a Rush 2 model file
- **Output layout:** Rush 2's 10-word header, then the texture section, display lists, group / named-object /
  texture tables.
- **Display lists:** copy as is, rewriting pointers to Rush 2's base convention.
  - TXLD SETTIMG: IMAG-relative → file-relative.
  - Strip or translate any 2049 `E0 01` conditional ops.
- **LODs:** 2049 OBHD LODs become separate Rush 2 named objects (`NAME`, `NAME_LOD1`…) or group LOD entries,
  whichever matches how Rush 2 tracks reference LODs. Confirm by reading an existing Rush 2 track's tables.
- **Ordering:** sort named objects for the binary search.
- **Lighting:** carry over 2049's lit/unlit choice (lodflags 0x10) to Rush 2's model flag 0x800, which
  `func_8007AA48` uses to switch lighting.

---------------------------------------------------------------------------------------------------------------------

## 8. Work plan

Each phase ends with something testable. Use the in-game test recipe in §10.

### Phase 0: research tooling (½–1 day)
- Create `tools/rush2049/` with:
  - `lz.py`, `files.py`: dump the 2049 file table and decompress any file.
  - `model.py`: parse both container formats, list objects and textures, walk display lists.
  - `compare.py`: side-by-side dumps of the matching Rush 2 and 2049 file for each kind.
- Confirm the unknowns in §9 that block Phase 1.
- **Done when:** every file kind in §4.2 that this plan uses can be decoded and listed by name.

### Phase 1: loader interception and memory (1–2 days)
- Add a "replacement registry": asset index → host buffer + size, behind a Rush 2049 tab option.
- Hook the decompress dispatch and the size table (§7.2). Test by re-serving a **Rush 2 file decompressed on the host**
  (same bytes); the game must be unchanged.
- Move the heap arena above 4 MB (§6.1). Verify menus, races and split screen still work, and that a 1.6 MB dummy
  allocation in the track loader succeeds.
- **Done when:** a re-served stock file loads, and the heap has room for the largest 2049 track.

### Phase 2: one 2049 car in a Rush 2 slot (3–5 days)
1. **Model conversion:** `CARnFRAME1` becomes the body.
   - Generate all **39 part names** from §6.2: point every damage-state part (`D0_*`, `D1_*`, `D2_*`, `H*`) at the
     intact body or at empty display lists.
   - Damage deformation won't show. That's acceptable.
   - Check how Rush 2 draws parts (does it draw FRAME1 plus per-panel parts, or swap panels?). Choose the mapping so
     the body appears exactly once.
2. **HOOD / SHEEN:** SHEEN is probably the environment-mapped gloss pass. Find how 2049 draws it (texture-gen
   flags), and either map it to Rush 2's chrome handling or drop it at first.
3. **Wheels:**
   - Rush 2 uses `WHEELWHEEL` models.
   - Either keep Rush 2's wheels, or convert 2049's file 80/81 rims.
   - Wheel positions come from the physics descriptor; 2049 wheels sit at x = ±2.75, z = ±3.925 per the wing notes
     **[I]**.
4. **Paint:** Rush 2 colours cars through PRIM/ENV and palettes (DEW has its own). Find how 2049 picks paint (files
   10–21 or 88–100 palettes?) and pick one colour scheme first.
5. **Stats:** locate 2049's per-car physics descriptors and stat tables (§9). Fill the Rush 2 per-car table rows for
   the replaced slot (§6.4).
6. **Name and select screen:** replace the name string. Car-select 3D preview and stats bars come with the tables.
7. **Sound:** keep the replaced Rush 2 car's engine sounds. 2049 engine sounds would need its synth (§9).
- **Done when:**
  - The 2049 car can be picked in place of the chosen Rush 2 car, renders correctly in races, menus and split screen,
    with interpolation and widescreen.
  - It drives with 2049's stats.
  - Every other car still works, and turning the option off restores the original car.

### Phase 3: one 2049 track in a Rush 2 slot (2–4 weeks)
1. **Geometry:** convert track file 101–106 (§7.3), including sky objects. Rename the sky object to the name
   `func_800A45A8` expects.
2. **Placement:** convert file 120–125 to Rush 2's layout. Drop placements for objects Rush 2 can't use (coins,
   animated 2049 props), or keep animated props as static models.
3. **Collision:** confirm the record layout (§5.4) field by field against a Rush 2 file, then convert file 139–144.
   Check surface types (road/grass/water); the codes may differ.
4. **AI paths:**
   - Map the 2049 path files (158–182) to tracks.
   - Convert the normal path, and generate the mirrored one (§5.6).
   - Check that drones lap the track.
5. **Required names:**
   - Provide `<prefix>FINISH`, `<prefix>FINISHB`, `CHKPNT`/`FINISH` objects.
   - Audit the ~150 hardcoded name lookups for the replaced slot and supply dummies where a lookup can't fail
     safely.
   - Remove the slot's per-track prop list entries (0x800C5D2C) if they reference Rush 2 objects.
6. **Per-track tables (for the replaced slot):**
   - lap count, start grid, fog/sky colours, camera/flyover data
   - track name, music
   - minimap (Rush 2 draws a track map; generate it from the AI path or the 2049 minimap data)
   - record-screen name
7. **Checkpoints and timing:** find how Rush 2 defines checkpoints (objects vs path indices) and map 2049's.
8. **Mirror mode:** test it, since Rush 2's relocator handles mirror-only geometry through the conditional op.
- **Done when:** a full race on the 2049 track works in normal and mirror mode, 1 and 2 players: start, laps, finish,
  drones racing, collision feeling right, no crashes.

### Phase 4: more cars and tracks (each after the first is mostly data)
- Repeat Phase 2 for the other 12 cars and Phase 3 for the other 5 tracks.
- **Picking the slot:** decide how players choose which Rush 2 slot each 2049 car/track replaces. Fixed pairs are
  simplest; a picker on the Rush 2049 tab is nicer.

### Phase 5 (optional): extra slots instead of replacements
- Extend every 12- and 22-sized table listed in §6.3/§6.4 (move them into recomp heap memory and repoint the code),
  plus the index arithmetic, the `slti 0xC` checks and the jump tables.
- **Saves:** Controller Pak records hold per-track data. Extra tracks need side storage (like `wings_controls.json`)
  so the `.mpk` stays compatible.

### Phase 6 (optional): 2049 animated track objects
- Reimplement the PATH/PTHD followers (trains, gondolas, trap doors) as C++ that moves placed objects each frame,
  using 2049's code as reference.

---------------------------------------------------------------------------------------------------------------------

## 9. Open questions to answer first

1. **2049 car stats:**
   - Where are 2049's per-car physics descriptors (`player+0x0` points at one) and display stats?
   - Compare the layout with Rush 2's descriptors; the wing-physics investigation found them identical in the fields
     it checked.
2. **Files 10–21 (12 LZ files, `0x00000018 0x0018…` headers) and 38–53:** what are they (car paint, damage maps,
   track minimaps)?
3. **AI path files 158–182:** which belong to which track (and to battle/stunt)? Do any 2049 tracks ship a mirrored
   path?
4. **Collision record layout** (§5.4), and whether the surface ids match.
5. **Rush 2 track geometry header:** fields [2], [3], [6] and the 0x34-byte group record. How LODs and the
   conditional opcode are used in practice.
6. **Rush 2 checkpoint/finish definition** and the minimap source.
7. **SHEEN pass:** how 2049 renders it (TEXTURE_GEN? second pass?).
8. **Music:** 2049 music uses its own synth; Rush 2 uses libaudio sequences. Plan to keep Rush 2 music on 2049 tracks.

---------------------------------------------------------------------------------------------------------------------

## 10. Testing

- **Build:** `sh tools/rebuild.sh` (regenerates `RecompiledFuncs` from `us.toml`, then builds).
- **Into a race:** run `build/Rush2Recompiled.exe` from the `build` folder with
  `--autostart --input-script "8:START,11:A,14:A,17:A,20:A,24:A:12"`.
  - The flyover is at about 20–22 s, the race at about 24 s. The first race is Las Vegas.
  - `--input-script` entries are `<seconds>:<button>[:<hold seconds>]`, buttons A B Z START L R CU CD CL CR DU DD DL
    DR.
  - Pass the script as a separate argument (PowerShell: `-ArgumentList @('--autostart','--input-script','…')`).
- **Menus from the title:** START at 8 s, then D-down ×4 + A = Setup, D-down + A = Controls.
- **Screenshots:** use `PrintWindow` (flag 3) **with `SetProcessDPIAware()`**. Without it, the capture is cropped to
  the top-left on high-DPI displays.
- **Config gotcha:** `recomp::config` falls back to `<id>.json.bak`. Delete both files to reset a tab.
- **Hook gotchas (learned the hard way):**
  - **Use `MEM_W(0, (int32_t)(base + offset))`.** `MEM_W(offset, (int32_t)base)` with an unsigned offset breaks sign
    extension and reads outside RDRAM.
  - N64Recomp emits a branch label **before** the hook text at the same address. Hooks on branch targets run on
    every path, but a hook placed one instruction early can be skipped by a branch to the next one.
  - Don't hook delay-slot instructions (they are emitted twice). Instruction patches on them are fine.
  - Only one hook per instruction. Extend the existing C function instead (see `rush2_model_draw` calling the wings).
  - The executable's include path finds RT64's nlohmann::json 3.12 before librecomp's 3.9. Any file calling a
    librecomp function with a json parameter (e.g. `Config::load_config`) must include
    `lib/N64ModernRuntime/thirdparty/json/json.hpp` first (see `src/wings.cpp`).

---------------------------------------------------------------------------------------------------------------------

## 11. Reference: Rush 2049 wings (done)

Kept here because cars and tracks build on the same pieces:
- Settings tab, ROM picker, SHA-1 check and ROM storage: `src/wings.cpp`.
- 2049 LZ and file reading: `src/wings_rom.cpp`.
- Physics hooks (torque in `func_800706D0`, gravity in `func_8006A02C`, drag in `func_8006AFD8`, init in
  `func_8008DBA0`): `src/wings_state.cpp`.
- Loading a 2049 model into RDRAM and drawing it on a car: `src/wings_render.cpp`.
- 2049 ADPCM sample decoding and host-side mixing: `src/wings_sound.cpp`.
  - Sound chain: file 6 sound defs (12-byte entries) → file 7 voice scripts → file 8 sample headers (0x1C bytes:
    id, offset into file 9, root key, rate, format, length, loop) → file 9 data.
  - Format 3 = custom ADPCM: 0x100-byte codebook, 40-byte blocks of 2 × 32 samples with their own anchors.
  - The same path can extract any 2049 sound effect.
- Controller Setup row: `src/wings_menu.cpp`.
