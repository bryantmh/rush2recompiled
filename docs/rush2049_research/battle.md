# Rush 2049 battle mode

Port: `src/battle.cpp`, hosting in `src/track2049.cpp` / `src/track2049_menu.cpp`. Tags: **[V]** verified in code or data,
**[I]** inferred or approximated.

## 1. Where the code is

- The battle logic is **not in the main segment**: it is a mode overlay, raw deflate at **ROM 0xB6FEC4**, 0xAB70 bytes,
  loaded at **0x8038A400** (strings: `STNT_*`, `HEALTHBG/HEALTHBAR`, `WPR_*`, `WFX_*`, `MINE_CAP*`, `HIT_WALL`,
  `SCORCH_MARK`, `X %i`). The setup screen overlay (ROM 0xB5C534) loads at the same address; they are different overlays.
  `python tools/rush2049/decomp.py ovl ADDR` decompiles its functions (m2c; function bounds found by scanning for `jr $ra`).
- Game type `0x8014A110 == 6` is battle (stunt 4, obstacle 5). Timer setup at 0x800FC0FC: a battle uses `0x80140BD8`
  minutes x 60 when `0x80140B08 == 1`, else 1200 s. Both come from the options object (+0x25, +0x27); their defaults
  live in the setup overlay and weren't read. The port's limit is the Games tab option (default 3 minutes).
- Per-car battle state: `0x80152818 + i * 0x3B8` (2049). +0x384 weapon type, +0x385 ammo, +0x386 s16 **health
  (max 0x320 = 800, the HEAL pickup sets it)**, +0x388 last damage, +0x38C power-up flags (1 invisible, 2/8 shield bits,
  4 shield), +0x390 invisibility seconds (30.0), +0x3AC ram cooldown, +0x35B player index, +0x359 dead flag.
- Damage: `func_8038D3A4(attacker, victim, amount)` (overlay): ignores a victim of the same team (`0x8012E67C` per
  player), multiplies by `0x80394DC4` = **0.2** while the shield is on (+0x38C & 4), subtracts from +0x386, and at <= 0
  calls the kill function `func_800C55E4`. Explosions (`func_8038D798`) damage a car within radius r by
  `(1 - d^2 / r^2)^2 x damage`. The RAM weapon (type 5) is passive: `func_800CE358` (main) damages a car it hits by
  roughly `160 + 7 x speed` and uses an ammo; in battle (type 6) car-to-car contact doesn't wreck.
- Weapons (pickup anim ids 0x15E-0x168 -> type via `func_8010D3C0`): 0 CANN, 1 GATT, 2 GREN, 3 MINE, 4 MISS, 5 RAM,
  6 ROCK, 7 SONC; HEAL, INVS, SHLD are power-ups, POWUP a random power-up. **Starting ammo** `0x80121D60`: 20, 100, 20,
  3, 3, 5, 20, 5. Pickup radius 6.0. Projectile and explosion tuning (speeds, damage, cooldowns) is **[I]** in the port.

## 2. Assets

- File 76: `WEPICON_*` (pickups), `WEP_*` (the weapon mounted on a car), `WPR_*` (projectiles: CANN, GREN, MINE, MISS,
  ROCK, SONC), `WFX_*` (effects: tracer, muzzle flashes, shield, ripples, sonic blast, mine rays), `MINE_CAP*`,
  `WEAPONHUD` (128 x 16 strip of icons: gatling, cannon, rockets, missile, stealth, shield, burst, empty).
- File 63: `BCOIN_BLUE/RED/YELLOW/GREEN` (coin models and 16 x 16 images: 2049 counts kills as coins, "X n"),
  `HEALTHBG` (64 x 8 frame), `HEALTHBAR` (64 x 4 intensity gradient, tinted with the primitive color), `AMMOCOUNT`.
- The converter merges both into a battle arena's geometry. Rush 2's HUD layout entries (0x28 bytes, +0 = a texture
  **name**) find textures by name in any loaded container, so the 2049 images are used as they are.
- Each arena places 9 weapon pickups (8 weapons + POWUP x6-9, flag 0x40).

## 3. What the port does

- Arenas are hosted in STUNT1 (stunt mode), **no computer cars** (2049's battle was multiplayer only; AI is future work).
- Health 800, pickups (respawn 20 s [I]), the 8 weapons, power-ups, kills, the Games tab time limit (the stunt clock,
  `0x8010C204`, is 3.5 during the start countdown and 300 afterwards: only the 300 is replaced).
- HUD: Rush 2's elements are removed except the speedometer (elements are in a pool at 0x802F6400, `func_80060418`);
  the stunt score panels are hidden (`func_800B9CC0`, `func_800B7654`). Battle elements: HEALTHBG with a flat fill
  (HEALTHBAR can't be tinted by a widget), the weapon's rotating 3D model at the bottom left (a pool record in front of
  the camera; camera +0x24 position, +0 right, +0xC up, +0x18 **forward**), the coin and kill count at the bottom right.
  With more than one player the weapon shows as its WEAPONHUD icon, because the model would show in every view.
- Image widgets clip instead of stretching, so the HUD is smaller than 2049's.

## 4. Not done / known issues

- Projectiles pass through walls (no collision query); no sounds; no kill feed or end of battle screen/winner;
  coin icon draws black corners (alpha); text uses Rush 2's font in green rather than 2049's white digits;
  the direction arrows to opponents; the SHLD/INVS visuals; split-screen HUD placement of the 3D weapon;
  3-4 player HUD layout; `WPR_MINE` placed objects; DM5's PVS table; unverified weapon numbers.
- Test aids (env vars): `R2_BATTLE_GIVE=<weapon 0-7>` gives car 0 a weapon; `R2_BATTLE_TEST=1` destroys car 0 at 6 s.
