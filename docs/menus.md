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
| 1 | Select Player (the profile list; A picks one, then the track select) |
| 0xD | Controller Pak manager: never shown, any switch to it is redirected (docs/game_fixes.md#no-controller-pak-manager) |
| 0xE | Boot logos (then the title) |
| 0x10 | Title screen (START goes on to Start Game) |
| 0x11, 0x12 | Attract demo |

`front_end_from_logos` 0x800BF31C: set, the front end after a race starts over from screen 0xE; clear, from 0xF.
`front_end_exit` 0x800FAE6C (u8): 2 leaves the front end for the track select, as Select Player does.

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
