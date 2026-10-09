# Rush 2 menus around track choice, and 2049 assets for new entries

Scope: how Rush 2's menus offer and remember tracks, every UI place indexed by the track id, a design for six
extra entries (menu ids 12–17), and which Rush 2049 assets could fill them. Track data formats are covered
elsewhere.

**Tags:** **[V]** = verified in the disassembly or by decoding data; **[I]** = inferred.

**Tools** (run from `tools/rush2049`):
- `menus.py`: `tables`, `widgets`, `refs ADDR..`, `logos OUTDIR`, `diorama`.
- `ui49.py OUTDIR [--raw] [FILE..]`: dumps 2049 UI textures to PNG (defaults to files 60 and 59).

**Abbreviations:**
- `track` = byte 0x8010C3F0
- `mode` = word 0x8010C3E8 (1 = circuit, 2 = a mode offering only TRACK…WIND, practice **[I]**)
- `players` = s16 0x8010C3E2

---------------------------------------------------------------------------------------------------------------------

## 1. Track select screen

### 1.1 Entry points **[V]**
- **`func_803ABE0C`:** the per-frame handler.
  - It reads buttons (0x80125A68 / 0x8011962A) and calls `func_803AB294(1)`, which draws the dioramas.
  - On B it calls `func_803AB294(0)`, which tears the screen down.
  - It is called from `func_800AE670`.
- **`func_803AB294(a0)`, the init + draw function:**
  - With a0 = 0 it frees the screen.
  - Otherwise, the first time through (flag 0x803C9218):
    1. Builds the option list.
    2. Picks the initial track.
    3. Loads assets.
    4. Builds the 3D carousel.
    5. Creates the screen's widgets: `func_800604FC(0, 0, 0x803C921C, 27)`.
  - On every call it animates and draws the carousel.

### 1.2 Widgets **[V]**
A widget record is 0x28 bytes: `char *texture; s16 x, y; 3×-1; 0; 0; 0xFF; callback; arg`. The live widget keeps
`arg` at +0x2C. `python menus.py widgets` dumps every static list.

| Widget | Callback | Role |
|---|---|---|
| TITLELEFT/RIGHT, OPTIONBG* | — | static art |
| 4 × OPTIONTEXTBOX (arg 0–3) | `func_803C5710` | option rows (4 visible rows of an 11-option list) |
| OPTIONSLIDER/OPTSLIDERKNOB | `func_803C5798` | slider value for FOG/WIND/DIFFICULTY/HANDICAP |
| OPTIONARROW | `func_803C59A0` | option cursor arrows |
| BIGARROW/BIGARROWGRAY | `func_803C5CC8` | left/right carousel arrows |
| OPTZ | `func_803C5E30` | Z hint |
| TRACKFAN1 | `func_803C60AC` | wind fan animation (`TRACKFAN1..4`, wind 0x8010D388); not track-indexed |
| **"VEGAS" at (18,178)** | **`func_803C6208`** | **track name logo**: when `track` changes it calls `func_80080D40(widget, logoName[track], 0)` with names from **0x803C9668[12]** |
| (no texture) | `func_803C6268` | draws option names/values text (labels 0x800C48F8 + lang·11, values 0x800C4A90) |

### 1.3 Options
**List built at init** [V] (`func_803AB294`, 0x803AB320–0x803AB3B8):
- It stores option ids 0..10 into `int 0x803D05D8[]`, with the count at 0x803D05CC.
- Option ids: 0 TRACK, 1 BACKWARD, 2 MIRROR, 3 FOG, 4 WIND, 5 LAPS, 6 DRONES, 7 DIFFICULTY, 8 HANDICAP,
  9 CHECKPOINTS, 10 DEATHS (labels at 0x800C48F8).
- With `mode == 2`, only ids 0–4 are offered.
- With `players == 1`, id 8 (HANDICAP) is dropped.

**Handling** [V] (`func_803ABE0C`):
- Left/right (0x200 / 0x100) dispatch through the 11-entry jump table **0x803CAC10**.
- Each case edits the menu settings struct **0x800D5760**:

| Option | Byte | Range |
|---|---|---|
| TRACK | `track` (see 1.4) | |
| BACKWARD | +0xC | toggle |
| MIRROR | +0xD | toggle; forced 2 / limited by 0x800C2144 |
| FOG | +0xE | 1..4, wraps |
| WIND | +0xF | 0..4 |
| LAPS | +0x0 | 1..8 |
| DRONES | +0x1 | &7 |
| DIFFICULTY | +0xA | 0..5 |
| HANDICAP | +0xB | 0..2 |
| CHECKPOINTS | +0x19 | |
| DEATHS | +0x11 | |

**Copying to race globals** [V]: after every change, `func_80094698` copies the struct into the race globals.
- Backward 0x80119848 (= +0xC)
- Mirror 0x800D0190 (= +0xD)
- Fog 0x800D1FA8 (= +0xE)
- Wind 0x8010D388 (= +0xF)
- Laps 0x8010C0E2
- Drones 0x800D3E90
- Difficulty 0x8010C211
- Handicap 0x800D5798
- …

**Overrides inside `func_80094698`:**
- In circuit mode (`mode == 1`) the values come from the circuit entry 0x800D3A60[race·4]:
  - `[1]` bit0 = backward, bit1 = mirror
  - `[2]` = fog
  - `[3]` = wind
- Literal track tests: track 11 (STUNT) forces backward and mirror off; track 10 (ATARI) forces fog 1 and wind 0.

**Greyed options** [V] (0x803ABF28–0x803AC000, and 0x803AC59C–0x803AC638 → flag 0x803D0608):
- In circuit mode with race index > 0, nothing can be changed. In circuit mode at race 0, DEATHS is greyed.
- On ATARI (10), FOG and WIND are greyed.
- On STUNT (11), everything except TRACK, FOG and WIND is greyed.
- On LA (5), the FOG label reads "SMOG" (0x800C4A98) (`func_803C59A0` 0x803C5BC0, `func_803C6268`).

**Option rows on screen** [V]:
- TRACK (option row 0) has no text row: the carousel arrows stand for it. The 4 rows on screen show options
  `top` .. `top + 3`, `top` = 0x803D05C4 (1 when the screen opens, `func_803AB294` 0x803AB314). The cursor row is
  0x803D05BC, the list's count 0x803D05CC.
- Each OPTIONTEXTBOX widget (`func_803C5710`, arg 0-3) reads option `0x803D05D8[top + arg]` with no bound check and
  hides itself (`func_80060DA8`'s second argument, widget +0x1A) under a slider option (FOG, WIND, DIFFICULTY,
  HANDICAP: ids 3, 4, 7, 8). The slider widget (`func_803C5798`) reads the same entry.
- `func_803C6268` draws 4 rows' label and value (y from 0xBC, 0xB apart, until 0xE8), the row index wrapping to 0 at
  the count, so a list shorter than 5 options would draw TRACK's label in a row.
- Moving down (0x803AC4C8-0x803AC51C): the cursor goes up to count - 1; `top` steps on unless `top + 4 == count` or
  the cursor is 1. A list of 4 options or fewer would scroll past its end.
- DRONES' value (case 5, 0x803C66F0) prints the digits 0-7, the race's drone count (s16 0x800D3E90) in style 0xA
  (green) and the others in style 4 (gray). Its left/right case (0x803AC2CC) wraps menu settings +0x1
  (0x800D5761) with `& 7`, or sets 7 with no step.
- In the port (src/track2049_menu.cpp), the STUNT and BATTLE track selects list only TRACK, FOG, WIND and, on the
  obstacle course, DEATHS; hooks stop the text loop after the list (0x803C6934), hide the boxes past it
  (0x803C5774) and keep a list of 4 or fewer from scrolling (0x803AC4FC). In a ghost race (src/ghost.cpp), DRONES
  counts the drones besides the ghosts, from 0 to 7 - players - 3 (hooks at 0x803AC2F0, 0x803AC300, 0x803C6714,
  and 0x803C6738, which dims the digits past it with `func_800735A8`).

**Text colors** [V]: `func_800737E4(style)` reads 8 bytes per style from 0x800BEF6C (fg RGBA, bg RGBA; 0x14 styles,
style 0x14 flashes) and passes them to `func_800735A8(layer, r, g, b, a on the stack)`, layer 0 = fg, 1 = bg. Grays:
style 1 0xE6, 2 0xA0, 3 0x78, 4 0x50; style 0xA is the menus' green (0x00AF00).

### 1.4 Cycling the track
**TRACK case** [V] (0x803AC098–0x803AC12C):
- `carouselIndex` (s16 0x803D05B4) += dir, wrapping to [0, `count`) where `count` = s16 0x803D05AE.
- `track` += dir. Then:
  - if track < 0 → 11, if track ≥ 12 → 0;
  - repeat while `!func_803AB01C(track)`.
- **Note:** register t0 = 0xB is both the wrap target and the STUNT literal elsewhere in the function.

**Availability `func_803AB01C(t)`** [V]: t < 9 → yes. 9 PIPE → byte 0x800E7D50. 10 ATARI → byte 0x800E7D19.
Anything ≥ 11 → yes, **so 12–17 already pass**.

**Unlocks** [V] (`func_80094F1C`):
- PIPE unlocks when tracks 0–8 have a non-default record (+0xA) in every profile scanned.
- ATARI unlocks via profile fields +0x4F4/6/8, or the default table 0x800C20D4.

**Carousel size and contents** [V]:
- Size: `count = 10 + PIPE + ATARI` (0x803AB6BC).
- **Build loop** (0x803AB6D0–0x803AB870, bound `fp = 0xC`) runs over t = 0..11 where available and fills
  0x1C-byte entries at **0x803D0698**:
  - +0 track id
  - +4 position vec3 (x = slot·100, wrapped to ±count·50)
  - +0x14 slot index
  - +0x18 object instance
- **Capacity: 12 entries**, ending at 0x803D07C8. 0x803D07E8+ is used by other code [V], so 18 entries need a new
  home.
- Users of 0x803D0698 [V]: 0x803AB104 (`func_803AB0E4`), 0x803AB6F4, 0x803AB8A4, 0x803ABAC8, 0x803ABCD4.
- **Initial `carouselIndex` search:** 0x803AB640–0x803AB688 (`slti 0xC`).

**Initial track** [V] (0x803AB3C0–0x803AB5C0):
- Circuit mode: `track = 0x800D3A60[0x800D3DF0·4]`.
- Else, if 0x800E7D38 > 0 (ATARI just unlocked) → 10.
- Else, if 0x800E7DE0 > 0 (PIPE just unlocked) → 9.
- Else `track` = **low nibble of byte +0x30 of player 1's save record** (pointer at 0x800C2140+0x24).
- If `func_803AB01C` rejects it → 0.

**Remembering the choice** [V]:
- At the end of every `func_803ABE0C` frame, when `track` differs from s16 0x803D05AC, the game writes
  `rec[+0x30] = (rec[+0x30] & 0xF0) | track` for player 1 (0x803AC658–0x803AC67C). With ≥ 2 players it also
  writes player 2's record (pointer 0x800C218C, 0x803AC69C–0x803AC6C4).
- Then `func_8005F3F8` marks the bytes dirty for saving. The save area is 0x8004B1E0–0x800539E0: 0x2200 per
  Controller Pak, 5 players × 0x6C0.
- **A track id ≥ 16 would corrupt the high nibble.** Ids 12–15 fit but are meaningless to an unmodified game.

---------------------------------------------------------------------------------------------------------------------

## 2. What is drawn per track, and where it comes from

### 2.1 Assets loaded on entry [V]
0x803AB5D0–0x803AB60C loads asset **0x12**, **1**, **3**, then **4..15** (loop `slti 0xC`), all through the generic
loader `func_80086A60(i, 0)`.
- That loader uses `func_8008687C` to load, then `func_80077CC0` to register the container.
- Models and textures are then looked up **globally by name**.

| Asset | Contents |
|---|---|
| 1 | Menu chrome: TITLELEFT/RIGHT, OPTION*, BIGARROW*, RUSH2BKO1..10 background meshes |
| 3 (474008 bytes) | **13 diorama models**: VEGASTRACK, NYLTRACK, HAWAIITRACK, NYUTRACK, ALCATRAZTRACK, LATRACK, SEATTLETRACK, HALFPIPETRACK, CRASHTRACK, PIPETRACK, ATARITRACK, STUNTTRACK, CLOUDSELO1 (cloud), plus textures OPTIONBGTRKMID and TRACKFAN1..4 |
| 4..0x10 | **One logo texture each**: VEGAS, NEWYORKL, HAWAII, NEWYORKU, ALCATRAZ, LA, SEATTLE, HALFPIPE, CRASH, PIPE, ATARI, STUNT1, CIRCUIT. Each is a 4712-byte container with no models |

### 2.2 Diorama ("3D track with the route in red") [V]
- **Mesh:** `func_803AB294` instances `func_8005BE3C(name)` (`func_8008035C`) for each carousel entry, where name
  comes from **0x803C91E0[track]** = "VEGASTRACK"…"STUNTTRACK" (`python menus.py diorama`).
- **Geometry:**
  - **One LOD, untextured (texture handle 0), vertex-coloured.**
  - 750–2400 vertices; bounding radius 33–57.
- **The red route is baked into the mesh:**
  - Each race diorama has 100–270 vertices in one dark/pure red (VEGAS 0xBC0000, ALCATRAZ 0xCA0000, LA/SEATTLE 0xA10000, NYONE/ATARI 0x860000…) [V counted].
  - That this is the route line is **[I]** from the colour.
  - It is not drawn from the AI path [V: the screen loads no path data].
- **Per-track draw constants:**
  - **0x803C9180[12]** f32 diorama scale (0.6–1.2), used at 0x803ABC74 via s2 = 0x803C9180.
  - **0x803C91B0[12]** f32 cloud height (15, 15.5 for CRASH/PIPE), used at 0x803AB8B0 and 0x803ABD00.
- **Cloud and fog:**
  - The cloud object "CLOUDSELO1" (0x803CAB48) is one shared instance (0x803D0684).
  - `func_803AB0E4` tints it with 0x800C1D7C (3 bytes per track) **only for LA** (track == 5, smog), else white.
- **Spin:** the diorama spins via 0x803CB6E8 at a rate that depends on wind.

### 2.3 Name logo [V]
- **Texture:** 128×32 CI8 (fmt 0x48008000, texels at +0x28, RGBA5551 palette at +0x1028), **rows stored bottom-up**
  (`menus.py logos` writes them upright). Each logo has its own style: neon "LAS VEGAS", a green street sign
  "Los Angeles", etc.
- **Selection:** by name through **0x803C9668[12]** in `func_803C6208`.
- **Text:** the screen shows no text name for the track. The option rows show labels/values only
  (`func_803C6268`, which also tests track == 5 / 10 / 11).

### 2.4 Tables indexed by the track id on this screen
| Table | Entries | Use |
|---|---|---|
| 0x803C91E0 | 12 ptr | diorama object names |
| 0x803C9180 | 12 f32 | diorama scale |
| 0x803C91B0 | 12 f32 | cloud height |
| 0x803C9668 | 12 ptr | logo texture names |
| 0x800C1D7C | 3 B/track | cloud tint (LA only) |
| asset 4 + t | 12 | logo containers |
| player record +0x30 nibble | 4 bits | last track |

The 0x803D0698 carousel array is filled per available track.

---------------------------------------------------------------------------------------------------------------------

## 3. Other UI places indexed by the track id

| Place | Function / table | What is indexed | For 6 more tracks |
|---|---|---|---|
| **Post-race / pause high-score screen** (widgets 0x800C228C: SCORETITLE, BACKWARDON, logo at (159,36), 5 rows TIMEBG/NAMEBG/CARBG) | `func_800606A8`, called from `func_800A952C`, `func_800A0FB4`, `func_800A068C`, `func_80060DE4`, `func_800AAC64`, `func_800BAD44` | logo name **0x800C25AC[track]** written into widget 0x800C2304; **record slot** `s = track + 12·backward, s ≥ 11 → s−1` (23 slots); default tables 0x800D2160 (stride 0x4C·…) and boot-segment floats 0x8001CF3C[24] | the logo needs a name and a loaded texture; records need their own slots (see below) [V] |
| In-race logo load | `func_800A5110` 0x800A55B8: `func_80086A60(track + 4)`; also `func_800ABE7C` 0x800AC0D4 | asset 4 + track | load a custom logo container [V] |
| **Record writes** | `func_80065360`, `func_8006544C`, `func_800A9164` (gated by byte **0x800D5751**), `func_800A9250`, `func_800A7804` | player record +0x1F0 + slot·0x20 (23 slots), or the default table **0x800C1DD4** + (track + 12·bw)·0x20 when no profile | 2049 records must not land in host slots. Clearing 0x800D5751 skips the first three [V]; `func_800A7804` needs its own check [I] |
| **Records screen** (overlay, list 0x803C89C0, 40 widgets; title SCORETITLE) | `func_803B0A90`; logo callback `func_803C5468` | index **0x803CB6FC = 0..24**: track + 12·backward, 24 = CIRCUIT; logo = **0x803C900C[idx % 12]** (13 names incl. CIRCUIT); cycling uses `% 12` and skips locked PIPE/ATARI; loads assets 4..0x10 (bound 0xD) | leave 2049 tracks out first; later remap the index space (25 → 37) [V] |
| Key icons | `func_8005F00C` (race), `func_803B1AB0` (car select `func_803B81F0`) | **0x800C1DBC[track]** u16 key mask, and player record +0x18 + track·2 | ids ≥ 12 read past the tables (+0x30 is the nibble byte); guard to "no keys" [V] |
| **Circuit** | generator `func_800A7DCC` 0x800A8344–0x800A8560: entry = {rand % **7**, rand & 3, fog 1–3, wind rand % 3}, 4 bytes at **0x800D3A60** (race count 0x800D3DF0); review screen `func_803B6260` (list 0x803C7AAC) draws the same dioramas (0x803C91E0 / 9180 / 91B0) and logos **0x803C7D6C[7]** (`func_803BE528`, index 0x803CB414) | tracks 0–6 only | not needed for a first version; to include 2049 tracks, change `% 7` and the 7-wrap (0x800A84AC) and extend 0x803C7D6C [V] |
| **Attract / demo** | `func_800ABE7C` saves `track` in 0x80125AA8 and sets 0; `func_800AB824` gives the next demo track: t + 1, wraps at 7, and after 6 goes to 10 if ATARI is unlocked | 0–6, 10 | 2049 tracks never appear unless `func_800AB824` is patched (optional) [V] |
| Race loader | `func_800A5110` → `func_800A4C98`, `func_800A45A8`, `func_8008BC64(track)`, prefix **0x800C182C[track]** via `func_800601C4` | — | handled by the hosting translation (section 4) [V] |
| Car select | `func_803B81F0` → `func_803B1AB0` (keys) | 0x800C1DBC[track] | guard [V] |
| Pause menu, loading screen | no track-indexed name/logo table found among the `track` readers beyond `func_800606A8` | — | [I] |
| Full names "LAS VEGAS"… "STUNT 1" | 0x800C4C40[0..11]. Entries 12+ are Controller Pak strings; the table is shared with them | no direct `base + track·4` load found | [I] unused by the code paths above |

**Save layout implications** [V]:
- Player records are 0x6C0 bytes:
  - +0x18: u16 key masks [12]
  - +0x30: last-track nibble
  - +0x1F0: 23 × 0x20 record slots, through +0x4D0
  - +0x4F4…: other fields
- Six more tracks need 12 × 0x20 more slots and 6 key masks. Those do not fit without moving fields, and the
  Controller Pak note layout is fixed.
- **So keep 2049 records in side storage**, as the plan suggests for `.mpk` compatibility.

---------------------------------------------------------------------------------------------------------------------

## 4. Design: six more entries

### 4.1 Hosting vs true ids
- **True ids 12–17 in the race:**
  - Every race-side site in plan §6.3 breaks:
    - asset arithmetic: 0x33 + 12 = 0x3F is track 0's placement file, and the mirrored-path index 0x57 + t + 12 collides;
    - 19 `slti 0xC` checks;
    - the 12-way jump table 0x800CF9A4;
    - more than 15 twelve- or twenty-four-entry tables.
  - Records (slot = t + 12·bw), key masks (+0x18 + 2t runs into +0x30) and the saved nibble overflow too.
  - Menus need the same patches as with hosting.
- **Hosting (recommended):**
  - Menus show ids 12–17, but **`track` holds 12–17 only while in the menus**.
  - At race load the id is translated to a host slot and a recomp flag `r49_active = k`. Every race-side table
    and literal then sees a valid Rush 2 id.
  - The menu patch count is the same as for true ids. Nothing race-side is needed beyond what the track-data
    work already does for a replaced slot.
- **Host slot:**
  - Pick one race track. HAWAII (2) or SEATTLE (6) are best **[I]**: no Taxi forcing (1, 3), no smog (5), no
    `func_8005D890` (0, 11) cases, and no 0x800C5D2C prop list (0, 1, 3, 5, 11).
  - Records, key bits and the logo then come from the host unless redirected (patches R2–R4).

**Minimal variant for a prototype [I]:** a "2049 page" toggle on the track select (for example the L button):
1. Rewrites in place (overlay data is RAM) 0x803C91E0 / 9180 / 91B0 / 9668 entries 0–5 to the 2049 versions.
2. Makes `func_803AB01C` hide 6–11.
3. Sets `count = 6`.

This needs no array relocation and no loop-bound patches, but it is a hidden UI.

### 4.2 Patch list for the recommended design
Recomp-side state:
- `r49_ok`: 2049 ROM converted.
- `r49_sel`: last 2049 choice, persisted in a side JSON.
- `r49_active`: index of the 2049 track in the current race, or −1.

**Overlay, track select:**

| # | Site | Change |
|---|---|---|
| M1 | `func_803AB294` 0x803AB5F4–0x803AB60C (logo load loop) | After it, register one synthetic container "R49SEL" with 6 diorama models `R49TRK1..6` and 6 textures `R49LOGO1..6`, through the same `func_8008687C` / `func_80077CC0` path, or by hooking `func_80086A60` for index 3 |
| M2 | 0x803AB67C (`slti 0xC`, current-index search) and 0x803AB6D0 (`fp = 0xC`, build loop) | Bound 12 → 18 when `r49_ok` |
| M3 | 0x803AB6BC (`count = flags + 10`) | +6 when `r49_ok`. Count must equal the number of available entries |
| M4 | 0x803D0698 array users: 0x803AB104, 0x803AB6F4, 0x803AB8A4, 0x803ABAC8, 0x803ABCD4 | Rebase to an 18 × 0x1C block owned by the recomp |
| M5 | Diorama tables: 0x803AB7D0 (0x803C91E0), 0x803ABB14 (s2 = 0x803C9180), 0x803AB8B0 and 0x803ABD00 (0x803C91B0) | Point to 18-entry copies. Entries 12–17: "R49TRK1..6", scale about 45 / radius, cloud height 15 |
| M6 | `func_803C6208` (logo callback, table 0x803C9668) | Replace in C, using an 18-entry name table |
| M7 | TRACK case 0x803AC0E8–0x803AC12C | Wrap at 18 (`slti 0xC` at 0x803AC0FC → 0x12; the store `sb t0` at 0x803AC100 must write 17, not t0). Simplest: reimplement the case in a hook |
| M8 | `func_803AB01C` | Return `r49_ok` for 12–17 (it currently returns true for all ≥ 11) |
| M9 | Saved nibble writes 0x803AC668 and 0x803AC6B0 | When `track ≥ 12`, keep the old nibble and set `r49_sel` instead |
| M10 | Init, before the carousel build (around 0x803AB5C0) | If not circuit, no unlock preselect, and `r49_sel` was the last choice → `track = 12 + r49_sel`, 0x803D05AC = same |
| M11 | `func_803B1AB0` and `func_8005F00C` | Return "no keys" for `track ≥ 12` (needed for car select while `track` is 12–17) |

The literal tests (5, 10, 11), `func_80094698`, `func_803C6268` and `func_803C5710/5798/59A0` need no change. They
only compare against 5 / 10 / 11 [V].

**Race transition:**

| # | Site | Change |
|---|---|---|
| R1 | `func_800A5110` entry (called from `func_800A5ADC` 0x800A5F40 / 0x800A5F60 and `func_800A57C8`) | If `track ≥ 12`: `r49_active = track − 12`; `track = HOST`. The track-data hooks swap the host's files when `r49_active ≥ 0` |
| R2 | In-race logo: `func_800A5110` 0x800A55C0 (asset 4 + track) and `func_800606A8` 0x80060748 (`sw` of 0x800C25AC[track] into 0x800C2304) | Load the R49 logo container and store "R49LOGOn" when `r49_active ≥ 0` |
| R3 | Records | Set 0x800D5751 = 0 after `func_800AE670` sets it, when `r49_active ≥ 0`. That skips `func_80065360`, `func_8006544C` and `func_800A9164` [V]. Guard `func_800A7804` too [I]. To show 2049 records instead, redirect the slot pointer in `func_800606A8` (0x800607A0–0x80060840) to a side table |
| R4 | Returning to the menus (overlay reload `func_800A62C0`, or M10) | Clear `r49_active`; the menu restores `12 + r49_sel` |

**Left alone in a first version:** records screen (`func_803B0A90`, index 0–24), circuits (`func_800A7DCC` rand % 7),
circuit review logos (0x803C7D6C), attract (`func_800AB824`).

---------------------------------------------------------------------------------------------------------------------

## 5. Rush 2049 assets for the new entries
Details from `ui49.py` [V unless marked]:
- **No track names or name logos exist in 2049.**
  - The English text bank (file 0) holds 237 strings: u32 count, then u32 offsets, then NUL-terminated latin1.
    None is a track name. Useful entries: 45 "SELECT TRACK", 57 "PLACE IN CIRCUITS TO UNLOCK RACE TRACKS".
  - Main code has only "TRACK 1".."TRACK 6" (0x80122470, 8-byte stride; then BATTLE 1–8, STUNT 1–4, OBSTACLE) and
    "Track1".. (0x801223F4).
  - Marketing names (Marina, Haight, …) do not appear in the ROM. The user has to supply names.
- **Thumbnails:**
  - File 60 holds `TPIC1..6`: **128×128 CI8** (RGBA5551 palette, 256 entries, alpha edge), circular screenshots,
    **stored bottom-up** like Rush 2's logos.
  - Same format for battle (`DPIC1..8`), stunt (`SPIC1..4`) and obstacle (`OPIC1`).
  - The 2049 menu (overlay ROM 0xB5C534) draws them as a 2D sprite at (176,32); the track index is at 0x8014978C.
- **No stored 3D track map:**
  - File 60's meshes `TRK_T1G1..T6G1` and `TRK_SELG1` all share one flat untextured plate (52 verts, a copy of
    `TRK_D3G1`).
  - The 3D route tube the screen shows is built at runtime from the AI path (§7).

**What fits Rush 2's slots:**
- **Logo slot** (texture 128×32 CI8 at (18,178), and (159,36) on the high-score screen):
  - Generate a 128×32 CI8 texture at conversion time: the user-chosen name (default "TRACK 1".."TRACK 6", or
    "RUSH 2049 · 1") in a neon style, with a glow palette. Store rows bottom-up.
  - Option: a 32×32 downscale of TPIC*n* at the left as an icon (CI8 32×32 = 1 KB, fits TMEM).
  - A full 128×128 TPIC doesn't fit this slot.
- **Diorama slot:**
  - **Recommended:** generate a Rush 2 model container (header layout as in `model.py`; one model, one LOD,
    texture handle 0, vertex-coloured triangles) from the converted track. This matches the Rush 2 look.
    - **Route ribbon:** a quad strip about 4–6 units wide along the 2049 AI path / PATH centreline, in red
      0xBC0000, slightly raised.
    - **Base:** a grey ground plate (0x808080) from the XZ bounds, optionally with a few extruded white blocks
      from coarse collision cells for a "city" feel.
    - **Size:** normalise to radius about 45 units (scale-table entry 0.8, cloud height 15). Keep under about
      2500 verts like the originals.
  - **Simpler:** a flat quad textured with TPIC*n*.
    - 128×128 CI8 exceeds one TMEM load (CI8 max 2 KB texels), so it needs 8 tiles of 64×32, or a 64×64 CI4
      re-quantise.
    - Easy, but unlike the other dioramas.
  - **Not usable:** 2049's own TRK_T*G1 plate (identical for all six).

---------------------------------------------------------------------------------------------------------------------

## 6. Open points
- What the high nibble of player record +0x30 holds [I: another setting]. Avoid writing it (M9).
- Whether the 3D instance pool used by `func_8008035C` limits the number of carousel instances (18 + cloud) [I].
- What `func_80080D40` does when a texture name is missing (unchecked handle risk as in plan §6.2). Make sure the
  R49 container is always registered before the logo widget runs.
- `func_800A7804` record write path (called from `func_800A952C` 0x800A9ADC) has no 0x800D5751 gate in its
  reference list. Check it before relying on R3.

---------------------------------------------------------------------------------------------------------------------

## 7. Rush 2049's route tube (its track select model) [V]

Rush 2049's track select shows each race track as a 3D "tube" along its route, in front of the round TPIC
screenshot, and all of them small on a map of San Francisco (arenas: their flat outline from file 60). It isn't stored in the ROM: the track select overlay
(ROM 0xB5C534, raw deflate, loaded at 0x8038A400) builds it at runtime
from the AI path. `src/track2049_art.cpp` (`build_tube`) ports it as the R49TRACK*n* diorama.

### 7.1 Tables in the overlay
| Address | Contents |
|---|---|
| 0x803B7738 | names `TRK_T1G1..T6G1, TRK_D1-8G1, TRK_S1-4G1, TRK_O1G1, TRK_SELG1` (file 60). The race ones are copies of `TRK_D3G1` (a flat plate); only their object records are used, as carriers for the generated lists |
| 0x803B7788 | `TPIC1-6, DPIC1-8, SPIC1-4, OPIC1` |
| 0x803B7CD8 | s32 ring count, **100** |
| 0x803B7CE4 + i·0x48 | per entry (i = track select index, 19 = the big display): +0 node handle, +4/+8 map offset x/y, +0xC map angle, +0x10 map scale, +0x14 display scale (set by the builder), +0x18 matrix, +0x3C position. Race tracks 0–5: offsets (6.19, 54.21) (−18.46, 11.33) (−19.05, 21.76) (12.11, 47.80) (−11.24, −11.66) (−29.03, 61.65), angles −0.182 −0.246 −0.236 −0.190 1.432 −0.304, scales 0.846 0.957 0.924 0.871 1.268 0.662 |
| 0x803B8338 / 0x803B8344 | map origin (−100, 30, 200) / big display position (50, −15, 100) |
| 0x803B92C8.. | −π/2 (map tilt), 0.8, …, −π/6 (display tilt), −π/2 |

### 7.2 Builder `func_8038CD14` (race tracks, 0x8038D058–0x8038DE08)
For each race track t = 0..5 (file `0x9E + t` = 158 + t, the forward AI path of track k = t + 1):
1. Loads the path (`func_80097798`, `func_80096298`, `func_800BADE0`, `func_800BAAA0`): spine count u16 at
   0x801407F0, spine points (s16 x, y, z) at *0x801407F4, bounding box max at 0x801407B4, min at 0x801407D4.
2. Centre `c = −(max + min) / 2` per axis (integer, rounds toward zero); display scale
   `5000 / max(x extent, z extent)` → entry +0x14.
3. Allocates 100·4 vertices and a list. For ring i (0..99) at spine index `count·i/100`:
   - next = `count·(i+1)/100`; for the last ring, the spine point nearest the finish checkpoint and ahead of it
     (`func_800BA61C(header +2)`, checkpoints at path +12 + n·0x50 {f32 pos[3], f32 dir[3]}, XZ only);
     prev = `count·(i−1)/100`, or `2·cur − next` for ring 0.
   - side vector = mean of the two segments' left normals, each 100 long: `((dz, −dx)/|d|·100)` for
     prev→cur and cur→next (not renormalised).
   - 4 vertices: `cur ± side` at `y + c.y + 50` (top) and `cur ∓ side` at `min.y + c.y − 50` (a floor under the whole
     route), i.e. a flat band 200 wide with a curtain down to the floor; ring 99 gets +0.1 on y.
   - stored as s16 `trunc((p + c)·0.01·16)`, flags 0x8000, s = t = 0, colour 0x3F3F3FFF (overwritten per frame).
4. List: `D7000000 FFFFFFFF`, `E7`, `E200001C C8112230`, `FCFFFFFF FFFE7C38`; per segment
   `01008010` (rings i, i+1 to slots 0–7) or, for the last, `01004008` ring 99 + `01004010` ring 0;
   then `06000802 000A0208`, `06020A04 000C040A`, `06040C06 000E060C`, `06060E00 0008000E` (top, side, floor, side);
   `D7000002 FFFFFFFF`, `DF`. No textures, no lighting (LOD flags 0).
5. The list and vertices become LOD 0 of the `TRK_T{t+1}G1` object (copied record, 0x803BAAE8 + t·16), and a node
   is created for it on the map: scale entry +0x10, `rotX(−π/2)` (top-down), `rotZ(−angle)`, at
   map origin + (x, y offsets).

### 7.3 Per frame `func_8038A820`
- Highlight phase `0x803B7CE0` += Δt/5 (one run along the route every 5 s, wraps at 1).
- Vertex colours (bytes +0xC R, +0xD G, +0xE B): the floor vertices (index & 2) are black; top vertices take the
  track number n = t + 1: **bit 0 blue, bit 1 green, bit 2 red** (1 blue, 2 green, 3 teal, 4 red, 5 purple,
  6 yellow).
  - Other tracks (on the map): component = bit ? 127 : 31.
  - The selected track: `a = min(2·((phase·100 − ring) mod 100), 64)`, component = bit ? 255 − a : 255 − 3a, so a
    white head fades into the colour over 32 rings behind it. Ring 99's top is white.
- The selected track's node (entry 19, `0x8038B4F8`–`0x8038B830`) animates over a selection fraction s (+4·Δt):
  position lerps from its map spot to (50, −15, 100), scale from the map scale to the display scale,
  angle about Y from the map angle to the turn `0x803B8350`, tilt about X from −π/2 to −π/6 (mirrored with
  `func_800FD754` in some modes). The matrix is the scale, then `func_8009EB10` (the turn about Y), then
  `func_800B5898` (the tilt).
- **The selected track turns:** `0x8038B3EC` adds Δt·2π/10 (`0x803B92D8` = 2π, wraps there) to `0x803B8350` each
  frame, so the model spins about its own vertical axis once every 10 s.
- The battle and stunt builders (game types 6 and 4, `0x8038CEC0` and `0x8038CD24`) instance the file 60 meshes
  `TRK_D1-8G1` (entries 6–13) and `TRK_S1-4G1` (entries 14–17) as they are: flat (y = 0), every vertex white, list
  `D7000000`, `E200001C C8112230`, shade combiner. Their display scale (entry +0x14) is 1. Game type 5 (the obstacle
  course) builds nothing, so its select shows `OPIC1` alone.
- The screenshot is a 2D sprite: `func_800B3704(name, 0xB0, 0x20, 0)` at 0x8038AF24, 128×128 at (176, 32).

### 7.4 In the port: dioramas
`build_tube` makes the same rings, list and render state, with the selected colours frozen at phase 0 (white head
at the start line), scaled so the longer side is 1120 model units (like the generated miniature, menu scale 0.45).
Deviation: a closing segment longer than 1000 world units is left out (`tube_close_gaps`): track 6's route ends far
from its start, and 2049's tube draws a sliver across the model there. An arena's diorama is its outline
(`build_outline`), scaled the same. These R49TRACKn / R49STUNTn / R49BATTLEn / R49OBSTACLE models are what the
unlock shop and the circuit screen show.

### 7.5 In the port: the track select preview
The track select shows 2049's own preview instead (`build_preview` in `src/track2049_art.cpp`, posed by
`rush2_track49_select_pose` in `src/track2049_menu.cpp`):
- **Model:** R49PTRACKn / R49PSTUNTn / R49PBATTLEn / R49POBSTACLE, one per entry, laid out in 2049's track select
  camera space (x right, y up, z forward, 16 model units per unit): the tube (display scale 5000/extent) or the
  outline at (50, −15, 100) tilted −π/6 (`preview_place`), then the screenshot as eight textured 128-wide strips
  (16 rows loaded, 14 drawn, so filtering has neighbours at the seams; clear palette entries made black so the
  edge doesn't fringe with the key colour) at depth 150, placed so 2049's projection shows it at (176, 32).
  The disc is drawn after the model, translucent with z compare, so the model hides it.
- **2049's projection** isn't in the overlay's data; it was fitted to a capture of the battle 2 select (DPIC2 and
  `TRK_D2G1`, least squares on the projected outline's coverage of the capture's white pixels): focal length 167 px, centre (162, 116)
  (`preview_focal`, `preview_center_*`) [I].
- **Rush 2's track select camera** (view 0, `0x800E79D0`): rows −right, up, forward, position (0, 20, 45), pitched
  24° down; projection (`func_80054A50(0, 60, 0, 320, 360, 160, 65)` at 0x803AB968, view struct `0x802401F0`):
  60° horizontal FOV, so focal 277 px, straight ahead at (160, 65) [V: logged].
- **Pose:** the carousel builds the entries' nodes from the diorama names; the hook at 0x803AB7DC hands it the preview
  name for a 2049 entry. Each frame the hook at 0x803ABCA0 overwrites the node's pose (`0x800D9E94` + node·56 →
  matrix[9], position[3]) with one that maps 2049 camera space onto Rush 2's: Z' = aZ, X' = a·f₁/f₂·X + bZ,
  Y' = a·f₁/f₂·Y + cZ. The shears b and c make Rush 2's projection show every point at 2049's screen point plus a
  constant offset, so the layout is exact at any depth; a puts the preview's middle (Z = 110) at the diorama's depth.
  The offset centres the disc at (160, 108), between the carousel arrows, and the model is raised 10 px from 2049's
  spot so its front edge clears Rush 2's logo. The node sits at the camera (plus the carousel's slide along x), so
  sliding entries still move in and out.
- **Animation:** the hook keeps 2049's highlight phase (+Δt/5) and turn (+Δt·2π/10), advanced on the centre entry, and
  rewrites the model's vertices in RDRAM each frame: positions from their own-space coordinates (read back once with
  `preview_unplace`) turned and placed again, and for race tracks the tube's top colours (`preview_tube_color`).
- **Logos:** every 2049 entry's logo is a banner in the 2049 banner font (`tools/build_banners.py`,
  `tools/banner_font.py`, which has digits for STUNT n / BATTLE n): banners 7–19 of `include/track2049_banners.h`.

## 8. Car select option list [V]

The car select (menu overlay; per frame `func_803B9478`, setup `func_803B81F0`) has two players' panels (car select
slots 0 and 1; with 3 or 4 players a second round for players 3 and 4, src/players4.cpp) and one option list for both:

- `func_803B81F0` builds it at 0x803B8258-0x803B8484: ids into s32 0x803CB3B8[], count s32 0x803CB3B0. Ids (labels
  0x800C4924 + language * 60 + id * 4): 0 CAR, 1 TRANSMISSION, 2 MAIN COLOR, 3 ACCENT COLOR, 4 STRIPE, 5 STRIPE
  COLOR, 6 TIRE RIMS, 7 HORN, 8 ENGINE, 9 TORQUE, 10 SUSPENSION, 11 TIRES, 12 DURABILITY, and 13 TIRE SIZE F and 14
  TIRE SIZE R only while the cheat byte 0x8010C3D4 is set. The array has room for 15 ids; the word after it,
  0x803CB3F4, is not referenced.
- Per slot: the cursor row s32 0x803C6990[slot], the top row on screen s32 0x803CB3A0[slot] (4 rows on screen,
  scrolled as on the track select: 0x803B97AC-0x803B9808).
- Left/right (`func_803B9478` 0x803B983C-0x803B98BC, $a1 = -1 / 1, $s7 = slot) dispatch on the id through the
  15-entry table 0x803CADF4; larger ids change nothing. After any change 0x803BA644-0x803BA7E4 compare ids (1, 8,
  5, ...) for the sounds and model updates, and `func_803B7F7C` redoes the bars.
- `func_803BC048` draws each row's label (0x803BC2F4-0x803BC368, measured and printed from the label table) and value
  (ids 1-11 through the table at 0x803CAE9C; others none), the value text in $s0 at 0x803BC63C.
- `func_803BAFD8` places the option arrows from the cursor row's label width (label reads at 0x803BB2C8 and
  0x803BB338). Other widgets compare ids only: color swatches for 2, 3, 5 (`func_803BAA44`), sliders for 12-14
  (`func_803BAC24`, `func_803BACF8`); `func_803BB9F8` picks row art through a 15-entry table and skips larger ids.

In the port, src/wings_menu.cpp adds WINGS (id 15) above DURABILITY while wings are on: hooks give it its label
(0x803BC310, 0x803BC350, 0x803BB2E4, 0x803BB34C read a pointer to "WINGS" in place of the table's entry), its value
(0x803BC63C) and its steps (0x803B98A4).
