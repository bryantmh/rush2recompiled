# Unlocks plan: SF Rush keys, Rush 2049 coins, progress UI

Goal: bring back SF Rush's keys on the SF Rush tracks (ids 18-24) and Rush 2049's coins on the 2049 tracks (ids
12-17), with the unlocks each game gates behind them. Then show progress for all three games: first in a recomp menu
tab, later in game.

**[V]** verified in the disassembly or ROM data, **[I]** inferred.

**Status.** The keys and coins are in (src/collectibles.cpp, src/progress_tab.cpp):
- SF Rush keys and 2049 coins (race tracks and stunt arenas) are picked up through Rush 2's key code (§2.2).
- They are kept per profile name in `collectibles.json`.
- The recomp menu's Progress tab shows them, along with Rush 2's own keys and Dew cans (read from its save once per
  frame; the game itself only shows one track's keys, on car select).

What they unlock (§2.3-2.4, and the store or ladder idea) is still undecided.

---------------------------------------------------------------------------------------------------------------------

## 1. Research

### 1.1 Rush 2's key system (what we reuse) [V]

- **Placement.** Each Rush 2 track has 12 `KEY` and 4 `DOTHEDEW` objects (behavior 8). Track 2 has a fifth
  `DOTHEDEW`.
- **Save.** Each track has a u16 mask in the player record at +0x18 + track·2. Bits 0-11 are keys and bits 12-15 are
  Dew cans. Without a profile the mask lives in the session table 0x800C1DBC[track].
- **Pickup.**
  - `func_8005F508` is the touch callback. It runs only when 0x800E9C18[player·9] == 8.
  - It calls `func_8005F418(bit, player)`, which sets the bit and marks the pak block dirty through `func_8005F338`.
  - The bit comes from object +0x68. The Dew model id is 0x756.
  - `func_8005F00C(bit)` returns 0 if any racer in the race already has the bit, which hides the key. It's called by
    the key's per-frame update (`func_8005F118`).
  - Nothing is collected when 0x80113C18 == 3.
- **Unlocks.** `func_803B1AB0(player, out, dew)` counts the current track's low 12 bits, or the high 4 bits when
  `dew` is set. Car select (0x803B85C0-0x803B8738) uses the counts for each player:
  - types 0-15: always;
  - 16 TAXI, 17 HOTROD, 18 FORM1, 19 GT90: type 16 + i when i < keys / 3 (3, 6, 9 and 12 keys);
  - 20 ROCKET: record +0x4B4 != 0, or default table 0x800C20D4 + 4 without a profile;
  - 21 DEW: all 4 Dew cans (dew / 4).

  **Unlocks are per track.** A mystery car is offered only on the track where its keys were found.
- **Existing hook.** `rush2_track49_keys` (src/track2049_menu.cpp) already makes `func_803B1AB0` return 0 keys for
  ids 12 and up.

### 1.2 SF Rush keys [V]

- **Placement.** `KEYL1` children in placement assets 0-6. Field +0x4A of the record is the key number, 1-8.
  - Keys per track (table 0x800CAD48): **6, 7, 8, 8, 8, 8, 8** (53 in all).
  - All are dropped today (`r1_dropped` in src/track1_convert.cpp, `R1_DROPPED` in tools/rush1/track1.py).
- **Save.** One u8 mask per track: player record + 0x1C0 + track·0x16 + 7, or 0x800CAC7C + track·0x16 + 7 without a
  profile.
  - Collect: `func_8006C2E8` (Rush 2's `func_8005F418`). Hide-if-taken: `func_8006BFEC`, which takes object +0x76
    minus 1.
- **Unlocks** (car select 0x800ABE70, through `func_8009931C`). Each player gets
  `cars = 8 + min(G, 2·keys / total)`, where G is the global byte 0x80162AD0 (a cheat level [I]).
  - Half of a track's keys adds car 8 (**TAXI**). All of them adds car 9 (**HOTROD**).
  - **Per track**, like Rush 2.
  - Rush 1's TAXI and HOTROD are Rush 2's types **16 and 17**, so this maps directly onto Rush 2's mystery-car slots
    with no new car work.

### 1.3 Rush 2049 coins [V]

- **Placement.** `GOLDCOIN` and `SILVERCOIN` (dynamic type kind 6, init 0x8010DF90, update 0x8010E0FC, model file
  68).
  - **8 gold + 8 silver** on each race track (files 120-125) and each stunt arena (134-137). The obstacle course has
    none.
  - All are dropped today (`R49_DROP` in tools/rush2049/placement.py and its C++ twin).
- **Index.** Each coin gets the next index at spawn: silver 0-7 (counter 0x80151964), gold 8-15 (counter 0x80151610
  plus 8).
- **Save.** One u16 mask per course in the profile. Collect: `func_800F7C20`. Test: `func_800F7A98`.
  - Race track t (0-5): +0xE8 + t·0x60, mirrored at +0x328 + t·0x60.
  - Stunt arena s (14-17): +0x1C8 + s·0x40.
  - Debug flag 0x801174B4 & 8 counts every coin as collected.
- **Unlocks** (`func_800F2A28`). The 13-byte car table 0x80150E30 + player·13 is set from coin totals over the
  profile:

  | 2049 car index | Requirement |
  |---|---|
  | 0-5 | always |
  | 6 | all 48 race silver coins |
  | 7 | 24 race gold coins |
  | 8 | 36 race gold coins |
  | 9 | all 32 stunt silver coins |
  | 10 | 16 stunt gold coins |
  | 11 | 24 stunt gold coins |
  | 12 | every coin (48 / 48 / 32 / 32) |

  - Index → name is probably the CAR1-13 order (FORMULA 1 ... PANTHER) [I]. Check it against 2049's car select
    before shipping.
  - **Only cars are gated by coins.** The same function sets the track, mode and parts tables from points and
    stunt-score totals (§1.4), not from coins. TODO.txt's "tracks gated behind coin counts" is wrong.
  - 0x801164C2 and 0x801164C4 are 2049's unlock-all flags.
- **Coins are global, not per track.** Unlike keys, totals add up over every course.

### 1.4 Rush 2049 part unlocks [V for the rules, I for which row is which]

`func_800F2A28` also sets four per-player part tables. They come from one value, not from coins:
P = (sum of the u32 at profile +0xE4 + k·0x60, k = 0-11) / 10. P is probably race points [I].

| Table (per player) | Size | Always | Unlocked at P ≥ | Probable row [I] |
|---|---|---|---|---|
| 0x80150F40 | 6 | 0-2 | 150, 400, 700 | ENGINE (6 levels) |
| 0x80150F00 | 5 | 0 | 1: 300, 2: 1200, 3: 100, 4: 600 | TIRES (5) |
| 0x80150EB8 | 8 | 0, 1 | 2-3: 200, 4-5: 500; 6-7 never | ? (6 and 7 are never set: cheat or unused) |
| 0x80150ED8 | 9 | 0-2 | 250, 500, 800, 1200, 1600, 2000 | ? |

- FRAME also has 6 levels, so 0x80150F40 could be FRAME instead. Settle this by finding the setup screen's reader.
  - The accessors (`func_800F75B0` F40, `func_800F75D0` F00, `func_800F75EC` EB8, `func_800F7604` ED8,
    `func_800F7620` cars) have no `jal` callers in main or any ROM file. They are reached through pointers, or from
    code not yet disassembled.
- The tables are copied to 0x801426A0.. before a race (0x800F6040) and compared afterwards (0x80103DC0) for the
  "new part unlocked" message.
- The same function also unlocks tracks: battle arenas from battle stats (P ≥ 100 / 250 / 500 / 1000 on profile
  +0x60C) and stunt arenas 2-4 + the obstacle course from stunt scores (≥ 100k / 250k / 500k / 1M on +0x50C). None
  of those are ported yet.
- 0x801164C4 unlocks all parts. 0x801164C2 unlocks all tracks.
- **In this port.** 2049 cars use Rush 2's option rows (ENGINE = 2049 ENGINE 1-6, DURABILITY = FRAME;
  docs/rush2049_research/cars.md §9). Gating parts means:
  - recording P (+0xE4 isn't tracked; 2049 races write Rush 2 stats slots);
  - limiting those rows' values for 2049 cars;
  - TODO 11.
- **Sound.** The coin sound is 2049 sfx group 0x06 (docs/rush2049_research/audio.md).

---------------------------------------------------------------------------------------------------------------------

## 2. Design

### 2.1 Storage: `collectibles.json`, keyed by profile name

The `.mpk` layout is fixed, and hosted races run in HAWAII's slot, so neither SF Rush keys nor 2049 coins can go in
the record. Store them like `track2049_records.json`:
- One block per profile name: SF Rush key masks (7 × u8) and 2049 coin masks (6 race + 4 stunt × u16).
- Bound to the record position's name. The binding code from src/track2049_records.cpp moves into a shared helper
  that both files use.
- Written through `.tmp` + rename when a block changes. Deleting or clearing a profile clears its block (extend the
  41d6601 delete path).
- No profile: a session-only block, the same as Rush 2's 0x800C1DBC.

Rush 2's own keys stay in the pak record. The UI only reads them.

### 2.2 Shared pickup path: SF Rush keys and 2049 coins both run as Rush 2 keys

Both are "touch it, it disappears, a bit is saved". Rush 2's behavior 8 already does the touch test, the hiding, the
pickup effect and the sound. So:
- **Converters** emit both as Rush 2 key records (behavior 8):
  - **SF Rush.** Key n becomes bit n - 1. The model is Rush 1's own `KEYL1` from asset 12, which needs the same
    base-shift and 1/16 handling as the breakables. Fall back to Rush 2's `KEY` model.
  - **2049.** Silver coins get bits 0-7 and gold coins bits 8-15, in record order (matching 2049's spawn order). The
    models are `GOLDCOING_COIN` and `SILVERCOINS_COI` from file 68, converted into the track container.
  - Python and C++ must stay byte-identical (`cpp_test/build.bat` for both).
  - First, confirm which Rush 2 record field becomes object +0x68 (compare a Rush 2 `KEY` record with SF Rush's
    +0x4A).
- **Redirect hooks**, active only while a track1 or 2049 track is hosted:
  - `func_8005F00C` (hidden?): test the hosted course's side mask, not HAWAII's record.
  - `func_8005F418` (collect): set the side bit, skip `func_8005F338`, and mark the side block dirty.
  - Without these, a pickup would write **HAWAII's** key bits into the pak.
- **Coin sound.** On collection in a 2049 race, play 2049's coin sfx through src/audio2049.cpp instead of the key
  sound.
- **Dew guard.** Rush 2's 0x756 Dew animation branch must not catch coin models. Coins spin like keys.

### 2.3 SF Rush unlocks (tracks 18-24)

- Extend `rush2_track49_keys`: for ids 18-24, return the key count scaled so car select's `/ 3` gives SF Rush's
  rule. That means 3 for half the keys (TAXI) and 6 for all of them (TAXI + HOTROD), with 0 Dew. No other car-select
  change is needed.
- Car select draws key icons through `func_803B1B78` from the same mask (0x800C1DBC / record +0x18). It needs the
  SF Rush mask and the track's key total (6-8, not 12). Check how it lays out 12 + 4 icons first.

### 2.4 2049 unlocks (tracks 12-17)

- Evaluate 2049's table (§1.3) for each active profile from the side block whenever it changes.
- **Car select.** src/car2049.cpp appends types 22-34. Append only unlocked ones for each player. "Unlock All Cars"
  (src/cheats.cpp) still lists all of them.
- **Stunt arenas aren't ported** (TODO 5), so cars 9-12 can't be earned yet. Decision needed (§4).
- **Option.** "Rush 2049 Car Unlocks" on the Rush 2049 tab. Today every 2049 car is available, and turning locks on
  takes cars away.

### 2.5 Progress UI, phase A: recomp menu tab "Progress"

- **Profile picker.** In-use profiles from the pak image (0x8004B220, 4 × 0x2200) and the active records
  (0x8010D740 + k·0x6C0), plus "No profile (this session)".
- **Rush 2.** Per track (12):
  - keys n/12 and Dew n/4;
  - mystery cars earned on that track (TAXI / HOTROD / FORM1 / GT90 at 3 / 6 / 9 / 12, DEW car at 4 cans);
  - plus ROCKET (record +0x4B4), and PIPE / MIDWAY availability (0x800E7D50 / 0x800E7D19).
- **SF Rush.** Per track (7): keys n/total, with TAXI at half and HOTROD at all.
- **Rush 2049.**
  - Per track (6): silver n/8 and gold n/8.
  - Totals against each threshold.
  - The 13 cars, locked or unlocked, each with its requirement.
- **Reading.**
  - The game thread copies a snapshot once per frame, from a hook already called every frame.
  - The page reads the snapshot under a mutex. It never touches RDRAM from the UI thread.
  - The page refreshes on `EventType::Update` like `PlayersPage` (src/players_tab.cpp).
- **Read-only.** Unlock-all stays in the Cheats tab.

### 2.6 Progress UI, phase B: in game (later)

- **Race HUD.** A coin counter on 2049 tracks (2049 shows one) and the key pickup message on SF Rush tracks (check
  what Rush 2 shows today).
- **Car select.** Key icons for SF Rush tracks (from §2.3) and a coin total for 2049 tracks.
- **Menus.** A progress page in the game's own menus (for example next to the Records screen) showing the same data
  as the tab, drawn with the game's widgets and fonts.

---------------------------------------------------------------------------------------------------------------------

## 3. Phases

1. **Storage.** Shared name-bound block helper, `collectibles.json`, clear on delete, guest block.
2. **SF Rush keys.**
   - Converter (py + C++ parity) and the key model.
   - Redirect hooks.
   - Car-select count for 18-24 and key icons.
   - Test: pick a key up, see it stay gone next race, unlock TAXI at half the keys. Check that HAWAII's mask in the
     pak doesn't change.
3. **2049 coins.**
   - Converter and coin models.
   - Redirect hooks with bit layout silver 0-7 / gold 8-15.
   - Coin sound.
   - Unlock evaluation and car-select gating, behind the option.
   - Test: same as phase 2, plus totals.
4. **2049 parts** (§1.4).
   - Find which setup row each table gates.
   - Record P for each profile in `collectibles.json`. First pin down what +0xE4 counts and how 2049 awards it per race.
   - Limit ENGINE / DURABILITY (and any other matching rows) on 2049 cars to unlocked values. "Unlock All Cars" or a
     parts cheat lifts the limit.
5. **Progress tab** (phase A UI). It also shows P and the parts each threshold unlocks.
6. **In-game UI** (phase B).
7. **Docs.** Update docs/rush1_research.md §8, TODO.txt (SF Rush 1, 2049 1-2) and placement.md's dropped list.

---------------------------------------------------------------------------------------------------------------------

## 4. Open decisions

1. **2049 stunt-gated cars (9-12)** while stunt arenas aren't ported. Options:
   - keep them unlocked until the arenas exist (recommended: nothing is taken away for coins that can't be found);
   - keep 2049's rule and leave them locked;
   - remap them to race-coin thresholds.
2. **2049 car locks default.** Recommended: on, since that's the point of bringing them back, with the option to
   turn them off.
3. **Key and coin art.** Recommended: each game's own models (Rush 1 `KEYL1`, 2049 coins), not Rush 2's key.
