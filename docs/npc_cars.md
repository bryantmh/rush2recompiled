# AI Opponents (computer car choices)

The Players tab's **AI Opponents** section (src/npc_cars.cpp, saved in npc_cars.json) chooses each computer car's
car, stripe, rims and MAIN / ACCENT / STRIPE COLOR, like the GameShark codes that set the opponents' cars
(`800D9C1A + 9n`, the car table's type byte, and `800F6476 + 0x81C n`, the physics car's copy). Every choice starts at
Random (rims: the car's own), which leaves Rush 2's own picks alone, so no separate toggle is needed.

Opponent n is the n-th drone slot (physics car `0x800F5470 + slot * 0x81C`, `+0x7E8 == 1`) in slot order: with one
player, car slot n.

## Drone car table

func_800A37F4 (`race_preload_car_assets`) loops over the 8 car slots and, for each drone (`+0x7E8 == 1`, or every slot
while `0x800FAE6C` is set), fills its entry in the race car table `0x800D9C10 + slot * 9`:

| Byte | Meaning |
|---|---|
| +1 | car type (car select names, from its logo table 0x803C7688: 0 PICKUP, 1 COMPACT, 2 MUSCLE, 3 MOBSTER, 4 SEDAN, 5 BANDIT, 6 COUPE, 7 EXOTIC, 8 VAN, 9 SPORTSTER, 10 SUBCOMPACT, 11 CONCEPT, 12 HATCHBACK, 13 CRUISER, 14 STALLION, 15 4X4, 16 TAXI, 17 HOTROD, 18 FORMULA, 19 PROTOTYPE, 20 ROCKET, 21 DEW; 23-35 the Rush 2049 cars): random 0-15 (`0x800A3AE8..0x800A3B98`), redrawn until no earlier slot has it (`0x800A3BA8` loop) |
| +2 | MAIN COLOR: random 0-31 (`0x800A3C14`), redrawn until no earlier slot has it |
| +3 | ACCENT COLOR |
| +4 | STRIPE: 0 NONE, 1 SINGLE, 2 SINGLE FAT, 3 SNGL FADE, 4 DOUBLE, 5 DOUBLE FAT, 6 TRIPLE A, 7 TRIPLE B (car select value strings 0x800CC8D8.., pointer table 0x800C4960; TIRE RIMS values RIM 1-21 follow at 0x800C4980) |
| +5 | STRIPE COLOR |
| +6 | grid slot (func_8006DE20) |
| +8 | driver: 8 for a human (func_8009E6DC: `+8 < 8` makes the physics car a drone, kind 1) |

After MAIN, a coin flip (`0x800A3DAC`) picks either a stripe (`0x800A3DF4`: +3 = MAIN, +4 = 1-7, +5 a color other than
MAIN from func_800889BC; the path then branches to `0x800A3E80`) or none (`0x800A3E6C`: +3 a color other than MAIN, +4 = 0,
falling through `0x800A3E7C`). `0x800A3E80` is where both join; the loop after it checks the type's asset fits the
car slot memory and steps the type (mod 16) until it does. func_8009E6DC copies the entry to the physics car:
`+0x7EA` type, `+0x7EB..+0x7EE` MAIN, ACCENT, STRIPE, STRIPE COLOR.

The New York Cabs cheat (`0x800D9E89`) skips both draws when `0x8010C3F0` is 1 or 3: every drone gets type 0x10 (TAXI),
MAIN and ACCENT 0x1A, no stripe (`0x800A3AAC..0x800A3ADC`, `0x800A3BCC..0x800A3C0C`). In attract mode (`0x800E7BB0`)
the draws are skipped too.

The choices are applied by a hook at `0x800A3E80` (us.toml, `rush2_npc_cars_apply`), after Rush 2's draw and before
the ghost races' hook (src/ghost.cpp), which puts a ghost's recorded type and colors back on its slot. The hook text
then reloads `$v1` (the type, which the asset check reads) from the entry. Nothing is applied in attract mode or when
the cab cheat makes taxis, so the cheat wins. A chosen type skips the "no earlier slot has it" check (two cars may
share a type, as the GameShark codes allowed); later random drones still avoid it. A stripe chosen for a car that
Rush 2 drew without one gets a random STRIPE COLOR unless one is chosen.

The ghost hook used to sit at `0x800A3E7C`, which the stripe path skips, so a ghost whose drone slot drew a stripe
kept the random colors; moving it to `0x800A3E80` fixed that too.

## Rims

TIRE RIMS are not in the car table. A car's rim is the per-type table `0x80201020` (Rush 2's `0x800C0E48`, moved to 36
types by src/car2049.cpp; RIM01 + value, 0-20) at row 0 for a drone, row player + 1 for a human (the row is 0 unless
the physics car's `+0x7E8` is 2). It is read in four places, each hooked so an opponent's chosen rim replaces the
type's:

| Function | Hook | Car | Row | Rim register |
|---|---|---|---|---|
| func_80085FD0 (`car_build_scene_nodes`): the wheel nodes | `0x800865CC` | `$v1` (= `$fp` % 36) | `$s5` | `$t9` |
| func_8005A598 (`carselect_preview_update`, per car per frame in a race): the wheel texture, blurred (`0x800CEDAC` table) or sharp | `0x8005AAF8`, `0x8005AB08` | `$t1` (physics car) | `$ra` | `$v0` |
| func_80087290 (`car_preview_apply_state`) | `0x800875F8` | `[$sp + 0x3C]` (physics car) | `$v0` | `$t9` |
| func_8008DBA0 (`car_init_setup`): stored at car `+0x59E` | `0x8008DD54` | `$s2` (physics car) | `$s1` | `$t8` |

func_80085FD0 is also used by the car select's previews (`$fp` a preview id there), so its hook only applies to the car
func_80086CA4 (`race_load_car_assets`) is building (hook at `0x80086E0C`, before its call).
