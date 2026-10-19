# Menu map

Where the game's menus are, what state says which one is up, and how to drive them from a test. Read this before
scripting a run through the menus instead of working it out again.

## Game state

`game_state` 0x8010C0D0 (s32) says which part of the game runs:

| Value | Part |
| --- | --- |
| 0 | Front end (the screens below) |
| 1 | Track select |
| 2 | Car select |
| 3 | Racing |
| 6 | Post-race (records, lap and race times, name entry; func_800A952C) |
| 7 | Race start (set by the car select to leave for the race) |
| 9 | Countdown |
| 10 | Race start, after loading |

The pause menu is not a game state: `pause_state` 0x8002305C (s8) is nonzero while it is up (see below).

## Front-end screens

In game state 0, `front_end_screen` 0x800E7BC4 (s16, -1 none) is the screen up, and `front_end_next_screen`
0x800E7BB4 (s32) the one `menu_frontend_loop_800AE288` switches to (at 0x800AE494). `menu_screen_dispatch_800AE0D4`
runs a screen through its jump table.

| Screen | What |
| --- | --- |
| 0 | Start Game (the race-type menu, below) |
| 1 | Select Player (the profile list; A picks one, then the track select). The list is JUST PLAY, CREATE PLAYER, then the saved players, with the cursor on the first saved player, so test runs press up twice: `8:START,11:A,12.5:DU,13.2:DU,14:A` reaches the track select as JUST PLAY. |
| 0xD | Controller Pak manager: never shown, any switch to it is redirected (docs/game_fixes.md#no-controller-pak-manager) |
| 0xE | Boot logos (then the title) |
| 0x10 | Title screen (START goes on to Start Game) |
| 0x11, 0x12 | Attract demo |

`front_end_from_logos` 0x800BF31C: set, the front end after a race starts over from screen 0xE; clear, from 0xF.
`front_end_exit` 0x800FAE6C (u8): 2 leaves the front end for the track select, as Select Player does.
The game itself only reads and clears `front_end_from_logos` (func_8009FD68): its initial value sends the first front
end through screen 0xE, and nothing in the original game sends it there again. Only our online race exit sets it.

### No-controllers marquee

Screen 0xE (`menu_screen_text_entry_overlay_803B5E90`, case 14 of the dispatch) calls `save_boot_load_settings_800B3528`
on its third frame. That function was written to run once, at power-on: it clears every port's present byte
(0x800D393C[4]), runs one pak scan (`save_pak_check_all_ports_80098D14`), then sets the no-controllers flag 0x800D3E93
unless some port's present byte is set again. The scan only marks a port present when its bit in the port mask
0x800BD428 is new, and it only scans on every 5th call (counter 0x800BD420). At boot the mask is empty and the counter
starts at 4, so port 1 is found. On any later run the ports are already in the mask, so no present byte comes back and
the flag is set. `race_start_timer_800AE670` then scrolls "THERE ARE NO CONTROLLERS CONNECTED. PLEASE TURN OFF THE
N64..." (message pointer 0x800C4D08) along the bottom of every menu at 0x800AF1F4. The flag is cleared only when a
controller turns up on a port that wasn't in the mask yet, so the message stays until the game restarts. Port 1 always
reports a controller (src/main.cpp `get_connected_device_info`), so the flag never comes from a missing controller: it
comes from the second pass through screen 0xE that online `end_race` causes when it sets `front_end_from_logos`
(src/online/online_game.cpp). Fixed by `rush2_boot_settings_ports` (src/pak.cpp, hooked at 0x800B39A4): it rebuilds
the present bytes and the flag from the port mask, which at boot already match them.

## Start Game rows

`start_game_row` 0x80023064 (u16) is the cursor. The rows depend on what is installed
(`mode_rows`, src/rush2049/track2049_menu.cpp): ONE RACE, CIRCUIT, PRACTICE, GHOST RACE, STUNT, then BATTLE (only with
the Rush 2049 ROM), RECORDS, UNLOCKS (only once shown), SETUP. Use `rush2::track2049::start_menu_row(kind)` for a
row's index, never a fixed number.

## Track select and car select

- Track select (state 1): the carousel's shown entry s16 0x803D05B4, entry count s16 0x803D05AE; the option rows'
  cursor s32 0x803D05BC, top row on screen 0x803D05C4, row count 0x803D05CC. A confirms the track and goes on to the
  car select. `RUSH2_TEST_TRACK=ID` opens it on a track.
- Car select (state 2): A on the last row picks the car; with every player done the race starts. B goes back to the
  track select.

### Game filter

With more than one game's tracks (cars) on a select, C-up (Y, Triangle, I) opens the game filter panel
(src/game_filter.cpp; General tab option Game Filter). While it is open the game sees no input, and the panel reads the
ports through `get_menu_input`, so `--input-script` presses (which skip that path) don't reach it: drive it with
`RUSH2_TEST_VPAD` (Y, UP/DOWN, A, B), which needs Background Input on when the window isn't focused. The hidden games
are saved in the saves' "game_filter" section.

How a select is rebuilt after a change, as the game rebuilds it between visits:

- Track select: `trackselect_frame_803ABE0C` calls `trackselect_init_draw_803AB294(1)` every frame at 0x803ABEDC,
  after its A/START and B checks; the setup runs once per visit, gated by byte 0x803C9218 (set by the setup, cleared by
  `trackselect_init_draw_803AB294(0)`, the teardown B runs). The filter's hook there tears down and jumps to the
  frame's end (L_803AC6D4); the next frame sets it up again (`rush2_track49_select_init` keeps the shown track).
- Car select: `menu_screen_car_select_frame_803B9478` calls `carselect_build_list_803B81F0(1)` every frame at
  0x803B94C0, gated by byte 0x803C69C0 (cleared by `carselect_build_list_803B81F0(0)`). In the setup, sp + 0x104 is
  the player being set up; at 0x803B8CE4 $v0 is the car type the carousel opens on, looked up in that player's list.
  Not found, the game opens on index 0 with that player's number as the type, so `rush2_filter_car_cursor` opens on
  the list's first car when the filter took the record's car off.

## Pause menu

`race_pause_menu_step_800AFD44` (src/pause_menu.cpp has the full layout).

- `pause_state` 0x8002305C: 0 racing, 1 the list, 2 OPTIONS, 3 CONTROLS, 4 AUDIO, 5 RESTART's and 6 QUIT's
  ARE YOU SURE.
- Cursor 0x80119635: 0 CONTINUE, 1 OPTIONS, 2 CONTROLS, 3 AUDIO, 4 RESTART, 5 QUIT (SKIP TRACK, the Pause Menu
  Extras option, sits between AUDIO and RESTART).
- Online the cursor only moves over CONTINUE, RESTART and QUIT, so QUIT is two DOWNs from CONTINUE.
- RESTART and QUIT ask ARE YOU SURE with NO under the cursor: RIGHT then A for YES.
- Offline QUIT goes to the front end (state 0); online every machine leaves the race on the same frame
  (docs/online.md#race-end).
- Online only the host can QUIT; a joiner leaves by leaving the lobby.
- Opening the pause menu turns off menu input on every port except records 0 and 1's (`input_port_state_80125AC4`);
  the menus read only enabled ports' presses (menu buttons 0x8011962A pressed, 0x80125A68 repeat). Online the
  records have no port, so leaving the race turns the menus' ports back on (docs/online.md#race-end).

## Starting a race without the menus

A test that only needs a race shouldn't script the menus at all: `RUSH2_TEST_RACE=<track select id or custom track
folder>` (src/test_race.cpp) sets the race up on the first frame the title screen is up and starts it, so the race is
loading about 5 s after launch (through the menus it took about 25 s). `tools/test_race.py TRACK --cars T,T` runs one
hidden and silent from a data folder of its own and reports when it loaded, each player's car type and how far it
drove; read its docstring for the options.

- `RUSH2_TEST_RACE_CARS=T[,T...]`: one car type per player, 1 to 4 players (split screen). Without it one player, in
  `RUSH2_TEST_CAR`'s car or the Pickup (0). `RUSH2_TEST_RACE_LAPS`, `RUSH2_TEST_RACE_DRONES`: the track select's LAPS
  and DRONES (0x800D5760 +0 and +1, raw counts); otherwise the saved ones. `RUSH2_TEST_RACE_BATTLE=1`: the race
  select's BATTLE row on (not saved; docs/rush2049_research/battle.md section 7.2). `RUSH2_TEST_MANUAL`, `RUSH2_TEST_DEATHS`
  and `RUSH2_TEST_FINISH` (src/players4.cpp) still apply.
- Each player races as JUST PLAY leaves them, never with a saved player: record +0x24 is the controller's own buffer
  (`controller_buffers` 0x8010D740 + port * 0x6C0), its 0xC0 bytes of options at +0x4C4 reset from
  `controller_default_options` 0x80118A50 + port * 0xC0, as `menu_select_player_pick_saved_803B44E8` does for JUST PLAY
  (0x803B4590-0x803B4620, cursor 0). Player p is on controller port p, so the input script drives player 1.
- The car select's part: `profile_set_selected_car_type_80097F8C(buffer + 0x31, type)` (which keeps a 2049 or other
  added type in full, src/rush2049/car2049.cpp), the car table entry (0x800D9C10 + p * 9: type, colors from the
  buffer's getters) and the car type tables' player rows (`rush2::race_setup::fill_player_rows`).
- The track select's part: the track through `rush2::track2049::host_menu_track` (an added track races in its host
  slot), ONE RACE's game mode (0x8010C3E8 = 0; the race start makes a stunt track or arena stunt mode itself).
- Then the front end is left as for a race and game state 7 starts it (`rush2::race_setup::leave_front_end`, also how
  an online quick race starts, src/online/online_game.cpp).

The setup pieces are in src/race_setup.cpp, shared with online races. After the race the game goes back to the
front end as after any race.

## Driving the menus from a test

`--input-script "<seconds>:<button>[:<hold seconds>],..."` presses N64 buttons (A B Z START DU DD DL DR L R CU CD CL
CR; SL SR the stick) at times counted from that game's own launch. A press lasts 0.15 s unless given; use 0.3 s or
more for the pause menu. The scripted presses skip the controller path (port claim, bindings): to test that, use
`RUSH2_TEST_VPAD` (src/input.cpp).

A recipe that reaches an online race through every menu (tools/online_test.py, the joiner launched 42 s after the
host; `RUSH2_ONLINE_PORT` keeps it off a lobby already open on this machine):

- Host, `RUSH2_ONLINE_TEST=menus:host RUSH2_ONLINE_PORT=27999 RUSH2_TEST_TRACK=5`:
  `14:START,18:A,22:A,128:A,133:A,145:A,160:A:25` (title START, ONE RACE, Select Player, then the track select and
  car select once the joiner is there; A held from 160 s drives). The race loads about 140 s in.
- Joiner, `RUSH2_ONLINE_TEST=menus:join:127.0.0.1:27999`: `75:A,80:A,98:A,118:A:40` (its own launch time).
- Pause and quit at T: `T:START:0.4,T+4:DD:0.3,T+6:DD:0.3,T+8:A:0.3,T+10:DR:0.3,T+12:A:0.3`.
- `RUSH2_ONLINE_CAR=TYPE` forces a car type at the car select.

The boot takes longer with more games starting at once: run one pair at a time, or the scripted presses land on the
boot logos.

## Launcher on a first boot

`tools/first_boot_test.py OUT_DIR` runs the whole check (picker, cancel, a wrong file's error box, the ROM, Start Game).
To test the launcher's Locate ROM (the first-boot screen) without touching the player's data, run in a simulation's own
data folder with the ROM import skipped: `RUSH2_SIM=1 RUSH2_SIM_TURBO=0 RUSH2_SIM_SHOW=1 RUSH2_TEST_FIRST_BOOT=1
RUSH2_DATA_DIR=<empty tmp folder>`. The launcher shows Locate ROM focused, so a posted ENTER opens the file picker.
On Windows the picker and the error boxes are `#32770` windows owned by the game window (the picker titled `Open`);
set its `Edit` child's text to a path and post IDOK to pick, IDCANCEL to cancel. Picking the ROM stores it in that
folder and the row turns to Start Game. Posted mouse clicks don't reach the UI, and ENTER doesn't press the Games
tab's Select ROM buttons; the launcher route is the one that works. The picker logs `[file picker]` lines
(lib/RecompFrontend/recompui/src/util/file.cpp).
