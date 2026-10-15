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

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cctype>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>

#include "recomp.h"
#include "online.h"
#include "rush2.h"
#include "battle.h"
#include "track2049.h"

// ultramodern's conversion of a controller's stick to the N64 stick's range and shape (ultramodern/src/input.cpp).
void convert_to_n64_range(float x, float y, int8_t& stick_x, int8_t& stick_y);

extern "C" void physics_thread_step_once_80076D70(uint8_t* rdram, recomp_context* ctx);
extern "C" void race_end_to_menu_800AAC64(uint8_t* rdram, recomp_context* ctx);
extern "C" void menu_screen_dispatch_800AE0D4(uint8_t* rdram, recomp_context* ctx);
extern "C" void music_queue_stop_80062FA4(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_build_list_803B81F0(uint8_t* rdram, recomp_context* ctx);
extern "C" void overlay_load_800A62C0(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_get_logo_color_800B2694(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_options_get_byte1_field_800B2640(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_options_get_color_lo_800B2608(uint8_t* rdram, recomp_context* ctx);
extern "C" void car_options_get_color_hi_800B25D0(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_lookup_value_f_800B259C(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_get_stat_value_b_800B2468(uint8_t* rdram, recomp_context* ctx);
extern "C" void carselect_get_stat_value_a_800B2424(uint8_t* rdram, recomp_context* ctx);

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
    std::array<PlayerInput, max_players> frame_inputs{};
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
    constexpr uint32_t track_id = 0x8010C3F0;        // u8
    constexpr uint32_t menu_settings = 0x800D5760;   // +0 laps, +1 drones (func_80094698 copies them to the race)
    constexpr uint32_t car_table = 0x800D9C10;       // 9 bytes per car slot: +1 type, +2 main, +3 accent, +4 stripe,
    constexpr uint32_t car_entry_size = 9;           // +5 stripe color
    constexpr uint32_t demo_flag = 0x800FAE6C;       // u8: the attract demo is running
    constexpr uint32_t view_cars = 0x80240380;       // u8 per view: the car it follows (moved by players4)
    constexpr int32_t record_car = 0x0;
    constexpr int32_t record_port = 0x1;
    constexpr int32_t record_masks = 0xC;            // u16 per action: the N64 buttons bound to it
    constexpr int32_t record_masks_size = 0x12;
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
    // race menu settings and the car view 0 follows.
    struct SavedRange {
        uint32_t address;
        uint32_t size;
    };
    constexpr SavedRange saved_ranges[] = {
        { 0x800C2140, 8 * 0x28 },
        { 0x800D9C10, 8 * 9 },
        { 0x800D5760, 2 },
        { 0x80240380, 1 },
    };
    std::vector<uint8_t> menu_snapshot;
    int saved_battle_points = 0;
    int saved_battle_minutes = 0;
    int local_player = 0;   // This machine's player (car slot) in the race.
    int race_players = 1;
    std::vector<uint8_t> race_car_types;  // Each player's car type (rush2::online::game::car_type).
    uint32_t hud_rewritten = 0;  // The HUD element list rush2::online::game::hud_built last set up.

    // The front end screen up (func_800AE0D4): 0x10 the title screen, 0 the main menu after it, up to 0xD the other
    // menu screens; 0xE is the boot logos, 0x11 and 0x12 the attract demo, -1 none.
    constexpr uint32_t menu_screen = 0x800E7BC4;  // s16
    constexpr int16_t last_menu_screen = 0xD;
    constexpr int16_t title_screen = 0x10;

    // Whether the game is in the menus (title screen, front end, track or car select), where a race can be set up.
    bool in_menus(uint8_t* rdram) {
        int32_t state = MEM_W(0, (int32_t)game_state);
        int16_t screen = MEM_H(0, (int32_t)menu_screen);
        bool front_end = state == 0 && ((screen >= 0 && screen <= last_menu_screen) || screen == title_screen);
        return (front_end || state == 1 || state == 2) && MEM_BU(0, (int32_t)demo_flag) == 0;
    }

    // The per-player rows of the car type tables at 0x80200000 (row 0 the computer cars', row p + 1 player p's, 36
    // types a row): the car's settings, colors, rims and wheel model scales (carselect_preview_update_8005A598 draws a
    // human car's wheels at its row's scale, so an empty row draws none). The car select fills them for its own
    // players from their profiles (carselect_build_list_803B81F0, 0x803B8B10-0x803B8F7C); online it only ever has one,
    // as row 1, so the other players' rows were empty (a Rush 2 car's wheel scales 0) or another player's. The tables
    // that only change how a car looks are filled the same way here for every race player from the profile the race
    // uses, the same bytes on every machine. Only Rush 2's types: a Rush 2049 type's settings live in a side slot of
    // the save profile (src/rush2049/car2049.cpp), which a profile sent from another machine hasn't, and its rows are
    // all set at boot. The settings rows are left alone, as the race checks they match. Only 4 player rows exist.
    void fill_player_rows(uint8_t* rdram, recomp_context* ctx) {
        constexpr int types = 36;
        constexpr int rush2_types = 22;
        constexpr int player_rows = 4;
        struct Table {
            uint32_t address;
            bool is_float;
            void (*get)(uint8_t*, recomp_context*);
        };
        static const Table tables[] = {
            { 0x80201190, false, carselect_get_logo_color_800B2694 },
            { 0x80201248, false, car_options_get_byte1_field_800B2640 },
            { 0x80201300, false, car_options_get_color_lo_800B2608 },
            { 0x802013B8, false, car_options_get_color_hi_800B25D0 },
            { 0x80201020, false, carselect_lookup_value_f_800B259C },  // Rims (the wheels' texture).
            { 0x802006E8, true, carselect_get_stat_value_b_800B2468 },  // Front wheel scale.
            { 0x802009B8, true, carselect_get_stat_value_a_800B2424 },  // Rear wheel scale.
        };
        recomp_context saved = *ctx;
        for (int p = 0; p < race_players && p < player_rows; p++) {
            uint32_t profile = (uint32_t)MEM_W(record_profile, (int32_t)(player_records + p * player_record_size));
            int row = p + 1;
            for (const Table& t : tables) {
                for (int type = 0; type < rush2_types; type++) {
                    ctx->r4 = (gpr)(int32_t)profile;
                    ctx->r5 = (gpr)type;
                    t.get(rdram, ctx);
                    if (t.is_float) {
                        MEM_W(0, (int32_t)(t.address + (row * types + type) * 4)) = (int32_t)ctx->f0.u32l;
                    }
                    else {
                        MEM_B(0, (int32_t)(t.address + row * types + type)) = (int8_t)ctx->r2;
                    }
                }
            }
        }
        *ctx = saved;
    }

    // Sets the race up as the menus would and starts it (game state 7). Players are the roster's used slots in slot
    // order: player i drives car slot i.
    // from_car_select: the car select has every player's car (rush2::online::game::car_select_done) and starts the race
    // itself once this returns; otherwise the race starts from wherever the front end is (the test aids).
    void setup_race(uint8_t* rdram, recomp_context* ctx, bool from_car_select) {
        RaceSettings settings = race_settings();
        std::vector<RosterEntry> roster = race_roster();
        int me = local_slot();
        race_players = 0;
        local_player = 0;
        std::vector<CarChoice> cars;
        std::vector<const std::vector<uint8_t>*> profiles;
        for (int slot = 0; slot < max_players; slot++) {
            if (!roster[slot].used) {
                continue;
            }
            if (slot == me) {
                local_player = race_players;
            }
            cars.push_back(roster[slot].car);
            profiles.push_back(&roster[slot].profile);
            race_players++;
        }
        race_car_types.clear();
        for (const CarChoice& c : cars) {
            race_car_types.push_back(c.type);
        }
        race_players = std::clamp(race_players, 1, (int)max_players);
        hud_rewritten = 0;

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
        if (settings.mode == GameMode::Battle) {
            rush2::battle::set_points_to_win(settings.battle_points);
            rush2::battle::set_time_limit_minutes(settings.battle_minutes);
        }
        rush2::battle::set_online_seed(race_seed() | 1);

        MEM_H(0, (int32_t)num_players) = (int16_t)race_players;
        MEM_H(0, (int32_t)num_views) = 1;
        // The race the host chose in its menus: race type (the race start, func_800AE670 state 7, turns a race on the
        // stunt track, 11, and the Rush 2049 arenas hosted there, into stunt mode itself), track and the track
        // select's settings (laps, drones, options; func_80094698 copies them to the race).
        MEM_W(0, (int32_t)race_type) = settings.race_type;
        // A custom track is this machine's own id for it (the host's track select only starts one every player has).
        int track = settings.track;
        if (!settings.mod_track.empty()) {
            track = rush2::track2049::track_for_online_key(settings.mod_track);
            if (track < 0) {
                std::fprintf(stderr, "[online] this machine hasn't the custom track %s\n", settings.mod_track.c_str());
                track = settings.track;  // The race's load check (report_loaded) ends it.
            }
        }
        MEM_B(0, (int32_t)track_id) = (int8_t)track;
        for (int i = 0; i < (int)settings.menu.size(); i++) {
            MEM_B(i, (int32_t)menu_settings) = (int8_t)settings.menu[i];
        }

        // This machine's player's own profile: record 0's, as Select Player and the car select left it.
        uint32_t own_profile = (uint32_t)MEM_W(record_profile, (int32_t)player_records);
        if (own_profile < 0x80000000u || own_profile + profile_size > 0x80800000u) {
            own_profile = 0;
        }
        // Every machine's records start the same: blank but for the button masks, the game's default layout that the
        // binding tables are locked to (src/controls.cpp). The menus fill them only for players who picked a profile,
        // so a race started from the title screen would have none (no brake, gears or the like). Records 4-7 fit since
        // the globals after the records moved (tools/players4/relocate.py).
        static constexpr uint16_t masks[record_masks_size / 2] = {
            0x8000,  // GAS: A
            0x4000,  // BRAKE: B
            0xFFFE,  // STEERING: the stick (-2)
            0x0010,  // SHIFT UP: R
            0x2000,  // SHIFT DOWN: Z
            0x0004,  // REVERSE: C-down
            0x0008,  // ABORT: C-up
            0x0020,  // VIEW: L
            0x0001,  // HORN: C-right
        };
        for (int p = 0; p < race_players; p++) {
            int32_t record = (int32_t)(player_records + p * player_record_size);
            for (int32_t i = 0; i < (int32_t)player_record_size; i++) {
                MEM_B(i, record) = 0;
            }
            for (int i = 0; i < record_masks_size / 2; i++) {
                MEM_H(record_masks + i * 2, record) = (int16_t)masks[i];
            }
            MEM_B(record_car, record) = (int8_t)p;
            MEM_B(record_port, record) = (int8_t)no_port;
            MEM_B(record_automatic, record) = (int8_t)cars[p].automatic;
            MEM_B(record_range, record) = default_range;
            MEM_B(record_range + 1, record) = default_range;
            // The profile as the player's car select sent it (the same bytes on every machine; this machine's player's
            // own already holds them), its car type set to the car they picked.
            // A player who sent none (the test aids' races) races with a blank one on every machine.
            const std::vector<uint8_t>& sent = *profiles[p];
            bool have = sent.size() == profile_size;
            bool own = p == local_player && own_profile != 0 && have;
            uint32_t profile = own ? own_profile : guest_profiles + p * profile_size;
            // This machine's player's profile is their save itself: it's never copied over (that put back what the
            // game had saved to it since the car select sent it).
            if (!own) {
                for (uint32_t i = 0; i < profile_size; i++) {
                    MEM_B(0, (int32_t)(profile + i)) = have ? (int8_t)sent[i] : 0;
                }
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
        // The one view follows this machine's car; the race setup (func_800A3690) gives that car the view.
        int view_car = local_player;
        if (const char* forced = std::getenv("RUSH2_ONLINE_VIEW_CAR")) {  // Test aid: every machine views this car.
            view_car = std::atoi(forced);
        }
        MEM_B(0, (int32_t)view_cars) = (int8_t)view_car;
        // The front end is left as its loop leaves it for a race (func_800AE288 at 0x800AE428-0x800AE458): the screen up
        // is told it's left ($s1 the screen, $s0 0) and none is up. The title screen keeps its data in the heap the
        // race reuses, and would otherwise be left after the race with that data gone.
        if (!from_car_select && MEM_H(0, (int32_t)menu_screen) >= 0) {
            recomp_context saved = *ctx;
            ctx->r17 = (gpr)(int32_t)MEM_H(0, (int32_t)menu_screen);
            ctx->r16 = 0;
            menu_screen_dispatch_800AE0D4(rdram, ctx);
            *ctx = saved;
        }
        if (!from_car_select) {
            MEM_H(0, (int32_t)menu_screen) = -1;
            MEM_W(0, (int32_t)game_state) = 7;
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
        std::fprintf(stderr, "[online] race set up: %d players, this machine is player %d, track %d\n", race_players,
            local_player + 1, settings.track);
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

    // The state every machine must agree on: the cars' physics and race state and the random seeds.
    uint32_t state_hash(uint8_t* rdram) {
        constexpr uint32_t cars = 0x800F5470;
        constexpr uint32_t car_size = 0x81C;
        uint32_t h = 2166136261u;
        auto add = [&](uint32_t address, uint32_t size) {
            for (uint32_t i = 0; i < size; i++) {
                uint8_t b = (uint8_t)MEM_BU(0, (int32_t)(address + i));
                h = (h ^ b) * 16777619u;
            }
        };
        for (int car = 0; car < 8; car++) {
            add(cars + car * car_size, car_size);
        }
        // The records, but for the profile pointer (+0x24): this machine's player's is their own profile.
        for (int p = 0; p < race_players; p++) {
            add(player_records + p * player_record_size, record_profile);
            add(player_records + p * player_record_size + record_profile + 4, player_record_size - record_profile - 4);
        }
        // The cars' race state, but for the camera's bytes (+0x347 the view, +0x348/+0x349 the camera mode), which
        // differ by machine.
        for (int car = 0; car < 8; car++) {
            uint32_t state = car_states + car * car_state_size;
            add(state, 0x347);
            add(state + 0x34A, car_state_size - 0x34A);
        }
        for (uint32_t seed_address : rng_seeds) {
            add(seed_address, 4);
        }
        return h;
    }

    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_BREAK makes this machine fail a check, for the paths that end a
    // race: "load" reports a different starting state (a load mismatch), "noload" never reports loaded (the host drops
    // the player after its load timeout), a frame number F reports a different state hash from frame F on (a desync).
    const std::string& break_test() {
        static const std::string spec = std::getenv("RUSH2_ONLINE_BREAK") ? std::getenv("RUSH2_ONLINE_BREAK") : "";
        return spec;
    }

    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_BOT=SEED drives this machine's car from the frame number alone
    // (gas held, steering weaving, a brake now and then), so runs repeat exactly.
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
        return in;
    }

    // Test aid (docs/online.md#test-aids): RUSH2_ONLINE_TEST=host:N hosts a lobby and starts a race at once from the
    // title screen once N players are in (skipping the menus); RUSH2_ONLINE_TEST=join:ADDRESS joins and says it has a
    // car. RUSH2_ONLINE_NAME sets the name; RUSH2_ONLINE_TRACK the track select id and RUSH2_ONLINE_RACE_TYPE the race
    // type. RUSH2_ONLINE_TEST=menus:host / menus:join:ADDRESS only hosts or joins, for going through the menus.
    // RUSH2_ONLINE_MANUAL=1 gives this machine's player the manual gearbox in a quick race. RUSH2_ONLINE_PORT hosts on
    // another port.
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
        // RUSH2_ONLINE_MANUAL=1: this machine's player races with the manual gearbox (CarChoice::automatic).
        static bool manual_sent = false;
        if (!manual_sent && std::getenv("RUSH2_ONLINE_MANUAL") != nullptr) {
            manual_sent = true;
            CarChoice car;
            car.automatic = 0;
            set_car(car, {});
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
                    std::fprintf(stderr, "[online] can't start: %s\n", reported.c_str());
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
    // screen 0xF (func_8009FD68). The players' car select needs the profiles they picked, which an online race (set
    // up from the title screen too) may not have.
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
        for (int p = 0; p < players && p < 2; p++) {
            int port = MEM_BU(record_port, (int32_t)(player_records + p * player_record_size));
            if (port < num_ports) {
                MEM_B(0, (int32_t)(input_port_state + port)) = 1;
            }
        }
    }

    // The race is over (game state 6, or the front end after the pause menu's QUIT): the machines part. The post-race
    // works on the game's players 1 and up, so this machine's player becomes the only one, with the record the menus
    // gave them (their profile and controller) and their car; it saves their stats and times and runs their name entry
    // as after a local race. After QUIT the cars go back to the menus' too.
    void finish_race_locally(uint8_t* rdram, bool quit) {
        lockstep = false;
        fixed_frame_time = false;
        rush2::battle::set_online_seed(0);
        rush2::battle::set_points_to_win(saved_battle_points);
        rush2::battle::set_time_limit_minutes(saved_battle_minutes);
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
        MEM_B(record_car, (int32_t)player_records) = (int8_t)local_player;
        MEM_H(0, (int32_t)num_players) = 1;
        MEM_H(0, (int32_t)num_views) = 1;
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
            MEM_B(0, (int32_t)front_end_from_logos) = 1;
            recomp_context saved = *ctx;
            race_end_to_menu_800AAC64(rdram, ctx);
            *ctx = saved;
        }
        rush2::battle::set_online_seed(0);
        if (!menu_snapshot.empty()) {
            rush2::battle::set_points_to_win(saved_battle_points);
            rush2::battle::set_time_limit_minutes(saved_battle_minutes);
            size_t at = 0;
            for (const SavedRange& r : saved_ranges) {
                for (uint32_t i = 0; i < r.size; i++) {
                    MEM_B(0, (int32_t)(r.address + i)) = (int8_t)menu_snapshot[at++];
                }
            }
            menu_snapshot.clear();
        }
        enable_menu_ports(rdram);
        phase = Phase::Idle;
    }

    // The race ended without the game ending it (the host left, a desync, a load mismatch): back to the menus as the
    // pause menu's QUIT goes, and, if the lobby is still there, back to the track select with it (race_finished).
    void race_ended_under_us(uint8_t* rdram, recomp_context* ctx) {
        std::fprintf(stderr, "[online] race ended: %s\n", last_error().c_str());
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
    if (phase == Phase::Running && (now_state == state_post_race || now_state == state_front_end)) {
        // Race over, or QUIT from the pause menu: every machine gets here on the same frame and leaves the lockstep.
        // The post-race (saving to the profiles, the track's record tables, name entry) runs on each machine for its
        // own player only.
        std::fprintf(stderr, "[online] race %s at frame %u\n", now_state == state_front_end ? "quit" : "over",
            race_frame);
        finish_race_locally(rdram, now_state == state_front_end);
        race_finished();
        rush2::online::game::race_over();
        return;
    }
    if (phase == Phase::WaitMenus) {
        // From the car select, the race starts there (rush2::online::game::car_select_done); from anywhere else in
        // the menus (the test aids), here.
        if (MEM_W(0, (int32_t)game_state) == state_car_select) {
            // Everyone has picked a car: the race starts as the car select starts it once all its players have
            // (func_803B9478 at 0x803B96D4-0x803B96F4), with the race set up first.
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
        else if (in_menus(rdram)) {
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
        uint32_t hash = state_hash(rdram);
        std::fprintf(stderr, "[online] race loaded at frame %u, state %08X\n", game_frame, hash);
        if (break_test() == "load") {
            hash ^= 1;
        }
        if (break_test() != "noload") {
            report_loaded(hash);
        }
        phase = Phase::Running;
        lockstep = true;
        race_frame = 0;
    }
    if (!lockstep) {
        return;
    }

    // This frame's inputs: send ours (for `delay` frames ahead) and wait for everyone's. This machine's player plays
    // with the controller on port 1 (the menus' player 1).
    PlayerInput local{};
    static const char* bot = std::getenv("RUSH2_ONLINE_BOT");
    if (bot != nullptr) {
        local = bot_input(race_frame, std::atoi(bot));
    }
    else if (input_source != nullptr) {
        uint16_t buttons = 0;
        float x = 0.0f, y = 0.0f;
        input_source(0, &buttons, &x, &y);
        local.buttons = buttons;
        convert_to_n64_range(x, y, local.stick_x, local.stick_y);
        float gas = 0.0f, brake = 0.0f;
        rush2::input::get_pedals(0, &gas, &brake);
        local.gas = uint8_t(std::lround(std::clamp(gas, 0.0f, 1.0f) * 255.0f));
        local.brake = uint8_t(std::lround(std::clamp(brake, 0.0f, 1.0f) * 255.0f));
        local.battle = rush2::controls::battle_buttons(0);
    }
    submit_local_input(race_frame + input_delay(), local);
    Wait got;
    while ((got = wait_frame_inputs(race_frame, frame_inputs.data(), 250)) == Wait::Timeout) {
        rush2::online::game::race_notice(race_status());
    }
    rush2::online::game::race_notice({});
    if (got == Wait::Ended) {
        race_ended_under_us(rdram, ctx);
        return;
    }

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
    static const char* dump_dir = std::getenv("RUSH2_ONLINE_DUMP");
    if (dump_at != nullptr && dump_dir != nullptr) {
        std::string list = std::string(",") + dump_at + ",";
        if (list.find("," + std::to_string(race_frame) + ",") != std::string::npos) {
            std::vector<uint8_t> image(0x800000);
            for (uint32_t i = 0; i < image.size(); i++) {
                image[i] = (uint8_t)MEM_BU(0, (int32_t)(0x80000000u + i));
            }
            std::string path = std::string(dump_dir) + "/full_" + std::to_string(race_frame) + ".bin";
            if (FILE* f = std::fopen(path.c_str(), "wb")) {
                std::fwrite(image.data(), 1, image.size(), f);
                std::fclose(f);
            }
        }
    }

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
            std::fprintf(stderr, "[online] frame %u hash %08X\n", race_frame, hash);
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
// view 3, which a one-view race leaves unused, for the respawn camera writes. The hooks pass the car state and the
// view just loaded and get back the view to use.
constexpr int scratch_view = 3;

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
    constexpr uint32_t place_scratch = 0x80E14000;  // A view's 0x40-byte pose block (0x800D79D0 + view x 0x40)
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
// +2, the presses). Online view 0 is this machine's player's, whose record isn't record 0 on every machine.
extern "C" uint32_t rush2_online_view_record(uint8_t* rdram, int view, uint32_t record) {
    if (phase == Phase::Idle || view != 0) {
        return record;
    }
    return player_records + local_player * player_record_size;
}

// With two or more players each human car gets an ARROW polygon over it (race_load_car_assets_80086CA4 at
// 0x80086FF8-0x80087048), drawn in the other player's view: its flags are 0x200 | 1 << (1 - car), the views it is
// drawn in, and its update (func_80059B9C at 0x80059BF4) sizes it by the distance from view (car ^ 1)'s camera. Both
// assume two players with a view each. Online the arrows over the other players' cars are drawn in this machine's one
// view and sized from it; this machine's player's car has none.
extern "C" uint32_t rush2_online_arrow_views(uint8_t* rdram, int car, uint32_t flags) {
    if (phase == Phase::Idle) {
        return flags;
    }
    constexpr uint32_t arrow_flags = 0x200;
    return car == local_player ? arrow_flags : arrow_flags | 1;
}

extern "C" int rush2_online_arrow_view(uint8_t* rdram, int view) {
    return phase == Phase::Idle ? view : 0;
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
// it sets up next (func_80054BB0): online each machine has one view (at the race start and at a RESTART).
extern "C" void rush2_online_race_views(uint8_t* rdram, recomp_context* ctx) {
    if (phase != Phase::Idle) {
        MEM_H(0, (int32_t)num_views) = 1;
    }
}

bool rush2::online::game::read_local_controller(uint16_t* buttons, float* x, float* y) {
    return input_source != nullptr && input_source(0, buttons, x, y);
}

bool rush2::online::game::in_race() {
    return phase != Phase::Idle;
}

bool rush2::online::game::car_type(int car, int* type) {
    if (phase == Phase::Idle || car < 0 || car >= (int)race_car_types.size()) {
        return false;
    }
    *type = race_car_types[car];
    return true;
}

int rush2::online::game::local_race_player() {
    return phase == Phase::Idle ? -1 : local_player;
}

// The physics tick writes a human car's rumble level to its record's port (0x800D50E8 + port, physics_car_tick_80075880
// at 0x80075AF8 and 0x80075C08), and src/rumble.cpp reads the port the same way. Online every record has no port, so
// nothing rumbled: this machine's player's level goes to port 0, whose controller drives them (read_local_controller),
// and the other players' stay on none, as each machine rumbles only for its own player.
int rush2::online::game::rumble_port(uint32_t record, int port) {
    if (phase == Phase::Idle) {
        return port;
    }
    return record == player_records + (uint32_t)local_player * player_record_size ? 0 : no_port;
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
// game's player 1's. Online player 1 is whoever has the first lobby slot, so around those calls player 1's record swaps cars
// with this machine's player's and takes controller port 0 (every record has no port online, so no controller reaches the race). The
// rest of the record, which the race also writes, stays player 1's, and no race code runs in between, so every
// machine's race stays the same. The engine sounds (func_80067358, in the physics tick) can't be moved the same way:
// they write the player cars' +0x660, which the physics reads (func_80071D78), so they stay player 1's. The rumble is
// each machine's own player's (rush2::online::game::rumble_port).
// The player count reads 1 there too (hud_players_begin): the HUD's per-player elements show themselves by it (the
// race time hud_widget_cb_icon_row_a_800B9754, the lap time 800B9978, the deaths 800B9C44, the lap banner 800BA868),
// and with the race's count player 2's showed on the one view, a second race time under player 1's gear box. Nothing
// else there reads it but the views' fog (view_fog_setup_8007C624, for one view either way).
namespace {
    bool presenting = false;
    int8_t saved_car = 0;
    int8_t saved_port = 0;
    int hud_depth = 0;
    int16_t saved_players = 0;
}

void rush2::online::game::hud_players_begin(uint8_t* rdram) {
    if (phase == Phase::Idle) {
        return;
    }
    if (hud_depth++ == 0) {
        saved_players = MEM_H(0, (int32_t)num_players);
        MEM_H(0, (int32_t)num_players) = 1;
    }
}

// Three HUD elements read their player's physics car and car state by number, where most go through the player's
// record (which rush2_online_present_begin swaps): the gear box (hud_widget_cb_model_lookup_800B92F4, its player in
// bits 8-11 of the element's parameter), the tachometer's lit part (hud_widget_cb_orbit_icons_800B7E88, shown in
// manual) and its background, which also works out the race time (hud_widget_cb_race_time_display_800B7B1C; both with
// the player in the low bits). Online their player-1 elements are set to this machine's player and their player-2 ones
// to player 15, and each shows itself only while its player is below the count the hooks give them
// (rush2_online_hud_views, in place of the view count): this machine's player + 1. The gear box and the tachometer
// write only their own widget; the race time writes its player's digits (0x80240720 + player * 8), which player 1's
// time widget reads at player 1's (rush2_online_hud_time_digits copies them there). The rewrite is once per element
// list (a RESTART builds another).
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
    for (uint32_t e = head; e != 0; e = (uint32_t)MEM_W(0x3C, (int32_t)e)) {
        uint32_t callback = (uint32_t)MEM_W(0x28, (int32_t)e);
        uint32_t param = (uint32_t)MEM_W(0x2C, (int32_t)e);
        if (callback == gear_box) {
            uint32_t player = (param >> 8) & 0xF;
            param = (param & ~0xF00u) | ((player == 0 ? (uint32_t)local_player : hidden) << 8);
        }
        else if (callback == tach || callback == tach_back_time) {
            uint32_t player = param & 0xF;
            param = (param & ~0xFu) | (player == 0 ? (uint32_t)local_player : hidden);
        }
        else {
            continue;
        }
        MEM_W(0x2C, (int32_t)e) = (int32_t)param;
    }
}

extern "C" int rush2_online_hud_views(uint8_t* rdram, int views) {
    return phase == Phase::Idle ? views : local_player + 1;
}

extern "C" void rush2_online_hud_time_digits(uint8_t* rdram) {
    constexpr uint32_t time_digits = 0x80240720;  // 8 bytes per player (race_format_time_digits_8008ECC4's output)
    if (phase == Phase::Idle || local_player == 0) {
        return;
    }
    for (int32_t i = 0; i < 8; i++) {
        MEM_B(i, (int32_t)time_digits) = MEM_B(i, (int32_t)(time_digits + local_player * 8));
    }
}

void rush2::online::game::hud_players_end(uint8_t* rdram) {
    if (hud_depth == 0) {
        return;
    }
    // Put back only the 1 set here: if the race ended in between (phase Idle) or the game set its own count, that stays.
    if (--hud_depth == 0 && phase != Phase::Idle && MEM_H(0, (int32_t)num_players) == 1) {
        MEM_H(0, (int32_t)num_players) = saved_players;
    }
}

extern "C" void rush2_online_present_begin(uint8_t* rdram, recomp_context* ctx) {
    if (phase == Phase::Idle || presenting) {
        return;
    }
    const int32_t first = (int32_t)player_records;
    const int32_t mine = (int32_t)(player_records + local_player * player_record_size);
    saved_car = MEM_B(record_car, first);
    saved_port = MEM_B(record_port, first);
    MEM_B(record_car, first) = MEM_B(record_car, mine);
    MEM_B(record_car, mine) = saved_car;
    MEM_B(record_port, first) = 0;
    presenting = true;
    rush2::online::game::hud_players_begin(rdram);
}

extern "C" void rush2_online_present_end(uint8_t* rdram, recomp_context* ctx) {
    if (!presenting) {
        return;
    }
    const int32_t first = (int32_t)player_records;
    const int32_t mine = (int32_t)(player_records + local_player * player_record_size);
    MEM_B(record_car, mine) = MEM_B(record_car, first);
    MEM_B(record_car, first) = saved_car;
    MEM_B(record_port, first) = saved_port;
    presenting = false;
    rush2::online::game::hud_players_end(rdram);
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
