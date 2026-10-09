# Rush 2 records, and separate records for the Rush 2049 tracks

Scope: every place Rush 2 reads, writes, displays, defaults and saves track records; the data layouts; and the design
and hooks that give the six Rush 2049 tracks (forward and backward) their own records while they are raced in the
host slot (HAWAII, track id 2; see `src/rush2049/track2049.cpp`). Implementation: `src/rush2049/track2049_records.cpp`; the us.toml and
`rush2_hooks.h` entries are in `records_us_toml.txt`.

**Tags:** **[V]** = verified in the disassembly or by running code; **[I]** = inferred.

**Abbreviations:**
- `track` = byte 0x8010C3F0, `bw` = backward byte 0x80119848, `mode` = word 0x8010C3E8, `players` = s16 0x8010C3E2
- `slot` = record slot `track + 12·bw`, minus 1 when that is ≥ 11 (so 0–22; STUNT1 forward and ATARI forward both
  land on 10, but STUNT1 has no lap records)
- `rec(p)` = player record of profile p (0–19) in the Controller Pak image: `0x8004B220 + (p / 5)·0x2200 + (p % 5)·0x6C0`.
  The code often computes it as `base = 0x8004B1E0 + …` and adds 0x40 to each field offset.
- Packed time (u16): `min << 13 | sec << 7 | hundredths` (`func_8006002C` unpacks, `func_80094A40` packs and clamps
  to 7:59.99, `func_8006005C` returns seconds as f32). 0 = empty.

Tools: `tools/rush2049/decomp.py r2 ADDR` (m2c) and `annot2.py` (r2.asm with resolved addresses, `out/r2a.asm`).

---------------------------------------------------------------------------------------------------------------------

## 1. Where records live [V]

| Data | Address | Layout | Saved? |
|---|---|---|---|
| Controller Pak image | 0x8004B1E0–0x800539E0 | 4 paks × 0x2200: 0x40-byte header, then 5 player records × 0x6C0 | yes, `.mpk` |
| Player record | `rec(p)` | +0x01 flags (bit 0 = in use), +0x02 name (13 bytes, NUL-terminated), +0x18 u16 key masks [12], +0x30 last-track nibble, **+0x38 best-time lists**, **+0x1F0 stats slots**, +0x4F4… other fields | yes |
| Best-time lists | `rec(p) + 0x38 + page·0xDC + slot·0xA` | 5 packed u16, best first; 2 pages × 22 slots | yes |
| Stats slots | `rec(p) + 0x1F0 + slot·0x20` | 0x20 bytes (§1.1); 23 slots, to +0x4D0 | yes |
| No-profile stats | 0x800C1DD4 + (track + 12·bw)·0x20 | same 0x20 layout, **24 entries, index not adjusted**; .data, all zero at boot | no (RAM) |
| High-score table | 0x800D2160 + page·0x18C + slot·0x12 | 2 pages × 22 rows of 0x12 bytes (§1.2) | no (bss) |
| Guest names | 0x800D2630 + i·13, i = 0–19 | NUL-terminated; entry 0 is never handed out and stays blank | no (bss) |
| Guest name ages | 0x800D3AF8 + i·2 | u16, bumped per name chosen, 0 = just used (LRU) | no |
| Seed times | 0x8001CF3C + (track + 12·bw)·4 | f32 seconds (boot segment); e.g. HAWAII 142 / 143 | ROM |
| Dirty bitmap | 0x800D5670 | 1 bit per 32-byte block of the pak image (0x88 bytes = 4 × 0x110 blocks) | — |
| Player struct | 0x800C2140 + i·0x28 | +0x00 car, +0x01 controller id, **+0x24 = `rec(p)`** of its profile | — |
| Profile of a player | 0x801174F0 + controller·2 | s16 profile index, −1 = none | — |
| Guest of a player | 0x800C226C + controller·2 | s16 who byte of the guest name chosen | — |

**Pages:** page 0 = the race's average lap (sum of lap times / laps, `func_800A7AF0` → 0x800D3628 + player·0x18 + 4),
page 1 = the race's best lap (+0x34). The high-score screen alternates between them (0x800C2268). Both are lap
times, which is why one seed per slot serves both.

### 1.1 Stats slot (0x20 bytes) [V for the writers, I for meanings]
| Offset | Type | Written by | Meaning |
|---|---|---|---|
| +0x01 | u8 | `func_800A7804` (profile path only) | laps of the race that became the best average (rank 0 in page 0) |
| +0x02 | u8 | `func_80065360` | maximum of its argument (stunt scoring) |
| +0x04 | u16 packed | `func_800A7804` | average lap over all races, weighted by laps |
| +0x06 | u16 | `func_800A7804` | total laps counted in +0x04 |
| +0x08 | u16 | `func_800A9250` | races finished |
| +0x0A/0x0C/0x0E | u16 | `func_800A9250` | 1st / 2nd / 3rd places (car +0xEA == 1, place car +0xE9, cars 0x8010C15A ≥ 2 / ≥ 2 / ≥ 3). +0x0A is the unlock test of `func_80094F1C` |
| +0x10 | u16 | `func_800A9250` | sum of the car's u16 +0x1E |
| +0x12/0x14 | u16 | `func_800A9250` | races with 0x8010BC98 set / of those, finished with car +0x343 == 0 |
| +0x16–0x1C | u16[4] | `func_8006544C` | stunt counters by type 0–3 |
| +0x1E | u16 | `func_800A9164` | maximum of 0x8010C074[player] |

### 1.2 High-score row (0x12 bytes) [V]
| Offset | Meaning |
|---|---|
| +0x00–0x04 | who per position: 0xFF = empty; 0–19 = profile index (name read from `rec(p)+2`); 20–39 = guest name `who − 20` |
| +0x05 | unused (0) |
| +0x06–0x0F | 5 packed times, best first |
| +0x10 | lap count of the race, set when a guest's entry tops every other guest entry (0x800A9D7C); cleared at boot |
| +0x11 | unused |

The table is a cache: `func_80094BFC` rebuilds it from every profile's best-time lists, and guest entries only exist
until the console is switched off.

---------------------------------------------------------------------------------------------------------------------

## 2. Every access, by function [V]

Register names at the hooked instructions are what `src/rush2049/track2049_records.cpp` relies on.

### 2.1 Stats writers (all compute the slot themselves)
Each loads `track`, adds 12 if `bw`, then: if the player has no profile (0x801174F0[struct+1] == −1) uses
`0x800C1DD4 + (t + 12b)·0x20`; otherwise `slot` (−1 from 11) and `*(struct + 0x24) + 0x1F0 + slot·0x20`, marks it
dirty with `jal 0x8005F338` (`$a0` = address, `$a1` = 0x20, `$t1` = 0), and then both paths join and use **`$t3`**.

| Function | Called from | Gate | `$t3` computed / join | Writes |
|---|---|---|---|---|
| `func_80065360(player, value)` | `func_80065588` 0x80065DC0 (stunt scoring, in race) | byte 0x800D5751 ≠ 0 (stunt flag, set by `func_800AE670`) | 0x8006541C / **0x80065428** | +0x02 = max |
| `func_8006544C(player, type)` | `func_80065588` ×4 | 0x800D5751 | 0x80065508 / **0x80065514** | +0x16 + type·2 ++ |
| `func_800A9164(player, value)` | `func_800A9250` 0x800A92BC, 0x800A9340 | 0x800D5751 | 0x800A9220 / **0x800A922C** | +0x1E = max |
| `func_800A9250()` (race end, per player) | `func_800A952C` 0x800A9574 | attract byte **0x800FAE6C** == 0 and `mode` ≠ 2, 3 | 0x800A93A8 / **0x800A93B4** (index `$s2` = t + 12b) | +0x08…+0x14; then calls `func_80094F1C` |
| `func_800A7804(player, f32 avg, laps, rank)` | `func_800A952C` 0x800A9ADC | 0x800FAE6C == 0, `mode` ≠ 2, 3 | 0x800A78E0, jal at **0x800A78E4**; no-profile path joins at **0x800A78F8** | +0x01 (profile path, at 0x800A78F4, before the join), +0x04, +0x06 |

(0x800FAE6C: `lui 0x8010; lbu -0x5194`. Older notes give 0x8010AE6C.)

### 2.2 Post-race records: `func_800A952C` (state machine, one call per frame; only `jr $ra` at 0x800AA964)
Called from `func_800AE670` 0x800AF0F4. First frame (0x800C25EC == 0):
1. `func_800A0FB4(0)`, then `func_800A9250()` (stats).
2. Per human player: copies its record (0x6C0) to 0x8010D740 + controller·0x6C0; stores `slot` in **s16 0x800D3938**
   (0x800A98A8–0x800A98D4; also written by `func_800606A8`); `func_800A7AF0` fills the race's average and best lap at
   0x800D3628 + player·0x18 (+0x30 per page).
3. Profile player (0x800A992C–0x800A9A50): `$t4` = `base` + page·0xDC, **`$t3 = $t4 + slot·0xA + 0x78`** (0x800A99C4),
   `$a3 = $t3` (0x800A99C8) → `func_80094B2C(min, sec, hund, list)` = insert position or −1, then `func_80094A98`
   with `$s3 = $t3`, `$s4 = 0` inserts into the profile's list (each shifted entry and the new one marked dirty).
   Loop over both pages (`$t2` up to 0x800D3688).
4. `func_800A7804` (stats, average lap).
5. If the player finished: for both pages `func_80094B2C(…, 0x800D2160 + page·0x18C + slot·0x12 + 6)` (0x800A9BA4
   loop, `$t2` = page base up to 0x800D2478) gives its place in the high-score table (0x800D3628[…] = place).
   A guest gets name entry (0x800E7D20 = 0); a guest above all other guests sets row +0x10 (0x800A9D7C).
Later frames: name entry `func_800A6E60` (0x800AA8D8) → `func_800A663C` (0x800A7444) returns the guest's who byte
(existing equal name, else a free entry 1–19, else an unreferenced one, else the oldest, whose references in the table
become 20), stored in 0x800C226C[controller] and 0x800D2940[player]. Then 0x800AA1AC–0x800AA520 insert into the table
rows: `$s3` = row + 6, `$s4` = row, who = stack arg (0x1C(sp)) = 0x800D2940[player], via `func_80094A98`. The screen
itself is `func_800606A8` (0x800AA754).

### 2.3 High-score screen: `func_800606A8(show)` (only `jr $ra` at 0x80060DA0)
Callers: `func_80060DE4`, `func_800A068C`, `func_800A0FB4` (pause / race end), `func_800A952C`, `func_800AAC64`,
`func_800BAD44`. When the widget list 0x800C2280 is 0 and `show`:
- 0x800C2288 set → page 0x800C2268 = 0, logo widget name 0x800C2304 = 0x800C25AC[track]; else the page toggles.
- Creates widgets 0x800C228C (`func_800604FC`), stores `slot` in 0x800D3938 (0x800607A0–0x800607D8).
- `$s4` = row `0x800D2160 + page·0x18C + slot·0x12` (0x80060814), `$s3 = $s4 + 6`.
- `$f20` = seed 0x8001CF3C[track + 12·bw] (0x80060840, delay slot), plus 1–3 s on page 0; both paths meet at
  **0x800608A4**.
- Per position: who 0xFF → a random default name 0x800C2194[rand·53] and `$f20` += 0–1 s, printed with 0x800CB940;
  who ≥ 20 → guest name; else the profile's name at `base + 0x42`; time from `func_8006002C`, and `$f20` = that time.
  Text goes to 0x800D3F38 (9 bytes per row), name pointers to 0x800D4008.

### 2.4 Menu-side users (outside any race)
| Function | Does |
|---|---|
| `func_80094BFC` (callers `func_800986B0`, `func_80098D14`, `func_800B306C`, `func_800B3110`: pak / profile handling) | Rebuilds the high-score table: drops every profile entry (shifting guests up), then for each pak, each record in use (+0x41 bit 0 of `base`), each page and slot merges the record's best-time list (`base + 0x78 + page·0xDC + slot·0xA`) with `func_80094B2C` / `func_80094A98`, who = pak·5 + record |
| `func_800AA974` (from boot init `func_800B3528`) | Clears the table (who 0xFF, times 0, +0x10 = 0), guest names, ages, and 0x800D3978 |
| `func_80094F1C` (from `func_800987DC`, `func_80098CB8`, `func_800A9250`, `func_800B3528`) | Unlock: 0x800E7D50 = 1 unless some track 0–8 has no 1st place (+0x0A of stats slot, forward or backward) in any record or the no-profile table; also 0x800E7D19 from +0x4F4/+0x4F6/+0x4F8 |
| Records screen, overlay `func_803B0A90` / `func_803C430C` | Index 0x803CB6FC (0–24) → slot (−1 from 11), page 0x803CB8E0. "All" (0x803D026C < 0): table rows; one profile: its best-time list at `base + 0x78 + page·0xDC + slot·0xA` (0x803C4538–0x803C45A0). Seed 0x8001CF3C[index] + 0x803CB950 |
| `func_800B3110(p)` (records screen "clear", 0x803B1080) | Zeroes the profile's best-time lists (0x1B8), stats slots 0–11 (0x180), 0x14 bytes at +0x4B0, keys +0x18; rebuilds the table |
| `func_800B306C(p)` (delete, 0x803B1098) | Clears the in-use bit, rebuilds the table |
| `func_80097FCC(rec, …)` | Creates a record: zeroes 0x6C0, sets the in-use bit |

### 2.5 Saving [V]
`func_8005F338(addr, len)` (`$t1` = 0 set, ≠ 0 clear) marks the 32-byte blocks of `[addr, addr + len)` dirty in
0x800D5670, **only if the range lies inside 0x8004B1E0–0x800539E0** (otherwise it returns). `func_8005F3F8` is the same
with `$t1` = 0. The Controller Pak service `func_800971A8` finds runs of dirty blocks per pak with `func_80096D3C`
(0x110 blocks per pak) and writes them to the pak file. So anything outside the image is never saved.

---------------------------------------------------------------------------------------------------------------------

## 3. Design for the 2049 tracks

### 3.1 Constraints
- No room for 12 more slots in the record, and the `.mpk` layout is fixed.
- 2049 values must never be written to HAWAII's slots of the pak image (it can be saved at any time).
- HAWAII's own records, the records screen and unlocks must be unchanged.
- The high-score table and the no-profile stats are RAM only, so swapping data through them is allowed.

### 3.2 What the implementation does
"Hosted" = `rush2::track2049::race_track()` = k (1–6) **and** `track` == 2. Course = (k − 1) + 6·bw.

1. **Side blocks in recomp memory** (`recomp::alloc`): one 0x270-byte block per pak record position (20), holding for
   the 12 courses a stats slot (0x20) and two best-time lists (0xA each), plus a 12 × 0x20 no-profile block.
2. **Redirect stats and best-time addresses.** At each site of §2.1 / §2.2-3 the computed address (`$t3`) is checked:
   if it is HAWAII's slot (record field 0x1F0 + 2·0x20 or 0x1F0 + 13·0x20; best-time field 0x38 + page·0xDC + 2·0xA or
   + 13·0xA; no-profile entry 2 or 14) and a 2049 track is hosted, it is replaced with the course's entry in the side
   block. The game then reads and writes the side buffer in place; `func_8005F338` ignores it, so nothing reaches
   the `.mpk`. Other slots and unhosted races pass through unchanged.
3. **Binding blocks to profiles.** A block is bound to the name in its record position (`rec(p) + 2`). When the name
   changes (pak swapped, profile renamed or replaced), the block is saved under the old name and loaded for the new
   one. Records are kept per name in the `records` section of `saves/rush2.n64.us.json`:
   ```json
   { "version": 1, "profiles": [ { "name": "BOB", "key": "424f42", "courses": [
       { "track": 3, "backward": false, "race": ["0:28.00", null, null, null, null],
         "lap": [null, null, null, null, null], "stats": "<0x20 bytes hex>" } ] } ] }
   ```
   `key` is the name's bytes (the game's character set isn't always ASCII); `race` = page 0 (average lap), `lap` =
   page 1 (best lap). The file is written (via a .tmp and rename) when the post-race or high-score screen function
   returns and a block changed. The no-profile block is session-only, like Rush 2's.
4. **High-score rows, swapped per call.** At entry of `func_800A952C` and `func_800606A8` (nesting counted), when
   hosted, HAWAII's four rows (pages 0/1, slots 2/13) are stashed and the 2049 track's rows written in their place;
   at their `jr $ra` the 2049 rows are stored back into session state and HAWAII's restored. The 2049 rows are built
   like `func_80094BFC` builds the game's: the session's guest entries with profile entries dropped, plus every
   in-use profile's side best-time lists merged. Guest entries are stored with their names as text and re-resolved
   on write (the same entry if it still holds the name, else an equal name, else a free entry 1–19, else entry 0,
   which is what the game itself does when it reuses a name).
5. **Seed:** at 0x800608A4 `$f20` += seed49 − seed(HAWAII), seed49 = 2049 boot 0x8002E870[(k − 1) + 19·bw] read from
   the user's ROM (`0x1000 + 0x2E470` in the z64), fallback {47, 81, 65, 97, 78, 117} / {48, 79, 76, 104, 86, 119}.
6. **Clear / delete:** at the start of `func_800B3110` and `func_800B306C` (`$a0` = profile index) the profile's
   2049 records are removed from the file and from any block bound to that name.

Why not other approaches:
- **Swapping HAWAII's slots in the pak image** during a 2049 race would put 2049 values in saved memory.
- **A virtual slot number** (0x800D3938 ≥ 23) only covers the high-score table (rows at `0x800D2160 + slot·0x12` for
  s16 slots reach 0x80042172–0x8016214E, all game memory), and the stats/best-time code recomputes the slot from
  `track` anyway.
- **Redirecting every high-score row address** needs about ten hooks across `func_800A952C` with different registers;
  the per-call swap needs four and also keeps name entry and its reference counting consistent.

### 3.3 Hooks (exact entries in `records_us_toml.txt`)
| Function | Before | Hook | Registers |
|---|---|---|---|
| `func_80065360` | 0x80065428 | `rush2_track49_records_stats` | `$t3` (r11) = stats slot |
| `func_8006544C` | 0x80065514 | `rush2_track49_records_stats` | `$t3` |
| `func_800A9164` | 0x800A922C | `rush2_track49_records_stats` | `$t3` |
| `func_800A9250` | 0x800A93B4 | `rush2_track49_records_stats` | `$t3` |
| `func_800A7804` | 0x800A78E4 (the dirty-marking `jal`), 0x800A78F8 (no-profile join) | `rush2_track49_records_stats` | `$t3` (idempotent on the profile path) |
| `func_800A952C` | 0x800A99C8 | `rush2_track49_records_times` | `$t3` = best-time list |
| `func_800A952C` | 0x800A952C / 0x800AA964 | `rush2_track49_records_enter` / `_exit` | — |
| `func_800606A8` | 0x800606A8 / 0x80060DA0 | `rush2_track49_records_enter` / `_exit` | — |
| `func_800606A8` | 0x800608A4 | `rush2_track49_records_seed` | `$f20` (`ctx->f20.fl`) |
| `func_800B3110`, `func_800B306C` | entry | `rush2_track49_records_clear` | `$a0` = profile index |

None is a delay slot; 0x80065428, 0x80065514, 0x800A922C, 0x800A93B4, 0x800A78F8 and 0x800608A4 are branch targets
reached by both paths. In the four writers that join after `jal 0x8005F338`, HAWAII's real slot is still marked dirty
on the profile path (its unchanged bytes get rewritten to the pak); harmless.

### 3.4 Known limits
- Two profiles with the same name (on different paks) share 2049 records; deleting one clears them for both.
- With more than 19 distinct guest names in one session, a 2049 or HAWAII guest entry that was swapped out while the
  game reused its name entry shows a blank name (the game blanks such entries the same way).
- The records screen in the menus doesn't list 2049 tracks; 2049 first places don't count toward unlocks
  (`func_80094F1C` reads the pak image directly). Both match "HAWAII's records unaffected".
- If the 2049 track fails to convert and HAWAII is raced instead while `race_track()` is set, that race's records
  still go to the 2049 course (`track2049.cpp` has no public "applied" flag).
- Stunt-mode writes during a hosted race are saved to the file the next time the high-score screen or post-race
  function returns; quitting the program mid-race loses them.

### 3.5 Test plan
1. Race a 2049 track forward with a profile; finish. The post-race table shows default names around the 2049 seed
   (e.g. track 1 about 0:47) and your time; the `records` section of `saves/rush2.n64.us.json` appears with the course.
2. Pause during the next race on it: same table. Race HAWAII afterwards: HAWAII's table and times are unchanged
   (also check the records screen for HONOLULU before and after).
3. Race the same 2049 track backward, and a different 2049 track: each has its own table.
4. Restart the program: the profile's 2049 times reappear; guest entries don't (as in Rush 2).
5. Race without a profile (guest name entry) on a 2049 track; then on HAWAII; check names are right on both.
6. Records screen → clear the profile's records: its 2049 entry disappears from the JSON.
7. Check the `.mpk` (saves/<game id>.mpk) changes only as for a normal race.
