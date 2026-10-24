// Online play: what the game does differently in an online race (docs/online.md#the-race).
//
// Every machine runs the same race from the same inputs, so everything that moves the race forward has to happen in
// the same order on every machine:
// - Physics. Rush 2 runs its physics on the input thread (func_80076DE0), one tick per vertical retrace (60 Hz), and
//   the game thread's frames (30 Hz) read the result whenever they run: the ticks fall between the game thread's OS
//   calls depending on timing. Online, the input thread skips its tick (rush2_online_skip_physics) and the game thread
//   runs exactly two ticks at the top of each frame (func_80076D70: the input step, then the tick).
// - Frame time. The frame setup measures each frame in retraces (frame_dt, 0x80023028); a frame that took longer on
//   one machine would move its race clock further. Online every frame is two retraces (rush2_online_frame_time).
// - Inputs. Each frame waits for every player's input for it (online_net.cpp) and each tick's input step writes them
//   into the player records (rush2_online_player_input), as the game does from a controller port.
// - The pause menu. Every player's presses reach it in lockstep (any START pauses everyone's race), the physics ticks
//   stop while it is up, and it offers CONTINUE, RESTART and QUIT only (src/pause_menu.cpp).
// - Resyncs. Whatever still differs is repaired, not fatal: the race state the machines compare (synced_ranges) is
//   also what the host sends when they differ, and every other machine takes it at the start of the same frame
//   (docs/online.md#resyncs), logging what differed to the online folder of its data folder.

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

#include <chrono>
#include <ctime>
#include <filesystem>

#include "car_types.h"
#include "car_mods.h"
#include "recomp.h"
#include "util/file.h"
#include "online.h"
#include "online_sync.h"
#include "rush2.h"
#include "battle.h"
#include "ghost.h"
#include "track2049.h"
#include "car2049.h"
#include "circuit.h"
#include "players4.h"
#include "race_setup.h"

// ultramodern's conversion of a controller's stick to the N64 stick's range and shape (ultramodern/src/input.cpp).
void convert_to_n64_range(float x, float y, int8_t& stick_x, int8_t& stick_y);

extern "C" void physics_thread_step_once_80076D70(uint8_t* rdram, recomp_context* ctx);
extern "C" void race_end_to_menu_800AAC64(uint8_t* rdram, recomp_context* ctx);
extern "C" void music_queue_stop_80062FA4(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_build_list_803B81F0(uint8_t* rdram, recomp_context* ctx);
extern "C" void overlay_load_800A62C0(uint8_t* rdram, recomp_context* ctx);

namespace rush2::ram_watch {
    void frame(uint8_t* rdram, uint32_t frame_number);
}

namespace {
    using namespace rush2::online;

    // Game variables.
    constexpr uint32_t player_records = 0x800C2140;  // 0x28 each: +4 held buttons, +8/+9 stick, +0xA/+0xB stick range
    constexpr uint32_t player_record_size = 0x28;
    constexpr int32_t record_pressed = 0x2;
    constexpr int32_t record_held = 0x4;
    constexpr int32_t record_repeat = 0x6;
    constexpr int32_t record_stick = 0x8;
    constexpr int32_t record_range = 0xA;
    constexpr uint32_t physics_flag = 0x800201E4;    // u16: physics ticks run (a race is under way)
    constexpr uint32_t frame_dt = 0x80023028;        // f32: the frame's length in seconds
    constexpr uint32_t frame_retraces = 0x8002301C;  // s32: the frame's length in retraces
    constexpr uint32_t tv_type = 0x80000300;         // 0 PAL, else NTSC/MPAL
    constexpr uint32_t retrace_period_ntsc = 0x800D0098; // f32
    constexpr uint32_t retrace_period_pal = 0x800D009C;  // f32
    constexpr uint32_t rng_seeds[] = { 0x800BD808, 0x800D4030 };
    constexpr uint32_t frame_count = 0x8011962C;     // s32: frames since boot (game_main_loop_frame_800B0228)
    constexpr uint32_t draw_detail = 0x8011748C;     // u8: the cars' part animations run (car_state_draw_objects)
    constexpr uint32_t car_states = 0x801124A0;      // 0x354 per car: +0x346 the car, +0x347 its view (-1 none)
    constexpr uint32_t car_state_size = 0x354;

    // The game's own stick processing (func_80076A00): a stick past +-0x38 also sets a d-pad bit, and the stick's range
    // (what full deflection is) starts at 0x46 and grows by one per tick toward 0x7F while the stick goes past it.
    constexpr int stick_dpad_threshold = 0x38;
    constexpr uint16_t stick_left = 0x0200, stick_right = 0x0100, stick_down = 0x0400, stick_up = 0x0800;
    constexpr int8_t default_range = 0x46;
    constexpr int8_t max_range = 0x7F;

    uint32_t game_frame = 0;  // Frames since boot, for the RAM watch.

    // The online race on this machine.
    bool lockstep = false;    // Frames run in lockstep.
    // Every frame is two retraces, from the race's setup on (read by the scheduler thread, rush2_online_frame_time).
    std::atomic<bool> fixed_frame_time = false;
    bool ticking = false;     // Inside the frame's physics ticks: the input step takes the frame's inputs.
    uint32_t race_frame = 0;  // The next lockstep frame.
    std::array<PlayerInput, max_players> frame_inputs{};  // By race player (car slot).
    std::array<PlayerInput, max_players> slot_inputs{};   // By lobby slot, as wait_frame_inputs gives them.
    int8_t stick_range[max_players][2];
    uint16_t previous_held[max_players];  // Each player's held buttons last frame, for the frame's presses.
    uint16_t pending_pressed[max_players];  // Last frame's presses, for the next frame start (rush2_online_record_presses).

    // Where this machine's player gets their input (main.cpp's controller callback, which also plays input scripts).
    bool (*input_source)(int, uint16_t*, float*, float*) = nullptr;

    uint32_t fnv(uint32_t h, const uint8_t* data, size_t size) {
        for (size_t i = 0; i < size; i++) {
            h = (h ^ data[i]) * 16777619u;
        }
        return h;
    }
}

namespace {
    // Sets this frame's length to two retraces: the retrace count (0x8002301C, which the track's animated props and the
    // camera also read) and the seconds (frame_dt).
    void set_fixed_frame_time(uint8_t* rdram) {
        MEM_W(0, (int32_t)frame_retraces) = 2;
        uint32_t period_bits = (uint32_t)MEM_W(0, (int32_t)(MEM_W(0, (int32_t)tv_type) != 0 ? retrace_period_ntsc : retrace_period_pal));
        float period;
        std::memcpy(&period, &period_bits, sizeof(period));
        float dt = period * 2.0f;
        uint32_t dt_bits;
        std::memcpy(&dt_bits, &dt, sizeof(dt_bits));
        MEM_W(0, (int32_t)frame_dt) = (int32_t)dt_bits;
    }
}

void rush2::online::sync::rdram_words(Walker& w, uint32_t address, uint32_t size) {
    uint8_t* rdram = w.rdram;
    if (rdram == nullptr) {
        return;
    }
    for (uint32_t at = address & ~3u; at < address + size; at += 4) {
        w.bytes(&MEM_W(0, (int32_t)at), 4);
    }
}

namespace rush2::online::game {
    void set_input_source(bool (*source)(int, uint16_t*, float*, float*)) {
        input_source = source;
    }

    bool pedals(int player, float* gas, float* brake) {
        if (!ticking || player < 0 || player >= max_players) {
            return false;
        }
        *gas = frame_inputs[player].gas / 255.0f;
        *brake = frame_inputs[player].brake / 255.0f;
        return true;
    }
}

namespace {
    // Race setup (what the track and car selects leave for the race start, game state 7).
    constexpr uint32_t game_state = 0x8010C0D0;      // 0 menus, 1 track select, 2 car select, 7 race start, 3 racing
    constexpr uint32_t num_players = 0x8010C3E2;     // s16
    constexpr uint32_t num_views = 0x8010C3EC;       // s16
    constexpr uint32_t race_type = 0x8010C3E8;       // s32: 0 a race, 1 circuit, 2 stunt
    constexpr int32_t race_type_stunt = 2;
    // Stunt points (u16) and best (u8) per player, in players4's copies for eight (rush2::players4::moved).
    constexpr uint32_t stunt_points = 0x8010C074;
    constexpr uint32_t stunt_best = 0x8010C068;
    // A circuit (see read_circuit below).
    constexpr uint32_t circuit_block_ptr = 0x800D3E40;
    constexpr uint32_t circuit_block_size = 0x60;
    constexpr uint32_t circuit_races = 0x800D3A60;
    constexpr uint32_t circuit_races_size = 28 * 4;
    constexpr uint32_t circuit_standings = 0x800D3B28;
    constexpr uint32_t circuit_standings_size = 8 * 0x58;
    constexpr uint32_t circuit_race_index = 0x800D3DF0;  // u8
    constexpr uint32_t followed_block = 0x80E15600;      // To 0x80E15660.
    constexpr uint32_t track_id = 0x8010C3F0;        // u8
    constexpr uint32_t menu_settings = 0x800D5760;   // +0 laps, +1 drones (func_80094698 copies them to the race)
    constexpr uint32_t car_table = 0x800D9C10;       // 9 bytes per car slot: +1 type, +2 main, +3 accent, +4 stripe,
    constexpr uint32_t car_entry_size = 9;           // +5 stripe color
    constexpr uint32_t demo_flag = 0x800FAE6C;       // u8: the attract demo is running
    constexpr uint32_t view_cars = 0x80240380;       // u8 per view: the car it follows (moved by players4)
    constexpr uint32_t race_saved_players = 0x800E7BB8;  // s32: the player count the race was set up with
    constexpr int32_t record_car = 0x0;
    constexpr int32_t record_port = 0x1;
    constexpr int32_t record_automatic = 0x20;       // u8: TRANSMISSION, 1 automatic (CarChoice::automatic)
    constexpr int32_t record_profile = 0x24;         // The player's save profile (a controller's buffer, 0x6C0 bytes)
    // Each player races with the profile their car select left (RosterEntry::profile): the race reads the car's type
    // from it (+0x31, func_8009E6DC at 0x8009E9B8) and the keys from it. This machine's player races with their own
    // profile, which the race's keys and the post-race save to; the others with copies here, the same on every machine.
    // In spare RDRAM 0x80E10000-0x80E20000 (between the wings model and battle_render; see src/controls_menu.cpp's map).
    // 0x80F80000-0x81000000 is the Light Probes' per-car copies (src/car_probes.cpp).
    constexpr uint32_t guest_profiles = 0x80E10000;  // 8 x 0x6C0, to 0x80E13600
    constexpr uint32_t profile_size = 0x6C0;
    constexpr int32_t profile_car = 0x31;            // Low 5 bits: the car type (Rush 2's; Rush 2049's are kept apart)
    constexpr int32_t profile_section_end = 0x33;    // +0 is the byte sum of +1..+0x33, checked when the pak loads
    constexpr int rush2_car_types = 22;
    constexpr uint8_t no_port = 5;
    constexpr uint32_t initial_seed = 123456789;     // The game's own first seed (0x800BD808 at boot).

    enum class Phase {
        Idle,       // No online race.
        WaitMenus,  // The race was started: waiting for the game to be in the menus to set it up.
        Loading,    // Set up (game state 7): waiting for the race's first physics frame.
        Running,    // Lockstep.
    };
    Phase phase = Phase::Idle;

    // The menus' state the race setup overwrites: the player records (all eight), the car table's eight entries, the
    // race menu settings and the cars the views follow.
    struct SavedRange {
        uint32_t address;
        uint32_t size;
    };
    constexpr SavedRange saved_ranges[] = {
        { 0x800C2140, 8 * 0x28 },
        { 0x800D9C10, 8 * 9 },
        { 0x800D5760, 2 },
        { 0x80240380, 4 },
    };
    std::vector<uint8_t> menu_snapshot;
    int saved_battle_points = 0;
    int saved_battle_minutes = 0;
    bool saved_race_battle = false;
    // Each machine's players drive neighboring cars (setup_race's race order): this machine's are local_player to
    // local_player + local_count - 1, and its views 0 to local_count - 1 follow them.
    int local_player = 0;   // This machine's first player (car slot) in the race.
    int local_count = 1;    // This machine's players (split screen views).
    int race_players = 1;
    std::vector<int> race_order;          // The lobby slot of each race player (car slot).
    std::vector<uint8_t> race_car_types;  // Each player's car type (rush2::online::game::car_type).
    std::vector<uint8_t> race_wing_styles;  // Each player's wing style (rush2::online::game::wing_style).

    bool is_local_car(int car) {
        return car >= local_player && car < local_player + local_count;
    }
    uint32_t hud_rewritten = 0;  // The HUD element list rush2::online::game::hud_built last set up.
    // The online folder's log of this race's resyncs (each machine's own, in its data folder): opened at the race's
    // first resync.
    std::string desync_log_path;

    // Whether the game is in the menus (title screen, front end, track or car select), where a race can be set up.
    bool in_menus(uint8_t* rdram) {
        return rush2::race_setup::in_menus(rdram);
    }

    // The per-player rows of the car type tables at 0x80200000 (row 0 the computer cars', row p + 1 player p's, every
    // type a row; include/car_types.h): the car's settings (the race's car setup, car_init_setup_8008DBA0,
    // reads a human car's mass, ENGINE, torque map, suspension and handling from its row), colors, rims and wheel model
    // scales (carselect_preview_update_8005A598 draws a human car's wheels at its row's scale, so an empty row draws
    // none). The car select fills them for its own players from their profiles (carselect_build_list_803B81F0,
    // 0x803B8B10-0x803B8F7C, these 13 getters); online it has only this machine's players, as rows 1 and up, so the
    // other players' rows were empty (a Rush 2 car's wheel scales 0) or another player's (on each machine its own
    // player's settings drove player 1's car). Every row is filled here for every race player from the profile the race
    // uses, the same bytes on every machine, and an extra type's options from the profile's side block
    // (rush2::car2049::set_guest_side_block, sent with the profile). The tables have 8 player rows.
    void fill_player_rows(uint8_t* rdram, recomp_context* ctx) {
        rush2::race_setup::fill_player_rows(rdram, ctx, race_players);
    }

    // Sets the race up as the menus would and starts it (game state 7). The players are the roster's used slots by
    // machine (in the order of the machines' first slots), then by local player: race player i drives car slot i, and
    // each machine's players drive neighboring cars, which its views 0 and up follow. Every machine works out the same
    // order from the race's roster.
    // from_car_select: the car select has every player's car (rush2::online::game::car_select_done) and starts the race
    // itself once this returns; otherwise the race starts from wherever the front end is (the test aids).
    void setup_race(uint8_t* rdram, recomp_context* ctx, bool from_car_select) {
        RaceSettings settings = race_settings();
        std::vector<RosterEntry> roster = race_roster();
        int me = local_slot();
        race_order.clear();
        for (int machine = 0; machine < max_players; machine++) {
            for (int local = 0; local < max_local_players; local++) {
                for (int slot = 0; slot < max_players; slot++) {
                    const RosterEntry& e = roster[slot];
                    if (e.used && e.machine == machine && e.local_index == local) {
                        race_order.push_back(slot);
                    }
                }
            }
        }
        race_players = 0;
        local_player = -1;
        local_count = 0;
        std::vector<CarChoice> cars;
        std::vector<const RosterEntry*> entries;
        for (int slot : race_order) {
            if (roster[slot].machine == me) {
                if (local_player < 0) {
                    local_player = race_players;
                }
                local_count++;
            }
            cars.push_back(roster[slot].car);
            entries.push_back(&roster[slot]);
            race_players++;
        }
        if (local_player < 0) {
            local_player = 0;
            local_count = 1;
        }
        local_count = std::clamp(local_count, 1, (int)max_local_players);

        // Mod cars (docs/online.md#mod-cars). The car's type number is part of the race's state (physics car +0x7EA),
        // so a mod car races in the same slot on every machine: its picker's slot, or, when another mod car already
        // took that slot (two machines with different mods there), the first free one. Every machine works the slots
        // out the same way from the race's roster. One that has the car in that slot races it as it is; one that has it
        // in another slot copies it there; one without it puts a stand-in there with the car's physics (the picker sent
        // its spec), drawn as the base Rush 2 car. Its own cars come back after the race (return_mod_slots).
        rush2::car2049::return_mod_slots(rdram);
        std::array<uint32_t, max_mod_cars> slot_car{};               // The raced car in each slot (its check), 0 none.
        std::array<const std::vector<uint8_t>*, max_mod_cars> slot_spec{};
        std::vector<int> moved_from(cars.size(), -1);  // A player's mod car moved from this slot (its options move too).
        for (size_t p = 0; p < cars.size(); p++) {
            if (!rush2::car_types::is_mod(cars[p].type)) {
                continue;
            }
            int picked = cars[p].type - rush2::car_types::first_mod;
            uint32_t check = entries[p]->mod_cars[picked];
            if (check == 0) {
                check = 0x80000000u | (uint32_t)p;  // Its machine reported none: a car of its own.
            }
            int n = -1;
            for (int k = 0; k < max_mod_cars && n < 0; k++) {
                n = slot_car[k] == check ? k : -1;
            }
            if (n < 0 && slot_car[picked] == 0) {
                n = picked;
            }
            for (int k = 0; k < max_mod_cars && n < 0; k++) {
                n = slot_car[k] == 0 ? k : -1;
            }
            if (n < 0) {
                continue;  // More different mod cars than slots: can't be, with 8 players and 16 slots.
            }
            if (slot_car[n] == 0) {
                slot_car[n] = check;
                slot_spec[n] = &entries[p]->mod_spec;
            }
            if (n != picked) {
                moved_from[p] = picked;
                cars[p].type = (uint8_t)(rush2::car_types::first_mod + n);
            }
        }
        std::array<uint32_t, max_mod_cars> own_cars = rush2::online::game::mod_car_checks();
        for (int n = 0; n < max_mod_cars; n++) {
            if (slot_car[n] == 0 || own_cars[n] == slot_car[n]) {
                continue;
            }
            int from = -1;
            for (int k = 0; k < max_mod_cars && from < 0; k++) {
                from = own_cars[k] == slot_car[n] ? k : -1;
            }
            rush2::car2049::ModCar spec;
            if (slot_spec[n] == nullptr || !rush2::car2049::read_mod_car_spec(*slot_spec[n], spec)) {
                spec = rush2::car2049::ModCar{};  // None came: the base car's physics, the same on every machine.
            }
            rush2::online::session_log("[online] mod car slot %d: %s\n", n + 1,
                from >= 0 ? ("this machine's car from slot " + std::to_string(from + 1)).c_str() : "a stand-in");
            rush2::car2049::lend_mod_slot(rdram, n, from, spec);
        }

        race_car_types.clear();
        race_wing_styles.clear();
        for (const CarChoice& c : cars) {
            race_car_types.push_back(c.type);
            race_wing_styles.push_back(c.wing_style);
        }
        race_players = std::clamp(race_players, 1, (int)max_players);
        hud_rewritten = 0;
        desync_log_path.clear();

        // What the race changes that the menus own, put back when it ends (end_race).
        menu_snapshot.clear();
        for (const SavedRange& r : saved_ranges) {
            for (uint32_t i = 0; i < r.size; i++) {
                menu_snapshot.push_back((uint8_t)MEM_BU(0, (int32_t)(r.address + i)));
            }
        }

        // A battle's rules are the host's; the battle's own random numbers start from the race's seed.
        saved_battle_points = rush2::battle::points_to_win();
        saved_battle_minutes = rush2::battle::time_limit_minutes();
        saved_race_battle = rush2::battle::race_battle();
        if (settings.mode == GameMode::Battle) {
            rush2::battle::set_points_to_win(settings.battle_points);
            rush2::battle::set_time_limit_minutes(settings.battle_minutes);
        }
        // So is the race track select's BATTLE row (weapons, and half as much time again on the clock): the lobby
        // shows the host's only while in the track select, and puts this machine's own back after it. A ghost race
        // chosen offline before isn't raced online (it would keep the BATTLE row off here alone).
        rush2::battle::set_race_battle(settings.race_battle != 0);
        rush2::ghost::set_chosen(false);
        rush2::battle::set_online_seed(race_seed() | 1);

        // This machine's players' own profiles: menu records 0 and up, as Select Player and the car select left them.
        uint32_t own_profiles[max_local_players] = {};
        for (int i = 0; i < local_count; i++) {
            uint32_t own = (uint32_t)MEM_W(record_profile, (int32_t)(player_records + i * player_record_size));
            own_profiles[i] = own >= 0x80000000u && own + profile_size <= 0x80800000u ? own : 0;
        }

        MEM_H(0, (int32_t)num_players) = (int16_t)race_players;
        MEM_H(0, (int32_t)num_views) = (int16_t)local_count;
        // The race the host chose in its menus: race type (the race start, func_800AE670 state 7, turns a race on the
        // stunt track, 11, and the Rush 2049 arenas hosted there, into stunt mode itself), track and the track
        // select's settings (laps, drones, options; func_80094698 copies them to the race).
        MEM_W(0, (int32_t)race_type) = settings.race_type;
        // A custom track is this machine's own id for it (the host's track select only starts one every player has).
        int track = settings.track;
        if (!settings.mod_track.empty()) {
            track = rush2::track2049::track_for_online_key(settings.mod_track);
            if (track < 0) {
                rush2::online::session_log("[online] this machine hasn't the custom track %s\n", settings.mod_track.c_str());
                track = settings.track;  // The race's load check (report_loaded) ends it.
            }
        }
        MEM_B(0, (int32_t)track_id) = (int8_t)track;
        // A circuit race is the host's circuit's (its block sets the difficulty and deaths, func_80094698), its track
        // this machine's id for it.
        if (!settings.circuit.empty() && !is_host()) {
            rush2::online::game::follow_circuit(rdram, settings.circuit);
            int index = MEM_BU(0, (int32_t)circuit_race_index);
            if (index < 28) {
                MEM_B(0, (int32_t)(circuit_races + index * 4)) = (int8_t)track;
            }
        }
        for (int i = 0; i < (int)settings.menu.size(); i++) {
            MEM_B(i, (int32_t)menu_settings) = (int8_t)settings.menu[i];
        }
        if (const char* drones = std::getenv("RUSH2_ONLINE_DRONES")) {  // Test aid: the race's computer car count.
            MEM_B(1, (int32_t)menu_settings) = (int8_t)std::atoi(drones);
        }

        // Every machine's records start the same: blank but for the button masks (rush2::race_setup::init_record).
        rush2::car2049::clear_guest_side_blocks();
        for (int p = 0; p < race_players; p++) {
            int32_t record = (int32_t)(player_records + p * player_record_size);
            rush2::race_setup::init_record(rdram, p, no_port, cars[p].automatic);
            // The profile as the player's car select sent it (the same bytes on every machine; this machine's players'
            // own already hold them), its car type set to the car they picked, and its Rush 2049 car options.
            // A player who sent none (the test aids' races) races with a blank one on every machine.
            const std::vector<uint8_t>& sent = entries[p]->profile;
            bool have = sent.size() == profile_size;
            uint32_t own_profile = is_local_car(p) ? own_profiles[p - local_player] : 0;
            // A mod car moved to another slot keeps its options (the profile's side block, 13 bytes per type) in its
            // picked slot's place: it races with a copy whose options are in the new slot's place, even this
            // machine's own player (whose save keeps them where they were).
            bool own = own_profile != 0 && have && moved_from[p] < 0;
            uint32_t profile = own ? own_profile : guest_profiles + p * profile_size;
            // This machine's players' profiles are their saves themselves: they're never copied over (that put back
            // what the game had saved to them since the car select sent them).
            if (!own) {
                for (uint32_t i = 0; i < profile_size; i++) {
                    MEM_B(0, (int32_t)(profile + i)) = have ? (int8_t)sent[i] : 0;
                }
                std::vector<uint8_t> side = entries[p]->side;
                constexpr size_t options_size = 13;
                size_t from = (size_t)(moved_from[p] + rush2::car_types::first_mod - rush2_car_types) * options_size;
                size_t to = (size_t)(cars[p].type - rush2_car_types) * options_size;
                if (moved_from[p] >= 0 && from + options_size <= side.size() && to + options_size <= side.size()) {
                    std::copy_n(side.begin() + from, options_size, side.begin() + to);
                }
                rush2::car2049::set_guest_side_block(rdram, p, profile, side);
            }
            // This machine's player's profile is their save (its Controller Pak note): the byte's section keeps its
            // byte sum at +0, which the game checks when it reads the pak (func_80098190), so it's updated with the
            // byte. A stale sum made the next boot find the note damaged and wipe it. A Rush 2049 type doesn't fit
            // the 5 bits (its choice is kept apart, src/rush2049/car2049.cpp): the byte stays as the car select left it.
            uint8_t type_bits = (uint8_t)MEM_BU(profile_car, (int32_t)profile);
            uint8_t car_bits = (uint8_t)((type_bits & 0xE0) | (cars[p].type & 0x1F));
            if (cars[p].type < rush2_car_types && car_bits != type_bits) {
                MEM_B(profile_car, (int32_t)profile) = (int8_t)car_bits;
                uint8_t sum = 0;
                for (int32_t i = 1; i <= profile_section_end; i++) {
                    sum += (uint8_t)MEM_BU(i, (int32_t)profile);
                }
                MEM_B(0, (int32_t)profile) = (int8_t)sum;
            }
            MEM_W(record_profile, record) = (int32_t)profile;
            int32_t entry = (int32_t)(car_table + p * car_entry_size);
            MEM_B(1, entry) = (int8_t)cars[p].type;
            for (int i = 0; i < 4; i++) {
                MEM_B(2 + i, entry) = (int8_t)cars[p].colors[i];
            }
        }
        fill_player_rows(rdram, ctx);
        for (uint32_t seed_address : rng_seeds) {
            MEM_W(0, (int32_t)seed_address) = (int32_t)(seed_address == rng_seeds[0] ? (race_seed() | 1) : 0);
        }
        // View i follows this machine's player i's car; the race setup (func_800A3690) gives those cars their views.
        for (int i = 0; i < local_count; i++) {
            MEM_B(i, (int32_t)view_cars) = (int8_t)(local_player + i);
        }
        if (const char* forced = std::getenv("RUSH2_ONLINE_VIEW_CAR")) {  // Test aid: every machine views this car.
            MEM_B(0, (int32_t)view_cars) = (int8_t)std::atoi(forced);
        }
        // The front end is left as its loop leaves it for a race (func_800AE288 at 0x800AE428-0x800AE458): the screen up
        // is told it's left ($s1 the screen, $s0 0) and none is up. The title screen keeps its data in the heap the
        // race reuses, and would otherwise be left after the race with that data gone.
        if (!from_car_select) {
            rush2::race_setup::leave_front_end(rdram, ctx);
        }
        // From here on every frame is two retraces, this one (the race start) included.
        set_fixed_frame_time(rdram);
        fixed_frame_time = true;
        for (auto& range : stick_range) {
            range[0] = default_range;
            range[1] = default_range;
        }
        std::fill(std::begin(previous_held), std::end(previous_held), 0);
        std::fill(std::begin(pending_pressed), std::end(pending_pressed), 0);
        rush2::online::session_log("[online] race set up: %d players, this machine is players %d-%d, track %d\n", race_players,
            local_player + 1, local_player + local_count, settings.track);
    }

    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_DUMP=DIR writes what state_hash covers to DIR/frame_N.bin each
    // time it's hashed, to find what differs between machines (tools/ram_diff.py compares two).
    void dump_state(uint8_t* rdram, uint32_t frame) {
        static const char* dir = std::getenv("RUSH2_ONLINE_DUMP");
        if (dir == nullptr) {
            return;
        }
        // Laid out at their RDRAM addresses in an 8 MB image, so tools/ram_diff.py names them.
        static std::vector<uint8_t> image(0x800000);
        std::fill(image.begin(), image.end(), 0);
        auto copy = [&](uint32_t address, uint32_t size) {
            for (uint32_t i = 0; i < size; i++) {
                image[address - 0x80000000u + i] = (uint8_t)MEM_BU(0, (int32_t)(address + i));
            }
        };
        copy(0x800F5470, 8 * 0x81C);
        for (uint32_t seed_address : rng_seeds) {
            copy(seed_address, 4);
        }
        char name[512];
        std::snprintf(name, sizeof(name), "%s/frame_%06u.bin", dir, frame);
        if (FILE* f = std::fopen(name, "wb")) {
            std::fwrite(image.data(), 1, image.size(), f);
            std::fclose(f);
        }
    }

    // Test aid: all of RDRAM (N64 byte order) to RUSH2_ONLINE_DUMP/full_NAME.bin, to diff two machines' whole memory
    // (tools/ram_diff.py).
    void dump_full(uint8_t* rdram, const std::string& name) {
        static const char* dir = std::getenv("RUSH2_ONLINE_DUMP");
        if (dir == nullptr) {
            return;
        }
        std::vector<uint8_t> image(0x800000);
        for (uint32_t i = 0; i < image.size(); i++) {
            image[i] = (uint8_t)MEM_BU(0, (int32_t)(0x80000000u + i));
        }
        std::string path = std::string(dir) + "/full_" + name + ".bin";
        if (FILE* f = std::fopen(path.c_str(), "wb")) {
            std::fwrite(image.data(), 1, image.size(), f);
            std::fclose(f);
        }
    }

    // The race state every machine must agree on: the cars' physics and race state, the player records and the random
    // seeds. state_hash compares it every 30 frames, and a resync copies the host's (capture_state, apply_state), so the
    // two can't drift apart.
    constexpr uint32_t sync_cars = 0x800F5470;      // Physics cars, 0x81C each
    constexpr uint32_t sync_car_size = 0x81C;
    struct SyncRange {
        uint32_t address;
        uint32_t size;
    };

    // The race's own variables beyond the cars: what the physics tick (physics_thread_step_once_80076D70) and the
    // frame's race update (race_frame_update_800A3050) write, as `tools/rush2049/fsum.py --reach 0x80076D70 0x800A3050
    // --stop 0x800A06F8 0x80062F50 0x8009E0C4` lists them, less the drawing, text and sound ones, and the race clock the
    // drones steer by. Without them a resync copied the cars but not, say, which drone path node was being steered for
    // (drone_steer_along_path_80074284), and the machines parted again within a second.
    struct RaceGlobal {
        uint32_t address;
        uint32_t size;
        const char* name;
    };
    constexpr RaceGlobal race_globals[] = {
        { 0x80117488, 4, "the race clock (timer_game_time)" },
        { 0x800D8FBC, 2, "the drones' path node (drone_path_node_index)" },
        { 0x800D97E0, 4, "the drones' last path choice (drone_path_progress)" },
        { 0x800E79B8, 2, "race_car_count" },
        { 0x800E7B36, 2, "drone_tick_counter" },
        { 0x800E7B9A, 2, "car_physics_counter" },
        { 0x800E7BA0, 2, "drone_tick_state" },
        { 0x800E7BAA, 2, "car_physics_state" },
        { 0x800C1014, 4, "car_collision_f32" },
        { 0x800FAEF8, 1, "physics_snapshot_flag" },
        { 0x8010C034, 4, "race_time_accum" },
        { 0x8010C0E4, 4, "physics_dt_scale" },
        { 0x80111934, 2, "physics_substep_count" },
        { 0x8010C020, 1, "race_checkpoint_flag" },
        { 0x8010C038, 4, "race_checkpoint_word" },
        { 0x8010C052, 1, "race_gate_flag" },
        { 0x8010C05E, 1, "race_checkpoint_byte" },
        { 0x8010C0E0, 1, "race_finished_flag" },
        { 0x8010C171, 1, "race_checkpoint_event_flag" },
        { 0x8010C204, 4, "race_time_float" },
        { 0x80125B58, 4, "race_frame_float" },
        // The race's copy of the path header (race_path_time_param_8010BCE8): the start time (+0), the checkpoint
        // count (+8) and up to 10 checkpoints of 0x50 bytes from +0xC (each one's time extensions +0x1E/+0x20, its
        // spine and lane crossings +0x22). The clock and the drones run on it, and the race setup's patches
        // (track2049.cpp path_race_time, the BATTLE row's extra time) write it: one machine's different times
        // parted the race at the first checkpoint.
        { 0x8010BCE8, 0xC + 10 * 0x50, "the path header" },
    };

    std::vector<SyncRange> synced_ranges() {
        std::vector<SyncRange> out;
        out.push_back({ sync_cars, 8 * sync_car_size });
        // The records, but for the profile pointer (+0x24): this machine's player's is their own profile.
        for (int p = 0; p < race_players; p++) {
            out.push_back({ player_records + p * player_record_size, (uint32_t)record_profile });
            uint32_t after = record_profile + 4;
            if (after < player_record_size) {
                out.push_back({ player_records + p * player_record_size + after, player_record_size - after });
            }
        }
        // The cars' race state, but for the camera's bytes (+0x347 the view, +0x348/+0x349 the camera mode) and the
        // scene node handles (+0xF0 the body's, +0x8 of each 0x1C byte part object from +0x108), which differ by
        // machine: the race setup allocates nodes for each view (a machine with two players had every node one
        // further on, and the race couldn't start).
        auto machine_own = [](uint32_t offset) {
            if (offset >= 0x347 && offset < 0x34A) {
                return true;
            }
            if (offset >= 0xF0 && offset < 0xF4) {
                return true;
            }
            return offset >= 0x108 && offset < 0x300 && (offset - 0x108) % 0x1C >= 8 && (offset - 0x108) % 0x1C < 12;
        };
        for (int car = 0; car < 8; car++) {
            uint32_t state = car_states + car * car_state_size;
            uint32_t run = 0;
            for (uint32_t offset = 0; offset <= car_state_size; offset++) {
                if (offset == car_state_size || machine_own(offset)) {
                    if (offset > run) {
                        out.push_back({ state + run, offset - run });
                    }
                    run = offset + 1;
                }
            }
        }
        for (uint32_t seed_address : rng_seeds) {
            out.push_back({ seed_address, 4 });
        }
        for (const RaceGlobal& g : race_globals) {
            out.push_back({ g.address, g.size });
        }
        return out;
    }

    // The race state the port's C++ keeps (include/online_sync.h), walked by each module, compared and copied after
    // the RDRAM ranges.
    namespace sync = rush2::online::sync;
    struct SyncPart {
        const char* name;
        void (*walk)(sync::Walker&);
    };
    constexpr SyncPart sync_parts[] = {
        { "the cars' physics data", rush2::car2049::physics_online_sync },
        { "the Rush 2049 wings", rush2::wings::online_sync },
        { "California Speed's physics", rush2::calspeed::physics::online_sync },
        { "SF Rush's buses", rush2::track1::buses_online_sync },
        { "the Rush 2049 moving objects", rush2::track2049::movers_online_sync },
        { "the Rush 2049 stunt scoring", rush2::stunt2049::online_sync },
        { "the battle", rush2::battle::online_sync },
        { "the battle's computer cars", rush2::battle_ai::online_sync },
    };

    class Saver : public sync::Walker {
    public:
        std::vector<uint8_t> out;
        void bytes(void* data, size_t size) override {
            const uint8_t* p = static_cast<const uint8_t*>(data);
            out.insert(out.end(), p, p + size);
        }
        void count(size_t& n) override {
            uint32_t v = (uint32_t)n;
            bytes(&v, sizeof(v));
        }
        bool loading() const override {
            return false;
        }
    };

    // Reads a Saver's bytes back. A count beyond what's left, or a walk running past the end, marks it bad and stops
    // (the same build saves and loads, so it doesn't happen but for a damaged snapshot).
    class Loader : public sync::Walker {
    public:
        const uint8_t* data = nullptr;
        size_t size = 0, at = 0;
        bool bad = false;
        void bytes(void* out, size_t n) override {
            if (bad || n > size - at) {
                bad = true;
                return;
            }
            std::memcpy(out, data + at, n);
            at += n;
        }
        void count(size_t& n) override {
            uint32_t v = 0;
            bytes(&v, sizeof(v));
            if (bad || v > size - at) {
                bad = true;
                return;
            }
            n = v;
        }
        bool loading() const override {
            return true;
        }
    };

    std::vector<uint8_t> save_part(uint8_t* rdram, const SyncPart& part) {
        Saver s;
        s.rdram = rdram;
        part.walk(s);
        return std::move(s.out);
    }

    uint32_t state_hash(uint8_t* rdram) {
        uint32_t h = 2166136261u;
        for (const SyncRange& r : synced_ranges()) {
            for (uint32_t i = 0; i < r.size; i++) {
                h = (h ^ (uint8_t)MEM_BU(0, (int32_t)(r.address + i))) * 16777619u;
            }
        }
        for (const SyncPart& part : sync_parts) {
            std::vector<uint8_t> bytes = save_part(rdram, part);
            h = fnv(h, bytes.data(), bytes.size());
        }
        return h;
    }

    // The synced RDRAM ranges' bytes, then each C++ part's as a u32 length and its walk's bytes.
    std::vector<uint8_t> capture_state(uint8_t* rdram) {
        std::vector<uint8_t> out;
        for (const SyncRange& r : synced_ranges()) {
            for (uint32_t i = 0; i < r.size; i++) {
                out.push_back((uint8_t)MEM_BU(0, (int32_t)(r.address + i)));
            }
        }
        for (const SyncPart& part : sync_parts) {
            std::vector<uint8_t> bytes = save_part(rdram, part);
            uint32_t n = (uint32_t)bytes.size();
            out.insert(out.end(), reinterpret_cast<const uint8_t*>(&n), reinterpret_cast<const uint8_t*>(&n) + 4);
            out.insert(out.end(), bytes.begin(), bytes.end());
        }
        return out;
    }

    // What a synced address is, for the desync log.
    std::string describe(uint32_t address) {
        char text[96];
        if (address >= sync_cars && address < sync_cars + 8 * sync_car_size) {
            uint32_t o = address - sync_cars;
            std::snprintf(text, sizeof(text), "car %u physics +0x%03X", o / sync_car_size, o % sync_car_size);
        }
        else if (address >= car_states && address < car_states + 8 * car_state_size) {
            uint32_t o = address - car_states;
            std::snprintf(text, sizeof(text), "car %u race state +0x%03X", o / car_state_size, o % car_state_size);
        }
        else if (address >= player_records && address < player_records + max_players * player_record_size) {
            uint32_t o = address - player_records;
            std::snprintf(text, sizeof(text), "player %u record +0x%02X", o / player_record_size + 1,
                o % player_record_size);
        }
        else {
            for (const RaceGlobal& g : race_globals) {
                if (address >= g.address && address < g.address + g.size) {
                    if (g.size > 4) {
                        std::snprintf(text, sizeof(text), "%s +0x%03X", g.name, address - g.address);
                    }
                    else {
                        std::snprintf(text, sizeof(text), "%s", g.name);
                    }
                    return text;
                }
            }
            std::snprintf(text, sizeof(text), "random seed 0x%08X", address & ~3u);
        }
        return text;
    }

    void desync_log(const std::string& text) {
        if (desync_log_path.empty()) {
            std::filesystem::path dir = recompui::file::get_app_folder_path() / "online";
            std::error_code ec;
            std::filesystem::create_directories(dir, ec);
            std::time_t now = std::time(nullptr);
            char name[64];
            std::strftime(name, sizeof(name), "desync_%Y%m%d_%H%M%S.txt", std::localtime(&now));
            desync_log_path = (dir / name).string();
        }
        if (FILE* f = std::fopen(desync_log_path.c_str(), "a")) {
            std::fputs(text.c_str(), f);
            std::fclose(f);
        }
        std::fputs(text.c_str(), stderr);
    }

    // The cars in the race: the players (num_players, setup_race's race_players) and the computer cars (the race's
    // drone count, s16 0x800D3E90, src/ghost.cpp's race_drones), the same on every machine. The physics car slots after
    // them keep whatever this machine raced last (an earlier race, the attract demo), which differs by machine: a 2-car
    // race with the host's attract demo cars left in slots 2-7 and the joiner's empty failed the old load check.
    int race_cars(uint8_t* rdram) {
        return std::clamp<int>(MEM_H(0, (int32_t)num_players) + MEM_H(0, (int32_t)0x800D3E90), 0, 8);
    }

    // Takes the host's race state (capture_state's bytes) and logs where this machine's differed. A word that is an
    // RDRAM address on both machines but a different one is this machine's own (a pointer into memory laid out
    // differently here) and is kept: copying the host's would point at the wrong thing. But not in a car slot after
    // the race's cars (race_cars), which no car of the race uses: kept there, a leftover car's descriptor (physics car
    // +0) from each machine's own last race stayed different, every check after the resync failed again and the race
    // was given up on (a user's 2-player LAN race; RUSH2_ONLINE_BREAK=stale).
    void apply_state(uint8_t* rdram, const std::vector<uint8_t>& state, uint32_t frame) {
        std::vector<SyncRange> ranges = synced_ranges();
        const uint32_t cars = (uint32_t)race_cars(rdram);
        auto unused_slot = [cars](uint32_t address) {
            if (address >= sync_cars && address < sync_cars + 8 * sync_car_size) {
                return (address - sync_cars) / sync_car_size >= cars;
            }
            return address >= car_states && address < car_states + 8 * car_state_size &&
                (address - car_states) / car_state_size >= cars;
        };
        size_t total = 0;
        for (const SyncRange& r : ranges) {
            total += r.size;
        }
        if (state.size() < total) {
            desync_log("[online] resync at frame " + std::to_string(frame) + ": the host's state has " +
                std::to_string(state.size()) + " bytes, this machine's " + std::to_string(total) + " (kept its own)\n");
            return;
        }
        auto is_pointer = [](uint32_t v) { return v > 0x80000000u && v < 0x80800000u; };
        std::string log = "[online] resync at frame " + std::to_string(frame) + ", race player " +
            std::to_string(local_player + 1) + ": what differed from the host (this machine -> host)\n";
        int differences = 0;
        // Every difference by area ("car 3 physics", a variable's name), as the list shows only the first 200.
        std::vector<std::pair<std::string, int>> areas;
        auto count_area = [&](uint32_t address) {
            std::string area = describe(address);
            size_t plus = area.find(" +0x");
            if (plus != std::string::npos) {
                area.resize(plus);
            }
            for (auto& a : areas) {
                if (a.first == area) {
                    a.second++;
                    return;
                }
            }
            areas.emplace_back(area, 1);
        };
        size_t at = 0;
        for (const SyncRange& r : ranges) {
            for (uint32_t i = 0; i < r.size;) {
                uint32_t address = r.address + i;
                // A whole aligned word inside the range: compared as one (and kept if it's a pointer of this machine).
                if ((address & 3) == 0 && i + 4 <= r.size) {
                    uint32_t mine = (uint32_t)MEM_W(0, (int32_t)address);
                    uint32_t host = 0;
                    for (int b = 0; b < 4; b++) {
                        host = (host << 8) | state[at + i + b];
                    }
                    if (mine != host) {
                        bool kept = is_pointer(mine) && is_pointer(host) && !unused_slot(address);
                        count_area(address);
                        if (differences++ < 200) {
                            char line[160];
                            std::snprintf(line, sizeof(line), "  %-28s %08X -> %08X%s\n", describe(address).c_str(),
                                mine, host, kept ? " (an address: kept)" : "");
                            log += line;
                        }
                        if (!kept) {
                            MEM_W(0, (int32_t)address) = (int32_t)host;
                        }
                    }
                    i += 4;
                    continue;
                }
                uint8_t mine = (uint8_t)MEM_BU(0, (int32_t)address);
                uint8_t host = state[at + i];
                if (mine != host) {
                    count_area(address);
                    if (differences++ < 200) {
                        char line[160];
                        std::snprintf(line, sizeof(line), "  %-28s %02X -> %02X\n", describe(address).c_str(), mine, host);
                        log += line;
                    }
                    MEM_B(0, (int32_t)address) = (int8_t)host;
                }
                i++;
            }
            at += r.size;
        }
        // The C++ parts after the ranges: one that differs takes the host's whole.
        for (const SyncPart& part : sync_parts) {
            uint32_t n = 0;
            if (state.size() - at < 4) {
                log += std::string("  ") + part.name + ": missing from the host's state (kept this machine's)\n";
                break;
            }
            std::memcpy(&n, state.data() + at, 4);
            at += 4;
            if (n > state.size() - at) {
                log += std::string("  ") + part.name + ": cut short in the host's state (kept this machine's)\n";
                break;
            }
            std::vector<uint8_t> mine = save_part(rdram, part);
            if (mine.size() != n || std::memcmp(mine.data(), state.data() + at, n) != 0) {
                Loader l;
                l.rdram = rdram;
                l.data = state.data() + at;
                l.size = n;
                part.walk(l);
                differences++;
                areas.emplace_back(part.name, 1);
                log += std::string("  ") + part.name + (l.bad || l.at != n ? ": differed, and the host's didn't read back" :
                    ": differed (took the host's)") + "\n";
            }
            at += n;
        }
        log += "  " + std::to_string(differences) + " difference(s)" + (differences > 200 ? " (the first 200 shown)" : "") +
            "\n";
        if (!areas.empty()) {
            log += "  by area:";
            for (size_t i = 0; i < areas.size(); i++) {
                log += (i ? ", " : " ") + areas[i].first + " " + std::to_string(areas[i].second);
            }
            log += "\n";
        }
        desync_log(log);
    }

    // What the race was set up with, beyond the cars' state: the track's collision data (the file the ground and wall
    // queries read, collision_file_load_800814B0: its 0xC-byte header at race_collision_asset_ptr 0x800D5754, u16
    // counts and section sizes, the sections after it in order, VLIST last (pointer 0x80111930, its size at header
    // +0xA); the loader writes nothing into it, so the same track hashes alike on every machine, wherever it sits),
    // and what each car's physics read (rush2::car2049::physics_hash by the car's type, physics car +0x7EA, and table
    // row, 0 for a computer car and +0x7E0 + 1 for a human, +0x7E8 == 2). These differing can't be repaired by a resync:
    // the race doesn't start (online_net.cpp's host_check_loaded names who differs and why).
    uint32_t track_fingerprint(uint8_t* rdram) {
        constexpr uint32_t collision_header = 0x800D5754;   // race_collision_asset_ptr
        constexpr uint32_t vlist = 0x80111930;              // collision_point_test_ptr_b: the VLIST section
        uint32_t base = (uint32_t)MEM_W(0, (int32_t)collision_header);
        uint32_t end = (uint32_t)MEM_W(0, (int32_t)vlist);
        if (base < 0x80000000u || base >= 0x80800000u || end < base || end >= 0x80800000u) {
            return 0;
        }
        end += (uint16_t)MEM_HU(0xA, (int32_t)base);
        uint32_t h = 2166136261u;
        for (uint32_t at = base; at < end && at < 0x80800000u; at++) {
            h = (h ^ (uint8_t)MEM_BU(0, (int32_t)at)) * 16777619u;
        }
        return h;
    }


    uint32_t cars_fingerprint(uint8_t* rdram) {
        uint32_t h = 2166136261u;
        for (int car = 0; car < race_cars(rdram); car++) {
            uint32_t physics_car = sync_cars + car * sync_car_size;
            int type = MEM_BU(0x7EA, (int32_t)physics_car);
            bool human = MEM_BU(0x7E8, (int32_t)physics_car) == 2;
            int row = human ? MEM_H(0x7E0, (int32_t)physics_car) + 1 : 0;
            h = rush2::car2049::physics_hash(rdram, type, row, h);
        }
        return h;
    }

    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_BREAK makes this machine fail a check: "load" reports a
    // different starting state (a resync at frame 0), "track" a different track (the race doesn't start), "cars" car
    // 0's physics data set up differently (frame 0 resyncs and the car takes the host's), "stale" the slots after the
    // race's cars holding an earlier race's car on its descriptor copy (as two machines' different last races leave
    // them), "noload" never reports
    // loaded (the host drops the player after its load timeout), "nudge:F" changes the race seed at frame F (a real difference, which a resync repairs), a frame
    // number F reports a different state hash from frame F on (resyncs can't fix it: the race is given up on).
    const std::string& break_test() {
        static const std::string spec = std::getenv("RUSH2_ONLINE_BREAK") ? std::getenv("RUSH2_ONLINE_BREAK") : "";
        return spec;
    }

    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_BOT=SEED drives this machine's car from the frame number alone
    // (gas held, steering weaving, a brake now and then, firing in a battle), so runs repeat exactly.
    PlayerInput bot_input(uint32_t frame, int seed) {
        PlayerInput in{};
        static const int idle = std::getenv("RUSH2_ONLINE_BOT_IDLE") ? std::atoi(std::getenv("RUSH2_ONLINE_BOT_IDLE")) : 0;
        if ((int)frame < idle) {
            return in;
        }
        uint32_t h = (frame / 15) * 2654435761u + (uint32_t)seed * 40503u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        in.buttons = 0x8000;  // A: gas
        in.gas = 255;
        if ((h & 0x1F) == 0) {
            in.buttons = 0x4000;  // B: brake
            in.gas = 0;
            in.brake = 255;
        }
        in.stick_x = int8_t(int((h >> 8) % 161) - 80);
        // In a battle, fire now and then (a quarter of the time, in quarter-second bursts).
        if (((h >> 20) & 3) == 0) {
            in.battle = rush2::controls::battle_fire;
        }
        return in;
    }

    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_TEST=host:N hosts a lobby and starts a race at once from the
    // title screen once N players are in (skipping the menus); RUSH2_ONLINE_TEST=join:ADDRESS joins and says it has a
    // car. RUSH2_ONLINE_NAME sets the name; RUSH2_ONLINE_TRACK the track select id and RUSH2_ONLINE_RACE_TYPE the race
    // type. RUSH2_ONLINE_TEST=menus:host / menus:join:ADDRESS only hosts or joins, for going through the menus.
    // RUSH2_ONLINE_MANUAL=1 gives this machine's players the manual gearbox in a quick race. RUSH2_ONLINE_LOCAL=N gives
    // the quick race N players on this machine (split screen; host:N counts every machine's players), and
    // RUSH2_ONLINE_CARS=T,T,... their car types. RUSH2_ONLINE_PORT hosts on another port.
    void test_aid(uint8_t* rdram) {
        static const char* spec = std::getenv("RUSH2_ONLINE_TEST");
        static bool started = false;
        if (spec == nullptr) {
            return;
        }
        std::string text = spec;
        bool menus = text.rfind("menus:", 0) == 0;
        if (menus) {
            text = text.substr(6);
        }
        if (!started) {
            started = true;
            if (const char* name = std::getenv("RUSH2_ONLINE_NAME")) {
                set_display_name(name);
            }
            if (text.rfind("host", 0) == 0) {
                HostOptions options;
                options.open_router_port = false;
                // RUSH2_ONLINE_PORT: host on this port (join with ADDRESS:PORT), clear of a lobby already open here.
                if (const char* port = std::getenv("RUSH2_ONLINE_PORT")) {
                    options.port = (uint16_t)std::atoi(port);
                }
                host(options);
            }
            else if (text.rfind("join:", 0) == 0) {
                join(text.substr(5));
            }
            return;
        }
        if (menus || state() != State::Lobby) {
            return;
        }
        set_local_mod_cars(rush2::online::game::mod_car_checks());
        // RUSH2_ONLINE_LOCAL, RUSH2_ONLINE_CARS, RUSH2_ONLINE_MANUAL: this machine's players and cars.
        static bool cars_sent = false;
        if (!cars_sent) {
            cars_sent = true;
            const char* local = std::getenv("RUSH2_ONLINE_LOCAL");
            int count = std::clamp(local != nullptr ? std::atoi(local) : 1, 1, (int)max_local_players);
            std::string types = std::getenv("RUSH2_ONLINE_CARS") ? std::getenv("RUSH2_ONLINE_CARS") : "";
            std::vector<LocalPlayer> players(count);
            size_t at = 0;
            for (LocalPlayer& p : players) {
                p.car.automatic = std::getenv("RUSH2_ONLINE_MANUAL") != nullptr ? 0 : 1;
                if (at < types.size()) {
                    p.car.type = (uint8_t)std::atoi(types.c_str() + at);
                    size_t comma = types.find(',', at);
                    at = comma == std::string::npos ? types.size() : comma + 1;
                }
                if (rush2::car_types::is_mod(p.car.type)) {
                    p.mod_spec = rush2::car2049::mod_car_spec(p.car.type - rush2::car_types::first_mod);
                }
            }
            set_cars(players);
        }
        if (text.rfind("host", 0) == 0) {
            int wanted = text.size() > 5 ? std::atoi(text.c_str() + 5) : 1;
            int used = 0;
            bool chosen = true;
            for (const RosterEntry& e : roster()) {
                used += e.used ? 1 : 0;
                chosen &= !e.used || !e.connected || e.progress == uint8_t(Progress::CarChosen);
            }
            set_progress(Progress::CarChosen);
            static bool raced = false;
            if (!raced && used >= wanted && chosen && in_menus(rdram)) {
                RaceSettings s;
                for (int i = 0; i < (int)s.menu.size(); i++) {
                    s.menu[i] = (uint8_t)MEM_BU(i, (int32_t)menu_settings);
                }
                if (const char* track = std::getenv("RUSH2_ONLINE_TRACK")) {
                    s.track = (uint8_t)std::atoi(track);
                }
                if (const char* type = std::getenv("RUSH2_ONLINE_RACE_TYPE")) {
                    s.race_type = (uint8_t)std::atoi(type);
                }
                s.mode = s.track >= 30 && s.track < 38 ? GameMode::Battle : s.track == 11 ? GameMode::Stunt : GameMode::Race;
                raced = start_race(s);
                static std::string reported;
                if (!raced && last_error() != reported) {
                    reported = last_error();
                    rush2::online::session_log("[online] can't start: %s\n", reported.c_str());
                }
            }
        }
        else {
            set_progress(Progress::CarChosen);
        }
    }
}

namespace {
    constexpr uint32_t race_restart_flag = 0x800C1A9C;  // u8: func_800AAC64 restarts the race (0: to the menus)
    // u8: the front end after a race starts over from the boot logos (screen 0xE, then the title screen) rather than at
    // screen 0xF (func_8009FD68). Kept 0 (end_race): screen 0xE runs the power-on save load again.
    constexpr uint32_t front_end_from_logos = 0x800BF31C;
    constexpr int32_t state_post_race = 6;           // func_800A952C: records, lap and race times, name entry
    constexpr int32_t state_car_select = 2;
    constexpr int32_t state_front_end = 0;           // The pause menu's QUIT (func_800AAC64) goes back to it.

    // The menus take a port's presses (into the menu buttons 0x8011962A and 0x80125A68, player_join_init_all_80080788)
    // only while its input_port_state byte (0x80125AC4 + port) is set. Opening the pause menu clears all four and sets
    // the ports of records 0 and 1 back (input_set_port_state_80093DC0 at 0x800AFE60-0x800AFE98), but online the
    // records have no port (5, which it ignores), so after a pause every port stayed off and the track select the race
    // leaves for took no buttons. Once the menus' records are back, their players' ports take presses again.
    void enable_menu_ports(uint8_t* rdram) {
        constexpr uint32_t input_port_state = 0x80125AC4;
        constexpr int num_ports = 4;
        int players = MEM_H(0, (int32_t)num_players);
        // Players 3 and 4 (src/players4.cpp) have a port only while they're in; player 2's record keeps its port after
        // they leave, so it counts only while the game has 2 players.
        for (int p = 0; p < max_local_players; p++) {
            int port = MEM_BU(record_port, (int32_t)(player_records + p * player_record_size));
            if (port < num_ports && (p < players || p >= 2)) {
                MEM_B(0, (int32_t)(input_port_state + port)) = 1;
            }
        }
    }

    // The race is over (game state 6, or the front end after the pause menu's QUIT): the machines part. The post-race
    // works on the game's players 1 and up, so this machine's players become the only ones, with the records the menus
    // gave them (their profiles and controllers) and their cars; it saves their stats and times and runs their name
    // entry as after a local race. After QUIT the cars go back to the menus' too.
    void finish_race_locally(uint8_t* rdram, bool quit) {
        lockstep = false;
        fixed_frame_time = false;
        rush2::battle::set_online_seed(0);
        rush2::battle::set_points_to_win(saved_battle_points);
        rush2::battle::set_time_limit_minutes(saved_battle_minutes);
        rush2::battle::set_race_battle(saved_race_battle);
        if (!menu_snapshot.empty()) {
            size_t at = 0;
            for (const SavedRange& r : saved_ranges) {
                for (uint32_t i = 0; i < r.size; i++, at++) {
                    // The car table stays as raced: the post-race shows the cars.
                    if (quit || r.address != car_table) {
                        MEM_B(0, (int32_t)(r.address + i)) = (int8_t)menu_snapshot[at];
                    }
                }
            }
            menu_snapshot.clear();
        }
        for (int i = 0; i < local_count; i++) {
            int32_t record = (int32_t)(player_records + i * player_record_size);
            MEM_B(record_car, record) = (int8_t)(local_player + i);
            // The post-race counts the race in each player's profile (record_stats_race_end_800A9250, record +0x24).
            // A player without one (a race started from the title screen) counts it in their race copy, which isn't
            // saved: a null profile was a crash.
            uint32_t profile = (uint32_t)MEM_W(record_profile, record);
            if (profile < 0x80000000u || profile + profile_size > 0x80800000u) {
                MEM_W(record_profile, record) = (int32_t)(guest_profiles + (local_player + i) * profile_size);
            }
        }
        // The post-race counts each player's stunt points and best (record_stats_race_end_800A9250 and the stunt
        // results read them by record): this machine's players' go to records 0 and up.
        if (MEM_W(0, (int32_t)race_type) == race_type_stunt) {
            std::string points_text;
            for (int p = 0; p < race_players; p++) {
                points_text += " " + std::to_string(
                    (uint16_t)MEM_H(p * 2, (int32_t)rush2::players4::moved(stunt_points)));
            }
            rush2::online::session_log("[online] stunt points:%s\n", points_text.c_str());
        }
        for (int i = 0; local_player != 0 && i < local_count; i++) {
            uint32_t points = rush2::players4::moved(stunt_points);
            uint32_t best = rush2::players4::moved(stunt_best);
            MEM_H(i * 2, (int32_t)points) = MEM_H((local_player + i) * 2, (int32_t)points);
            MEM_B(i, (int32_t)best) = MEM_B(local_player + i, (int32_t)best);
        }
        // The post-race and the menus are built for two players (src/players4.cpp, rush2_players4_state clamps a local
        // race of 3 or 4 the same way): with 4 the menus' sky (render_sky_cloud_scroll_8008918C, per-view arrays of
        // two) read view 2's matrix pointer from other data, a crash once the post-race was over.
        MEM_H(0, (int32_t)num_players) = (int16_t)std::min(local_count, 2);
        MEM_H(0, (int32_t)num_views) = (int16_t)std::min(local_count, 2);
        // race_setup_800A5ADC saved the race's player count (0x800A628C), which the menus get back after the post-race
        // (menu_title_start_pressed_800A6B58 sets the count and players 1 and up's ports from 0x800BF314): with the
        // race's count every machine came back with as many players as the race had, one more after every race.
        MEM_W(0, (int32_t)race_saved_players) = std::min(local_count, 2);
        rush2::car2049::clear_guest_side_blocks();
        enable_menu_ports(rdram);
        phase = Phase::Idle;
    }

    // Leaves the online race for the menus, as the pause menu's Quit does (func_800AFD44 at 0x800B0040), and puts back
    // the menus' records, cars and settings. Run at the top of the game thread's frame, where the pause step runs.
    void end_race(uint8_t* rdram, recomp_context* ctx, bool in_game) {
        lockstep = false;
        fixed_frame_time = false;
        if (in_game) {
            MEM_B(0, (int32_t)race_restart_flag) = 0;
            // To screen 0xF, as the pause menu's QUIT goes (which online races leave by too). Starting over from the
            // boot logos (screen 0xE) ran the power-on save load again (save_boot_load_settings_800B3528): its pak scan
            // found no pak, the players' profiles were gone from the menus, and the host's game hung in the loading
            // screen (loading_screen_wait_frame_80088C64) after a race given up on.
            MEM_B(0, (int32_t)front_end_from_logos) = 0;
            recomp_context saved = *ctx;
            race_end_to_menu_800AAC64(rdram, ctx);
            *ctx = saved;
        }
        rush2::battle::set_online_seed(0);
        if (!menu_snapshot.empty()) {
            rush2::battle::set_points_to_win(saved_battle_points);
            rush2::battle::set_time_limit_minutes(saved_battle_minutes);
            rush2::battle::set_race_battle(saved_race_battle);
            size_t at = 0;
            for (const SavedRange& r : saved_ranges) {
                for (uint32_t i = 0; i < r.size; i++) {
                    MEM_B(0, (int32_t)(r.address + i)) = (int8_t)menu_snapshot[at++];
                }
            }
            menu_snapshot.clear();
        }
        MEM_W(0, (int32_t)race_saved_players) = std::min(local_count, 2);
        rush2::car2049::clear_guest_side_blocks();
        enable_menu_ports(rdram);
        phase = Phase::Idle;
    }

    // The race ended without the game ending it (the host left, a desync, a load mismatch): back to the menus as the
    // pause menu's QUIT goes, and, if the lobby is still there, back to the track select with it (race_finished).
    void race_ended_under_us(uint8_t* rdram, recomp_context* ctx) {
        rush2::online::session_log("[online] race ended: %s\n", last_error().c_str());
        end_race(rdram, ctx, phase != Phase::WaitMenus);
        race_finished();
        if (state() == State::Lobby) {
            rush2::online::game::race_over();
        }
    }
}

namespace {
    // A player's held buttons as the game makes them from a controller: the stick past +-0x38 also holds a d-pad
    // direction.
    uint16_t held_buttons(const PlayerInput& in) {
        uint16_t held = in.buttons;
        if (in.stick_x < -stick_dpad_threshold) held |= stick_left;
        if (in.stick_x > stick_dpad_threshold) held |= stick_right;
        if (in.stick_y < -stick_dpad_threshold) held |= stick_down;
        if (in.stick_y > stick_dpad_threshold) held |= stick_up;
        return held;
    }
}

// Top of the game thread's frame (func_800B0228's loop), once per frame in menus and races.
extern "C" void rush2_online_frame(uint8_t* rdram, recomp_context* ctx) {
    game_frame++;
    rush2::ram_watch::frame(rdram, game_frame);
    rush2::online::update();
    test_aid(rdram);

    State st = state();
    if (phase == Phase::Idle && st == State::Loading) {
        phase = Phase::WaitMenus;
    }
    // The test aids' quick races skip the menus (and the lobby that leads players through them).
    static const bool quick_test = std::getenv("RUSH2_ONLINE_TEST") != nullptr &&
        std::string(std::getenv("RUSH2_ONLINE_TEST")).rfind("menus:", 0) != 0;
    if (!quick_test) {
        rush2::online::game::lobby_frame(rdram);
    }
    if (phase != Phase::Idle && !racing()) {
        // The race ended under us: the host left (the session is gone), or a desync or a load mismatch ended it
        // (online_net.cpp's end_race: the lobby stays).
        race_ended_under_us(rdram, ctx);
        return;
    }
    int32_t now_state = MEM_W(0, (int32_t)game_state);
    // Frames the car select has been up: a race starts there once it has settled (below).
    static int car_select_frames = 0;
    car_select_frames = now_state == state_car_select ? car_select_frames + 1 : 0;
    // Back in the menus after an online race: the mod car slots it lent get this machine's own cars back.
    if (phase == Phase::Idle && (now_state == state_front_end || now_state == 1)) {
        rush2::car2049::return_mod_slots(rdram);
    }
    if (phase == Phase::Running && (now_state == state_post_race || now_state == state_front_end)) {
        // Race over, or QUIT from the pause menu: every machine gets here on the same frame and leaves the lockstep.
        // The post-race (saving to the profiles, the track's record tables, name entry) runs on each machine for its
        // own player only.
        rush2::online::session_log("[online] race %s at frame %u\n", now_state == state_front_end ? "quit" : "over",
            race_frame);
        finish_race_locally(rdram, now_state == state_front_end);
        race_finished();
        rush2::online::game::race_over();
        return;
    }
    if (phase == Phase::WaitMenus) {
        // From the car select, the race starts there (rush2::online::game::car_select_done); from anywhere else in
        // the menus (the test aids), here.
        constexpr int car_select_settle = 15;
        if (MEM_W(0, (int32_t)game_state) == state_car_select && (quick_test || car_select_frames >= car_select_settle)) {
            // Everyone has picked a car: the race starts as the car select starts it once all its players have
            // (func_803B9478 at 0x803B96D4-0x803B96F4), with the race set up first. A car select that only just came
            // up (a player who backed out to change car as the host started is walked back in, online_lobby.cpp) gets
            // a few frames to set itself up first.
            setup_race(rdram, ctx, true);
            phase = Phase::Loading;
            recomp_context saved = *ctx;
            music_queue_stop_80062FA4(rdram, ctx);
            MEM_W(0, (int32_t)game_state) = 7;
            ctx->r4 = 0;
            carselect_build_list_803B81F0(rdram, ctx);
            *ctx = saved;
            ctx->r4 = 0;
            overlay_load_800A62C0(rdram, ctx);
            *ctx = saved;
        }
        else if (in_menus(rdram) && (quick_test || MEM_W(0, (int32_t)game_state) != 1)) {
            // Anywhere else in the menus (the test aids' quick races, the title screen) the race starts from the front
            // end. A track select is walked on into the car select instead (online_lobby.cpp), whose race start
            // leaves the menus as a local race does.
            setup_race(rdram, ctx, false);
            phase = Phase::Loading;
        }
        return;
    }
    if (phase == Phase::Loading) {
        if (MEM_HU(0, (int32_t)physics_flag) == 0) {
            return;
        }
        // The race's first physics frame: every machine reports what it set up, then waits for everyone.
        if (break_test() == "cars") {
            // Test aid: car 0's own descriptor gets half again its drag (+0x60), as a car set up differently here.
            uint32_t desc = (uint32_t)MEM_W(0, (int32_t)sync_cars);
            if (desc >= 0x80000000u && desc < 0x80800000u) {
                uint32_t bits = (uint32_t)MEM_W(0x60, (int32_t)desc);
                float drag;
                std::memcpy(&drag, &bits, 4);
                drag *= 1.5f;
                std::memcpy(&bits, &drag, 4);
                MEM_W(0x60, (int32_t)desc) = (int32_t)bits;
            }
        }
        if (break_test() == "stale") {
            // Test aid: the slots after the race's cars hold what an earlier race here left, its car on its own
            // descriptor copy (car_descs 0x80225000 + slot x 0xEC, rush2_car49_setup_desc).
            for (int car = race_cars(rdram); car < 8; car++) {
                uint32_t physics_car = sync_cars + car * sync_car_size;
                MEM_W(0, (int32_t)physics_car) = (int32_t)(0x80225000u + car * 0xECu);
                MEM_B(0x7EA, (int32_t)physics_car) = 1;
            }
        }
        uint32_t hash = state_hash(rdram);
        uint32_t track = track_fingerprint(rdram);
        uint32_t cars = cars_fingerprint(rdram);
        rush2::online::session_log("[online] race loaded at frame %u, state %08X, track %08X, cars %08X (%d players, %d "
            "computer cars)\n", game_frame, hash, track, cars, MEM_H(0, (int32_t)num_players),
            MEM_H(0, (int32_t)0x800D3E90));
        // Each car's part of the fingerprint, so two machines' logs show which car was set up differently.
        for (int car = 0; car < race_cars(rdram); car++) {
            uint32_t physics_car = sync_cars + car * sync_car_size;
            int type = MEM_BU(0x7EA, (int32_t)physics_car);
            bool human = MEM_BU(0x7E8, (int32_t)physics_car) == 2;
            int row = human ? MEM_H(0x7E0, (int32_t)physics_car) + 1 : 0;
            rush2::online::session_log("[online]   car %d: type %d, row %d, physics %08X\n", car, type, row,
                rush2::car2049::physics_hash(rdram, type, row, 2166136261u));
        }
        dump_full(rdram, "load");
        if (break_test() == "load") {
            hash ^= 1;
        }
        if (break_test() == "track") {
            track ^= 1;
        }
        if (break_test() != "noload") {
            report_loaded(hash, track, cars);
        }
        phase = Phase::Running;
        lockstep = true;
        race_frame = 0;
    }
    if (!lockstep) {
        return;
    }

    // This frame's inputs: send ours (for `delay` frames ahead) and wait for everyone's. This machine's player i plays
    // with the controller on port i + 1 (the menus' player i + 1).
    std::vector<PlayerInput> locals(local_count);
    static const char* bot = std::getenv("RUSH2_ONLINE_BOT");
    for (int i = 0; i < local_count; i++) {
        PlayerInput& local = locals[i];
        if (bot != nullptr) {
            local = bot_input(race_frame, std::atoi(bot) + i * 7919);
        }
        else if (input_source != nullptr) {
            uint16_t buttons = 0;
            float x = 0.0f, y = 0.0f;
            input_source(i, &buttons, &x, &y);
            local.buttons = buttons;
            convert_to_n64_range(x, y, local.stick_x, local.stick_y);
            float gas = 0.0f, brake = 0.0f;
            rush2::input::get_pedals(i, &gas, &brake);
            local.gas = uint8_t(std::lround(std::clamp(gas, 0.0f, 1.0f) * 255.0f));
            local.brake = uint8_t(std::lround(std::clamp(brake, 0.0f, 1.0f) * 255.0f));
            local.battle = rush2::controls::battle_buttons(i);
        }
    }
    submit_local_input(race_frame + input_delay(), locals);
    Wait got;
    bool resync = false;
    while ((got = wait_frame_inputs(race_frame, slot_inputs.data(), 250, &resync)) == Wait::Timeout) {
        rush2::online::game::race_notice(race_status());
    }
    // By lobby slot to by race player (setup_race's order). A slot without a player left the lobby before the race.
    for (int p = 0; p < max_players; p++) {
        frame_inputs[p] = p < (int)race_order.size() ? slot_inputs[race_order[p]] : PlayerInput{};
    }
    if (got == Wait::Ended) {
        race_ended_under_us(rdram, ctx);
        return;
    }
    // A resync (the machines' races differed): this frame starts from the host's race state on every machine.
    if (resync) {
        if (is_host()) {
            send_snapshot(race_frame, capture_state(rdram));
            desync_log("[online] resync at frame " + std::to_string(race_frame) + ": sent this race's state\n");
        }
        else {
            std::vector<uint8_t> state;
            Wait w;
            while ((w = wait_snapshot(race_frame, &state, 250)) == Wait::Timeout) {
                rush2::online::game::race_notice("Resyncing the race with the host...");
            }
            if (w == Wait::Ended) {
                race_ended_under_us(rdram, ctx);
                return;
            }
            apply_state(rdram, state, race_frame);
        }
    }
    if (break_test().rfind("nudge:", 0) == 0 && race_frame == (uint32_t)std::atoi(break_test().c_str() + 6)) {
        // The race's random seed (kept odd): every later draw differs from the other machines'.
        MEM_W(0, (int32_t)rng_seeds[0]) = MEM_W(0, (int32_t)rng_seeds[0]) ^ 2;
        rush2::online::session_log("[online] test: changed the race seed at frame %u\n", race_frame);
    }
    // What the race waits for or a notice (a resync) for a few seconds; nothing otherwise.
    rush2::online::game::race_notice(race_status());

    // Test aid: RUSH2_ONLINE_TRACE=DIR writes the cars' physics structs (8 x 0x81C bytes) before every frame's ticks to
    // DIR/cars.bin, one record per frame, to find the first frame and field where two runs part.
    static const char* trace_dir = std::getenv("RUSH2_ONLINE_TRACE");
    if (trace_dir != nullptr) {
        static FILE* trace = nullptr;
        if (trace == nullptr) {
            std::string path = std::string(trace_dir) + "/cars.bin";
            trace = std::fopen(path.c_str(), "wb");
        }
        if (trace != nullptr) {
            uint8_t cars[8 * 0x81C];
            for (uint32_t i = 0; i < sizeof(cars); i++) {
                cars[i] = (uint8_t)MEM_BU(0, (int32_t)(0x800F5470u + i));
            }
            std::fwrite(cars, 1, sizeof(cars), trace);
            std::fflush(trace);
        }
    }

    // Test aid: RUSH2_ONLINE_DUMP_AT=F1,F2,... writes all of RDRAM before those frames' ticks to
    // RUSH2_ONLINE_DUMP/full_F.bin (N64 byte order), to diff two machines' whole memory (tools/ram_diff.py).
    static const char* dump_at = std::getenv("RUSH2_ONLINE_DUMP_AT");
    if (dump_at != nullptr) {
        std::string list = std::string(",") + dump_at + ",";
        if (list.find("," + std::to_string(race_frame) + ",") != std::string::npos) {
            dump_full(rdram, std::to_string(race_frame));
        }
    }

    // The main loop counts every frame since boot (0x8011962C, game_main_loop_frame_800B0228 after race_start_timer_
    // 800AE670), and car_state_draw_objects_80082D14 picks which of the cars' part animations run from its low two bits
    // (the animations step car state bytes and draw from the race's random seed). The machines boot and load at their
    // own times, so their counts differed (out of sync once the first part animated, on three runs in four): online it
    // is the lockstep frame.
    MEM_W(0, (int32_t)frame_count) = (int32_t)race_frame;

    // The buttons each player pressed this frame (+2, and +6, the menus' repeat). Online records have no port, so the
    // frame start (func_80080788) clears them before the gear shifts and the like read them (func_80080524): it gets
    // them back there next frame (rush2_online_record_presses), and the rest of this frame has them now.
    for (int p = 0; p < race_players; p++) {
        int32_t record = (int32_t)(player_records + p * player_record_size);
        uint16_t held = held_buttons(frame_inputs[p]);
        uint16_t pressed = held & (uint16_t)~previous_held[p];
        MEM_H(record_pressed, record) = (int16_t)pressed;
        MEM_H(record_repeat, record) = (int16_t)pressed;
        pending_pressed[p] = pressed;
        previous_held[p] = held;
    }

    // Two physics ticks, each with its input step, unless the race is paused (the pause menu clears the flag, as it
    // stops the input thread's ticks; a RESTART's loading clears it too).
    if (MEM_HU(0, (int32_t)physics_flag) != 0) {
        recomp_context saved = *ctx;
        ticking = true;
        for (int tick = 0; tick < 2; tick++) {
            physics_thread_step_once_80076D70(rdram, ctx);
            *ctx = saved;
        }
        ticking = false;
        // The ticks' input steps set the presses from the records' ports (none): the rest of the frame (the pause
        // menu, the camera's VIEW) has the players' presses back.
        for (int p = 0; p < race_players; p++) {
            int32_t record = (int32_t)(player_records + p * player_record_size);
            MEM_H(record_pressed, record) = (int16_t)pending_pressed[p];
            MEM_H(record_repeat, record) = (int16_t)pending_pressed[p];
        }
    }

    // Every 30 frames the machines compare their states (online_net.cpp ends the race if they differ).
    if (race_frame % 30 == 0) {
        uint32_t hash = state_hash(rdram);
        dump_state(rdram, race_frame);
        if (std::isdigit((unsigned char)break_test().c_str()[0]) && race_frame >= (uint32_t)std::atoi(break_test().c_str())) {
            hash ^= 1;
        }
        report_state_hash(race_frame, hash);
        static const bool log_hashes = std::getenv("RUSH2_ONLINE_HASHLOG") != nullptr;
        if (log_hashes) {
            rush2::online::session_log("[online] frame %u hash %08X\n", race_frame, hash);
        }
    }
    race_frame++;
}

// The frame setup has just measured the frame (func_800B4EB0, $s2 = the retrace count's address): online, every frame
// is two retraces.
extern "C" void rush2_online_frame_time(uint8_t* rdram, recomp_context* ctx) {
    if (!fixed_frame_time) {
        return;
    }
    set_fixed_frame_time(rdram);
}


// The input thread is about to run its input step and physics tick (func_80076DE0 at 0x8007728C): online the game
// thread runs them instead.
extern "C" int rush2_online_skip_physics(uint8_t* rdram, recomp_context* ctx) {
    return racing() ? 1 : 0;
}

// The input step's per-player loop (func_80076A00 at 0x80076CE0, $s3 = the player record, $s5 = the player) has
// filled the record from the player's controller port: online it gets the player's input for the frame instead.
extern "C" void rush2_online_player_input(uint8_t* rdram, recomp_context* ctx) {
    if (!ticking) {
        return;
    }
    int player = (int)ctx->r21;
    if (player < 0 || player >= max_players) {
        return;
    }
    int32_t record = (int32_t)ctx->r19;
    const PlayerInput& in = frame_inputs[player];
    uint16_t held = held_buttons(in);
    int8_t* range = stick_range[player];
    const int8_t stick[2] = { in.stick_x, in.stick_y };
    for (int axis = 0; axis < 2; axis++) {
        int magnitude = stick[axis] < 0 ? -stick[axis] : stick[axis];
        if (range[axis] < magnitude && range[axis] < max_range) {
            range[axis]++;
        }
    }
    MEM_H(record_held, record) = (int16_t)held;
    MEM_B(record_stick, record) = stick[0];
    MEM_B(record_stick + 1, record) = stick[1];
    MEM_B(record_range, record) = range[0];
    MEM_B(record_range + 1, record) = range[1];
}

// A car's view (car state +0x347, -1 none) also forks the simulation: a wrecked car with a view has its velocity
// damped while it steers to the respawn point (func_800754B4 at 0x800757AC) and takes the respawn camera path, with
// its own timing, in the respawn wait (func_8008E40C at 0x8008E4C4). Each machine has a view only on its own car, so
// online every human car takes the viewed path on every machine; the cars without a view on this machine are given
// view 64. That path plays the respawn sounds (menu_play_sound_80064908, which draws from the race's random seed
// 0x800BD808: the reason the paths must match), sets the car's camera mode (+0x348 = 4, +0x349 its old one) and copies
// the view's camera position (0x800E79D0 + view x 0x40 + 0x24, read) into its camera state (players4 moved it to
// 0x80240390 + view x 0x30, +0x24-+0x2F written). With up to four local views no view below 4 is spare; view 64 writes
// at 0x80240FB4-0x80240FBF, unused RDRAM of players4's window (0x80240850-0x80248000 free). The hooks pass the car
// state and the view just loaded and get back the view to use.
constexpr int scratch_view = 64;

extern "C" int rush2_online_sim_view(uint8_t* rdram, uint32_t car_state, int view) {
    if (phase == Phase::Idle || view >= 0) {
        return view;
    }
    int car = MEM_B(0x346, (int32_t)car_state);
    return car >= 0 && car < race_players ? scratch_view : view;
}

// The camera update (func_8009D9F4) places a human car's body (its scene node, car state +0xF0) in the loop over the
// views, for the car each view follows (func_8009D02C at 0x8009D08C-0x8009D0F4: the car's position, raised by the f32
// at 0x800CFD84, and its matrix, car state +0x48, through func_8009ABDC), and a computer car's after it
// (0x8009DFD8-0x8009E084, skipping cars whose physics car +0x7E8 is 2, human). Online each machine has one view, so
// the other players' bodies would stay where they started, only their shadows moving. At 0x8009E01C ($t2 = +0x7E8 of
// car $s0) a human car without a view is placed as a view places its car, and still skips the computer cars' path.
// Drawing only.
extern "C" void car_matrix_copy_and_apply_8009ABDC(uint8_t* rdram, recomp_context* ctx);

extern "C" int rush2_online_body_kind(uint8_t* rdram, recomp_context* ctx, int car, int kind) {
    constexpr int human = 2;
    constexpr uint32_t body_raise = 0x800CFD84;     // f32
    constexpr uint32_t place_scratch = 0x80E14000;  // A view's 0x40-byte pose block (0x800E79D0 + view x 0x40)
    if (phase == Phase::Idle || kind != human || car < 0 || car >= race_players) {
        return kind;
    }
    uint32_t state = car_states + car * car_state_size;
    if (MEM_B(0x347, (int32_t)state) >= 0) {
        return kind;
    }
    float y, raise;
    uint32_t bits = (uint32_t)MEM_W(4, (int32_t)state);
    std::memcpy(&y, &bits, 4);
    bits = (uint32_t)MEM_W(0, (int32_t)body_raise);
    std::memcpy(&raise, &bits, 4);
    y += raise;
    std::memcpy(&bits, &y, 4);
    MEM_W(0x24, (int32_t)place_scratch) = MEM_W(0, (int32_t)state);
    MEM_W(0x28, (int32_t)place_scratch) = (int32_t)bits;
    MEM_W(0x2C, (int32_t)place_scratch) = MEM_W(8, (int32_t)state);
    recomp_context saved = *ctx;
    ctx->r4 = (gpr)(int32_t)state;
    ctx->r5 = (gpr)(int32_t)(place_scratch + 0x24);
    ctx->r6 = (gpr)(int32_t)(state + 0x48);
    car_matrix_copy_and_apply_8009ABDC(rdram, ctx);
    *ctx = saved;
    return kind;
}

// The wheel placer (carselect_preview_update_8005A598, per car per frame) shows or hides a car's wheel nodes through
// node_swap_visibility_8005A528: for a car with a view (+0x347) it hides them in that view's own close cameras, and for
// a car without one it shows them in views 0 and 1 only when $s0 (car state +0x348 >= 2) is set and hides them
// otherwise. Locally every human car has a view; online the other players' don't, and their wheels were hidden.
// Online a human car without a view shows its wheels, as the computer cars' are.
extern "C" int rush2_online_wheels_shown(uint8_t* rdram, uint32_t physics_car, int view, int shown) {
    constexpr int human = 2;
    if (phase == Phase::Idle || view >= 0 || MEM_B(0x7E8, (int32_t)physics_car) != human) {
        return shown;
    }
    return 1;
}

// The camera update's loop over the views (func_8009D9F4 from 0x8009DA60, $s5 the view, $s4 its player's record,
// stepped by 0x28) switches the view's camera when its player presses VIEW (record +0x1A, the button's mask, against
// +2, the presses). Online view i is this machine's player i's, whose record is the one of their car.
extern "C" uint32_t rush2_online_view_record(uint8_t* rdram, int view, uint32_t record) {
    if (phase == Phase::Idle || view < 0 || view >= local_count) {
        return record;
    }
    return player_records + (local_player + view) * player_record_size;
}

// With two or more players each human car gets an ARROW polygon over it (race_load_car_assets_80086CA4 at
// 0x80086FF8-0x80087048), drawn in the other player's view: its flags are 0x200 | 1 << (1 - car), the views it is
// drawn in, and its update (func_80059B9C at 0x80059BF4) sizes it by the distance from view (car ^ 1)'s camera. Both
// assume two players with a view each. Online the arrows are src/arrows.cpp's (one per view for every other player's
// car), which hides these; they are kept to this machine's views other than the car's own and sized from the first.
extern "C" uint32_t rush2_online_arrow_views(uint8_t* rdram, int car, uint32_t flags) {
    if (phase == Phase::Idle) {
        return flags;
    }
    uint32_t views = 0;
    for (int v = 0; v < local_count; v++) {
        views |= local_player + v == car ? 0 : 1u << v;
    }
    return 0x200 | views;
}

// $a1 = car ^ 1, the other player's view.
extern "C" int rush2_online_arrow_view(uint8_t* rdram, int view) {
    if (phase == Phase::Idle) {
        return view;
    }
    int car = view ^ 1;
    return car == local_player && local_count > 1 ? 1 : 0;
}

// The frame start's per-player loop (func_80080788 at 0x80080B0C, $a2 = the record) has set the record's presses from
// its port, none online: they become the player's presses of the last lockstep frame, before func_80080524 shifts
// gears and the like from them.
extern "C" void rush2_online_record_presses(uint8_t* rdram, recomp_context* ctx) {
    if (!lockstep) {
        return;
    }
    uint32_t record = (uint32_t)ctx->r6;
    int p = (int)((record - player_records) / player_record_size);
    if (record < player_records || p >= race_players) {
        return;
    }
    MEM_H(record_pressed, (int32_t)record) = (int16_t)pending_pressed[p];
    MEM_H(record_repeat, (int32_t)record) = (int16_t)pending_pressed[p];
    pending_pressed[p] = 0;
}

// The pause menu step (func_800AFD44 at 0x800AFDE0): a player didn't press START; the game pauses anyway if their
// port's controller (0x8010C150[player], the pad at 0x80023044 + 6 x player) reports the bit 8. Online that would read
// this machine's controllers, which differ by machine: only the players' START (their lockstep presses) pauses.
extern "C" int rush2_online_pause_start_only(uint8_t* rdram, recomp_context* ctx) {
    return lockstep ? 1 : 0;
}

// The race setup (func_800A5ADC at 0x800A5FD0) has just set the view count to the player count, for the split screen
// it sets up next (func_80054BB0): online each machine has a view per player of its own (at the race start and at a
// RESTART).
extern "C" void rush2_online_race_views(uint8_t* rdram, recomp_context* ctx) {
    if (phase != Phase::Idle) {
        MEM_H(0, (int32_t)num_views) = (int16_t)local_count;
    }
}

bool rush2::online::game::read_local_controller(uint16_t* buttons, float* x, float* y) {
    return input_source != nullptr && input_source(0, buttons, x, y);
}

bool rush2::online::game::in_race() {
    return phase != Phase::Idle;
}

// The race's roster as it started (a dropped player's slot stays used), so every machine answers alike.
bool rush2::online::game::race_has_game(uint8_t game) {
    if (phase == Phase::Idle) {
        return true;
    }
    for (const RosterEntry& e : race_roster()) {
        if (e.used && (e.games & game) == 0) {
            return false;
        }
    }
    return true;
}

int rush2::online::game::local_race_players() {
    return phase == Phase::Idle ? 0 : local_count;
}

int rush2::online::game::local_car(int i) {
    return phase == Phase::Idle || i < 0 || i >= local_count ? -1 : local_player + i;
}

int rush2::online::game::race_players() {
    return phase == Phase::Idle ? 0 : ::race_players;
}

bool rush2::online::game::engine_slot_car(int slot, int* car) {
    if (phase == Phase::Idle) {
        return false;
    }
    *car = slot >= 0 && slot < std::min(local_count, 2) ? local_player + slot : -1;
    return true;
}

// Online circuits. A circuit's state is the profile's circuit block (0x60 bytes, pointer 0x800D3E40: +1 flags and
// difficulty, +2 races finished, +4 seed, +8 the cars' places), its races (0x800D3A60, 4 bytes each: track, direction,
// fog, wind), the standings (0x800D3B28, 0x58 bytes a car: places, points, the computer cars' skill) and the race index
// 0x800D3DF0, plus the length (src/circuit.cpp). The host sends its own with the menu and the race start; every other
// machine copies it into a block of its own at 0x80E15600 (the 0x800D3E40 pointer is moved there as the circuit menu
// sets it up, rush2_online_circuit_block), so its profile's saved circuit is never touched. Each race's results then
// change every machine's copy alike (func_800A8D1C), and the next race's start sends the host's again.
namespace {
    constexpr size_t circuit_data_size = circuit_block_size + circuit_races_size + circuit_standings_size + 2;
    std::atomic<int> followed_length = -1;

    void copy_out(uint8_t* rdram, std::vector<uint8_t>& out, uint32_t address, uint32_t size) {
        for (uint32_t i = 0; i < size; i++) {
            out.push_back((uint8_t)MEM_BU(0, (int32_t)(address + i)));
        }
    }

    size_t copy_in(uint8_t* rdram, const std::vector<uint8_t>& in, size_t at, uint32_t address, uint32_t size) {
        for (uint32_t i = 0; i < size; i++) {
            MEM_B(0, (int32_t)(address + i)) = (int8_t)in[at + i];
        }
        return at + size;
    }
}

std::vector<uint8_t> rush2::online::game::read_circuit(uint8_t* rdram) {
    std::vector<uint8_t> out;
    uint32_t block = (uint32_t)MEM_W(0, (int32_t)circuit_block_ptr);
    if (MEM_W(0, (int32_t)race_type) != 1 || block < 0x80000000u || block >= 0x80800000u) {
        return out;
    }
    out.reserve(circuit_data_size);
    copy_out(rdram, out, block, circuit_block_size);
    copy_out(rdram, out, circuit_races, circuit_races_size);
    copy_out(rdram, out, circuit_standings, circuit_standings_size);
    out.push_back((uint8_t)MEM_BU(0, (int32_t)circuit_race_index));
    out.push_back((uint8_t)std::clamp(rush2::circuit::length(rdram), 1, 255));
    return out;
}

void rush2::online::game::follow_circuit(uint8_t* rdram, const std::vector<uint8_t>& circuit) {
    if (circuit.size() != circuit_data_size) {
        followed_length = -1;
        return;
    }
    size_t at = copy_in(rdram, circuit, 0, followed_block, circuit_block_size);
    MEM_W(0, (int32_t)circuit_block_ptr) = (int32_t)followed_block;
    at = copy_in(rdram, circuit, at, circuit_races, circuit_races_size);
    at = copy_in(rdram, circuit, at, circuit_standings, circuit_standings_size);
    MEM_B(0, (int32_t)circuit_race_index) = (int8_t)circuit[at];
    followed_length = circuit[at + 1];
}

int rush2::online::game::circuit_length() {
    return followed_length.load();
}

// func_803B7018 (the circuit menu, screen 0xA) at 0x803B7110, just after it pointed 0x800D3E40 at the profile's
// circuit block ($t8): a machine following the host's circuit keeps its own copy instead.
extern "C" void rush2_online_circuit_block(uint8_t* rdram, recomp_context* ctx) {
    if (followed_length.load() < 0) {
        return;
    }
    MEM_W(0, (int32_t)circuit_block_ptr) = (int32_t)followed_block;
    ctx->r24 = (gpr)(int32_t)followed_block;
}

// func_800A8D1C (a circuit race's results, from the post-race) at 0x800A8D8C: it has pointed 0x800D3E40 at the profile's
// circuit block again (record 0x800D39D2's profile + players * 0x60 + 0x464), and goes on through the pointer. A
// machine following the host's circuit scores the race into its copy, never into the player's saved circuit.
// circuit_generate_800A7DCC at 0x800A7E40: it too points 0x800D3E40 at the profile's block, and works through $s7 (the
// results run it after every race, the circuit menu on CONTINUE). A machine following the host's circuit generates
// from its copy. The block's other users read the pointer; only the host starts a circuit or steps its DEATHS
// (func_800B2A58, func_800B29DC, which work out the profile's block too).
extern "C" void rush2_online_circuit_generate(uint8_t* rdram, recomp_context* ctx) {
    if (followed_length.load() >= 0) {
        MEM_W(0, (int32_t)circuit_block_ptr) = (int32_t)followed_block;
        ctx->r23 = (gpr)(int32_t)followed_block;
    }
}

extern "C" void rush2_online_circuit_results(uint8_t* rdram) {
    if (followed_length.load() >= 0) {
        MEM_W(0, (int32_t)circuit_block_ptr) = (int32_t)followed_block;
    }
}

// The per-player sound and rumble effects run for the first players only: players4 points their count at
// clamped_players, min(players shown, 2) (tools/players4/relocate.py, CLAMPED). They also change the race:
// car_sound_update_all_80067358 steps the cars' +0x660 (set by car_physics_step_80071D78 on a shift), scores stunts
// (car_stunt_scoring_update_80065588) and plays sounds that draw from the race's random seed, and
// car_material_effects_update_800663CC sets the wheels' dust bits of +0x7F4. With several players on one machine the
// count differed by machine (a machine with two players stepped car 1's +0x660, the others didn't: out of sync at
// frame 30). Online these functions see 1 on every machine (hooks at their entry and return; they call each other).
namespace {
    int sim_players_depth = 0;
    int16_t saved_clamped_players = 0;
}

extern "C" void rush2_online_sim_players_begin(uint8_t* rdram) {
    if (phase == Phase::Idle) {
        return;
    }
    if (sim_players_depth++ == 0) {
        saved_clamped_players = MEM_H(0, (int32_t)rush2::players4::clamped_players);
        MEM_H(0, (int32_t)rush2::players4::clamped_players) = 1;
    }
}

extern "C" void rush2_online_sim_players_end(uint8_t* rdram) {
    if (sim_players_depth > 0 && --sim_players_depth == 0) {
        MEM_H(0, (int32_t)rush2::players4::clamped_players) = saved_clamped_players;
    }
}

// car_state_draw_objects_80082D14 runs a car state's part objects (+0x108..+0x2FF) and first sets 0x8011748C from the
// last frame's GPU time (0x80023358, which the scheduler sched_thread_main_800009EC sets from osGetTime): 1 when the
// frame was quick (thresholds 0x800CFA10-0x800CFA18 by TV type and player count). With 0 the parts don't animate; with
// 1 their callbacks (carselect_preview_cb_d/e) step car state bytes and draw from the race's random seed 0x800BD808
// (car_preview_part_random_hide_80059450). The GPU time is this machine's own: with two games of four players on one
// PC one machine sometimes had a slow frame the other didn't, and the race parted (out of sync at frame 900, on some
// runs only). Online the switch is 1 on every machine, as on any machine that keeps up.
extern "C" void rush2_online_draw_detail(uint8_t* rdram) {
    if (phase == Phase::Idle) {
        return;
    }
    MEM_B(0, (int32_t)draw_detail) = 1;
}

// Stunts. car_sound_update_all_80067358 scores each of its players' stunts (car_stunt_scoring_update_80065588: rolls,
// air time, the points 0x8010C074 and best 0x8010C068 per player, which the stunt HUD hud_widget_text_cb_player_stats_
// 800B7654 shows and the post-race counts), and sets their wheels' dust (car_material_effects_update_800663CC). Its
// players are the first one online (above), and the scoring's arrays held two players: tools/players4/relocate.py
// moved them to copies for eight. After its loop (0x80067A88) the race's other human cars get the same, on every
// machine: the material effects (which play sounds only for the players the count covers) and the scoring, for a car
// whose engine runs (+0x648 0) while the update's first argument is set and its second clear, as the loop does; for
// any other car, the scoring's state is cleared as car_sound_slots_init_8006722C clears the first two players' (it
// calls stunt_param_store_800671E8 with player 0 or 1, the second only with two players counted).
extern "C" void car_material_effects_update_800663CC(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_stunt_scoring_update_80065588(uint8_t* rdram, recomp_context* ctx);

namespace {
    // The stunt arrays, per player (old addresses: rush2::players4::moved gives the copies), and the stunt HUD's.
    struct StuntArray {
        uint32_t address;
        int32_t size;   // Bytes per player (int: MEM_ offsets must be signed).
    };
    constexpr StuntArray stunt_arrays[] = {
        { 0x800E7CB8, 4 }, { 0x800E7D08, 4 }, { 0x800E7D28, 4 }, { 0x800E7D40, 4 }, { 0x800E7D58, 4 },
        { 0x800E7DE8, 4 }, { 0x800E7DF8, 4 }, { 0x800E7E08, 4 }, { 0x800E7E18, 4 },
        { 0x8010C068, 1 }, { 0x8010C074, 2 }, { 0x8010C0C0, 1 },
        { 0x8010C070, 1 }, { 0x8010C0B4, 1 }, { 0x8010C0D4, 1 }, { 0x8010C0D8, 4 },
    };
    constexpr uint32_t stunt_on = 0x800E7D08;        // s32 per player: scoring has started
    constexpr uint32_t stunt_airborne = 0x800E7DF8;  // s32 per player
    constexpr uint32_t car_physics = 0x800F5470;     // Physics cars, 0x81C each
    constexpr uint32_t car_physics_size = 0x81C;
    uint8_t stunt_saved[std::size(stunt_arrays)][max_players * 4];

    void stunt_reset(uint8_t* rdram, int player) {
        MEM_W(player * 4, (int32_t)rush2::players4::moved(stunt_on)) = 0;
        MEM_W(player * 4, (int32_t)rush2::players4::moved(stunt_airborne)) = 0;
    }

    void stunt_clear(uint8_t* rdram) {
        for (const StuntArray& a : stunt_arrays) {
            uint32_t base = rush2::players4::moved(a.address);
            for (int32_t i = 0; i < max_players * a.size; i++) {
                MEM_B(i, (int32_t)base) = 0;
            }
        }
    }

    // The stunt HUD shows players 0 and up (the player count reads this machine's then): during the presentation each
    // array holds this machine's players first, as the records do, and anything the HUD changed goes back after.
    void stunt_rotate(uint8_t* rdram, bool back) {
        if (local_player == 0 || race_players <= 1) {
            return;
        }
        for (size_t k = 0; k < std::size(stunt_arrays); k++) {
            const StuntArray& a = stunt_arrays[k];
            uint32_t base = rush2::players4::moved(a.address);
            uint8_t* saved = stunt_saved[k];
            if (!back) {
                for (int32_t i = 0; i < race_players * a.size; i++) {
                    saved[i] = (uint8_t)MEM_BU(i, (int32_t)base);
                }
            }
            for (int p = 0; p < race_players; p++) {
                int from = (p + local_player) % race_players;
                for (int32_t b = 0; b < a.size; b++) {
                    if (back) {
                        saved[from * a.size + b] = (uint8_t)MEM_BU(p * a.size + b, (int32_t)base);
                    }
                    else {
                        MEM_B(p * a.size + b, (int32_t)base) = (int8_t)saved[from * a.size + b];
                    }
                }
            }
            if (back) {
                for (int32_t i = 0; i < race_players * a.size; i++) {
                    MEM_B(i, (int32_t)base) = (int8_t)saved[i];
                }
            }
        }
    }
}

extern "C" void rush2_online_extra_player_effects(uint8_t* rdram, recomp_context* ctx) {
    if (phase == Phase::Idle) {
        return;
    }
    uint32_t sp = (uint32_t)ctx->r29;
    bool running = MEM_H(0x7A, (int32_t)sp) != 0;   // The update's arguments, as its loop reads them.
    bool skip = MEM_B(0x7F, (int32_t)sp) != 0;
    int first = MEM_H(0, (int32_t)rush2::players4::clamped_players);
    recomp_context saved = *ctx;
    for (int p = std::max(first, 0); p < race_players; p++) {
        int car = MEM_BU(record_car, (int32_t)(player_records + p * player_record_size));
        uint32_t physics_car = car_physics + car * car_physics_size;
        if (MEM_B(0x648, (int32_t)physics_car) != 0 || !running) {
            stunt_reset(rdram, p);
            continue;
        }
        if (skip) {
            continue;
        }
        int16_t id = MEM_H(0x7E0, (int32_t)physics_car);
        ctx->r4 = (gpr)(int32_t)id;
        car_material_effects_update_800663CC(rdram, ctx);
        *ctx = saved;
        ctx->r4 = (gpr)(int32_t)id;
        ctx->r5 = (gpr)p;
        car_stunt_scoring_update_80065588(rdram, ctx);
        *ctx = saved;
    }
}

// car_sound_slots_init_8006722C's entry ($a0 the car): online it clears the stunt scoring's state of players 2 and up
// too (the race setup and a car's reset call it).
extern "C" void rush2_online_stunt_reset(uint8_t* rdram, int car) {
    if (phase == Phase::Idle || car < 1 || car >= race_players) {
        return;
    }
    stunt_reset(rdram, car);
}

bool rush2::online::game::car_type(int car, int* type) {
    if (phase == Phase::Idle || car < 0 || car >= (int)race_car_types.size()) {
        return false;
    }
    *type = race_car_types[car];
    return true;
}

bool rush2::online::game::wing_style(int car, int* style) {
    if (phase == Phase::Idle || car < 0 || car >= (int)race_wing_styles.size()) {
        return false;
    }
    *style = race_wing_styles[car];
    return true;
}

std::array<uint32_t, max_mod_cars> rush2::online::game::mod_car_checks() {
    std::array<uint32_t, max_mod_cars> checks = rush2::car_mods::online_car_checks();
    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_MODCARS=none reports no mod cars, as a machine without the mods
    // would (its races put stand-ins in the slots).
    static const bool none = std::getenv("RUSH2_ONLINE_MODCARS") != nullptr &&
        std::string(std::getenv("RUSH2_ONLINE_MODCARS")) == "none";
    if (none) {
        checks.fill(0);
    }
    return checks;
}

int rush2::online::game::local_race_player() {
    return phase == Phase::Idle ? -1 : local_player;
}

// The physics tick writes a human car's rumble level to its record's port (0x800D50E8 + port, physics_car_tick_80075880
// at 0x80075AF8 and 0x80075C08), and src/rumble.cpp reads the port the same way. Online every record has no port, so
// nothing rumbled: this machine's player i's level goes to port i, whose controller drives them, and the other
// players' stay on none, as each machine rumbles only for its own players.
int rush2::online::game::rumble_port(uint32_t record, int port) {
    if (phase == Phase::Idle) {
        return port;
    }
    int player = (int)((record - player_records) / player_record_size);
    return record >= player_records && is_local_car(player) ? player - local_player : no_port;
}

// Before physics_car_tick_80075880 adds the record's port ($v0, from $a3 = the record) to the rumble level's address.
extern "C" void rush2_online_rumble_port(uint8_t* rdram, recomp_context* ctx) {
    ctx->r2 = (gpr)(int32_t)rush2::online::game::rumble_port((uint32_t)ctx->r7, (int)(int32_t)ctx->r2);
}

bool rush2::online::game::battle_buttons(int player, uint8_t* out) {
    if (!lockstep || player < 0 || player >= max_players) {
        return false;
    }
    *out = frame_inputs[player].battle;
    return true;
}

// The race's presentation (the HUD's update and the frame's drawing, func_800AE670 0x800AF1D4-0x800AF1F4) is the
// game's players 1 and up, view i being player i + 1's. Online this machine's players drive cars local_player and up,
// so around those calls the records' cars are rotated by local_player (record i shows car i + local_player, wrapping
// round), and this machine's players' records take controller ports 0 and up (every record has no port online, so no
// controller reaches the race). The rest of each record, which the race also writes, stays its own, and no race code
// runs in between, so every machine's race stays the same. The rumble is each machine's own players'
// (rush2::online::game::rumble_port), and so are the engine sounds of Rush 2's two engine voices
// (rush2_online_engine_start, rush2_online_engine_car).
// The player count reads this machine's player count there too (hud_players_begin): the HUD's per-player elements
// show themselves by it (the race time hud_widget_cb_icon_row_a_800B9754, the lap time 800B9978, the deaths 800B9C44,
// the lap banner 800BA868), and with the race's count the other players' showed on this machine's views. Nothing else
// there reads it but the views' fog (view_fog_setup_8007C624, which has rows for 1 and 2 players: players4 clamps it).
namespace {
    bool presenting = false;
    int8_t saved_cars[max_players] = {};
    int8_t saved_ports[max_players] = {};
    int hud_depth = 0;
    int16_t saved_players = 0;
}

void rush2::online::game::hud_players_begin(uint8_t* rdram) {
    if (phase == Phase::Idle) {
        return;
    }
    if (hud_depth++ == 0) {
        saved_players = MEM_H(0, (int32_t)num_players);
        MEM_H(0, (int32_t)num_players) = (int16_t)local_count;
    }
}

// Three HUD elements read their player's physics car and car state by number, where most go through the player's
// record (which rush2_online_present_begin rotates): the gear box (hud_widget_cb_model_lookup_800B92F4, its player in
// bits 8-11 of the element's parameter), the tachometer's lit part (hud_widget_cb_orbit_icons_800B7E88, shown in
// manual) and its background, which also works out the race time (hud_widget_cb_race_time_display_800B7B1C; both with
// the player in the low bits). Online the elements of players 1 to local_count (views 0 and up, players 3 and 4's
// cloned by src/players4.cpp) are set to this machine's players' cars and the others to player 15, and each shows
// itself only while its player is below the count the hooks give them (rush2_online_hud_views, in place of the view
// count): local_player + local_count. The gear box and the tachometer write only their own widget; the race time
// writes its player's digits (0x80240720 + player * 8), which view i's time widget reads at player i's
// (rush2_online_hud_time_digits copies them there). The rewrite is once per element list (a RESTART builds another).
void rush2::online::game::hud_built(uint8_t* rdram) {
    constexpr uint32_t hud_elements = 0x8010C030;  // Head of the element list (+0x3C next, +0x28 callback, +0x2C param)
    constexpr uint32_t gear_box = 0x800B92F4;
    constexpr uint32_t tach = 0x800B7E88;
    constexpr uint32_t tach_back_time = 0x800B7B1C;
    constexpr uint32_t hidden = 0xF;
    uint32_t head = (uint32_t)MEM_W(0, (int32_t)hud_elements);
    if (phase == Phase::Idle || head == 0 || head == hud_rewritten) {
        return;
    }
    hud_rewritten = head;
    // A race's (or a RESTART's) stunts start from nothing for every player: the game clears only the first two's.
    stunt_clear(rdram);
    auto car_of = [](uint32_t player) {
        return player < (uint32_t)local_count ? (uint32_t)(local_player + (int)player) : hidden;
    };
    for (uint32_t e = head; e != 0; e = (uint32_t)MEM_W(0x3C, (int32_t)e)) {
        uint32_t callback = (uint32_t)MEM_W(0x28, (int32_t)e);
        uint32_t param = (uint32_t)MEM_W(0x2C, (int32_t)e);
        if (callback == gear_box) {
            param = (param & ~0xF00u) | (car_of((param >> 8) & 0xF) << 8);
        }
        else if (callback == tach || callback == tach_back_time) {
            param = (param & ~0xFu) | car_of(param & 0xF);
        }
        else {
            continue;
        }
        MEM_W(0x2C, (int32_t)e) = (int32_t)param;
    }
}

extern "C" int rush2_online_hud_views(uint8_t* rdram, int views) {
    return phase == Phase::Idle ? views : local_player + local_count;
}

// Just after hud_widget_cb_race_time_display_800B7B1C formatted player `player`'s race time (its element's parameter,
// a car once hud_built rewrote it): this machine's player i's digits go to view i's place. The elements run in
// player order, so a view's place is written after the car whose digits it held was formatted.
extern "C" void rush2_online_hud_time_digits(uint8_t* rdram, int player) {
    constexpr uint32_t time_digits = 0x80240720;  // 8 bytes per player (race_format_time_digits_8008ECC4's output)
    if (phase == Phase::Idle || local_player == 0 || !is_local_car(player)) {
        return;
    }
    int view = player - local_player;
    for (int32_t i = 0; i < 8; i++) {
        MEM_B(i, (int32_t)(time_digits + view * 8)) = MEM_B(i, (int32_t)(time_digits + player * 8));
    }
}

void rush2::online::game::hud_players_end(uint8_t* rdram) {
    if (hud_depth == 0) {
        return;
    }
    // Put back only the count set here: if the race ended in between (phase Idle) or the game set its own count, that
    // stays.
    if (--hud_depth == 0 && phase != Phase::Idle && MEM_H(0, (int32_t)num_players) == local_count) {
        MEM_H(0, (int32_t)num_players) = saved_players;
    }
}

extern "C" void rush2_online_present_begin(uint8_t* rdram, recomp_context* ctx) {
    if (phase == Phase::Idle || presenting) {
        return;
    }
    for (int p = 0; p < race_players; p++) {
        const int32_t record = (int32_t)(player_records + p * player_record_size);
        saved_cars[p] = MEM_B(record_car, record);
        saved_ports[p] = MEM_B(record_port, record);
    }
    for (int p = 0; p < race_players; p++) {
        const int32_t record = (int32_t)(player_records + p * player_record_size);
        MEM_B(record_car, record) = saved_cars[(p + local_player) % race_players];
        MEM_B(record_port, record) = (int8_t)(p < local_count ? p : no_port);
    }
    stunt_rotate(rdram, false);
    presenting = true;
    rush2::online::game::hud_players_begin(rdram);
}

extern "C" void rush2_online_present_end(uint8_t* rdram, recomp_context* ctx) {
    if (!presenting) {
        return;
    }
    for (int p = 0; p < race_players; p++) {
        const int32_t record = (int32_t)(player_records + p * player_record_size);
        MEM_B(record_car, record) = saved_cars[p];
        MEM_B(record_port, record) = saved_ports[p];
    }
    stunt_rotate(rdram, true);
    presenting = false;
    rush2::online::game::hud_players_end(rdram);
}

// Rush 2's engine voices (src/engine_preview.cpp): two, for the local players 1 and 2. The car setup
// (race_setup_cars_8009E6DC, car index $s4) starts slot 0 for player 1's car and slot 1 for every other player's
// (race_start_player_engine_8009E664, $a0 the slot), and players 3 and up get none (src/car_engines.cpp plays their
// cars). Online player 1 and 2's cars are the first two machines' players', on every machine: this machine's players'
// cars start the slots instead, slot i the car of this machine's player i, with the engine the setup picked for that
// car (0x800E7BBC + slot * 2, written for the slot it chose). Returns true to skip the start. Only the sound: the
// physics' copy of the engine (car +0x7E9) is stored from the setup's own slot after this returns.
extern "C" int rush2_online_engine_start(uint8_t* rdram, recomp_context* ctx) {
    constexpr uint32_t slot_engines = 0x800E7BBC;  // s16 per slot
    int slot = (int16_t)ctx->r4;
    int car = (int32_t)ctx->r20;
    if (phase == Phase::Idle) {
        return slot >= 2 || car >= 2;
    }
    int local = car - local_player;
    if (local < 0 || local >= std::min(local_count, 2)) {
        return 1;
    }
    if (local != slot) {
        MEM_H(local * 2, (int32_t)slot_engines) = MEM_H(slot * 2, (int32_t)slot_engines);
        ctx->r4 = (gpr)local;
    }
    return 0;
}

// Each physics tick hands the engine slots their cars' rpm and load (physics_stage_post_snapshot_8006890C:
// car_engine_sound_set_rpm_load_800650DC with the physics car at 0x80068AD4 for slot 0 and 0x80068B3C for slot 1, from
// record 1's car). Online the slots are this machine's players' cars. Sound only: it writes the slots' rpm and load
// (car_engine_sound_update_80064F70) and the engine sound globals 0x800E7BB2 and 0x800E7BC8, which only that reads.
extern "C" uint32_t rush2_online_engine_car(uint8_t* rdram, int slot, uint32_t physics_car) {
    constexpr uint32_t physics_cars = 0x800F5470;
    constexpr uint32_t physics_car_size = 0x81C;
    int car;
    if (!rush2::online::game::engine_slot_car(slot, &car)) {
        return physics_car;
    }
    // A slot without a car of this machine's is never started (rush2_online_engine_start): it is fed the first one's.
    return physics_cars + (uint32_t)(car >= 0 ? car : local_player) * physics_car_size;
}

// The car select, every player having picked a car (rush2::online::game::car_select_done): true if the race starts
// now. It is set up for the car select to start it (game state 7, as after a local car select).
bool rush2::online::game::start_from_car_select(uint8_t* rdram, recomp_context* ctx) {
    if (phase != Phase::WaitMenus) {
        return false;
    }
    setup_race(rdram, ctx, true);
    phase = Phase::Loading;
    return true;
}
