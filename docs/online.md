# Online play

Up to 8 players race, up to four on each machine in split screen, on a LAN or over the internet by the host's
address. There is no server: one player hosts and the others connect to them. Every machine runs the whole race itself from the same inputs
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
(`is_context_capturing_input`, `is_prompt_open`), since showing a document focuses it. A joiner whose lobby is lost
(the host closed it or quit its game, or the connection dropped; `take_lobby_lost`, set by the net thread's disconnect
and not by leaving from the Online tab) gets the overlay for 8 s with the reason (`last_error`), titled "Online", in
the lobby or in a race. Checked by quitting the host from a `menus:` lobby. Each player is shown by the name
of the profile they picked on Select Player (`set_profile_name`, from `rush2::collectibles::player_name`), or their
display name before that or without one.

The host publishes where it is every frame (`MenuState`: phase, the race settings as its menus have them, its track
select's cursor). Each machine's progress comes from where its own game is, never from what it was told, so a player
who backs out or joins late is waited for, and nobody waits on a step that already happened.

- **Waiting**: while the host is anywhere else in its menus, the others wait on the title screen (screen 0x10, its
  attract demo held off by keeping 0x8010C034 at the game clock 0x80117488; a demo that started anyway is ended with
  START) with their controllers off.
- **Select Player**: when the host chooses One Race, Circuit, Stunt or Battle on Start Game, each other game gets there as the
  host did: START on the title screen (which makes its controller player 1, `menu_title_start_pressed_800A6B58`), the
  same choice on Start Game, A. The choice is sent as what it is (`rush2::track2049::StartKind`), not as the row
  (0x80023064): the menu's rows differ by machine (BATTLE needs the Rush 2049 tracks, UNLOCKS the unlock system;
  `start_menu_row`). Each player picks their own player, and any screen Select Player opens is theirs too. Players 2-4
  of a machine join there with START as in a local race (src/players4.cpp). Select Player leaves the front end for the
  track select by setting 0x800FAE6C = 2 (`menu_frontend_loop_800AE288` then leaves the screen and sets game state 1).
  The host is never held: Practice, Ghost Race and the profile screens keep the others in the lobby, and the host's
  track select tells it to back out. Past Start Game a circuit is told by the race type (0x8010C3E8 = 1), since the
  circuit menu keeps its cursor in the Start Game row, and the other kinds by what Start Game chose
  (`rush2::track2049::chosen_start_kind`, set in `rush2_mode_menu_choose`), not by the row: 0x80023064 is the front
  end's shared cursor, and leaving Start Game (`func_803B12C8`) pushes it onto a stack of the screens' rows (depth
  0x80023066, rows 0x80023068) and zeroes it. Read from the row, the host's Select Player published One Race for a
  Stunt or Battle, the joiners chose One Race, and their race selects showed the race tracks' dioramas under the
  host's arena's name. A joiner whose choice isn't the host's (the host went back and chose another) backs out of
  its track select and Select Player to Start Game and chooses again.
- **Circuit menu** (circuits): Select Player goes on to it (screen 0xA, `menu_screen_circuit_menu_803B7018`: CONTINUE,
  ENTER CODE, NEW CIRCUIT, VIEW, which opens the standings, screen 0xB). The host's choice is the circuit; the others
  wait on theirs with their controllers off and, once the host is on its track select, take the host's circuit
  (Circuits below) and go on as CONTINUE does (0x800FAE6C = 2).
- **Track select**: the host's. It browses freely but its A and START only work once every player is on their track
  select, every player has the game ROM the track needs, and it has been up 20 frames. The others' track selects show
  the host's every frame with their controllers off: the track (0x8010C3F0), the carousel entry showing it (s16
  0x803D05B4, found in the carousel at 0x80310800 by `carousel_index`), the settings (0x800D5760), the option cursor
  (0x803D05BC) and top row (0x803D05C4), and the BATTLE row and battle rules (put back after). The carousel moves
  one place per change of the shown entry: each frame `trackselect_init_draw_803AB294` (0x803ABB1C-0x803ABCC4) shifts
  an entry's target by 100 when its place relative to the shown one (+0x14) changed, whatever the change, so a jump of
  several entries left the dioramas out of line with the track. The shown entry steps one entry a frame toward the
  host's, the shorter way round (carousel length s16 0x803D05AE). A joiner's track select
  offers every track that machine has, whatever its profile unlocked (`set_all_tracks_open`), so the host's is on it,
  but only those the host's own select offers: the host sends its carousel's tracks (`MenuState::offered_tracks`, from
  `offered_tracks`; custom tracks count as one, being numbered by machine) and the joiner's select keeps those
  (`set_online_offered`, rebuilt when the list changes as a game filter change rebuilds it). So the host's unlocks
  and game filter apply on every machine and both carousels step through the same entries; before, a joiner's
  carousel held tracks the host's filter hid and scrolled through all of them as the host moved one place. A list
  with no track of the select's kind (the host's last select was another) is ignored.
  A custom track's select id (38 and up) is the machine's own numbering, so the host sends it as its key
  (`RaceSettings::mod_track`, `mod_id/folder#hash` from `rush2::track_mod::identity`, the hash of every file the
  conversion reads) and each machine looks its own id up (`track_for_online_key`). Each machine reports whether it has
  the track the host is on (`set_track_check`: the key's FNV hash and a "lacks" flag, sent with its progress); the host
  can't confirm until everyone has answered for that track and has it, and it names who doesn't. A joiner without it
  keeps its own track on screen and says so. A player who joins while the host is already on the car select stays on
  their track select, and the race doesn't start until the host goes back and picks a track they have.
  When the host goes on to the car select, theirs are pressed A. One that is for another kind of race than the host's
  (the host went back and chose another) backs out and comes in again.
- **Car select**: everyone picks their own car (a machine with 3 or 4 players in two rounds, as locally). A machine
  whose players all have sends them together (`set_cars`: name, car, profile and Rush 2049 car options each), and the
  host gives each a slot of the lobby (the extra ones named after their profile). A player who has stays on the screen (the car select's
  everyone-picked check, `menu_screen_car_select_frame_803B9478` at 0x803B96D4, through src/players4.cpp's hook; its
  flag in 0x803CB39C set) with B only. The host's B takes everyone back to the track select as in a local race; a
  joiner's B takes them alone to their track select (`car_select_back` lets a player who picked leave), which presses A
  again while the host's car select is up, so they pick again: whatever holds the race up, the player can change their
  car. Why the host's race can't start yet (`start_race`'s error, `MenuState::start_error`) shows on every machine's
  overlay, so the player it names sees it. Once
  all have, the host starts the race; each machine sets it up and starts it as the car select does
  (0x803B96D4-0x803B96F4: `music_queue_stop_80062FA4`, game state 7, `carselect_build_list_803B81F0(0)`,
  `overlay_load_800A62C0(0)`). A joiner who backed out to change car just as the host started is on the track select
  when the race comes: its A takes it back to the car select, where the race starts once the screen has been up 15
  frames (the race start from the track select, `leave_front_end`, would leave the screen up). A Battle needs the Rush
  2049 ROM for its Start Game row: the host is told who lacks it.

The players' controllers are steered at the game's controller read (`get_game_input` in main.cpp): where the players
choose (Select Player, the car select) every port is theirs; elsewhere every port is masked (no A and START, B only)
or off, or port 1 is pressed for player 1 and the others read nothing. Everything the race needs (race type, track, the track select's
settings, each player's car and profile) goes to every machine with the race start, so the race is the same whatever
the menus showed.

## LAN lobbies

A hosting machine broadcasts a beacon ("R2OL", lobby name, players, settings, compatibility key) to 255.255.255.255 and
127.0.0.1 on port 47101 every second. `lan_lobbies()` lists those heard in the last few seconds; a lobby with another
compatibility key (another build) is listed but can't be joined. A lobby that is racing can be joined: the player waits
in the lobby for the next race (their slot isn't in the race's roster, `race_slots`, so the race neither waits for nor
checks them; `RosterEntry::in_race` tells the Online tab who is racing). So a player who left or was dropped can come
straight back. When the lobby closes the host sends one more beacon
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
  (0x6C0 bytes, with the car; `set_cars`), and the race start carries them all. This machine's players race with their
  own profiles, so the keys they take and the post-race save to them; the others race with copies at 0x80E10000 +
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
- **Car parts**: `car_state_draw_objects_80082D14` runs the cars' part animations (car state +0x108..+0x2FF; the
  callbacks `carselect_preview_cb_d/e` step car state bytes and draw from the race's seed 0x800BD808 through
  `car_preview_part_random_hide_80059450`). Two of its inputs are this machine's own: which parts run this frame
  comes from the low two bits of the main loop's frame count since boot (0x8011962C, counted by
  `game_main_loop_frame_800B0228`), and whether they animate at all from the last frame's GPU time (0x80023358, set by
  `sched_thread_main_800009EC` from osGetTime, against the thresholds 0x800CFA10-0x800CFA18) into 0x8011748C. With 4+4
  players the race went out of sync at frame 900 on most runs (the first part to animate). Online the count is the
  lockstep frame and 0x8011748C is 1 (hook at 0x80082E64, `rush2_online_draw_detail`), as on a machine that keeps up.
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
  and the seeds; if they differ, the first frame starts from the host's (Resyncs). Left out are what differs by machine: the records' profile
  pointers, the car race state's camera bytes (+0x347-+0x349) and its scene node handles (+0xF0 the body's, +0x8 of
  each 0x1C byte part object from +0x108): the race setup allocates nodes for each view, so with two players on a
  machine every node was one further on. Every 30 frames they compare the same hash; a difference resyncs the race
  (Resyncs). A player whose input doesn't come for 30 s is dropped: their car coasts (Connections and timeouts).
- **Waiting**: while a frame's inputs are late by over a second, the lobby overlay comes up naming who it waits for
  (`race_status`, `race_notice`); it goes when they arrive.
- **Pause**: any player's START pauses everyone's race. `race_pause_menu_step_800AFD44` reads every player's presses,
  which are the lockstep's, so the menu runs the same on every machine; the frame's physics ticks are skipped while
  0x800201E4 is clear (the pause menu clears it, and a RESTART's loading). Its other way to pause, a player's port
  reporting bit 8 (0x8010C150[player], the pad at 0x80023044 + 6 * player: this machine's controllers), is skipped
  online (hook at 0x800AFDE0, `rush2_online_pause_start_only`). OPTIONS, CONTROLS, AUDIO and SKIP TRACK change one
  machine's own settings, so online the cursor moves over CONTINUE, RESTART and QUIT only (`src/pause_menu.cpp`).
  RESTART restarts the race in lockstep; QUIT ends it (race end).

## Resyncs

Whatever makes the machines' races differ, the race goes on. The state the machines compare is also what a resync
copies, so the hash and the copy can't drift apart:

- `synced_ranges` in online_game.cpp: the eight physics cars 0x800F5470, the race players' records but for the profile
  pointer, the cars' race state 0x801124A0 but for the machine's own bytes listed under Checks, and the two seeds;
- `race_globals`: the race's variables beyond the cars, as `tools/rush2049/fsum.py --reach 0x80076D70 0x800A3050
  --stop 0x800A06F8 0x80062F50 0x8009E0C4` lists what the physics tick and the frame's race update write (less the
  drawing, text and sound ones): the race clock the drones steer by (`timer_game_time` 0x80117488), the checkpoint
  clock (`race_time_float` 0x8010C204), the drones' path node and choice, the physics counters and substeps, the
  checkpoint flags, and the race's copy of the path header (0x8010BCE8, 0x32C bytes: the start time, the checkpoint
  count and the 10 checkpoints with their time extensions and crossings). Without them a resync copied the cars but
  not, say, which drone path node was being steered for, and the machines parted again within a second;
- the race state the port's C++ keeps outside RDRAM (`sync_parts`, include/online_sync.h): the Rush 2049 wings,
  California Speed's physics, SF Rush's buses, the Rush 2049 moving objects (with the collision polygons and vertices
  they move), the Rush 2049 stunt scoring, the battle (fighters, pickups, shots, its random numbers and clock) and the
  battle's computer cars. Each module lists its fields once, in a walk taking a `Walker` (its `online_sync`): the same
  walk saves them (for the hash and the host's snapshot) and loads them (a resync). What one machine draws or plays for
  its own players (views, sounds, the HUD) stays out. A part that differs takes the host's whole.

- When the hashes for a frame differ (every 30 frames), or the machines loaded different starts, the host marks the
  next frame it hasn't broadcast (`schedule_resync`, `frame_resync` in its FrameInputs message): no machine has run it
  yet, since none runs a frame before the host broadcasts it. At the top of that frame, after its inputs, the host's
  game sends its state (`send_snapshot`, Msg::Snapshot) and every other game waits for it (`wait_snapshot`, the
  overlay saying "Resyncing the race with the host...") and takes it (`apply_state`). Every machine then runs the frame
  from the same state. Hashes of the frames before the resync are dropped. A load mismatch resyncs frame 0, so the race
  starts instead of being refused.
- `apply_state` keeps a word that is an RDRAM address on both machines but a different one (a pointer into memory laid
  out differently on that machine; copying it would point at the wrong thing). The hashed state normally holds none.
  A kept word stays different, so every later check fails again and the race is given up on: the car slots after the
  race's cars (`race_cars`, Load check) always take the host's words, pointers too. Each holds the car each machine
  raced there last, whose descriptor pointer (physics car +0: the type's shared descriptor, or its copy at
  0x80225000 + slot x 0xEC) differs whenever the machines' last races did. A user's 2-player LAN race, started once
  cars no longer failed the load check, resynced at every check on six such words and ended with "The race couldn't
  be kept in step". Reproduced with `RUSH2_ONLINE_BREAK=stale` on the joiner and the host's attract demo cars in its
  slots 2-7 (the Load check's run): 15 resyncs each listing "car 2..7 physics +0x000 ... (an address: kept)", then
  the user's message. With the fix the same run resyncs once at frame 0 and races to the end in sync.
- Each machine writes what differed to `online/desync_<date>_<time>.txt` in its data folder (one file per race; the
  host notes the frames it sent its state for): each differing field as "car N physics +0xOFF", "car N race state
  +0xOFF", "player N record +0xOFF", a race global's name (with +0xOFF in the path header) or the seed, with this
  machine's value and the host's (the first 200), then every difference counted by area ("by area: car 0 physics 126,
  ...") and each C++ part that differed. These are what to read to
  find a divergence's cause. Checked with `RUSH2_ONLINE_BREAK=nudge:300` (the joiner's seed changed at frame 300): a
  resync at frame 302, the log naming the seed, the hashes equal to frame 2010.
- Every `[online]` and `[lobby]` line (joins, race setups and loads, resyncs, drops, why a race ended or couldn't
  start) also goes to `online/session_<date>_<time>.txt`, one per game run, the last 20 kept
  (`rush2::online::session_log`), so a kick on a player's machine leaves a record without a console.
- Only when 15 checks in a row differ even after resyncs (the machines set up different races, which the lobby's
  checks prevent) does the race end, back to the lobby, naming the player and the logs.

A resync repairs a race that drifted, not one set up differently: a setting that differs by machine comes back after
every resync, and the log shows the same areas each time. What the race is set up from therefore comes from the host
on every machine (Gameplay settings), and what can't be repaired is checked at the load (Load check).

Wario Stadium (a custom track) with the track select's BATTLE row on resynced every 30 frames: the lobby showed the
host's BATTLE row only while in the track select and put the joiner's own back at the car select, so the host raced
with weapons and half as much time again on the clock (68 s against the track's 45) and the joiner without. The
desync log's first difference was `race_time_float` at frame 120, then every car's physics once the drones fought.

## Load check

When the race loads, each machine also reports two fingerprints (protocol 8, `report_loaded`): its track's collision
data (`track_fingerprint`: the collision file from its header at 0x800D5754 to the end of its VLIST section, which the
loader doesn't write, so the same track hashes alike wherever it sits) and what each car's physics read
(`cars_fingerprint`: `rush2::car2049::physics_hash` by the car's type, physics car +0x7EA, and table row). A resync can't
repair the track, so the host ends the race when it differs (Msg::LoadMismatch, `host_check_loaded` in online_net.cpp),
naming who differs (another version of a custom track, another copy of a game's ROM). `RUSH2_ONLINE_BREAK=track` tests
it.

The cars never end a race. A resync copies the data each car's physics run on as well
(`rush2::car2049::physics_online_sync`, the first sync part: physics_hash's table entries with the type's descriptor
index and pointer, the type's descriptor, the car's own descriptor at car +0, the four tire curves it points at and the
box), and any difference also differs the start hash, so frame 0 resyncs and the car takes the host's. The host builds
every player's rows from that player's own profile, so its copy is each player's own choices. The bytes it overwrote
are kept (`lent_physics`) and put back by `return_mod_slots`, so the machine's own races run on its own data again. The
cars' fingerprint is only logged: each machine's session log lists every car's type, row and physics hash at the load,
and the host's names a player whose cars differed.

The fingerprint and the sync part cover only the race's cars: the players (num_players 0x8010C3E2) and the computer
cars (s16 0x800D3E90), at most 8 (`race_cars`). A user's race ended with the old "set the cars up with different
physics" error with the same ROMs, mods and settings on both machines: the fingerprint covered all 8 physics car slots,
and a race with fewer cars leaves the rest holding what each machine raced last. Reproduced with two quick races
(`RUSH2_ONLINE_DRONES=0`, the host idling 45 s on the title so its attract demo filled slots 2-7, the joiner fresh): the
host's slots 2-7 held the demo's cars, the joiner's type 0, and the race ended with the user's message. With the count
the same run loads alike (2 players, 0 computer cars), 51 checks in sync. (`race_car_count` 0x800E79B8 is 0 at the
load: not the count.) Two other ways the cars could differ were closed too:

- The setup from a car select that only just came up (`Phase::WaitMenus` in online_game.cpp: a player walked back in
  as the host started) runs `carselect_build_list_803B81F0` for player 0, whose unlock filter (`limit_tires`,
  `limit_engines`) looked up race player 0's parts in this machine's purchases (another machine's player has no name
  here, so the guest's) and set bought ones back to stock. The limits now skip an online race
  (`rush2::online::game::in_race`). The usual start (`start_from_car_select`, every player having picked) doesn't run
  it: a host 8-Ball on a bought 5.0L HP V6 (`RUSH2_TEST_GUEST_UNLOCKS`) loaded alike on both machines before the fix.
- The computer cars' random types (`rush2_car49_drone`) include the Rush 2049 and California Speed cars only when every
  racer has that ROM (`rush2::online::game::race_has_game`), so every machine draws from the same pool.

## Mod cars

A mod's car (types 57-72, src/car_mods.cpp) is in the slot each machine's own mods put it in, and its type number is in
the race's state (physics car +0x7EA, `race_setup_cars_8009E6DC`), so a raced mod car must be in the same slot on every
machine. Each machine reports its mod cars (`RosterEntry::mod_cars`, `rush2::car_mods::online_car_checks`: a hash of
the mod id, the car id and its files, per slot), and a player who picks one sends its spec with the car
(`LocalPlayer::mod_spec`, `rush2::car2049::mod_car_spec`: the physics, base car and look settings of `ModCar`, not the
model). `setup_race` gives each raced mod car a slot, the same on every machine: its picker's, or the first free one
when another raced mod car already took that slot (two machines with different mods there); that player's car options
in the profile's side block (13 bytes per type) move with it, and they race with a profile copy. Then for each slot:

- this machine has the car there: nothing changes;
- it has the car in another slot: it copies its own (`copy_mod_car`, the model, textures and spec), so the car looks as
  it does on the picker's machine whatever the mod order;
- it hasn't the car: a stand-in (`set_stand_in_mod_car`): the sent spec's physics through `build_mod_physics`, as
  `set_mod_car` builds them, drawn as the spec's base Rush 2 car (its container from `rush2::assets::read_current`, its
  part name prefix from the copied name table entry, painted and striped as a Rush 2 car).

`lend_mod_slot` keeps what the slot held and `return_mod_slots` puts it back once the game is in the menus again (and
before the next race's setup), synchronously, so the next race never meets a half-restored slot. Checked with the
joiner reporting no mod cars (`RUSH2_ONLINE_MODCARS=none`), host CADDY (57), joiner DEUCE (58): the same start hash, the
joiner's DEUCE drawn as its base HOTROD, in sync to the end.

## Players per machine

A machine races with the players its car select had (1-4, src/players4.cpp's split screen). The lobby gives each a
slot (`RosterEntry::machine` is the machine's first slot, `local_index` the player's place on it), and the race puts
each machine's players on neighbouring cars: the race order is by machine, then by player (`race_order`, which the
frame's inputs are mapped through: they arrive by lobby slot). So this machine's players drive cars `local_player` to
`local_player + local_count - 1`, and each sends their own input (the input scripts and controllers of ports 1-4, or
`RUSH2_ONLINE_BOT` per player).

The race setup sets the view count to the player count (`race_setup_800A5ADC` at 0x800A5FCC, `sh` through $v0, so the
reference scan misses it); a hook just after (0x800A5FD0, `rush2_online_race_views`) sets it to this machine's player
count. View i follows this machine's player i's car (0x80240380, the view-to-car table players4 moved, read by
`func_800A3690` at setup).

The car's view (car state +0x347, -1 none) forks the simulation in two places, found by running the same race with
each machine viewing a different car (it parted at the first wreck):

- `car_wreck_steer_to_respawn_800754B4` at 0x800757AC: a wrecked car with a view has its velocity (+0x218) damped while
  it glides to the respawn point.
- `race_wreck_respawn_wait_8008E40C` at 0x8008E4C4: a car with a view takes the respawn camera path (camera mode
  +0x348 = 4, the view's camera state at 0x800E79D0 + view * 0x40 and 0x80240390 + view * 0x30, a sound, then +0x342 = 1
  and +0x104) with its own timing.

`rush2_online_sim_view` (hooks at 0x800757B0 and 0x8008E4C8) gives every human car without a view view 64, so every
machine takes the viewed path for every human car. That path plays the respawn sound through `menu_play_sound_80064908`,
which draws from the race's random seed 0x800BD808, so the paths must match. With up to four views no view below 4 is
spare; view 64's writes land at 0x80240FB4-0x80240FBF, unused RDRAM of players4's window. The other readers of +0x347
are camera, particles and the car selection preview.

The per-player sound and rumble effects run for the first players only: players4 points their count at
`clamped_players` (0x8024F000, min(players shown, 2)). They change the race too: `car_sound_update_all_80067358` steps
the cars' +0x660 (set to 1 or 3 by `car_physics_step_80071D78` on a shift), scores stunts
(`car_stunt_scoring_update_80065588`, whose per-player arrays at 0x800E7CB8-0x800E7E18 hold two players) and plays
sounds that draw from the race seed, and `car_material_effects_update_800663CC` sets the wheels' dust bits of +0x7F4.
A machine with two players stepped car 1's +0x660 and the others didn't (out of sync at frame 30). Online these
functions and `car_sound_update_events_80067D9C` and `car_sound_slots_init_8006722C` see 1 on every machine (hooks at
their entry and return, `rush2_online_sim_players_begin/end`; they call each other, so the count is put back after the
outermost).

The race's other human cars get the stunt scoring and the wheels' dust on every machine: after the loop (0x80067A88,
`rush2_online_extra_player_effects`) each runs `car_material_effects_update_800663CC` (its sounds play only for the
players the count covers) and `car_stunt_scoring_update_80065588` (player index in $a1), for a car whose engine runs
(+0x648 0) while the update's first argument is set and its second clear, as the loop does; any other car's scoring
state is cleared, as `car_sound_slots_init_8006722C` clears the first two players' through `stunt_param_store_800671E8`
(players 2 and up at its entry, `rush2_online_stunt_reset`). The scoring's per-player arrays held two players
(0x800E7CB8-0x800E7E20, the points u16 0x8010C074, the best u8 0x8010C068 and the multiplier 0x8010C0C0):
tools/players4/relocate.py moves them to copies for eight (their only users are the scoring, `car_helper_80065270`,
`stunt_param_store_800671E8`, the stunt HUD `hud_widget_text_cb_player_stats_800B7654` and the race-end stats
`record_stats_race_end_800A9250`). The game clears only the first two players' at a race start, so the race HUD's build
clears them all (`hud_built`). The stunt HUD shows players 0 to n-1 by index, so the presentation rotates its arrays as
it rotates the records (`stunt_rotate`), and the race end copies this machine's players' points and best to their
records' places for the post-race.

Rush 2 places a human car's body (its scene node, car state +0xF0) in the camera update's loop over the views, for the
car each view follows (`cam_update_dispatch_8009D02C` 0x8009D08C-0x8009D0F4: the car's position raised by the f32 at
0x800CFD84, and its matrix, car state +0x48, through `car_matrix_copy_and_apply_8009ABDC`), and the computer cars'
after it (`cam_update_all_players_8009D9F4` 0x8009DFD8-0x8009E084, skipping cars whose physics car +0x7E8 is 2). The
other machines' players' bodies stayed on the grid and only their shadows moved; at 0x8009E01C a human car without a
view is placed as a view places its car (`rush2_online_body_kind`). The same loop switches a view's camera on its
player's VIEW (record +0x1A against +2), taking view v's player as record v: online view v takes this machine's
player v's record (hook at 0x8009DA60, `rush2_online_view_record`). With two or more players every human car gets an
ARROW (`race_load_car_assets_80086CA4` 0x80086FF8-0x80087048) drawn in the other player's view (flags 0x200 | 1 << (1 -
car)) and sized from that view's camera (`car_arrow_update_80059B9C` at 0x80059BF4, view car ^ 1); online a car's
arrow is drawn in this machine's views other than its own and sized from the first (`rush2_online_arrow_views`,
`rush2_online_arrow_view`).

The wheel placer (`carselect_preview_update_8005A598`, per car per frame) shows a car's wheel nodes through
`node_swap_visibility_8005A528`: for a car with a view it hides them in that view's close cameras, and for a car
without one (`node_reset_visibility_800590AC`, which also reads $s0) it shows them in every view only when $s0 (car
state +0x348 >= 2) is set; the hook at 0x8005A770 sets it for a viewless human car (`rush2_online_wheels_shown`).

The per-player car type tables at 0x80200000 (36 types a row; row 0 the computer cars, row player + 1 a human car's,
by the physics car's +0x7E0) hold what the car select took from each player's profile: the car's stats (weight
0x80200418, ENGINE 0x80201348, torque 0x80201490, suspension 0x80201720, handling 0x80201868, which
`car_init_setup_8008DBA0` sets the physics from) and its looks (colors 0x80201C40-0x80202018, rims 0x802019B0, wheel
scales 0x80200928 front and 0x80200E38 rear, read by the wheel placer at 0x8005AB78-0x8005AC2C). The car select fills
the rows of this machine's players only (`carselect_build_list_803B81F0`, 0x803B8B10-0x803B8F7C: 13 getters,
0x800B2424-0x800B2694, per car type). So every car of another machine's player raced with this machine's player 1's
settings, and was drawn with wheels of size 0. The race setup fills all 13 for every race player from the profile the
race uses (`fill_player_rows`, which calls the getters with that record's profile). The tables have 9 rows (8 players),
moved there by tools/rush2049/cartypes.py (`NEW_PLAYER_ROWS`). A Rush 2049 car's options (ENGINE level and the like)
are in a side slot of its profile (src/rush2049/car2049.cpp): each player's car select sends theirs
(`rush2::car2049::side_block`), and the other machines' players race with them in guest side slots
(`car_types::guest_side_slots`, `set_guest_side_block`, 0x2A0 bytes each; protocol version 5 added California Speed's
types 36-56, version 6 the mod cars' 57-72, which race online as Mod cars below says).

The HUD and the frame's drawing are records 0 to n-1's. Around them (`race_start_timer_800AE670` 0x800AF1D4 to
0x800AF1F4) the records' cars rotate by `local_player`, so record i holds this machine's player i's car, and those
records take controller ports 0 to n-1 (`rush2_online_present_begin/end`); nothing in the race runs in between.

### The HUD online

A networked race's HUD looks like a local race's with this machine's players. The HUD is built for the race's
player count, so a joiner first got player 2's gear box (manual or not) and a second race time under player 1's. Two
things fix it:

- **Player count**: around the HUD build and update (`src/hud.cpp`, `hud_players_begin/end`) the player count
  0x8010C3E2 reads this machine's player count. The per-player elements show themselves by it: the race time
  (`hud_widget_cb_icon_row_a_800B9754`), the lap time (0x800B9978), the deaths (0x800B9C44) and the lap banner
  (0x800BA868). Nothing else there reads it but the view fog (`view_fog_setup_8007C624`, rows for 1 and 2 players,
  which players4 clamps).
- **Elements by player number**: three element callbacks read a player's physics car by number rather than through
  record 0 (element list head 0x8010C030: +0x28 callback, +0x2C parameter, +0x3C next). They are the gear box
  `hud_widget_cb_model_lookup_800B92F4` (player in bits 8-11), the tachometer's lit part `hud_widget_cb_orbit_icons_800B7E88`
  (manual only) and its background `hud_widget_cb_race_time_display_800B7B1C`, which also formats the race time
  (player in the low nibble). Once per element list (`hud_built`, again after a RESTART) the elements of players 1 to n
(this machine's views, players 3 and 4's cloned by players4) are set to this machine's players' cars and the others to
15. Each element shows itself only while its player is below the view count, which hooks replace with
`local_player + local_count` (`rush2_online_hud_views` at 0x800B9358, 0x800B7B64 and 0x800B7E9C). The race time's
digits go to 0x80240720 + player * 8, where view i's time widget reads player i's, so
`rush2_online_hud_time_digits` (0x800B7CA0) copies this machine's player i's digits to view i's place.

players4's HUD clones for players 3 and 4 and its finish overlay count this machine's players online
(`shown_players`, `rush2::online::game::local_race_players`).

The rumble is each machine's own players'. `physics_car_tick_80075880` writes a human car's rumble level to its
record's port (0x800D50E8 + port, at 0x80075AF8 and 0x80075C08), which `rumble_update_80063E4C` plays on that port's
Rumble Pak, and Enhanced rumble (src/rumble.cpp) reads the port the same way. Online every record has no port (5), so
nothing rumbled. `rush2_online_rumble_port` (hooks at 0x80075AEC and 0x80075C00) sends this machine's player i's level
to port i, whose controller drives them, and leaves the other players' on none.

### Engine sounds

Rush 2 has two engine voices, for local players 1 and 2 (src/engine_preview.cpp). The car setup
(`race_setup_cars_8009E6DC`, car index $s4) starts voice 0 for player 1's car and voice 1 for every other player's
(`race_start_player_engine_8009E664`, $a0 the voice), and each physics tick hands the voices their cars' rpm and load
(`physics_stage_post_snapshot_8006890C`: `car_engine_sound_set_rpm_load_800650DC` with record 0's car at 0x80068AD4
and record 1's at 0x80068B3C). Online those are the first two machines' players on every machine. `rush2_online_engine_start`
starts voice i for this machine's player i's car instead, with the engine the setup picked for that car (0x800E7BBC +
voice * 2), and `rush2_online_engine_car` feeds the voices from those cars. This is only the sound: it writes the
voices' rpm and load and the engine sound globals 0x800E7BB2 and 0x800E7BC8, which only the sound reads. Players 3 and
4 of a machine and the other machines' players are played by src/car_engines.cpp (their engines play when the Other
Cars' Engines option is on, as computer cars' do; a machine's own players 3 and 4 always), and a Rush 2049 car's
voice plays Rush 2049's engine layers (src/rush2049/engine2049.cpp, `rush2::online::game::engine_slot_car`).

## Eight players

Rush 2 keeps two player records (0x800C2140, 0x28 each); players4 moved the string table after them for records 2-3.
For 8, `tools/players4/relocate.py` also moves the globals at 0x800C2268-0x800C2280 (record 7 ends at 0x800C2280),
the lap timing byte per player at 0x800C2274 (to an 8-byte copy), the place widget byte per player 0x800BF318 and the
HUD bytes 0x8010C070, 0x8010C0B4 and 0x8010C0D4, and its per-player HUD arrays hold eight. `tools/players4/triage.py
--entries 8` lists what is left; the rest is per view (four at most), per car (eight already), menus, the post-race
records (skipped online) or the sound and rumble, which read players4's count clamped to 2 (1 online, Players per
machine). The per-player car type tables have rows for 8 players (Players per machine).

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

The Rush 2049 stunt scoring (`src/rush2049/stunt2049.cpp`) keeps each race player's score (players are the race's
cars 0 and up). Its HUD and Rush 2's stunt panel show a view's player, so a view reads its race player's score
(`view_player`: `local_car(view)` online). They read view i's score before, so a joiner's HUD showed the host's
points and its own scored nowhere. The HUD also no longer writes into the score (2049's overlay copies the counts and
points and counts down the "BONUS X n" flashes there): the score is checked and resynced, and each machine draws only
its own players, so those are the HUD's own (`HudPlayer::kinds_flashed`).

Battles run C++ state of their own (`src/rush2049/battle.cpp`, `battle_ai.cpp`). Online (`rush2::battle::
set_online_seed`): both of its random number generators start from the race seed at each race setup, the computer
cars wait for their navigation grid (built on a thread of its own, so ready on a different frame on each machine)
before driving, while its build is still running (`nav_building`: a build that ends without a grid, an arena with no
usable triangles, doesn't hold the race), the host's POINTS and TIME LIMIT are used, and the weapon buttons travel
with the inputs (`rush2::online::game::battle_buttons`). The arena's health bar is this machine's player's car
(`hud_car`: car slot `local_race_player`, not record 0's, which is theirs only inside the presentation). The Rush 2049
and SF Rush tracks need every player to have that game's ROM (the roster's `games` bits, which say only that the ROM is
there: the Games tab options that turn its tracks and cars on follow the host's with the other gameplay settings; the
host can't start otherwise), and so does a Rush 2049 car (types 23-35) any player picked (`start_race`, checked once
every car is picked); a machine that can't load one
anyway gives that car the Pickup and the load check ends the race. The results boxes give each view its race player
number (a machine with players 4-7 shows PLAYER 4 to PLAYER 7), as the winner's line does.

A round won on points ended only offline: the game's clock test (`race_start_timer_800AE670` at 0x800AEDC4) clears
the out of time flag 0x800FAE98 in every frame with time left, and offline the physics thread's tick happened to set
it again before the race end read it; online the ticks run before the test, and the race stayed on the results. The
round's winning car now also runs the stunt clock out (`time_allowed` = the time raced), so the race ends as at the
time limit. Checked with three games of 1, 2 and 4 players (7 cars, Rush 2049 cars, `R2_BATTLE_TEST=winkills`): in
sync, the race over on the same frame, back to the menus.

## Circuits

A circuit's state is the profile's circuit block (0x60 bytes, pointer 0x800D3E40: +1 flags, difficulty and deaths, +2
races finished, +4 seed, +8 the cars' places; docs/rush2049_research/menus.md#9-circuit-results-and-the-victory-screen-v),
its races (0x800D3A60, 4 bytes each: track, direction, fog, wind), the standings (0x800D3B28, 0x58 bytes a car:
places, points, the computer cars' skill), the race index 0x800D3DF0 and the length (src/circuit.cpp). The host races
its own profile's circuit and sends it all (`RaceSettings::circuit`, `rush2::online::game::read_circuit`) with its
menu state and the race start. Every other machine copies it into a block of its own at 0x80E15600
(`follow_circuit`), so its profile's saved circuit is never touched: the circuit menu points 0x800D3E40 at the
profile's block as it comes up (`menu_screen_circuit_menu_803B7018` at 0x803B710C), and a hook just after
(0x803B7110, `rush2_online_circuit_block`) points it at the copy instead. The race's difficulty and deaths come from
the block (`menu_settings_to_race_globals_80094698`), so each race start sends the host's again. The races of the copy
are the host's ids; the current one is this machine's id for a custom track (`mod_track`).

After each race every machine's post-race scores it into its block (`save_write_settings_to_pak_800A8D1C`, the cars'
places, the same on every machine). That function points 0x800D3E40 at the profile's block again first (record
0x800D39D2's profile + players * 0x60 + 0x464), which wrote the host's results into a joiner's saved circuit; a hook
at 0x800A8D8C (`rush2_online_circuit_results`) points it at the copy, as one at 0x800A7E40 does for
`circuit_generate_800A7DCC`, which does the same (into $s7). Only the host starts a circuit or steps its DEATHS
(`func_800B2A58`, `func_800B29DC`, which work out the profile's block too). The post-race then makes the next race, from its own course choices (`rush2_track49_circuit`), so
the track select shows the host's (`follow_track_select`). A joiner's circuit length is the host's
(`rush2::circuit::length`, `circuit_length`), and its saves file gets no record of the host's circuit
(`rush2_circuit_race_car`). Once the last race is run, every machine goes on to its victory screen (screen 0xC) instead
of the track select, where a joiner's controller is theirs until START.

## Gameplay settings

Every machine needs the same settings that change the race: General's Accurate Car Stats, Torque Rebalance, Respawn
in 1st Gear and Disable Drone Cheating, and all of the Games, Cheats and AI Opponents settings (`gameplay_configs` in
`src/online_tab.cpp`). The host's are the lobby's. Each player sends theirs ("config/option=value" lines,
`set_local_gameplay`), and a joiner's game takes the host's (`rush2::online_tab::follow_host_settings`, from the
lobby's frame): in memory only, with the joiner's own kept in players.json's `online_backup` section first. They come
back (and are written) on leaving the lobby, or at the next start if the game closed in one. Changes wait while a
menu is open, since the configs are the UI's then, and never happen mid-race; the host's race start waits for any game
that hasn't taken them yet. The joiner's own BATTLE row and battle rules are shown as the host's on the track select
and put back after; the race itself takes the host's BATTLE row (`setup_race`) and puts the machine's own back when it
ends. With the Rush 2049 tracks on, that row arms the race, so it then needs every player to have the Rush 2049 ROM
(`RaceSettings::needs_games`, which the lobby names), and a ghost race chosen offline before is cleared (it turns the
row off).

Other inputs to the race that differ by machine are made the same online:

- Keys and coins: the game puts a key out unless every racer's profile has it (`race_key_all_racers_have_8005F00C`),
  and each machine knows only its own players' profiles. Online every key is out (`rush2_collect_taken`); taking one
  still counts for this machine's players.
- The Rush 2049 wings: every race player's car flies with the wing style its player picked (`CarChoice::wing_style`,
  `rush2::online::game::wing_style`), cars 5-8 included.
- Controller combos read by port (`button_held` in src/controls_menu.cpp) ignore the port in an online race.

## Race end

At game state 6 (the post-race), or game state 0 after the pause menu's QUIT, every machine leaves the lockstep on the
same frame, and the race goes on as a local one for this machine's players: the setup's snapshot of the records, menu
settings and view table is put back, records 0 to n-1 (the players', with the profiles and controllers they picked)
take their cars, and the player and view counts are this machine's, two at most (the post-race and the menus have
per-player and per-view arrays of two, as for a local race of 3 or 4, src/players4.cpp; with 4 the menus' sky,
`render_sky_cloud_scroll_8008918C`, read view 2's matrix pointer from other data and crashed). The player count
`race_setup_800A5ADC` saved at 0x800A628C (0x800E7BB8, the race's online) is set to the same: after the post-race
`menu_title_start_pressed_800A6B58` gives the menus that many players, with the ports saved at 0x800BF314 (port 0 for
a player who has none), so every machine came back from a race with as many players as the race had. The post-race then saves their stats and times to
their profiles and the track's tables and runs their name entry, as after a local race; the other players' results
are theirs to save. A player without a profile (a test aid's race, started from the title screen) gets their race copy,
which isn't saved: `record_stats_race_end_800A9250` counts the race in record +0x24's profile, and a null one was a
crash. After QUIT the car table goes back to the menus' too.
When the front end comes back up, each machine sets 0x800FAE6C = 2 so it goes on to the track select with the same
players, where the host leads the next race (a circuit's next race; a finished circuit goes on to its victory screen
instead, Circuits). Everyone's progress starts over then. Only the host can QUIT; a joiner
leaves a race by leaving the lobby.

The menus take a controller port's presses only while its `input_port_state_80125AC4` byte is set:
`player_join_init_all_80080788` ORs each enabled port's pressed buttons into 0x8011962A and its repeat buttons into
0x80125A68, which the menus read (the track select: 0x9000 A or START, 0x4000 B, 0x2300 left/right). Opening the pause
menu clears all four (`input_set_port_state_80093DC0(-1, 0)` at 0x800AFE60) and sets back the ports of records 0 and 1,
which online are 5 (no port; the function ignores ports of 4 and up). After any pause every port stayed off, and the
track select the race left for took no buttons (the softlock after QUIT). Leaving the race (`finish_race_locally`,
`end_race`) sets the menus' records' ports back on (`enable_menu_ports`).

A race that ends early (the host leaves, a desync) leaves as the pause menu's Quit does (`race_end_to_menu_800AAC64`
with 0x800C1A9C = 0; 1 restarts the race), to screen 0xF with 0x800BF31C = 0 as Quit leaves it. It used to set
0x800BF31C = 1, starting the front end over from the boot logos (screen 0xE): that runs the power-on save load again
(`save_boot_load_settings_800B3528`, docs/menus.md#no-controllers-marquee), whose pak scan finds no pak. A player's
profile was gone from the menus after a race given up on, and in a sim run the host's game hung in
`loading_screen_wait_frame_80088C64`. With 0xF the host is back on Select Player with its profiles
(`RUSH2_ONLINE_BREAK=600` on the joiner, profiles in both data folders' saves). The front end loop (`menu_frontend_loop_800AE288`) switches
to the next screen (0x800E7BB4) when it differs from the one up (0x800E7BC4, -1 none).

A race given up on (Resyncs) ends the race but not the lobby: the host sends every machine Msg::LoadMismatch with the
reason (without it the others waited for the next frame's inputs for ever: "froze the guest") and online_net.cpp's
`end_race` marks the race over
(`racing()` false), the game thread leaves it the same way on the next frame (`race_ended_under_us`), the session goes
back to the lobby (`race_finished`) and the lobby takes everyone back to the track select as after a race. The host
stops relaying, comparing hashes and checking loads once the race is over. A player who still hasn't loaded the race 180
s after it started is dropped (`load_timeout`), as a player whose input stops coming is in the race, and the others start
without them.

## Test aids

| Variable | What |
|---|---|
| `RUSH2_ONLINE_TEST=host:N` / `join:ADDRESS` | Quick races: host and start from the title screen once N players are in, skipping the menus / join. |
| `RUSH2_ONLINE_TEST=menus:host` / `menus:join:ADDRESS` | Only host or join, for going through the menus (with `--input-script`). |
| `RUSH2_ONLINE_NAME`, `RUSH2_ONLINE_TRACK`, `RUSH2_ONLINE_RACE_TYPE` | Name; a quick race's track select id and race type. |
| `RUSH2_ONLINE_LOCAL=N`, `RUSH2_ONLINE_CARS=T,T,...` | A quick race's players on this machine (split screen; `host:N` counts every machine's players) and their car types. |
| `RUSH2_TEST_FINISH=S` | Finishes the players' cars S seconds of game clock into the race (src/players4.cpp), for the race end. |
| `RUSH2_ONLINE_FAKE=K` | The host fills K more slots with players who aren't there (no input). |
| `RUSH2_ONLINE_BOT=SEED`, `RUSH2_ONLINE_BOT_IDLE=F` | Drive (and fire, in a battle) from the frame number (repeatable runs), each of the machine's players with its own seed, after F frames. |
| `RUSH2_ONLINE_SEED` | A fixed race seed. |
| `RUSH2_ONLINE_HASHLOG` | Log the state hash every 30 frames. |
| `RUSH2_ONLINE_BREAK=load` / `track` / `cars` / `stale` / `noload` / `nudge:F` / `F` | This machine fails a check: a different starting state (a resync at frame 0), a different track fingerprint (the race doesn't start: Load check), car 0's descriptor drag x1.5 (a car set up differently: frame 0 resyncs and it takes the host's, Load check), the slots after the race's cars holding an earlier race's car on its descriptor copy (frame 0 resyncs and they take the host's, Resyncs), never loading (dropped after the load timeout), its race seed changed at frame F (a real difference, which a resync repairs), a different state hash from frame F on (resyncs can't fix it: the race is given up on). |
| `RUSH2_ONLINE_DRONES=N` | The race's computer car count (the track select's DRONES, menu settings +1), set on every machine: a race with empty car slots. |
| `RUSH2_TEST_GUEST_UNLOCKS=ID,...` | The guest (JUST PLAY) owns these unlock items for the run (src/unlocks.cpp ids: `engine_4`-`engine_9`, `tires_slicks`, `tires_pro`), never saved: a host racing bought parts. |
| `RUSH2_ONLINE_MODCARS=none` | This machine reports no mod cars, as one without the mods: its races put stand-ins in the raced mod cars' slots. |
| `RUSH2_ONLINE_TRACE=DIR` | Write the cars' physics before every frame to DIR/cars.bin; `tools/online_trace_diff.py A B` finds the first frame and words that differ. |
| `RUSH2_ONLINE_DUMP=DIR`, `RUSH2_ONLINE_DUMP_AT=F,...` | Write the cars' physics and the seeds every 30 frames, and all of RDRAM at the load check (full_load.bin) / before frames F (full_F.bin); `tools/ram_diff.py` compares two (a load mismatch: diff full_load.bin over the hashed ranges). |
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

- The per-player sound effects other than the engine (skids, shifts, horn) follow the race's first player on every
  machine (Players per machine): their state holds two players and changes the race.
- The battle's own state isn't in the hash or a resync (a difference shows once it moves a car, and is then corrected).

## Connections and timeouts

Nothing drops a player whose game and connection are working:

- ENet gives up on a peer after 10 s without acknowledgements once its retries are spent, and always after 30 s
  (`peer_timeout_min_ms`, `peer_timeout_max_ms`), set on both ends: a network hiccup of a few seconds drops no one.
- A player whose game sends no input for 30 s while connected is dropped (a hung game can't hold everyone's race); the
  overlay counts it down ("they leave the race in N s"). They can join again at once and race the next race.
- A player who hasn't loaded the race after 180 s is dropped (a first load may convert a track); the host's overlay
  names who it waits for.
- The first frames' inputs are kept even if they arrive before Go (the two travel on different channels; with a lost
  packet a client used to drop them and wait on frame 0 until the host dropped it).
- The host reads a joiner's version before the rest of its hello, so a different version is told so instead of timing
  out as "couldn't reach the host".
- Computer cars' random paint colors come from the race's seed online (src/npc_cars.cpp), so every machine shows the same
  colors.
