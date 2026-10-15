# Online play

Up to 8 players race on separate machines, on a LAN or over the internet by the host's address. There is no server:
one player hosts and the others connect to them. Every machine runs the whole race itself from the same inputs
(deterministic lockstep), so only inputs travel over the network.

| File | What |
|---|---|
| `include/online.h` | The API: lobby, roster, race settings, lockstep, the game side. |
| `src/online/online_net.cpp` | ENet (`lib/enet`) session on a thread of its own: lobby, host relay, load and desync checks. |
| `src/online/online_discovery.cpp` | LAN lobby beacons (UDP broadcast on port 47101). |
| `src/online/online_upnp.cpp` | Router port mapping (UPnP, `lib/miniupnp`). |
| `src/online/online_game.cpp` | What the game does differently in an online race (hooks in `us.toml`). |
| `src/online_tab.cpp` | The Online tab and the `online` config (`online.json`). |
| `src/ram_watch.cpp` | Test aid: RAM watches and dumps. |

Nothing changes unless a lobby is hosted or joined, so offline play is untouched and no config needs migrating. A
joiner's gameplay settings are the host's while in a lobby and come back on leaving (Gameplay settings below).

## The Online tab

Settings > Online (src/online_tab.cpp) is only for getting into a lobby, and shows one of two views by the session's
state. Not in a lobby: **Host a Lobby** (the Host Lobby button, Max Players, Port) and **Join a Lobby** (the LAN lobbies,
each with a Join button and why it can't be joined, then Host Address and Join by Address), with the last error above
them. Connecting or in a lobby: the lobby's name and what happens next, Cancel / Close Lobby / Leave Lobby, **Invite
Players** for the host (the LAN address and the internet address with the router port's state, `router_port()`; when
the port is closed, what to forward where) and **Players** (name, how far through the menus, Host / You / ping). On a
switch the controller's focus goes to the new view's first button. The display name is on the Controls tab (Online
heading). Port and max players apply the next time you host. The race itself is chosen in the game (the lobby).

## The lobby

`src/online/online_lobby.cpp`. The host goes through the game's own menus as for a local race, and every other
player's game follows it; an overlay (a RecompUI context that takes no input, made at startup in the launcher's init)
lists the players and what the lobby waits for. The overlay steps aside while a menu or prompt is open
(`is_context_capturing_input`, `is_prompt_open`), since showing a document focuses it. Each player is shown by the name
of the profile they picked on Select Player (`set_profile_name`, from `rush2::collectibles::player_name`), or their
display name before that or without one.

The host publishes where it is every frame (`MenuState`: phase, the race settings as its menus have them, its track
select's cursor). Each machine's progress comes from where its own game is, never from what it was told, so a player
who backs out or joins late is waited for, and nobody waits on a step that already happened.

- **Waiting**: while the host is anywhere else in its menus, the others wait on the title screen (screen 0x10, its
  attract demo held off by keeping 0x8010C034 at the game clock 0x80117488; a demo that started anyway is ended with
  START) with their controllers off.
- **Select Player**: when the host chooses One Race, Stunt or Battle on Start Game, each other game gets there as the
  host did: START on the title screen (which makes its controller player 1, `menu_title_start_pressed_800A6B58`), the
  same choice on Start Game, A. The choice is sent as what it is (`rush2::track2049::StartKind`), not as the row
  (0x80023064): the menu's rows differ by machine (BATTLE needs the Rush 2049 tracks, UNLOCKS the unlock system;
  `start_menu_row`). Each player picks their own player, and any screen Select Player opens is theirs too. Select
  Player leaves the front end for the track select by setting 0x800FAE6C = 2 (`menu_frontend_loop_800AE288` then
  leaves the screen and sets game state 1). The host is never held: Circuit, Practice, Ghost Race and the profile
  screens keep the others in the lobby, and the host's track select tells it to back out.
- **Track select**: the host's. It browses freely but its A and START only work once every player is on their track
  select, every player has the game ROM the track needs, and it has been up 20 frames. The others' track selects show
  the host's every frame with their controllers off: the track (0x8010C3F0), the carousel entry showing it (s16
  0x803D05B4, found in the carousel at 0x80310400 by `carousel_index`), the settings (0x800D5760), the option cursor
  (0x803D05BC) and top row (0x803D05C4), and the BATTLE row and battle rules (put back after). The carousel moves
  one place per change of the shown entry: each frame `trackselect_init_draw_803AB294` (0x803ABB1C-0x803ABCC4) shifts
  an entry's target by 100 when its place relative to the shown one (+0x14) changed, whatever the change, so a jump of
  several entries left the dioramas out of line with the track. The shown entry steps one entry a frame toward the
  host's, the shorter way round (carousel length s16 0x803D05AE). A joiner's track select
  offers every track that machine has, whatever its profile unlocked (`set_all_tracks_open`), so the host's is on it.
  A custom track's select id (38 and up) is the machine's own numbering, so the host sends it as its key
  (`RaceSettings::mod_track`, `mod_id/folder#hash` from `rush2::track_mod::identity`, the hash of every file the
  conversion reads) and each machine looks its own id up (`track_for_online_key`). Each machine reports whether it has
  the track the host is on (`set_track_check`: the key's FNV hash and a "lacks" flag, sent with its progress); the host
  can't confirm until everyone has answered for that track and has it, and it names who doesn't. A joiner without it
  keeps its own track on screen and says so. A player who joins while the host is already on the car select stays on
  their track select, and the race doesn't start until the host goes back and picks a track they have.
  When the host goes on to the car select, theirs are pressed A. One that is for another kind of race than the host's
  (the host went back and chose another) backs out and comes in again.
- **Car select**: everyone picks their own car. A player who has stays on the screen (the car select's
  everyone-picked check, `menu_screen_car_select_frame_803B9478` at 0x803B96D4, through src/players4.cpp's hook; its
  flag in 0x803CB39C set) with their controller off; the host keeps B, which takes everyone back to the track select as in a local race. Once
  all have, the host starts the race; each machine sets it up and starts it as the car select does
  (0x803B96D4-0x803B96F4: `music_queue_stop_80062FA4`, game state 7, `carselect_build_list_803B81F0(0)`,
  `overlay_load_800A62C0(0)`). A joiner only backs out of it to follow the host.

The players' controllers are steered at the game's controller read (`get_game_input` in main.cpp): in a lobby ports
2-4 read nothing (nobody else joins on the same machine), and port 1 is the player's, the player's without A and
START, B only, nothing, or buttons pressed for them. Everything the race needs (race type, track, the track select's
settings, each player's car and profile) goes to every machine with the race start, so the race is the same whatever
the menus showed.

## LAN lobbies

A hosting machine broadcasts a beacon ("R2OL", lobby name, players, settings, compatibility key) to 255.255.255.255 and
127.0.0.1 on port 47101 every second. `lan_lobbies()` lists those heard in the last few seconds; a lobby with another
compatibility key (another build) is listed but can't be joined. When the lobby closes the host sends one more beacon
with max players 0 (`stop_beacons`), which takes it off every list at once. The listener thread is stopped by the
online `atexit` handler (`stop_lan_listener`): a joinable `std::thread` left to its static destructor calls `abort()`,
and Windows then spends seconds writing a crash dump on every close.

## Hosting over the internet

Players join by the host's public address and port (UDP 47100 by default). Hosting always asks the router to forward
the port (UPnP IGD: `upnpDiscover`, `UPNP_GetValidIGD`, `UPNP_AddPortMapping` with a one-hour lease, renewed while
hosting) and shows the router's external address. A router behind carrier-grade NAT (the IGD reports a private
external address) can't be reached this way; the status says so. Without UPnP the host forwards the port by hand. There is
no setting for it; `HostOptions::open_router_port` is off only in the test aid's quick races.

## The race

Every machine sets the race up the same way and then runs it in lockstep:

- **Setup** (`setup_race`, from the car select): player and view counts (0x8010C3E2, 0x8010C3EC), the host's race
  type (0x8010C3E8), track (0x8010C3F0, a track select id: the race setup turns the added games' ids into their host
  slot) and track select settings (0x800D5760: laps, drones, options), the car table (0x800D9C10), the seeds (0x800BD808 = race seed | 1, 0x800D4030 = 0),
  and the player records: blank but for the button masks (the game's default layout, A gas, B brake, stick steering, R
  and Z shifts, C-down reverse, C-up abort, L view, C-right horn), car = player, no port (5), a profile (+0x24) and
  the gearbox each player picked (+0x20, 1 automatic; `CarChoice::automatic`, from the joiner's own record +0x20 at
  its car select; the race copies it to the physics car's +0x3D8, 0 manual).
  The front end is left as its loop leaves it for a race (`menu_screen_dispatch_800AE0D4` with the screen up and
  $s0 = 0, then screen -1), then game state 7 starts the race.
- **Profiles**: the race reads a human car's type from its profile (+0x31 low 5 bits, `race_setup_cars_8009E6DC` at
  0x8009E9B8; the Rush 2049 types through `rush2_car49_select_get`, which online gives each car the type its player
  picked, `rush2::online::game::car_type`), and the keys from it. Each player sends the profile their car select left
  (0x6C0 bytes, with the car; `set_car`), and the race start carries them all. This machine's player races with their
  own profile, so the keys they take and the post-race save to it; the others race with copies at 0x80FF0000 +
  player * 0x6C0, the same bytes on every machine. The hash leaves the records' profile pointer out. One shared blank
  profile made every human car type 0 in the physics while each machine loaded the picked car's model.
- **The players' saves**: this machine's player's profile is their Controller Pak note itself. The race setup never
  copies the sent bytes over it (that put back whatever the game had saved since the car select), and when it sets
  the car byte (+0x31, Rush 2 types only: a Rush 2049 type doesn't fit the 5 bits) it updates the section's byte sum
  at +0 (+1..+0x33), which the game checks when it reads the pak. A stale sum made the next boot call the note damaged
  and wipe it. `tools/pak_check.py` checks a save's profiles.
- **Cars**: the race setup marks the players' car table entries human (`race_assign_cars_to_players_800A580C`: +7 =
  0xB0, +8 = 8), `race_reset_car_results_8009ECD0` makes their physics cars +0x7E8 = 2, and
  `race_preload_car_assets_800A37F4` loads each human car's type (`car_slot_load_80087CE4`, its model container in
  `car_banks` 0x80221750) as for a local multiplayer race, whatever the machine's car select showed.
- **Ports**: online every record's port is 5, so the pause menu's `input_set_port_state_80093DC0` calls with it write
  one byte past the four at 0x80125AC4. Which player a controller drives (`rush2::input::port_player`, used by the
  Controls tab and the per-player bindings and control profiles) can't come from the records then: online, port 1 is
  this machine's player and the other ports nobody. Reading the records flickered, because the presentation lends
  record 0 port 1 for each frame's drawing, so the controller's bindings swapped between players frame to frame.
- **Frame time**: every frame is two retraces from the setup on: the frame setup's retrace count (0x8002301C) and
  `frame_dt` (0x80023028) are overwritten after `game_thread_main_init_800B4EB0` measures them (hook at 0x800B5464).
- **Physics**: the input thread's input step and physics tick (`input_thread_main_80076DE0` at 0x8007728C) are
  skipped; the game thread runs exactly two (`physics_thread_step_once_80076D70`) at the top of each frame, after
  every player's input for the frame has arrived.
- **Inputs**: each machine sends its input (N64 buttons in the game's layout, stick, analog pedals, battle buttons)
  for frame `f + delay`; the host relays every frame's inputs once it has them all. The delay is about one round trip
  of the slowest player (1-8 frames); the first `delay` frames use no input. The input step writes them into the
  records (`input_apply_default_controls_80076A00` at 0x80076CE0: held buttons with the stick's d-pad bits, stick,
  stick range). The analog pedals come from them too (`rush2_analog_pedals`).
- **Presses**: a record's pressed (+2) and repeat (+6) buttons come from its port, which online records don't have.
  The frame sets them from the held buttons of this frame and the last, `player_join_init_all_80080788` gets the
  last frame's again at 0x80080B0C, where it clears a portless record's before `player_join_init_car_80080524` shifts
  gears from them, and they are set again after the physics ticks, whose input steps clear them, for the rest of the
  frame (the pause menu, the camera's VIEW).
- **Checks**: when the race loads, each machine reports a hash of the cars' physics and race state, the player records
  and the seeds; the race starts only if all match. Every 30 frames they compare the same hash; a mismatch ends the race
  for everyone ("desync"). A player whose input doesn't come for 15 s is dropped: their car coasts.
- **Waiting**: while a frame's inputs are late by over a second, the lobby overlay comes up naming who it waits for
  (`race_status`, `race_notice`); it goes when they arrive.
- **Pause**: any player's START pauses everyone's race. `race_pause_menu_step_800AFD44` reads every player's presses,
  which are the lockstep's, so the menu runs the same on every machine; the frame's physics ticks are skipped while
  0x800201E4 is clear (the pause menu clears it, and a RESTART's loading). Its other way to pause, a player's port
  reporting bit 8 (0x8010C150[player], the pad at 0x80023044 + 6 * player: this machine's controllers), is skipped
  online (hook at 0x800AFDE0, `rush2_online_pause_start_only`). OPTIONS, CONTROLS, AUDIO and SKIP TRACK change one
  machine's own settings, so online the cursor moves over CONTINUE, RESTART and QUIT only (`src/pause_menu.cpp`).
  RESTART restarts the race in lockstep; QUIT ends it (race end).

## One view per machine

The race setup sets the view count to the player count (`race_setup_800A5ADC` at 0x800A5FCC, `sh` through $v0, so the
reference scan misses it); a hook just after (0x800A5FD0) puts it back to 1. View 0 follows this machine's car
(0x80240380, the view-to-car table players4 moved, read by `func_800A3690` at setup).

The car's view (car state +0x347, -1 none) forks the simulation in two places, found by running the same race with
each machine viewing a different car (it parted at the first wreck):

- `car_wreck_steer_to_respawn_800754B4` at 0x800757AC: a wrecked car with a view has its velocity (+0x218) damped while
  it glides to the respawn point.
- `race_wreck_respawn_wait_8008E40C` at 0x8008E4C4: a car with a view takes the respawn camera path (camera mode
  +0x348 = 4, the view's camera state at 0x800E79D0 + view * 0x40 and 0x80240390 + view * 0x30, a sound, then +0x342 = 1
  and +0x104) with its own timing.

`rush2_online_sim_view` (hooks at 0x800757B0 and 0x8008E4C8) gives every human car without a view view 3, which a
one-view race leaves unused, so every machine takes the viewed path for every human car. The other readers of +0x347
are camera, particles and the car selection preview.

Rush 2 places a human car's body (its scene node, car state +0xF0) in the camera update's loop over the views, for the
car each view follows (`cam_update_dispatch_8009D02C` 0x8009D08C-0x8009D0F4: the car's position raised by the f32 at
0x800CFD84, and its matrix, car state +0x48, through `car_matrix_copy_and_apply_8009ABDC`), and the computer cars'
after it (`cam_update_all_players_8009D9F4` 0x8009DFD8-0x8009E084, skipping cars whose physics car +0x7E8 is 2). With
one view the other players' bodies stayed on the grid and only their shadows moved; at 0x8009E01C a human car without
a view is placed as a view places its car (`rush2_online_body_kind`). The same loop switches a view's camera on its
player's VIEW (record +0x1A against +2), taking view v's player as record v: online view 0 takes this machine's
player's record (hook at 0x8009DA60, `rush2_online_view_record`). With two or more players every human car gets an
ARROW (`race_load_car_assets_80086CA4` 0x80086FF8-0x80087048) drawn in the other player's view (flags 0x200 | 1 << (1 -
car)) and sized from that view's camera (`car_arrow_update_80059B9C` at 0x80059BF4, view car ^ 1); online the
other players' arrows are drawn in view 0 and sized from it, and this machine's player's has none
(`rush2_online_arrow_views`, `rush2_online_arrow_view`).

The wheel placer (`carselect_preview_update_8005A598`, per car per frame) shows a car's wheel nodes through
`node_swap_visibility_8005A528`: for a car with a view it hides them in that view's close cameras, and for a car
without one (`node_reset_visibility_800590AC`, which also reads $s0) it shows them in every view only when $s0 (car
state +0x348 >= 2) is set; the hook at 0x8005A770 sets it for a viewless human car (`rush2_online_wheels_shown`).
It then draws each wheel at its car type's wheel model scale (0x802006E8 front, 0x802009B8 rear, f32, at 0x8005AB78-
0x8005AC2C) from a per-player row of the car type tables at 0x80200000: row 0 for the computer cars, row +0x7E0 + 1
for a human car (36 types a row). The car select fills a player's rows from their profile
(`carselect_build_list_803B81F0`, 0x803B8B10-0x803B8F7C: 13 getters, 0x800B2424-0x800B2694, per car type); online it
has one player, so it only ever fills row 1, and a Rush 2 car's wheel scales in the other rows are 0: the other
players' wheels were drawn at size 0. The race setup fills the rows that only change how a car looks (the colors,
rims at 0x80201020 and wheel scales) for every race player from the profile the race uses (`fill_player_rows`). Only
Rush 2 types: a Rush 2049 type's settings are in a side slot of the save profile, which a profile sent from another
machine hasn't (filling those made the machines set up different races), and its rows are all set at boot. The
settings rows (0x80200418, 0x80200C88-0x80200F68, 0x802010D8) are left as they were. Only 4 player rows exist.

The HUD and the frame's drawing are player 1's (record 0). Around them (`race_start_timer_800AE670` 0x800AF1D4 to
0x800AF1F4) player 1's record swaps cars with this machine's player's and takes controller port 0
(`rush2_online_present_begin/end`); nothing in the race runs in between.

### The HUD online

A networked race's HUD looks like a one-player race's. The HUD is built for the race's player count, so the joiner
first got player 2's gear box (manual or not) and a second race time under player 1's. Two things fix it:

- **Player count**: around the HUD build and update (`src/hud.cpp`, `hud_players_begin/end`) the player count
  0x8010C3E2 reads 1. The per-player elements show themselves by it: the race time (`hud_widget_cb_icon_row_a_800B9754`),
  the lap time (0x800B9978), the deaths (0x800B9C44) and the lap banner (0x800BA868). Nothing else there reads it but the
  view fog (`view_fog_setup_8007C624`), which gives one view either way.
- **Elements by player number**: three element callbacks read a player's physics car by number rather than through
  record 0 (element list head 0x8010C030: +0x28 callback, +0x2C parameter, +0x3C next). They are the gear box
  `hud_widget_cb_model_lookup_800B92F4` (player in bits 8-11), the tachometer's lit part `hud_widget_cb_orbit_icons_800B7E88`
  (manual only) and its background `hud_widget_cb_race_time_display_800B7B1C`, which also formats the race time
  (player in the low nibble). Once per element list (`hud_built`, again after a RESTART) player 1's elements are set to
  this machine's player and player 2's to 15. Each element shows itself only while its player is below the view count,
  which hooks replace with this machine's player + 1 (`rush2_online_hud_views` at 0x800B9358, 0x800B7B64 and 0x800B7E9C).
  The race time's digits go to 0x80240720 + player * 8, where player 1's time widget reads only player 1's, so
  `rush2_online_hud_time_digits` (0x800B7CA0) copies this machine's player's digits there.

players4's HUD clones for players 3 and 4
and its finish overlay count one shown player online (`shown_players`). The engine sounds can't be moved the same
way: `car_sound_update_all_80067358` (in the physics tick) writes the player cars' +0x660, which
`car_physics_step_80071D78` reads, so they stay player 1's.

The rumble is each machine's own player's. `physics_car_tick_80075880` writes a human car's rumble level to its
record's port (0x800D50E8 + port, at 0x80075AF8 and 0x80075C08), which `rumble_update_80063E4C` plays on that port's
Rumble Pak, and Enhanced rumble (src/rumble.cpp) reads the port the same way. Online every record has no port (5), so
nothing rumbled. `rush2_online_rumble_port` (hooks at 0x80075AEC and 0x80075C00) sends this machine's player's level
to port 0, whose controller drives them, and leaves the other players' on none.

## Eight players

Rush 2 keeps two player records (0x800C2140, 0x28 each); players4 moved the string table after them for records 2-3.
For 8, `tools/players4/relocate.py` also moves the globals at 0x800C2268-0x800C2280 (record 7 ends at 0x800C2280),
the lap timing byte per player at 0x800C2274 (to an 8-byte copy), the place widget byte per player 0x800BF318 and the
HUD bytes 0x8010C070, 0x8010C0B4 and 0x8010C0D4, and its per-player HUD arrays hold eight. `tools/players4/triage.py
--entries 8` lists what is left; the rest is per view (one online), per car (eight already), menus, the post-race
records (skipped online) or the sound and rumble, which read players4's count clamped to 2 (1 online).

`car_race_start_pose_setup_8009A264` picks a value per car type from a table of 22-byte rows at 0x800C0D68 (moved for
the 2049 cars), row 0 for computer cars and the player number + 1 for human cars. Rows 1 and 2 (players 1 and 2) are
all zeros; rows 3 and up are whatever follows, which crashed a race with 6 or more players (and could in 3 and 4 player
races, by car type). A hook at 0x8009A41C gives players 3 and up player 1's row.

`RUSH2_ONLINE_FAKE` (test aids) fills a lobby to 8 on one machine.

## Stunt and battle

The race start turns a race (type 0) on track 11, Rush 2's stunt arena, into stunt mode (race type 0x8010C3E8 = 2,
0x800D5751 = 1,
`race_start_timer_800AE670` 0x800AE75C-0x800AE7A8); the Rush 2049 stunt and battle arenas (track select ids 25-28 and
30-37) are hosted there the same way (`rush2_track49_stunt_mode`, `enter_race` in `src/rush2049/track2049_menu.cpp`).
So an online stunt or battle race is set up like any race with that track. Setting race type 2 directly left the
stunt flag clear and the car stuck.

Battles run C++ state of their own (`src/rush2049/battle.cpp`, `battle_ai.cpp`). Online (`rush2::battle::
set_online_seed`): both of its random number generators start from the race seed at each race setup, the computer
cars wait for their navigation grid (built on a thread of its own, so ready on a different frame on each machine)
before driving, the host's POINTS and TIME LIMIT are used, and the weapon buttons travel with the inputs
(`rush2::online::game::battle_buttons`). The Rush 2049 and SF Rush tracks need every player to have that game's ROM
(the roster's `games` bits; the host can't start otherwise). Online battles haven't been run yet (no Rush 2049 ROM in
the test setup).

## Gameplay settings

Every machine needs the same settings that change the race: General's Accurate Car Stats, Torque Rebalance, Respawn
in 1st Gear and Disable Drone Cheating, and all of the Games, Cheats and AI Opponents settings (`gameplay_configs` in
`src/online_tab.cpp`). The host's are the lobby's. Each player sends theirs ("config/option=value" lines,
`set_local_gameplay`), and a joiner's game takes the host's (`rush2::online_tab::follow_host_settings`, from the
lobby's frame): in memory only, with the joiner's own kept in players.json's `online_backup` section first. They come
back (and are written) on leaving the lobby, or at the next start if the game closed in one. Changes wait while a
menu is open, since the configs are the UI's then, and never happen mid-race; the host's race start waits for any game
that hasn't taken them yet. The joiner's own BATTLE row and battle rules are shown as the host's on the track select
and put back after.

## Race end

At game state 6 (the post-race), or game state 0 after the pause menu's QUIT, every machine leaves the lockstep on the
same frame, and the race goes on as a local one for this machine's player: the setup's snapshot of the records, menu settings and view table is put back, record 0
(the player's, with the profile and controller they picked) takes their car, and the player and view counts are 1.
The post-race then saves their stats and times to their profile and the track's tables and runs their name entry, as
after a local race; the other players' results are theirs to save. After QUIT the car table goes back to the menus' too.
When the front end comes back up, each machine sets 0x800FAE6C = 2 so it goes on to the track select with the same
player, where the host leads the next race. Everyone's progress starts over then. Only the host can QUIT; a joiner
leaves a race by leaving the lobby.

The menus take a controller port's presses only while its `input_port_state_80125AC4` byte is set:
`player_join_init_all_80080788` ORs each enabled port's pressed buttons into 0x8011962A and its repeat buttons into
0x80125A68, which the menus read (the track select: 0x9000 A or START, 0x4000 B, 0x2300 left/right). Opening the pause
menu clears all four (`input_set_port_state_80093DC0(-1, 0)` at 0x800AFE60) and sets back the ports of records 0 and 1,
which online are 5 (no port; the function ignores ports of 4 and up). After any pause every port stayed off, and the
track select the race left for took no buttons (the softlock after QUIT). Leaving the race (`finish_race_locally`,
`end_race`) sets the menus' records' ports back on (`enable_menu_ports`).

A race that ends early (the host leaves, a desync) leaves as the pause menu's Quit does (`race_end_to_menu_800AAC64`
with 0x800C1A9C = 0; 1 restarts the race), with 0x800BF31C = 1 so the front end starts over from the boot logos (screen
0xE) and the title screen instead of the car select (0xF). The front end loop (`menu_frontend_loop_800AE288`) switches
to the next screen (0x800E7BB4) when it differs from the one up (0x800E7BC4, -1 none).

## Test aids

| Variable | What |
|---|---|
| `RUSH2_ONLINE_TEST=host:N` / `join:ADDRESS` | Quick races: host and start from the title screen once N players are in, skipping the menus / join. |
| `RUSH2_ONLINE_TEST=menus:host` / `menus:join:ADDRESS` | Only host or join, for going through the menus (with `--input-script`). |
| `RUSH2_ONLINE_NAME`, `RUSH2_ONLINE_TRACK`, `RUSH2_ONLINE_RACE_TYPE` | Name; a quick race's track select id and race type. |
| `RUSH2_TEST_FINISH=S` | Finishes the players' cars S seconds of game clock into the race (src/players4.cpp), for the race end. |
| `RUSH2_ONLINE_FAKE=K` | The host fills K more slots with players who aren't there (no input). |
| `RUSH2_ONLINE_BOT=SEED`, `RUSH2_ONLINE_BOT_IDLE=F` | Drive from the frame number (repeatable runs), after F frames. |
| `RUSH2_ONLINE_SEED` | A fixed race seed. |
| `RUSH2_ONLINE_HASHLOG` | Log the state hash every 30 frames. |
| `RUSH2_ONLINE_TRACE=DIR` | Write the cars' physics before every frame to DIR/cars.bin; `tools/online_trace_diff.py A B` finds the first frame and words that differ. |
| `RUSH2_ONLINE_DUMP=DIR`, `RUSH2_ONLINE_DUMP_AT=F,...` | Write the hashed state every 30 frames / all of RDRAM before frames F; `tools/ram_diff.py` compares two. |
| `RUSH2_ONLINE_VIEW_CAR=N` | Every machine's view follows car N (how the view dependence was found). |
| `RUSH2_ONLINE_CAR=T` | The car select sends car type T whatever was picked (a remote car of another type). |
| `RUSH2_ONLINE_MANUAL=1` | A quick race's player on this machine races with the manual gearbox. |
| `RUSH2_ONLINE_PORT=P` | Host on port P (join with ADDRESS:P), clear of a lobby already open on this machine. |
| `RUSH2_TEST_TRACK=ID` | The track select opens on that track (with `menus:host`, a custom track at 38 and up). |
| `RUSH2_TEST_VPAD=S:INPUT[:HOLD],...` | A virtual SDL controller pressing INPUT S seconds after launch, through the real controller path: port claim, bindings, Background Input (src/input.cpp). |
| `RUSH2_RAM_WATCH=ADDR:SIZE,...`, `RUSH2_RAM_DUMP=DIR` | Log changes of RAM values each frame / dump RDRAM on each change (`src/ram_watch.cpp`). |

Several instances on one machine: `tools/online_test.py OUT --times T,... --instance "name|ENV=...|input script"`
starts them from the one build, a few seconds apart, the host first, screenshots each window at the given times
without taking focus, and closes them after. `--hold name:START-END:KEY` holds a keyboard key on one window through the
real keyboard path. `--key name:T:KEY,...` taps keys on a window (ESC, F15 and the arrows drive the recomp menu, as in
tools/menu_shot.py) and `--close name:T` closes one and prints how long it takes to exit. `tools/gen_disasm.py` writes `analysis/out_disasm/r2.asm`
for the players4 tools. docs/menus.md has the input scripts that go through every menu and the pause menu's QUIT.
The instances play on build/saves, the real save: back it up before a run that reaches Select Player, check it with
`tools/pak_check.py` after, and run one lobby at a time.

## Two games on one machine

With Background Input off, a game whose window isn't focused leaves the controllers alone (`hands_off`,
src/input.cpp): SDL drops their presses, and the game reads no input, claims no port and sends them nothing (rumble,
trigger rumble, DualSense and Steam haptics; what it was playing stops once). Before, the unfocused game's race kept
writing rumble and haptics levels to the same controller the focused game was driving with (SDL skips a rumble value
that didn't change, so the menus sent next to nothing and the race a stream). The game side was checked with both
machines driving through the menus with `--input-script` (105 and 110 MPH), and the real controller path with a
virtual pad in each game (`RUSH2_TEST_VPAD`, Background Input on: 119 and 113 MPH).

Open: one physical controller shared by two games still loses race input when switching windows (a keyboard in one
game and the controller in the other works). The cause isn't found yet.

## Known limitations

- The engine sounds follow player 1's car on every machine (see one view per machine).
- The battle's own state isn't in the desync hash (a difference shows once it moves a car).
